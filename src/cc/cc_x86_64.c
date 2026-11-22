#include "compiler.h"
#include "parser.h"

#include <inttypes.h>
#include <limits.h>

const Target x86_64_linux = {
    .arch = "x86_64", .display_name = "x86-64", .emit = emit_x86_64,
    .char_size = 1, .short_size = 2, .int_size = 4, .long_size = 8,
    .long_long_size = 8, .pointer_size = 8, .long_double_size = 16
};

typedef enum { ABI_NONE, ABI_GP, ABI_SSE, ABI_X87, ABI_X87_UP, ABI_MEMORY } AbiClass;
typedef struct { AbiClass part[2]; unsigned gp, sse; bool memory; } Abi;

static AbiClass merge_class(AbiClass a, AbiClass b)
{
    if (a == b || b == ABI_NONE)
        return a;
    if (a == ABI_NONE)
        return b;
    if (a == ABI_MEMORY || b == ABI_MEMORY)
        return ABI_MEMORY;
    if (a == ABI_GP || b == ABI_GP)
        return ABI_GP;
    if (a == ABI_X87 || b == ABI_X87 || a == ABI_X87_UP || b == ABI_X87_UP)
        return ABI_MEMORY;
    return ABI_SSE;
}

static bool classify_parts(Type *t, size_t offset, Abi *abi)
{
    if (offset % t->align || offset + t->size > 16)
        return false;
    if (t->kind == TY_ARRAY) {
        for (size_t i = 0; i < t->length; i++)
            if (!classify_parts(t->base, offset + i * t->base->size, abi))
                return false;
    } else if (aggregate(t)) {
        for (size_t i = 0; i < t->members.len; i++) {
            Member m = t->members.data[i];
            if (m.bit_field) {
                if (m.width) {
                    size_t bit = (offset + m.offset) * 8 + m.bit;
                    for (size_t n = bit / 64; n <= (bit + m.width - 1) / 64; n++)
                        abi->part[n] = merge_class(abi->part[n], ABI_GP);
                }
            } else if (!classify_parts(m.type, offset + m.offset, abi)) {
                return false;
            }
        }
    } else if (t->kind != TY_VOID) {
        size_t part = offset / 8;
        AbiClass kind = t->kind != TY_FLOAT ? ABI_GP : t->size == 16 ? ABI_X87 : ABI_SSE;
        abi->part[part] = merge_class(abi->part[part], kind);
        if (kind == ABI_X87)
            abi->part[part + 1] = merge_class(abi->part[part + 1], ABI_X87_UP);
    }
    return true;
}

static Abi classify(Type *t)
{
    Abi abi = {0};
    if (!classify_parts(t, 0, &abi) || abi.part[0] == ABI_MEMORY || abi.part[1] == ABI_MEMORY ||
        (abi.part[1] == ABI_X87_UP && abi.part[0] != ABI_X87))
        return (Abi){.memory = true};
    for (size_t i = 0; i < 2; i++) {
        abi.gp += abi.part[i] == ABI_GP;
        abi.sse += abi.part[i] == ABI_SSE;
    }
    return abi;
}

enum { GP_COUNT = 6, SSE_COUNT = 8, GP_SAVE_SIZE = 8 * GP_COUNT,
       REGISTER_SAVE_SIZE = GP_SAVE_SIZE + 16 * SSE_COUNT };

typedef struct { unsigned gp, sse; size_t stack; } ArgumentState;
typedef struct { Abi abi; unsigned gp, sse; size_t offset; bool stack; } Argument;

static Argument argument(ArgumentState *state, Type *t)
{
    Abi abi = classify(t);
    Argument a = {.abi = abi};
    if (!abi.memory && abi.part[0] != ABI_X87 &&
        abi.gp <= GP_COUNT - state->gp && abi.sse <= SSE_COUNT - state->sse) {
        a.gp = state->gp;
        a.sse = state->sse;
        state->gp += abi.gp;
        state->sse += abi.sse;
    } else {
        a.stack = true;
        a.offset = align_to(state->stack, t->align > 8 ? t->align : 8);
        state->stack = a.offset + align_to(t->size, 8);
    }
    return a;
}

typedef struct { const char *q, *l, *w, *b; } Reg;
static const Reg argument_regs[] = {
    {"%rdi", "%edi", "%di", "%dil"}, {"%rsi", "%esi", "%si", "%sil"},
    {"%rdx", "%edx", "%dx", "%dl"}, {"%rcx", "%ecx", "%cx", "%cl"},
    {"%r8", "%r8d", "%r8w", "%r8b"}, {"%r9", "%r9d", "%r9w", "%r9b"}
};
static const Reg result_regs[] = {
    {"%rax", "%eax", "%ax", "%al"}, {"%rdx", "%edx", "%dx", "%dl"}
};

