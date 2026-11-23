#include "compiler.h"
#include "parser.h"
#include "int128_math.h"
#include "register.h"

#include <stdlib.h>
#include <string.h>

typedef struct Lifetime Lifetime;
struct Lifetime {
    Lifetime *previous;
    size_t stack, pin, depth;
    Node *cleanup;
    bool variable_type;
};
typedef struct { size_t index, target; Lifetime *source; Loc loc; } Goto;
typedef struct { size_t index, resume; Lifetime *lifetime; } Checkpoint;
typedef struct { Node *declaration; size_t checkpoint, label; } NonlocalLabel;
typedef struct { const Node *expression; size_t instruction, arguments[2]; } AllocationSite;
typedef struct { Symbol *symbol; Loc loc; size_t instruction; } VariableArraySite;
typedef struct Substitution Substitution;
struct Substitution { Substitution *previous; Node *node; size_t value; };

typedef struct {
    Symbol *symbol;
    size_t number;
    Lifetime *lifetime;
} Label;

typedef struct {
    size_t label;
    Integer128 value, high;
    bool otherwise;
    Node *node;
} Case;

typedef VECTOR(Case) Cases;
typedef struct {
    Context *context;
    FunctionIR *function;
    VECTOR(Label) labels;
    VECTOR(Goto) gotos, indirect_gotos;
    VECTOR(Checkpoint) checkpoints;
    VECTOR(NonlocalLabel) nonlocal_labels;
    VECTOR(AllocationSite) allocation_sites;
    VECTOR(VariableArraySite) variable_array_sites;
    size_t next_label, break_label, continue_label, switch_id;
    Cases *cases;
    Lifetime *lifetime, *break_lifetime, *continue_lifetime, *switch_lifetime;
    Type *stack_type;
    size_t stack_pin;
    Substitution *substitution;
    bool suppress_unused_result, has_call_diagnostics, has_nonnull_comparisons;
    bool has_access_calls;
} Lower;

static size_t frame_slot(Lower *l, Type *type, unsigned alignment)
{
    PUSH(l->context, l->function->slots, ((FrameSlot){type, alignment}));
    return l->function->slots.len;
}

static size_t slot(Lower *l, Type *type)
{
    return frame_slot(l, type, type->align > 16 ? 16 : type->align);
}

size_t layout_frame_slot(size_t *frame, FrameSlot slot)
{
    size_t size = slot.type->size < 8 ? 8 : slot.type->size;
    unsigned alignment = slot.alignment < 8 ? 8 : slot.alignment;
    size_t offset = align_to(*frame + size, alignment > 16 ? 16 : alignment);
    /* Leave room to round down the address from the 16-byte aligned frame pointer. */
    *frame = offset + (alignment > 16 ? alignment - 16 : 0);
    return offset;
}

static void put(Lower *l, Inst inst)
{
    PUSH(l->context, l->function->instructions, inst);
}

static size_t new_label(Lower *l)
{
    return ++l->next_label;
}

static void mark(Lower *l, size_t label)
{
    put(l, (Inst){.code = I_LABEL, .label = label});
}

static void jump(Lower *l, size_t label)
{
    put(l, (Inst){.code = I_JUMP, .label = label});
}

static size_t nonlocal_checkpoint(Lower *l)
{
    size_t resume = new_label(l);
    Checkpoint checkpoint = {l->function->instructions.len, resume, l->lifetime};
    PUSH(l->context, l->checkpoints, checkpoint);
    jump(l, resume);
    mark(l, resume);
    return l->checkpoints.len - 1;
}

static size_t immediate_integer(Lower *l, Type *type, Integer128 n)
{
    size_t id = slot(l, type);
    put(l, (Inst){.code = I_IMMEDIATE, .dest = id, .immediate = n.lo,
                  .immediate_high = n.hi, .type = type});
    return id;
}

static size_t imm(Lower *l, Type *type, uint64_t n)
{
    return immediate_integer(l, type, (Integer128){n, 0});
}

static size_t wide_value_operation(Lower *, int, size_t, size_t, Type *);
static size_t scalar_cast(Lower *, size_t, Type *, Type *);
static size_t quad_operation(Lower *, int, size_t, size_t, Type *);
static size_t decimal_value_operation(Lower *, int, size_t, size_t, Type *);

static bool x86_quad(Lower *l, Type *type)
{
    return l->context->target == &x86_64_linux && binary128(l->context->target, type);
}

static size_t operation(Lower *l, int op, size_t a, size_t b, Type *type)
{
    Type *operand = l->function->slots.data[a - 1].type;
    if (operand->kind == TY_DECIMAL)
        return decimal_value_operation(l, op, a, b, type);
    if (x86_quad(l, operand))
        return quad_operation(l, op, a, b, type);
    if (narrow_float(operand)) {
        Type *eval = type_float(l->context, 4);
        a = scalar_cast(l, a, operand, eval);
        b = scalar_cast(l, b, l->function->slots.data[b - 1].type, eval);
        size_t result = operation(l, op, a, b, narrow_float(type) ? eval : type);
        return narrow_float(type) ? scalar_cast(l, result, eval, type) : result;
    }
    if (integer128(operand))
        return wide_value_operation(l, op, a, b, type);
    size_t id = slot(l, type);
    put(l, (Inst){.code = I_BINARY, .dest = id, .a = a, .b = b, .op = op,
                  .type = type, .source_type = l->function->slots.data[a - 1].type});
    return id;
}

static size_t truth(Lower *l, size_t id, Loc loc)
{
    Type *from = id ? l->function->slots.data[id - 1].type : NULL;
    if (!scalar(from)) {
        error(l->context, loc, "condition requires a scalar operand");
        return imm(l, type_int(l->context, 0, true), 0);
    }
    if (from->kind != TY_FLOAT && from->kind != TY_DECIMAL && !integer128(from))
        return id;
    Type *to = type_int(l->context, 0, true);
    return scalar_cast(l, id, from, to);
}

static void branch(Lower *l, size_t value, size_t label, bool nonzero, Loc loc)
{
    value = truth(l, value, loc);
    put(l, (Inst){.code = I_ZERO, .a = value, .label = label, .op = nonzero});
}

enum { CONDITION_UNKNOWN, CONDITION_FALSE, CONDITION_TRUE };

static Symbol *nonnull_parameter_symbol(Lower *l, Node *n)
{
    Symbol *function = l->function->function;
    if (!n || n->type->kind != TY_PTR || !function->type->nullability.nonnull.len)
        return NULL;
    while (n->kind == N_CAST && n->left->type->kind == TY_PTR)
        n = n->left;
    if (n->kind != N_SYMBOL || n->symbol->owner != function || n->symbol->pointer_size_untracked ||
        (n->type->qualifiers & (S_VOLATILE | S_ATOMIC)))
        return NULL;
    for (size_t i = 0; i < function->params.len; i++)
        if (function->params.data[i] == n->symbol && nonnull_parameter(function->type, i + 1))
            return n->symbol;
    return NULL;
}

static void mark_nonnull_comparison(Lower *l, Node *n, Loc loc)
{
    Context *c = l->context;
    if (c->syntax_only || !n || !warning_enabled(c, WARNING_NONNULL_COMPARE, loc))
        return;
    Symbol *param = nonnull_parameter_symbol(l, n);
    if (param) {
        l->has_nonnull_comparisons = true;
        put(l, (Inst){.code = I_NONNULL_COMPARE, .symbol = param, .loc = loc});
    }
}

static Node *compared_to_null(Context *c, Node *n)
{
    if (n->kind != N_BINARY || (n->op != EQ && n->op != NE) || n->left->type->kind != TY_PTR)
        return NULL;
    if (known_null_pointer(c, n->right))
        return n->left;
    return known_null_pointer(c, n->left) ? n->right : NULL;
}

static bool fixed_control_value(Context *, Node *);

static bool nonnull_result_truth(Context *c, Node *n, bool *result)
{
    if (c->no_delete_null_pointer_checks || c->syntax_only)
        return false;
    if (call_returns_nonnull(n)) {
        *result = true;
        return true;
    }
    if (n->kind == N_UNARY && n->op == '!' && nonnull_result_truth(c, n->left, result)) {
        *result = !*result;
        return true;
    }
    if (n->kind == N_CAST && (n->type->kind == TY_BOOL ||
                             (n->type->kind == TY_INT && n->left->type->kind == TY_BOOL)))
        return nonnull_result_truth(c, n->left, result);
    Node *pointer = compared_to_null(c, n);
    if (pointer && call_returns_nonnull(pointer) &&
        fixed_control_value(c, pointer == n->left ? n->right : n->left)) {
        *result = n->op == NE;
        return true;
    }
    return false;
}

static bool constant_control_truth(Context *, Node *, bool *);

static bool fixed_control_value(Context *c, Node *n)
{
    if (n->kind == N_INT || n->kind == N_FLOAT)
        return true;
    if (n->kind == N_COND) {
        bool test;
        return constant_control_truth(c, n->left, &test) &&
               fixed_control_value(c, test ? n->right : n->third);
    }
    if (n->kind == N_BINARY && (n->op == AND || n->op == OR)) {
        bool test;
        return constant_control_truth(c, n->left, &test) &&
               (test == (n->op == OR) || fixed_control_value(c, n->right));
    }
    if (n->kind == N_UNARY && n->op == '-')
        return fixed_control_value(c, n->left);
    if (n->kind == N_CAST && n->type->kind == TY_PTR)
        return fixed_control_value(c, n->left);
    /* Runtime floating-point arithmetic can observe the current rounding mode. */
    if (n->type->kind != TY_INT && n->type->kind != TY_BOOL)
        return false;
    switch (n->kind) {
    case N_CAST: case N_UNARY:
        return fixed_control_value(c, n->left);
    case N_BINARY:
        return fixed_control_value(c, n->left) && fixed_control_value(c, n->right);
    default:
        return false;
    }
}

static bool constant_control_truth(Context *c, Node *n, bool *truth_value)
{
    return fixed_control_value(c, n) && constant_truth(c, n, truth_value);
}

static void branch_expression(Lower *l, Node *expr, size_t value, size_t label, bool nonzero, Loc loc)
{
    mark_nonnull_comparison(l, expr, loc);
    bool truth_value;
    if (nonnull_result_truth(l->context, expr, &truth_value)) {
        if (truth_value == nonzero)
            jump(l, label);
        return;
    }
    branch(l, value, label, nonzero, loc);
    if (!l->context->syntax_only) {
        Instructions *instructions = &l->function->instructions;
        Inst *test = &instructions->data[instructions->len - 1];
        test->branch_condition = expr;
        if (constant_control_truth(l->context, expr, &truth_value))
            test->immediate = truth_value ? CONDITION_TRUE : CONDITION_FALSE;
    }
}

typedef enum { RESULT_USED, RESULT_DISCARDED, RESULT_CAST_TO_VOID } ResultUse;
typedef enum { FULL_VALUE, FULL_EFFECT, FULL_CONDITION } FullExpression;

static size_t evaluate(Lower *, Node *, ResultUse);
static size_t value(Lower *l, Node *n)
{
    return evaluate(l, n, RESULT_USED);
}

static void statement(Lower *, Node *);
static size_t block_value(Lower *, Node *, Node *);
static void init_object(Lower *, size_t, Type *, Node *);
static size_t dynamic_compound_literal(Lower *, Node *);

static void record_store_value(Lower *l, size_t address, Node *value)
{
    Instructions *instructions = &l->function->instructions;
    if (!l->context->syntax_only && value->type &&
        (integer_type(value->type) || value->type->kind == TY_PTR) && instructions->len) {
        Inst *in = &instructions->data[instructions->len - 1];
        if (in->code == I_STORE && in->a == address)
            in->stored_value = value;
    }
}

static size_t storage_address(Lower *l, Symbol *s)
{
    Type *t = pointer(l->context, s->type);
    size_t dest = slot(l, t);
    put(l, (Inst){.code = I_ADDRESS, .dest = dest, .symbol = s, .type = t});
    return dest;
}

static size_t address_symbol(Lower *l, Symbol *s)
{
    size_t address = storage_address(l, s);
    if (!s->type->variable_size)
        return address;
    Type *t = pointer(l->context, s->type);
    size_t dest = slot(l, t);
    put(l, (Inst){.code = I_LOAD, .dest = dest, .a = address, .type = t});
    return dest;
}

void check_frame_size(Context *c, const Symbol *function, size_t frame)
{
    uint64_t limit = c->frame_warning_limit_set ? c->frame_warning_limit : PTRDIFF_MAX;
    if (!c->syntax_only && frame > limit)
        warning_at(c, WARNING_FRAME_LARGE, function->function_end_loc,
                   "the frame size of %llu bytes is larger than %llu bytes",
                   (unsigned long long)frame, (unsigned long long)limit);
}

void check_stack_usage(Context *c, const FunctionIR *ir, size_t frame, size_t outgoing, bool outgoing_dynamic)
{
    const Symbol *function = ir->function;
    Loc loc = function->function_end_loc;
    uint64_t limit = c->stack_warning_limit_set ? c->stack_warning_limit : PTRDIFF_MAX;
    if (c->syntax_only || (!c->stack_usage_report &&
        (limit == UINT64_MAX || !warning_enabled(c, WARNING_STACK_USAGE, loc))))
        return;
    uint64_t total = frame;
    bool overflow = total > UINT64_MAX - 16;
    if (!overflow)
        total += 16; /* Saved frame pointer and return address. */
    if (!overflow && outgoing <= UINT64_MAX - total)
        total += outgoing;
    else
        overflow = true;
    if (!overflow && ir->dynamic_stack_bound <= UINT64_MAX - total)
        total += ir->dynamic_stack_bound;
    else
        overflow = true;
    bool unbounded = ir->dynamic_stack_unbounded || overflow;
    bool dynamic = ir->has_dynamic_stack || outgoing_dynamic;
    if (c->stack_usage_report) {
        Loc definition = function->function_definition_loc;
        fprintf(c->stack_usage_report, "%s:%u:%u:%s\t%llu\t%s%s\n",
                definition.file ? definition.file : c->input, definition.line, definition.column, function->name,
                (unsigned long long)total, dynamic ? "dynamic" : "static", dynamic && !unbounded ? ",bounded" : "");
    }
    if (limit == UINT64_MAX || !warning_enabled(c, WARNING_STACK_USAGE, loc))
        return;
    if (unbounded)
        warning_at(c, WARNING_STACK_USAGE, loc, "stack usage might be unbounded");
    else if (total > limit)
        warning_at(c, WARNING_STACK_USAGE, loc, ir->has_dynamic_stack ?
                   "stack usage might be %llu bytes" : "stack usage is %llu bytes",
                   (unsigned long long)total);
}

static size_t frame_slot_address(Lower *l, size_t object, Type *type)
{
    size_t address = slot(l, pointer(l->context, type));
    put(l, (Inst){.code = I_ADDRESS, .dest = address, .a = object});
    return address;
}

static size_t object_slot(Lower *l, Type *t)
{
    return frame_slot_address(l, frame_slot(l, t, t->align), t);
}

static size_t value_address(Lower *l, size_t value, Type *type)
{
    if (address_value(type))
        return value;
    return frame_slot_address(l, value, type);
}

static size_t offset(Lower *l, size_t id, size_t amount)
{
    if (!amount)
        return id;
    return operation(l, '+', id, imm(l, type_int(l->context, 4, true), amount),
                     l->function->slots.data[id - 1].type);
}

static size_t vector_element_address(Lower *l, size_t source, size_t index, Type *element)
{
    Type *index_type = l->function->slots.data[index - 1].type;
    size_t bytes = operation(l, '*', index, imm(l, index_type, element->size), index_type);
    return operation(l, '+', source, bytes, pointer(l->context, element));
}

static size_t address(Lower *l, Node *n)
{
    switch (n->kind) {
    case N_SYMBOL:
        if (!global_register(n->symbol))
            return address_symbol(l, n->symbol);
        error(l->context, n->loc, "cannot take address of explicit register variable");
        return imm(l, pointer(l->context, n->type), 0);
    case N_COMPOUND: {
        if (n->type->variable_size)
            return dynamic_compound_literal(l, n);
        size_t at = address_symbol(l, n->symbol);
        if (n->symbol->local) {
            if (aggregate(n->type) || n->type->kind == TY_VECTOR)
                put(l, (Inst){.code = I_MEMZERO, .a = at, .immediate = n->type->size});
            init_object(l, at, n->type, n->symbol->init);
        }
        return at;
    }
    case N_DEREF: return value(l, n->left);
    case N_MEMBER: {
        size_t base = offset(l, value(l, n->left), n->integer);
        if (!n->right)
            return base;
        size_t displacement = value(l, n->right);
        return operation(l, '+', base, displacement, pointer(l->context, n->type));
    }
    case N_VECTOR_ELEMENT: {
        Context *c = l->context;
        Type *index_type = type_int(c, 4, false);
        size_t base = value(l, n->left);
        size_t index = value(l, convert(c, n->right, index_type, n->loc));
        return vector_element_address(l, base, index, n->type);
    }
    default:
        error(l->context, n->loc, "cannot take the address of this expression");
        return imm(l, pointer(l->context, n->type), 0);
    }
}

static size_t plain_load(Lower *l, size_t at, Type *type)
{
    if (address_value(type) || type->kind == TY_FUNC)
        return at;
    if (type->kind == TY_VOID)
        return 0;
    size_t dest = slot(l, type);
    put(l, (Inst){.code = I_LOAD, .dest = dest, .a = at, .type = type});
    return dest;
}

static size_t register_slot(Lower *l, Type *t)
{
    if (aggregate(t))
        t = type_int(l->context, t->size == 16 ? 6 : 4, true);
    return slot(l, t);
}

static size_t copy_register_operand(Lower *l, size_t source, Type *t)
{
    if (!aggregate(t) && t->kind != TY_VECTOR)
        return source;
    size_t raw = register_slot(l, t);
    size_t dest = frame_slot_address(l, raw, t);
    put(l, (Inst){.code = I_MEMCOPY, .a = dest, .b = source, .immediate = t->size});
    return raw;
}

static size_t read_register_bits(Lower *l, Symbol *s)
{
    size_t dest = register_slot(l, s->type);
    put(l, (Inst){.code = I_REGISTER_READ, .dest = dest, .symbol = s, .type = s->type});
    return dest;
}

static size_t read_register(Lower *l, Symbol *s)
{
    size_t bits = read_register_bits(l, s);
    return address_value(s->type) ? frame_slot_address(l, bits, s->type) : bits;
}

static size_t runtime_call(Lower *l, const char *name, Type *result,
                           const size_t *args, size_t count)
{
    Symbol *target = alloc(l->context, sizeof *target);
    target->assembly = name;
    Inst call = {.code = I_CALL, .type = result, .symbol = target};
    for (size_t i = 0; i < count; i++) {
        PUSH(l->context, call.args, args[i]);
        PUSH(l->context, call.arg_types, l->function->slots.data[args[i] - 1].type);
    }
    if (result->kind != TY_VOID)
        call.dest = slot(l, result);
    put(l, call);
    return call.dest;
}

static Type *runtime_void(Context *c)
{
    Type *nothing = alloc(c, sizeof *nothing);
    *nothing = (Type){.kind = TY_VOID, .align = 1, .complete = true};
    return nothing;
}

static size_t decimal_value_operation(Lower *l, int op, size_t a, size_t b, Type *type)
{
    Context *c = l->context;
    Type *source = l->function->slots.data[a - 1].type;
    DecimalOperation operation;
    if (!decimal_operation(op, &operation)) {
        error(c, (Loc){0}, "unsupported decimal operation");
        return a;
    }
    Type *word = type_int(c, 3, true);
    size_t left = value_address(l, a, source);
    size_t right = value_address(l, b, source);
    size_t width = imm(l, word, source->size), code = imm(l, word, operation);
    if (operation >= DEC_LT) {
        size_t args[] = {left, right, width, code};
        size_t result = runtime_call(l, "__mcc_decimal_relation", type_int(c, 3, false), args, 4);
        return type->kind == TY_BOOL ? scalar_cast(l, result, type_int(c, 3, false), type) : result;
    }
    size_t result = object_slot(l, type);
    size_t args[] = {result, left, right, width, code};
    runtime_call(l, "__mcc_decimal_binary", runtime_void(c), args, 5);
    return plain_load(l, result, type);
}

static size_t floating_value(Lower *l, Node *n)
{
    Context *c = l->context;
    Type *word = type_int(c, 3, true);
    Type *fp = n->operand_type;
    bool decimal_comparison = fp->kind == TY_DECIMAL && n->op >= FP_ISUNORDERED;
    size_t format = imm(l, word, decimal_comparison ? fp->size : builtin_float_format(c->target, fp));
    if (n->op == FP_NAN || n->op == FP_SNAN) {
        size_t output = object_slot(l, fp), payload = value(l, n->left);
        size_t args[] = {output, payload, format, imm(l, word, (unsigned)n->op)};
        runtime_call(l, "__mcc_fp_nan", runtime_void(c), args, 4);
        return plain_load(l, output, fp);
    }
    size_t left = value_address(l, value(l, n->left), fp);
    if (n->op == FP_CLASSIFY) {
        size_t args[7] = {left, format};
        for (size_t i = 0; i < 5; i++)
            args[i + 2] = value(l, n->list.data[i]);
        return runtime_call(l, "__mcc_fp_classify", n->type, args, 7);
    }
    size_t right = n->right ? value_address(l, value(l, n->right), fp) : imm(l, pointer(c, fp), 0);
    if (n->op == FP_ABS || n->op == FP_COPYSIGN) {
        size_t output = object_slot(l, fp);
        size_t args[] = {output, left, right, format};
        runtime_call(l, "__mcc_fp_sign", runtime_void(c), args, 4);
        return plain_load(l, output, fp);
    }
    size_t args[] = {left, right, format, imm(l, word, (unsigned)n->op)};
    const char *name = decimal_comparison ? "__mcc_decimal_predicate" : "__mcc_fp_predicate";
    return runtime_call(l, name, n->type, args, 4);
}

