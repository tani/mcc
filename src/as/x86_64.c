#include "as.h"

#include <string.h>

enum { RIP = 16 };
typedef struct { int id; unsigned width; } Register;
typedef enum { OP_REGISTER, OP_MEMORY, OP_IMMEDIATE } OperandKind;
typedef struct {
    OperandKind kind;
    Register reg;
    int base, index;
    unsigned scale, segment;
    AsmExpr expr;
    uint32_t relocation;
} Operand;

static bool reg(const char *text, Register *out)
{
    static const char *const names[4][8] = {
        {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"},
        {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"},
        {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"},
        {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"},
    };
    if (*text++ != '%')
        return false;
    for (unsigned width = 0; width < 4; width++)
        for (int i = 0; i < 8; i++)
            if (equal(text, names[width][i])) {
                *out = (Register){i, 1u << width};
                return true;
            }
    if (!strncmp(text, "xmm", 3)) {
        uint64_t id;
        if (mas_integer(text + 3, &id) && id < 16) {
            *out = (Register){(int)id, 16};
            return true;
        }
    }
    if (*text == 'r' && text[1] >= '0' && text[1] <= '9') {
        unsigned id = (unsigned)(text[1] - '0');
        text += 2;
        if (*text >= '0' && *text <= '9')
            id = id * 10 + (unsigned)(*text++ - '0');
        if (id < 8 || id > 15)
            return false;
        unsigned width = *text == 'b' ? 1 : *text == 'w' ? 2 : *text == 'd' ? 4 : !*text ? 8 : 0;
        if (!width || (*text && text[1]))
            return false;
        *out = (Register){(int)id, width};
        return true;
    }
    return false;
}

static bool expression(Assembler *a, char *text, const char *modifier, AsmExpr *out, bool *modified)
{
    char *at = strchr(text, '@');
    *modified = at != NULL;
    if (at) {
        size_t len = strlen(modifier);
        if (strncmp(at, modifier, len) ||
            (at[len] && at[len] != '+' && at[len] != '-'))
            return false;
        memmove(at, at + len, strlen(at + len) + 1);
    }
    return mas_expression(a, text, out);
}

static bool operand(Assembler *a, char *text, Operand *out)
{
    *out = (Operand){.base = -1, .index = -1};
    if (*text == '$') {
        out->kind = OP_IMMEDIATE;
        return mas_expression(a, text + 1, &out->expr);
    }
    if (!strncmp(text, "%fs:", 4) || !strncmp(text, "%gs:", 4)) {
        out->segment = text[1] == 'f' ? 0x64 : 0x65;
        text = mas_trim(text + 4);
    }
    if (*text == '%') {
        out->kind = OP_REGISTER;
        return !out->segment && reg(text, &out->reg);
    }
    out->kind = OP_MEMORY;
    char *open = strchr(text, '('), *close = strrchr(text, ')');
    if (out->segment && !open && !close)
        return mas_expression(a, text, &out->expr) && !out->expr.symbol &&
               fits_signed((int64_t)out->expr.value, 32);
    if (!open || !close || close < open || close[1])
        return false;
    *open++ = *close = 0;
    char *index = strchr(open, ',');
    if (index)
        *index++ = 0;
    const char *base = mas_trim(open);
    Register r;
    if (equal(base, "%rip"))
        out->base = RIP;
    else if (*base) {
        if (!reg(base, &r) || r.width != 8)
            return false;
        out->base = r.id;
    }
    if (index) {
        char *scale = strchr(index, ',');
        if (scale)
            *scale++ = 0;
        if (out->base == RIP || !reg(mas_trim(index), &r) || r.width != 8 || r.id == 4)
            return false;
        out->index = r.id;
        if (scale) {
            uint64_t value;
            if (!mas_integer(mas_trim(scale), &value) ||
                (value != 1 && value != 2 && value != 4 && value != 8))
                return false;
            out->scale = value == 8 ? 3 : value == 4 ? 2 : value == 2 ? 1 : 0;
        }
    }
    text = mas_trim(text);
    if (*text) {
        bool tls = strstr(text, "@tlsgd") != NULL;
        bool modified;
        if (!expression(a, text, tls ? "@tlsgd" : "@GOTPCREL", &out->expr, &modified))
            return false;
        if (modified && !out->expr.symbol)
            return false;
        if (modified)
            out->relocation = tls ? R_X86_64_TLSGD : R_X86_64_GOTPCREL;
    }
    return (out->base == RIP || (!out->relocation && !out->expr.symbol)) &&
           (out->expr.symbol || fits_signed((int64_t)out->expr.value, 32));
}

static bool is_rm(const Operand *operand, unsigned width)
{
    return operand->kind == OP_MEMORY ||
           (operand->kind == OP_REGISTER && operand->reg.width == width);
}

static unsigned low_register(int id)
{
    return (unsigned)id & 7;
}

static void encode(Assembler *a, unsigned prefix, unsigned width, uint16_t opcode, unsigned field,
                   const Operand *rm, unsigned trailing)
{
    bool low_byte = prefix == 0x40;
    if (rm->segment)
        mas_emit(a, rm->segment, 1);
    if (prefix && !low_byte)
        mas_emit(a, prefix, 1);
    if (width == 2)
        mas_emit(a, 0x66, 1);
    unsigned rex = (width == 8 ? 8u : 0u) | (field >= 8 ? 4u : 0u);
    int base = rm->kind == OP_REGISTER ? rm->reg.id : rm->base;
    if (rm->index >= 8)
        rex |= 2;
    if (base >= 8 && base != RIP)
        rex |= 1;
    if (rex || low_byte || (width == 1 && (field >= 4 || (rm->kind == OP_REGISTER && base >= 4))))
        mas_emit(a, rex | 0x40, 1);
    if (opcode > 0xff)
        mas_emit(a, opcode >> 8, 1);
    mas_emit(a, opcode & 0xff, 1);
    unsigned middle = (field & 7) << 3;
    if (rm->kind == OP_REGISTER) {
        mas_emit(a, 0xc0 | middle | low_register(base), 1);
        return;
    }
    if (base == RIP) {
        mas_emit(a, middle | 5, 1);
        AsmExpr disp = rm->expr;
        if (disp.symbol) {
            disp.value -= 4 + trailing;
            mas_relocate(a, rm->relocation ? rm->relocation : R_X86_64_PC32, disp);
            disp.value = 0;
        }
        mas_emit(a, disp.value, 4);
        return;
    }
    int64_t disp = (int64_t)rm->expr.value;
    unsigned mod = 0, length = 0;
    if (base < 0)
        length = 4;
    else if (disp || low_register(base) == 5) {
        mod = fits_signed(disp, 8) ? 1 : 2;
        length = mod == 1 ? 1 : 4;
    }
    bool sib = base < 0 || low_register(base) == 4 || rm->index >= 0;
    mas_emit(a, (mod << 6) | middle | (sib ? 4 : low_register(base)), 1);
    if (sib) {
        unsigned index = rm->index < 0 ? 4 : low_register(rm->index);
        mas_emit(a, (rm->scale << 6) | (index << 3) | (base < 0 ? 5 : low_register(base)), 1);
    }
    mas_emit(a, rm->expr.value, length);
}

static int condition(const char *name)
{
    static const char *const names[] = {
        "o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"
    };
    if (equal(name, "z"))
        return 4;
    if (equal(name, "nz"))
        return 5;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (equal(name, names[i]))
            return (int)i;
    return -1;
}

static bool branch(Assembler *a, const char *op, AsmArgs *args)
{
    if (args->len != 1)
        return false;
    bool call = equal(op, "call") || equal(op, "callq");
    bool jump = equal(op, "jmp") || equal(op, "jmpq");
    int cond = *op == 'j' ? condition(op + 1) : -1;
    if (!call && !jump && cond < 0)
        return false;
    char *text = args->data[0];
    if (*text == '*') {
        Operand rm;
        if ((!call && !jump) || !operand(a, mas_trim(text + 1), &rm) || !is_rm(&rm, 8))
            return false;
        encode(a, 0, 0, 0xff, call ? 2u : 4u, &rm, 0);
        return true;
    }
    AsmExpr expression_value;
    bool plt;
    if (!expression(a, text, "@PLT", &expression_value, &plt) || !expression_value.symbol ||
        (plt && !call && !jump))
        return false;
    if (call || jump)
        mas_emit(a, call ? 0xe8 : 0xe9, 1);
    else {
        mas_emit(a, 0x0f, 1);
        mas_emit(a, 0x80u + (unsigned)cond, 1);
    }
    expression_value.value -= 4;
    mas_relocate(a, plt ? R_X86_64_PLT32 : R_X86_64_PC32, expression_value);
    mas_emit(a, 0, 4);
    return true;
}

static bool immediate_fits(AsmExpr expr, unsigned width, bool sign)
{
    if (expr.symbol)
        return width >= 4;
    if (width == 8)
        return true;
    return fits_signed((int64_t)expr.value, width * 8) ||
           (!sign && expr.value < (UINT64_C(1) << (width * 8)));
}

static void emit_immediate(Assembler *a, AsmExpr expr, unsigned width, bool sign)
{
    if (expr.symbol)
        mas_relocate(a, width == 8 ? R_X86_64_64 : sign ? R_X86_64_32S : R_X86_64_32, expr);
    mas_emit(a, expr.symbol ? 0 : expr.value, width);
}

static bool move(Assembler *a, const char *op, unsigned width, Operand *source, Operand *dest)
{
    bool absolute = equal(op, "movabs");
    if ((!absolute && !equal(op, "mov")) || !is_rm(dest, width))
        return false;
    if (source->kind == OP_IMMEDIATE) {
        unsigned size = width == 8 ? 4 : width;
        if (absolute || (width == 8 && dest->kind == OP_REGISTER && !immediate_fits(source->expr, size, true))) {
            if (width != 8 || dest->kind != OP_REGISTER)
                return false;
            mas_emit(a, dest->reg.id >= 8 ? 0x49 : 0x48, 1);
            mas_emit(a, 0xb8 | low_register(dest->reg.id), 1);
            emit_immediate(a, source->expr, 8, false);
        } else {
            if (!immediate_fits(source->expr, size, width == 8))
                return false;
            encode(a, 0, width, width == 1 ? 0xc6 : 0xc7, 0, dest, size);
            emit_immediate(a, source->expr, size, width == 8);
        }
    } else if (!absolute && is_rm(source, width) && source->kind == OP_REGISTER) {
        encode(a, 0, width, width == 1 ? 0x88 : 0x89, (unsigned)source->reg.id, dest, 0);
    } else if (!absolute && source->kind == OP_MEMORY && dest->kind == OP_REGISTER) {
        encode(a, 0, width, width == 1 ? 0x8a : 0x8b, (unsigned)dest->reg.id, source, 0);
    } else {
        return false;
    }
    return true;
}

typedef struct { const char *name; unsigned operation; } Arithmetic;
static const Arithmetic arithmetic_ops[] = {
    {"add", 0}, {"or", 1}, {"adc", 2}, {"sbb", 3},
    {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7},
};

static bool arithmetic(Assembler *a, const char *op, unsigned width, Operand *source, Operand *dest)
{
    const Arithmetic *insn = NULL;
    for (size_t i = 0; i < sizeof arithmetic_ops / sizeof *arithmetic_ops; i++)
        if (equal(op, arithmetic_ops[i].name))
            insn = &arithmetic_ops[i];
    bool test = equal(op, "test");
    if ((!insn && !test) || !is_rm(dest, width))
        return false;
    unsigned operation = test ? 0 : insn->operation;
    unsigned opcode = test ? 0x84 : operation * 8;
    if (source->kind == OP_IMMEDIATE) {
        unsigned size = width == 8 ? 4 : width;
        if (!immediate_fits(source->expr, size, width == 8))
            return false;
        unsigned immediate = test ? 0xf6 : 0x80;
        encode(a, 0, width, (uint16_t)(immediate + (width != 1)), operation, dest, size);
        emit_immediate(a, source->expr, size, width == 8);
    } else if (source->kind == OP_REGISTER && is_rm(source, width)) {
        encode(a, 0, width, (uint16_t)(opcode + (width != 1)), (unsigned)source->reg.id, dest, 0);
    } else if (source->kind == OP_MEMORY && dest->kind == OP_REGISTER) {
        bool reversed = !test;
        encode(a, 0, width, (uint16_t)(opcode + 2u * reversed + (width != 1)),
               (unsigned)dest->reg.id, source, 0);
    } else {
        return false;
    }
    return true;
}

static bool shifts(Assembler *a, const char *op, unsigned width, Operand *count, Operand *dest)
{
    unsigned code = equal(op, "shl") || equal(op, "sal") ? 4u : equal(op, "shr") ? 5u :
                    equal(op, "sar") ? 7u : 0u;
    if (!code || !is_rm(dest, width))
        return false;
    if (count->kind == OP_IMMEDIATE && !count->expr.symbol && count->expr.value <= 255) {
        encode(a, 0, width, width == 1 ? 0xc0 : 0xc1, code, dest, 1);
        mas_emit(a, count->expr.value, 1);
    } else if (count->kind == OP_REGISTER && count->reg.id == 1 && count->reg.width == 1) {
        encode(a, 0, width, width == 1 ? 0xd2 : 0xd3, code, dest, 0);
    } else {
        return false;
    }
    return true;
}

static bool unary_instruction(Assembler *a, const char *op, unsigned width, Operand *argument)
{
    if ((equal(op, "push") || equal(op, "pop")) && width == 8) {
        bool push = equal(op, "push");
        if (push && argument->kind == OP_IMMEDIATE && immediate_fits(argument->expr, 4, true)) {
            mas_emit(a, 0x68, 1);
            emit_immediate(a, argument->expr, 4, true);
        } else if (is_rm(argument, width)) {
            encode(a, 0, 0, push ? 0xff : 0x8f, push ? 6u : 0u, argument, 0);
        } else {
            return false;
        }
        return true;
    }
    unsigned code = equal(op, "not") ? 2u : equal(op, "neg") ? 3u : equal(op, "mul") ? 4u :
                    equal(op, "imul") ? 5u : equal(op, "div") ? 6u : equal(op, "idiv") ? 7u : 0u;
    if (!code || !is_rm(argument, width))
        return false;
    encode(a, 0, width, width == 1 ? 0xf6 : 0xf7, code, argument, 0);
    return true;
}

static bool conversion(Assembler *a, const char *op, Operand *src, Operand *dst)
{
    bool sign = !strncmp(op, "movs", 4);
    if ((!sign && strncmp(op, "movz", 4)) || strlen(op) != 6 ||
        dst->kind != OP_REGISTER)
        return false;
    unsigned from = op[4] == 'b' ? 1u : op[4] == 'w' ? 2u : op[4] == 'l' ? 4u : 0u;
    unsigned to = op[5] == 'l' ? 4u : op[5] == 'q' ? 8u : op[5] == 'w' ? 2u : 0u;
    if (!from || !to || to <= from || dst->reg.width != to || !is_rm(src, from))
        return false;
    if (from == 4 && !sign)
        return false;
    uint16_t code = from == 4 ? 0x63 : (uint16_t)((sign ? 0x0fbe : 0x0fb6) + (from == 2));
    /* MOVSX/MOVZX may require a bare REX for the low byte of SP, BP, SI or DI. */
    bool byte_rex = from == 1 && to < 8 && src->kind == OP_REGISTER &&
                    src->reg.id >= 4 && src->reg.id < 8 && dst->reg.id < 8;
    encode(a, byte_rex ? 0x40u : 0u, to, code, (unsigned)dst->reg.id, src, 0);
    return true;
}

static bool sse(Assembler *a, const char *op, Operand *src, Operand *dst)
{
    bool src_xmm = src->kind == OP_REGISTER && src->reg.width == 16;
    bool dst_xmm = dst->kind == OP_REGISTER && dst->reg.width == 16;
    if (!src_xmm && !dst_xmm)
        return false;
    if ((equal(op, "movd") || equal(op, "movq")) && src_xmm != dst_xmm) {
        unsigned size = equal(op, "movd") ? 4u : 8u;
        Operand *rm = dst_xmm ? src : dst;
        if (!is_rm(rm, size))
            return false;
        encode(a, 0x66, size, dst_xmm ? 0x0f6e : 0x0f7e,
               (unsigned)(dst_xmm ? dst->reg.id : src->reg.id), rm, 0);
        return true;
    }
    static const struct { const char *name; unsigned prefix; uint16_t load, store; } ops[] = {
        {"movss", 0xf3, 0x0f10, 0x0f11}, {"movsd", 0xf2, 0x0f10, 0x0f11},
        {"movaps", 0, 0x0f28, 0x0f29},
        {"movups", 0, 0x0f10, 0x0f11},
        {"addss", 0xf3, 0x0f58, 0}, {"addsd", 0xf2, 0x0f58, 0},
        {"subss", 0xf3, 0x0f5c, 0}, {"subsd", 0xf2, 0x0f5c, 0},
        {"mulss", 0xf3, 0x0f59, 0}, {"mulsd", 0xf2, 0x0f59, 0},
        {"divss", 0xf3, 0x0f5e, 0}, {"divsd", 0xf2, 0x0f5e, 0},
        {"ucomiss", 0, 0x0f2e, 0}, {"ucomisd", 0x66, 0x0f2e, 0},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (!equal(op, ops[i].name))
            continue;
        if (dst_xmm && is_rm(src, 16)) {
            encode(a, ops[i].prefix, 0, ops[i].load, (unsigned)dst->reg.id, src, 0);
            return true;
        }
        if (ops[i].store && src_xmm && dst->kind == OP_MEMORY) {
            encode(a, ops[i].prefix, 0, ops[i].store, (unsigned)src->reg.id, dst, 0);
            return true;
        }
    }
    return false;
}

static bool x87(Assembler *a, const char *op, AsmArgs *args)
{
    if (args->len == 2) {
        static const struct { const char *name; unsigned first, second; } ops[] = {
            {"faddp", 0xde, 0xc1}, {"fsubrp", 0xde, 0xe9}, {"fmulp", 0xde, 0xc9},
            {"fdivrp", 0xde, 0xf9}, {"fucomip", 0xdf, 0xe9},
        };
        for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
            if (!equal(op, ops[i].name))
                continue;
            bool compare = equal(op, "fucomip");
            if (!equal(args->data[0], compare ? "%st(1)" : "%st") ||
                !equal(args->data[1], compare ? "%st" : "%st(1)"))
                return false;
            mas_emit(a, ops[i].first, 1);
            mas_emit(a, ops[i].second, 1);
            return true;
        }
    }
    if (args->len != 1)
        return false;
    if (equal(op, "fstp") && equal(args->data[0], "%st(0)")) {
        mas_emit(a, 0xdd, 1);
        mas_emit(a, 0xd8, 1);
        return true;
    }
    static const struct { const char *name; uint16_t opcode; unsigned field; } memory_ops[] = {
        {"flds", 0xd9, 0}, {"fldl", 0xdd, 0}, {"fldt", 0xdb, 5},
        {"fstps", 0xd9, 3}, {"fstpl", 0xdd, 3}, {"fstpt", 0xdb, 7},
        {"fildq", 0xdf, 5}, {"fistpq", 0xdf, 7},
        {"fldcw", 0xd9, 5}, {"fnstcw", 0xd9, 7},
        {"faddl", 0xdc, 0}, {"fsubl", 0xdc, 4},
        {"ldmxcsr", 0x0fae, 2}, {"stmxcsr", 0x0fae, 3},
    };
    for (size_t i = 0; i < sizeof memory_ops / sizeof *memory_ops; i++) {
        if (!equal(op, memory_ops[i].name))
            continue;
        Operand rm;
        if (!operand(a, args->data[0], &rm) || rm.kind != OP_MEMORY)
            return false;
        encode(a, 0, 0, memory_ops[i].opcode, memory_ops[i].field, &rm, 0);
        return true;
    }
    return false;
}

static bool simple(Assembler *a, const char *op)
{
    static const struct { const char *name; uint32_t opcode; } ops[] = {
        {"ret", 0xc3}, {"retq", 0xc3}, {"leave", 0xc9}, {"nop", 0x90}, {"cqto", 0x4899},
        {"hlt", 0xf4}, {"syscall", 0x0f05}, {"ud2", 0x0f0b}, {"int3", 0xcc},
        {"fldz", 0xd9ee}, {"fchs", 0xd9e0}, {"cld", 0xfc}, {"rex64", 0x48},
        {"lfence", 0x0faee8},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        if (equal(op, ops[i].name)) {
            if (ops[i].opcode > 0xffff)
                mas_emit(a, ops[i].opcode >> 16, 1);
            if (ops[i].opcode > 0xff)
                mas_emit(a, (ops[i].opcode >> 8) & 0xff, 1);
            mas_emit(a, ops[i].opcode & 0xff, 1);
            return true;
        }
    return false;
}

static bool integer_instruction(Assembler *a, const char *op, size_t count, unsigned width,
                                Operand *source, Operand *dest)
{
    if (!width || width > 8)
        return false;
    if (count == 1)
        return unary_instruction(a, op, width, source);
    if (equal(op, "lea") && source->kind == OP_MEMORY && source->relocation != R_X86_64_GOTPCREL &&
        dest->kind == OP_REGISTER && dest->reg.width == width && width != 1) {
        encode(a, 0, width, 0x8d, (unsigned)dest->reg.id, source, 0);
        return true;
    }
    if (equal(op, "imul") && width != 1 && is_rm(source, width) &&
        dest->kind == OP_REGISTER && dest->reg.width == width) {
        encode(a, 0, width, 0x0faf, (unsigned)dest->reg.id, source, 0);
        return true;
    }
    return move(a, op, width, source, dest) || arithmetic(a, op, width, source, dest) ||
           shifts(a, op, width, source, dest);
}

bool mas_x86_64(Assembler *a, const char *op, AsmArgs *args)
{
    if (!args->len)
        return simple(a, op);
    if (args->len == 1 && equal(op, "rep")) {
        const char *name = args->data[0];
        if (!equal(name, "movsb") && !equal(name, "stosb"))
            return false;
        mas_emit(a, 0xf3, 1);
        mas_emit(a, equal(name, "movsb") ? 0xa4 : 0xaa, 1);
        return true;
    }
    if (branch(a, op, args) || x87(a, op, args))
        return true;
    bool tls_prefix = equal(op, "data16") && args->len == 2 && !strncmp(args->data[0], "leaq ", 5);
    Operand src, dst = {0};
    if (args->len > 2 || !operand(a, tls_prefix ? mas_trim(args->data[0] + 5) : args->data[0], &src) ||
        (args->len == 2 && !operand(a, args->data[1], &dst)))
        return false;
    if (tls_prefix) {
        if (src.kind != OP_MEMORY || src.relocation != R_X86_64_TLSGD ||
            dst.kind != OP_REGISTER || dst.reg.width != 8)
            return false;
        encode(a, 0x66, 8, 0x8d, (unsigned)dst.reg.id, &src, 0);
        return true;
    }
    if (args->len == 1 && !strncmp(op, "set", 3)) {
        int cond = condition(op + 3);
        if (cond < 0 || !is_rm(&src, 1))
            return false;
        encode(a, 0, 1, (uint16_t)(0x0f90 + cond), 0, &src, 0);
        return true;
    }
    if (args->len == 1 && src.kind == OP_MEMORY && !strncmp(op, "prefetch", 8)) {
        static const char *const hints[] = {"prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"};
        for (unsigned i = 0; i < sizeof hints / sizeof *hints; i++)
            if (equal(op, hints[i])) {
                encode(a, 0, 4, 0x0f18, i, &src, 0);
                return true;
            }
        return false;
    }
    if (args->len == 2 && (sse(a, op, &src, &dst) || conversion(a, op, &src, &dst)))
        return true;

    size_t len = strlen(op);
    char suffix = op[len - 1];
    unsigned width = suffix == 'b' ? 1u : suffix == 'w' ? 2u : suffix == 'l' ? 4u : suffix == 'q' ? 8u : 0;
    char name[16];
    if (width && len < sizeof name) {
        memcpy(name, op, len - 1);
        name[len - 1] = 0;
        if (integer_instruction(a, name, args->len, width, &src, &dst))
            return true;
    }
    width = args->len == 2 && dst.kind == OP_REGISTER ? dst.reg.width :
            src.kind == OP_REGISTER ? src.reg.width : 0;
    return integer_instruction(a, op, args->len, width, &src, &dst);
}
