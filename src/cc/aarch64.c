#include "compiler.h"
#include "parser.h"
#include "inline_asm.h"

const Target aarch64_linux = {
    .arch = "aarch64", .display_name = "AArch64", .emit = emit_aarch64,
    .char_size = 1, .short_size = 2, .int_size = 4, .long_size = 8,
    .long_long_size = 8, .pointer_size = 8, .long_double_size = 16,
    .char_unsigned = true, .wchar_unsigned = true
};

typedef struct {
    Context *context;
    FILE *out;
    FunctionIR *ir;
    size_t *offsets;
    size_t register_spills[64];
    uint64_t jump_registers;
    bool stack_checkpoint_pin;
    size_t varargs_gp, varargs_fp;
    size_t indirect_return;
    unsigned named_gp, named_fp;
    size_t named_stack;
    unsigned function_id, next_label;
} Emitter;

static void number(FILE *out, unsigned reg, uint64_t n)
{
    fprintf(out, "\tmovz x%u, #%u\n", reg, (unsigned)(n & 0xffff));
    for (unsigned shift = 16; shift < 64; shift += 16) {
        unsigned part = (unsigned)((n >> shift) & 0xffff);
        if (part)
            fprintf(out, "\tmovk x%u, #%u, lsl #%u\n", reg, part, shift);
    }
}

static void frame_offset(Emitter *e, unsigned reg, unsigned base, size_t offset)
{
    if (offset <= 4095)
        fprintf(e->out, "\tsub x%u, x%u, #%zu\n", reg, base, offset);
    else {
        number(e->out, 15, offset);
        fprintf(e->out, "\tsub x%u, x%u, x15\n", reg, base);
    }
}

static void frame_address(Emitter *e, unsigned reg, size_t offset)
{
    frame_offset(e, reg, 29, offset);
}

static void align_address(Emitter *e, unsigned reg, unsigned alignment)
{
    if (alignment > 16)
        fprintf(e->out, "\tand x%u, x%u, #-%u\n", reg, reg, alignment);
}

static void slot_address(Emitter *e, unsigned reg, size_t slot)
{
    frame_address(e, reg, e->offsets[slot]);
    align_address(e, reg, e->ir->slots.data[slot - 1].alignment);
}

static uint64_t argument_register_mask(unsigned gp, unsigned fp)
{
    return ((UINT64_C(1) << gp) - 1) | (((UINT64_C(1) << fp) - 1) << 32);
}

static void sync_registers(Emitter *e, uint64_t registers, bool reload)
{
    if (!registers)
        return;
    const char *op = reload ? "ldr" : "str";
    /* Capture x14 before using it to address SIMD slots, and reload it afterward. */
    for (unsigned i = 0; i < 64; i++) {
        unsigned reg = reload ? 63 - i : i;
        if (!(registers & (UINT64_C(1) << reg)))
            continue;
        size_t offset = e->register_spills[reg];
        if (reg < 32) {
            fprintf(e->out, "\t%s x%u, [x29, #-%zu]\n", op, reg, offset);
        } else {
            frame_address(e, 14, offset);
            fprintf(e->out, "\t%s q%u, [x14]\n", op, reg - 32);
        }
    }
}

static void sync_register_spills(Emitter *e, uint64_t skip, bool reload)
{
    sync_registers(e, e->context->global_register_spills & ~skip, reload);
}

static void quad_call(Emitter *e, const char *name, unsigned gp, unsigned fp)
{
    sync_register_spills(e, argument_register_mask(gp, fp), true);
    fprintf(e->out, "\tbl %s\n", name);
    sync_register_spills(e, 0, false);
}

static void enclosing_frame(Emitter *e, Symbol *owner, unsigned reg)
{
    fprintf(e->out, "\tmov x%u, x29\n", reg);
    for (Symbol *fn = e->ir->function; fn != owner; fn = fn->owner) {
        frame_offset(e, reg, reg, fn->static_chain->frame_offset);
        fprintf(e->out, "\tldr x%u, [x%u]\n", reg, reg);
    }
}

static void slot_load(Emitter *e, unsigned reg, size_t slot)
{
    frame_address(e, 14, e->offsets[slot]);
    fprintf(e->out, "\tldr x%u, [x14]\n", reg);
}

static void slot_store(Emitter *e, unsigned reg, size_t slot)
{
    frame_address(e, 14, e->offsets[slot]);
    fprintf(e->out, "\tstr x%u, [x14]\n", reg);
}

static void slot_pair(Emitter *e, unsigned low, unsigned high, size_t slot, bool load)
{
    frame_address(e, 14, e->offsets[slot]);
    fprintf(e->out, "\t%s x%u, x%u, [x14]\n", load ? "ldp" : "stp", low, high);
}

static char fp_size(unsigned size)
{
    return size < 4 ? 'h' : size == 4 ? 's' : size == 8 ? 'd' : 'q';
}

static char fp_width(Type *t)
{
    return fp_size(t->size);
}

static bool quad(Type *t)
{
    return (t->kind == TY_FLOAT || t->kind == TY_DECIMAL) && t->size == 16;
}

static void fp_load(Emitter *e, unsigned reg, size_t id)
{
    Type *t = e->ir->slots.data[id - 1].type;
    frame_address(e, 14, e->offsets[id]);
    fprintf(e->out, "\tldr %c%u, [x14]\n", fp_width(t), reg);
}

static void fp_store(Emitter *e, unsigned reg, size_t id)
{
    Type *t = e->ir->slots.data[id - 1].type;
    frame_address(e, 14, e->offsets[id]);
    fprintf(e->out, "\tstr %c%u, [x14]\n", fp_width(t), reg);
}

static void write_register_slot(Emitter *e, unsigned reg, size_t source)
{
    size_t spill = e->register_spills[reg];
    if (reg >= 32) {
        fp_load(e, spill ? 0 : reg - 32, source);
        if (spill) {
            frame_address(e, 14, spill);
            fputs("\tstr q0, [x14]\n", e->out);
        }
    } else {
        slot_load(e, spill ? 9 : reg, source);
        if (spill)
            fprintf(e->out, "\tstr x9, [x29, #-%zu]\n", spill);
    }
}

static void adjust_stack(FILE *out, size_t size, bool pop)
{
    if (!size)
        return;
    const char *op = pop ? "add" : "sub";
    if (size <= 4095)
        fprintf(out, "\t%s sp, sp, #%zu\n", op, size);
    else {
        number(out, 15, size);
        fprintf(out, "\t%s sp, sp, x15\n", op);
    }
}

