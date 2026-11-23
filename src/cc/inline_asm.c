#include "inline_asm.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

static const char *const x86_registers[][4] = {
    {"rax","eax","ax","al"}, {"rcx","ecx","cx","cl"}, {"rdx","edx","dx","dl"},
    {"rbx","ebx","bx","bl"}, {"rsp","esp","sp","spl"}, {"rbp","ebp","bp","bpl"},
    {"rsi","esi","si","sil"}, {"rdi","edi","di","dil"},
    {"r8","r8d","r8w","r8b"}, {"r9","r9d","r9w","r9b"},
    {"r10","r10d","r10w","r10b"}, {"r11","r11d","r11w","r11b"},
    {"r12","r12d","r12w","r12b"}, {"r13","r13d","r13w","r13b"},
    {"r14","r14d","r14w","r14b"}, {"r15","r15d","r15w","r15b"}
};
static const char *const x86_high[] = {"ah","ch","dh","bh"};

const char *x86_asm_register(unsigned reg, unsigned width)
{
    return x86_registers[reg][width == 1 ? 3 : width == 2 ? 2 : width == 4 ? 1 : 0];
}

static uint64_t reg_bit(unsigned reg)
{
    return UINT64_C(1) << reg;
}

static int register_suffix(const char *name, const char *end)
{
    if (name == end || (size_t)(end - name) > 2)
        return -1;
    int n = 0;
    for (; name != end; name++) {
        if (*name < '0' || *name > '9')
            return -1;
        n = n * 10 + (*name - '0');
    }
    return n;
}

static int named_register(Context *c, const char *s, const char *end)
{
    if (s != end && *s == '%')
        s++;
    size_t len = (size_t)(end - s);
    if (!len)
        return -1;
    if (c->target == &x86_64_linux) {
        for (unsigned i = 0; i < 16; i++) {
            if ((i < 4 && strlen(x86_high[i]) == len && !memcmp(s, x86_high[i], len)))
                return (int)i;
            for (unsigned j = 0; j < 4; j++) {
                const char *name = x86_registers[i][j];
                if (strlen(name) == len && !memcmp(s, name, len))
                    return (int)i;
            }
        }
        if (len > 3 && !memcmp(s, "xmm", 3)) {
            int n = register_suffix(s + 3, end);
            return n >= 0 && n < 16 ? n + 32 : -1;
        }
    } else {
        if (len == 2) {
            if (!memcmp(s, "sp", 2)) return 31;
            if (!memcmp(s, "fp", 2)) return 29;
            if (!memcmp(s, "lr", 2)) return 30;
        }
        int n = register_suffix(s + 1, end);
        if (n < 0 || n > 31)
            return -1;
        if (*s == 'x' || *s == 'w')
            return n;
        if (strchr("vqdshb", *s))
            return n + 32;
    }
    return -1;
}

static uint64_t general_registers(Context *c, bool global)
{
    if (c->target == &x86_64_linux)
        return UINT64_C(0xffcf);
    if (global)
        return UINT64_C(0x1fffffff);
    /* Frame-addressing scratch registers, platform register, FP, LR and SP. */
    return UINT64_C(0x1ffb3fff);
}

static uint64_t floating_registers(Context *c)
{
    return c->target == &x86_64_linux ? UINT64_C(0xffff00000000) : UINT64_C(0xffffffff00000000);
}

static uint64_t registers_for_type(Context *c, Type *type, bool global)
{
    if (aggregate(type) && type->complete) {
        switch (type->size) {
        case 1: case 2: case 4: case 8: return general_registers(c, global) | floating_registers(c);
        case 16: return floating_registers(c);
        default: return 0;
        }
    }
    if (type->kind == TY_FLOAT && (type->size <= 8 || c->target == &aarch64_linux))
        return floating_registers(c);
    if (type->kind != TY_COMPLEX && scalar(type) && type->size <= 8)
        return general_registers(c, global);
    return 0;
}