typedef struct {
    Context *context;
    FILE *out;
    size_t *offsets;
    unsigned function_id, next_label;
    size_t indirect_result, registers, scratch;
    ArgumentState named;
} Emitter;

static void number(FILE *out, uint64_t value, const char *reg)
{
    fprintf(out, "\tmovabsq $%" PRIu64 ", %s\n", value, reg);
}

static void slot_load(Emitter *e, size_t id, const char *reg)
{
    fprintf(e->out, "\tmovq -%zu(%%rbp), %s\n", e->offsets[id], reg);
}

static void slot_store(Emitter *e, size_t id, const char *reg)
{
    fprintf(e->out, "\tmovq %s, -%zu(%%rbp)\n", reg, e->offsets[id]);
}

static bool extended(Type *t)
{
    return t->kind == TY_FLOAT && t->size == 16;
}

static char fp_suffix(Type *t)
{
    return t->size == 4 ? 's' : t->size == 8 ? 'l' : 't';
}

static const char *sse_suffix(Type *t)
{
    return t->size == 4 ? "ss" : "sd";
}

static void x87_load(Emitter *e, Type *t, size_t id)
{
    fprintf(e->out, "\tfld%c -%zu(%%rbp)\n", fp_suffix(t), e->offsets[id]);
}

static void x87_store(Emitter *e, Type *t, size_t id)
{
    fprintf(e->out, "\tfstp%c -%zu(%%rbp)\n", fp_suffix(t), e->offsets[id]);
}

static void normalize(FILE *out, Type *t)
{
    if (t->kind == TY_BOOL)
        fputs("\ttestq %rax, %rax\n\tsetne %al\n\tmovzbl %al, %eax\n", out);
    else if (t->kind == TY_INT && t->size == 1)
        fputs(t->uns ? "\tmovzbl %al, %eax\n" : "\tmovsbq %al, %rax\n", out);
    else if (t->kind == TY_INT && t->size == 2)
        fputs(t->uns ? "\tmovzwl %ax, %eax\n" : "\tmovswq %ax, %rax\n", out);
    else if (t->kind == TY_INT && t->size == 4)
        fputs(t->uns ? "\tmovl %eax, %eax\n" : "\tmovslq %eax, %rax\n", out);
}

static void object_load(FILE *out, Type *t, const char *address)
{
    const char *op = t->size == 1 ? (t->uns ? "movzbl" : "movsbq") :
                     t->size == 2 ? (t->uns ? "movzwl" : "movswq") :
                     t->size == 4 ? (t->kind != TY_INT || t->uns ? "movl" : "movslq") : "movq";
    bool eax = (t->size == 4 && (t->kind != TY_INT || t->uns)) ||
               (t->size < 4 && t->uns);
    fprintf(out, "\t%s (%s), %s\n", op, address, eax ? "%eax" : "%rax");
}

static void object_store(FILE *out, Type *t, const char *address)
{
    const Reg *r = &result_regs[0];
    char width = t->size == 1 ? 'b' : t->size == 2 ? 'w' : t->size == 4 ? 'l' : 'q';
    const char *reg = t->size == 1 ? r->b : t->size == 2 ? r->w : t->size == 4 ? r->l : r->q;
    fprintf(out, "\tmov%c %s, (%s)\n", width, reg, address);
}

static void load_chunk(FILE *out, Reg reg, const char *base, ptrdiff_t offset, unsigned size)
{
    if (size == 8)
        fprintf(out, "\tmovq %td(%s), %s\n", offset, base, reg.q);
    else if (size == 4)
        fprintf(out, "\tmovl %td(%s), %s\n", offset, base, reg.l);
    else if (size == 2)
        fprintf(out, "\tmovzwl %td(%s), %s\n", offset, base, reg.l);
    else if (size == 1)
        fprintf(out, "\tmovzbl %td(%s), %s\n", offset, base, reg.l);
    else {
        fprintf(out, "\txorl %s, %s\n", reg.l, reg.l);
        for (unsigned i = size; i > 0; i--)
            fprintf(out, "\tshlq $8, %s\n\tmovb %td(%s), %s\n",
                    reg.q, offset + (ptrdiff_t)(i - 1), base, reg.b);
    }
}

static void store_chunk(FILE *out, Reg reg, const char *base, ptrdiff_t offset, unsigned size)
{
    if (size == 8)
        fprintf(out, "\tmovq %s, %td(%s)\n", reg.q, offset, base);
    else if (size == 4)
        fprintf(out, "\tmovl %s, %td(%s)\n", reg.l, offset, base);
    else if (size == 2)
        fprintf(out, "\tmovw %s, %td(%s)\n", reg.w, offset, base);
    else if (size == 1)
        fprintf(out, "\tmovb %s, %td(%s)\n", reg.b, offset, base);
    else {
        fprintf(out, "\tmovq %s, %%r11\n", reg.q);
        for (unsigned i = 0; i < size; i++)
            fprintf(out, "\tmovb %%r11b, %td(%s)\n\tshrq $8, %%r11\n",
                    offset + (ptrdiff_t)i, base);
    }
}