static size_t quad_operation(Lower *l, int op, size_t a, size_t b, Type *type)
{
    const char *helper;
    switch (op) {
    case '+': helper = "__addtf3"; break;
    case '-': helper = "__subtf3"; break;
    case '*': helper = "__multf3"; break;
    case '/': helper = "__divtf3"; break;
    case '<': case LE: helper = "__letf2"; break;
    case '>': case GE: helper = "__getf2"; break;
    default: helper = "__eqtf2"; break;
    }
    size_t args[] = {a, b};
    if (type->kind == TY_FLOAT)
        return runtime_call(l, helper, type, args, 2);
    Type *integer = type_int(l->context, 3, false);
    size_t cmp = runtime_call(l, helper, integer, args, 2);
    return operation(l, op, cmp, imm(l, integer, 0), type);
}

static size_t wide_value_operation(Lower *l, int op, size_t a, size_t b, Type *type)
{
    Context *c = l->context;
    Type *operand = l->function->slots.data[a - 1].type;
    WideOperation operation;
    if (!wide_operation(op, &operation)) {
        error(c, (Loc){0}, "unsupported 128-bit arithmetic operation");
        return imm(l, type, 0);
    }
    size_t result = slot(l, operand);
    Type *small = type_int(c, 3, true), *boolean = type_int(c, 0, true);
    Type *nothing = alloc(c, sizeof *nothing);
    *nothing = (Type){.kind = TY_VOID, .align = 1, .complete = true};
    size_t args[] = {value_address(l, result, operand), value_address(l, a, operand),
                     value_address(l, b, operand), imm(l, small, operation),
                     imm(l, boolean, operand->uns)};
    runtime_call(l, "__mcc_int128_binary", nothing, args, sizeof args / sizeof *args);
    if (integer128(type))
        return result;
    size_t dest = slot(l, type);
    put(l, (Inst){.code = I_CAST, .dest = dest, .a = result, .type = type,
                  .source_type = operand});
    return dest;
}

static size_t convert_wide_float(Lower *l, size_t source, Type *from, Type *to)
{
    Context *c = l->context;
    bool to_integer = integer128(to);
    Type *floating = to_integer ? from : to;
    bool uns = (to_integer ? to : from)->uns;
    size_t result = slot(l, to);
    Type *nothing = alloc(c, sizeof *nothing);
    *nothing = (Type){.kind = TY_VOID, .align = 1, .complete = true};
    size_t args[] = {value_address(l, result, to), value_address(l, source, from),
                     imm(l, type_int(c, 3, true), floating_format(c->target, floating)),
                     imm(l, type_int(c, 0, true), uns)};
    const char *helper = to_integer ? "__mcc_int128_from_float" :
                         narrow_float(to) ? "__mcc_int128_to_float16" : "__mcc_int128_to_float";
    runtime_call(l, helper, nothing, args, sizeof args / sizeof *args);
    return result;
}

static size_t atomic_size(Lower *l, Type *type)
{
    return imm(l, type_int(l->context, 4, true), type->size);
}

static void atomic_access(Lower *l, Type *type, size_t at, size_t input, size_t output,
                          unsigned operation)
{
    Type *void_type = alloc(l->context, sizeof *void_type);
    *void_type = (Type){.kind = TY_VOID, .align = 1, .complete = true};
    Type *ptr = pointer(l->context, void_type);
    Type *integer = type_int(l->context, 3, true);
    size_t args[] = {atomic_size(l, type), at, input ? input : imm(l, ptr, 0),
                     output ? output : imm(l, ptr, 0), imm(l, integer, operation)};
    runtime_call(l, "__mcc_atomic_access", void_type, args, sizeof args / sizeof *args);
}

static size_t atomic_compare(Lower *l, Type *type, size_t at, size_t expected, size_t desired)
{
    size_t args[] = {atomic_size(l, type), at, expected, desired};
    return runtime_call(l, "__mcc_atomic_compare", type_int(l->context, 0, true),
                        args, sizeof args / sizeof *args);
}

static size_t atomic_compare_hle(Lower *l, Type *type, size_t at, size_t expected, size_t desired,
                                 unsigned hint)
{
    if (!hint)
        return atomic_compare(l, type, at, expected, desired);
    size_t args[] = {atomic_size(l, type), at, expected, desired,
                     imm(l, type_int(l->context, 3, true), hint)};
    return runtime_call(l, "__mcc_atomic_compare_hle", type_int(l->context, 0, true),
                        args, sizeof args / sizeof *args);
}

static size_t load(Lower *l, size_t at, Type *type)
{
    if (type->qualifiers & S_ATOMIC) {
        type = unqualified_type(l->context, type);
        size_t object = object_slot(l, type);
        atomic_access(l, type, at, 0, object, AT_LOAD);
        at = object;
    }
    return plain_load(l, at, type);
}

static size_t dynamic_size(Lower *l, Type *type)
{
    Symbol *size = type->runtime_size;
    return plain_load(l, address_symbol(l, size), size->type);
}

static size_t allocate_variable_temporary(Lower *l, Type *type, unsigned alignment)
{
    size_t size = dynamic_size(l, type);
    size_t result = slot(l, pointer(l->context, type));
    put(l, (Inst){.code = I_STACK_ALLOC, .dest = result, .a = size, .immediate = alignment, .type = type});
    return result;
}

static void zero_variable_object(Lower *l, size_t address, Type *type)
{
    size_t zero = imm(l, type_int(l->context, 3, false), 0);
    size_t args[] = {address, zero, dynamic_size(l, type)};
    runtime_call(l, "memset", pointer(l->context, type), args, 3);
}

static void store(Lower *l, size_t at, size_t v, Type *type)
{
    if (type->qualifiers & S_ATOMIC) {
        Type *value_type = unqualified_type(l->context, type);
        atomic_access(l, value_type, at, value_address(l, v, value_type), 0, AT_STORE);
    } else if (type->variable_size) {
        size_t args[] = {at, v, dynamic_size(l, type)};
        runtime_call(l, "memmove", pointer(l->context, type), args, 3);
    } else {
        put(l, (Inst){.code = I_STORE, .a = at, .b = v, .type = type});
    }
}

static size_t quad_cast(Lower *l, size_t in, Type *from, Type *to)
{
    if (x86_quad(l, from)) {
        if (x86_quad(l, to))
            return in;
        if (to->kind == TY_FLOAT) {
            const char *helper = to->size == 4 ? "__trunctfsf2" :
                                 to->size == 8 ? "__trunctfdf2" : "__trunctfxf2";
            return runtime_call(l, helper, to, &in, 1);
        }
        if (to->kind == TY_BOOL) {
            size_t zero = slot(l, from);
            put(l, (Inst){.code = I_REAL, .dest = zero, .type = from});
            return operation(l, NE, in, zero, to);
        }
        Type *wide = type_int(l->context, 4, to->uns);
        size_t integer = runtime_call(l, to->uns ? "__fixunstfdi" : "__fixtfdi", wide, &in, 1);
        return same_type(wide, to) ? integer : scalar_cast(l, integer, wide, to);
    }
    if (from->kind == TY_FLOAT) {
        const char *helper = from->size == 4 ? "__extendsftf2" :
                             from->size == 8 ? "__extenddftf2" : "__extendxftf2";
        return runtime_call(l, helper, to, &in, 1);
    }
    Type *wide = type_int(l->context, 4, from->uns);
    if (!same_type(from, wide))
        in = scalar_cast(l, in, from, wide);
    return runtime_call(l, from->uns ? "__floatunditf" : "__floatditf", to, &in, 1);
}

static size_t decimal_cast(Lower *l, size_t in, Type *from, Type *to)
{
    Context *c = l->context;
    bool from_decimal = from->kind == TY_DECIMAL, to_decimal = to->kind == TY_DECIMAL;
    if (from_decimal && to_decimal && from->size == to->size)
        return in;
    if (to->kind == TY_BOOL)
        return operation(l, NE, in, imm(l, from, 0), to);
    Type *word = type_int(c, 3, true);
    size_t result = object_slot(l, to);
    size_t input = value_address(l, in, from);
    if (from_decimal && to_decimal) {
        size_t args[] = {result, input, imm(l, word, to->size), imm(l, word, from->size)};
        runtime_call(l, "__mcc_decimal_convert", runtime_void(c), args, 4);
    } else if (from->kind == TY_FLOAT || to->kind == TY_FLOAT) {
        Type *binary = from_decimal ? to : from;
        unsigned width = (from_decimal ? from : to)->size;
        unsigned format = floating_format(c->target, binary);
        size_t args[] = {result, input, imm(l, word, width), imm(l, word, format)};
        runtime_call(l, from_decimal ? "__mcc_decimal_to_binary" : "__mcc_decimal_from_binary",
                     runtime_void(c), args, 4);
    } else {
        Type *decimal = from_decimal ? from : to;
        Type *integer = from_decimal ? to : from;
        size_t args[] = {result, input, imm(l, word, decimal->size), imm(l, word, integer->size),
                         imm(l, type_int(c, 0, true), integer->uns)};
        runtime_call(l, from_decimal ? "__mcc_decimal_to_integer" : "__mcc_decimal_from_integer",
                     runtime_void(c), args, 5);
    }
    return plain_load(l, result, to);
}

static size_t scalar_cast(Lower *l, size_t in, Type *from, Type *to)
{
    if (from->kind == TY_DECIMAL || to->kind == TY_DECIMAL)
        return decimal_cast(l, in, from, to);
    if ((integer128(to) && from->kind == TY_FLOAT) ||
        (to->kind == TY_FLOAT && integer128(from)))
        return convert_wide_float(l, in, from, to);
    if (narrow_float(from)) {
        if (narrow_float(to) && from->bfloat == to->bfloat)
            return in;
        Type *raw = type_int(l->context, 2, true), *eval = type_float(l->context, 4);
        size_t bits = plain_load(l, value_address(l, in, from), raw);
        size_t format = imm(l, type_int(l->context, 3, true), floating_format(l->context->target, from));
        size_t args[] = {bits, format};
        size_t result = runtime_call(l, "__mcc_float16_to_float", eval, args, 2);
        return same_type(eval, to) ? result : scalar_cast(l, result, eval, to);
    }
    if (narrow_float(to)) {
        Type *raw = type_int(l->context, 2, true);
        const Target *target = l->context->target;
        bool quad = binary128(target, from);
        Type *extended = quad ? from : type_float(l->context, target->long_double_size);
        size_t source = same_type(from, extended) ? in : scalar_cast(l, in, from, extended);
        size_t format = imm(l, type_int(l->context, 3, true), floating_format(target, to));
        size_t args[] = {source, format};
        size_t bits = runtime_call(l, quad ? "__mcc_float16_from_quad" : "__mcc_float16_from_long_double",
                                   raw, args, 2);
        return plain_load(l, value_address(l, bits, raw), to);
    }
    if (from->kind == TY_FLOAT && to->kind == TY_FLOAT &&
        floating_format(l->context->target, from) == floating_format(l->context->target, to))
        return in;
    if (x86_quad(l, from) || x86_quad(l, to))
        return quad_cast(l, in, from, to);
    size_t dest = slot(l, to);
    put(l, (Inst){.code = I_CAST, .dest = dest, .a = in, .type = to, .source_type = from});
    return dest;
}

static size_t scalar_zero(Lower *l, Type *t)
{
    if (t->kind != TY_FLOAT)
        return imm(l, t, 0);
    size_t dest = slot(l, t);
    put(l, (Inst){.code = I_REAL, .dest = dest, .type = t});
    return dest;
}

typedef struct { size_t real, imaginary; } ComplexParts;

static ComplexParts split_complex(Lower *l, size_t at, Type *t)
{
    Type *base = t->base;
    ComplexParts value = {.real = load(l, at, base)};
    value.imaginary = load(l, offset(l, at, base->size), base);
    return value;
}

static size_t make_complex(Lower *l, Type *t, ComplexParts parts)
{
    size_t dest = object_slot(l, t);
    store(l, dest, parts.real, t->base);
    store(l, offset(l, dest, t->base->size), parts.imaginary, t->base);
    return dest;
}

static size_t complex_truth(Lower *l, size_t in, Type *t)
{
    ComplexParts parts = split_complex(l, in, t);
    Type *boolean = type_int(l->context, 0, true);
    size_t first = truth(l, parts.real, (Loc){0});
    size_t second = truth(l, parts.imaginary, (Loc){0});
    if (t->base->kind != TY_FLOAT && !integer128(t->base)) {
        size_t zero = scalar_zero(l, t->base);
        first = operation(l, NE, first, zero, boolean);
        second = operation(l, NE, second, zero, boolean);
    }
    return operation(l, '|', first, second, boolean);
}

static size_t condition_value(Lower *l, Node *n)
{
    n = excess_value(n);
    size_t in = value(l, n);
    return n->type->kind == TY_COMPLEX ? complex_truth(l, in, n->type) : in;
}

static size_t negate_scalar(Lower *l, size_t id, Type *base)
{
    if (base->kind == TY_DECIMAL) {
        Type *raw = type_int(l->context, base->size == 16 ? 6 : base->size == 8 ? 4 : 3, true);
        size_t bits = plain_load(l, value_address(l, id, base), raw);
        Integer128 sign = wide_shift_left((Integer128){1, 0}, base->size * 8 - 1);
        size_t flipped = operation(l, '^', bits, immediate_integer(l, raw, sign), raw);
        return plain_load(l, value_address(l, flipped, raw), base);
    }
    if (x86_quad(l, base))
        return runtime_call(l, "__negtf2", base, &id, 1);
    if (narrow_float(base)) {
        Type *raw = type_int(l->context, 2, true);
        size_t bits = plain_load(l, value_address(l, id, base), raw);
        size_t sign = operation(l, '^', bits, imm(l, raw, 0x8000), raw);
        return plain_load(l, value_address(l, sign, raw), base);
    }
    if (integer128(base))
        return operation(l, '-', scalar_zero(l, base), id, base);
    size_t dest = slot(l, base);
    put(l, (Inst){.code = I_UNARY, .dest = dest, .a = id, .op = '-',
                  .type = base, .source_type = base});
    return dest;
}

static ComplexParts complex_operand(Lower *l, Node *n)
{
    size_t in = value(l, n);
    return n->type->kind == TY_COMPLEX ? split_complex(l, in, n->type) : (ComplexParts){in, 0};
}

static size_t integer_magnitude(Lower *l, size_t value, Type *base, Type *uns)
{
    if (base->uns)
        return value;
    size_t count = imm(l, base, base->size * 8 - 1);
    size_t signed_mask = operation(l, SHR, value, count, base);
    size_t mask = scalar_cast(l, signed_mask, base, uns);
    size_t bits = scalar_cast(l, value, base, uns);
    return operation(l, '-', operation(l, '^', bits, mask, uns), mask, uns);
}

static void integer_complex_divide(Lower *l, size_t dest, Type *base,
                                   ComplexParts a, ComplexParts b, bool swapped)
{
    size_t ratio = operation(l, '/', b.imaginary, b.real, base);
    size_t tail = operation(l, '*', b.imaginary, ratio, base);
    size_t denominator = operation(l, '+', b.real, tail, base);
    size_t cross_real = operation(l, '*', a.imaginary, ratio, base);
    size_t numerator_real = operation(l, '+', a.real, cross_real, base);
    size_t cross_imag = operation(l, '*', a.real, ratio, base);
    size_t numerator_imag = swapped ? operation(l, '-', cross_imag, a.imaginary, base) :
                                      operation(l, '-', a.imaginary, cross_imag, base);
    store(l, dest, operation(l, '/', numerator_real, denominator, base), base);
    store(l, offset(l, dest, base->size), operation(l, '/', numerator_imag, denominator, base), base);
}

static size_t integer_complex_quotient(Lower *l, Node *n, ComplexParts a, ComplexParts b)
{
    Type *t = n->operand_type, *base = t->base;
    Type *uns = type_int(l->context, base->rank, true);
    Type *boolean = type_int(l->context, 0, true);
    if (!a.imaginary)
        a.imaginary = scalar_zero(l, base);
    size_t real = integer_magnitude(l, b.real, base, uns);
    size_t imag = integer_magnitude(l, b.imaginary, base, uns);
    size_t dest = object_slot(l, t), swap = new_label(l), done = new_label(l);
    branch(l, operation(l, '<', real, imag, boolean), swap, true, n->loc);
    integer_complex_divide(l, dest, base, a, b, false);
    jump(l, done);
    mark(l, swap);
    integer_complex_divide(l, dest, base, (ComplexParts){a.imaginary, a.real},
                           (ComplexParts){b.imaginary, b.real}, true);
    mark(l, done);
    return dest;
}

static size_t complex_binary(Lower *l, Node *n)
{
    Type *t = n->operand_type, *base = t->base;
    bool right_complex = n->right->type->kind == TY_COMPLEX;
    if (base->kind == TY_FLOAT && right_complex &&
        (n->op == '/' || (n->op == '*' && n->left->type->kind == TY_COMPLEX))) {
        size_t a = value(l, n->left);
        if (n->left->type->kind != TY_COMPLEX)
            a = make_complex(l, t, (ComplexParts){a, scalar_zero(l, base)});
        size_t b = value(l, n->right);
        size_t dest = object_slot(l, t);
        Type *void_type = alloc(l->context, sizeof *void_type);
        *void_type = (Type){.kind = TY_VOID, .align = 1, .complete = true};
        size_t divide = imm(l, type_int(l->context, 0, true), n->op == '/');
        if (binary128(l->context->target, base)) {
            size_t args[] = {dest, a, b, divide};
            runtime_call(l, "__mcc_quad_complex_binary", void_type, args, 4);
        } else {
            size_t args[] = {dest, a, b, imm(l, type_int(l->context, 3, true), base->size), divide};
            runtime_call(l, "__mcc_complex_binary", void_type, args, 5);
        }
        return dest;
    }
    ComplexParts a = complex_operand(l, n->left);
    ComplexParts b = complex_operand(l, n->right);
    if (n->op == '/' && right_complex)
        return integer_complex_quotient(l, n, a, b);
    if (n->op == EQ || n->op == NE) {
        size_t real = operation(l, n->op, a.real, b.real, n->type);
        size_t ai = a.imaginary ? a.imaginary : scalar_zero(l, base);
        size_t bi = b.imaginary ? b.imaginary : scalar_zero(l, base);
        size_t imag = operation(l, n->op, ai, bi, n->type);
        return operation(l, n->op == EQ ? '&' : '|', real, imag, n->type);
    }
    ComplexParts result = {0};
    if (n->op == '*' && a.imaginary && b.imaginary) {
        size_t ac = operation(l, '*', a.real, b.real, base);
        size_t bd = operation(l, '*', a.imaginary, b.imaginary, base);
        size_t ad = operation(l, '*', a.real, b.imaginary, base);
        size_t bc = operation(l, '*', a.imaginary, b.real, base);
        result.real = operation(l, '-', ac, bd, base);
        result.imaginary = operation(l, '+', ad, bc, base);
    } else {
        result.real = operation(l, n->op, a.real, b.real, base);
        if (n->op == '*' || n->op == '/') {
            result.imaginary = a.imaginary ? operation(l, n->op, a.imaginary, b.real, base) :
                                            operation(l, '*', a.real, b.imaginary, base);
        } else if (a.imaginary && b.imaginary) {
            result.imaginary = operation(l, n->op, a.imaginary, b.imaginary, base);
        } else if (a.imaginary) {
            result.imaginary = a.imaginary;
        } else {
            result.imaginary = n->op == '-' ? negate_scalar(l, b.imaginary, base) : b.imaginary;
        }
    }
    return make_complex(l, t, result);
}

static size_t checked_arithmetic(Lower *l, Node *n)
{
    size_t a = value(l, n->left);
    size_t b = value(l, n->right);
    size_t third = value(l, n->third);
    Type *small = type_int(l->context, 3, true);
    bool wide = integer128(n->left->type);
    size_t args[] = {wide ? value_address(l, a, n->left->type) : a,
                     wide ? value_address(l, b, n->right->type) : b,
                     imm(l, small, (unsigned)n->op), imm(l, small, n->integer),
                     imm(l, small, n->flags)};
    size_t overflow = runtime_call(l, wide ? "__mcc_integer128_overflow" : "__mcc_integer_overflow", n->type,
                                   args, sizeof args / sizeof *args);
    if (n->operand_type)
        store(l, third, operation(l, n->op, a, b, n->left->type), n->operand_type);
    return overflow;
}

static size_t arithmetic_with_carry(Lower *l, Node *n)
{
    const int op = n->op;
    Type *t = n->type;
    size_t a = value(l, n->list.data[0]);
    size_t b = value(l, n->list.data[1]);
    size_t carry = value(l, n->list.data[2]);
    size_t out = value(l, n->list.data[3]);
    size_t partial = operation(l, op, a, b, t);
    size_t result = operation(l, op, partial, carry, t);
    size_t first = op == '+' ? operation(l, '<', partial, a, t) : operation(l, '<', a, b, t);
    size_t second = op == '+' ? operation(l, '<', result, partial, t) :
                                operation(l, '<', partial, carry, t);
    store(l, out, operation(l, '|', first, second, t), t);
    return result;
}

static size_t atomic_update(Lower *l, Node *n, size_t at)
{
    Type *type = n->type;
    size_t operand = value(l, n->third);
    size_t expected = object_slot(l, type);
    atomic_access(l, type, at, 0, expected, AT_LOAD);
    size_t retry = new_label(l);
    mark(l, retry);
    size_t before = plain_load(l, expected, type);
    Substitution rhs = {l->substitution, n->third, operand};
    Substitution lhs = {&rhs, n->left, before};
    l->substitution = &lhs;
    size_t desired = value(l, n->right);
    l->substitution = rhs.previous;
    size_t success = atomic_compare(l, type, at, expected, value_address(l, desired, type));
    branch(l, success, retry, false, n->loc);
    return n->postfix ? before : desired;
}

static size_t atomic_wide_rmw(Lower *l, Node *n, size_t at, size_t operand, unsigned op)
{
    static const int operators[] = {
        [AT_ADD] = '+', [AT_SUB] = '-', [AT_AND] = '&', [AT_OR] = '|', [AT_XOR] = '^', [AT_NAND] = '&'
    };
    Type *type = n->operand_type;
    size_t expected = object_slot(l, type);
    atomic_access(l, type, at, 0, expected, AT_LOAD);
    size_t retry = new_label(l);
    mark(l, retry);
    size_t before = plain_load(l, expected, type);
    size_t after = operation(l, operators[op], before, operand, type);
    if (op == AT_NAND) {
        size_t mask = immediate_integer(l, type, wide_mask(128));
        after = operation(l, '^', after, mask, type);
    }
    size_t success = atomic_compare(l, type, at, expected, value_address(l, after, type));
    branch(l, success, retry, false, n->loc);
    return n->op & AT_RETURN_NEW ? after : before;
}

