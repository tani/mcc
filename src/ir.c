#include "compiler.h"
#include "parser.h"

typedef struct Lifetime Lifetime;
struct Lifetime { Lifetime *previous; size_t stack; };
typedef struct { size_t index, target; Lifetime *source; Loc loc; } Goto;

typedef struct {
    const char *name;
    size_t number;
    Loc loc;
    bool defined;
    Lifetime *lifetime;
} Label;

typedef struct {
    size_t label;
    uint64_t value, high;
    bool otherwise;
    Node *node;
} Case;

typedef VECTOR(Case) Cases;
typedef struct {
    Context *context;
    FunctionIR *function;
    VECTOR(Label) labels;
    VECTOR(Goto) gotos;
    size_t next_label, break_label, continue_label;
    Cases *cases;
    Lifetime *lifetime, *break_lifetime, *continue_lifetime, *switch_lifetime;
    Node *substitution;
    size_t substitute_value;
} Lower;

static size_t slot(Lower *l, Type *type)
{
    PUSH(l->context, l->function->slots, type);
    return l->function->slots.len;
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

static size_t unwind(Lower *l, Lifetime *source, Lifetime *target, Loc loc)
{
    size_t last = 0;
    for (Lifetime *p = source; p != target; p = p->previous) {
        if (!p) {
            error(l->context, loc, "jump enters the scope of a variable-length array");
            return 0;
        }
        last = p->stack;
    }
    return last;
}

static void restore(Lower *l, Lifetime *prior, Loc loc)
{
    size_t stack = unwind(l, l->lifetime, prior, loc);
    if (stack)
        put(l, (Inst){.code = I_STACK_RESTORE, .a = stack});
    l->lifetime = prior;
}

static size_t imm(Lower *l, Type *type, uint64_t n)
{
    size_t id = slot(l, type);
    put(l, (Inst){.code = I_IMMEDIATE, .dest = id, .immediate = n, .type = type});
    return id;
}

static size_t operation(Lower *l, int op, size_t a, size_t b, Type *type)
{
    size_t id = slot(l, type);
    put(l, (Inst){.code = I_BINARY, .dest = id, .a = a, .b = b, .op = op,
                  .type = type, .source_type = l->function->slots.data[a - 1]});
    return id;
}

static size_t truth(Lower *l, size_t id, Loc loc)
{
    Type *from = id ? l->function->slots.data[id - 1] : NULL;
    if (!scalar(from)) {
        error(l->context, loc, "condition requires a scalar operand");
        return imm(l, type_int(l->context, 0, true), 0);
    }
    if (from->kind != TY_FLOAT)
        return id;
    Type *to = type_int(l->context, 0, true);
    size_t dest = slot(l, to);
    put(l, (Inst){.code = I_CAST, .dest = dest, .a = id, .type = to, .source_type = from});
    return dest;
}

static void branch(Lower *l, size_t value, size_t label, bool nonzero, Loc loc)
{
    value = truth(l, value, loc);
    put(l, (Inst){.code = I_ZERO, .a = value, .label = label, .op = nonzero});
}

static size_t value(Lower *, Node *);
static void statement(Lower *, Node *);
static void init_object(Lower *, size_t, Type *, Node *);

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
    if (!s->type->variable_bound)
        return address;
    Type *t = pointer(l->context, s->type);
    size_t dest = slot(l, t);
    put(l, (Inst){.code = I_LOAD, .dest = dest, .a = address, .type = t});
    return dest;
}

static size_t object_slot(Lower *l, Type *t)
{
    size_t object = slot(l, t);
    size_t address = slot(l, pointer(l->context, t));
    put(l, (Inst){.code = I_ADDRESS, .dest = address, .a = object});
    return address;
}

static size_t offset(Lower *l, size_t id, size_t amount)
{
    if (!amount)
        return id;
    return operation(l, '+', id, imm(l, type_int(l->context, 4, true), amount),
                     l->function->slots.data[id - 1]);
}