static unsigned chunk_size(Type *t, unsigned part)
{
    unsigned remaining = t->size - part * 8;
    return remaining < 8 ? remaining : 8;
}

static void register_value(FILE *out, Type *t, Abi abi, const Reg *gp, unsigned sse,
                           const char *base, ptrdiff_t offset, bool load)
{
    if (abi.part[0] == ABI_X87) {
        fprintf(out, "\t%s %td(%s)\n", load ? "fldt" : "fstpt", offset, base);
        return;
    }
    for (unsigned i = 0; i < 2; i++, offset += 8) {
        if (abi.part[i] == ABI_NONE)
            continue;
        unsigned size = chunk_size(t, i);
        if (abi.part[i] == ABI_SSE) {
            char width = size == 4 ? 'd' : 'q';
            if (load)
                fprintf(out, "\tmov%c %td(%s), %%xmm%u\n", width, offset, base, sse);
            else
                fprintf(out, "\tmov%c %%xmm%u, %td(%s)\n", width, sse, offset, base);
            sse++;
        } else {
            if (load)
                load_chunk(out, *gp, base, offset, size);
            else
                store_chunk(out, *gp, base, offset, size);
            gp++;
        }
    }
}

static void copy_bytes(FILE *out, size_t size)
{
    number(out, size, "%rcx");
    fputs("\trep movsb\n", out);
}

static void object_address(Emitter *e, Symbol *s)
{
    FILE *out = e->out;
    if (s->local) {
        fprintf(out, "\tleaq -%zu(%%rbp), %%rax\n", e->offsets[s->slot]);
    } else if (s->thread_local_storage) {
        /* The GNU TLSGD sequence also lets the linker relax executable-local TLS. */
        fprintf(out, "\tdata16 leaq %s@tlsgd(%%rip), %%rdi\n"
                     "\t.value 0x6666\n\trex64\n\tcall __tls_get_addr@PLT\n", s->assembly);
    } else if (s->internal) {
        fprintf(out, "\tleaq %s(%%rip), %%rax\n", s->assembly);
    } else {
        fprintf(out, "\tmovq %s@GOTPCREL(%%rip), %%rax\n", s->assembly);
    }
}

static const char *integer_condition(int op, bool uns)
{
    switch (op) {
    case EQ: return "e";
    case NE: return "ne";
    case '<': return uns ? "b" : "l";
    case '>': return uns ? "a" : "g";
    case LE: return uns ? "be" : "le";
    case GE: return uns ? "ae" : "ge";
    default: return NULL;
    }
}

static void comparison(FILE *out, const char *condition)
{
    fprintf(out, "\tset%s %%al\n\tmovzbl %%al, %%eax\n", condition);
}

static void fp_comparison(FILE *out, int op)
{
    const char *condition = integer_condition(op, true);
    fprintf(out, "\tset%s %%al\n", condition);
    if (op == NE)
        fputs("\tsetp %dl\n\torb %dl, %al\n", out);
    else if (op == EQ || op == '<' || op == LE)
        fputs("\tsetnp %dl\n\tandb %dl, %al\n", out);
    fputs("\tmovzbl %al, %eax\n", out);
}

static void fp_binary(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    Type *t = in->source_type;
    bool compare = integer_condition(in->op, true) != NULL;
    const char *op = in->op == '+' ? "add" : in->op == '-' ? "sub" :
                     in->op == '*' ? "mul" : "div";
    if (extended(t)) {
        if (compare) {
            x87_load(e, t, in->b);
            x87_load(e, t, in->a);
            fputs("\tfucomip %st(1), %st\n\tfstp %st(0)\n", out);
        } else {
            x87_load(e, t, in->a);
            x87_load(e, t, in->b);
            const char *ordered = in->op == '-' ? "subr" : in->op == '/' ? "divr" : op;
            fprintf(out, "\tf%sp %%st, %%st(1)\n", ordered);
            x87_store(e, in->type, in->dest);
        }
    } else {
        const char *suffix = sse_suffix(t);
        fprintf(out, "\tmov%s -%zu(%%rbp), %%xmm0\n", suffix, e->offsets[in->a]);
        if (compare) {
            fprintf(out, "\tucomi%s -%zu(%%rbp), %%xmm0\n", suffix, e->offsets[in->b]);
        } else {
            fprintf(out, "\t%s%s -%zu(%%rbp), %%xmm0\n"
                         "\tmov%s %%xmm0, -%zu(%%rbp)\n",
                    op, suffix, e->offsets[in->b], suffix, e->offsets[in->dest]);
        }
    }
    if (compare) {
        fp_comparison(out, in->op);
        slot_store(e, in->dest, "%rax");
    }
}