static size_t atomic_value(Lower *l, Node *n)
{
    Context *c = l->context;
    unsigned op = (unsigned)n->op & AT_OPERATION_MASK;
    bool pointers = (n->op & AT_POINTER_ARGUMENTS) != 0;
    size_t args[6];
    for (size_t i = 0; i < n->list.len; i++)
        args[i] = value(l, n->list.data[i]);
    if (op == AT_THREAD_FENCE || op == AT_SIGNAL_FENCE) {
        uint64_t order = 0;
        if (op == AT_THREAD_FENCE &&
            (!n->list.len || !constant_int(c, n->list.data[0], &order) || order))
            runtime_call(l, "__mcc_atomic_fence", n->type, NULL, 0);
        return 0;
    }
    Type *type = n->operand_type;
    size_t at = args[0];
    if (op == AT_LOCK_FREE) {
        if (!type)
            return runtime_call(l, "__mcc_atomic_lock_free", n->type, args, 2);
        size_t query[] = {atomic_size(l, type), at};
        return runtime_call(l, "__mcc_atomic_lock_free", n->type, query, 2);
    }
    unsigned hle = 0;
    if (c->target == &x86_64_linux && (n->op & AT_HAS_ORDER)) {
        size_t index = op == AT_COMPARE ? 4 : n->list.len - 1;
        uint64_t order;
        if (constant_int(c, n->list.data[index], &order)) {
            uint64_t model = order & UINT64_C(0xffff);
            uint64_t extension = order & ~UINT64_C(0xffff);
            if ((extension == AT_HLE_ACQUIRE && (model == 2 || model == 4 || model == 5)) ||
                (extension == AT_HLE_RELEASE && (model == 3 || model == 4 || model == 5)))
                hle = (unsigned)extension;
        }
    }
    if (op == AT_COMPARE) {
        size_t desired = pointers ? args[2] : value_address(l, args[2], type);
        size_t expected = n->op & AT_EXPECTED_VALUE ? value_address(l, args[1], type) : args[1];
        size_t compared = atomic_compare_hle(l, type, at, expected, desired, hle);
        return n->op & AT_RETURN_OBSERVED ? plain_load(l, expected, type) : compared;
    }
    if (op >= AT_ADD && op <= AT_TEST_AND_SET) {
        if (integer128(type))
            return atomic_wide_rmw(l, n, at, args[1], op);
        Type *wide = type_int(c, 4, true), *small = type_int(c, 3, true);
        size_t operand = op == AT_TEST_AND_SET ? imm(l, wide, 1) : args[1];
        size_t rmw[] = {at, operand, imm(l, small, type->size), imm(l, small, op | hle),
                        imm(l, type_int(c, 0, true), (n->op & AT_RETURN_NEW) != 0)};
        size_t result = runtime_call(l, "__mcc_atomic_rmw", wide, rmw, sizeof rmw / sizeof *rmw);
        if (op == AT_TEST_AND_SET)
            return operation(l, NE, result, imm(l, wide, 0), n->type);
        return plain_load(l, value_address(l, result, wide), type);
    }
    size_t input = 0, output = 0;
    if (op == AT_CLEAR || op == AT_UNLOCK) {
        op = AT_STORE;
        input = value_address(l, imm(l, type, 0), type);
    } else if (op != AT_LOAD) {
        input = pointers ? args[1] : value_address(l, args[1], type);
    }
    if (op != AT_STORE)
        output = pointers ? args[op == AT_LOAD ? 1 : 2] : object_slot(l, type);
    atomic_access(l, type, at, input, output, op | hle);
    return output && !pointers ? plain_load(l, output, type) : 0;
}

static size_t nested_pointer(Lower *l, Symbol *function, Type *type)
{
    Symbol *owner = function->owner;
    size_t frame = slot(l, owner->trampolines->type);
    put(l, (Inst){.code = I_FRAME, .symbol = owner, .dest = frame});
    size_t args[] = {storage_address(l, owner->trampolines), storage_address(l, function), frame};
    return runtime_call(l, "__mcc_nested_pointer", type, args, sizeof args / sizeof *args);
}

static void return_value(Lower *l, size_t result, Loc loc)
{
    Symbol *pool = l->function->function->trampolines;
    if (pool) {
        size_t head = load(l, storage_address(l, pool), pool->type);
        size_t done = new_label(l);
        branch(l, head, done, false, pool->loc);
        runtime_call(l, "__mcc_release_nested_pointers", pool->type->base, &head, 1);
        mark(l, done);
    }
    Type *type = l->function->function->type->base;
    size_t size = result && type->variable_size ? dynamic_size(l, type) : 0;
    put(l, (Inst){.code = I_RETURN, .a = result, .b = size, .loc = loc});
}

static size_t bit_load(Lower *l, size_t at, Type *type, unsigned bit, unsigned width)
{
    if (integer128(type)) {
        size_t bits = plain_load(l, at, type);
        size_t shifted = operation(l, SHL, bits, imm(l, type, 128 - bit - width), type);
        return operation(l, SHR, shifted, imm(l, type, 128 - width), type);
    }
    size_t dest = slot(l, type);
    put(l, (Inst){.code = I_BITLOAD, .a = at, .dest = dest, .type = type, .bit = bit, .width = width});
    return dest;
}

static void bit_store(Lower *l, size_t at, size_t val, Type *type, unsigned bit, unsigned width)
{
    if (integer128(type)) {
        Integer128 mask = wide_shift_left(wide_mask(width), bit);
        Integer128 inverse = {~mask.lo, ~mask.hi};
        size_t previous = plain_load(l, at, type);
        size_t clear = operation(l, '&', previous, immediate_integer(l, type, inverse), type);
        size_t shifted = operation(l, SHL, val, imm(l, type, bit), type);
        size_t field = operation(l, '&', shifted, immediate_integer(l, type, mask), type);
        store(l, at, operation(l, '|', clear, field, type), type);
        return;
    }
    put(l, (Inst){.code = I_BITSTORE, .a = at, .b = val, .type = type, .bit = bit, .width = width});
}

static size_t packed_bit_load(Lower *l, size_t at, Type *type, unsigned bit, unsigned width)
{
    Type *byte = type_int(l->context, 1, true);
    size_t result = 0;
    for (unsigned done = 0; done < width;) {
        unsigned position = (bit + done) % 8;
        unsigned count = 8 - position;
        if (count > width - done)
            count = width - done;
        size_t from = offset(l, at, (bit + done) / 8);
        size_t part = scalar_cast(l, bit_load(l, from, byte, position, count), byte, type);
        if (done)
            part = operation(l, SHL, part, imm(l, type, done), type);
        result = done ? operation(l, '|', result, part, type) : part;
        done += count;
    }
    if (!type->uns && width < type->size * 8) {
        size_t shift = imm(l, type, type->size * 8 - width);
        result = operation(l, SHR, operation(l, SHL, result, shift, type), shift, type);
    }
    return result;
}

static void packed_bit_store(Lower *l, size_t at, size_t value, Type *type, unsigned bit, unsigned width)
{
    Type *byte = type_int(l->context, 1, true);
    for (unsigned done = 0; done < width;) {
        unsigned position = (bit + done) % 8;
        unsigned count = 8 - position;
        if (count > width - done)
            count = width - done;
        size_t into = offset(l, at, (bit + done) / 8);
        size_t part = done ? operation(l, SHR, value, imm(l, type, done), type) : value;
        bit_store(l, into, scalar_cast(l, part, type, byte), byte, position, count);
        done += count;
    }
}

static size_t read_lvalue(Lower *l, Node *n, size_t at)
{
    if (n->flags & PACKED_BIT_FIELD && n->kind == N_MEMBER)
        return packed_bit_load(l, at, n->type, n->bit, n->width);
    return n->width ? bit_load(l, at, n->type, n->bit, n->width) : load(l, at, n->type);
}

static void write_lvalue(Lower *l, Node *n, size_t at, size_t val)
{
    if (n->flags & PACKED_BIT_FIELD && n->kind == N_MEMBER)
        packed_bit_store(l, at, val, n->type, n->bit, n->width);
    else if (n->width)
        bit_store(l, at, val, n->type, n->bit, n->width);
    else
        store(l, at, val, n->type);
}

static size_t write_register_object(Lower *l, Symbol *s, Node *lhs, size_t displacement, size_t val)
{
    size_t raw;
    if (lhs->kind == N_SYMBOL) {
        raw = copy_register_operand(l, val, s->type);
    } else {
        raw = read_register_bits(l, s);
        size_t at = offset(l, frame_slot_address(l, raw, s->type), displacement);
        write_lvalue(l, lhs, at, val);
        if (lhs->width)
            val = read_lvalue(l, lhs, at);
    }
    put(l, (Inst){.code = I_REGISTER_WRITE, .a = raw, .symbol = s, .type = s->type});
    return val;
}

/* Vector element assignment updates a snapshot and writes the complete
   register back. Evaluate the index once for both read-modify-write and store. */
static size_t update_global_vector_element(Lower *l, Node *n, Symbol *reg, size_t displacement)
{
    Node *lhs = n->left;
    Type *index_type = type_int(l->context, 4, false);
    size_t index = value(l, convert(l->context, lhs->right, index_type, lhs->loc));
    size_t before = 0;
    Substitution substitute = {.previous = l->substitution, .node = lhs};
    if (n->kind == N_UPDATE) {
        size_t prior = read_register_bits(l, reg);
        size_t base = offset(l, frame_slot_address(l, prior, reg->type), displacement);
        before = read_lvalue(l, lhs, vector_element_address(l, base, index, lhs->type));
        substitute.value = before;
        l->substitution = &substitute;
    }
    size_t result = value(l, n->right);
    l->substitution = substitute.previous;
    /* The right side may call a function that changes other lanes. */
    size_t raw = read_register_bits(l, reg);
    size_t base = offset(l, frame_slot_address(l, raw, reg->type), displacement);
    size_t at = vector_element_address(l, base, index, lhs->type);
    write_lvalue(l, lhs, at, result);
    put(l, (Inst){.code = I_REGISTER_WRITE, .a = raw, .symbol = reg, .type = reg->type});
    return n->postfix ? before : result;
}

static void copy_value(Lower *l, size_t dest, size_t src)
{
    if (dest)
        put(l, (Inst){.code = I_COPY, .dest = dest, .a = src,
                      .type = l->function->slots.data[dest - 1].type});
}

static void begin_stack_scope(Lower *l, Lifetime *scope)
{
    put(l, (Inst){.code = I_STACK_SAVE, .dest = scope->stack});
    if (scope->pin)
        copy_value(l, scope->pin, imm(l, l->stack_type, 0));
}

static Lifetime *push_lifetime(Lower *l, size_t stack, size_t pin, Node *cleanup)
{
    Lifetime *scope = alloc(l->context, sizeof *scope);
    size_t depth = l->lifetime ? l->lifetime->depth + 1 : 1;
    *scope = (Lifetime){.previous = l->lifetime, .stack = stack, .pin = pin, .depth = depth, .cleanup = cleanup};
    l->lifetime = scope;
    return scope;
}

static void enter_stack_scope(Lower *l, const Node *block)
{
    /* Checkpoints preserve one pin for the function's alloca storage. */
    Symbol *function = l->function->function;
    if (!(block->flags & BLOCK_STACK_ALLOCATION) || function->has_nonlocal_labels ||
        has_stack_checkpoints(function))
        return;
    size_t saved = slot(l, l->stack_type);
    size_t pin = slot(l, l->stack_type);
    begin_stack_scope(l, push_lifetime(l, saved, pin, NULL));
}

static void clamp_stack(Lower *l, size_t restore, size_t pin, Loc loc)
{
    if (!pin)
        return;
    size_t skip = new_label(l);
    branch(l, pin, skip, false, loc);
    size_t earlier = operation(l, '<', pin, restore, l->stack_type);
    branch(l, earlier, skip, false, loc);
    copy_value(l, restore, pin);
    mark(l, skip);
}

static void restore_stack(Lower *l, size_t stack, Lifetime *live, Loc loc)
{
    if (l->stack_pin) {
        size_t address = slot(l, l->stack_type);
        copy_value(l, address, stack);
        clamp_stack(l, address, l->stack_pin, loc);
        for (Lifetime *p = live; p; p = p->previous)
            clamp_stack(l, address, p->pin, loc);
        stack = address;
    }
    size_t done = 0;
    if (l->function->function->has_explicit_stack) {
        /* An explicit restore may already have discarded this scope's stack area. */
        Context *c = l->context;
        size_t current = slot(l, type_int(c, 4, true));
        put(l, (Inst){.code = I_STACK_SAVE, .dest = current});
        size_t discarded = operation(l, '<', stack, current, type_int(c, 3, false));
        done = new_label(l);
        branch(l, discarded, done, true, loc);
    }
    put(l, (Inst){.code = I_STACK_RESTORE, .a = stack});
    if (done)
        mark(l, done);
}

static Lifetime *stack_lifetime(Lifetime *scope)
{
    while (scope && !scope->stack && !scope->variable_type)
        scope = scope->previous;
    return scope;
}

static void run_cleanups(Lower *l, Lifetime *source, Lifetime *target)
{
    for (Lifetime *p = source; p != target; p = p->previous)
        if (p->cleanup)
            value(l, p->cleanup);
}

static bool move_lifetimes(Lower *l, Lifetime *source, Lifetime *target, Loc loc, bool indirect)
{
    Lifetime *from = source, *to = target;
    while (from != to) {
        if (from && (!to || from->depth >= to->depth))
            from = from->previous;
        else
            to = to->previous;
    }
    for (Lifetime *p = target; p != to; p = p->previous) {
        if (p->variable_type) {
            if (!indirect)
                error(l->context, loc, "jump enters the scope of a variably modified declaration");
            return false;
        }
    }
    if (!indirect)
        run_cleanups(l, source, from);
    size_t saved = 0;
    for (Lifetime *p = source; p != from; p = p->previous)
        if (p->stack)
            saved = p->stack;
    if (saved)
        restore_stack(l, saved, from, loc);
    for (Lifetime *p = target; p != to; p = p->previous)
        if (p->stack)
            begin_stack_scope(l, p);
    return true;
}

static void restore(Lower *l, Lifetime *prior, Loc loc)
{
    move_lifetimes(l, l->lifetime, prior, loc, false);
    l->lifetime = prior;
}

static Lifetime *begin_full_expression(Lower *l)
{
    Lifetime *prior = l->lifetime;
    if (l->function->function->has_variable_temporary) {
        size_t saved = slot(l, type_int(l->context, 4, true));
        begin_stack_scope(l, push_lifetime(l, saved, 0, NULL));
    }
    return prior;
}

static size_t scalar_full_expression(Lower *l, Node *expr, FullExpression context)
{
    if (!expr)
        return 0;
    Lifetime *prior = begin_full_expression(l);
    size_t result = context == FULL_CONDITION ? condition_value(l, expr) :
        evaluate(l, expr, context == FULL_EFFECT ? RESULT_DISCARDED : RESULT_USED);
    restore(l, prior, expr->loc);
    return result;
}

static void branch_full_expression(Lower *l, Node *expr, size_t label, bool nonzero, Loc loc)
{
    expr = decay(l->context, expr);
    size_t result = scalar_full_expression(l, expr, FULL_CONDITION);
    branch_expression(l, expr, result, label, nonzero, loc);
}

static void record_allocation_extent(Lower *l, Node *n, Substitution *inputs, size_t address)
{
    Symbol *extents[] = {n->symbol, n->symbol->object_subextent};
    Substitution *previous = l->substitution;
    l->substitution = inputs;
    for (size_t i = 0; i < 2; i++) {
        Symbol *extent = extents[i];
        Node *size_node = i && n->object_subsize ? n->object_subsize : n->third;
        size_t size = value(l, size_node);
        size_t base = address;
        if (!i && n->object_displacement)
            base = operation(l, '-', address, value(l, n->object_displacement), extent->object_base->type);
        store(l, storage_address(l, extent), size, extent->type);
        store(l, storage_address(l, extent->object_base), base, extent->object_base->type);
    }
    l->substitution = previous;
}

static void pin_stack_allocation(Lower *l, size_t result, bool block)
{
    size_t pin = l->stack_pin;
    if (block) {
        for (Lifetime *p = l->lifetime; p; p = p->previous) {
            if (p->pin) {
                pin = p->pin;
                break;
            }
        }
    }
    if (has_stack_checkpoints(l->function->function))
        put(l, (Inst){.code = I_STACK_SAVE, .dest = pin});
    else
        copy_value(l, pin, result);
}

static size_t dynamic_compound_literal(Lower *l, Node *n)
{
    for (size_t i = 0; i < n->list.len; i++)
        value(l, n->list.data[i]);
    size_t result = allocate_variable_temporary(l, n->type, n->type->align);
    pin_stack_allocation(l, result, true);
    zero_variable_object(l, result, n->type);
    return result;
}

static size_t stack_allocation(Lower *l, Node *n)
{
    size_t start = l->function->instructions.len;
    size_t amount = value(l, n->left);
    size_t result = slot(l, n->type);
    if (!l->context->syntax_only) {
        AllocationSite site = {.expression = n, .instruction = l->function->instructions.len,
                               .arguments = {start, 0}};
        PUSH(l->context, l->allocation_sites, site);
    }
    put(l, (Inst){.code = I_STACK_ALLOC, .dest = result, .a = amount, .immediate = n->integer, .access_call = n});
    pin_stack_allocation(l, result, n->flags & BLOCK_STACK_ALLOCATION);
    if (n->third) {
        Substitution input = {l->substitution, n->left, amount};
        record_allocation_extent(l, n, &input, result);
    }
    return result;
}

static size_t remaining_object_size(Lower *l, Node *n)
{
    Type *t = n->type;
    size_t extent = value(l, n->left);
    size_t offset = value(l, n->right);
    size_t outer = value(l, n->third);
    size_t result = slot(l, t), done = new_label(l);
    if (n->integer) {
        size_t unknown = imm(l, t, n->integer);
        copy_value(l, result, unknown);
        branch(l, operation(l, EQ, extent, unknown, t), done, true, n->loc);
        if (outer)
            branch(l, operation(l, EQ, outer, unknown, t), done, true, n->loc);
    }
    if (outer) {
        size_t limit = slot(l, t), capped = new_label(l);
        copy_value(l, limit, extent);
        branch(l, operation(l, '<', outer, extent, t), capped, false, n->loc);
        copy_value(l, limit, outer);
        mark(l, capped);
        extent = limit;
    }
    copy_value(l, result, imm(l, t, 0));
    branch(l, operation(l, '>', offset, extent, t), done, true, n->loc);
    copy_value(l, result, operation(l, '-', extent, offset, t));
    mark(l, done);
    return result;
}

static void update_object_extent(Lower *l, Symbol *s, Node *rhs)
{
    Symbol *extent = s->object_extent;
    if (!extent)
        return;
    Node *source = pointer_value_source(rhs);
    if (source && (source->kind == N_CALL || source->kind == N_ALLOCA || source->kind == N_ADDR ||
                   ((source->kind == N_BINARY || source->kind == N_COND) && source->extent_source)) &&
        source->symbol == extent)
        return;
    Symbol *copied = source && source->kind == N_SYMBOL && source->symbol->owner == s->owner ?
                     source->symbol->object_extent : NULL;
    if (copied == extent)
        return;
    Symbol *extents[] = {extent, extent->object_subextent};
    Symbol *copies[] = {copied, copied ? copied->object_subextent : NULL};
    for (size_t i = 0; i < 2; i++) {
        Symbol *current = extents[i], *from = copies[i];
        size_t size = from ? load(l, storage_address(l, from), from->type) :
                             imm(l, current->type, UINT64_MAX);
        store(l, storage_address(l, current), size, current->type);
        Symbol *base = current->object_base;
        size_t address = from ? load(l, storage_address(l, from->object_base), from->object_base->type) :
                                imm(l, base->type, 0);
        store(l, storage_address(l, base), address, base->type);
    }
}

static void record_call_extent(Lower *l, Node *n, Inst *call)
{
    AllocationSize allocation = call->source_type->allocation;
    size_t first = allocation.first - 1, second = allocation.second ? allocation.second - 1 : first;
    Substitution a = {l->substitution, n->list.data[first], call->args.data[first]};
    Substitution b = {&a, n->list.data[second], call->args.data[second]};
    record_allocation_extent(l, n, &b, call->dest);
}

static void record_offset_extent(Lower *l, Node *n, size_t original, size_t old_size[2],
                                 size_t base[2], size_t result)
{
    Symbol *extents[] = {n->symbol, n->symbol->object_subextent};
    for (size_t i = 0; i < 2; i++) {
        Symbol *extent = extents[i];
        Type *size_type = extent->type;
        size_t unknown = imm(l, size_type, UINT64_MAX);
        size_t updated = slot(l, size_type), done = new_label(l);
        copy_value(l, updated, unknown);
        branch(l, operation(l, EQ, old_size[i], unknown, size_type), done, true, n->loc);
        size_t prior_offset = operation(l, '-', original, base[i], size_type);
        size_t total = operation(l, '+', prior_offset, old_size[i], size_type);
        size_t limit = imm(l, size_type, PTRDIFF_MAX);
        branch(l, operation(l, '>', prior_offset, limit, size_type), done, true, n->loc);
        branch(l, operation(l, '>', total, limit, size_type), done, true, n->loc);
        size_t offset = operation(l, '-', result, base[i], size_type);
        copy_value(l, updated, imm(l, size_type, 0));
        branch(l, operation(l, '>', offset, total, size_type), done, true, n->loc);
        copy_value(l, updated, operation(l, '-', total, offset, size_type));
        mark(l, done);
        store(l, storage_address(l, extent), updated, size_type);
        store(l, storage_address(l, extent->object_base), base[i], extent->object_base->type);
    }
}

