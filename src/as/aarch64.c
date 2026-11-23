#include "as.h"

#include <string.h>
#include <strings.h>

typedef struct { unsigned id; char kind; bool stack; } Register;

static bool reg(const char *text, Register *result)
{
    if (equal(text, "sp") || equal(text, "wsp")) {
        *result = (Register){31, *text == 's' ? 'x' : 'w', true};
        return true;
    }
    if (equal(text, "xzr") || equal(text, "wzr")) {
        *result = (Register){31, *text, false};
        return true;
    }
    uint64_t n;
    if (!*text || !strchr("xwhsdq", *text) || !mas_integer(text + 1, &n) || n > 31 ||
        (n == 31 && (*text == 'x' || *text == 'w')))
        return false;
    *result = (Register){(unsigned)n, *text, false};
    return true;
}

static bool gp(Register r)
{
    return r.kind == 'x' || r.kind == 'w';
}

static bool immediate(const char *text, uint64_t *result)
{
    return mas_integer(text + (*text == '#'), result);
}

static uint32_t sf(Register r)
{
    return r.kind == 'x' ? UINT32_C(0x80000000) : 0;
}

static uint32_t regs(Register d, Register n, Register m)
{
    return (m.id << 16) | (n.id << 5) | d.id;
}

static int condition(const char *name)
{
    static const char *const names[] = {
        "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"
    };
    if (equal(name, "cs"))
        return 2;
    if (equal(name, "cc"))
        return 3;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (equal(name, names[i]))
            return (int)i;
    return -1;
}

static bool target_word(Assembler *a, char *operand, uint32_t word, uint32_t type)
{
    AsmExpr target;
    if (!mas_expression(a, operand, &target) || !target.symbol)
        return false;
    mas_relocate(a, type, target);
    mas_emit(a, word, 4);
    return true;
}

static bool branch(Assembler *a, const char *op, AsmArgs *args)
{
    const size_t count = args->len;
    char **p = args->data;
    if (count == 1 && (equal(op, "b") || equal(op, "bl")))
        return target_word(a, p[0], equal(op, "b") ? 0x14000000 : 0x94000000,
                           equal(op, "b") ? R_AARCH64_JUMP26 : R_AARCH64_CALL26);
    if (count == 1 && !strncmp(op, "b.", 2)) {
        int cond = condition(op + 2);
        return cond >= 0 && target_word(a, p[0], UINT32_C(0x54000000) | (unsigned)cond,
                                        R_AARCH64_CONDBR19);
    }
    Register n;
    if (count == 2 && (equal(op, "cbz") || equal(op, "cbnz")) && reg(p[0], &n) && gp(n) && !n.stack)
        return target_word(a, p[1], (equal(op, "cbz") ? 0x34000000 : 0x35000000) | sf(n) | n.id,
                           R_AARCH64_CONDBR19);
    uint64_t bit;
    if (count == 3 && (equal(op, "tbz") || equal(op, "tbnz")) && reg(p[0], &n) && gp(n) &&
        !n.stack && immediate(p[1], &bit) && bit < (n.kind == 'x' ? 64u : 32u))
        return target_word(a, p[2], (equal(op, "tbz") ? 0x36000000 : 0x37000000) |
                           ((uint32_t)(bit & 32) << 26) | ((uint32_t)(bit & 31) << 19) | n.id,
                           R_AARCH64_TSTBR14);
    bool returns = equal(op, "ret");
    if (!returns && !equal(op, "br") && !equal(op, "blr"))
        return false;
    n = (Register){30, 'x', false};
    if (count == 1 ? !reg(p[0], &n) || n.kind != 'x' || n.stack : count || !returns)
        return false;
    uint32_t word = returns ? 0xd65f0000 : equal(op, "br") ? 0xd61f0000 : 0xd63f0000;
    mas_emit(a, word | (n.id << 5), 4);
    return true;
}

static uint32_t move_wide(Register dest, unsigned part, unsigned shift, uint32_t opcode)
{
    return opcode | sf(dest) | ((shift / 16) << 21) | (part << 5) | dest.id;
}

