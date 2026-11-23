#ifndef MCC_INTEGER_OVERFLOW_H
#define MCC_INTEGER_OVERFLOW_H

#include "int128_math.h"

enum { OVERFLOW_SIGNED_A = 1, OVERFLOW_SIGNED_B = 2, OVERFLOW_SIGNED_RESULT = 4 };

static inline uint64_t magnitude_limit(unsigned width, bool signed_result, bool negative)
{
    if (signed_result)
        return (UINT64_C(1) << (width - 1)) - !negative;
    return negative ? 0 : UINT64_MAX >> (64 - width);
}

/* The operands are sign- or zero-extended to 64 bits. Compare their exact
   magnitude against the destination's range before forming a sum or product. */
static inline bool integer_overflow(uint64_t a, uint64_t b, unsigned operation,
                                    unsigned width, unsigned flags)
{
    bool negative_a = (flags & OVERFLOW_SIGNED_A) && (a >> 63);
    bool negative_b = (flags & OVERFLOW_SIGNED_B) && (b >> 63);
    bool signed_result = (flags & OVERFLOW_SIGNED_RESULT) != 0;
    a = negative_a ? 0 - a : a;
    b = negative_b ? 0 - b : b;
    if (operation == '*') {
        uint64_t limit = magnitude_limit(width, signed_result, negative_a != negative_b);
        return a && b > limit / a;
    }
    if (operation == '-')
        negative_b = !negative_b;
    if (negative_a == negative_b) {
        uint64_t limit = magnitude_limit(width, signed_result, negative_a);
        return a > limit || b > limit - a;
    }
    if (a >= b)
        return a - b > magnitude_limit(width, signed_result, negative_a);
    return b - a > magnitude_limit(width, signed_result, negative_b);
}

static inline bool integer128_overflow(Integer128 a, Integer128 b, unsigned operation,
                                       unsigned width, unsigned flags)
{
    bool a_negative = (flags & OVERFLOW_SIGNED_A) && (a.hi >> 63);
    bool b_negative = (flags & OVERFLOW_SIGNED_B) && (b.hi >> 63);
    bool signed_result = (flags & OVERFLOW_SIGNED_RESULT) != 0;
    if (a_negative)
        a = wide_negate(a);
    if (b_negative)
        b = wide_negate(b);
    if (operation == '*') {
        Integer128 limit = wide_limit(width, signed_result, a_negative != b_negative);
        return wide_nonzero(a) && wide_compare(b, wide_divide(limit, a, true, false), true) > 0;
    }
    if (operation == '-')
        b_negative = !b_negative;
    if (a_negative == b_negative) {
        Integer128 limit = wide_limit(width, signed_result, a_negative);
        return wide_compare(a, limit, true) > 0 ||
               wide_compare(b, wide_subtract(limit, a), true) > 0;
    }
    if (wide_compare(a, b, true) >= 0)
        return wide_compare(wide_subtract(a, b), wide_limit(width, signed_result, a_negative), true) > 0;
    return wide_compare(wide_subtract(b, a), wide_limit(width, signed_result, b_negative), true) > 0;
}

#endif