static size_t flow_value(Lower *l, Node *n, ResultUse use)
{
    size_t other = new_label(l), done = new_label(l);
    Type *type = n->type;
    if (address_value(type))
        type = pointer(l->context, type);
    size_t dest = type->kind == TY_VOID ? 0 : slot(l, type);
    Node *test_node = n->postfix ? n->left : excess_value(n->left);
    size_t test = value(l, test_node);
    size_t condition = test_node->type->kind == TY_COMPLEX ? complex_truth(l, test, test_node->type) : test;
    bool suppress = l->suppress_unused_result;
    bool selected = false;
    bool known = !suppress && !l->context->syntax_only &&
                 (nonnull_result_truth(l->context, test_node, &selected) ||
                  constant_truth(l->context, test_node, &selected));
    if (n->kind == N_COND) {
        branch_expression(l, test_node, condition, other, false, n->loc);
        /* A value-returning conditional consumes both operands until a branch is selected. */
        ResultUse arm = known || use == RESULT_CAST_TO_VOID ? use : RESULT_USED;
        Substitution substitute = {l->substitution, n->left, test};
        if (n->postfix)
            l->substitution = &substitute;
        l->suppress_unused_result = suppress || (known && !selected);
        copy_value(l, dest, evaluate(l, n->right, arm));
        if (n->extent_source)
            update_object_extent(l, n->extent_source, n->right);
        l->substitution = substitute.previous;
        jump(l, done);
        mark(l, other);
        l->suppress_unused_result = suppress || (known && selected);
        copy_value(l, dest, evaluate(l, n->third, arm));
        if (n->extent_source)
            update_object_extent(l, n->extent_source, n->third);
    } else {
        bool is_or = n->op == OR;
        branch_expression(l, test_node, condition, other, is_or, n->loc);
        l->suppress_unused_result = suppress || (known && selected == is_or);
        size_t right = condition_value(l, n->right);
        mark_nonnull_comparison(l, n->right, n->loc);
        bool right_truth;
        right = nonnull_result_truth(l->context, n->right, &right_truth) ?
            imm(l, type_int(l->context, 0, true), right_truth) : truth(l, right, n->loc);
        size_t zero = imm(l, l->function->slots.data[right - 1].type, 0);
        copy_value(l, dest, operation(l, NE, right, zero, type));
        jump(l, done);
        mark(l, other);
        copy_value(l, dest, imm(l, type, is_or));
    }
    l->suppress_unused_result = suppress;
    mark(l, done);
    return dest;
}

static size_t vector_lane(Lower *l, Node *operand, size_t in, size_t lane, Type *base)
{
    Type *from = operand->type;
    if (from->kind == TY_VECTOR) {
        from = from->base;
        in = plain_load(l, offset(l, in, lane * from->size), from);
    }
    return same_type(from, base) ? in : scalar_cast(l, in, from, base);
}

static size_t vector_operation(Lower *l, Node *n)
{
    Type *type = n->type, *result_type = type->base;
    size_t left = value(l, n->left), right = value(l, n->right);
    size_t result = object_slot(l, type);
    bool comparison = n->op == EQ || n->op == NE || n->op == '<' || n->op == '>' ||
                      n->op == LE || n->op == GE;
    Type *base = n->operand_type ? n->operand_type : result_type;
    Type *comparison_type = comparison ? type_int(l->context, 3, false) : NULL;
    for (size_t lane = 0; lane < type->length; lane++) {
        size_t first = vector_lane(l, n->left, left, lane, base), out = first;
        if (n->right) {
            size_t second = vector_lane(l, n->right, right, lane, base);
            out = operation(l, n->op, first, second, comparison ? comparison_type : result_type);
            if (comparison)
                out = negate_scalar(l, scalar_cast(l, out, comparison_type, result_type), result_type);
        } else if (n->op == '-') {
            out = negate_scalar(l, first, base);
        } else if (n->op == '~') {
            size_t ones = immediate_integer(l, base, (Integer128){UINT64_MAX, UINT64_MAX});
            out = operation(l, '^', first, ones, base);
        }
        store(l, offset(l, result, lane * result_type->size), out, result_type);
    }
    return result;
}

static size_t vector_shuffle(Lower *l, Node *n)
{
    Type *t = n->type, *element = t->base;
    size_t left = value(l, n->left), right = value(l, n->right), mask = value(l, n->third);
    size_t left_count = n->left->type->length, result = object_slot(l, t);
    Type *index_type = type_int(l->context, 4, true);
    for (size_t lane = 0; lane < t->length; lane++) {
        size_t from;
        if (mask) {
            size_t index = vector_lane(l, n->third, mask, lane, index_type);
            size_t source = left;
            if (right) {
                size_t first = new_label(l);
                source = slot(l, pointer(l->context, element));
                copy_value(l, source, left);
                size_t high = operation(l, '&', index, imm(l, index_type, left_count), index_type);
                branch(l, high, first, false, n->loc);
                copy_value(l, source, right);
                mark(l, first);
            }
            size_t wrapped = operation(l, '&', index, imm(l, index_type, left_count - 1), index_type);
            from = vector_element_address(l, source, wrapped, element);
        } else {
            size_t index = n->list.data[lane]->integer;
            from = index < left_count ? offset(l, left, index * element->size) :
                                       offset(l, right, (index - left_count) * element->size);
        }
        store(l, offset(l, result, lane * element->size), plain_load(l, from, element), element);
    }
    return result;
}

typedef struct { size_t begin, end; } BitRange;
typedef VECTOR(BitRange) BitRanges;
typedef struct PaddingStep PaddingStep;
typedef VECTOR(PaddingStep) PaddingSteps;

struct PaddingStep {
    size_t offset, bytes;
    unsigned keep;
    Type *repeat;
    Type *record_tail;
    Type *record_general;
    PaddingSteps element;
};

static void add_value_bits(Context *c, BitRanges *ranges, size_t begin, size_t end)
{
    if (begin >= end)
        return;
    if (ranges->len) {
        BitRange *last = &ranges->data[ranges->len - 1];
        if (begin >= last->begin && begin <= last->end) {
            if (end > last->end)
                last->end = end;
            return;
        }
    }
    PUSH(c, *ranges, ((BitRange){begin, end}));
}

static int compare_bit_ranges(const void *a, const void *b)
{
    const BitRange *left = a, *right = b;
    return (left->begin > right->begin) - (left->begin < right->begin);
}

static void merge_value_bits(Context *c, BitRanges *ranges)
{
    if (ranges->len < 2)
        return;
    size_t count = ranges->len;
    qsort(ranges->data, count, sizeof *ranges->data, compare_bit_ranges);
    ranges->len = 0;
    for (size_t i = 0; i < count; i++) {
        BitRange range = ranges->data[i];
        add_value_bits(c, ranges, range.begin, range.end);
    }
}

static size_t scalar_value_bits(Context *c, Type *t)
{
    if (t->kind == TY_FLOAT && floating_format(c->target, t) == 10)
        return 80;
    return (size_t)t->size * 8;
}

static void collect_value_bits(Context *, Type *, size_t, size_t, BitRanges *);

static void collect_member_bits(Context *c, Type *t, size_t origin, size_t limit,
                                BitRanges *ranges, bool nested)
{
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        size_t begin = origin + m.offset * 8;
        if (m.bit_field) {
            if (!m.name)
                continue;
            begin += m.bit;
            size_t end = begin + m.width;
            if (t->kind == TY_UNION) {
                /* GNU keeps the bytes occupied by a direct union bit-field. */
                begin = begin / 8 * 8;
                end = (end + 7) / 8 * 8;
            }
            add_value_bits(c, ranges, begin, end < limit ? end : limit);
        } else if (nested) {
            collect_value_bits(c, m.type, begin, limit, ranges);
        } else {
            add_value_bits(c, ranges, begin, begin + (size_t)m.type->size * 8);
        }
    }
}

static void collect_value_bits(Context *c, Type *t, size_t origin, size_t limit, BitRanges *ranges)
{
    if (origin >= limit)
        return;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        collect_member_bits(c, t, origin, limit, ranges, true);
    } else if (indexed_elements(t) || t->kind == TY_COMPLEX) {
        size_t count = t->kind == TY_COMPLEX ? 2 : t->length;
        size_t stride = (size_t)t->base->size * 8;
        if (!count || !stride)
            return;
        if (limit - origin < stride) {
            collect_value_bits(c, t->base, origin, limit, ranges);
            return;
        }
        BitRanges element = {0};
        collect_value_bits(c, t->base, 0, stride, &element);
        merge_value_bits(c, &element);
        if (element.len == 1 && !element.data[0].begin && element.data[0].end == stride) {
            size_t end = origin + stride * count;
            add_value_bits(c, ranges, origin, end < limit ? end : limit);
            return;
        }
        if (element.len)
            for (size_t i = 0; i < count && origin < limit; i++, origin += stride)
                for (size_t j = 0; j < element.len; j++) {
                    BitRange span = element.data[j];
                    size_t end = origin + span.end;
                    add_value_bits(c, ranges, origin + span.begin, end < limit ? end : limit);
                }
    } else {
        size_t end = origin + scalar_value_bits(c, t);
        add_value_bits(c, ranges, origin, end < limit ? end : limit);
    }
}

static void clear_bit_range(Context *c, PaddingSteps *steps, size_t origin, size_t begin, size_t end)
{
    if (begin >= end)
        return;
    if (begin % 8) {
        size_t byte_start = begin / 8 * 8;
        size_t next = byte_start + 8;
        unsigned keep = (1u << (begin - byte_start)) - 1;
        if (end < next)
            keep |= (255u << (end - byte_start)) & 255u;
        PUSH(c, *steps, ((PaddingStep){.offset = origin + byte_start / 8, .keep = keep}));
        begin = next;
    }
    if (begin / 8 < end / 8) {
        size_t count = end / 8 - begin / 8;
        PUSH(c, *steps, ((PaddingStep){.offset = origin + begin / 8, .bytes = count}));
        begin = end / 8 * 8;
    }
    if (begin < end) {
        unsigned keep = (255u << (end - begin)) & 255u;
        PUSH(c, *steps, ((PaddingStep){.offset = origin + begin / 8, .keep = keep}));
    }
}

static void plan_gaps(Context *c, BitRanges *live, size_t origin, size_t bits, PaddingSteps *steps)
{
    merge_value_bits(c, live);
    size_t end = 0;
    for (size_t i = 0; i < live->len; i++) {
        clear_bit_range(c, steps, origin, end, live->data[i].begin);
        end = live->data[i].end;
    }
    clear_bit_range(c, steps, origin, end, bits);
}

static bool same_padding_period(Member member, Type *array)
{
    Type *t = member.type;
    return !member.bit_field && t->kind == TY_ARRAY &&
           t->size == array->size && t->base->size == array->base->size;
}

static size_t plan_repeating_union_tail(Context *c, Type *t, size_t origin, PaddingSteps *steps)
{
    Type *array = NULL;
    for (size_t i = 0; i < t->members.len; i++) {
        Type *member = t->members.data[i].type;
        if (member->kind == TY_ARRAY && member->size && (!array || member->size > array->size))
            array = member;
    }
    if (!array)
        return t->size;

    /* Past the shorter members, the remaining array elements have one repeating layout. */
    size_t prefix = 0, stride = array->base->size;
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        if (!same_padding_period(m, array)) {
            size_t end = m.bit_field ? (m.bit + m.width + 7) / 8 : m.type->size;
            if (end > prefix)
                prefix = end;
        }
    }
    prefix = (prefix + stride - 1) / stride * stride;
    if (prefix >= array->size)
        return t->size;

    BitRanges live = {0};
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        if (same_padding_period(m, array))
            collect_value_bits(c, m.type->base, 0, stride * 8, &live);
    }
    PaddingSteps element = {0};
    plan_gaps(c, &live, 0, stride * 8, &element);
    if (element.len)
        PUSH(c, *steps, ((PaddingStep){.offset = origin + prefix, .bytes = array->size - prefix,
                                      .repeat = array, .element = element}));
    clear_bit_range(c, steps, origin, (size_t)array->size * 8, (size_t)t->size * 8);
    return prefix;
}

static void plan_padding(Context *c, Type *t, size_t origin, PaddingSteps *steps)
{
    if (!t->size && !t->variable_size)
        return;
    if (indexed_elements(t) || t->kind == TY_COMPLEX) {
        PaddingSteps element = {0};
        plan_padding(c, t->base, 0, &element);
        if (element.len)
            PUSH(c, *steps, ((PaddingStep){.offset = origin, .bytes = t->size, .repeat = t,
                                          .element = element}));
        return;
    }
    if (t->kind != TY_STRUCT && t->kind != TY_UNION) {
        clear_bit_range(c, steps, origin, scalar_value_bits(c, t), (size_t)t->size * 8);
        return;
    }
    if (t->variable_size) {
        bool trailing_array = (t->kind == TY_STRUCT || t->members.len == 1) && t->members.len &&
                              t->members.data[t->members.len - 1].type->kind == TY_ARRAY &&
                              t->members.data[t->members.len - 1].type->variable_size;
        for (size_t i = 0; trailing_array && i + 1 < t->members.len; i++)
            trailing_array = !t->members.data[i].type->variable_size;
        if (!trailing_array) {
            PUSH(c, *steps, ((PaddingStep){.offset = origin, .record_general = t}));
            return;
        }
        Member tail = t->members.data[t->members.len - 1];
        Type prefix = *t;
        prefix.members.len--;
        prefix.size = (unsigned)tail.offset;
        prefix.variable_size = false;
        plan_padding(c, &prefix, origin, steps);
        plan_padding(c, tail.type, origin + tail.offset, steps);
        PUSH(c, *steps, ((PaddingStep){.offset = origin + tail.offset, .record_tail = t}));
        return;
    }

    /* The live bits of a union are the union of the live bits of its members. */
    bool overlap = t->kind == TY_UNION && t->members.len > 1;
    size_t bytes = overlap ? plan_repeating_union_tail(c, t, origin, steps) : t->size;
    BitRanges live = {0};
    collect_member_bits(c, t, 0, bytes * 8, &live, overlap);
    plan_gaps(c, &live, origin, bytes * 8, steps);
    if (!overlap)
        for (size_t i = 0; i < t->members.len; i++) {
            Member m = t->members.data[i];
            if (!m.bit_field)
                plan_padding(c, m.type, origin + m.offset, steps);
        }
}

/* Add the value bits covering one byte of an object to a runtime mask. */
static void mark_value_byte(Lower *l, Type *t, size_t position, size_t mask, Loc loc)
{
    Context *c = l->context;
    Type *size_type = type_int(c, 4, true);
    Type *word = type_int(c, 3, true);
    Type *condition_type = type_int(c, 3, false);
    if (!t->variable_size && !t->size)
        return;
    if (indexed_elements(t) || t->kind == TY_COMPLEX) {
        if (!indexed_elements(t->base) && t->base->kind != TY_COMPLEX &&
            t->base->kind != TY_STRUCT && t->base->kind != TY_UNION &&
            scalar_value_bits(c, t->base) == (size_t)t->base->size * 8) {
            copy_value(l, mask, operation(l, '|', mask, imm(l, word, 255), word));
            return;
        }
        size_t stride = t->base->variable_size ? dynamic_size(l, t->base) : imm(l, size_type, t->base->size);
        size_t within = operation(l, '%', position, stride, size_type);
        mark_value_byte(l, t->base, within, mask, loc);
        return;
    }
    if (t->kind != TY_STRUCT && t->kind != TY_UNION) {
        size_t bits = scalar_value_bits(c, t);
        size_t skip = 0;
        if (bits < (size_t)t->size * 8) {
            skip = new_label(l);
            size_t outside = operation(l, GE, position, imm(l, size_type, bits / 8), condition_type);
            branch(l, outside, skip, true, loc);
        }
        copy_value(l, mask, operation(l, '|', mask, imm(l, word, 255), word));
        if (skip)
            mark(l, skip);
        return;
    }
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        size_t start = m.runtime_offset ? value(l, m.runtime_offset) : imm(l, size_type, m.offset);
        if (m.bit_field) {
            if (!m.name || !m.width)
                continue;
            unsigned first = m.bit / 8, last = (m.bit + m.width - 1) / 8;
            for (unsigned byte = first; byte <= last; byte++) {
                unsigned begin = m.bit > byte * 8 ? m.bit - byte * 8 : 0;
                unsigned end = m.bit + m.width < byte * 8 + 8 ? m.bit + m.width - byte * 8 : 8;
                unsigned bits = t->kind == TY_UNION ? 255 : ((1u << (end - begin)) - 1) << begin;
                size_t at = byte ? operation(l, '+', start, imm(l, size_type, byte), size_type) : start;
                size_t skip = new_label(l);
                size_t different = operation(l, NE, position, at, condition_type);
                branch(l, different, skip, true, loc);
                copy_value(l, mask, operation(l, '|', mask, imm(l, word, bits), word));
                mark(l, skip);
            }
            continue;
        }
        if (!m.type->variable_size && !m.type->size)
            continue;
        size_t extent = m.type->variable_size ? dynamic_size(l, m.type) : imm(l, size_type, m.type->size);
        size_t end = operation(l, '+', start, extent, size_type);
        size_t skip = new_label(l);
        size_t before = operation(l, '<', position, start, condition_type);
        branch(l, before, skip, true, loc);
        size_t after = operation(l, GE, position, end, condition_type);
        branch(l, after, skip, true, loc);
        size_t within = operation(l, '-', position, start, size_type);
        mark_value_byte(l, m.type, within, mask, loc);
        mark(l, skip);
    }
}

static void lower_general_padding(Lower *l, size_t at, Type *record, Loc loc)
{
    Context *c = l->context;
    Type *size_type = type_int(c, 4, true);
    Type *word = type_int(c, 3, true);
    Type *byte = type_int(c, 1, true);
    Type *condition_type = type_int(c, 3, false);
    Type *pointer_type = l->function->slots.data[at - 1].type;
    size_t length = dynamic_size(l, record);
    size_t index = slot(l, size_type), mask = slot(l, word);
    copy_value(l, index, imm(l, size_type, 0));
    size_t loop = new_label(l), done = new_label(l), advance = new_label(l);
    mark(l, loop);
    size_t finished = operation(l, GE, index, length, condition_type);
    branch(l, finished, done, true, loc);
    copy_value(l, mask, imm(l, word, 0));
    mark_value_byte(l, record, index, mask, loc);
    size_t complete = operation(l, EQ, mask, imm(l, word, 255), condition_type);
    branch(l, complete, advance, true, loc);
    size_t place = operation(l, '+', at, index, pointer_type);
    size_t partial = new_label(l), written = new_label(l);
    branch(l, mask, partial, true, loc);
    store(l, place, imm(l, byte, 0), byte);
    jump(l, written);
    mark(l, partial);
    size_t old = plain_load(l, place, byte);
    store(l, place, operation(l, '&', old, mask, byte), byte);
    mark(l, written);
    mark(l, advance);
    copy_value(l, index, operation(l, '+', index, imm(l, size_type, 1), size_type));
    jump(l, loop);
    mark(l, done);
}

static void lower_padding(Lower *l, size_t at, PaddingSteps steps, Loc loc)
{
    Context *c = l->context;
    for (size_t i = 0; i < steps.len; i++) {
        PaddingStep step = steps.data[i];
        size_t place = offset(l, at, step.offset);
        if (step.record_general) {
            lower_general_padding(l, place, step.record_general, loc);
        } else if (step.record_tail) {
            Type *record = step.record_tail;
            Type *array = record->members.data[record->members.len - 1].type;
            Type *pointer_type = l->function->slots.data[place - 1].type;
            Type *byte = type_int(c, 1, true);
            size_t current = slot(l, pointer_type);
            copy_value(l, current, operation(l, '+', place, dynamic_size(l, array), pointer_type));
            size_t start = offset(l, at, step.offset - record->members.data[record->members.len - 1].offset);
            size_t end = operation(l, '+', start, dynamic_size(l, record), pointer_type);
            size_t loop = new_label(l), done = new_label(l);
            mark(l, loop);
            size_t finished = operation(l, EQ, current, end, type_int(c, 3, false));
            branch(l, finished, done, true, loc);
            store(l, current, imm(l, byte, 0), byte);
            copy_value(l, current, operation(l, '+', current, imm(l, pointer_type, 1), pointer_type));
            jump(l, loop);
            mark(l, done);
        } else if (step.repeat) {
            Type *t = step.repeat;
            Type *pointer_type = l->function->slots.data[place - 1].type;
            Type *size_type = type_int(c, 4, true);
            size_t extent = t->variable_size ? dynamic_size(l, t) : imm(l, size_type, step.bytes);
            size_t stride = t->base->variable_size ? dynamic_size(l, t->base) : imm(l, size_type, t->base->size);
            size_t current = slot(l, pointer_type);
            copy_value(l, current, place);
            size_t end = operation(l, '+', place, extent, pointer_type);
            size_t loop = new_label(l), done = new_label(l);
            mark(l, loop);
            size_t finished = operation(l, EQ, current, end, type_int(c, 3, false));
            branch(l, finished, done, true, loc);
            lower_padding(l, current, step.element, loc);
            copy_value(l, current, operation(l, '+', current, stride, pointer_type));
            jump(l, loop);
            mark(l, done);
        } else if (step.bytes) {
            put(l, (Inst){.code = I_MEMZERO, .a = place, .immediate = step.bytes});
        } else {
            Type *byte = type_int(c, 1, true);
            size_t mask = imm(l, byte, step.keep);
            size_t contents = plain_load(l, place, byte);
            store(l, place, operation(l, '&', contents, mask, byte), byte);
        }
    }
}

static void dwarf_register_sizes(Lower *l, size_t buffer)
{
    typedef struct { unsigned start, count, bytes; } RegisterRange;
    static const RegisterRange aarch64[] = {
        {0, 32, 8}, {46, 1, 8}, {48, 24, 0}, {72, 8, 8}, {80, 16, 0}, {96, 1, 8}
    };
    static const RegisterRange x86_64[] = {{0, 17, 8}};
    Context *c = l->context;
    bool arm = c->target == &aarch64_linux;
    const RegisterRange *ranges = arm ? aarch64 : x86_64;
    size_t count = arm ? sizeof aarch64 / sizeof *aarch64 : sizeof x86_64 / sizeof *x86_64;
    Type *word = type_int(c, 4, true), *byte = type_int(c, 1, true);
    for (size_t i = 0; i < count; i++) {
        RegisterRange range = ranges[i];
        if (!range.bytes) {
            size_t at = offset(l, buffer, range.start);
            put(l, (Inst){.code = I_MEMZERO, .a = at, .immediate = range.count});
            continue;
        }
        size_t pattern = imm(l, word, UINT64_C(0x0101010101010101) * range.bytes);
        size_t end = range.start + range.count, at = range.start;
        for (; end - at >= 8; at += 8)
            store(l, offset(l, buffer, at), pattern, word);
        for (; at < end; at++)
            store(l, offset(l, buffer, at), pattern, byte);
    }
}