uint64_t asm_call_saved_registers(Context *c)
{
    if (c->target == &x86_64_linux)
        return UINT64_C(0xf008);
    return UINT64_C(0xff001ff80000);
}

unsigned asm_register_binding(Context *c, const char *name, Type *type, bool global, Loc loc)
{
    int reg = named_register(c, name, name + strlen(name));
    uint64_t permitted = registers_for_type(c, type, global);
    if (reg < 0 || !(permitted & reg_bit((unsigned)reg))) {
        error(c, loc, "invalid register '%s' for %s register variable", name,
              global ? "global" : "local");
        return 0;
    }
    return (unsigned)reg + 1;
}

bool asm_call_clobbered(Context *c, unsigned binding)
{
    return !(asm_call_saved_registers(c) & reg_bit(binding - 1));
}

size_t asm_register_spill_layout(uint64_t registers, size_t offsets[64])
{
    size_t frame = 0;
    if (!registers)
        return frame;
    /* General-purpose spills come first so AArch64 can address them from FP directly. */
    for (unsigned reg = 0; reg < 64; reg++) {
        if (!(registers & reg_bit(reg)))
            continue;
        size_t width = reg < 32 ? 8 : 16;
        frame = align_to(frame, width) + width;
        offsets[reg] = frame;
    }
    return frame;
}

typedef struct {
    uint64_t registers;
    bool memory;
} Choice;

static bool match_name(const char *name, const char *begin, const char *end)
{
    return name && strlen(name) == (size_t)(end - begin) &&
           !memcmp(name, begin, (size_t)(end - begin));
}

static int operand_named(const InlineAssembly *assembly, const char *start, const char *end,
                         bool label)
{
    size_t count = label ? assembly->labels.len : assembly->operands.len;
    for (size_t i = 0; i < count; i++) {
        const char *name = label ? assembly->labels.data[i]->name : assembly->operands.data[i].name;
        if (match_name(name, start, end))
            return (int)i;
    }
    return -1;
}

static bool immediate_fits(Context *c, int code, uint64_t bits)
{
    int64_t value = (int64_t)bits;
    if (c->target == &x86_64_linux) {
        switch (code) {
        case 'I': return bits <= 31;
        case 'J': return bits <= 63;
        case 'K': return value >= -128 && value <= 127;
        case 'M': return bits <= 3;
        case 'N': return bits <= 255;
        case 'e': return value >= INT32_MIN && value <= INT32_MAX;
        case 'Z': return bits <= UINT32_MAX;
        default: return false;
        }
    }
    if (code == 'J')
        bits = 0 - bits;
    return (code == 'I' || code == 'J') && (bits <= 4095 ||
           (!(bits & 4095) && (bits >> 12) <= 4095));
}

static bool condition_ok(Context *c, const char *s)
{
    static const char *const arm[] = {
        "eq","ne","cs","hs","cc","lo","mi","pl","vs","vc","hi","ls","ge","lt","gt","le"
    };
    static const char *const x86[] = {
        "a","ae","b","be","c","e","z","g","ge","l","le","o","p","s",
        "na","nae","nb","nbe","nc","ne","ng","nge","nl","nle","no","np","ns","nz",
        "pe","po"
    };
    bool amd = c->target == &x86_64_linux;
    const char *const *set = amd ? x86 : arm;
    size_t len = amd ? sizeof x86 / sizeof *x86 : sizeof arm / sizeof *arm;
    for (size_t i = 0; i < len; i++)
        if (equal(s, set[i]))
            return true;
    return false;
}

