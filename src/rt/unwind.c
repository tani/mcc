#include "types.h"

/* The C unwind ABI used by static glibc for backtraces and thread cleanup. */
typedef struct UnwindContext UnwindContext;
typedef struct UnwindException UnwindException;
typedef int (*Stop)(int, int, uint64_t, UnwindException *, UnwindContext *, void *);
typedef int (*Trace)(UnwindContext *, void *);
typedef int (*Personality)(int, int, uint64_t, UnwindException *, UnwindContext *);

struct UnwindException {
    uint64_t exception_class;
    void (*cleanup)(int, UnwindException *);
    Stop stop;
    void *argument;
};

#if defined(__aarch64__)
enum { REG_COUNT = 80, STACK = 31, PC = 32 };
uintptr_t mcc_strip_pac(uintptr_t);
#else
enum { REG_COUNT = 17, STACK = 7, PC = 16 };
#endif

struct UnwindContext {
    uintptr_t regs[REG_COUNT];
    uintptr_t cfa, start, map_end;
    const uint8_t *lsda;
    bool signal;
};

enum {
    NO_REASON = 0, FATAL_PHASE2 = 2, END_OF_STACK = 5, INSTALL_CONTEXT = 7, CONTINUE_UNWIND = 8,
    CLEANUP_PHASE = 2, FORCE_UNWIND = 8, END_ACTION = 16,
};

/* AArch64 and x86-64 share this prefix of glibc's dl_find_object result. */
struct dl_find_object {
    uint64_t flags;
    void *map_start, *map_end, *link_map, *eh_frame;
    uint64_t reserved[12];
};

int _dl_find_object(void *, struct dl_find_object *);
#if defined(__aarch64__)
unsigned long getauxval(unsigned long);
#endif
_Noreturn void abort(void);
void mcc_capture_frame(uintptr_t regs[REG_COUNT]);
_Noreturn void mcc_install_frame(const uintptr_t regs[REG_COUNT]);
uintptr_t _Unwind_GetCFA(UnwindContext *);
uintptr_t _Unwind_GetIP(UnwindContext *);
int _Unwind_Backtrace(Trace, void *);
int _Unwind_ForcedUnwind(UnwindException *, Stop, void *);
_Noreturn void _Unwind_Resume(UnwindException *);
int __gcc_personality_v0(int, int, uint64_t, UnwindException *, UnwindContext *);

typedef struct { const uint8_t *at, *end; bool failed; } Reader;
typedef struct { uintptr_t lo, hi; } Bounds;
typedef struct {
    Reader program;
    unsigned code_align, return_register, pointer_encoding, lsda_encoding;
    int64_t data_align;
    uintptr_t personality;
    bool augmentation_length, signal;
} Cie;
typedef struct {
    Reader program;
    Cie cie;
    uintptr_t start, end;
    const uint8_t *lsda;
    Bounds bounds;
} Frame;

enum { OMIT = 0xff };
enum { SAME, UNDEFINED, OFFSET, REGISTER, VALUE_OFFSET, EXPRESSION, VALUE_EXPRESSION };

typedef struct { const uint8_t *expression; int64_t value; unsigned kind; } Rule;
typedef struct { Rule regs[REG_COUNT]; Rule cfa; unsigned cfa_register; bool signed_pc; } State;

static bool available(Reader *r, uint64_t count)
{
    if (r->failed || (uintptr_t)r->at > (uintptr_t)r->end || count > (uintptr_t)r->end - (uintptr_t)r->at) {
        r->failed = true;
        return false;
    }
    return true;
}

static uint64_t fixed(Reader *r, unsigned n)
{
    uint64_t value = 0;
    if (available(r, n)) {
        for (unsigned i = 0; i < n; i++)
            value |= (uint64_t)*r->at++ << (i * 8);
    }
    return value;
}

static uint64_t leb(Reader *r, bool signed_value)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64 && !r->failed; shift += 7) {
        unsigned byte = (unsigned)fixed(r, 1);
        value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128)) {
            if (signed_value && (byte & 64) && shift + 7 < 64)
                value |= UINT64_MAX << (shift + 7);
            return value;
        }
    }
    r->failed = true;
    return 0;
}

static Reader subrange(Reader *r, uint64_t n)
{
    Reader sub = {r->at, r->at, !available(r, n)};
    if (!sub.failed) {
        r->at += n;
        sub.end = r->at;
    }
    return sub;
}