static void normalize(FILE *out, Type *type, unsigned reg)
{
    if (type->kind == TY_BOOL)
        fprintf(out, "\tcmp x%u, #0\n\tcset w%u, ne\n", reg, reg);
    else if (type->kind == TY_INT && type->size < 8) {
        const char *width = type->size == 1 ? "b" : type->size == 2 ? "h" : "w";
        if (type->size == 4 && type->uns)
            fprintf(out, "\tmov w%u, w%u\n", reg, reg);
        else
            fprintf(out, "\t%sxt%s %c%u, w%u\n", type->uns ? "u" : "s", width,
                    type->uns ? 'w' : 'x', reg, reg);
    }
}

static void object_load(FILE *out, Type *t, unsigned dest, unsigned at)
{
    const char *op = "ldr";
    char width = 'x';
    if (t->size == 1) { op = t->uns ? "ldrb" : "ldrsb"; width = t->uns ? 'w' : 'x'; }
    if (t->size == 2) { op = t->uns ? "ldrh" : "ldrsh"; width = t->uns ? 'w' : 'x'; }
    if (t->size == 4 && t->kind == TY_INT && !t->uns) op = "ldrsw";
    else if (t->size == 4) width = 'w';
    fprintf(out, "\t%s %c%u, [x%u]\n", op, width, dest, at);
}

static void object_store(FILE *out, Type *t, unsigned src, unsigned at)
{
    const char *op = t->size == 1 ? "strb" : t->size == 2 ? "strh" : "str";
    fprintf(out, "\t%s %c%u, [x%u]\n", op, t->size < 8 ? 'w' : 'x', src, at);
}

static void object_address(Emitter *e, Symbol *symbol)
{
    if (symbol->local) {
        if (symbol->owner == e->ir->function) {
            slot_address(e, 9, symbol->slot);
        } else {
            enclosing_frame(e, symbol->owner, 9);
            frame_offset(e, 9, 9, symbol->frame_offset);
            if (!symbol->type->variable_size)
                align_address(e, 9, symbol_alignment(symbol));
        }
    } else if (symbol->thread_local_storage) {
        const char *name = symbol->assembly;
        sync_register_spills(e, argument_register_mask(2, 0), true);
        fprintf(e->out, "\tadrp x0, :tlsdesc:%s\n\tldr x1, [x0, #:tlsdesc_lo12:%s]\n"
                        "\tadd x0, x0, #:tlsdesc_lo12:%s\n\t.tlsdesccall %s\n\tblr x1\n",
                name, name, name, name);
        sync_register_spills(e, 0, false);
        fputs("\tmrs x9, tpidr_el0\n\tadd x9, x9, x0\n", e->out);
    } else if (symbol->internal && !symbol->weak_reference) {
        fprintf(e->out, "\tadrp x9, %s\n\tadd x9, x9, :lo12:%s\n",
                symbol->assembly, symbol->assembly);
    } else {
        fprintf(e->out, "\tadrp x9, :got:%s\n\tldr x9, [x9, :got_lo12:%s]\n",
                symbol->assembly, symbol->assembly);
    }
}

static const char *condition(int op, bool uns, bool fp)
{
    switch (op) {
    case EQ: return "eq";
    case NE: return "ne";
    case '<': return fp ? "mi" : uns ? "lo" : "lt";
    case '>': return uns ? "hi" : "gt";
    case LE: return fp ? "ls" : uns ? "ls" : "le";
    case GE: return uns ? "hs" : "ge";
    default: return NULL;
    }
}

static void binary_instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    Type *type = in->source_type;
    bool fp = type->kind == TY_FLOAT;
    const char *cond = condition(in->op, type->uns, fp);
    if (fp) {
        fp_load(e, 0, in->a);
        fp_load(e, 1, in->b);
        char r = fp_width(type);
        if (quad(type)) {
            const char *helper;
            switch (in->op) {
            case '+': helper = "__addtf3"; break;
            case '-': helper = "__subtf3"; break;
            case '*': helper = "__multf3"; break;
            case '/': helper = "__divtf3"; break;
            case '<': case LE: helper = "__letf2"; break;
            case '>': case GE: helper = "__getf2"; break;
            default: helper = "__eqtf2"; break;
            }
            quad_call(e, helper, 0, 2);
            if (!cond) {
                fp_store(e, 0, in->dest);
                return;
            }
            fprintf(out, "\tcmp w0, #0\n\tcset w9, %s\n", condition(in->op, false, false));
        } else if (cond) {
            fprintf(out, "\tfcmp %c0, %c1\n\tcset w9, %s\n", r, r, cond);
        } else {
            const char *op = in->op == '+' ? "fadd" : in->op == '-' ? "fsub" :
                             in->op == '*' ? "fmul" : "fdiv";
            fprintf(out, "\t%s %c0, %c0, %c1\n", op, r, r, r);
            fp_store(e, 0, in->dest);
            return;
        }
    } else {
        slot_load(e, 9, in->a);
        slot_load(e, 10, in->b);
        if (cond) {
            fprintf(out, "\tcmp x9, x10\n\tcset w9, %s\n", cond);
        } else {
            const char *op;
            switch (in->op) {
            case '+': op = "add"; break;
            case '-': op = "sub"; break;
            case '*': op = "mul"; break;
            case '&': op = "and"; break;
            case '|': op = "orr"; break;
            case '^': op = "eor"; break;
            case SHL: op = "lsl"; break;
            case SHR: op = type->uns ? "lsr" : "asr"; break;
            case '/': op = type->uns ? "udiv" : "sdiv"; break;
            case '%':
                fprintf(out, "\t%s x11, x9, x10\n\tmsub x9, x11, x10, x9\n",
                        type->uns ? "udiv" : "sdiv");
                op = NULL;
                break;
            default: op = NULL; break;
            }
            if (op)
                fprintf(out, "\t%s x9, x9, x10\n", op);
        }
    }
    normalize(out, in->type, 9);
    slot_store(e, 9, in->dest);
}