static size_t address(Lower *l, Node *n)
{
    switch (n->kind) {
    case N_SYMBOL: return address_symbol(l, n->symbol);
    case N_COMPOUND: {
        size_t at = address_symbol(l, n->symbol);
        if (n->symbol->local) {
            if (aggregate(n->type))
                put(l, (Inst){.code = I_MEMZERO, .a = at, .immediate = n->type->size});
            init_object(l, at, n->type, n->symbol->init);
        }
        return at;
    }
    case N_DEREF: return value(l, n->left);
    case N_MEMBER: return offset(l, value(l, n->left), n->integer);
    default:
        error(l->context, n->loc, "cannot take the address of this expression");
        return imm(l, pointer(l->context, n->type), 0);
    }
}

static size_t load(Lower *l, size_t at, Type *type)
{
    if (aggregate(type) || type->kind == TY_FUNC)
        return at;
    if (type->kind == TY_VOID)
        return 0;
    size_t dest = slot(l, type);
    put(l, (Inst){.code = I_LOAD, .dest = dest, .a = at, .type = type});
    return dest;
}

static void store(Lower *l, size_t at, size_t v, Type *type)
{
    put(l, (Inst){.code = I_STORE, .a = at, .b = v, .type = type});
}

static size_t bit_load(Lower *l, size_t at, Type *type, unsigned bit, unsigned width)
{
    size_t dest = slot(l, type);
    put(l, (Inst){.code = I_BITLOAD, .a = at, .dest = dest, .type = type, .bit = bit, .width = width});
    return dest;
}

static void bit_store(Lower *l, size_t at, size_t val, Type *type, unsigned bit, unsigned width)
{
    put(l, (Inst){.code = I_BITSTORE, .a = at, .b = val, .type = type, .bit = bit, .width = width});
}

static void copy_value(Lower *l, size_t dest, size_t src)
{
    if (dest)
        put(l, (Inst){.code = I_COPY, .dest = dest, .a = src,
                      .type = l->function->slots.data[dest - 1]});
}

static size_t flow_value(Lower *l, Node *n)
{
    size_t other = new_label(l), done = new_label(l);
    Type *type = n->type;
    if (aggregate(type))
        type = pointer(l->context, type);
    size_t dest = type->kind == TY_VOID ? 0 : slot(l, type);
    size_t test = value(l, n->left);
    if (n->kind == N_COND) {
        branch(l, test, other, false, n->loc);
        Node *old_node = l->substitution;
        size_t old_value = l->substitute_value;
        if (n->postfix) {
            l->substitution = n->left;
            l->substitute_value = test;
        }
        copy_value(l, dest, value(l, n->right));
        l->substitution = old_node;
        l->substitute_value = old_value;
        jump(l, done);
        mark(l, other);
        copy_value(l, dest, value(l, n->third));
    } else {
        bool is_or = n->op == OR;
        branch(l, test, other, is_or, n->loc);
        size_t right = truth(l, value(l, n->right), n->loc);
        size_t zero = imm(l, l->function->slots.data[right - 1], 0);
        copy_value(l, dest, operation(l, NE, right, zero, type));
        jump(l, done);
        mark(l, other);
        copy_value(l, dest, imm(l, type, is_or));
    }
    mark(l, done);
    return dest;
}