static uintptr_t address(Reader *r, unsigned encoding, uintptr_t data, uintptr_t function, Bounds bounds)
{
    uintptr_t base = (uintptr_t)r->at;
    uint64_t value;
    unsigned width;
    switch (encoding & 15) {
    case 0: case 8: width = 8; break;
    case 1: case 9: width = 0; break;
    case 2: case 10: width = 2; break;
    case 3: case 11: width = 4; break;
    case 4: case 12: width = 8; break;
    default: r->failed = true; return 0;
    }
    if (!width) {
        value = leb(r, (encoding & 8) != 0);
    } else {
        value = fixed(r, width);
        if ((encoding & 8) && width < 8 && (value >> (width * 8 - 1)))
            value |= UINT64_MAX << (width * 8);
    }
    if (!value)
        return 0;
    switch (encoding & 0x70) {
    case 0: break;
    case 0x10: value += base; break;
    case 0x30: value += data; break;
    case 0x40: value += function; break;
    default: r->failed = true; return 0;
    }
    if (!(encoding & 0x80))
        return value;
    if (value < bounds.lo || value > bounds.hi || bounds.hi - value < sizeof value) {
        struct dl_find_object segment;
        if (_dl_find_object((void *)value, &segment) != 0 || (uintptr_t)segment.map_end < value ||
            (uintptr_t)segment.map_end - value < sizeof value) {
            r->failed = true;
            return 0;
        }
    }
    uint64_t indirect;
    memcpy(&indirect, (const void *)value, sizeof indirect);
    return indirect;
}

static Reader record(Reader *r, unsigned *width)
{
    uint64_t size = fixed(r, 4);
    *width = 4;
    if (size == UINT32_MAX) {
        size = fixed(r, 8);
        *width = 8;
    }
    return subrange(r, size);
}

static bool read_cie(const uint8_t *start, const uint8_t *end, Bounds bounds, Cie *cie)
{
    Reader entry = {start, end, false};
    unsigned width;
    Reader r = record(&entry, &width);
    if (fixed(&r, width) != 0)
        return false;
    unsigned version = (unsigned)fixed(&r, 1);
    const uint8_t *augmentation = r.at;
    while (fixed(&r, 1) != 0)
        ;
    if (version == 4 && (fixed(&r, 1) != 8 || fixed(&r, 1) != 0))
        return false;
    if (version != 1 && version != 3 && version != 4)
        return false;
    *cie = (Cie){.code_align = (unsigned)leb(&r, false), .lsda_encoding = OMIT};
    cie->data_align = (int64_t)leb(&r, true);
    cie->return_register = (unsigned)(version == 1 ? fixed(&r, 1) : leb(&r, false));
    if (!r.failed && *augmentation == 'z') {
        uint64_t length = leb(&r, false);
        Reader aug = subrange(&r, length);
        cie->augmentation_length = true;
        for (const uint8_t *a = augmentation + 1; *a; a++) {
            unsigned encoding;
            switch (*a) {
            case 'L': cie->lsda_encoding = (unsigned)fixed(&aug, 1); break;
            case 'R': cie->pointer_encoding = (unsigned)fixed(&aug, 1); break;
            case 'P':
                encoding = (unsigned)fixed(&aug, 1);
                cie->personality = address(&aug, encoding, bounds.lo, 0, bounds);
                break;
            case 'S': cie->signal = true; break;
            case 'B': break; /* PAC key selection does not affect stripping the signature. */
            default: return false;
            }
        }
        if (aug.failed)
            return false;
    } else if (!r.failed && *augmentation) {
        return false;
    }
    cie->program = r;
    return !r.failed && cie->return_register < REG_COUNT;
}