static void cast_instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    Type *a = in->source_type, *b = in->type;
    if (integer128(a) || integer128(b)) {
        if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) {
            error(e->context, in->loc, "128-bit integer and floating point conversion is unsupported");
        } else if (integer128(a)) {
            slot_pair(e, 9, 10, in->a, true);
            if (integer128(b)) {
                slot_pair(e, 9, 10, in->dest, false);
            } else {
                if (b->kind == TY_BOOL)
                    fputs("\torr x9, x9, x10\n", out);
                normalize(out, b, 9);
                slot_store(e, 9, in->dest);
            }
        } else {
            slot_load(e, 9, in->a);
            fputs(a->uns ? "\tmov x10, #0\n" : "\tasr x10, x9, #63\n", out);
            slot_pair(e, 9, 10, in->dest, false);
        }
        return;
    }
    char from = fp_width(a), to = fp_width(b);
    if (a->kind == TY_FLOAT) {
        fp_load(e, 0, in->a);
        if (b->kind == TY_FLOAT) {
            if (quad(a) && !quad(b))
                quad_call(e, to == 's' ? "__trunctfsf2" : "__trunctfdf2", 0, 1);
            else if (!quad(a) && quad(b))
                quad_call(e, from == 's' ? "__extendsftf2" : "__extenddftf2", 0, 1);
            else if (from != to)
                fprintf(out, "\tfcvt %c0, %c0\n", to, from);
            fp_store(e, 0, in->dest);
            return;
        }
        if (quad(a) && b->kind == TY_BOOL) {
            fputs("\tmovi v1.16b, #0\n", out);
            quad_call(e, "__eqtf2", 0, 2);
            fputs("\tcmp w0, #0\n\tcset w9, ne\n", out);
        } else if (quad(a)) {
            quad_call(e, b->uns ? "__fixunstfdi" : "__fixtfdi", 0, 1);
            fputs("\tmov x9, x0\n", out);
        } else if (b->kind == TY_BOOL)
            fprintf(out, "\tfcmp %c0, #0.0\n\tcset w9, ne\n", from);
        else
            fprintf(out, "\tfcvtz%c x9, %c0\n", b->uns ? 'u' : 's', from);
    } else {
        slot_load(e, 9, in->a);
        if (b->kind == TY_FLOAT) {
            if (quad(b)) {
                fputs("\tmov x0, x9\n", out);
                quad_call(e, a->uns ? "__floatunditf" : "__floatditf", 1, 0);
            } else {
                fprintf(out, "\t%ccvtf %c0, x9\n", a->uns ? 'u' : 's', to);
            }
            fp_store(e, 0, in->dest);
            return;
        }
    }
    normalize(out, b, 9);
    slot_store(e, 9, in->dest);
}

typedef enum { ARG_GP, ARG_FP, ARG_INDIRECT } ArgumentKind;
typedef struct {
    ArgumentKind kind;
    unsigned count, element;
    size_t offsets[4];
    bool stack, vector_members, decimal_members, complex_bfloat;
    unsigned reg;
    size_t offset;
} Argument;
typedef struct { unsigned gp, fp; size_t stack; } ArgumentState;

static bool hfa(Type *t, size_t offset, Argument *a)
{
    if (t->kind == TY_COMPLEX && t->base->bfloat) {
        /* A single complex BF value occupies two SIMD registers. */
        if (a->count)
            return false;
        a->element = t->base->size;
        a->offsets[a->count++] = offset;
        a->offsets[a->count++] = offset + a->element;
        a->complex_bfloat = true;
        return true;
    }
    if (t->kind == TY_COMPLEX)
        return hfa(t->base, offset, a) && hfa(t->base, offset + t->base->size, a);
    /* Baseline GNU AArch64 classifies records of scalar BF members as composites. */
    if (t->bfloat)
        return false;
    bool vector = t->kind == TY_VECTOR && (t->size == 8 || t->size == 16);
    bool decimal = t->kind == TY_DECIMAL;
    if (t->kind == TY_FLOAT || decimal || vector) {
        if (a->count == 4 || (a->count && (a->element != t->size || a->vector_members != vector ||
                                         a->decimal_members != decimal || a->complex_bfloat)))
            return false;
        a->element = t->size;
        a->vector_members = vector;
        a->decimal_members = decimal;
        a->offsets[a->count++] = offset;
        return true;
    }
    if (t->kind == TY_ARRAY) {
        if (!t->length || t->length > 4)
            return false;
        for (size_t j = 0; j < t->length; j++)
            if (!hfa(t->base, offset + j * t->base->size, a))
                return false;
        return true;
    }
    if (t->kind == TY_STRUCT && t->members.len) {
        unsigned first = a->count;
        for (size_t j = 0; j < t->members.len; j++) {
            Member m = t->members.data[j];
            if (m.bit_field && !m.width)
                continue;
            if (!hfa(m.type, offset + m.offset, a))
                return false;
        }
        return a->count > first && (a->count - first) * a->element == t->size;
    }
    if (t->kind == TY_UNION && t->members.len) {
        Argument largest = {0};
        for (size_t j = 0; j < t->members.len; j++) {
            Argument branch = {0};
            if (!hfa(t->members.data[j].type, offset, &branch) ||
                (largest.count && (largest.element != branch.element ||
                                   largest.vector_members != branch.vector_members ||
                                   largest.decimal_members != branch.decimal_members ||
                                   largest.complex_bfloat != branch.complex_bfloat)))
                return false;
            if (branch.count > largest.count)
                largest = branch;
        }
        if (largest.count > 4 - a->count ||
            (a->count && (a->element != largest.element || a->vector_members != largest.vector_members ||
                          a->decimal_members != largest.decimal_members || a->complex_bfloat ||
                          largest.complex_bfloat)))
            return false;
        a->element = largest.element;
        a->vector_members = largest.vector_members;
        a->decimal_members = largest.decimal_members;
        a->complex_bfloat = largest.complex_bfloat;
        for (unsigned j = 0; j < largest.count; j++)
            a->offsets[a->count++] = largest.offsets[j];
        return true;
    }
    return false;
}

static Argument argument_model(Type *t)
{
    if (t->variable_size)
        return (Argument){.kind = ARG_INDIRECT, .count = 1};
    if (t->bfloat)
        return (Argument){.kind = ARG_FP, .count = 1, .element = t->size};
    Argument a = {.kind = ARG_FP};
    if (hfa(t, 0, &a))
        return a;
    if (address_value(t) && t->size > 16)
        return (Argument){.kind = ARG_INDIRECT, .count = 1};
    return (Argument){.kind = ARG_GP, .count = (address_value(t) || integer128(t)) ? (t->size + 7) / 8 : 1};
}

static Argument parameter_model(Type *t)
{
    Argument a = argument_model(t);
    /* A direct one-lane TF vector argument is split across two SIMD registers. */
    if (t->kind == TY_VECTOR && t->size == 16 && quad(t->base)) {
        a.count = 2;
        a.element = 8;
        a.offsets[1] = 8;
    }
    return a;
}