static uint64_t registers_for(Context *c, char code, bool global)
{
    uint64_t general = general_registers(c, global), fp = floating_registers(c);
    if (strchr("rgXp", code))
        return general;
    if (c->target == &aarch64_linux) {
        if (code == 'w') return fp;
        if (code == 'x') return fp & UINT64_C(0xffff00000000);
        if (code == 'y') return fp & UINT64_C(0xff00000000);
    } else {
        switch (code) {
        case 'a': return reg_bit(0);
        case 'c': return reg_bit(1);
        case 'd': return reg_bit(2);
        case 'b': return reg_bit(3);
        case 'S': return reg_bit(6);
        case 'D': return reg_bit(7);
        case 'q': return general;
        case 'Q': return UINT64_C(15);
        case 'R': return general & UINT64_C(255);
        case 'x': return fp;
        default: break;
        }
    }
    return 0;
}

static Type *asm_operand_type(Context *c, Node *expression, Symbol *reg_object, bool output)
{
    Type *type = expression->type;
    if (type->kind == TY_FUNC || (type->kind == TY_ARRAY && !reg_object && !output))
        return decay(c, expression)->type;
    return type;
}

static bool read_constraint(Context *c, AsmPlan *plan, size_t index, unsigned alternative,
                            Choice *choice)
{
    InlineAssembly *assembly = plan->assembly;
    AsmOperand *operand = &assembly->operands.data[index];
    AsmBinding *binding = &plan->bindings[index];
    bool output = index < assembly->outputs;
    const char *start = operand->constraint;
    binding->read_write = output && *start == '+';
    if (output)
        start++;
    for (unsigned i = 0; i < alternative; i++) {
        start = strchr(start, ',');
        if (!start)
            return false;
        start++;
    }
    const char *end = strchr(start, ',');
    if (!end)
        end = start + strlen(start);
    Node *expression = operand->expression;
    Symbol *reg_object = explicit_register_object(c, expression, NULL);
    Type *type = asm_operand_type(c, expression, reg_object, output);
    binding->width = type->size;
    if (expression->kind == N_SYMBOL) {
        binding->register_binding = expression->symbol->register_binding;
        binding->global_register = global_register(expression->symbol);
    }
    binding->match = -1;
    if (output && (size_t)(end - start) > 3 && !memcmp(start, "@cc", 3)) {
        const char *condition = copy(c, start + 3, (size_t)(end - start - 3));
        if (binding->read_write || binding->register_binding ||
            (type->kind != TY_INT && type->kind != TY_BOOL) ||
            type->size > 8 || !condition_ok(c, condition))
            return false;
        binding->condition = condition;
        binding->location = ASM_FLAGS;
        return true;
    }
    bool constant = false, allow_symbol = false, allow_number = false;
    uint64_t bits = 0;
    Symbol *symbol = NULL;
    if (!output && scalar(type))
        constant = address_constant(c, decay(c, operand->expression), &symbol, &bits);
    for (const char *p = start; p < end; p++) {
        char code = *p;
        if (isspace((unsigned char)code) || strchr("%?!*", code))
            continue;
        if (code == '#')
            break;
        if (code == '&' && output) {
            binding->early = true;
            continue;
        }
        if (code >= '0' && code <= '9' && !output) {
            const char *last = p + 1;
            while (last < end && isdigit((unsigned char)*last))
                last++;
            binding->match = register_suffix(p, last);
            if (binding->match < 0)
                return false;
            p = last - 1;
        } else if (code == '[' && !output) {
            const char *last = memchr(p + 1, ']', (size_t)(end - p - 1));
            if (!last)
                return false;
            binding->match = operand_named(assembly, p + 1, last, false);
            if (binding->match < 0)
                return false;
            p = last;
        } else if (code == '{') {
            const char *last = memchr(p + 1, '}', (size_t)(end - p - 1));
            if (!last)
                return false;
            int reg = named_register(c, p + 1, last);
            if (reg < 0)
                return false;
            choice->registers |= reg_bit((unsigned)reg);
            p = last;
        } else {
            uint64_t regs = registers_for(c, code, binding->global_register);
            bool mem = strchr("mogX", code) != NULL;
            bool imm = strchr("insgX", code) != NULL;
            bool machine = strchr(c->target == &x86_64_linux ? "IJKMNeZ" : "IJ", code) != NULL;
            if (!regs && !mem && !imm && !machine)
                return false;
            choice->registers |= regs;
            choice->memory |= mem && operand->addressable && !reg_object;
            allow_symbol |= code == 'i' || code == 's' || code == 'g' || code == 'X';
            allow_number |= (imm && code != 's') || (machine && immediate_fits(c, code, bits));
        }
    }
    if (constant && (symbol ? allow_symbol : allow_number) && binding->match < 0) {
        binding->location = ASM_IMMEDIATE;
        binding->symbol = symbol;
        binding->immediate = bits;
    }
    choice->registers &= registers_for_type(c, type, binding->global_register);
    if (binding->register_binding)
        choice->registers &= reg_bit(binding->register_binding - 1);
    if (!binding->global_register)
        choice->registers &= ~c->global_registers;
    return true;
}