static bool find_frame(uintptr_t pc, Frame *frame)
{
    struct dl_find_object object;
    if (_dl_find_object((void *)pc, &object) != 0 || !object.eh_frame)
        return false;
    Bounds bounds = {(uintptr_t)object.map_start, (uintptr_t)object.map_end};
    uintptr_t header = (uintptr_t)object.eh_frame;
    if (header < bounds.lo || header >= bounds.hi) {
        /* Static executables can have separate executable and read-only mappings. */
        struct dl_find_object segment;
        if (_dl_find_object(object.eh_frame, &segment) != 0)
            return false;
        bounds = (Bounds){(uintptr_t)segment.map_start, (uintptr_t)segment.map_end};
        if (header < bounds.lo || header >= bounds.hi)
            return false;
    }
    Reader r = {object.eh_frame, (const uint8_t *)bounds.hi, false};
    if (fixed(&r, 1) != 1)
        return false;
    unsigned encoding = (unsigned)fixed(&r, 1);
    fixed(&r, 2);
    uintptr_t eh = address(&r, encoding, header, 0, bounds);
    if (r.failed || eh < bounds.lo || eh >= bounds.hi)
        return false;
    r.at = (const uint8_t *)eh;
    const uint8_t *last_cie = NULL;
    Cie cie;
    while (r.at != r.end && !r.failed) {
        unsigned width;
        Reader fde = record(&r, &width);
        if (fde.failed || fde.end == fde.at)
            break;
        const uint8_t *id_field = fde.at;
        uint64_t id = fixed(&fde, width);
        if (fde.failed || !id || id > (uint64_t)((uintptr_t)id_field - eh))
            continue;
        const uint8_t *cie_start = id_field - id;
        if (cie_start != last_cie) {
            if (!read_cie(cie_start, id_field, bounds, &cie))
                continue;
            last_cie = cie_start;
        }
        uintptr_t start = address(&fde, cie.pointer_encoding, bounds.lo, 0, bounds);
        uintptr_t size = address(&fde, cie.pointer_encoding & 15, 0, 0, bounds);
        if (fde.failed || pc < start || pc - start >= size || size > UINT64_MAX - start)
            continue;
        uintptr_t lsda = 0;
        if (cie.augmentation_length) {
            uint64_t length = leb(&fde, false);
            Reader aug = subrange(&fde, length);
            if (cie.lsda_encoding != OMIT)
                lsda = address(&aug, cie.lsda_encoding, bounds.lo, start, bounds);
            if (aug.failed)
                return false;
        }
        *frame = (Frame){fde, cie, start, start + size, (const uint8_t *)lsda, bounds};
        return !fde.failed;
    }
    return false;
}

static Rule expression_rule(Reader *r, unsigned kind)
{
    uint64_t length = leb(r, false);
    Reader expression = subrange(r, length);
    return (Rule){expression.at, (int64_t)length, kind};
}

static void set_rule(State *state, uint64_t reg, Rule rule)
{
    if (reg < REG_COUNT)
        state->regs[reg] = rule;
}

static bool restore_rule(State *state, const State *initial, uint64_t reg)
{
    if (!initial)
        return false;
    if (reg < REG_COUNT)
        state->regs[reg] = initial->regs[reg];
    return true;
}

static bool cfi(Reader r, const Frame *frame, const State *initial, State *state, uintptr_t pc)
{
    uintptr_t location = frame->start;
    int64_t factor = frame->cie.data_align;
    State saved[8];
    unsigned depth = 0;
    while (!r.failed && r.at != r.end && location <= pc) {
        unsigned op = (unsigned)fixed(&r, 1);
        uint64_t reg;
        int64_t offset;
        switch (op >> 6) {
        case 1: location += (op & 63) * frame->cie.code_align; continue;
        case 2:
            offset = (int64_t)(leb(&r, false) * (uint64_t)factor);
            set_rule(state, op & 63, (Rule){NULL, offset, OFFSET});
            continue;
        case 3:
            if (!restore_rule(state, initial, op & 63))
                return false;
            continue;
        default: break;
        }
        switch (op) {
        case 0: break;
        case 1:
            location = address(&r, frame->cie.pointer_encoding, frame->bounds.lo, frame->start, frame->bounds);
            break;
        case 2: case 3: case 4:
            location += fixed(&r, 1u << (op - 2)) * frame->cie.code_align;
            break;
        case 5: case 17: case 20: case 21:
            reg = leb(&r, false);
            offset = (int64_t)(leb(&r, op == 17 || op == 21) * (uint64_t)factor);
            set_rule(state, reg, (Rule){NULL, offset, op >= 20 ? VALUE_OFFSET : OFFSET});
            break;
        case 6:
            if (!restore_rule(state, initial, leb(&r, false)))
                return false;
            break;
        case 7: case 8:
            set_rule(state, leb(&r, false), (Rule){.kind = op == 7 ? UNDEFINED : SAME});
            break;
        case 9:
            reg = leb(&r, false);
            set_rule(state, reg, (Rule){NULL, (int64_t)leb(&r, false), REGISTER});
            break;
        case 10:
            if (depth == sizeof saved / sizeof saved[0])
                return false;
            saved[depth++] = *state;
            break;
        case 11:
            if (!depth)
                return false;
            *state = saved[--depth];
            break;
        case 12: case 18:
            state->cfa_register = (unsigned)leb(&r, false);
            state->cfa = (Rule){.value = (int64_t)(leb(&r, op == 18) * (op == 18 ? (uint64_t)factor : 1))};
            break;
        case 13: state->cfa_register = (unsigned)leb(&r, false); break;
        case 14: case 19:
            state->cfa.value = (int64_t)(leb(&r, op == 19) * (op == 19 ? (uint64_t)factor : 1));
            break;
        case 15: state->cfa = expression_rule(&r, VALUE_EXPRESSION); break;
        case 16: case 22:
            reg = leb(&r, false);
            set_rule(state, reg, expression_rule(&r, op == 16 ? EXPRESSION : VALUE_EXPRESSION));
            break;
        case 0x2d: state->signed_pc = !state->signed_pc; break;
        case 0x2e: leb(&r, false); break; /* GNU_args_size */
        default: return false;
        }
    }
    return !r.failed;
}