static unsigned argument_alignment(Type *type)
{
    /* AAPCS64 uses the alignment before applying a whole-record attribute. */
    return type->record_alignment ? type->member_alignment : abi_alignment(type);
}

static bool narrow_float_vector(Type *t)
{
    return t->kind == TY_VECTOR && t->size < 8 && t->base->kind == TY_FLOAT;
}

static Argument classify(ArgumentState *state, Type *t)
{
    Argument a = parameter_model(t);
    unsigned *next = a.kind == ARG_FP ? &state->fp : &state->gp;
    unsigned type_alignment = argument_alignment(t);
    size_t alignment = a.kind == ARG_INDIRECT ? 8 : type_alignment > 8 ? type_alignment : 8;
    if (narrow_float_vector(t)) {
        state->gp = 8;
        a.stack = true;
        a.offset = align_to(state->stack, alignment);
        state->stack = a.offset + align_to(t->size, 8);
        return a;
    }
    if (a.kind == ARG_GP && alignment >= 16)
        *next = (*next + 1) & ~1u;
    if (a.count <= 8 - *next) {
        a.reg = *next;
        *next += a.count;
    } else {
        *next = 8;
        a.stack = true;
        a.offset = align_to(state->stack, alignment);
        state->stack = a.offset + (a.kind == ARG_INDIRECT ? 8 : align_to(t->size, 8));
    }
    return a;
}

static void stack_address(FILE *out, bool incoming, size_t offset)
{
    const char *base = incoming ? "x29" : "sp";
    offset += incoming ? 16 : 0;
    if (offset <= 4095) {
        fprintf(out, "\tadd x13, %s, #%zu\n", base, offset);
    } else {
        number(out, 13, offset);
        fprintf(out, "\tadd x13, %s, x13\n", base);
    }
}

static void memory_loop(Emitter *, size_t, bool);

static void aggregate_registers(FILE *out, Type *t, Argument a, unsigned address,
                                bool from_registers)
{
    if (a.kind == ARG_FP) {
        char width = fp_size(a.element);
        for (unsigned i = 0; i < a.count; i++)
            fprintf(out, "\t%s %c%u, [x%u, #%zu]\n", from_registers ? "ldr" : "str",
                    width, a.reg + i, address, a.offsets[i]);
        return;
    }
    for (unsigned i = 0; i < a.count; i++) {
        unsigned reg = a.reg + i;
        unsigned remaining = t->size - i * 8;
        if (remaining >= 8) {
            fprintf(out, "\t%s x%u, [x%u, #%u]\n", from_registers ? "ldr" : "str",
                    reg, address, i * 8);
        } else if (from_registers) {
            fprintf(out, "\tmov x%u, #0\n", reg);
            for (unsigned j = 0; j < remaining; j++)
                fprintf(out, "\tldrb w11, [x%u, #%u]\n\torr x%u, x%u, x11, lsl #%u\n",
                        address, i * 8 + j, reg, reg, j * 8);
        } else {
            for (unsigned j = 0; j < remaining; j++)
                fprintf(out, "\tlsr x11, x%u, #%u\n\tstrb w11, [x%u, #%u]\n",
                        reg, j * 8, address, i * 8 + j);
        }
    }
}

static void parameters(Emitter *e)
{
    Symbol *function = e->ir->function;
    ArgumentState state = {0};
    for (size_t i = 0; i < function->params.len; i++) {
        Symbol *param = function->params.data[i];
        Type *t = parameter_abi_type(param->type);
        Argument place = classify(&state, t);
        slot_address(e, 14, param->slot);
        if (t->variable_size) {
            if (place.stack) {
                stack_address(e->out, true, place.offset);
                fputs("\tldr x10, [x13]\n\tstr x10, [x14]\n", e->out);
            } else {
                fprintf(e->out, "\tstr x%u, [x14]\n", place.reg);
            }
        } else if (address_value(t)) {
            if (place.kind == ARG_INDIRECT || place.stack) {
                if (place.stack) {
                    stack_address(e->out, true, place.offset);
                    if (place.kind == ARG_INDIRECT)
                        fputs("\tldr x10, [x13]\n", e->out);
                    else
                        fputs("\tmov x10, x13\n", e->out);
                } else {
                    fprintf(e->out, "\tmov x10, x%u\n", place.reg);
                }
                fputs("\tmov x9, x14\n", e->out);
                memory_loop(e, t->size, true);
            } else {
                aggregate_registers(e->out, t, place, 14, false);
            }
        } else if (place.stack) {
            stack_address(e->out, true, place.offset);
            if (quad(t) || integer128(t))
                fputs("\tldr q16, [x13]\n\tstr q16, [x14]\n", e->out);
            else {
                object_load(e->out, t, 9, 13);
                object_store(e->out, t, 9, 14);
            }
        } else if (t->kind == TY_FLOAT || t->kind == TY_DECIMAL) {
            fprintf(e->out, "\tstr %c%u, [x14]\n", fp_width(t), place.reg);
        } else if (integer128(t)) {
            fprintf(e->out, "\tstp x%u, x%u, [x14]\n", place.reg, place.reg + 1);
        } else {
            object_store(e->out, t, place.reg, 14);
        }
    }
    e->named_gp = state.gp;
    e->named_fp = state.fp;
    e->named_stack = state.stack;
}

static void va_start_instruction(Emitter *e, Inst *in)
{
    slot_load(e, 9, in->a);
    stack_address(e->out, true, e->named_stack);
    fputs("\tstr x13, [x9]\n", e->out);
    frame_address(e, 10, e->varargs_gp - 64);
    frame_address(e, 11, e->varargs_fp - 128);
    fputs("\tstp x10, x11, [x9, #8]\n", e->out);
    number(e->out, 10, (uint64_t)((int)e->named_gp * 8 - 64));
    number(e->out, 11, (uint64_t)((int)e->named_fp * 16 - 128));
    fputs("\tstp w10, w11, [x9, #24]\n", e->out);
}