static void integer_binary(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    slot_load(e, in->a, "%rax");
    slot_load(e, in->b, "%rcx");
    bool uns = in->source_type->uns;
    const char *condition = integer_condition(in->op, uns);
    if (condition) {
        fputs("\tcmpq %rcx, %rax\n", out);
        comparison(out, condition);
    } else {
        switch (in->op) {
        case '+': fputs("\taddq %rcx, %rax\n", out); break;
        case '-': fputs("\tsubq %rcx, %rax\n", out); break;
        case '*': fputs("\timulq %rcx, %rax\n", out); break;
        case '&': fputs("\tandq %rcx, %rax\n", out); break;
        case '|': fputs("\torq %rcx, %rax\n", out); break;
        case '^': fputs("\txorq %rcx, %rax\n", out); break;
        case SHL: fputs("\tshlq %cl, %rax\n", out); break;
        case SHR: fputs(uns ? "\tshrq %cl, %rax\n" : "\tsarq %cl, %rax\n", out); break;
        case '/': case '%':
            fputs(uns ? "\txorl %edx, %edx\n\tdivq %rcx\n" :
                        "\tcqto\n\tidivq %rcx\n", out);
            if (in->op == '%')
                fputs("\tmovq %rdx, %rax\n", out);
            break;
        }
    }
    normalize(out, in->type);
    slot_store(e, in->dest, "%rax");
}

static void fp_zero_comparison(Emitter *e, Type *t, size_t id, int op)
{
    fputs("\tfldz\n", e->out);
    x87_load(e, t, id);
    fputs("\tfucomip %st(1), %st\n\tfstp %st(0)\n", e->out);
    fp_comparison(e->out, op);
}

static void fp_to_integer(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    bool unsigned_wide = in->type->uns && in->type->size == 8;
    x87_load(e, in->source_type, in->a);
    if (unsigned_wide) {
        unsigned label = e->next_label++;
        fputs("\txorl %r10d, %r10d\n\tfldl .Ltwo63(%rip)\n"
              "\tfucomip %st(1), %st\n", out);
        fprintf(out, "\tja .Lcast%u_%u\n\tfsubl .Ltwo63(%%rip)\n",
                e->function_id, label);
        number(out, UINT64_C(1) << 63, "%r10");
        fprintf(out, ".Lcast%u_%u:\n", e->function_id, label);
    }
    /* x86-64 baseline has x87 but does not require SSE3's truncating FISTTP. */
    fprintf(out, "\tfnstcw -%zu(%%rbp)\n\tmovzwl -%zu(%%rbp), %%eax\n"
                 "\torw $3072, %%ax\n\tmovw %%ax, -%zu(%%rbp)\n"
                 "\tfldcw -%zu(%%rbp)\n\tfistpq -%zu(%%rbp)\n"
                 "\tfldcw -%zu(%%rbp)\n",
            e->scratch, e->scratch, e->scratch - 2, e->scratch - 2,
            e->offsets[in->dest], e->scratch);
    slot_load(e, in->dest, "%rax");
    if (unsigned_wide)
        fputs("\torq %r10, %rax\n", out);
    normalize(out, in->type);
    slot_store(e, in->dest, "%rax");
}

static void cast_instruction(Emitter *e, Inst *in)
{
    Type *from = in->source_type, *to = in->type;
    FILE *out = e->out;
    if (from->kind == TY_FLOAT) {
        if (to->kind == TY_FLOAT) {
            x87_load(e, from, in->a);
            x87_store(e, to, in->dest);
        } else if (to->kind == TY_BOOL) {
            fp_zero_comparison(e, from, in->a, NE);
            slot_store(e, in->dest, "%rax");
        } else {
            fp_to_integer(e, in);
        }
    } else if (to->kind == TY_FLOAT) {
        fprintf(out, "\tfildq -%zu(%%rbp)\n", e->offsets[in->a]);
        if (from->uns && from->size == 8) {
            unsigned label = e->next_label++;
            slot_load(e, in->a, "%rax");
            fprintf(out, "\ttestq %%rax, %%rax\n\tjns .Lcast%u_%u\n"
                         "\tfaddl .Ltwo64(%%rip)\n.Lcast%u_%u:\n",
                    e->function_id, label, e->function_id, label);
        }
        x87_store(e, to, in->dest);
    } else {
        slot_load(e, in->a, "%rax");
        normalize(out, to);
        slot_store(e, in->dest, "%rax");
    }
}