static uintptr_t load_word(uintptr_t address_value)
{
    uintptr_t value;
    memcpy(&value, (const void *)address_value, sizeof value);
    return value;
}

static bool signal_frame(UnwindContext *ctx)
{
#if defined(__aarch64__)
    /* Linux returns from signal handlers through the vDSO rt_sigreturn stub. */
    uintptr_t vdso = getauxval(33); /* AT_SYSINFO_EHDR */
    uintptr_t pc = ctx->regs[PC];
    if (!vdso || pc < vdso || (pc & 3))
        return false;
    Reader header = {(const uint8_t *)vdso + 32, (const uint8_t *)vdso + 64, false};
    uint64_t phoff = fixed(&header, 8);
    header.at += 14;
    uint64_t stride = fixed(&header, 2), count = fixed(&header, 2);
    if (stride != 56 || phoff > 4096 || count > (4096 - phoff) / stride)
        return false;
    bool executable = false;
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *start = (const uint8_t *)vdso + phoff + i * stride;
        Reader ph = {start, start + stride, false};
        unsigned type = (unsigned)fixed(&ph, 4), flags = (unsigned)fixed(&ph, 4);
        ph.at += 8;
        uint64_t begin = fixed(&ph, 8);
        ph.at += 16;
        uint64_t size = fixed(&ph, 8);
        if (type == 1 && (flags & 1) && size >= 12 && pc - vdso >= begin && pc - vdso - begin <= size - 12)
            executable = true;
    }
    if (!executable)
        return false;
    Reader code = {(const uint8_t *)pc, (const uint8_t *)pc + 12, false};
    uint64_t first = fixed(&code, 4);
    if (first == 0xd503245f) /* bti c */
        first = fixed(&code, 4);
    if (first != 0xd2801168 || fixed(&code, 4) != 0xd4000001) /* mov x8, #139; svc #0 */
        return false;

    /* The signal frame contains siginfo followed by ucontext. The latter's
       machine context starts at byte 176 and begins with the fault address. */
    const uint8_t *machine = (const uint8_t *)ctx->regs[STACK] + 128 + 176;
    uintptr_t restored[33];
    memcpy(restored, machine + 8, sizeof restored);
    if (!restored[PC] || !restored[STACK] || (restored[STACK] & 15))
        return false;
    memcpy(ctx->regs, restored, sizeof restored);
    const uint8_t *extra = machine + 288;
    Reader fpsimd = {extra, extra + 528, false};
    if (fixed(&fpsimd, 4) == 0x46508001 && fixed(&fpsimd, 4) == 528)
        for (unsigned i = 8; i < 16; i++)
            ctx->regs[64 + i] = load_word((uintptr_t)extra + 16 + i * 16);
    ctx->cfa = restored[STACK];
    ctx->signal = true;
    return true;
#else
    (void)ctx;
    return false;
#endif
}