static void va_arg_instruction(Emitter *e, Inst *in)
{
    Type *t = in->type;
    Argument model = parameter_model(t);
    bool fp = model.kind == ARG_FP, indirect = model.kind == ARG_INDIRECT;
    unsigned idx = e->next_label++;
    FILE *out = e->out;
    unsigned reg_offset = fp ? 28 : 24;
    unsigned increment = model.count * (fp ? 16 : 8);
    size_t size = indirect ? 8 : align_to(t->size, 8);
    bool aligned = !indirect && argument_alignment(t) > 8;
    slot_load(e, 9, in->a);
    if (narrow_float_vector(t))
        fprintf(out, "\tstr wzr, [x9, #24]\n\tb .Lvstack%u_%u\n", e->function_id, idx);
    fprintf(out, "\tldrsw x10, [x9, #%u]\n\ttbz x10, #63, .Lvstack%u_%u\n",
            reg_offset, e->function_id, idx);
    if (!fp && aligned)
        fputs("\tadd x10, x10, #15\n\tand x10, x10, #-16\n", out);
    fprintf(out, "\tadds w11, w10, #%u\n\tstr w11, [x9, #%u]\n\tb.gt .Lvstack%u_%u\n"
                 "\tldr x11, [x9, #%u]\n\tadd x10, x11, x10\n",
            increment, reg_offset, e->function_id, idx, fp ? 16u : 8u);
    if (fp && address_value(t)) {
        slot_load(e, 9, in->dest);
        char width = fp_size(model.element);
        for (unsigned j = 0; j < model.count; j++)
            fprintf(out, "\tldr %c0, [x10, #%u]\n\tstr %c0, [x9, #%zu]\n",
                    width, j * 16, width, model.offsets[j]);
        fprintf(out, "\tb .Lvcomplete%u_%u\n", e->function_id, idx);
    } else {
        fprintf(out, "\tb .Lvarg%u_%u\n", e->function_id, idx);
    }
    fprintf(out, ".Lvstack%u_%u:\n\tldr x10, [x9]\n", e->function_id, idx);
    if (aligned)
        fputs("\tadd x10, x10, #15\n\tand x10, x10, #-16\n", out);
    number(out, 11, size);
    fprintf(out, "\tadd x11, x10, x11\n\tstr x11, [x9]\n.Lvarg%u_%u:\n", e->function_id, idx);
    if (address_value(t)) {
        if (indirect)
            fputs("\tldr x10, [x10]\n", out);
        slot_load(e, 9, in->dest);
        memory_loop(e, t->size, true);
    } else if (integer128(t)) {
        fputs("\tldp x9, x10, [x10]\n", out);
        slot_pair(e, 9, 10, in->dest, false);
    } else if (quad(t)) {
        fputs("\tldr q0, [x10]\n", out);
        fp_store(e, 0, in->dest);
    } else {
        object_load(out, t, 9, 10);
        slot_store(e, 9, in->dest);
    }
    fprintf(out, ".Lvcomplete%u_%u:\n", e->function_id, idx);
}

static void call_instruction(Emitter *e, Inst *in)
{
    ArgumentState state = {0};
    for (size_t i = 0; i < in->args.len; i++)
        classify(&state, in->arg_types.data[i]);
    size_t outgoing = align_to(state.stack, 16);
    adjust_stack(e->out, outgoing, false);
    state = (ArgumentState){0};
    uint64_t occupied = 0;
    for (size_t i = 0; i < in->args.len; i++) {
        size_t id = in->args.data[i];
        Type *t = in->arg_types.data[i];
        Argument place = classify(&state, t);
        if (!place.stack)
            occupied |= ((UINT64_C(1) << place.count) - 1) <<
                        (place.reg + (place.kind == ARG_FP ? 32 : 0));
        if (place.stack) {
            stack_address(e->out, false, place.offset);
            if (place.kind == ARG_INDIRECT) {
                slot_load(e, 9, id);
                fputs("\tstr x9, [x13]\n", e->out);
            } else if (address_value(t)) {
                slot_load(e, 10, id);
                fputs("\tmov x9, x13\n", e->out);
                memory_loop(e, t->size, true);
            } else if (quad(t)) {
                fp_load(e, 16, id);
                fputs("\tstr q16, [x13]\n", e->out);
            } else if (integer128(t)) {
                slot_pair(e, 9, 10, id, true);
                fputs("\tstp x9, x10, [x13]\n", e->out);
            } else {
                slot_load(e, 9, id);
                object_store(e->out, t, 9, 13);
            }
        } else if (place.kind == ARG_INDIRECT) {
            slot_load(e, place.reg, id);
        } else if (address_value(t)) {
            slot_load(e, 9, id);
            aggregate_registers(e->out, t, place, 9, true);
        } else if (t->kind == TY_FLOAT || t->kind == TY_DECIMAL) {
            fp_load(e, place.reg, id);
        } else if (integer128(t)) {
            slot_pair(e, place.reg, place.reg + 1, id, true);
        } else {
            slot_load(e, place.reg, id);
        }
    }
    Argument result = argument_model(in->type);
    if (result.kind == ARG_INDIRECT) {
        slot_load(e, 8, in->dest);
        occupied |= UINT64_C(1) << 8;
    }
    if (in->b) {
        slot_load(e, 18, in->b);
        occupied |= UINT64_C(1) << 18;
    } else if (in->symbol && in->symbol->owner) {
        enclosing_frame(e, in->symbol->owner, 18);
        occupied |= UINT64_C(1) << 18;
    }
    bool got_call = call_through_got(e->context, in->symbol);
    if (!in->symbol)
        slot_load(e, 16, in->a);
    if (!in->symbol || got_call)
        occupied |= UINT64_C(1) << 16;
    sync_register_spills(e, occupied, true);
    if (got_call) {
        const char *name = in->symbol->assembly;
        fprintf(e->out, "\tadrp x16, :got:%s\n\tldr x16, [x16, :got_lo12:%s]\n\tblr x16\n", name, name);
    } else if (in->symbol) {
        fprintf(e->out, "\tbl %s\n", in->symbol->assembly);
    } else {
        fputs("\tblr x16\n", e->out);
    }
    sync_register_spills(e, 0, false);
    adjust_stack(e->out, outgoing, true);
    if (address_value(in->type)) {
        if (result.kind != ARG_INDIRECT) {
            slot_load(e, 9, in->dest);
            aggregate_registers(e->out, in->type, result, 9, false);
        }
    } else if (in->type->kind == TY_FLOAT || in->type->kind == TY_DECIMAL) {
        fp_store(e, 0, in->dest);
    } else if (integer128(in->type)) {
        slot_pair(e, 0, 1, in->dest, false);
    } else if (in->dest) {
        normalize(e->out, in->type, 0);
        slot_store(e, 0, in->dest);
    }
}