static bool move(Assembler *a, const char *op, AsmArgs *args)
{
    Register dest, src;
    char **p = args->data;
    if (args->len < 2 || args->len > 3 || !reg(p[0], &dest) || !gp(dest))
        return false;
    bool plain = equal(op, "mov");
    if (plain && args->len == 2 && reg(p[1], &src) && src.kind == dest.kind) {
        uint32_t word = dest.stack || src.stack ? 0x11000000 | (src.id << 5) :
                                                 0x2a0003e0 | (src.id << 16);
        if ((dest.stack || src.stack) && ((dest.id == 31 && !dest.stack) || (src.id == 31 && !src.stack)))
            return false;
        mas_emit(a, word | sf(dest) | dest.id, 4);
        return true;
    }
    uint64_t value, shift = 0;
    unsigned width = dest.kind == 'x' ? 64 : 32;
    if (dest.stack || !immediate(p[1], &value))
        return false;
    if (plain) {
        if (args->len != 2 || (width == 32 && value > UINT32_MAX &&
                              !fits_signed((int64_t)value, 32)))
            return false;
        mas_emit(a, move_wide(dest, (unsigned)(value & 65535), 0, 0x52800000), 4);
        for (unsigned at = 16; at < width; at += 16) {
            unsigned part = (unsigned)((value >> at) & 65535);
            if (part)
                mas_emit(a, move_wide(dest, part, at, 0x72800000), 4);
        }
        return true;
    }
    if (args->len == 3 && (strncmp(p[2], "lsl ", 4) ||
                           !immediate(mas_trim(p[2] + 4), &shift)))
        return false;
    if (value > 65535 || shift >= width || shift % 16)
        return false;
    uint32_t opcode = equal(op, "movk") ? 0x72800000 : equal(op, "movn") ? 0x12800000 : 0x52800000;
    mas_emit(a, move_wide(dest, (unsigned)value, (unsigned)shift, opcode), 4);
    return true;
}

static bool logical_immediate(uint64_t value, unsigned width, uint32_t *bits)
{
    if (width == 32)
        value &= UINT32_MAX;
    for (unsigned size = 2; size <= width; size *= 2) {
        uint64_t mask = UINT64_MAX >> (64 - size);
        uint64_t pattern = value & mask, repeated = pattern;
        for (unsigned i = size; i < width; i += size)
            repeated |= pattern << i;
        if (repeated != value)
            continue;
        for (unsigned count = 1; count < size; count++) {
            uint64_t ones = (UINT64_C(1) << count) - 1;
            for (unsigned rotate = 0; rotate < size; rotate++) {
                uint64_t encoded = rotate ? ((ones >> rotate) | (ones << (size - rotate))) & mask : ones;
                if (encoded == pattern) {
                    *bits = ((size == 64 ? 1u : 0u) << 22) | (rotate << 16) |
                            (((0u - 2 * size) | (count - 1)) & 63u) << 10;
                    return true;
                }
            }
        }
    }
    return false;
}

typedef struct { const char *name; uint32_t word, immediate; } Arithmetic;
static const Arithmetic arithmetic_ops[] = {
    {"add", 0x0b000000, 0x11000000}, {"sub", 0x4b000000, 0x51000000},
    {"adds", 0x2b000000, 0x31000000}, {"subs", 0x6b000000, 0x71000000},
    {"and", 0x0a000000, 0x12000000}, {"orr", 0x2a000000, 0x32000000},
    {"eor", 0x4a000000, 0x52000000}, {"mul", 0x1b007c00, 0},
    {"lsl", 0x1ac02000, 0}, {"lsr", 0x1ac02400, 0}, {"asr", 0x1ac02800, 0},
    {"udiv", 0x1ac00800, 0}, {"sdiv", 0x1ac00c00, 0},
};