static size_t value(Lower *l, Node *n)
{
    if (!n)
        return 0;
    if (n == l->substitution)
        return l->substitute_value;
    Type *t = n->type;
    switch (n->kind) {
    case N_RANGE_VALUE:
        if (!n->integer)
            n->integer = value(l, n->left);
        return n->integer;
    case N_INT: return imm(l, t, n->integer);
    case N_FLOAT: {
        size_t dest = slot(l, t);
        put(l, (Inst){.code = I_REAL, .dest = dest, .real = n->real, .type = t});
        return dest;
    }
    case N_MEMBER:
        if (n->width)
            return bit_load(l, address(l, n), t, n->bit, n->width);
        /* fall through */
    case N_SYMBOL: case N_DEREF: case N_COMPOUND:
        return load(l, address(l, n), t);
    case N_ADDR: return address(l, n->left);
    case N_COND: return flow_value(l, n);
    case N_SEQUENCE:
        value(l, n->left);
        return value(l, n->right);
    case N_STATEMENT_EXPR:
        statement(l, n->left);
        return value(l, n->right);
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
        size_t destination = aggregate(t) ? object_slot(l, t) : slot(l, t);
        put(l, (Inst){.code = I_VA_ARG, .a = at, .dest = destination, .type = t, .loc = n->loc});
        return destination;
    }
    case N_CAST: {
        size_t in = value(l, n->left);
        if (aggregate(t))
            return in;
        if (t->kind == TY_VOID)
            return 0;
        size_t dest = slot(l, t);
        put(l, (Inst){.code = I_CAST, .dest = dest, .a = in, .type = t,
                      .source_type = n->left->type, .loc = n->loc});
        return dest;
    }
    case N_UNARY: {
        size_t in = value(l, n->left);
        size_t dest = slot(l, t);
        put(l, (Inst){.code = I_UNARY, .dest = dest, .a = in, .op = n->op,
                      .type = t, .source_type = n->left->type});
        return dest;
    }
    case N_BINARY: {
        if (n->op == AND || n->op == OR)
            return flow_value(l, n);
        size_t a = value(l, n->left);
        size_t b = value(l, n->right);
        return operation(l, n->op, a, b, t);
    }
    case N_ASSIGN: case N_UPDATE: {
        size_t at = address(l, n->left);
        Node *old_node = l->substitution;
        size_t old_value = l->substitute_value, before = 0;
        Node *lhs = n->left;
        if (n->kind == N_UPDATE) {
            before = lhs->width ? bit_load(l, at, t, lhs->bit, lhs->width) : load(l, at, t);
            l->substitution = n->left;
            l->substitute_value = before;
        }
        size_t val = value(l, n->right);
        l->substitution = old_node;
        l->substitute_value = old_value;
        if (lhs->width) {
            bit_store(l, at, val, t, lhs->bit, lhs->width);
            if (!n->postfix)
                return bit_load(l, at, t, lhs->bit, lhs->width);
        } else {
            store(l, at, val, t);
        }
        return n->postfix ? before : aggregate(t) ? at : val;
    }
    case N_CALL: {
        Inst call = {.code = I_CALL, .type = t, .loc = n->loc};
        if (n->left->kind == N_ADDR && n->left->left->kind == N_SYMBOL &&
            n->left->left->type->kind == TY_FUNC)
            call.symbol = n->left->left->symbol;
        else
            call.a = value(l, n->left);
        for (size_t i = 0; i < n->list.len; i++) {
            Node *arg = n->list.data[i];
            size_t id = value(l, arg);
            if (aggregate(arg->type)) {
                size_t object = object_slot(l, arg->type);
                put(l, (Inst){.code = I_MEMCOPY, .a = object, .b = id, .immediate = arg->type->size});
                id = object;
            }
            PUSH(l->context, call.args, id);
            PUSH(l->context, call.arg_types, arg->type);
        }
        if (t->kind != TY_VOID)
            call.dest = aggregate(t) ? object_slot(l, t) : slot(l, t);
        put(l, call);
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
            Member m = t->kind == TY_ARRAY ? (Member){.type=t->base, .offset=index * t->base->size}
                                          : t->members.data[index];
            size_t place = offset(l, at, m.offset);
            if (m.bit_field)
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
        if (equal(l->labels.data[i].name, n->name))
            return &l->labels.data[i];
    Label label = {.name = n->name, .number = new_label(l), .loc = n->loc};
    PUSH(l->context, l->labels, label);
    return &l->labels.data[l->labels.len - 1];
}

static void gather_cases(Lower *l, Cases *cases, Node *n, Type *t)
{
    if (!n || n->kind == N_SWITCH)
        return;
    if (n->kind == N_CASE || n->kind == N_DEFAULT) {
        Case item = {.node = n, .label = new_label(l), .otherwise = n->kind == N_DEFAULT};
        if (!item.otherwise) {
            Node *low = convert(l->context, n->left, t, n->loc);
            if (!constant_int(l->context, low, &item.value))
                error(l->context, n->loc, "case value must be an integer constant");
            item.high = item.value;
            if (n->third && !constant_int(l->context, convert(l->context, n->third, t, n->loc), &item.high))
                error(l->context, n->loc, "case range must have constant bounds");
        }
        for (size_t i = 0; i < cases->len; i++) {
            Case prior = cases->data[i];
            bool overlaps = t->uns ? item.value <= prior.high && prior.value <= item.high :
                            (int64_t)item.value <= (int64_t)prior.high &&
                            (int64_t)prior.value <= (int64_t)item.high;
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
    size_t control = value(l, convert(c, n->left, t, n->loc));
    size_t done = new_label(l), fallback = done;
    Type *i = type_int(c, 3, false);
    Cases cases = {0};
    gather_cases(l, &cases, n->right, t);
    for (size_t j = 0; j < cases.len; j++) {
        Case item = cases.data[j];
        if (item.otherwise) {
            fallback = item.label;
        } else if (item.value == item.high) {
            size_t match = operation(l, EQ, control, imm(l, t, item.value), i);
            branch(l, match, item.label, true, n->loc);
        } else {
            size_t skip = new_label(l);
            size_t above = operation(l, GE, control, imm(l, t, item.value), i);
            branch(l, above, skip, false, n->loc);
            size_t below = operation(l, LE, control, imm(l, t, item.high), i);
            branch(l, below, item.label, true, n->loc);
            mark(l, skip);
        }
    }
    jump(l, fallback);
    Cases *prior_cases = l->cases;
    size_t prior_break = l->break_label;
    Lifetime *prior_break_lifetime = l->break_lifetime, *prior_switch = l->switch_lifetime;
    l->cases = &cases;
    l->break_label = done;
    l->break_lifetime = l->switch_lifetime = l->lifetime;
    statement(l, n->right);
    mark(l, done);
    l->cases = prior_cases;
    l->break_label = prior_break;
    l->break_lifetime = prior_break_lifetime;
    l->switch_lifetime = prior_switch;
}

static void loop(Lower *l, Node *n)
{
    size_t start = new_label(l), step = new_label(l), done = new_label(l);
    size_t prior_break = l->break_label, prior_continue = l->continue_label;
    Lifetime *prior = l->lifetime, *prior_break_lifetime = l->break_lifetime;
    Lifetime *prior_continue_lifetime = l->continue_lifetime;
    l->break_label = done;
    l->continue_label = step;
    if (n->fourth)
        statement(l, n->fourth);
    l->break_lifetime = l->continue_lifetime = l->lifetime;
    mark(l, start);
    if (n->kind == N_LOOP && n->left)
        branch(l, value(l, decay(l->context, n->left)), done, false, n->loc);
    statement(l, n->right);
    mark(l, step);
    if (n->kind == N_DO)
        branch(l, value(l, decay(l->context, n->left)), start, true, n->loc);
    else {
        value(l, n->third);
        jump(l, start);
    }
    mark(l, done);
    restore(l, prior, n->loc);
    l->break_label = prior_break;
    l->continue_label = prior_continue;
    l->break_lifetime = prior_break_lifetime;
    l->continue_lifetime = prior_continue_lifetime;
}

static void allocate_array(Lower *l, Symbol *s)
{
    Type *size_type = s->type->runtime_size->type;
    Node *bound = convert(l->context, s->type->variable_bound, size_type, s->loc);
    size_t count = value(l, bound);
    size_t size = operation(l, '*', count, imm(l, size_type, s->type->base->size), size_type);
    store(l, address_symbol(l, s->type->runtime_size), size, size_type);
    Type *pointer_type = pointer(l->context, s->type);
    size_t pointer_slot = slot(l, pointer_type);
    size_t stack_slot = slot(l, pointer_type);
    put(l, (Inst){.code = I_STACK_ALLOC, .dest = pointer_slot, .a = size, .b = stack_slot});
    store(l, storage_address(l, s), pointer_slot, pointer_type);
    Lifetime *next = alloc(l->context, sizeof *next);
    *next = (Lifetime){l->lifetime, stack_slot};
    l->lifetime = next;
}

static void statement(Lower *l, Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case N_BLOCK: {
        Lifetime *prior = l->lifetime;
        for (size_t i = 0; i < n->list.len; i++)
            statement(l, n->list.data[i]);
        restore(l, prior, n->loc);
        break;
    }
    case N_SEQUENCE:
        for (size_t i = 0; i < n->list.len; i++)
            statement(l, n->list.data[i]);
        break;
    case N_DECL: {
        Symbol *s = n->symbol;
        if (s->local && s->type->variable_bound) {
            allocate_array(l, s);
            break;
        }
        if (!s->local || !s->init)
            break;
        size_t place = address_symbol(l, s);
        if (aggregate(s->type))
            put(l, (Inst){.code = I_MEMZERO, .a = place, .immediate = s->type->size});
        init_object(l, place, s->type, s->init);
        break;
    }
    case N_EXPR: value(l, n->left); break;
    case N_RETURN: {
        size_t result = value(l, n->left);
        put(l, (Inst){.code = I_RETURN, .a = result});
        break;
    }
    case N_IF: {
        size_t otherwise = new_label(l), done = new_label(l);
        branch(l, value(l, decay(l->context, n->left)), otherwise, false, n->loc);
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
    case N_SWITCH: switch_statement(l, n); break;
    case N_CASE: case N_DEFAULT:
        if (!l->cases) {
            error(l->context, n->loc, "case/default outside of switch");
            break;
        }
        if (l->lifetime != l->switch_lifetime)
            error(l->context, n->loc, "switch case enters the scope of a variable-length array");
        for (size_t i = 0; i < l->cases->len; i++)
            if (l->cases->data[i].node == n)
                mark(l, l->cases->data[i].label);
        statement(l, n->right);
        break;
    case N_BREAK: case N_CONTINUE: {
        size_t dest = n->kind == N_BREAK ? l->break_label : l->continue_label;
        if (dest) {
            Lifetime *target = n->kind == N_BREAK ? l->break_lifetime : l->continue_lifetime;
            size_t stack = unwind(l, l->lifetime, target, n->loc);
            put(l, (Inst){.code = I_JUMP, .a = stack, .label = dest});
        } else {
            error(l->context, n->loc, "%s outside of a valid context",
                  n->kind == N_BREAK ? "break" : "continue");
        }
        break;
    }
    case N_GOTO: {
        size_t target = named_label(l, n)->number;
        Goto source = {l->function->instructions.len, target, l->lifetime, n->loc};
        PUSH(l->context, l->gotos, source);
        jump(l, target);
        break;
    }
    case N_LABEL: {
        Label *label = named_label(l, n);
        if (label->defined)
            error(l->context, n->loc, "duplicate label '%s'", label->name);
        label->defined = true;
        label->lifetime = l->lifetime;
        mark(l, label->number);
        statement(l, n->left);
        break;
    }
    default: error(l->context, n->loc, "cannot lower this statement"); break;
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
        local->slot = slot(&l, local->type->variable_bound ? pointer(c, local->type) : local->type);
    }
    for (size_t i = 0; i < function->params.len; i++) {
        Symbol *param = function->params.data[i];
        if (!param->type->complete)
            error(c, param->loc, "parameter '%s' has incomplete type", param->name);
    }
    if (c->failed)
        return false;
    statement(&l, function->body);
    for (size_t i = 0; i < l.labels.len; i++)
        if (!l.labels.data[i].defined)
            error(c, l.labels.data[i].loc, "undefined label '%s'", l.labels.data[i].name);
    for (size_t i = 0; i < l.gotos.len; i++) {
        Goto from = l.gotos.data[i];
        for (size_t j = 0; j < l.labels.len; j++) {
            Label label = l.labels.data[j];
            if (label.number == from.target && label.defined)
                output->instructions.data[from.index].a = unwind(&l, from.source, label.lifetime, from.loc);
        }
    }
    size_t result = equal(function->name, "main") ?
        imm(&l, type_int(c, 3, false), 0) : 0;
    put(&l, (Inst){.code = I_RETURN, .a = result});
    return !c->failed;
}