static void memory_loop_from_count(Emitter *e, bool copy, bool can_be_zero)
{
    unsigned id = e->next_label++;
    if (can_be_zero)
        fprintf(e->out, "\tcbz x12, .Lmdone%u_%u\n", e->function_id, id);
    fprintf(e->out, ".Lm%u_%u:\n", e->function_id, id);
    if (copy)
        fputs("\tldrb w11, [x10], #1\n\tstrb w11, [x9], #1\n", e->out);
    else
        fputs("\tstrb wzr, [x9], #1\n", e->out);
    fprintf(e->out, "\tsubs x12, x12, #1\n\tb.ne .Lm%u_%u\n", e->function_id, id);
    if (can_be_zero)
        fprintf(e->out, ".Lmdone%u_%u:\n", e->function_id, id);
}

static void memory_loop(Emitter *e, size_t size, bool copy)
{
    if (size) {
        number(e->out, 12, size);
        memory_loop_from_count(e, copy, false);
    }
}

static void preserve_asm_registers(Emitter *e, const AsmPlan *plan, bool restore)
{
    for (unsigned i = 0; i < 64; i++) {
        size_t id = plan->save_slots[i];
        if (!id)
            continue;
        if (i >= 32) {
            frame_address(e, 14, e->offsets[id]);
            fprintf(e->out, "\t%s d%u, [x14]\n", restore ? "ldr" : "str", i - 32);
        } else if (restore) {
            slot_load(e, i, id);
        } else {
            slot_store(e, i, id);
        }
    }
}

static void prepare_asm(Emitter *e, const AsmPlan *plan)
{
    preserve_asm_registers(e, plan, false);
    /* Tied inputs for spilled global outputs use scratch registers while staging. */
    for (int staged = 1; staged >= 0; staged--) {
        for (size_t i = 0; i < plan->assembly->operands.len; i++) {
            const AsmBinding *binding = &plan->bindings[i];
            if (binding->load && (e->register_spills[binding->reg] != 0) == staged)
                write_register_slot(e, binding->reg, binding->load);
        }
    }
    sync_register_spills(e, 0, true);
    emit_asm_template(e->context, e->out, plan, e->function_id);
}

static void finish_asm(Emitter *e, const AsmPlan *plan)
{
    sync_register_spills(e, 0, false);
    for (size_t i = 0; i < plan->assembly->outputs; i++) {
        const AsmBinding *binding = &plan->bindings[i];
        if (binding->location != ASM_REGISTER || !binding->result)
            continue;
        if (binding->reg >= 32)
            fp_store(e, binding->reg - 32, binding->result);
        else
            slot_store(e, binding->reg, binding->result);
    }
    for (size_t i = 0; i < plan->assembly->outputs; i++) {
        const AsmBinding *binding = &plan->bindings[i];
        if (binding->location == ASM_FLAGS) {
            fprintf(e->out, "\tcset w9, %s\n", binding->condition);
            slot_store(e, 9, binding->result);
        }
    }
    preserve_asm_registers(e, plan, true);
}

static void stack_pin_checkpoint(Emitter *e, size_t pin, bool restoring)
{
    if (!pin)
        return;
    if (restoring) {
        fputs("\tldr x10, [sp]\n", e->out);
        slot_store(e, 10, pin);
    } else {
        slot_load(e, 10, pin);
        fputs("\tstr x10, [sp]\n", e->out);
    }
}

static void setjmp_instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    unsigned label = ++e->next_label;
    slot_load(e, 9, in->a);
    /* The GNU buffer starts with the frame pointer, continuation and stack pointer. */
    fprintf(out, "\tadr x10, .Lsetjmp%u_%u\n\tstp x29, x10, [x9]\n",
            e->function_id, label);
    fputs("\tmov x10, sp\n\tstr x10, [x9, #16]\n", out);
    stack_pin_checkpoint(e, in->b, false);
    fprintf(out, "\tmov w9, #0\n\tb .Lsetjmp_done%u_%u\n.Lsetjmp%u_%u:\n",
            e->function_id, label, e->function_id, label);
    fputs("\t.word 0xd50324df // bti jc\n", out);
    sync_registers(e, e->jump_registers, true);
    stack_pin_checkpoint(e, in->b, true);
    fputs("\tmov w9, #1\n", out);
    fprintf(out, ".Lsetjmp_done%u_%u:\n", e->function_id, label);
    slot_store(e, 9, in->dest);
}