static unsigned count_registers(uint64_t mask)
{
    unsigned n = 0;
    for (; mask; mask &= mask - 1)
        n++;
    return n;
}

static bool choose_locations(Context *c, AsmPlan *plan, unsigned alternative, uint64_t clobbered)
{
    InlineAssembly *assembly = plan->assembly;
    Choice choices[ASM_MAX_OPERANDS] = {0};
    bool tied[ASM_MAX_OPERANDS] = {0};
    size_t count = assembly->operands.len;
    for (size_t i = 0; i < count; i++) {
        plan->bindings[i] = (AsmBinding){0};
        if (!read_constraint(c, plan, i, alternative, &choices[i]))
            return false;
        if (plan->file_scope && plan->bindings[i].location != ASM_IMMEDIATE)
            return false;
    }
    for (size_t i = assembly->outputs; i < count; i++) {
        AsmBinding *binding = &plan->bindings[i];
        if (binding->match < 0)
            continue;
        size_t match = (size_t)binding->match;
        if (match >= assembly->outputs || tied[match] || plan->bindings[match].read_write ||
            plan->bindings[match].location == ASM_FLAGS ||
            binding->width != plan->bindings[match].width)
            return false;
        tied[match] = true;
        choices[match].memory = false;
        if (binding->register_binding && !binding->global_register)
            choices[match].registers &= reg_bit(binding->register_binding - 1);
    }
    uint64_t occupied = clobbered, inputs = 0, outputs = 0, reusable = 0;
    for (;;) {
        size_t best = count;
        unsigned fewest = 65;
        uint64_t best_mask = 0;
        bool best_memory = false;
        for (size_t i = 0; i < count; i++) {
            AsmBinding *binding = &plan->bindings[i];
            if (binding->location != ASM_UNBOUND || binding->match >= 0)
                continue;
            uint64_t registers = choices[i].registers & ~occupied;
            uint64_t memory = choices[i].memory ?
                              general_registers(c, false) & ~(occupied | c->global_registers) : 0;
            if (binding->global_register && i >= assembly->outputs)
                registers |= choices[i].registers & inputs;
            if (!registers && !memory && i >= assembly->outputs)
                registers = choices[i].registers & (reusable & ~inputs);
            if (!registers && !memory && i < assembly->outputs &&
                !binding->early && !binding->read_write && !tied[i])
                registers = choices[i].registers & (inputs & ~outputs);
            unsigned n = count_registers(registers | memory);
            if (!n)
                return false;
            if (n < fewest) {
                best = i;
                fewest = n;
                best_mask = registers ? registers : memory;
                best_memory = !registers;
            }
        }
        if (best == count)
            break;
        unsigned reg = 0;
        while (!(best_mask & reg_bit(reg)))
            reg++;
        uint64_t bit = reg_bit(reg);
        AsmBinding *binding = &plan->bindings[best];
        binding->reg = reg;
        binding->location = best_memory ? ASM_MEMORY : ASM_REGISTER;
        occupied |= bit;
        if (best < assembly->outputs && !best_memory) {
            outputs |= bit;
            if (!binding->early && !binding->read_write && !tied[best])
                reusable |= bit;
        } else if (!best_memory) {
            inputs |= bit;
        }
    }
    for (size_t i = assembly->outputs; i < count; i++) {
        AsmBinding *binding = &plan->bindings[i];
        if (binding->match >= 0) {
            binding->location = ASM_REGISTER;
            binding->reg = plan->bindings[(size_t)binding->match].reg;
            Node *expression = assembly->operands.data[i].expression;
            Type *type = asm_operand_type(c, expression, explicit_register_object(c, expression, NULL), false);
            if (!(registers_for_type(c, type, plan->bindings[(size_t)binding->match].global_register) &
                  reg_bit(binding->reg)))
                return false;
        }
    }
    plan->preserve = occupied & asm_call_saved_registers(c) & ~c->global_registers;
    return true;
}