static bool evaluate(Rule rule, const UnwindContext *context, uintptr_t cfa, uintptr_t *value)
{
    Reader r = {rule.expression, rule.expression + rule.value, false};
    uintptr_t stack[16] = {cfa};
    unsigned n = 1;
    while (!r.failed && r.at != r.end) {
        unsigned op = (unsigned)fixed(&r, 1);
        uintptr_t item;
        if (op >= 0x30 && op <= 0x4f) {
            item = op - 0x30; /* DW_OP_litN */
        } else if (op >= 0x70 && op <= 0x8f) {
            unsigned reg = op - 0x70;
            if (reg >= REG_COUNT)
                return false;
            item = context->regs[reg] + leb(&r, true);
        } else {
            switch (op) {
            case 0x06: /* deref */
                if (!n || !stack[n - 1]) return false;
                stack[n - 1] = load_word(stack[n - 1]);
                continue;
            case 0x10: case 0x11: item = leb(&r, op == 0x11); break;
            case 0x1a: case 0x1c: case 0x22: case 0x24: case 0x25: case 0x2a:
                if (n < 2) return false;
                item = stack[--n];
                if (op == 0x1a) stack[n - 1] &= item;
                if (op == 0x1c) stack[n - 1] -= item;
                if (op == 0x22) stack[n - 1] += item;
                if (op == 0x24) stack[n - 1] = item < 64 ? stack[n - 1] << item : 0;
                if (op == 0x25) stack[n - 1] = item < 64 ? stack[n - 1] >> item : 0;
                if (op == 0x2a) stack[n - 1] = (int64_t)stack[n - 1] >= (int64_t)item;
                continue;
            case 0x23:
                if (!n) return false;
                stack[n - 1] += leb(&r, false);
                continue;
            case 0x92: {
                uint64_t reg = leb(&r, false);
                if (reg >= REG_COUNT) return false;
                item = context->regs[reg] + leb(&r, true);
                break;
            }
            case 0x9c: item = cfa; break; /* call_frame_cfa */
            default: return false;
            }
        }
        if (n == sizeof stack / sizeof stack[0])
            return false;
        stack[n++] = item;
    }
    if (r.failed || !n)
        return false;
    *value = stack[n - 1];
    return true;
}

static bool step(UnwindContext *context, const Frame *frame, uintptr_t pc)
{
    State initial = {.cfa_register = STACK};
    if (!cfi(frame->cie.program, frame, NULL, &initial, UINT64_MAX))
        return false;
    State state = initial;
    if (!cfi(frame->program, frame, &initial, &state, pc))
        return false;
    uintptr_t cfa;
    if (state.cfa.kind == VALUE_EXPRESSION) {
        if (!evaluate(state.cfa, context, 0, &cfa))
            return false;
    } else {
        if (state.cfa_register >= REG_COUNT)
            return false;
        cfa = context->regs[state.cfa_register] + (uint64_t)state.cfa.value;
    }
    if (!cfa)
        return false;
    UnwindContext next = *context;
    for (unsigned i = 0; i < REG_COUNT; i++) {
        Rule rule = state.regs[i];
        uintptr_t value = cfa + (uint64_t)rule.value;
        switch (rule.kind) {
        case UNDEFINED: next.regs[i] = 0; break;
        case OFFSET: next.regs[i] = load_word(value); break;
        case VALUE_OFFSET: next.regs[i] = value; break;
        case REGISTER:
            next.regs[i] = (uint64_t)rule.value < REG_COUNT ? context->regs[rule.value] : 0;
            break;
        case VALUE_EXPRESSION: case EXPRESSION:
            if (!evaluate(rule, context, cfa, &value) || (rule.kind == EXPRESSION && !value))
                return false;
            next.regs[i] = rule.kind == EXPRESSION ? load_word(value) : value;
            break;
        default: break;
        }
    }
    uintptr_t ip = next.regs[frame->cie.return_register];
#if defined(__aarch64__)
    if (state.signed_pc)
        ip = mcc_strip_pac(ip);
#endif
    if (!ip || (ip == context->regs[PC] && cfa == context->cfa))
        return false;
    next.regs[PC] = ip;
    next.regs[STACK] = cfa;
    next.cfa = cfa;
    next.signal = frame->cie.signal;
    *context = next;
    return true;
}

static uintptr_t instruction(const UnwindContext *ctx)
{
    return ctx->regs[PC] - (!ctx->signal && ctx->regs[PC] != 0);
}

uintptr_t _Unwind_GetCFA(UnwindContext *ctx) { return ctx->cfa; }
uintptr_t _Unwind_GetIP(UnwindContext *ctx) { return ctx->regs[PC]; }