static void instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    switch (in->code) {
    case I_FLOW_STATEMENT: case I_FALLTHROUGH: case I_CASE_BOUNDARY: case I_NOP:
    case I_NONNULL_COMPARE: case I_NONNULL_WRITE:
        break;
    case I_IMMEDIATE:
        number(out, 9, in->immediate);
        if (integer128(in->type) || quad(in->type)) {
            number(out, 10, in->immediate_high);
            slot_pair(e, 9, 10, in->dest, false);
        } else {
            normalize(out, in->type, 9);
            slot_store(e, 9, in->dest);
        }
        break;
    case I_REAL:
        if (quad(in->type)) {
            number(out, 9, in->immediate);
            number(out, 10, in->immediate_high);
            frame_address(e, 14, e->offsets[in->dest]);
            fputs("\tstp x9, x10, [x14]\n", out);
        } else {
            number(out, 9, float_bits(in->real, floating_format(e->context->target, in->type)));
            slot_store(e, 9, in->dest);
        }
        break;
    case I_ADDRESS:
        if (in->symbol)
            object_address(e, in->symbol);
        else
            slot_address(e, 9, in->a);
        slot_store(e, 9, in->dest);
        break;
    case I_SYSTEM_REGISTER: {
        const char *name = in->op == SYSREG_FPCR ? "fpcr" :
                           in->op == SYSREG_FPSR ? "fpsr" : "tpidr_el0";
        if (in->a) {
            slot_load(e, 9, in->a);
            fprintf(out, "\tmsr %s, x9\n", name);
        } else {
            fprintf(out, "\tmrs x9, %s\n", name);
            normalize(out, in->type, 9);
            slot_store(e, 9, in->dest);
        }
        break;
    }
    case I_FRAME:
        enclosing_frame(e, in->symbol, 9);
        slot_store(e, 9, in->dest);
        break;
    case I_FRAME_RETURN:
        slot_load(e, 9, in->a);
        /* XPACLRI is a hint on cores without pointer authentication. */
        fputs("\tldr x9, [x9, #8]\n\tmov x10, x30\n\tmov x30, x9\n"
              "\t.word 0xd50320ff // xpaclri\n\tmov x9, x30\n\tmov x30, x10\n", out);
        slot_store(e, 9, in->dest);
        break;
    case I_REGISTER_READ: {
        unsigned reg = in->symbol->register_binding - 1;
        size_t spill = e->register_spills[reg];
        if (reg >= 32) {
            if (spill) {
                frame_address(e, 14, spill);
                fprintf(out, "\tldr %c0, [x14]\n", fp_width(in->type));
                reg = 32;
            }
            fp_store(e, reg - 32, in->dest);
        } else if (in->type->size == 8 && !spill) {
            slot_store(e, reg, in->dest);
        } else {
            if (spill) {
                fprintf(out, "\tldr x9, [x29, #-%zu]\n", spill);
                reg = 9;
            }
            if (in->type->kind == TY_BOOL)
                fprintf(out, "\tuxtb w9, w%u\n", reg);
            else if (reg != 9)
                fprintf(out, "\tmov x9, x%u\n", reg);
            normalize(out, in->type, 9);
            slot_store(e, 9, in->dest);
        }
        break;
    }
    case I_REGISTER_WRITE:
        write_register_slot(e, in->symbol->register_binding - 1, in->a);
        break;
    case I_LOAD:
        slot_load(e, 10, in->a);
        if (integer128(in->type)) {
            fputs("\tldp x9, x10, [x10]\n", out);
            slot_pair(e, 9, 10, in->dest, false);
        } else if (quad(in->type)) {
            fputs("\tldr q0, [x10]\n", out);
            fp_store(e, 0, in->dest);
        } else {
            object_load(out, in->type, 9, 10);
            slot_store(e, 9, in->dest);
        }
        break;
    case I_STORE:
        slot_load(e, 9, in->a);
        if (integer128(in->type)) {
            slot_pair(e, 10, 11, in->b, true);
            fputs("\tstp x10, x11, [x9]\n", out);
        } else if (quad(in->type)) {
            fp_load(e, 0, in->b);
            fputs("\tstr q0, [x9]\n", out);
        } else {
            slot_load(e, 10, in->b);
            if (address_value(in->type))
                memory_loop(e, in->type->size, true);
            else
                object_store(out, in->type, 10, 9);
        }
        break;
    case I_COPY:
        if (integer128(in->type)) {
            slot_pair(e, 9, 10, in->a, true);
            slot_pair(e, 9, 10, in->dest, false);
        } else if (quad(in->type)) {
            fp_load(e, 0, in->a);
            fp_store(e, 0, in->dest);
        } else {
            slot_load(e, 9, in->a);
            slot_store(e, 9, in->dest);
        }
        break;
    case I_BITLOAD:
        slot_load(e, 10, in->a);
        object_load(out, in->type, 9, 10);
        fprintf(out, "\t%sbfx x9, x9, #%u, #%u\n",
                in->type->uns ? "u" : "s", in->bit, in->width);
        slot_store(e, 9, in->dest);
        break;
    case I_BITSTORE:
        slot_load(e, 10, in->a);
        slot_load(e, 11, in->b);
        object_load(out, in->type, 9, 10);
        fprintf(out, "\tbfi x9, x11, #%u, #%u\n", in->bit, in->width);
        object_store(out, in->type, 9, 10);
        break;
    case I_CAST: cast_instruction(e, in); break;
    case I_UNARY:
        if (in->source_type->kind == TY_FLOAT) {
            fp_load(e, 0, in->a);
            char r = fp_width(in->source_type);
            if (quad(in->source_type)) {
                if (in->op == '-') {
                    quad_call(e, "__negtf2", 0, 1);
                    fp_store(e, 0, in->dest);
                    break;
                }
                fputs("\tmovi v1.16b, #0\n", out);
                quad_call(e, "__eqtf2", 0, 2);
                fputs("\tcmp w0, #0\n\tcset w9, eq\n", out);
                slot_store(e, 9, in->dest);
                break;
            }
            if (in->op == '-') {
                fprintf(out, "\tfneg %c0, %c0\n", r, r);
                fp_store(e, 0, in->dest);
                break;
            }
            fprintf(out, "\tfcmp %c0, #0.0\n\tcset w9, eq\n", r);
        } else {
            slot_load(e, 9, in->a);
            if (in->op == '!')
                fputs("\tcmp x9, #0\n\tcset w9, eq\n", out);
            else
                fprintf(out, "\t%s x9, x9\n", in->op == '-' ? "neg" : "mvn");
        }
        normalize(out, in->type, 9);
        slot_store(e, 9, in->dest);
        break;
    case I_BINARY: binary_instruction(e, in); break;
    case I_LABEL:
        fprintf(out, ".L%u_%zu:\n", e->function_id, in->label);
        if (in->symbol && in->symbol->label_address_taken)
            fprintf(out, "%s:\n", in->symbol->assembly);
        break;
    case I_INDIRECT_JUMP:
        slot_load(e, 9, in->a);
        fputs("\tbr x9\n", out);
        break;
    case I_TRAP: fputs("\tbrk #0\n", out); break;
    case I_SPECULATION_BARRIER: fputs("\tdsb sy\n\tisb\n", out); break;
    case I_SETJMP: setjmp_instruction(e, in); break;
    case I_LONGJMP:
        slot_load(e, 9, in->a);
        fputs("\tldp x10, x16, [x9]\n\tldr x9, [x9, #16]\n"
              "\tmov x29, x10\n\tmov sp, x9\n\tbr x16\n", out);
        break;
    case I_PREFETCH: {
        static const char *const locality[] = {"l1strm", "l3keep", "l2keep", "l1keep"};
        slot_load(e, 9, in->a);
        fprintf(out, "\tprfm %s%s, [x9]\n", in->op ? "pst" : "pld", locality[in->immediate]);
        break;
    }
    case I_ASM: fprintf(out, "\t%s\n", in->asm_text); break;
    case I_EXTENDED_ASM: prepare_asm(e, in->asm_plan); break;
    case I_ASM_OUTPUT: finish_asm(e, in->asm_plan); break;
    case I_JUMP:
        fprintf(out, "\tb .L%u_%zu\n", e->function_id, in->label);
        break;
    case I_STACK_SAVE:
        stack_pin_checkpoint(e, in->b, false);
        fputs("\tmov x9, sp\n", out);
        slot_store(e, 9, in->dest);
        break;
    case I_STACK_ALLOC: {
        fputs("\tmov x9, sp\n", out);
        slot_load(e, 10, in->a);
        unsigned align = in->immediate > 16 ? (unsigned)in->immediate : 16;
        fprintf(out, "\tsub x9, x9, x10\n\tand x9, x9, #-%u\n\tmov sp, x9\n", align);
        /* Leave a word at each restorable stack position for the saved alloca pin. */
        if (e->stack_checkpoint_pin)
            fputs("\tsub sp, sp, #16\n", out);
        slot_store(e, 9, in->dest);
        break;
    }
    case I_STACK_RESTORE:
        slot_load(e, 9, in->a);
        fputs("\tmov sp, x9\n", out);
        stack_pin_checkpoint(e, in->b, true);
        break;
    case I_ZERO:
        slot_load(e, 9, in->a);
        fprintf(out, "\t%s x9, .L%u_%zu\n", in->op ? "cbnz" : "cbz", e->function_id, in->label);
        break;
    case I_CALL: call_instruction(e, in); break;
    case I_VA_START: va_start_instruction(e, in); break;
    case I_VA_ARG: va_arg_instruction(e, in); break;
    case I_MEMZERO: case I_MEMCOPY:
        slot_load(e, 9, in->a);
        if (in->code == I_MEMCOPY)
            slot_load(e, 10, in->b);
        memory_loop(e, in->immediate, in->code == I_MEMCOPY);
        break;
    case I_RETURN:
        if (in->a) {
            Type *result = e->ir->function->type->base;
            if (address_value(result)) {
                Argument model = argument_model(result);
                if (model.kind == ARG_INDIRECT) {
                    slot_load(e, 10, in->a);
                    frame_address(e, 14, e->indirect_return);
                    fputs("\tldr x9, [x14]\n", out);
                    if (in->b) {
                        slot_load(e, 12, in->b);
                        memory_loop_from_count(e, true, true);
                    } else {
                        memory_loop(e, result->size, true);
                    }
                } else {
                    slot_load(e, 9, in->a);
                    aggregate_registers(out, result, model, 9, true);
                }
            } else if (result->kind == TY_FLOAT || result->kind == TY_DECIMAL)
                fp_load(e, 0, in->a);
            else if (integer128(result))
                slot_pair(e, 0, 1, in->a, true);
            else
                slot_load(e, 0, in->a);
        }
        fprintf(out, "\tb .Lreturn%u\n", e->function_id);
        break;
    }
}