static void unary_value(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    Type *t = in->source_type;
    if (t->kind == TY_FLOAT) {
        if (in->op == '-') {
            x87_load(e, t, in->a);
            fputs("\tfchs\n", out);
            x87_store(e, in->type, in->dest);
            return;
        }
        fp_zero_comparison(e, t, in->a, EQ);
    } else {
        slot_load(e, in->a, "%rax");
        if (in->op == '!')
            fputs("\ttestq %rax, %rax\n\tsete %al\n\tmovzbl %al, %eax\n", out);
        else
            fputs(in->op == '-' ? "\tnegq %rax\n" : "\tnotq %rax\n", out);
        normalize(out, in->type);
    }
    slot_store(e, in->dest, "%rax");
}

static uint64_t low_mask(unsigned width)
{
    return UINT64_MAX >> (64 - width);
}

static void bit_load(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    slot_load(e, in->a, "%r10");
    object_load(out, in->type, "%r10");
    unsigned left = 64 - in->bit - in->width, right = 64 - in->width;
    if (left)
        fprintf(out, "\tshlq $%u, %%rax\n", left);
    if (right)
        fprintf(out, "\t%s $%u, %%rax\n", in->type->uns ? "shrq" : "sarq", right);
    slot_store(e, in->dest, "%rax");
}

static void bit_store(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    slot_load(e, in->a, "%r10");
    object_load(out, in->type, "%r10");
    uint64_t mask = low_mask(in->width);
    number(out, ~(mask << in->bit), "%r11");
    fputs("\tandq %r11, %rax\n", out);
    slot_load(e, in->b, "%rdx");
    number(out, mask, "%r11");
    fputs("\tandq %r11, %rdx\n", out);
    if (in->bit)
        fprintf(out, "\tshlq $%u, %%rdx\n", in->bit);
    fputs("\torq %rdx, %rax\n", out);
    object_store(out, in->type, "%r10");
}

static void save_arguments(Emitter *e)
{
    FILE *out = e->out;
    for (unsigned i = 0; i < GP_COUNT; i++)
        fprintf(out, "\tmovq %s, %td(%%rbp)\n", argument_regs[i].q,
                (ptrdiff_t)(i * 8) - (ptrdiff_t)e->registers);
    for (unsigned i = 0; i < SSE_COUNT; i++)
        fprintf(out, "\tmovaps %%xmm%u, %td(%%rbp)\n", i,
                (ptrdiff_t)(GP_SAVE_SIZE + i * 16) - (ptrdiff_t)e->registers);
}

static void parameters(Emitter *e, Symbol *s)
{
    FILE *out = e->out;
    ArgumentState state = {.gp = e->indirect_result ? 1u : 0u};
    if (e->indirect_result)
        fprintf(out, "\tmovq %%rdi, -%zu(%%rbp)\n", e->indirect_result);
    if (s->type->variadic)
        save_arguments(e);
    /* Stack copies use argument registers as scratch, so save register arguments first. */
    for (size_t i = 0; i < s->params.len; i++) {
        Symbol *p = s->params.data[i];
        Argument a = argument(&state, p->type);
        if (!a.stack)
            register_value(out, p->type, a.abi, argument_regs + a.gp, a.sse,
                           "%rbp", -(ptrdiff_t)e->offsets[p->slot], false);
    }
    e->named = state;
    state = (ArgumentState){.gp = e->indirect_result ? 1u : 0u};
    for (size_t i = 0; i < s->params.len; i++) {
        Symbol *p = s->params.data[i];
        Argument a = argument(&state, p->type);
        if (a.stack) {
            fprintf(out, "\tleaq %zu(%%rbp), %%rsi\n\tleaq -%zu(%%rbp), %%rdi\n",
                    a.offset + 16, e->offsets[p->slot]);
            copy_bytes(out, p->type->size);
        }
    }
}