static size_t frame_query(Lower *l, Node *n)
{
    Context *c = l->context;
    Type *t = n->type;
    Builtin builtin = n->op;
    if (builtin == BUILTIN_EXTRACT_RETURN_ADDR || builtin == BUILTIN_FROB_RETURN_ADDR)
        return value(l, n->left);
    if (builtin == BUILTIN_EXTEND_POINTER)
        return scalar_cast(l, value(l, n->left), n->left->type, t);
    if (builtin == BUILTIN_DWARF_REG_SIZES) {
        dwarf_register_sizes(l, value(l, n->left));
        return 0;
    }
    if (builtin == BUILTIN_DWARF_SP_COLUMN)
        return imm(l, t, c->target == &aarch64_linux ? 31 : 7);
    if (builtin == BUILTIN_EH_DATA_REGNO) {
        unsigned count = c->target == &aarch64_linux ? 4 : 2;
        return imm(l, t, n->integer < count ? n->integer : UINT64_MAX);
    }

    size_t frame = slot(l, t);
    put(l, (Inst){.code = I_FRAME, .dest = frame, .symbol = l->function->function});
    if (builtin == BUILTIN_DWARF_CFA)
        return offset(l, frame, 2 * c->target->pointer_size);

    size_t missing = 0;
    if (n->integer) {
        if (!c->syntax_only)
            warning_at(c, WARNING_FRAME_ADDRESS, n->loc,
                       "calling '%s' with a nonzero argument is unsafe", n->name);
        Type *word = type_int(c, 4, true);
        size_t count = imm(l, word, n->integer), one = imm(l, word, 1);
        size_t alignment_mask = imm(l, word, c->target->pointer_size - 1);
        size_t next_frame = new_label(l);
        missing = new_label(l);
        mark(l, next_frame);
        size_t next = plain_load(l, frame, t);
        /* Frame links on both targets point up the stack and end at a sentinel. */
        size_t past_top = operation(l, LE, next, frame, type_int(c, 3, false));
        branch(l, past_top, missing, true, n->loc);
        size_t misaligned = operation(l, '&', next, alignment_mask, word);
        branch(l, misaligned, missing, true, n->loc);
        copy_value(l, frame, next);
        copy_value(l, count, operation(l, '-', count, one, word));
        branch(l, count, next_frame, true, n->loc);
    }

    size_t result = frame;
    if (builtin == BUILTIN_RETURN_ADDRESS) {
        result = slot(l, t);
        put(l, (Inst){.code = I_FRAME_RETURN, .dest = result, .a = frame});
    }
    if (missing) {
        size_t done = new_label(l);
        jump(l, done);
        mark(l, missing);
        copy_value(l, result, imm(l, t, 0));
        mark(l, done);
    }
    return result;
}

static size_t evaluate(Lower *l, Node *n, ResultUse use)
{
    if (!n)
        return 0;
    for (Substitution *sub = l->substitution; sub; sub = sub->previous)
        if (n == sub->node)
            return sub->value;
    Type *t = n->type;
    switch (n->kind) {
    case N_RANGE_VALUE:
        if (!n->integer) {
            if (address_value(t)) {
                size_t copy = object_slot(l, t);
                put(l, (Inst){.code = I_MEMZERO, .a = copy, .immediate = t->size});
                init_object(l, copy, t, n->left);
                n->integer = copy;
            } else {
                n->integer = value(l, n->left);
            }
        }
        return n->integer;
    case N_INT: return immediate_integer(l, t, (Integer128){n->integer, n->integer_high});
    case N_FLOAT: {
        if (t->kind == TY_DECIMAL || (n->flags & FLOAT_BITS_LITERAL))
            return immediate_integer(l, t, (Integer128){n->integer, n->integer_high});
        size_t dest = slot(l, t);
        Integer128 bits = {0};
        if (binary128(l->context->target, t) && !quad_constant(l->context, n, &bits, false))
            error(l->context, n->loc, "invalid binary128 constant");
        put(l, (Inst){.code = I_REAL, .dest = dest, .real = n->real, .type = t,
                      .immediate = bits.lo, .immediate_high = bits.hi});
        return dest;
    }
    case N_FLOAT_BUILTIN:
        return floating_value(l, n);
    case N_COMPLEX: {
        ComplexParts parts = {.real = value(l, n->left)};
        parts.imaginary = value(l, n->right);
        return make_complex(l, t, parts);
    }
    case N_MEMBER: case N_VECTOR_ELEMENT: case N_SYMBOL: case N_DEREF: case N_COMPOUND: {
        size_t displacement;
        Symbol *reg = explicit_register_object(l->context, n, &displacement);
        if (!reg)
            return read_lvalue(l, n, address(l, n));
        if (!global_register(reg))
            return read_lvalue(l, n, offset(l, address_symbol(l, reg), displacement));
        size_t snapshot = read_register(l, reg);
        return n->kind == N_SYMBOL ? snapshot : read_lvalue(l, n, offset(l, snapshot, displacement));
    }
    case N_ADDR:
        if (explicit_register_object(l->context, n->left, NULL)) {
            error(l->context, n->loc, "cannot take address of explicit register variable");
            return imm(l, t, 0);
        }
        if (n->left->kind == N_SYMBOL && n->left->type->kind == TY_FUNC && n->left->symbol->owner)
            return nested_pointer(l, n->left->symbol, t);
        {
            size_t result = address(l, n->left);
            if (n->third)
                record_allocation_extent(l, n, NULL, result);
            return result;
        }
    case N_COND: return flow_value(l, n, use);
    case N_VECTOR_CONVERT: return vector_operation(l, n);
    case N_VECTOR_SHUFFLE: return vector_shuffle(l, n);
    case N_SEQUENCE:
        evaluate(l, n->left, RESULT_DISCARDED);
        return evaluate(l, n->right, use);
    case N_STATEMENT_EXPR: return block_value(l, n->left, n->right);
    case N_TRAP:
        put(l, (Inst){.code = I_TRAP});
        return 0;
    case N_SPECULATION_SAFE_VALUE: {
        size_t result = value(l, n->left);
        for (size_t i = 0; i < n->list.len; i++)
            value(l, n->list.data[i]);
        put(l, (Inst){.code = I_SPECULATION_BARRIER});
        return result;
    }
    case N_SYSTEM_REGISTER: {
        size_t input = n->left ? value(l, n->left) : 0;
        size_t result = n->left ? 0 : slot(l, t);
        put(l, (Inst){.code = I_SYSTEM_REGISTER, .dest = result, .a = input, .type = t, .op = n->op});
        return result;
    }
    case N_PREFETCH: {
        size_t at = value(l, n->left);
        for (size_t i = 0; i < n->list.len; i++)
            value(l, n->list.data[i]);
        put(l, (Inst){.code = I_PREFETCH, .a = at, .op = n->op, .immediate = n->integer});
        return 0;
    }
    case N_CLEAR_CACHE: {
        size_t start = value(l, n->left);
        size_t end = value(l, n->right);
        if (l->context->target == &aarch64_linux) {
            size_t args[] = {start, end};
            runtime_call(l, "__clear_cache", n->type, args, 2);
        }
        return 0;
    }
    case N_CLEAR_PADDING: {
        size_t at = value(l, n->left);
        PaddingSteps steps = {0};
        plan_padding(l->context, n->operand_type, 0, &steps);
        lower_padding(l, at, steps, n->loc);
        return 0;
    }
    case N_SETJMP: {
        size_t buffer = value(l, n->left);
        size_t result = slot(l, t);
        put(l, (Inst){.code = I_SETJMP, .a = buffer, .b = l->stack_pin, .dest = result});
        return result;
    }
    case N_LONGJMP:
        put(l, (Inst){.code = I_LONGJMP, .a = value(l, n->left)});
        return 0;
    case N_FRAME_QUERY: return frame_query(l, n);
    case N_STACK_SAVE: {
        size_t saved = slot(l, t);
        put(l, (Inst){.code = I_STACK_SAVE, .dest = saved, .b = l->stack_pin});
        return saved;
    }
    case N_STACK_RESTORE:
        put(l, (Inst){.code = I_STACK_RESTORE, .a = value(l, n->left), .b = l->stack_pin});
        return 0;
    case N_ATOMIC: return atomic_value(l, n);
    case N_OVERFLOW: return checked_arithmetic(l, n);
    case N_CARRY: return arithmetic_with_carry(l, n);
    case N_OBJECT_REMAINING: return remaining_object_size(l, n);
    case N_ALLOCA: return stack_allocation(l, n);
    case N_VA_END:
        address(l, n->left);
        return 0;
    case N_VA_START: {
        size_t at = address(l, n->left);
        put(l, (Inst){.code = I_VA_START, .a = at, .loc = n->loc});
        return 0;
    }
    case N_VA_ARG: {
        size_t at = address(l, n->left);
        if (t->variable_size)
            t = pointer(l->context, t);
        size_t destination = address_value(t) ? object_slot(l, t) : slot(l, t);
        put(l, (Inst){.code = I_VA_ARG, .a = at, .dest = destination, .type = t, .loc = n->loc});
        return destination;
    }
    case N_CAST: {
        size_t in = evaluate(l, n->left, t->kind == TY_VOID ? RESULT_CAST_TO_VOID : use);
        Type *from = n->left->type;
        if (t->kind == TY_BOOL) {
            mark_nonnull_comparison(l, n->left, n->loc);
            bool truth_value;
            if (nonnull_result_truth(l->context, n, &truth_value))
                return imm(l, t, truth_value);
        }
        if (aggregate(t))
            return in;
        if (t->kind == TY_VOID)
            return 0;
        if (t->kind == TY_VECTOR)
            return value_address(l, in, from);
        if (from->kind == TY_VECTOR)
            return plain_load(l, in, t);
        if (from->kind == TY_COMPLEX) {
            if (t->kind == TY_BOOL)
                return complex_truth(l, in, from);
            ComplexParts parts = split_complex(l, in, from);
            if (t->kind != TY_COMPLEX)
                return scalar_cast(l, parts.real, from->base, t);
            parts.real = scalar_cast(l, parts.real, from->base, t->base);
            parts.imaginary = scalar_cast(l, parts.imaginary, from->base, t->base);
            return make_complex(l, t, parts);
        }
        if (t->kind == TY_COMPLEX) {
            ComplexParts parts = {.real = scalar_cast(l, in, from, t->base)};
            parts.imaginary = scalar_zero(l, t->base);
            return make_complex(l, t, parts);
        }
        return scalar_cast(l, in, from, t);
    }
    case N_UNARY: {
        if (t->kind == TY_VECTOR)
            return vector_operation(l, n);
        size_t in = value(l, n->left);
        if (n->op == '!') {
            mark_nonnull_comparison(l, n->left, n->loc);
            bool truth_value;
            if (nonnull_result_truth(l->context, n, &truth_value))
                return imm(l, t, truth_value);
        }
        Type *from = n->left->type;
        if (from->kind == TY_COMPLEX) {
            if (n->op == '!') {
                size_t nonzero = complex_truth(l, in, from);
                Type *boolean = type_int(l->context, 0, true);
                return operation(l, EQ, nonzero, imm(l, boolean, 0), t);
            }
            ComplexParts parts = split_complex(l, in, from);
            if (n->op == '-')
                parts.real = negate_scalar(l, parts.real, from->base);
            parts.imaginary = negate_scalar(l, parts.imaginary, from->base);
            return make_complex(l, t, parts);
        }
        if (integer128(from)) {
            if (n->op == '~') {
                size_t ones = immediate_integer(l, from, (Integer128){UINT64_MAX, UINT64_MAX});
                return operation(l, '^', in, ones, t);
            }
            size_t zero = imm(l, from, 0);
            return n->op == '-' ? operation(l, '-', zero, in, t) : operation(l, EQ, in, zero, t);
        }
        if (narrow_float(from) || x86_quad(l, from) || from->kind == TY_DECIMAL) {
            if (n->op == '-')
                return negate_scalar(l, in, from);
            Type *boolean = type_int(l->context, 0, true);
            return operation(l, EQ, truth(l, in, n->loc), imm(l, boolean, 0), t);
        }
        size_t dest = slot(l, t);
        put(l, (Inst){.code = I_UNARY, .dest = dest, .a = in, .op = n->op,
                      .type = t, .source_type = n->left->type});
        return dest;
    }
    case N_BINARY: {
        if (t->kind == TY_VECTOR)
            return vector_operation(l, n);
        if (n->op == AND || n->op == OR)
            return flow_value(l, n, use);
        if (n->operand_type && n->operand_type->kind == TY_COMPLEX)
            return complex_binary(l, n);
        size_t a = value(l, n->left);
        size_t old_size[2] = {0}, old_base[2] = {0};
        if (n->extent_source) {
            Symbol *extents[] = {n->extent_source->object_extent, n->extent_source->object_subextent};
            for (size_t i = 0; i < 2; i++) {
                Symbol *extent = extents[i];
                old_size[i] = load(l, storage_address(l, extent), extent->type);
                old_base[i] = load(l, storage_address(l, extent->object_base), extent->object_base->type);
            }
        }
        size_t b = value(l, n->right);
        mark_nonnull_comparison(l, compared_to_null(l->context, n), n->loc);
        bool truth_value;
        if (nonnull_result_truth(l->context, n, &truth_value))
            return imm(l, t, truth_value);
        size_t result = operation(l, n->op, a, b, t);
        if (n->extent_source)
            record_offset_extent(l, n, a, old_size, old_base, result);
        return result;
    }
    case N_ASSIGN: case N_UPDATE: {
        Node *lhs = n->left;
        size_t displacement;
        if (lhs->kind == N_VECTOR_ELEMENT) {
            Symbol *owner = explicit_register_object(l->context, lhs->left, &displacement);
            if (owner && global_register(owner))
                return update_global_vector_element(l, n, owner, displacement);
        }
        Symbol *reg = explicit_register_object(l->context, lhs, &displacement);
        size_t at = !reg ? address(l, lhs) : global_register(reg) ? 0 :
                    offset(l, address_symbol(l, reg), displacement);
        bool atomic = (lhs->type->qualifiers & S_ATOMIC) != 0;
        if (atomic && n->kind == N_UPDATE) {
            if (at)
                return atomic_update(l, n, at);
            error(l->context, lhs->loc, "atomic updates of register subobjects are unsupported");
            return value(l, lhs);
        }
        size_t before = 0;
        Substitution substitute = {.previous = l->substitution, .node = lhs};
        if (n->kind == N_UPDATE) {
            before = at ? read_lvalue(l, lhs, at) : value(l, lhs);
            if (t->kind == TY_COMPLEX || t->kind == TY_VECTOR) {
                size_t snapshot = object_slot(l, t);
                put(l, (Inst){.code = I_MEMCOPY, .a = snapshot, .b = before, .immediate = t->size});
                before = snapshot;
            }
            substitute.value = before;
            l->substitution = &substitute;
        }
        size_t val = value(l, n->right);
        l->substitution = substitute.previous;
        if (!at) {
            val = write_register_object(l, reg, lhs, displacement, val);
        } else {
            write_lvalue(l, lhs, at, val);
            if (lhs->kind == N_SYMBOL)
                record_store_value(l, at, n->right);
            if (!n->postfix && lhs->width)
                val = read_lvalue(l, lhs, at);
        }
        if (lhs->kind == N_SYMBOL) {
            update_object_extent(l, lhs->symbol, n->right);
            Symbol *param = nonnull_parameter_symbol(l, lhs);
            if (param && !l->context->syntax_only)
                put(l, (Inst){.code = I_NONNULL_WRITE, .symbol = param});
        }
        return n->postfix ? before : at && address_value(t) && !atomic ? at : val;
    }
    case N_CALL: {
        Inst call = {.code = I_CALL, .type = t, .source_type = n->left->type->base, .loc = n->loc};
        AllocationSize allocation = call.source_type->allocation;
        AllocationSite site = {0};
        if (!l->context->syntax_only && allocation.first && allocation.first <= n->list.len &&
            allocation.second <= n->list.len)
            site.expression = n;
        if (!l->context->syntax_only && call.source_type->accesses) {
            call.access_call = n;
            call.access_argument_starts = alloc(l->context, n->list.len * sizeof *call.access_argument_starts);
            l->has_access_calls = true;
        }
        Node *callee = n->left;
        bool selected;
        while (callee->kind == N_COND && !callee->postfix &&
               constant_control_truth(l->context, callee->left, &selected) &&
               !expression_has_effect(l->context, callee->left))
            callee = selected ? callee->right : callee->third;
        if (callee->kind == N_ADDR && callee->left->kind == N_SYMBOL && callee->left->type->kind == TY_FUNC) {
            call.symbol = callee->left->symbol;
            l->has_call_diagnostics |= call.symbol->call_diagnostics.len != 0;
        } else {
            call.a = value(l, callee);
        }
        if (use != RESULT_USED && call.source_type->warn_unused_result &&
            !l->context->syntax_only && !l->suppress_unused_result) {
            if (call.symbol)
                warning_at(l->context, WARNING_UNUSED_RESULT, n->loc,
                           "ignoring return value of '%s' declared with attribute 'warn_unused_result'",
                           call.symbol->name);
            else
                warning_at(l->context, WARNING_UNUSED_RESULT, n->loc,
                           "ignoring return value of function declared with attribute 'warn_unused_result'");
        }
        for (size_t i = 0; i < n->list.len; i++) {
            Node *arg = n->list.data[i];
            if (call.access_argument_starts)
                call.access_argument_starts[i] = l->function->instructions.len;
            if (site.expression) {
                if (i + 1 == allocation.first)
                    site.arguments[0] = l->function->instructions.len;
                if (i + 1 == allocation.second)
                    site.arguments[1] = l->function->instructions.len;
            }
            size_t id = value(l, arg);
            Type *type = arg->type;
            if (i < call.source_type->members.len) {
                Type *parameter = call.source_type->members.data[i].type;
                if (parameter->transparent_union) {
                    type = parameter_abi_type(parameter);
                    id = plain_load(l, id, type);
                }
            }
            if (type->variable_size) {
                size_t copy = allocate_variable_temporary(l, type, type->align);
                store(l, copy, id, type);
                id = copy;
                type = pointer(l->context, type);
            } else if (address_value(type)) {
                size_t object = object_slot(l, type);
                put(l, (Inst){.code = I_MEMCOPY, .a = object, .b = id, .immediate = type->size});
                id = object;
            }
            PUSH(l->context, call.args, id);
            PUSH(l->context, call.arg_types, type);
        }
        if (n->right)
            call.b = value(l, n->right);
        if (t->variable_size)
            call.dest = allocate_variable_temporary(l, t, t->align);
        else if (t->kind != TY_VOID)
            call.dest = address_value(t) ? object_slot(l, t) : slot(l, t);
        if (site.expression) {
            site.instruction = l->function->instructions.len;
            PUSH(l->context, l->allocation_sites, site);
        }
        put(l, call);
        if (n->left->type->noreturn_pointer || function_noreturn(l->context, call.symbol))
            put(l, (Inst){.code = I_TRAP});
        else if (n->third)
            record_call_extent(l, n, &call);
        return call.dest;
    }
    default:
        error(l->context, n->loc, "cannot lower this expression");
        return imm(l, type_int(l->context, 3, false), 0);
    }
}

static void init_object(Lower *l, size_t at, Type *t, Node *init)
{
    if (init->kind == N_INIT) {
        for (size_t i = 0; i < init->list.len; i++) {
            Node *item = init->list.data[i];
            size_t index = item->integer;
            Member m = indexed_elements(t) ? (Member){.type=t->base, .offset=index * t->base->size}
                                          : t->members.data[index];
            size_t place = offset(l, at, m.offset);
            if (m.packed_bit_field)
                packed_bit_store(l, place, value(l, item->left), m.type, m.bit, m.width);
            else if (m.bit_field)
                bit_store(l, place, value(l, item->left), m.type, m.bit, m.width);
            else
                init_object(l, place, m.type, item->left);
        }
    } else if (t->kind == TY_ARRAY && init->kind == N_SYMBOL && init->symbol->bytes) {
        size_t from = value(l, init);
        size_t length = init->symbol->byte_count;
        if (length > t->size)
            length = t->size;
        put(l, (Inst){.code = I_MEMCOPY, .a = at, .b = from, .immediate = length});
    } else {
        store(l, at, value(l, init), t);
    }
}

static Label *named_label(Lower *l, Node *n)
{
    for (size_t i = 0; i < l->labels.len; i++)
        if (l->labels.data[i].symbol == n->symbol)
            return &l->labels.data[i];
    Label label = {.symbol = n->symbol, .number = new_label(l)};
    PUSH(l->context, l->labels, label);
    return &l->labels.data[l->labels.len - 1];
}

static void goto_label(Lower *l, Node *n)
{
    size_t target = named_label(l, n)->number;
    Goto source = {l->function->instructions.len, target, l->lifetime, n->loc};
    PUSH(l->context, l->gotos, source);
    put(l, (Inst){.code = I_JUMP, .label = target, .op = 1});
}