static void emit_function(Context *c, FILE *out, Symbol *s, unsigned id)
{
    FunctionIR ir;
    if (!lower(c, s, &ir))
        return;
    Emitter e = {.context = c, .out = out, .ir = &ir, .function_id = id};
    e.offsets = alloc(c, (ir.slots.len + 1) * sizeof *e.offsets);
    if (s->has_builtin_setjmp)
        e.jump_registers = asm_call_saved_registers(c) & ~c->global_registers;
    e.stack_checkpoint_pin = has_stack_checkpoints(s) && s->has_stack_alloca;
    uint64_t saved_registers = c->global_register_spills | e.jump_registers;
    size_t frame = asm_register_spill_layout(saved_registers, e.register_spills);
    for (size_t i = 0; i < ir.slots.len; i++)
        e.offsets[i + 1] = layout_frame_slot(&frame, ir.slots.data[i]);
    for (size_t i = 0; i < s->locals.len; i++)
        s->locals.data[i]->frame_offset = e.offsets[s->locals.data[i]->slot];
    if (argument_model(s->type->base).kind == ARG_INDIRECT) {
        frame = align_to(frame, 8) + 8;
        e.indirect_return = frame;
    }
    if (s->type->variadic) {
        e.varargs_gp = align_to(frame, 16) + 64;
        e.varargs_fp = e.varargs_gp + 128;
        frame = e.varargs_fp;
    }
    emit_function_entry(c, out, s, id);
    fputs("\tstp x29, x30, [sp, #-16]!\n\tmov x29, sp\n", out);
    if (e.stack_checkpoint_pin)
        frame += 16;
    frame = align_to(frame, 16);
    if (frame > 4095 && e.register_spills[15]) {
        fputs("\tstr x15, [sp, #-16]!\n", out);
        adjust_stack(out, frame - 16, false);
        fputs("\tldr x15, [x29, #-16]\n", out);
    } else {
        adjust_stack(out, frame, false);
    }
    sync_registers(&e, saved_registers, false);
    if (s->static_chain)
        slot_store(&e, 18, s->static_chain->slot);
    if (e.indirect_return) {
        frame_address(&e, 14, e.indirect_return);
        fputs("\tstr x8, [x14]\n", out);
    }
    if (s->type->variadic) {
        frame_address(&e, 14, e.varargs_gp);
        for (unsigned i = 0; i < 8; i += 2)
            fprintf(out, "\tstp x%u, x%u, [x14, #%u]\n", i, i + 1, i * 8);
        frame_address(&e, 14, e.varargs_fp);
        for (unsigned i = 0; i < 8; i += 2)
            fprintf(out, "\tstp q%u, q%u, [x14, #%u]\n", i, i + 1, i * 16);
    }
    parameters(&e);
    for (size_t i = 0; i < ir.instructions.len; i++)
        instruction(&e, &ir.instructions.data[i]);
    fprintf(out, ".Lreturn%u:\n", e.function_id);
    Argument result = argument_model(s->type->base);
    uint64_t return_registers = 0;
    if (s->type->base->kind != TY_VOID && result.kind != ARG_INDIRECT)
        return_registers = result.kind == ARG_FP ? argument_register_mask(0, result.count) :
                                                 argument_register_mask(result.count, 0);
    sync_register_spills(&e, return_registers, true);
    fputs("\tmov sp, x29\n\tldp x29, x30, [sp], #16\n\tret\n", out);
    fprintf(out, "\t.size %s, .-%s\n", s->assembly, s->assembly);
}

bool emit_aarch64(Context *c, FILE *out)
{
    for (size_t i = 0; i < c->globals.len && !c->failed; i++) {
        Symbol *s = c->globals.data[i];
        emit_symbol_linkage(out, s);
        if (s->alias)
            continue;
        if (s->asm_plan)
            emit_asm_template(c, out, s->asm_plan, 0);
        else if (s->asm_text)
            fprintf(out, "%s\n", s->asm_text);
        else if (s->type->kind == TY_FUNC && s->body)
            emit_function(c, out, s, (unsigned)i);
        else if (s->type->kind != TY_FUNC && !global_register(s) && (s->defined || s->tentative))
            emit_static_data(c, out, s);
    }
    emit_lifecycle_functions(c, out);
    fputs("\t.section .note.GNU-stack,\"\",%progbits\n", out);
    return !c->failed && !ferror(out);
}