bool plan_inline_asm(Context *c, AsmPlan *plan)
{
    InlineAssembly *assembly = plan->assembly;
    uint64_t clobbered = 0;
    for (size_t i = 0; i < assembly->clobbers.len; i++) {
        const char *name = assembly->clobbers.data[i];
        if (equal(name, "cc") || equal(name, "memory"))
            continue;
        int reg = named_register(c, name, name + strlen(name));
        bool fixed = c->target == &x86_64_linux ? reg == 4 || reg == 5 : reg == 29 || reg == 31;
        if (reg < 0 || fixed) {
            error(c, plan->loc, "invalid asm register clobber '%s'", name);
            return false;
        }
        clobbered |= reg_bit((unsigned)reg);
    }
    unsigned alternatives = 1;
    for (size_t i = 0; i < assembly->operands.len; i++) {
        unsigned n = 1;
        for (const char *p = assembly->operands.data[i].constraint; *p; p++)
            n += *p == ',';
        if (i && n != alternatives) {
            error(c, plan->loc, "asm operand constraints have different numbers of alternatives");
            return false;
        }
        alternatives = n;
    }
    for (unsigned i = 0; i < alternatives; i++)
        if (choose_locations(c, plan, i, clobbered))
            return true;
    error(c, plan->loc, "cannot satisfy extended asm operand constraints");
    return false;
}

static bool operand_text(Context *c, FILE *out, const AsmBinding *b, char modifier)
{
    bool amd = c->target == &x86_64_linux;
    if (b->location == ASM_IMMEDIATE) {
        if (!amd && (modifier == 'w' || modifier == 'x') && !b->symbol && !b->immediate) {
            fprintf(out, "%czr", modifier);
            return true;
        }
        if ((!amd && (modifier == 'w' || modifier == 'x')) ||
            (amd && modifier && strchr("bwkq", modifier)))
            modifier = 0;
        if (modifier && !strchr("cnpPl", modifier))
            return false;
        if (!modifier)
            fputc(amd ? '$' : '#', out);
        if (b->symbol) {
            if (modifier == 'n')
                return false;
            fputs(b->symbol->assembly, out);
            if (b->immediate)
                fprintf(out, "%+" PRId64, (int64_t)b->immediate);
            if (amd && modifier == 'P' && b->symbol->type->kind == TY_FUNC && !b->symbol->internal)
                fputs("@PLT", out);
        } else {
            uint64_t value = modifier == 'n' ? 0 - b->immediate : b->immediate;
            fprintf(out, "%" PRId64, (int64_t)value);
        }
        return true;
    }
    if (b->location == ASM_MEMORY) {
        if (amd) {
            if (modifier && modifier != 'a' && modifier != 'H')
                return false;
            fprintf(out, "%s(%%%s)", modifier == 'H' ? "8" : "", x86_asm_register(b->reg, 8));
        } else {
            if (modifier && modifier != 'a')
                return false;
            fprintf(out, "[x%u]", b->reg);
        }
        return true;
    }
    if (b->location != ASM_REGISTER)
        return false;
    if (b->reg >= 32) {
        if (amd) {
            if (modifier && modifier != 'x')
                return false;
            fprintf(out, "%%xmm%u", b->reg - 32);
        } else {
            if (modifier && !strchr("vqdshb", modifier))
                return false;
            fprintf(out, "%c%u", modifier ? modifier : 'v', b->reg - 32);
        }
        return true;
    }
    if (!amd) {
        if (modifier && !strchr("wxa", modifier))
            return false;
        char width = modifier == 'w' ? 'w' : 'x';
        if (modifier == 'a')
            fprintf(out, "[x%u]", b->reg);
        else
            fprintf(out, "%c%u", width, b->reg);
        return true;
    }
    unsigned width = b->width;
    switch (modifier) {
    case 'b': width = 1; break;
    case 'w': width = 2; break;
    case 'k': width = 4; break;
    case 'q': case 'V': width = 8; break;
    case 'a': fprintf(out, "(%%%s)", x86_asm_register(b->reg, 8)); return true;
    case 'h':
        if (b->reg >= 4)
            return false;
        fprintf(out, "%%%s", x86_high[b->reg]);
        return true;
    case 'z':
        fputc(width == 1 ? 'b' : width == 2 ? 'w' : width == 4 ? 'l' : 'q', out);
        return true;
    case 0: break;
    default: return false;
    }
    fprintf(out, "%s%s", modifier == 'V' ? "" : "%", x86_asm_register(b->reg, width));
    return true;
}