static void gather_cases(Lower *l, Cases *cases, Node *n, Type *t)
{
    if (!n || n->kind == N_SWITCH)
        return;
    if (n->kind == N_CASE || n->kind == N_DEFAULT) {
        Case item = {.node = n, .label = new_label(l), .otherwise = n->kind == N_DEFAULT};
        if (!item.otherwise) {
            Node *low = convert(l->context, n->left, t, n->loc);
            if (!constant_integer(l->context, low, &item.value))
                error(l->context, n->loc, "case value must be an integer constant");
            item.high = item.value;
            if (n->third && !constant_integer(l->context, convert(l->context, n->third, t, n->loc), &item.high))
                error(l->context, n->loc, "case range must have constant bounds");
        }
        for (size_t i = 0; i < cases->len; i++) {
            Case prior = cases->data[i];
            bool overlaps = wide_compare(item.value, prior.high, t->uns) <= 0 &&
                            wide_compare(prior.value, item.high, t->uns) <= 0;
            if (prior.otherwise == item.otherwise && (item.otherwise || overlaps))
                error(l->context, n->loc, "duplicate or overlapping switch case");
        }
        PUSH(l->context, *cases, item);
        gather_cases(l, cases, n->right, t);
        return;
    }
    switch (n->kind) {
    case N_BLOCK:
        for (size_t i = 0; i < n->list.len; i++)
            gather_cases(l, cases, n->list.data[i], t);
        break;
    case N_IF: gather_cases(l, cases, n->third, t); /* fall through */
    case N_LOOP: case N_DO: gather_cases(l, cases, n->right, t); break;
    case N_LABEL: gather_cases(l, cases, n->left, t); break;
    default: break;
    }
}

static void switch_statement(Lower *l, Node *n)
{
    Context *c = l->context;
    Type *t = n->left->type;
    if (t->kind != TY_INT && t->kind != TY_BOOL) {
        error(c, n->loc, "switch requires integer type");
        return;
    }
    if (t->rank < 3)
        t = type_int(c, 3, false);
    Node *expression = convert(c, n->left, t, n->loc);
    size_t control = scalar_full_expression(l, expression, FULL_VALUE);
    size_t done = new_label(l), fallback = done;
    Type *i = type_int(c, 3, false);
    Cases cases = {0};
    gather_cases(l, &cases, n->right, t);
    Integer128 selected_value;
    bool constant = !c->syntax_only && fixed_control_value(c, expression) &&
                    constant_integer(c, expression, &selected_value);
    size_t selected = 0;
    for (size_t j = 0; j < cases.len; j++) {
        Case item = cases.data[j];
        if (item.otherwise) {
            fallback = item.label;
        } else if (constant) {
            if (wide_compare(item.value, selected_value, t->uns) <= 0 &&
                wide_compare(selected_value, item.high, t->uns) <= 0)
                selected = item.label;
        } else if (!wide_compare(item.value, item.high, t->uns)) {
            size_t match = operation(l, EQ, control, immediate_integer(l, t, item.value), i);
            branch(l, match, item.label, true, n->loc);
        } else {
            size_t skip = new_label(l);
            size_t above = operation(l, GE, control, immediate_integer(l, t, item.value), i);
            branch(l, above, skip, false, n->loc);
            size_t below = operation(l, LE, control, immediate_integer(l, t, item.high), i);
            branch(l, below, item.label, true, n->loc);
            mark(l, skip);
        }
    }
    jump(l, selected ? selected : fallback);
    Cases *prior_cases = l->cases;
    size_t prior_break = l->break_label, prior_switch_id = l->switch_id;
    Lifetime *prior_break_lifetime = l->break_lifetime, *prior_switch = l->switch_lifetime;
    l->cases = &cases;
    l->break_label = done;
    l->switch_id = done;
    l->break_lifetime = l->switch_lifetime = l->lifetime;
    statement(l, n->right);
    mark(l, done);
    l->switch_id = prior_switch_id;
    l->cases = prior_cases;
    l->break_label = prior_break;
    l->break_lifetime = prior_break_lifetime;
    l->switch_lifetime = prior_switch;
}

static void flow_statement(Lower *l, Loc loc, bool effect)
{
    if (l->switch_id)
        put(l, (Inst){.code = I_FLOW_STATEMENT, .label = l->switch_id, .loc = loc, .op = effect});
}

static void flow_condition(Lower *l, Node *n)
{
    bool known;
    if (l->switch_id)
        flow_statement(l, n->loc, !constant_truth(l->context, n->left, &known));
}

static void loop(Lower *l, Node *n)
{
    size_t start = new_label(l), step = new_label(l), done = new_label(l);
    size_t prior_break = l->break_label, prior_continue = l->continue_label;
    Lifetime *prior = l->lifetime, *prior_break_lifetime = l->break_lifetime;
    Lifetime *prior_continue_lifetime = l->continue_lifetime;
    l->break_label = done;
    l->continue_label = step;
    enter_stack_scope(l, n);
    if (n->fourth)
        statement(l, n->fourth);
    l->break_lifetime = l->continue_lifetime = l->lifetime;
    mark(l, start);
    if (n->kind == N_LOOP && n->left) {
        flow_condition(l, n);
        branch_full_expression(l, n->left, done, false, n->loc);
    }
    statement(l, n->right);
    mark(l, step);
    if (n->kind == N_DO) {
        flow_condition(l, n);
        branch_full_expression(l, n->left, start, true, n->loc);
    } else {
        if (l->switch_id && expression_has_effect(l->context, n->third))
            flow_statement(l, n->third->loc, true);
        scalar_full_expression(l, n->third, FULL_EFFECT);
        jump(l, start);
    }
    mark(l, done);
    restore(l, prior, n->loc);
    l->break_label = prior_break;
    l->continue_label = prior_continue;
    l->break_lifetime = prior_break_lifetime;
    l->continue_lifetime = prior_continue_lifetime;
}

static void allocate_variable_object(Lower *l, Symbol *s)
{
    Type *pointer_type = pointer(l->context, s->type);
    size_t stack_slot = slot(l, pointer_type);
    put(l, (Inst){.code = I_STACK_SAVE, .dest = stack_slot});
    size_t object = allocate_variable_temporary(l, s->type, symbol_alignment(s));
    l->function->instructions.data[l->function->instructions.len - 1].label = stack_slot;
    store(l, storage_address(l, s), object, pointer_type);
    if (s->init)
        zero_variable_object(l, object, s->type);
    push_lifetime(l, stack_slot, 0, NULL)->variable_type = true;
    if (l->function->function->has_nonlocal_labels)
        nonlocal_checkpoint(l);
}

static size_t snapshot_result(Lower *l, Node *expr, size_t result)
{
    if (!expr || !result || !address_value(expr->type))
        return result;
    Type *type = expr->type;
    if (type->variable_size) {
        size_t copy = allocate_variable_temporary(l, type, type->align);
        store(l, copy, result, type);
        return copy;
    }
    size_t copy = object_slot(l, type);
    put(l, (Inst){.code = I_MEMCOPY, .a = copy, .b = result, .immediate = type->size});
    return copy;
}

static size_t block_value(Lower *l, Node *block, Node *expr)
{
    Lifetime *prior = l->lifetime;
    enter_stack_scope(l, block);
    for (size_t i = 0; i < block->list.len; i++)
        statement(l, block->list.data[i]);
    size_t result = value(l, expr);
    if (l->lifetime != prior) {
        result = snapshot_result(l, expr, result);
        if (expr && expr->type->variable_size) {
            /* The enclosing full expression releases the returned object and the block's stack. */
            run_cleanups(l, l->lifetime, prior);
            l->lifetime = prior;
            return result;
        }
    }
    restore(l, prior, block->loc);
    return result;
}

static void statement(Lower *l, Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case N_BLOCK: block_value(l, n, NULL); break;
    case N_SEQUENCE:
        for (size_t i = 0; i < n->list.len; i++)
            statement(l, n->list.data[i]);
        break;
    case N_DECL: {
        Symbol *s = n->symbol;
        if (n->list.len || (s->local && (s->init || s->type->variable_size)))
            flow_statement(l, n->loc, true);
        for (size_t i = 0; i < n->list.len; i++)
            scalar_full_expression(l, n->list.data[i], FULL_EFFECT);
        if ((n->flags & DECL_VARIABLY_MODIFIED) && !(s->local && s->type->variable_size)) {
            push_lifetime(l, 0, 0, NULL)->variable_type = true;
            if (l->function->function->has_nonlocal_labels)
                nonlocal_checkpoint(l);
        }
        if (!s->local)
            break;
        if (s->type->variable_size) {
            if (!l->context->syntax_only && s->type->kind == TY_ARRAY &&
                warning_enabled(l->context, WARNING_VLA_LARGE, n->loc)) {
                VariableArraySite site = {.symbol = s, .loc = n->loc,
                                          .instruction = l->function->instructions.len};
                PUSH(l->context, l->variable_array_sites, site);
            }
            allocate_variable_object(l, s);
        } else {
            if (s->init) {
                Lifetime *prior = begin_full_expression(l);
                size_t place = address_symbol(l, s);
                if (aggregate(s->type) || s->type->kind == TY_VECTOR)
                    put(l, (Inst){.code = I_MEMZERO, .a = place, .immediate = s->type->size});
                init_object(l, place, s->type, s->init);
                record_store_value(l, place, s->init);
                restore(l, prior, s->loc);
            }
            update_object_extent(l, s, s->init);
        }
        if (s->cleanup)
            push_lifetime(l, 0, 0, s->cleanup);
        break;
    }
    case N_EXPR:
        if (l->switch_id && expression_has_effect(l->context, n->left))
            flow_statement(l, n->loc, true);
        scalar_full_expression(l, n->left, FULL_EFFECT);
        break;
    case N_RETURN: {
        Lifetime *prior = begin_full_expression(l);
        size_t result = value(l, n->left);
        Lifetime *cleanup = l->lifetime;
        while (cleanup && !cleanup->cleanup)
            cleanup = cleanup->previous;
        if (cleanup) {
            result = snapshot_result(l, n->left, result);
            run_cleanups(l, cleanup, NULL);
        }
        return_value(l, result, n->loc);
        l->lifetime = prior;
        break;
    }
    case N_IF: {
        flow_statement(l, n->left->loc, true);
        size_t otherwise = new_label(l), done = new_label(l);
        branch_full_expression(l, n->left, otherwise, false, n->loc);
        statement(l, n->right);
        if (n->third)
            jump(l, done);
        mark(l, otherwise);
        if (n->third) {
            statement(l, n->third);
            mark(l, done);
        }
        break;
    }
    case N_LOOP: case N_DO: loop(l, n); break;
    case N_SWITCH:
        flow_statement(l, n->loc, true);
        switch_statement(l, n);
        break;
    case N_FALLTHROUGH:
        if (l->switch_id)
            put(l, (Inst){.code = I_FALLTHROUGH, .label = l->switch_id, .loc = n->loc});
        else if (!l->context->syntax_only)
            error(l->context, n->loc, "fallthrough attribute outside of a switch");
        break;
    case N_CASE: case N_DEFAULT:
        if (!l->cases) {
            error(l->context, n->loc, "case/default outside of switch");
            break;
        }
        /* A switch dispatch enters after the boundary. Sequential execution reaches it first. */
        put(l, (Inst){.code = I_CASE_BOUNDARY, .label = l->switch_id, .loc = n->loc});
        for (size_t i = 0; i < l->cases->len; i++) {
            if (l->cases->data[i].node != n)
                continue;
            size_t entry = l->cases->data[i].label;
            if (l->lifetime == l->switch_lifetime) {
                mark(l, entry);
            } else {
                size_t body = new_label(l);
                jump(l, body);
                mark(l, entry);
                move_lifetimes(l, l->switch_lifetime, l->lifetime, n->loc, false);
                mark(l, body);
            }
            break;
        }
        statement(l, n->right);
        break;
    case N_BREAK: case N_CONTINUE: {
        size_t dest = n->kind == N_BREAK ? l->break_label : l->continue_label;
        if (dest) {
            Lifetime *target = n->kind == N_BREAK ? l->break_lifetime : l->continue_lifetime;
            move_lifetimes(l, l->lifetime, target, n->loc, false);
            jump(l, dest);
        } else {
            error(l->context, n->loc, "%s outside of a valid context",
                  n->kind == N_BREAK ? "break" : "continue");
        }
        break;
    }
    case N_GOTO: goto_label(l, n); break;
    case N_LOCAL_LABEL: {
        if (n->symbol->nonlocal_state) {
            NonlocalLabel label = {.declaration = n, .checkpoint = nonlocal_checkpoint(l)};
            PUSH(l->context, l->nonlocal_labels, label);
        }
        break;
    }
    case N_NONLOCAL_GOTO: {
        size_t address = storage_address(l, n->symbol->nonlocal_state);
        Type *nothing = l->function->function->static_chain->type->base;
        runtime_call(l, "__mcc_restore_nonlocal", nothing, &address, 1);
        put(l, (Inst){.code = I_TRAP});
        break;
    }
    case N_LABEL: {
        Label *label = named_label(l, n);
        label->lifetime = l->lifetime;
        put(l, (Inst){.code = I_LABEL, .label = label->number, .symbol = n->symbol});
        statement(l, n->left);
        break;
    }
    case N_INDIRECT_GOTO: {
        size_t target = scalar_full_expression(l, n->left, FULL_VALUE);
        Goto source = {l->function->instructions.len, target, l->lifetime, n->loc};
        PUSH(l->context, l->indirect_gotos, source);
        put(l, (Inst){.code = I_INDIRECT_JUMP, .a = target});
        break;
    }
    default: error(l->context, n->loc, "cannot lower this statement"); break;
    }
}

static void resolve_nonlocal(Lower *l)
{
    /* Save each destination's frame at the point where its VLA scope becomes active. */
    for (size_t i = 0; i < l->nonlocal_labels.len; i++) {
        NonlocalLabel *item = &l->nonlocal_labels.data[i];
        Label *target = named_label(l, item->declaration);
        item->label = target->number;
        while (item->checkpoint < l->checkpoints.len &&
               stack_lifetime(l->checkpoints.data[item->checkpoint].lifetime) != stack_lifetime(target->lifetime))
            item->checkpoint++;
        if (item->checkpoint == l->checkpoints.len) {
            error(l->context, item->declaration->loc, "no active scope for nonlocal label '%s'",
                  item->declaration->symbol->name);
            return;
        }
    }
    Type *result = type_int(l->context, 3, false);
    for (size_t i = 0; i < l->checkpoints.len; i++) {
        Checkpoint checkpoint = l->checkpoints.data[i];
        bool emit = false;
        for (size_t j = 0; j < l->nonlocal_labels.len; j++) {
            NonlocalLabel item = l->nonlocal_labels.data[j];
            if (item.checkpoint != i)
                continue;
            if (!emit) {
                size_t entry = new_label(l);
                l->function->instructions.data[checkpoint.index].label = entry;
                mark(l, entry);
                emit = true;
            }
            size_t address = storage_address(l, item.declaration->symbol->nonlocal_state);
            size_t returned = runtime_call(l, "__mcc_save_nonlocal", result, &address, 1);
            Loc loc = item.declaration->loc;
            if (l->stack_pin) {
                size_t normal = new_label(l);
                branch(l, returned, normal, false, loc);
                size_t stack = slot(l, l->stack_type);
                put(l, (Inst){.code = I_STACK_SAVE, .dest = stack});
                restore_stack(l, stack, checkpoint.lifetime, loc);
                jump(l, item.label);
                mark(l, normal);
            } else {
                branch(l, returned, item.label, true, loc);
            }
        }
        if (emit)
            jump(l, checkpoint.resume);
    }
}

static void indirect_lifetimes(Lower *l)
{
    Type *integer_type = type_int(l->context, 3, false);
    for (size_t i = 0; i < l->indirect_gotos.len; i++) {
        Goto source = l->indirect_gotos.data[i];
        bool dispatch = false;
        for (size_t j = 0; j < l->labels.len; j++) {
            Label target = l->labels.data[j];
            if (!target.symbol->label_address_taken || stack_lifetime(target.lifetime) == stack_lifetime(source.source))
                continue;
            if (!dispatch) {
                size_t entry = new_label(l);
                l->function->instructions.data[source.index] = (Inst){.code = I_JUMP, .label = entry, .op = 1};
                mark(l, entry);
                dispatch = true;
            }
            size_t address = storage_address(l, target.symbol);
            size_t match = operation(l, EQ, source.target, address, integer_type);
            size_t next = new_label(l);
            branch(l, match, next, false, source.loc);
            if (move_lifetimes(l, source.source, target.lifetime, source.loc, true))
                jump(l, target.number);
            else
                put(l, (Inst){.code = I_TRAP});
            mark(l, next);
        }
        if (dispatch)
            put(l, (Inst){.code = I_INDIRECT_JUMP, .a = source.target});
    }
}

static bool branch_can_jump(const Inst *in)
{
    return in->op ? in->immediate != CONDITION_FALSE : in->immediate != CONDITION_TRUE;
}

typedef enum { FOLLOW_CASE, CASE_HAS_EFFECT, VALIDATE_ATTRIBUTE } SwitchWalk;
typedef struct { size_t at, effect; } SwitchPath;
typedef struct {
    Lower *lower;
    size_t *labels, *seen, visit;
    bool *attributes, *effects, *reported;
    VECTOR(SwitchPath) work;
} SwitchFlow;

static bool switch_path(SwitchFlow *flow, size_t from, SwitchWalk walk)
{
    Context *c = flow->lower->context;
    Inst *instructions = flow->lower->function->instructions.data;
    size_t scope = instructions[from].label;
    size_t end = flow->labels[scope];
    unsigned level = c->fallthrough_level ? c->fallthrough_level : 3;
    flow->visit++;
    flow->work.len = 0;
    PUSH(c, flow->work, ((SwitchPath){from + 1, 0}));
    for (size_t i = 0; i < flow->work.len; i++) {
        SwitchPath path = flow->work.data[i];
        for (size_t at = path.at; at > from && at < end; at++) {
            size_t state = 2 * at + (path.effect != 0);
            if (flow->seen[state] == flow->visit)
                break;
            flow->seen[state] = flow->visit;
            Inst *in = &instructions[at];
            if (in->code == I_CASE_BOUNDARY && in->label == scope) {
                if (walk == VALIDATE_ATTRIBUTE)
                    return true;
                if (walk == CASE_HAS_EFFECT && flow->effects[at])
                    return true;
                if (walk == FOLLOW_CASE && path.effect && flow->effects[at] &&
                    !flow->reported[at] && in->loc.fallthrough_comment < level) {
                    warning_at(c, WARNING_IMPLICIT_FALLTHROUGH, instructions[path.effect].loc,
                               "this statement may fall through");
                    note(c, in->loc, "here");
                    flow->reported[at] = true;
                }
                break;
            } else if (in->code == I_FLOW_STATEMENT && in->label == scope) {
                if (walk == VALIDATE_ATTRIBUTE)
                    break;
                if (in->op) {
                    if (walk == CASE_HAS_EFFECT)
                        return true;
                    path.effect = warning_enabled(c, WARNING_IMPLICIT_FALLTHROUGH, in->loc) ? at : 0;
                }
            } else if (in->code == I_FALLTHROUGH && in->label == scope && flow->attributes[at]) {
                path.effect = 0;
            }
            if (in->code == I_JUMP) {
                /* Named gotos are explicit transfers, even when they jump forward. */
                if (!in->op)
                    PUSH(c, flow->work, ((SwitchPath){flow->labels[in->label], path.effect}));
                break;
            }
            if (in->code == I_ZERO && branch_can_jump(in)) {
                PUSH(c, flow->work, ((SwitchPath){flow->labels[in->label], path.effect}));
                if (in->immediate != CONDITION_UNKNOWN)
                    break;
            }
            if (in->code == I_RETURN || in->code == I_TRAP || in->code == I_LONGJMP ||
                in->code == I_INDIRECT_JUMP)
                break;
        }
    }
    return false;
}

static void check_fallthrough(Lower *l)
{
    Context *c = l->context;
    Instructions instructions = l->function->instructions;
    bool has_attributes = false, has_warning = false;
    for (size_t i = 0; i < instructions.len; i++) {
        Inst *in = &instructions.data[i];
        has_attributes |= in->code == I_FALLTHROUGH;
        if (in->code == I_FLOW_STATEMENT && in->op)
            has_warning |= warning_enabled(c, WARNING_IMPLICIT_FALLTHROUGH, in->loc);
    }
    if (!has_attributes && !has_warning)
        return;
    SwitchFlow flow = {
        .lower = l,
        .labels = alloc(c, (l->next_label + 1) * sizeof *flow.labels),
        .seen = alloc(c, 2 * instructions.len * sizeof *flow.seen),
        .attributes = alloc(c, instructions.len * sizeof *flow.attributes),
        .effects = alloc(c, instructions.len * sizeof *flow.effects),
        .reported = alloc(c, instructions.len * sizeof *flow.reported),
    };
    for (size_t i = 0; i < instructions.len; i++)
        if (instructions.data[i].code == I_LABEL)
            flow.labels[instructions.data[i].label] = i;
    /* Each case can reuse the effect analysis of later cases in the same switch. */
    for (size_t i = instructions.len; i-- > 0;) {
        Inst *in = &instructions.data[i];
        if (in->code == I_FALLTHROUGH) {
            flow.attributes[i] = switch_path(&flow, i, VALIDATE_ATTRIBUTE);
            if (!flow.attributes[i])
                warning(c, in->loc, "'fallthrough' attribute not preceding a case label or default label");
        }
        if (has_warning && in->code == I_CASE_BOUNDARY)
            flow.effects[i] = switch_path(&flow, i, CASE_HAS_EFFECT);
    }
    if (has_warning)
        for (size_t i = 0; i < instructions.len; i++)
            if (instructions.data[i].code == I_CASE_BOUNDARY)
                switch_path(&flow, i, FOLLOW_CASE);
}

enum { FLOW_ACCESS = 1, FLOW_ESCAPE = 2, FLOW_EXTERNAL = 4 };

typedef struct {
    size_t *labels;
    bool *visited;
    Values targets, work;
} InstructionWalk;

