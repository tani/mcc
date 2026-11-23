#ifndef MCC_INT128_MATH_H
#define MCC_INT128_MATH_H

#include "int128.h"

static inline bool wide_nonzero(Integer128 x)
{
    return (x.lo | x.hi) != 0;
}

static inline int wide_compare(Integer128 a, Integer128 b, bool uns)
{
    uint64_t sign = uns ? 0 : UINT64_C(1) << 63;
    uint64_t ah = a.hi ^ sign, bh = b.hi ^ sign;
    if (ah != bh)
        return ah < bh ? -1 : 1;
    return a.lo < b.lo ? -1 : a.lo != b.lo;
}

static inline Integer128 wide_subtract(Integer128 a, Integer128 b)
{
    return (Integer128){a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo)};
}

static inline Integer128 wide_negate(Integer128 a)
{
    return wide_subtract((Integer128){0, 0}, a);
}

static inline Integer128 wide_shift_left(Integer128 a, unsigned n)
{
    if (!n)
        return a;
    if (n >= 64)
        return (Integer128){0, a.lo << (n - 64)};
    return (Integer128){a.lo << n, (a.hi << n) | (a.lo >> (64 - n))};
}

static inline Integer128 wide_shift_right(Integer128 a, unsigned n, bool uns)
{
    uint64_t fill = !uns && (a.hi >> 63) ? UINT64_MAX : 0;
    if (!n)
        return a;
    if (n == 64)
        return (Integer128){a.hi, fill};
    if (n > 64)
        return (Integer128){(a.hi >> (n - 64)) | (fill << (128 - n)), fill};
    return (Integer128){(a.lo >> n) | (a.hi << (64 - n)),
                        (a.hi >> n) | (fill << (64 - n))};
}

static inline Integer128 wide_mask(unsigned width)
{
    if (!width)
        return (Integer128){0, 0};
    return wide_shift_right((Integer128){UINT64_MAX, UINT64_MAX}, 128 - width, true);
}

static inline Integer128 wide_limit(unsigned width, bool signed_result, bool negative)
{
    if (negative)
        return signed_result ? wide_shift_left((Integer128){1, 0}, width - 1) :
                               (Integer128){0, 0};
    return wide_mask(width - signed_result);
}

static inline uint64_t wide_product_high(uint64_t a, uint64_t b)
{
    uint64_t al = a & UINT32_MAX, ah = a >> 32;
    uint64_t bl = b & UINT32_MAX, bh = b >> 32;
    uint64_t cross = (al * bl >> 32) + ah * bl;
    uint64_t upper = cross >> 32;
    cross = (cross & UINT32_MAX) + al * bh;
    return ah * bh + upper + (cross >> 32);
}

static inline Integer128 wide_divide(Integer128 a, Integer128 b, bool uns, bool remainder)
{
    bool negate_a = !uns && (a.hi >> 63);
    bool negate_b = !uns && (b.hi >> 63);
    if (negate_a)
        a = wide_negate(a);
    if (negate_b)
        b = wide_negate(b);
    Integer128 q = {0, 0}, r = {0, 0};
    if (!a.hi && !b.hi) {
        q.lo = a.lo / b.lo;
        r.lo = a.lo % b.lo;
    } else {
        for (unsigned i = 0; i < 128; i++) {
            r = wide_shift_left(r, 1);
            r.lo |= a.hi >> 63;
            a = wide_shift_left(a, 1);
            q = wide_shift_left(q, 1);
            if (wide_compare(r, b, true) >= 0) {
                r = wide_subtract(r, b);
                q.lo |= 1;
            }
        }
    }
    Integer128 result = remainder ? r : q;
    bool negative = remainder ? negate_a : negate_a != negate_b;
    return negative ? wide_negate(result) : result;
}

static inline Integer128 wide_binary(Integer128 a, Integer128 b, WideOperation op, bool uns)
{
    switch (op) {
    case WIDE_ADD: {
        uint64_t sum = a.lo + b.lo;
        return (Integer128){sum, a.hi + b.hi + (sum < a.lo)};
    }
    case WIDE_SUB: return wide_subtract(a, b);
    case WIDE_MUL:
        return (Integer128){a.lo * b.lo, wide_product_high(a.lo, b.lo) + a.lo * b.hi + a.hi * b.lo};
    case WIDE_DIV: case WIDE_REM: return wide_divide(a, b, uns, op == WIDE_REM);
    case WIDE_AND: return (Integer128){a.lo & b.lo, a.hi & b.hi};
    case WIDE_OR: return (Integer128){a.lo | b.lo, a.hi | b.hi};
    case WIDE_XOR: return (Integer128){a.lo ^ b.lo, a.hi ^ b.hi};
    case WIDE_SHL: return wide_shift_left(a, (unsigned)(b.lo & 127));
    case WIDE_SHR: return wide_shift_right(a, (unsigned)(b.lo & 127), uns);
    case WIDE_LT: return (Integer128){wide_compare(a, b, uns) < 0, 0};
    case WIDE_LE: return (Integer128){wide_compare(a, b, uns) <= 0, 0};
    case WIDE_GT: return (Integer128){wide_compare(a, b, uns) > 0, 0};
    case WIDE_GE: return (Integer128){wide_compare(a, b, uns) >= 0, 0};
    case WIDE_EQ: return (Integer128){a.lo == b.lo && a.hi == b.hi, 0};
    case WIDE_NE: return (Integer128){a.lo != b.lo || a.hi != b.hi, 0};
    }
    return (Integer128){0, 0};
}

#endif