static void call_instruction(Emitter *e, Inst *in)
{
    FILE *out = e->out;
    Abi result = classify(in->type);
    ArgumentState initial = {.gp = result.memory ? 1u : 0u}, state = initial;
    for (size_t i = 0; i < in->args.len; i++)
        argument(&state, in->arg_types.data[i]);
    size_t stack = align_to(state.stack, 16);
    if (stack > INT32_MAX) {
        error(e->context, in->loc, "outgoing x86-64 arguments are too large");
        return;
    }
    if (stack)
        fprintf(out, "\tsubq $%zu, %%rsp\n", stack);
    /* Copy stack arguments before filling the argument registers. */
    state = initial;
    for (size_t i = 0; i < in->args.len; i++) {
        Type *t = in->arg_types.data[i];
        Argument a = argument(&state, t);
        if (!a.stack)
            continue;
        size_t slot = in->args.data[i];
        if (aggregate(t) || extended(t)) {
            if (aggregate(t))
                slot_load(e, slot, "%rsi");
            else
                fprintf(out, "\tleaq -%zu(%%rbp), %%rsi\n", e->offsets[slot]);
            fprintf(out, "\tleaq %zu(%%rsp), %%rdi\n", a.offset);
            copy_bytes(out, t->size);
        } else {
            slot_load(e, slot, "%rax");
            fprintf(out, "\tmovq %%rax, %zu(%%rsp)\n", a.offset);
        }
    }
    state = initial;
    for (size_t i = 0; i < in->args.len; i++) {
        Type *t = in->arg_types.data[i];
        Argument a = argument(&state, t);
        if (a.stack)
            continue;
        size_t slot = in->args.data[i];
        if (aggregate(t)) {
            slot_load(e, slot, "%r10");
            register_value(out, t, a.abi, argument_regs + a.gp, a.sse, "%r10", 0, true);
        } else {
            register_value(out, t, a.abi, argument_regs + a.gp, a.sse,
                           "%rbp", -(ptrdiff_t)e->offsets[slot], true);
        }
    }
    if (result.memory)
        slot_load(e, in->dest, "%rdi");
    fprintf(out, "\tmovl $%u, %%eax\n", state.sse);
    if (in->symbol) {
        fprintf(out, "\tcall %s%s\n", in->symbol->assembly, in->symbol->internal ? "" : "@PLT");
    } else {
        slot_load(e, in->a, "%r11");
        fputs("\tcall *%r11\n", out);
    }
    if (stack)
        fprintf(out, "\taddq $%zu, %%rsp\n", stack);
    if (!in->dest || result.memory)
        return;
    if (aggregate(in->type)) {
        slot_load(e, in->dest, "%r10");
        register_value(out, in->type, result, result_regs, 0, "%r10", 0, false);
    } else if (in->type->kind == TY_FLOAT) {
        register_value(out, in->type, result, result_regs, 0,
                       "%rbp", -(ptrdiff_t)e->offsets[in->dest], false);
    } else {
        if (in->type->kind == TY_BOOL)
            fputs("\tmovzbl %al, %eax\n", out);
        normalize(out, in->type);
        slot_store(e, in->dest, "%rax");
    }
}

static void return_value(Emitter *e, Inst *in, Type *t)
{
    FILE *out = e->out;
    if (in->a) {
        if (e->indirect_result) {
            slot_load(e, in->a, "%rsi");
            fprintf(out, "\tmovq -%zu(%%rbp), %%rdi\n\tmovq %%rdi, %%rax\n", e->indirect_result);
            copy_bytes(out, t->size);
        } else if (aggregate(t)) {
            slot_load(e, in->a, "%r10");
            register_value(out, t, classify(t), result_regs, 0, "%r10", 0, true);
        } else if (t->kind == TY_FLOAT) {
            register_value(out, t, classify(t), result_regs, 0,
                           "%rbp", -(ptrdiff_t)e->offsets[in->a], true);
        } else {
            slot_load(e, in->a, "%rax");
        }
    }
    fprintf(out, "\tjmp .Lreturn%u\n", e->function_id);
}

static void va_start_instruction(Emitter *e, Inst *in)
{
    slot_load(e, in->a, "%r10");
    fprintf(e->out, "\tmovl $%u, (%%r10)\n\tmovl $%u, 4(%%r10)\n"
                    "\tleaq %zu(%%rbp), %%rax\n\tmovq %%rax, 8(%%r10)\n"
                    "\tleaq -%zu(%%rbp), %%rax\n\tmovq %%rax, 16(%%r10)\n",
            e->named.gp * 8, GP_SAVE_SIZE + e->named.sse * 16, e->named.stack + 16, e->registers);
}

static void read_vararg(Emitter *e, Inst *in, const char *source)
{
    Type *t = in->type;
    FILE *out = e->out;
    if (aggregate(t)) {
        slot_load(e, in->dest, "%rdi");
        copy_bytes(out, t->size);
    } else if (extended(t)) {
        fprintf(out, "\tfldt (%s)\n", source);
        x87_store(e, t, in->dest);
    } else {
        object_load(out, t, source);
        slot_store(e, in->dest, "%rax");
    }
}