static bool *reachable_instructions(Lower *l, Symbol *parameter, const unsigned char *effects, InstructionWalk *walk)
{
    Context *c = l->context;
    Instructions instructions = l->function->instructions;
    /* An access path can still be untouched, escaped, or possibly accessed. */
    size_t states = effects ? 3 : parameter ? 2 : 1;
    InstructionWalk single = {0};
    if (!walk)
        walk = &single;
    size_t bytes = instructions.len * states * sizeof *walk->visited;
    if (!walk->visited) {
        walk->visited = alloc(c, bytes);
        walk->labels = alloc(c, (l->next_label + 1) * sizeof *walk->labels);
        for (size_t i = 0; i < instructions.len; i++) {
            Inst *in = &instructions.data[i];
            if (in->code == I_LABEL) {
                walk->labels[in->label] = i;
                if (in->symbol && in->symbol->label_address_taken)
                    PUSH(c, walk->targets, i);
            }
        }
    } else {
        memset(walk->visited, 0, bytes);
    }
    if (!instructions.len)
        return walk->visited;
    walk->work.len = 0;
    PUSH(c, walk->work, 0);
    for (size_t i = 0; i < walk->work.len; i++) {
        size_t start = walk->work.data[i];
        size_t changed = start - start % instructions.len;
        for (size_t at = start - changed; at < instructions.len && !walk->visited[at + changed]; at++) {
            walk->visited[at + changed] = true;
            Inst *in = &instructions.data[at];
            if (parameter && in->code == I_NONNULL_WRITE && in->symbol == parameter)
                changed = instructions.len;
            if (effects && changed != 2 * instructions.len) {
                unsigned effect = effects[at];
                if ((effect & FLOW_ACCESS) || (changed && (effect & FLOW_EXTERNAL)))
                    changed = 2 * instructions.len;
                else if (effect & FLOW_ESCAPE)
                    changed = instructions.len;
            }
            if (in->code == I_RETURN || in->code == I_TRAP || in->code == I_LONGJMP)
                break;
            if (in->code == I_JUMP) {
                PUSH(c, walk->work, walk->labels[in->label] + changed);
                break;
            }
            if (in->code == I_ZERO) {
                if (branch_can_jump(in)) {
                    PUSH(c, walk->work, walk->labels[in->label] + changed);
                    if (in->immediate != CONDITION_UNKNOWN)
                        break;
                }
            }
            if (in->code == I_INDIRECT_JUMP) {
                for (size_t j = 0; j < walk->targets.len; j++)
                    PUSH(c, walk->work, walk->targets.data[j] + changed);
                break;
            }
        }
    }
    return walk->visited;
}

static void check_nonnull_comparisons(Lower *l)
{
    Context *c = l->context;
    Instructions instructions = l->function->instructions;
    Symbols params = l->function->function->params;
    InstructionWalk walk = {0};
    for (size_t i = 0; i < params.len; i++) {
        Symbol *param = params.data[i];
        const bool *reachable = NULL;
        for (size_t at = 0; at < instructions.len; at++) {
            Inst *in = &instructions.data[at];
            if (in->code != I_NONNULL_COMPARE || in->symbol != param)
                continue;
            if (!reachable)
                reachable = reachable_instructions(l, param, NULL, &walk);
            if (reachable[at] && !reachable[at + instructions.len])
                warning_at(c, WARNING_NONNULL_COMPARE, in->loc, "'nonnull' argument '%s' compared to NULL",
                           param->name);
        }
    }
}

typedef struct {
    uint64_t offset;
    bool points_to_object, known_offset;
} AccessAddress;

typedef struct {
    const Inst **definitions;
    AccessAddress *values, *aliases;
    unsigned char *effects;
    Symbol *object;
    InstructionWalk walk;
} AccessInitialization;

static bool merge_access_address(AccessAddress *into, AccessAddress source)
{
    if (!source.points_to_object)
        return false;
    if (!into->points_to_object) {
        *into = source;
        return true;
    }
    if (into->known_offset && (!source.known_offset || into->offset != source.offset)) {
        into->known_offset = false;
        return true;
    }
    return false;
}

static const Inst **unique_slot_definitions(Lower *l, const bool *reachable)
{
    static const Inst multiple = {.code = I_NOP};
    Instructions instructions = l->function->instructions;
    const Inst **definitions = alloc(l->context, (l->function->slots.len + 1) * sizeof *definitions);
    for (size_t i = 0; i < instructions.len; i++) {
        const Inst *in = &instructions.data[i];
        if (reachable[i] && in->dest)
            definitions[in->dest] = definitions[in->dest] ? &multiple : in;
    }
    return definitions;
}

static Symbol *direct_local_address(const Inst **definitions, size_t slot, const Symbol *function)
{
    const Inst *definition = definitions[slot];
    Symbol *s = definition && definition->code == I_ADDRESS ? definition->symbol : NULL;
    return s && s->local && s->owner == function ? s : NULL;
}

/* A returns-twice continuation can be reached again after an intervening call
   or a loop. Report a scalar only when one path reads its old value and a
   path can modify it before such a return. */
static void check_clobbered(Lower *l)
{
    Context *c = l->context;
    Symbol *function = l->function->function;
    if (!function->has_builtin_setjmp && !function->has_returns_twice)
        return;
    Instructions instructions = l->function->instructions;
    InstructionWalk walk = {0};
    const bool *reachable = reachable_instructions(l, NULL, NULL, &walk);
    const Inst **definitions = unique_slot_definitions(l, reachable);
    bool *visited = alloc(c, 2 * instructions.len * sizeof *visited);
    Values pending = {0};
    for (size_t v = 0; v < function->locals.len; v++) {
        Symbol *s = function->locals.data[v];
        if (!s->name || !s->loc.file || !warning_enabled(c, WARNING_CLOBBERED, s->loc) ||
            (s->type->qualifiers & (S_VOLATILE | S_ATOMIC)) || s->register_binding ||
            (!integer_type(s->type) && s->type->kind != TY_PTR && s->type->kind != TY_FLOAT))
            continue;
        /* Compiler-created temporaries use descriptive names containing spaces. */
        if (strchr(s->name, ' '))
            continue;
        bool report = false;
        for (size_t checkpoint = 0; checkpoint < instructions.len && !report; checkpoint++) {
            const Inst *jump = &instructions.data[checkpoint];
            bool twice = jump->code == I_SETJMP || (jump->code == I_CALL && function_returns_twice(c, jump->symbol));
            if (!twice || !reachable[checkpoint] || checkpoint + 1 >= instructions.len)
                continue;
            memset(visited, 0, 2 * instructions.len * sizeof *visited);
            pending.len = 0;
            PUSH(c, pending, checkpoint + 1);
            bool read_before_write = false, return_after_write = false;
            for (size_t w = 0; w < pending.len; w++) {
                size_t encoded = pending.data[w], at = encoded % instructions.len;
                bool written = encoded >= instructions.len;
                if (visited[encoded])
                    continue;
                visited[encoded] = true;
                const Inst *in = &instructions.data[at];
                if ((in->code == I_LOAD || in->code == I_BITLOAD) && !written &&
                    direct_local_address(definitions, in->a, function) == s)
                    read_before_write = true;
                if ((in->code == I_STORE || in->code == I_BITSTORE) &&
                    direct_local_address(definitions, in->a, function) == s)
                    written = true;
                if (written && (at == checkpoint || in->code == I_CALL || in->code == I_LONGJMP))
                    return_after_write = true;
                if (read_before_write && return_after_write) {
                    report = true;
                    break;
                }
                if (in->code == I_RETURN || in->code == I_TRAP || in->code == I_LONGJMP)
                    continue;
                size_t state = written ? instructions.len : 0;
                if (in->code == I_JUMP || (in->code == I_ZERO && branch_can_jump(in))) {
                    PUSH(c, pending, walk.labels[in->label] + state);
                    if (in->code == I_JUMP || in->immediate != CONDITION_UNKNOWN)
                        continue;
                }
                if (in->code == I_INDIRECT_JUMP) {
                    for (size_t j = 0; j < walk.targets.len; j++)
                        PUSH(c, pending, walk.targets.data[j] + state);
                    continue;
                }
                if (at + 1 < instructions.len)
                    PUSH(c, pending, at + 1 + state);
            }
        }
        if (report)
            warning_at(c, WARNING_CLOBBERED, s->loc,
                       "variable '%s' might be clobbered by 'longjmp' or 'vfork'", s->name);
    }
}

static bool tracked_pointer_alias(Symbol *s)
{
    return s && s->type->kind == TY_PTR && !s->pointer_size_untracked && !s->captured_in_nested_function &&
           !(s->type->qualifiers & (S_VOLATILE | S_ATOMIC));
}

static bool access_constant(const AccessInitialization *flow, size_t slot, uint64_t *number)
{
    const Inst *in = flow->definitions[slot];
    if (!in || !in->type || !integer_type(in->type) || !in->type->size || in->type->size > sizeof *number)
        return false;
    uint64_t value, right;
    switch (in->code) {
    case I_IMMEDIATE:
        value = in->immediate;
        break;
    case I_CAST:
        if (!access_constant(flow, in->a, &value))
            return false;
        break;
    case I_BINARY:
        if (!access_constant(flow, in->a, &value) || !access_constant(flow, in->b, &right))
            return false;
        switch (in->op) {
        case '+': value += right; break;
        case '-': value -= right; break;
        case '*': value *= right; break;
        default: return false;
        }
        break;
    default:
        return false;
    }
    unsigned bits = in->type->size * 8;
    if (in->type->kind == TY_BOOL) {
        value = value != 0;
    } else if (bits < 64) {
        uint64_t mask = (UINT64_C(1) << bits) - 1;
        value &= mask;
        if (!in->type->uns && (value >> (bits - 1)))
            value |= ~mask;
    }
    *number = value;
    return true;
}

static AccessAddress derived_access_address(const AccessInitialization *flow, const Inst *in, const Symbol *function)
{
    AccessAddress here = {.points_to_object = true, .known_offset = true};
    switch (in->code) {
    case I_ADDRESS:
        if (in->symbol && in->symbol == flow->object && !in->symbol->type->variable_size)
            return here;
        break;
    case I_LOAD: {
        Symbol *s = direct_local_address(flow->definitions, in->a, function);
        if (s && s == flow->object && s->type->variable_size)
            return here;
        if (tracked_pointer_alias(s))
            return flow->aliases[s->slot];
        break;
    }
    case I_COPY:
        return flow->values[in->a];
    case I_UNARY: {
        AccessAddress source = flow->values[in->a];
        source.known_offset = false;
        return source;
    }
    case I_CAST: {
        AccessAddress source = flow->values[in->a];
        if (in->type->size < in->source_type->size)
            source.known_offset = false;
        return source;
    }
    case I_BINARY: {
        AccessAddress left = flow->values[in->a], right = flow->values[in->b];
        if (!left.points_to_object && !right.points_to_object)
            break;
        uint64_t amount;
        AccessAddress source = left.points_to_object ? left : right;
        if ((in->op == '+' || (in->op == '-' && left.points_to_object)) &&
            !(left.points_to_object && right.points_to_object) &&
            access_constant(flow, left.points_to_object ? in->b : in->a, &amount)) {
            source.offset += in->op == '+' ? amount : 0 - amount;
        } else {
            source.known_offset = false;
        }
        return source;
    }
    default:
        break;
    }
    return (AccessAddress){0};
}

static void find_access_addresses(Lower *l, AccessInitialization *flow, const bool *reachable)
{
    Instructions instructions = l->function->instructions;
    Symbol *function = l->function->function;
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < instructions.len; i++) {
            if (!reachable[i])
                continue;
            const Inst *in = &instructions.data[i];
            if (in->dest)
                changed |= merge_access_address(&flow->values[in->dest], derived_access_address(flow, in, function));
            if (in->code == I_STORE && flow->values[in->b].points_to_object) {
                Symbol *alias = direct_local_address(flow->definitions, in->a, function);
                if (tracked_pointer_alias(alias))
                    changed |= merge_access_address(&flow->aliases[alias->slot], flow->values[in->b]);
            }
        }
    } while (changed);
}

static bool access_overlaps(AccessAddress address, uint64_t size, uint64_t start, uint64_t length)
{
    if (!address.points_to_object || !size)
        return false;
    if (!address.known_offset)
        return true;
    return address.offset <= start ? start - address.offset < size : address.offset - start < length;
}

static unsigned char access_effect(const AccessInitialization *flow, const Inst *in, const Symbol *function,
                                   uint64_t start, uint64_t length)
{
    AccessAddress at = flow->values[in->a];
    switch (in->code) {
    case I_STORE: case I_BITSTORE: {
        if (access_overlaps(at, in->type->size, start, length))
            return FLOW_ACCESS;
        Symbol *target = direct_local_address(flow->definitions, in->a, function);
        if (in->code == I_STORE && flow->values[in->b].points_to_object && !tracked_pointer_alias(target))
            return FLOW_ESCAPE;
        return target || at.points_to_object ? 0 : FLOW_EXTERNAL;
    }
    case I_REGISTER_WRITE:
        return at.points_to_object ? FLOW_ESCAPE : 0;
    case I_MEMZERO: case I_MEMCOPY:
        return access_overlaps(at, in->immediate, start, length) ? FLOW_ACCESS :
               at.points_to_object || direct_local_address(flow->definitions, in->a, function) ? 0 : FLOW_EXTERNAL;
    case I_VA_START: case I_VA_ARG:
        return at.points_to_object ? FLOW_ACCESS : 0;
    case I_CALL:
        /* A prior call may have written the storage or already used it as an input. */
        for (size_t i = 0; i < in->args.len; i++)
            if (flow->values[in->args.data[i]].points_to_object)
                return FLOW_ACCESS;
        return FLOW_EXTERNAL;
    default:
        return 0;
    }
}

typedef struct {
    IntegerRange range;
    bool reached, pending;
} RangePoint;

typedef struct { size_t from, next; } Predecessor;

typedef struct {
    Lower *lower;
    const InstructionWalk *walk;
    const bool *reachable;
    const Inst **definitions;
    size_t *predecessors, *pointer_visits, pointer_search;
    VECTOR(Predecessor) edges;
    Values pointer_work;
    RangePoint *points;
    size_t argument;
    const Symbol *symbol;
    unsigned layout_depth;
    Values work;
} IntegerFlow;

static bool comparison_symbol(Node *n, const Symbol *symbol)
{
    while (n->kind == N_CAST) {
        Type *from = n->left->type, *to = n->type;
        /* Only peel casts that preserve the order of the entire source type. */
        if (!integer_type(from) || !integer_type(to) || to->kind == TY_BOOL ||
            (from->kind != TY_BOOL && (to->size < from->size ||
             (from->uns != to->uns && (!from->uns || to->size == from->size)))))
            return false;
        n = n->left;
    }
    return n->kind == N_SYMBOL && n->symbol == symbol;
}

static int compare_range_value(Integer128 a, bool unsigned_a, Integer128 b, bool unsigned_b)
{
    bool negative_a = !unsigned_a && (a.hi >> 63), negative_b = !unsigned_b && (b.hi >> 63);
    if (negative_a != negative_b)
        return negative_a ? -1 : 1;
    return wide_compare(a, b, !negative_a);
}

static bool restrict_comparison(IntegerRange *range, bool uns, int op, Integer128 value, bool value_uns)
{
    Integer128 one = {1, 0};
    int low = compare_range_value(range->lower, uns, value, value_uns);
    int high = compare_range_value(range->upper, uns, value, value_uns);
    switch (op) {
    case EQ:
        if (low > 0 || high < 0)
            return false;
        *range = (IntegerRange){value, value};
        return true;
    case NE:
        if (!low && !high)
            return false;
        if (!low)
            range->lower = wide_binary(value, one, WIDE_ADD, uns);
        if (!high)
            range->upper = wide_subtract(value, one);
        return true;
    case '<': case LE:
        if (low > 0 || (low == 0 && op == '<'))
            return false;
        if (high >= 0)
            range->upper = op == '<' ? wide_subtract(value, one) : value;
        return true;
    case '>': case GE:
        if (high < 0 || (high == 0 && op == '>'))
            return false;
        if (low <= 0)
            range->lower = op == '>' ? wide_binary(value, one, WIDE_ADD, uns) : value;
        return true;
    default:
        return true;
    }
}

static bool restrict_integer_condition(Context *c, Node *expr, const Symbol *symbol, bool truth, IntegerRange *range)
{
    bool uns = symbol->type->uns;
    if (expr->kind == N_UNARY && expr->op == '!')
        return restrict_integer_condition(c, expr->left, symbol, !truth, range);
    if (comparison_symbol(expr, symbol))
        return restrict_comparison(range, uns, truth ? NE : EQ, (Integer128){0, 0}, true);
    if (expr->kind != N_BINARY)
        return true;
    if (expr->op == AND || expr->op == OR)
        return truth != (expr->op == AND) ||
               (restrict_integer_condition(c, expr->left, symbol, truth, range) &&
                restrict_integer_condition(c, expr->right, symbol, truth, range));
    int op = expr->op;
    if (op != EQ && op != NE && op != '<' && op != '>' && op != LE && op != GE)
        return true;
    Node *constant = expr->right;
    if (!comparison_symbol(expr->left, symbol)) {
        if (!comparison_symbol(expr->right, symbol))
            return true;
        constant = expr->left;
        op = op == '<' ? '>' : op == '>' ? '<' : op == LE ? GE : op == GE ? LE : op;
    }
    Integer128 value;
    if (!constant_integer(c, constant, &value))
        return true;
    if (!truth)
        op = op == EQ ? NE : op == NE ? EQ : op == '<' ? GE : op == '>' ? LE : op == LE ? '>' : '<';
    return restrict_comparison(range, uns, op, value, constant->type->uns);
}

static void merge_integer_edge(IntegerFlow *flow, size_t from, size_t to, IntegerRange range)
{
    if (to >= flow->lower->function->instructions.len || !flow->reachable[to])
        return;
    /* Back edges can run a new iteration with different values. */
    if (to <= from)
        range = integer_type_range(flow->symbol->type);
    RangePoint *point = &flow->points[to];
    bool uns = flow->symbol->type->uns;
    bool lower = !point->reached || wide_compare(range.lower, point->range.lower, uns) < 0;
    bool upper = !point->reached || wide_compare(range.upper, point->range.upper, uns) > 0;
    if (lower)
        point->range.lower = range.lower;
    if (upper)
        point->range.upper = range.upper;
    point->reached = true;
    if ((lower || upper) && !point->pending) {
        point->pending = true;
        PUSH(flow->lower->context, flow->work, to);
    }
}

static void bound_integer_symbol(IntegerFlow *flow, const Symbol *symbol)
{
    Lower *l = flow->lower;
    Context *c = l->context;
    Instructions instructions = l->function->instructions;
    size_t bytes = instructions.len * sizeof *flow->points;
    if (!flow->points) {
        flow->points = alloc(c, bytes);
        if (!flow->definitions)
            flow->definitions = unique_slot_definitions(l, flow->reachable);
    } else {
        memset(flow->points, 0, bytes);
    }
    flow->symbol = symbol;
    IntegerRange full = integer_type_range(symbol->type);
    merge_integer_edge(flow, 0, 0, full);
    while (flow->work.len) {
        size_t at = flow->work.data[--flow->work.len];
        RangePoint *point = &flow->points[at];
        point->pending = false;
        IntegerRange range = point->range;
        const Inst *in = &instructions.data[at];
        if (in->code == I_RETURN || in->code == I_TRAP || in->code == I_LONGJMP)
            continue;
        if (in->code == I_STORE && direct_local_address(flow->definitions, in->a, l->function->function) == symbol) {
            Node *value = in->stored_value;
            range = value ? convert_integer_range(integer_expression_range(c, value, NULL, NULL),
                                                  value->type, symbol->type) : full;
        }
        if (in->code == I_JUMP) {
            merge_integer_edge(flow, at, flow->walk->labels[in->label], range);
            continue;
        }
        if (in->code == I_ZERO) {
            Node *condition = in->branch_condition;
            if (condition && expression_has_effect(c, condition))
                condition = NULL;
            bool true_edge = in->op != 0;
            if (branch_can_jump(in)) {
                IntegerRange taken = range;
                if (!condition || restrict_integer_condition(c, condition, symbol, true_edge, &taken))
                    merge_integer_edge(flow, at, flow->walk->labels[in->label], taken);
                if (in->immediate != CONDITION_UNKNOWN)
                    continue;
            }
            if (condition && !restrict_integer_condition(c, condition, symbol, !true_edge, &range))
                continue;
        }
        if (in->code == I_INDIRECT_JUMP) {
            for (size_t i = 0; i < flow->walk->targets.len; i++)
                merge_integer_edge(flow, at, flow->walk->targets.data[i], full);
            continue;
        }
        merge_integer_edge(flow, at, at + 1, range);
    }
}

static IntegerRange access_integer_symbol(void *data, const Symbol *symbol)
{
    IntegerFlow *flow = data;
    const Symbol *function = flow->lower->function->function;
    if (!symbol->local || symbol->owner != function || symbol->address_taken || symbol->captured_in_nested_function ||
        symbol->register_binding || (symbol->type->qualifiers & (S_VOLATILE | S_ATOMIC)) ||
        function->has_nonlocal_labels || function->has_builtin_setjmp || function->has_returns_twice)
        return integer_type_range(symbol->type);
    if (flow->symbol != symbol)
        bound_integer_symbol(flow, symbol);
    RangePoint *point = &flow->points[flow->argument];
    return point->reached ? point->range : integer_type_range(symbol->type);
}

static void add_predecessor(IntegerFlow *flow, size_t from, size_t to)
{
    Lower *l = flow->lower;
    if (to >= l->function->instructions.len || !flow->reachable[to])
        return;
    PUSH(l->context, flow->edges, ((Predecessor){from, flow->predecessors[to]}));
    flow->predecessors[to] = flow->edges.len;
}

static void find_predecessors(IntegerFlow *flow)
{
    Lower *l = flow->lower;
    Context *c = l->context;
    Instructions instructions = l->function->instructions;
    if (!flow->definitions)
        flow->definitions = unique_slot_definitions(l, flow->reachable);
    flow->predecessors = alloc(c, instructions.len * sizeof *flow->predecessors);
    flow->pointer_visits = alloc(c, instructions.len * sizeof *flow->pointer_visits);
    for (size_t i = 0; i < instructions.len; i++) {
        const Inst *in = &instructions.data[i];
        if (!flow->reachable[i] || in->code == I_RETURN || in->code == I_TRAP || in->code == I_LONGJMP)
            continue;
        if (in->code == I_JUMP || (in->code == I_ZERO && branch_can_jump(in))) {
            add_predecessor(flow, i, flow->walk->labels[in->label]);
            if (in->code == I_JUMP || in->immediate != CONDITION_UNKNOWN)
                continue;
        }
        if (in->code == I_INDIRECT_JUMP) {
            for (size_t j = 0; j < flow->walk->targets.len; j++)
                add_predecessor(flow, i, flow->walk->targets.data[j]);
            continue;
        }
        add_predecessor(flow, i, i + 1);
    }
}

