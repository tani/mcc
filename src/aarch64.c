#include "compiler.h"
#include "parser.h"

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

static void frame_address(Emitter *e, unsigned reg, size_t offset)
{
    if (offset <= 4095)
        fprintf(e->out, "\tsub x%u, x29, #%zu\n", reg, offset);
    else {
        number(e->out, 15, offset);
        fprintf(e->out, "\tsub x%u, x29, x15\n", reg);
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

static char fp_width(Type *t)
{
    return t->size == 4 ? 's' : t->size == 8 ? 'd' : 'q';
}

static bool quad(Type *t)
{
    return t->kind == TY_FLOAT && t->size == 16;
}

static void fp_load(Emitter *e, unsigned reg, size_t id)
{
    Type *t = e->ir->slots.data[id - 1];
    frame_address(e, 14, e->offsets[id]);
    fprintf(e->out, "\tldr %c%u, [x14]\n", fp_width(t), reg);
}

static void fp_store(Emitter *e, unsigned reg, size_t id)
{
    Type *t = e->ir->slots.data[id - 1];
    frame_address(e, 14, e->offsets[id]);
    fprintf(e->out, "\tstr %c%u, [x14]\n", fp_width(t), reg);
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
        frame_address(e, 9, e->offsets[symbol->slot]);
    } else if (symbol->thread_local_storage) {
        const char *name = symbol->assembly;
        fprintf(e->out, "\tadrp x0, :tlsdesc:%s\n\tldr x1, [x0, #:tlsdesc_lo12:%s]\n"
                        "\tadd x0, x0, #:tlsdesc_lo12:%s\n\t.tlsdesccall %s\n\tblr x1\n"
                        "\tmrs x9, tpidr_el0\n\tadd x9, x9, x0\n",
                name, name, name, name);
    } else if (symbol->internal) {
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
            case '+': helper = "addtf3"; break;
            case '-': helper = "subtf3"; break;
            case '*': helper = "multf3"; break;
            case '/': helper = "divtf3"; break;
            case '<': case LE: helper = "letf2"; break;
            case '>': case GE: helper = "getf2"; break;
            default: helper = "eqtf2"; break;
            }
            fprintf(out, "\tbl __%s\n", helper);
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
    char from = fp_width(a), to = fp_width(b);
    if (a->kind == TY_FLOAT) {
        fp_load(e, 0, in->a);
        if (b->kind == TY_FLOAT) {
            if (quad(a) && !quad(b))
                fprintf(out, "\tbl __trunctf%sf2\n", to == 's' ? "s" : "d");
            else if (!quad(a) && quad(b))
                fprintf(out, "\tbl __extend%sftf2\n", from == 's' ? "s" : "d");
            else if (from != to)
                fprintf(out, "\tfcvt %c0, %c0\n", to, from);
            fp_store(e, 0, in->dest);
            return;
        }
        if (quad(a) && b->kind == TY_BOOL)
            fputs("\tmovi v1.16b, #0\n\tbl __eqtf2\n\tcmp w0, #0\n\tcset w9, ne\n", out);
        else if (quad(a))
            fprintf(out, "\tbl __fix%stfdi\n\tmov x9, x0\n", b->uns ? "uns" : "");
        else if (b->kind == TY_BOOL)
            fprintf(out, "\tfcmp %c0, #0.0\n\tcset w9, ne\n", from);
        else
            fprintf(out, "\tfcvtz%c x9, %c0\n", b->uns ? 'u' : 's', from);
    } else {
        slot_load(e, 9, in->a);
        if (b->kind == TY_FLOAT) {
            if (quad(b))
                fprintf(out, "\tmov x0, x9\n\tbl __float%sditf\n", a->uns ? "un" : "");
            else
                fprintf(out, "\t%ccvtf %c0, x9\n", a->uns ? 'u' : 's', to);
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
    bool stack;
    unsigned reg;
    size_t offset;
} Argument;
typedef struct { unsigned gp, fp; size_t stack; } ArgumentState;

static bool hfa(Type *t, size_t offset, Argument *a)
{
    if (t->kind == TY_FLOAT) {
        if (a->count == 4 || (a->count && a->element != t->size))
            return false;
        a->element = t->size;
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
        for (size_t j = 0; j < t->members.len; j++) {
            Member m = t->members.data[j];
            if (!hfa(m.type, offset + m.offset, a))
                return false;
        }
        return true;
    }
    if (t->kind == TY_UNION && t->members.len) {
        Argument largest = {0};
        for (size_t j = 0; j < t->members.len; j++) {
            Argument branch = {0};
            if (!hfa(t->members.data[j].type, offset, &branch) ||
                (largest.count && largest.element != branch.element))
                return false;
            if (branch.count > largest.count)
                largest = branch;
        }
        if (largest.count > 4 - a->count || (a->count && a->element != largest.element))
            return false;
        a->element = largest.element;
        for (unsigned j = 0; j < largest.count; j++)
            a->offsets[a->count++] = largest.offsets[j];
        return true;
    }
    return false;
}

static Argument argument_model(Type *t)
{
    Argument a = {.kind = ARG_FP};
    if (hfa(t, 0, &a))
        return a;
    if (aggregate(t) && t->size > 16)
        return (Argument){.kind = ARG_INDIRECT, .count = 1};
    return (Argument){.kind = ARG_GP, .count = aggregate(t) ? (t->size + 7) / 8 : 1};
}

static Argument classify(ArgumentState *state, Type *t)
{
    Argument a = argument_model(t);
    unsigned *next = a.kind == ARG_FP ? &state->fp : &state->gp;
    size_t alignment = a.kind == ARG_INDIRECT ? 8 : t->align > 8 ? t->align : 8;
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
        char width = a.element == 4 ? 's' : a.element == 8 ? 'd' : 'q';
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
        Type *t = param->type;
        Argument place = classify(&state, t);
        frame_address(e, 14, e->offsets[param->slot]);
        if (aggregate(t)) {
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
            if (quad(t))
                fputs("\tldr q16, [x13]\n\tstr q16, [x14]\n", e->out);
            else {
                object_load(e->out, t, 9, 13);
                object_store(e->out, t, 9, 14);
            }
        } else if (t->kind == TY_FLOAT) {
            fprintf(e->out, "\tstr %c%u, [x14]\n", fp_width(t), place.reg);
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
    Argument model = argument_model(t);
    bool fp = model.kind == ARG_FP, indirect = model.kind == ARG_INDIRECT;
    unsigned idx = e->next_label++;
    FILE *out = e->out;
    unsigned reg_offset = fp ? 28 : 24;
    unsigned increment = model.count * (fp ? 16 : 8);
    size_t size = indirect ? 8 : align_to(t->size, 8);
    bool aligned = !indirect && t->align > 8;
    slot_load(e, 9, in->a);
    fprintf(out, "\tldrsw x10, [x9, #%u]\n\ttbz x10, #63, .Lvstack%u_%u\n",
            reg_offset, e->function_id, idx);
    if (!fp && aligned)
        fputs("\tadd x10, x10, #15\n\tand x10, x10, #-16\n", out);
    fprintf(out, "\tadds w11, w10, #%u\n\tstr w11, [x9, #%u]\n\tb.gt .Lvstack%u_%u\n"
                 "\tldr x11, [x9, #%u]\n\tadd x10, x11, x10\n",
            increment, reg_offset, e->function_id, idx, fp ? 16u : 8u);
    if (fp && aggregate(t)) {
        slot_load(e, 9, in->dest);
        char width = model.element == 4 ? 's' : model.element == 8 ? 'd' : 'q';
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
    if (aggregate(t)) {
        if (indirect)
            fputs("\tldr x10, [x10]\n", out);
        slot_load(e, 9, in->dest);
        memory_loop(e, t->size, true);
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
    for (size_t i = 0; i < in->args.len; i++) {
        size_t id = in->args.data[i];
        Type *t = in->arg_types.data[i];
        Argument place = classify(&state, t);
        if (place.stack) {
            stack_address(e->out, false, place.offset);
            if (place.kind == ARG_INDIRECT) {
                slot_load(e, 9, id);
                fputs("\tstr x9, [x13]\n", e->out);
            } else if (aggregate(t)) {
                slot_load(e, 10, id);
                fputs("\tmov x9, x13\n", e->out);
                memory_loop(e, t->size, true);
            } else if (quad(t)) {
                fp_load(e, 16, id);
                fputs("\tstr q16, [x13]\n", e->out);
            } else {
                slot_load(e, 9, id);
                object_store(e->out, t, 9, 13);
            }
        } else if (place.kind == ARG_INDIRECT) {
            slot_load(e, place.reg, id);
        } else if (aggregate(t)) {
            slot_load(e, 9, id);
            aggregate_registers(e->out, t, place, 9, true);
        } else if (t->kind == TY_FLOAT) {
            fp_load(e, place.reg, id);
        } else {
            slot_load(e, place.reg, id);
        }
    }
    Argument result = argument_model(in->type);
    if (result.kind == ARG_INDIRECT)
        slot_load(e, 8, in->dest);
    if (in->symbol)
        fprintf(e->out, "\tbl %s\n", in->symbol->assembly);
    else {
        slot_load(e, 16, in->a);
        fputs("\tblr x16\n", e->out);
    }
    adjust_stack(e->out, outgoing, true);
    if (aggregate(in->type)) {
        if (result.kind != ARG_INDIRECT) {
            slot_load(e, 9, in->dest);
            aggregate_registers(e->out, in->type, result, 9, false);
        }
    } else if (in->type->kind == TY_FLOAT) {
        fp_store(e, 0, in->dest);
    } else if (in->dest) {
        normalize(e->out, in->type, 0);
        slot_store(e, 0, in->dest);
    }
}

static void memory_loop(Emitter *e, size_t size, bool copy)
{
    if (!size)
        return;
    unsigned id = e->next_label++;
    number(e->out, 12, size);
    fprintf(e->out, ".Lm%u_%u:\n", e->function_id, id);
    if (copy)
        fputs("\tldrb w11, [x10], #1\n\tstrb w11, [x9], #1\n", e->out);
    else
        fputs("\tstrb wzr, [x9], #1\n", e->out);
    fprintf(e->out, "\tsubs x12, x12, #1\n\tb.ne .Lm%u_%u\n", e->function_id, id);
}

static void instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    switch (in->code) {
    case I_IMMEDIATE:
        number(out, 9, in->immediate);
        normalize(out, in->type, 9);
        slot_store(e, 9, in->dest);
        break;
    case I_REAL:
        if (quad(in->type)) {
            uint64_t bits[2];
            if (extended_float_bits(e->context, in->real, bits, in->loc)) {
                number(out, 9, bits[0]);
                number(out, 10, bits[1]);
                frame_address(e, 14, e->offsets[in->dest]);
                fputs("\tstp x9, x10, [x14]\n", out);
            }
        } else {
            number(out, 9, float_bits(in->real, in->type->size));
            slot_store(e, 9, in->dest);
        }
        break;
    case I_ADDRESS:
        if (in->symbol)
            object_address(e, in->symbol);
        else
            frame_address(e, 9, e->offsets[in->a]);
        slot_store(e, 9, in->dest);
        break;
    case I_LOAD:
        slot_load(e, 10, in->a);
        if (quad(in->type)) {
            fputs("\tldr q0, [x10]\n", out);
            fp_store(e, 0, in->dest);
        } else {
            object_load(out, in->type, 9, 10);
            slot_store(e, 9, in->dest);
        }
        break;
    case I_STORE:
        slot_load(e, 9, in->a);
        if (quad(in->type)) {
            fp_load(e, 0, in->b);
            fputs("\tstr q0, [x9]\n", out);
        } else {
            slot_load(e, 10, in->b);
            if (aggregate(in->type))
                memory_loop(e, in->type->size, true);
            else
                object_store(out, in->type, 10, 9);
        }
        break;
    case I_COPY:
        if (quad(in->type)) {
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
                    fputs("\tbl __negtf2\n", out);
                    fp_store(e, 0, in->dest);
                    break;
                }
                fputs("\tmovi v1.16b, #0\n\tbl __eqtf2\n\tcmp w0, #0\n\tcset w9, eq\n", out);
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
    case I_LABEL: fprintf(out, ".L%u_%zu:\n", e->function_id, in->label); break;
    case I_JUMP:
        if (in->a) {
            slot_load(e, 9, in->a);
            fputs("\tmov sp, x9\n", out);
        }
        fprintf(out, "\tb .L%u_%zu\n", e->function_id, in->label);
        break;
    case I_STACK_ALLOC:
        fputs("\tmov x9, sp\n", out);
        slot_store(e, 9, in->b);
        slot_load(e, 10, in->a);
        fputs("\tadd x10, x10, #15\n\tand x10, x10, #-16\n\tsub sp, sp, x10\n\tmov x9, sp\n", out);
        slot_store(e, 9, in->dest);
        break;
    case I_STACK_RESTORE:
        slot_load(e, 9, in->a);
        fputs("\tmov sp, x9\n", out);
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
            if (aggregate(result)) {
                Argument model = argument_model(result);
                if (model.kind == ARG_INDIRECT) {
                    slot_load(e, 10, in->a);
                    frame_address(e, 14, e->indirect_return);
                    fputs("\tldr x9, [x14]\n", out);
                    memory_loop(e, result->size, true);
                } else {
                    slot_load(e, 9, in->a);
                    aggregate_registers(out, result, model, 9, true);
                }
            } else if (result->kind == TY_FLOAT)
                fp_load(e, 0, in->a);
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
    size_t frame = 0;
    for (size_t i = 0; i < ir.slots.len; i++) {
        Type *t = ir.slots.data[i];
        size_t size = t->size < 8 ? 8 : t->size;
        size_t alignment = t->align < 8 ? 8 : t->align;
        frame = align_to(frame + size, alignment);
        e.offsets[i + 1] = frame;
    }
    if (argument_model(s->type->base).kind == ARG_INDIRECT) {
        frame = align_to(frame, 8) + 8;
        e.indirect_return = frame;
    }
    if (s->type->variadic) {
        e.varargs_gp = align_to(frame, 16) + 64;
        e.varargs_fp = e.varargs_gp + 128;
        frame = e.varargs_fp;
    }
    fprintf(out, "\t.text\n\t.p2align 2\n");
    if (!s->internal)
        fprintf(out, "\t.global %s\n", s->assembly);
    fprintf(out, "\t.type %s, %%function\n%s:\n", s->assembly, s->assembly);
    fputs("\tstp x29, x30, [sp, #-16]!\n\tmov x29, sp\n", out);
    adjust_stack(out, align_to(frame, 16), false);
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
    fprintf(out, ".Lreturn%u:\n\tmov sp, x29\n\tldp x29, x30, [sp], #16\n\tret\n",
            e.function_id);
    fprintf(out, "\t.size %s, .-%s\n", s->assembly, s->assembly);
}

bool emit_aarch64(Context *c, FILE *out)
{
    for (size_t i = 0; i < c->globals.len && !c->failed; i++) {
        Symbol *s = c->globals.data[i];
        if (s->type->kind == TY_FUNC && s->body)
            emit_function(c, out, s, (unsigned)i);
        else if (s->type->kind != TY_FUNC && (s->defined || s->tentative))
            emit_static_data(c, out, s);
    }
    fputs("\t.section .note.GNU-stack,\"\",%progbits\n", out);
    return !c->failed && !ferror(out);
}