static bool arithmetic(Assembler *a, const char *op, AsmArgs *args)
{
    bool compare = equal(op, "cmp");
    const Arithmetic *operation = NULL;
    for (size_t i = 0; i < sizeof arithmetic_ops / sizeof *arithmetic_ops; i++)
        if (equal(compare ? "subs" : op, arithmetic_ops[i].name)) {
            operation = &arithmetic_ops[i];
            break;
        }
    if (!operation || args->len < (compare ? 2u : 3u) || args->len > (compare ? 3u : 4u))
        return false;
    char **p = args->data;
    Register d, n, m;
    if (!reg(p[compare ? 0 : 1], &n) || !gp(n))
        return false;
    if (compare)
        d = (Register){31, n.kind, false};
    else if (!reg(p[0], &d) || d.kind != n.kind)
        return false;
    const char *operand = p[compare ? 1 : 2];
    bool shifted = args->len == (compare ? 3u : 4u);
    bool add_sub = operation->immediate && (operation->word & 0x0f000000) == 0x0b000000;
    bool sets_flags = compare || equal(op, "adds") || equal(op, "subs");
    uint64_t width = d.kind == 'x' ? 64 : 32;
    uint32_t word = sf(d) | (n.id << 5) | d.id;
    if (reg(operand, &m)) {
        if (m.kind != n.kind || m.stack)
            return false;
        word |= operation->word | (m.id << 16);
        uint64_t shift = 0;
        if (shifted) {
            char *value = p[args->len - 1];
            if ((!add_sub && !operation->immediate) || strncmp(value, "lsl ", 4) ||
                !immediate(mas_trim(value + 4), &shift) || shift >= width)
                return false;
        }
        if (d.stack || n.stack) {
            if (!add_sub || shift > 4 || (sets_flags && d.stack) || (n.id == 31 && !n.stack))
                return false;
            word |= 0x00200000 | (d.kind == 'x' ? 3u : 2u) << 13;
        }
        word |= (uint32_t)shift << 10;
    } else {
        char *expr = p[compare ? 1 : 2] + (*operand == '#');
        uint64_t value;
        if (shifted)
            return false;
        if (add_sub) {
            word |= operation->immediate;
            if ((n.id == 31 && !n.stack) || (!sets_flags && d.id == 31 && !d.stack) ||
                (sets_flags && d.stack))
                return false;
            bool tls = !strncmp(expr, ":tlsdesc_lo12:", 14);
            if (equal(op, "add") && (tls || !strncmp(expr, ":lo12:", 6))) {
                AsmExpr address;
                if (!mas_expression(a, expr + (tls ? 14 : 6), &address) || !address.symbol)
                    return false;
                mas_relocate(a, tls ? R_AARCH64_TLSDESC_ADD_LO12 : R_AARCH64_ADD_ABS_LO12_NC, address);
            } else if (immediate(operand, &value) && value <= 4095) {
                word |= (uint32_t)value << 10;
            } else {
                return false;
            }
        } else if (immediate(operand, &value) && !d.stack && !n.stack) {
            if (operation->immediate) {
                uint32_t bits;
                if (!logical_immediate(value, (unsigned)width, &bits))
                    return false;
                word |= operation->immediate | bits;
            } else if ((equal(op, "lsr") || equal(op, "lsl") || equal(op, "asr")) && value < width) {
                unsigned shift = (unsigned)value;
                unsigned rotate = equal(op, "lsl") ? (0u - shift) & ((unsigned)width - 1) : shift;
                unsigned last = (unsigned)width - 1 - (equal(op, "lsl") ? shift : 0);
                word |= (equal(op, "asr") ? 0x13000000 : 0x53000000) |
                        (d.kind == 'x' ? 0x00400000 : 0) | (rotate << 16) | (last << 10);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    mas_emit(a, word, 4);
    return true;
}

typedef struct { Register base; char *offset; bool pre; } Address;

static bool address(char *text, Address *result)
{
    char *end = strchr(text, ']');
    if (*text != '[' || !end || (end[1] && !equal(end + 1, "!")))
        return false;
    result->pre = end[1] == '!';
    *end = 0;
    char *comma = strchr(++text, ',');
    if (comma)
        *comma = 0;
    result->offset = comma ? mas_trim(comma + 1) : NULL;
    return reg(mas_trim(text), &result->base) && result->base.kind == 'x' &&
           (result->base.id < 31 || result->base.stack);
}

static bool prefetch(Assembler *a, AsmArgs *args)
{
    static const char *const operations[32] = {
        [0] = "pldl1keep", "pldl1strm", "pldl2keep", "pldl2strm", "pldl3keep", "pldl3strm",
        [8] = "plil1keep", "plil1strm", "plil2keep", "plil2strm", "plil3keep", "plil3strm",
        [16] = "pstl1keep", "pstl1strm", "pstl2keep", "pstl2strm", "pstl3keep", "pstl3strm",
    };
    Address mem;
    uint64_t offset = 0;
    if (args->len != 2 || !address(args->data[1], &mem) || mem.pre ||
        (mem.offset && (!immediate(mem.offset, &offset) || offset > 32760 || offset % 8)))
        return false;
    uint64_t operation;
    if (!immediate(args->data[0], &operation)) {
        for (operation = 0; operation < 32; operation++)
            if (operations[operation] && !strcasecmp(args->data[0], operations[operation]))
                break;
    }
    if (operation >= 32)
        return false;
    mas_emit(a, UINT32_C(0xf9800000) | ((uint32_t)(offset / 8) << 10) |
             (mem.base.id << 5) | (uint32_t)operation, 4);
    return true;
}

static bool memory(Assembler *a, const char *op, AsmArgs *args)
{
    bool pair = equal(op, "stp") || equal(op, "ldp");
    bool load = !strncmp(op, "ld", 2);
    bool narrow = equal(op, "ldrb") || equal(op, "strb") || equal(op, "ldrh") ||
                  equal(op, "strh") || equal(op, "ldrsb") || equal(op, "ldrsh") || equal(op, "ldrsw");
    if (!pair && !narrow && !equal(op, "ldr") && !equal(op, "str"))
        return false;
    unsigned at = pair ? 2u : 1u;
    Register first, second = {0};
    Address mem;
    char **p = args->data;
    if (args->len < at + 1 || args->len > at + 2 || !reg(p[0], &first) || first.stack ||
        (pair && (!reg(p[1], &second) || second.stack || first.kind != second.kind)) ||
        !address(p[at], &mem))
        return false;
    bool post = args->len == at + 2;
    if (post && (mem.pre || mem.offset))
        return false;
    const char *offset = post ? p[at + 1] : mem.offset;
    uint32_t word;
    unsigned scale = first.kind == 'q' ? 16 : first.kind == 'x' || first.kind == 'd' ? 8 :
                     first.kind == 'h' ? 2 : 4;
    if (pair) {
        if (first.kind != 'x' && first.kind != 'w' && first.kind != 'q')
            return false;
        word = first.kind == 'x' ? 0xa9000000 : first.kind == 'w' ? 0x29000000 : 0xad000000;
        if (load)
            word |= 0x00400000;
        if (post)
            word -= 0x00800000;
        if (mem.pre)
            word += 0x00800000;
        uint64_t raw = 0;
        if ((offset && !immediate(offset, &raw)) || (int64_t)raw % (int64_t)scale ||
            !fits_signed((int64_t)raw / (int64_t)scale, 7))
            return false;
        word |= ((uint32_t)((int64_t)raw / (int64_t)scale) & 127) << 15 | (second.id << 10);
    } else {
        word = first.kind == 'w' ? 0xb9400000 : first.kind == 'x' ? 0xf9400000 : first.kind == 'h' ? 0x7d400000 :
               first.kind == 's' ? 0xbd400000 : first.kind == 'd' ? 0xfd400000 : 0x3dc00000;
        if (narrow) {
            if (!gp(first))
                return false;
            if (equal(op, "ldrb") || equal(op, "strb")) {
                if (first.kind != 'w')
                    return false;
                word = 0x39400000;
                scale = 1;
            } else if (equal(op, "ldrh") || equal(op, "strh")) {
                if (first.kind != 'w')
                    return false;
                word = 0x79400000;
                scale = 2;
            } else if (equal(op, "ldrsw")) {
                if (first.kind != 'x')
                    return false;
                word = 0xb9800000;
                scale = 4;
            } else {
                scale = equal(op, "ldrsb") ? 1 : 2;
                word = scale == 1 ? 0x39800000 : 0x79800000;
                if (first.kind == 'w')
                    word |= 0x00400000;
            }
        }
        if (!load)
            word &= ~UINT32_C(0x00400000);
        char *expr = mem.offset ? mem.offset + (*mem.offset == '#') : NULL;
        bool tls = expr && !strncmp(expr, ":tlsdesc_lo12:", 14);
        if (!post && !mem.pre && expr && (tls || !strncmp(expr, ":got_lo12:", 10)) &&
            equal(op, "ldr") && first.kind == 'x') {
            AsmExpr target;
            if (!mas_expression(a, expr + (tls ? 14 : 10), &target) || !target.symbol)
                return false;
            mas_relocate(a, tls ? R_AARCH64_TLSDESC_LD64_LO12 : R_AARCH64_LD64_GOT_LO12_NC, target);
        } else {
            uint64_t raw = 0;
            if (offset && !immediate(offset, &raw))
                return false;
            if (!mem.pre && !post && raw % scale == 0 && raw / scale < 4096) {
                word |= (uint32_t)(raw / scale) << 10;
            } else if (fits_signed((int64_t)raw, 9)) {
                word = (word & ~UINT32_C(0x01000000)) | (((uint32_t)raw & 511) << 12) |
                       (mem.pre ? 0x0c00u : post ? 0x0400u : 0);
            } else {
                return false;
            }
        }
    }
    mas_emit(a, word | (mem.base.id << 5) | first.id, 4);
    return true;
}

static bool system_register_access(Assembler *a, const char *op, AsmArgs *args)
{
    bool read = equal(op, "mrs");
    Register reg64;
    if (args->len != 2 || !reg(args->data[read ? 0 : 1], &reg64) ||
        reg64.kind != 'x' || reg64.stack)
        return false;
    const char *name = args->data[read ? 1 : 0];
    static const struct { const char *name; uint32_t field; bool writable; } registers[] = {
        {"tpidr_el0", 0x1bd040, true}, {"ctr_el0", 0x1b0020, false},
        {"fpcr", 0x1b4400, true}, {"fpsr", 0x1b4420, true},
    };
    for (size_t i = 0; i < sizeof registers / sizeof *registers; i++) {
        if (equal(name, registers[i].name) && (read || registers[i].writable)) {
            mas_emit(a, (read ? 0xd5200000 : 0xd5000000) | registers[i].field | reg64.id, 4);
            return true;
        }
    }
    return false;
}

static bool integer_misc(Assembler *a, const char *op, AsmArgs *args)
{
    char **p = args->data;
    Register d, n, m, r;
    if (!args->len || !reg(p[0], &d) || !gp(d) || d.stack)
        return false;
    if (args->len == 2 && equal(op, "cset")) {
        int cond = condition(p[1]);
        if (cond < 0)
            return false;
        mas_emit(a, UINT32_C(0x1a9f07e0) | sf(d) | ((unsigned)(cond ^ 1) << 12) | d.id, 4);
        return true;
    }
    if (args->len == 2 && (equal(op, "adr") || equal(op, "adrp")) && d.kind == 'x') {
        bool page = equal(op, "adrp");
        if (page && !strncmp(p[1], ":got:", 5))
            return target_word(a, p[1] + 5, 0x90000000 | d.id, R_AARCH64_ADR_GOT_PAGE);
        if (page && !strncmp(p[1], ":tlsdesc:", 9))
            return target_word(a, p[1] + 9, 0x90000000 | d.id, R_AARCH64_TLSDESC_ADR_PAGE21);
        return target_word(a, p[1], (page ? 0x90000000 : 0x10000000) | d.id,
                           page ? R_AARCH64_ADR_PREL_PG_HI21 : R_AARCH64_ADR_PREL_LO21);
    }
    if (args->len < 2 || !reg(p[1], &n) || !gp(n) || n.stack)
        return false;
    if (args->len == 2) {
        if ((equal(op, "neg") || equal(op, "mvn")) && n.kind == d.kind) {
            uint32_t word = equal(op, "neg") ? 0x4b0003e0 : 0x2a2003e0;
            mas_emit(a, word | sf(d) | (n.id << 16) | d.id, 4);
            return true;
        }
        if (strlen(op) == 4 && !strncmp(op + 1, "xt", 2) && (*op == 's' || *op == 'u') &&
            strchr("bhw", op[3]) && n.kind == 'w') {
            uint32_t word = *op == 's' ? 0x13000000 : 0x53000000;
            unsigned last = op[3] == 'b' ? 7u : op[3] == 'h' ? 15u : 31u;
            if (d.kind == 'w' && op[3] == 'w')
                return false;
            mas_emit(a, word | sf(d) | (d.kind == 'x' ? 0x00400000 : 0) |
                     (last << 10) | (n.id << 5) | d.id, 4);
            return true;
        }
    }
    if (args->len == 4 && n.kind == d.kind) {
        if (equal(op, "msub") && reg(p[2], &m) && reg(p[3], &r) &&
            m.kind == d.kind && r.kind == d.kind && !m.stack && !r.stack) {
            mas_emit(a, UINT32_C(0x1b008000) | sf(d) | regs(d, n, m) | (r.id << 10), 4);
            return true;
        }
        bool insert = equal(op, "bfi");
        if (insert || equal(op, "ubfx") || equal(op, "sbfx")) {
            uint64_t bit, count, width = d.kind == 'x' ? 64 : 32;
            if (!immediate(p[2], &bit) || !immediate(p[3], &count) || bit >= width ||
                !count || count > width - bit)
                return false;
            uint32_t word = insert ? 0x33000000 : equal(op, "ubfx") ? 0x53000000 : 0x13000000;
            unsigned rotate = (unsigned)(insert ? (0 - bit) & (width - 1) : bit);
            unsigned last = (unsigned)((insert ? 0 : bit) + count - 1);
            mas_emit(a, word | sf(d) | (d.kind == 'x' ? 0x00400000 : 0) |
                     (rotate << 16) | (last << 10) | (n.id << 5) | d.id, 4);
            return true;
        }
    }
    return false;
}

static bool fp(Register r)
{
    return r.kind == 's' || r.kind == 'd';
}

static bool floating(Assembler *a, const char *op, AsmArgs *args)
{
    char **p = args->data;
    Register d, n, m;
    if (args->len < 2 || args->len > 3 || !reg(p[0], &d))
        return false;
    uint32_t word = d.kind == 'd' ? 0x00400000 : 0;
    if (args->len == 2 && fp(d) && equal(op, "fcmp")) {
        word |= (d.id << 5) | 0x1e202000;
        if (reg(p[1], &m) && m.kind == d.kind)
            word |= m.id << 16;
        else if (equal(p[1], "#0.0") || equal(p[1], "#0"))
            word |= 8;
        else
            return false;
    } else if (args->len == 2 && reg(p[1], &n)) {
        if (fp(d) && n.kind == d.kind && equal(op, "fneg")) {
            word |= 0x1e214000;
        } else if (fp(d) && fp(n) && n.kind != d.kind && equal(op, "fcvt")) {
            word = d.kind == 's' ? 0x1e624000 : 0x1e22c000;
        } else if (fp(d) && gp(n) && !n.stack && (equal(op, "scvtf") || equal(op, "ucvtf"))) {
            word |= sf(n) | (equal(op, "scvtf") ? 0x1e220000 : 0x1e230000);
        } else if (gp(d) && !d.stack && fp(n) && (equal(op, "fcvtzs") || equal(op, "fcvtzu"))) {
            word = sf(d) | (n.kind == 'd' ? 0x00400000 : 0) |
                   (equal(op, "fcvtzs") ? 0x1e380000 : 0x1e390000);
        } else {
            return false;
        }
        word |= (n.id << 5) | d.id;
    } else if (args->len == 3 && fp(d) && reg(p[1], &n) && reg(p[2], &m) &&
               n.kind == d.kind && m.kind == d.kind) {
        uint32_t code = equal(op, "fadd") ? 0x1e202800 : equal(op, "fsub") ? 0x1e203800 :
                        equal(op, "fmul") ? 0x1e200800 : equal(op, "fdiv") ? 0x1e201800 : 0;
        if (!code)
            return false;
        word |= code | regs(d, n, m);
    } else {
        return false;
    }
    mas_emit(a, word, 4);
    return true;
}

bool mas_aarch64(Assembler *a, const char *op, AsmArgs *args)
{
    if (a->sections.data[a->section].size % 4)
        return false;
    if (!args->len && equal(op, "nop")) {
        mas_emit(a, 0xd503201f, 4);
        return true;
    }
    bool full_barrier = args->len == 1 && equal(args->data[0], "sy");
    if (full_barrier && equal(op, "dsb")) {
        mas_emit(a, 0xd5033f9f, 4);
        return true;
    }
    if ((!args->len || full_barrier) && equal(op, "isb")) {
        mas_emit(a, 0xd5033fdf, 4);
        return true;
    }
    if (args->len == 2 && equal(op, "movi") && *args->data[0] == 'v') {
        char *shape = strchr(args->data[0], '.');
        if (!shape || !equal(shape, ".16b"))
            return false;
        *shape = 0;
        uint64_t dest, value;
        if (!mas_integer(args->data[0] + 1, &dest) || dest > 31 ||
            !immediate(args->data[1], &value) || value > 255)
            return false;
        mas_emit(a, UINT32_C(0x4f00e400) | ((uint32_t)(value & 224) << 11) |
                 ((uint32_t)(value & 31) << 5) | (uint32_t)dest, 4);
        return true;
    }
    if (args->len == 1 && (equal(op, "svc") || equal(op, "brk"))) {
        uint64_t value;
        if (!immediate(args->data[0], &value) || value > 65535)
            return false;
        mas_emit(a, (equal(op, "svc") ? 0xd4000001 : 0xd4200000) | ((uint32_t)value << 5), 4);
        return true;
    }
    if (equal(op, "mov") || equal(op, "movz") || equal(op, "movk") || equal(op, "movn"))
        return move(a, op, args);
    if (equal(op, "prfm"))
        return prefetch(a, args);
    if (equal(op, "mrs") || equal(op, "msr"))
        return system_register_access(a, op, args);
    return branch(a, op, args) || arithmetic(a, op, args) || memory(a, op, args) ||
           integer_misc(a, op, args) || floating(a, op, args);
}