static bool access_pointer_stores(void *data, const Symbol *symbol, size_t point, AccessPointerStores *stores)
{
    IntegerFlow *flow = data;
    Lower *l = flow->lower;
    Context *c = l->context;
    const Symbol *function = l->function->function;
    if (symbol->captured_in_nested_function ||
        function->has_nonlocal_labels || function->has_builtin_setjmp || function->has_returns_twice || !flow->reachable[point])
        return false;
    if (!flow->predecessors)
        find_predecessors(flow);
    Instructions instructions = l->function->instructions;
    size_t visit = ++flow->pointer_search;
    flow->pointer_work.len = 0;
    PUSH(c, flow->pointer_work, point);
    /* A store kills all earlier values on that path. An entry path with no
       store, or an instruction with unknown effects, leaves the bound unknown. */
    while (flow->pointer_work.len) {
        size_t at = flow->pointer_work.data[--flow->pointer_work.len];
        if (!at)
            return false;
        for (size_t link = flow->predecessors[at]; link; link = flow->edges.data[link - 1].next) {
            size_t from = flow->edges.data[link - 1].from;
            const Inst *in = &instructions.data[from];
            if (flow->pointer_visits[from] == visit)
                continue;
            flow->pointer_visits[from] = visit;
            if (symbol->address_taken) {
                /* An escaped address may be written through an alias. A store
                   to a different known local cannot affect this pointer. */
                if (in->code == I_CALL)
                    return false;
                if (in->code == I_STORE || in->code == I_BITSTORE || in->code == I_MEMCOPY ||
                    in->code == I_MEMZERO || in->code == I_VA_START || in->code == I_VA_ARG) {
                    Symbol *target = direct_local_address(flow->definitions, in->a, function);
                    if (!target || (target == symbol && in->code != I_STORE))
                        return false;
                }
            }
            if (in->code == I_STORE && direct_local_address(flow->definitions, in->a, function) == symbol) {
                if (!in->stored_value)
                    return false;
                PUSH(c, *stores, ((AccessPointerStore){in->stored_value, from}));
            } else {
                PUSH(c, flow->pointer_work, from);
            }
        }
    }
    return stores->len != 0;
}

static IntegerRange access_offset_range(void *data, Node *value, size_t point)
{
    IntegerFlow *flow = data;
    Context *c = flow->lower->context;
    size_t argument = flow->argument;
    flow->argument = point;
    IntegerRange range = integer_expression_range(c, value, access_integer_symbol, flow);
    flow->argument = argument;
    return range;
}

static bool access_layout_range(void *, const Symbol *, size_t, IntegerRange *);

static IntegerRange access_layout_symbol(void *data, const Symbol *symbol)
{
    IntegerFlow *flow = data;
    IntegerRange result;
    if (symbol->array_layout_size && flow->layout_depth < 32) {
        flow->layout_depth++;
        bool found = access_layout_range(data, symbol, flow->argument, &result);
        flow->layout_depth--;
        if (found)
            return result;
    }
    return access_integer_symbol(data, symbol);
}

static bool access_layout_range(void *data, const Symbol *symbol, size_t point, IntegerRange *result)
{
    IntegerFlow *flow = data;
    Context *c = flow->lower->context;
    AccessPointerStore first;
    AccessPointerStores stores = {.data = &first, .cap = 1};
    if (!access_pointer_stores(data, symbol, point, &stores))
        return false;
    size_t argument = flow->argument;
    bool found = false;
    for (size_t i = 0; i < stores.len; i++) {
        AccessPointerStore store = stores.data[i];
        if (expression_has_effect(c, store.value)) {
            flow->argument = argument;
            return false;
        }
        flow->argument = store.point;
        IntegerRange range = integer_expression_range(c, store.value, access_layout_symbol, flow);
        range = convert_integer_range(range, store.value->type, symbol->type);
        if (!found || wide_compare(range.lower, result->lower, true) < 0)
            result->lower = range.lower;
        if (!found || wide_compare(range.upper, result->upper, true) > 0)
            result->upper = range.upper;
        found = true;
    }
    flow->argument = argument;
    return found;
}

static IntegerRange access_allocation_range(void *data, const Node *allocation, unsigned index, Node *value)
{
    IntegerFlow *flow = data;
    Lower *l = flow->lower;
    Context *c = l->context;
    if (expression_has_effect(c, value))
        return integer_expression_range(c, value, NULL, NULL);
    IntegerRange range = {0};
    bool found = false;
    size_t argument = flow->argument;
    /* A shared expression can be lowered at more than one point. Include
       each reachable evaluation of the argument in the bound. */
    for (size_t i = 0; i < l->allocation_sites.len; i++) {
        const AllocationSite *site = &l->allocation_sites.data[i];
        if (site->expression != allocation || !flow->reachable[site->instruction])
            continue;
        flow->argument = site->arguments[index];
        IntegerRange actual = integer_expression_range(c, value, access_integer_symbol, flow);
        if (!found || wide_compare(actual.lower, range.lower, value->type->uns) < 0)
            range.lower = actual.lower;
        if (!found || wide_compare(actual.upper, range.upper, value->type->uns) > 0)
            range.upper = actual.upper;
        found = true;
    }
    flow->argument = argument;
    return found ? range : integer_expression_range(c, value, NULL, NULL);
}

static IntegerRange bounded_alloca_range(IntegerFlow *flow, const Node *expression)
{
    Context *c = flow->lower->context;
    IntegerRange range = access_allocation_range(flow, expression, 0, expression->left);
    uint64_t known, maximum;
    if (expression->fourth && !constant_int(c, expression->left, &known) &&
        constant_int(c, expression->fourth, &maximum)) {
        Integer128 bound = {maximum, 0};
        if (wide_compare(range.upper, bound, true) > 0)
            range.upper = bound;
        if (wide_compare(range.lower, range.upper, true) > 0)
            range.lower = (Integer128){0, 0};
    }
    return range;
}

static uint64_t access_read_size(const Inst *in, AccessClause clause, IntegerRange counts)
{
    Type *element = in->source_type->members.data[clause.reference - 1].type->base;
    uint64_t width = element->kind == TY_VOID || !element->complete ? 1 : element->size;
    Integer128 elements = counts.upper;
    if (clause.size && !in->access_call->list.data[clause.size - 1]->type->uns && counts.lower.hi >> 63)
        return UINT64_MAX;
    if (!wide_nonzero(elements))
        return 0;
    if (element->variable_size || elements.hi || (width && elements.lo > UINT64_MAX / width))
        return UINT64_MAX;
    return elements.lo * width;
}

static void check_access_initialization(Lower *l, AccessInitialization *flow, const bool *reachable,
                                        const Inst *in, size_t index, AccessClause clause, IntegerRange counts)
{
    Context *c = l->context;
    Symbol *function = l->function->function;
    Instructions instructions = l->function->instructions;
    if ((clause.mode != ACCESS_READ && clause.mode != ACCESS_READ_WRITE) ||
        clause.reference > in->args.len || clause.size > in->args.len)
        return;
    WarningKind warning = clause.mode == ACCESS_READ ? WARNING_UNINITIALIZED : WARNING_MAYBE_UNINITIALIZED;
    if (!warning_enabled(c, warning, in->loc))
        return;
    uint64_t length = access_read_size(in, clause, counts);
    Symbol *object = length ? access_local_object(c, function, in->access_call->list.data[clause.reference - 1]) : NULL;
    if (!object || object->captured_in_nested_function)
        return;
    bool parameter = false;
    for (size_t j = 0; j < function->params.len; j++)
        parameter |= function->params.data[j] == object;
    if (parameter)
        return;
    size_t slots = l->function->slots.len + 1;
    if (!flow->definitions) {
        flow->definitions = unique_slot_definitions(l, reachable);
        flow->values = alloc(c, slots * sizeof *flow->values);
        flow->aliases = alloc(c, slots * sizeof *flow->aliases);
        flow->effects = alloc(c, instructions.len * sizeof *flow->effects);
    }
    if (flow->object != object) {
        flow->object = object;
        memset(flow->values, 0, slots * sizeof *flow->values);
        memset(flow->aliases, 0, slots * sizeof *flow->aliases);
        find_access_addresses(l, flow, reachable);
    }
    AccessAddress address = flow->values[in->args.data[clause.reference - 1]];
    if (!address.points_to_object)
        return;
    uint64_t start = address.known_offset ? address.offset : 0;
    if (address.known_offset && (start > PTRDIFF_MAX || (!object->type->variable_size && start >= object->type->size)))
        return;
    if (!address.known_offset)
        length = UINT64_MAX;
    for (size_t j = 0; j < instructions.len; j++)
        flow->effects[j] = reachable[j] ? access_effect(flow, &instructions.data[j], function, start, length) : 0;
    const bool *paths = reachable_instructions(l, NULL, flow->effects, &flow->walk);
    if (!paths[index + 2 * instructions.len]) {
        if (warning == WARNING_UNINITIALIZED)
            warning_at(c, warning, in->loc, "'%s' is used uninitialized", object->name);
        else
            warning_at(c, warning, in->loc, "'%s' may be used uninitialized", object->name);
        if (in->symbol)
            note(c, clause.loc, format(c, "in a call to '%s' declared with attribute 'access'", in->symbol->name));
        note(c, object->loc, format(c, "'%s' declared here", object->name));
    }
}

static void call_diagnostics(Context *c, const Inst *call)
{
    const Symbol *function = call->symbol;
    const char *error_message = NULL, *warning_message = NULL;
    for (size_t i = 0; i < function->call_diagnostics.len; i++) {
        CallDiagnostic diagnostic = function->call_diagnostics.data[i];
        if (diagnostic.kind == ATTR_ERROR)
            error_message = diagnostic.message;
        else
            warning_message = diagnostic.message;
    }
    if (error_message)
        error(c, call->loc, "call to '%s' declared with attribute 'error': %s", function->name, error_message);
    if (warning_message)
        warning_at(c, WARNING_ATTRIBUTE_WARNING, call->loc, "call to '%s' declared with attribute 'warning': %s",
                   function->name, warning_message);
}

/* Detect cycles through an allocation site, including computed gotos. */
static bool allocation_in_loop(IntegerFlow *flow, size_t point, size_t restore_slot)
{
    if (!flow->predecessors)
        find_predecessors(flow);
    Context *c = flow->lower->context;
    size_t visit = ++flow->pointer_search;
    flow->pointer_work.len = 0;
    PUSH(c, flow->pointer_work, point);
    while (flow->pointer_work.len) {
        size_t at = flow->pointer_work.data[--flow->pointer_work.len];
        for (size_t link = flow->predecessors[at]; link; link = flow->edges.data[link - 1].next) {
            size_t from = flow->edges.data[link - 1].from;
            if (from == point)
                return true;
            const Inst *in = &flow->lower->function->instructions.data[from];
            if (restore_slot && in->code == I_STACK_RESTORE && in->a == restore_slot)
                continue;
            if (flow->pointer_visits[from] != visit) {
                flow->pointer_visits[from] = visit;
                PUSH(c, flow->pointer_work, from);
            }
        }
    }
    return false;
}

/* Sum all reachable dynamic sites, conservatively including both sides of
   branches. Cyclic allocations may accumulate without a finite bound. */
static void analyze_stack_usage(Lower *l)
{
    FunctionIR *ir = l->function;
    InstructionWalk walk = {0};
    const bool *reachable = reachable_instructions(l, NULL, NULL, &walk);
    IntegerFlow sizes = {.lower = l, .walk = &walk, .reachable = reachable};
    for (size_t i = 0; i < ir->instructions.len; i++) {
        const Inst *in = &ir->instructions.data[i];
        if (in->code != I_STACK_ALLOC || (!reachable[i] && !ir->function->has_nonlocal_labels))
            continue;
        ir->has_dynamic_stack = true;
        if (ir->function->has_nonlocal_labels || ir->function->has_builtin_setjmp || ir->function->has_returns_twice ||
            allocation_in_loop(&sizes, i, in->label)) {
            ir->dynamic_stack_unbounded = true;
            continue;
        }
        IntegerRange range;
        bool known = false;
        if (in->access_call) {
            range = bounded_alloca_range(&sizes, in->access_call);
            known = true;
        } else if (in->type && in->type->runtime_size) {
            known = access_layout_range(&sizes, in->type->runtime_size, i, &range);
        }
        if (!known || range.upper.hi || range.upper.lo > PTRDIFF_MAX) {
            ir->dynamic_stack_unbounded = true;
            continue;
        }
        uint64_t alignment = in->immediate > 16 ? in->immediate : 16;
        uint64_t padding = alignment - 1;
        if (has_stack_checkpoints(ir->function) && ir->function->has_stack_alloca)
            padding += 16;
        uint64_t amount = range.upper.lo;
        if (amount > UINT64_MAX - padding || amount + padding > UINT64_MAX - ir->dynamic_stack_bound) {
            ir->dynamic_stack_unbounded = true;
            continue;
        }
        ir->dynamic_stack_bound += amount + padding;
    }
}

static void check_stack_allocation_warnings(Lower *l)
{
    Context *c = l->context;
    const bool *reachable = NULL;
    InstructionWalk walk = {0};
    IntegerFlow sizes = {.lower = l, .walk = &walk};
    for (size_t i = 0; i < l->allocation_sites.len; i++) {
        AllocationSite site = l->allocation_sites.data[i];
        const Node *expression = site.expression;
        if (expression->kind != N_ALLOCA)
            continue;
        bool alloca = warning_enabled(c, WARNING_ALLOCA, expression->loc);
        bool large = warning_enabled(c, WARNING_ALLOCA_LARGE, expression->loc);
        if (!alloca && !large)
            continue;
        if (!reachable) {
            reachable = reachable_instructions(l, NULL, NULL, &walk);
            sizes.reachable = reachable;
        }
        if (!reachable[site.instruction])
            continue;
        bool reported = false;
        for (size_t j = 0; j < i; j++)
            if (l->allocation_sites.data[j].expression == expression &&
                reachable[l->allocation_sites.data[j].instruction]) {
                reported = true;
                break;
            }
        if (reported)
            continue;
        if (alloca)
            warning_at(c, WARNING_ALLOCA, expression->loc, "use of 'alloca'");
        if (large) {
            uint64_t limit = c->alloca_warning_limit_set ? c->alloca_warning_limit : PTRDIFF_MAX;
            if (limit == UINT64_MAX)
                continue;
            IntegerRange range = bounded_alloca_range(&sizes, expression);
            bool zero = !wide_nonzero(range.lower) && !wide_nonzero(range.upper);
            bool too_large = range.upper.hi || range.upper.lo > limit;
            bool loop = false;
            for (size_t j = i; j < l->allocation_sites.len && !loop; j++) {
                AllocationSite other = l->allocation_sites.data[j];
                if (other.expression == expression && reachable[other.instruction])
                    loop = allocation_in_loop(&sizes, other.instruction, 0);
            }
            if (zero)
                warning_at(c, WARNING_ALLOCA_LARGE, expression->loc, "argument to 'alloca' is zero");
            else if (too_large)
                warning_at(c, WARNING_ALLOCA_LARGE, expression->loc,
                           "argument to 'alloca' may exceed %llu bytes", (unsigned long long)limit);
            else if (loop)
                warning_at(c, WARNING_ALLOCA_LARGE, expression->loc, "use of 'alloca' within a loop");
        }
    }
    for (size_t i = 0; i < l->variable_array_sites.len; i++) {
        VariableArraySite site = l->variable_array_sites.data[i];
        if (!warning_enabled(c, WARNING_VLA_LARGE, site.loc))
            continue;
        if (!reachable) {
            reachable = reachable_instructions(l, NULL, NULL, &walk);
            sizes.reachable = reachable;
        }
        if (!reachable[site.instruction])
            continue;
        uint64_t limit = c->vla_warning_limit_set ? c->vla_warning_limit : PTRDIFF_MAX;
        if (limit == UINT64_MAX)
            continue;
        IntegerRange range;
        Symbol *runtime_size = site.symbol->type->runtime_size;
        if (!runtime_size || !access_layout_range(&sizes, runtime_size, site.instruction, &range) ||
            range.upper.hi || range.upper.lo > limit)
            warning_at(c, WARNING_VLA_LARGE, site.loc, "variable length array '%s' may exceed %llu bytes",
                       site.symbol->name, (unsigned long long)limit);
    }
}

static void check_function_flow(Lower *l, bool noreturn)
{
    Context *c = l->context;
    InstructionWalk walk = {0};
    const bool *reachable = !c->syntax_only ? reachable_instructions(l, NULL, NULL, &walk) : NULL;
    Instructions instructions = l->function->instructions;
    AccessInitialization initialization = {0};
    IntegerFlow sizes = {.lower = l, .walk = &walk, .reachable = reachable};
    bool check_initialization = !l->function->function->has_builtin_setjmp &&
                                !l->function->function->has_returns_twice &&
                                !l->function->function->has_nonlocal_labels;
    bool reported_return = false;
    for (size_t i = 0; i < instructions.len; i++) {
        Inst *in = &instructions.data[i];
        if (l->has_call_diagnostics && reachable && !reachable[i]) {
            /* Retain labels because branches with known conditions may still name them. */
            if (in->code != I_LABEL)
                *in = (Inst){.code = I_NOP};
            continue;
        }
        if (noreturn && in->code == I_RETURN) {
            if (reachable && reachable[i] && !reported_return) {
                warning(c, in->loc, "'noreturn' function does return");
                reported_return = true;
            }
            in->code = I_TRAP;
        }
        if (reachable && in->code == I_CALL && in->symbol && in->symbol->call_diagnostics.len)
            call_diagnostics(c, in);
        if (reachable && reachable[i] && in->code == I_CALL && in->access_call) {
            const AccessClauses *clauses = in->source_type->accesses;
            for (size_t j = 0; j < clauses->len; j++) {
                AccessClause clause = clauses->data[j];
                if (clause.reference > in->args.len || clause.size > in->args.len)
                    continue;
                IntegerRange range = {{1, 0}, {1, 0}};
                if (clause.size) {
                    Node *size = in->access_call->list.data[clause.size - 1];
                    sizes.argument = in->access_argument_starts[clause.size - 1];
                    IntegerRangeLookup lookup = expression_has_effect(c, size) ? NULL : access_integer_symbol;
                    range = integer_expression_range(c, size, lookup, &sizes);
                }
                Node *address = in->access_call->list.data[clause.reference - 1];
                sizes.argument = in->access_argument_starts[clause.reference - 1];
                AccessRangeSource source = {.pointer_stores = access_pointer_stores, .offset = access_offset_range,
                                            .allocation = access_allocation_range, .layout = access_layout_range,
                                            .data = &sizes,
                                            .point = expression_has_effect(c, address) ? SIZE_MAX : sizes.argument};
                check_access_argument(c, l->function->function, in->symbol, in->access_call, clause, range, source);
                if (check_initialization)
                    check_access_initialization(l, &initialization, reachable, in, i, clause, range);
            }
        }
    }
}

bool lower(Context *c, Symbol *function, FunctionIR *output)
{
    *output = (FunctionIR){.function = function};
    Lower l = {.context = c, .function = output};
    for (size_t i = 0; i < function->locals.len; i++) {
        Symbol *local = function->locals.data[i];
        if (!local->type->complete)
            error(c, local->loc, "object '%s' has incomplete type", local->name);
        local->slot = local->type->variable_size ? slot(&l, pointer(c, local->type))
                                                 : frame_slot(&l, local->type, symbol_alignment(local));
    }
    for (size_t i = 0; i < function->params.len; i++) {
        Symbol *param = function->params.data[i];
        if (!param->type->complete)
            error(c, param->loc, "parameter '%s' has incomplete type", param->name);
    }
    if (c->failed)
        return false;
    for (size_t i = 0; i < function->params.len; i++)
        update_object_extent(&l, function->params.data[i], NULL);
    Symbol *pool = function->trampolines;
    if (pool) {
        size_t zero = imm(&l, pool->type, 0);
        store(&l, storage_address(&l, pool), zero, pool->type);
    }
    if (function->has_stack_alloca) {
        l.stack_type = type_int(c, 4, true);
        l.stack_pin = imm(&l, l.stack_type, 0);
    }
    for (size_t i = 0; i < function->parameter_bounds.len; i++)
        scalar_full_expression(&l, function->parameter_bounds.data[i], FULL_EFFECT);
    statement(&l, function->body);
    size_t result = !function->owner && equal(function->name, "main") ?
        imm(&l, type_int(c, 3, false), 0) : 0;
    return_value(&l, result, function->body->loc);
    for (size_t i = 0; i < l.gotos.len; i++) {
        Goto from = l.gotos.data[i];
        for (size_t j = 0; j < l.labels.len; j++) {
            Label label = l.labels.data[j];
            if (label.number == from.target && label.lifetime != from.source) {
                size_t entry = new_label(&l);
                output->instructions.data[from.index].label = entry;
                mark(&l, entry);
                if (move_lifetimes(&l, from.source, label.lifetime, from.loc, false))
                    jump(&l, label.number);
            }
        }
    }
    if (l.nonlocal_labels.len)
        resolve_nonlocal(&l);
    if (l.indirect_gotos.len)
        indirect_lifetimes(&l);
    if (!c->failed && !c->syntax_only && (function->has_builtin_setjmp || function->has_returns_twice))
        check_clobbered(&l);
    if (!c->failed && !c->syntax_only && function->has_switch)
        check_fallthrough(&l);
    if (!c->failed && l.has_nonnull_comparisons)
        check_nonnull_comparisons(&l);
    bool noreturn = function_noreturn(c, function);
    if (!c->failed && (noreturn || l.has_access_calls || (!c->syntax_only && l.has_call_diagnostics)))
        check_function_flow(&l, noreturn);
    if (!c->failed && !c->syntax_only && (l.allocation_sites.len || l.variable_array_sites.len))
        check_stack_allocation_warnings(&l);
    if (!c->failed && !c->syntax_only && (c->stack_usage_report ||
        warning_enabled(c, WARNING_STACK_USAGE, function->function_end_loc)))
        analyze_stack_usage(&l);
    return !c->failed;
}
