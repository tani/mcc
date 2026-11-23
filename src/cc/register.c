#include "register.h"

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
    if ((aggregate(type) || type->kind == TY_VECTOR) && type->complete) {
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