static void va_arg_instruction(Emitter *e, Inst *in)
{
    Type *t = in->type;
    Abi abi = classify(t);
    FILE *out = e->out;
    unsigned label = e->next_label++;
    slot_load(e, in->a, "%r10");
    if (!abi.memory && abi.part[0] != ABI_X87) {
        if (abi.gp)
            fprintf(out, "\tmovl (%%r10), %%r8d\n\tcmpl $%u, %%r8d\n\tja .Lvstack%u_%u\n",
                    GP_SAVE_SIZE - 8 * abi.gp, e->function_id, label);
        if (abi.sse)
            fprintf(out, "\tmovl 4(%%r10), %%r9d\n\tcmpl $%u, %%r9d\n\tja .Lvstack%u_%u\n",
                    REGISTER_SAVE_SIZE - 16 * abi.sse, e->function_id, label);
        if (abi.gp)
            fprintf(out, "\taddl $%u, (%%r10)\n", 8 * abi.gp);
        if (abi.sse)
            fprintf(out, "\taddl $%u, 4(%%r10)\n", 16 * abi.sse);
        fputs("\tmovq 16(%r10), %rsi\n", out);
        if (aggregate(t)) {
            slot_load(e, in->dest, "%rdi");
            for (unsigned i = 0; i < 2; i++) {
                if (abi.part[i] == ABI_NONE)
                    continue;
                bool gp = abi.part[i] == ABI_GP;
                const char *index = gp ? "%r8" : "%r9";
                fprintf(out, "\tleaq (%%rsi,%s), %%r11\n", index);
                unsigned size = chunk_size(t, i);
                load_chunk(out, result_regs[0], "%r11", 0, size);
                store_chunk(out, result_regs[0], "%rdi", (ptrdiff_t)(8 * i), size);
                fprintf(out, "\taddq $%u, %s\n", gp ? 8u : 16u, index);
            }
        } else {
            fprintf(out, "\tleaq (%%rsi,%s), %%r11\n", abi.gp ? "%r8" : "%r9");
            read_vararg(e, in, "%r11");
        }
        fprintf(out, "\tjmp .Lvcomplete%u_%u\n.Lvstack%u_%u:\n",
                e->function_id, label, e->function_id, label);
    }
    fputs("\tmovq 8(%r10), %rsi\n", out);
    if (t->align > 8)
        fputs("\taddq $15, %rsi\n\tandq $-16, %rsi\n", out);
    fprintf(out, "\tleaq %zu(%%rsi), %%rax\n\tmovq %%rax, 8(%%r10)\n", align_to(t->size, 8));
    read_vararg(e, in, "%rsi");
    fprintf(out, ".Lvcomplete%u_%u:\n", e->function_id, label);
}

static void copy_extended(FILE *out, const char *source, const char *destination)
{
    fprintf(out, "\tmovq (%s), %%rax\n\tmovq %%rax, (%s)\n"
                 "\tmovq 8(%s), %%rax\n\tmovq %%rax, 8(%s)\n",
            source, destination, source, destination);
}

static void slot_address(Emitter *e, size_t id, const char *reg)
{
    fprintf(e->out, "\tleaq -%zu(%%rbp), %s\n", e->offsets[id], reg);
}