void emit_asm_template(Context *c, FILE *out, const AsmPlan *plan, unsigned function)
{
    const InlineAssembly *assembly = plan->assembly;
    size_t count = assembly->operands.len;
    const char *stop = NULL, *close = NULL;
    fputc('\t', out);
    for (const char *p = assembly->text; *p;) {
        if (p == stop) {
            p = close + 1;
            stop = close = NULL;
            continue;
        }
        if (*p == '{' && !stop && c->target == &x86_64_linux) {
            close = strchr(p + 1, '}');
            const char *bar = strchr(p + 1, '|');
            if (bar && close && bar < close) {
                stop = bar;
                p++;
                continue;
            }
        }
        if (*p != '%') {
            fputc(*p++, out);
            continue;
        }
        p++;
        if (*p && strchr("%{|}", *p)) {
            fputc(*p++, out);
            continue;
        }
        if (*p == '=') {
            fprintf(out, "%u", plan->serial);
            p++;
            continue;
        }
        char modifier = isalpha((unsigned char)*p) ? *p++ : 0;
        bool label = modifier == 'l' && assembly->labels.len;
        int index;
        if (*p == '[') {
            const char *end = strchr(++p, ']');
            if (!end) {
                error(c, plan->loc, "unterminated asm operand reference");
                return;
            }
            index = operand_named(assembly, p, end, label);
            p = end + 1;
        } else {
            const char *begin = p;
            while (isdigit((unsigned char)*p))
                p++;
            index = register_suffix(begin, p);
            if (index >= 0 && label)
                index -= (int)(count + assembly->extra_inputs);
        }
        if (label && index >= 0 && (size_t)index < assembly->labels.len) {
            fprintf(out, ".L%u_%zu", function, plan->labels[(size_t)index]);
            continue;
        }
        if (!label && index >= 0 && (size_t)index >= count) {
            size_t implicit = (size_t)index - count;
            index = -1;
            for (size_t i = 0; i < assembly->outputs; i++) {
                if (plan->bindings[i].read_write && implicit-- == 0) {
                    index = (int)i;
                    break;
                }
            }
        }
        if (label || index < 0 || (size_t)index >= count ||
            !operand_text(c, out, &plan->bindings[(size_t)index], modifier)) {
            error(c, plan->loc, "invalid extended asm operand reference or modifier");
            return;
        }
    }
    fputc('\n', out);
}