int _Unwind_Backtrace(Trace trace, void *argument)
{
    UnwindContext ctx = {0};
    mcc_capture_frame(ctx.regs);
    ctx.cfa = ctx.regs[STACK];
    for (;;) {
        if (trace(&ctx, argument) != NO_REASON)
            break;
        uintptr_t pc = instruction(&ctx);
        Frame frame;
        if (!(find_frame(pc, &frame) ? step(&ctx, &frame, pc) : signal_frame(&ctx)))
            break;
    }
    return END_OF_STACK;
}

static int forced_unwind(UnwindException *exception, UnwindContext *ctx, bool resume)
{
    for (;;) {
        uintptr_t pc = instruction(ctx);
        Frame frame;
        bool found = find_frame(pc, &frame);
        UnwindContext next = *ctx;
        bool has_caller = found ? step(&next, &frame, pc) : signal_frame(&next);
        int actions = FORCE_UNWIND | CLEANUP_PHASE;
        if (!resume || !has_caller) {
            bool end = !found && !has_caller;
            int reason = exception->stop(1, actions | (end ? END_ACTION : 0), exception->exception_class,
                                         exception, ctx, exception->argument);
            if (reason != NO_REASON || end)
                return reason != NO_REASON ? FATAL_PHASE2 : END_OF_STACK;
            if (!resume && found && frame.cie.personality) {
                Personality personality = (Personality)frame.cie.personality;
                ctx->start = frame.start;
                ctx->lsda = frame.lsda;
                ctx->map_end = frame.bounds.hi;
                reason = personality(1, actions, exception->exception_class, exception, ctx);
                if (reason == INSTALL_CONTEXT)
                    mcc_install_frame(ctx->regs);
                if (reason != CONTINUE_UNWIND)
                    return FATAL_PHASE2;
            }
            if (!has_caller) {
                exception->stop(1, actions | END_ACTION, exception->exception_class,
                                exception, ctx, exception->argument);
                return END_OF_STACK;
            }
        }
        *ctx = next;
        resume = false;
    }
}

int _Unwind_ForcedUnwind(UnwindException *exception, Stop stop, void *argument)
{
    UnwindContext ctx = {0};
    mcc_capture_frame(ctx.regs);
    ctx.cfa = ctx.regs[STACK];
    exception->stop = stop;
    exception->argument = argument;
    return forced_unwind(exception, &ctx, false);
}

_Noreturn void _Unwind_Resume(UnwindException *exception)
{
    UnwindContext ctx = {0};
    mcc_capture_frame(ctx.regs);
    ctx.cfa = ctx.regs[STACK];
    if (exception->stop)
        forced_unwind(exception, &ctx, true);
    abort();
}

int __gcc_personality_v0(int version, int actions, uint64_t exception_class,
                        UnwindException *exception, UnwindContext *ctx)
{
    (void)exception_class;
    if (version != 1)
        return FATAL_PHASE2;
    if (!(actions & CLEANUP_PHASE) || !ctx->lsda)
        return CONTINUE_UNWIND;
    if ((uintptr_t)ctx->lsda >= ctx->map_end)
        return FATAL_PHASE2;
    Reader r = {ctx->lsda, (const uint8_t *)ctx->map_end, false};
    Bounds bounds = {(uintptr_t)ctx->lsda, ctx->map_end};
    unsigned encoding = (unsigned)fixed(&r, 1);
    uintptr_t landing_base = encoding == OMIT ? ctx->start : address(&r, encoding, 0, ctx->start, bounds);
    if (fixed(&r, 1) != OMIT)
        leb(&r, false); /* type table offset, unused for C cleanups */
    encoding = (unsigned)fixed(&r, 1);
    uint64_t size = leb(&r, false);
    Reader table = subrange(&r, size);
    uintptr_t offset = instruction(ctx) - ctx->start;
    while (!table.failed && table.at != table.end) {
        uintptr_t start = address(&table, encoding, 0, ctx->start, bounds);
        uintptr_t length = address(&table, encoding, 0, ctx->start, bounds);
        uintptr_t landing = address(&table, encoding, 0, ctx->start, bounds);
        leb(&table, false); /* action, unused for C cleanups */
        if (!table.failed && offset >= start && offset - start < length) {
            if (!landing)
                break;
            ctx->regs[0] = (uintptr_t)exception;
            ctx->regs[1] = 0;
            ctx->regs[PC] = landing_base + landing;
            return INSTALL_CONTEXT;
        }
    }
    return table.failed ? FATAL_PHASE2 : CONTINUE_UNWIND;
}