static void instruction(Emitter *e, Inst *in, Type *result)
{
    FILE *out = e->out;
    switch (in->code) {
    case I_IMMEDIATE:
        number(out, in->immediate, "%rax");
        normalize(out, in->type);
        slot_store(e, in->dest, "%rax");
        break;
    case I_REAL:
        if (extended(in->type)) {
            uint64_t bits[2];
            if (extended_float_bits(e->context, in->real, bits, in->loc)) {
                number(out, bits[0], "%rax");
                slot_store(e, in->dest, "%rax");
                number(out, bits[1], "%rax");
                fprintf(out, "\tmovq %%rax, -%zu(%%rbp)\n", e->offsets[in->dest] - 8);
            }
        } else {
            number(out, float_bits(in->real, in->type->size), "%rax");
            slot_store(e, in->dest, "%rax");
        }
        break;
    case I_ADDRESS:
        if (in->symbol)
            object_address(e, in->symbol);
        else
            slot_address(e, in->a, "%rax");
        slot_store(e, in->dest, "%rax");
        break;
    case I_LOAD:
        slot_load(e, in->a, "%r10");
        if (extended(in->type)) {
            slot_address(e, in->dest, "%r11");
            copy_extended(out, "%r10", "%r11");
        } else {
            object_load(out, in->type, "%r10");
            slot_store(e, in->dest, "%rax");
        }
        break;
    case I_STORE:
        if (aggregate(in->type)) {
            slot_load(e, in->a, "%rdi");
            slot_load(e, in->b, "%rsi");
            copy_bytes(out, in->type->size);
        } else {
            slot_load(e, in->a, "%r10");
            if (extended(in->type)) {
                slot_address(e, in->b, "%r11");
                copy_extended(out, "%r11", "%r10");
            } else {
                slot_load(e, in->b, "%rax");
                object_store(out, in->type, "%r10");
            }
        }
        break;
    case I_COPY:
        if (extended(in->type)) {
            slot_address(e, in->a, "%r10");
            slot_address(e, in->dest, "%r11");
            copy_extended(out, "%r10", "%r11");
        } else {
            slot_load(e, in->a, "%rax");
            slot_store(e, in->dest, "%rax");
        }
        break;
    case I_CAST: cast_instruction(e, in); break;
    case I_UNARY: unary_value(e, in); break;
    case I_BINARY:
        if (in->source_type->kind == TY_FLOAT)
            fp_binary(e, in);
        else
            integer_binary(e, in);
        break;
    case I_LABEL:
        fprintf(out, ".L%u_%zu:\n", e->function_id, in->label);
        break;
    case I_JUMP:
        if (in->a)
            slot_load(e, in->a, "%rsp");
        fprintf(out, "\tjmp .L%u_%zu\n", e->function_id, in->label);
        break;
    case I_ZERO:
        fprintf(out, "\tcmpq $0, -%zu(%%rbp)\n\t%s .L%u_%zu\n",
                e->offsets[in->a], in->op ? "jne" : "je", e->function_id, in->label);
        break;
    case I_CALL: call_instruction(e, in); break;
    case I_RETURN: return_value(e, in, result); break;
    case I_MEMCOPY:
        slot_load(e, in->a, "%rdi");
        slot_load(e, in->b, "%rsi");
        copy_bytes(out, in->immediate);
        break;
    case I_MEMZERO:
        slot_load(e, in->a, "%rdi");
        number(out, in->immediate, "%rcx");
        fputs("\txorl %eax, %eax\n\trep stosb\n", out);
        break;
    case I_VA_START: va_start_instruction(e, in); break;
    case I_VA_ARG: va_arg_instruction(e, in); break;
    case I_BITLOAD: bit_load(e, in); break;
    case I_BITSTORE: bit_store(e, in); break;
    case I_STACK_ALLOC:
        slot_store(e, in->b, "%rsp");
        slot_load(e, in->a, "%rax");
        fputs("\taddq $15, %rax\n\tandq $-16, %rax\n\tsubq %rax, %rsp\n", out);
        slot_store(e, in->dest, "%rsp");
        break;
    case I_STACK_RESTORE:
        slot_load(e, in->a, "%rsp");
        break;
    }
}

static void emit_function(Context *c, FILE *out, Symbol *s, unsigned id)
{
    FunctionIR ir;
    if (!lower(c, s, &ir))
        return;
    Emitter e = {.context = c, .out = out, .function_id = id};
    e.offsets = alloc(c, (ir.slots.len + 1) * sizeof *e.offsets);
    size_t frame = 0;
    for (size_t i = 0; i < ir.slots.len; i++) {
        Type *t = ir.slots.data[i];
        size_t width = t->size < 8 ? 8 : t->size;
        size_t alignment = t->align < 8 ? 8 : t->align;
        frame = align_to(frame + width, alignment);
        e.offsets[i + 1] = frame;
    }
    e.scratch = frame = align_to(frame, 8) + 8;
    Type *result = s->type->base;
    if (classify(result).memory)
        e.indirect_result = frame += 8;
    if (s->type->variadic)
        e.registers = frame = align_to(frame, 16) + REGISTER_SAVE_SIZE;
    frame = align_to(frame, 16);
    if (frame > INT32_MAX) {
        error(c, s->loc, "x86-64 stack frame is too large");
        return;
    }

    fputs("\t.text\n\t.p2align 4\n", out);
    if (!s->internal)
        fprintf(out, "\t.globl %s\n", s->assembly);
    fprintf(out, "\t.type %s, @function\n%s:\n"
                 "\tpushq %%rbp\n\tmovq %%rsp, %%rbp\n\tsubq $%zu, %%rsp\n",
            s->assembly, s->assembly, frame);
    parameters(&e, s);
    for (size_t i = 0; i < ir.instructions.len && !c->failed; i++)
        instruction(&e, &ir.instructions.data[i], result);
    fprintf(out, ".Lreturn%u:\n\tleave\n\tret\n\t.size %s, .-%s\n",
            id, s->assembly, s->assembly);
}

bool emit_x86_64(Context *c, FILE *out)
{
    for (size_t i = 0; i < c->globals.len && !c->failed; i++) {
        Symbol *s = c->globals.data[i];
        if (s->type->kind == TY_FUNC && s->body)
            emit_function(c, out, s, (unsigned)i);
        else if (s->type->kind != TY_FUNC && (s->defined || s->tentative))
            emit_static_data(c, out, s);
    }
    fputs("\t.section .rodata\n\t.p2align 3\n"
          ".Ltwo63:\n\t.quad 0x43e0000000000000\n"
          ".Ltwo64:\n\t.quad 0x43f0000000000000\n"
          "\t.section .note.GNU-stack,\"\",@progbits\n", out);
    return !c->failed && !ferror(out);
}
