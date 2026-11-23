#ifndef MCC_FLOAT16_H
#define MCC_FLOAT16_H

#include "int128_float.h"

/* Both supported long double formats have the same exponent and bias. */
static inline uint16_t float16_encode(Integer128 bits, bool extended, unsigned format,
                                      unsigned mode, unsigned *flags)
{
    unsigned sign_exponent = (unsigned)(extended ? bits.hi : bits.hi >> 48) & 0xffff;
    unsigned sign = sign_exponent & 0x8000, exponent = sign_exponent & 0x7fff;
    unsigned fraction = extended ? 63 : 112, result_fraction = wide_fp_precision(format) - 1;
    int bias = (int)wide_fp_bias(format), min = 1 - bias;
    unsigned leading = 1u << result_fraction, infinity = (unsigned)(2 * bias + 1) << result_fraction;
    Integer128 significand = extended ?
        (Integer128){bits.lo & (UINT64_MAX >> 1), 0} :
        (Integer128){bits.lo, bits.hi & UINT64_C(0xffffffffffff)};
    if (exponent == 0x7fff) {
        unsigned payload = (unsigned)wide_shift_right(significand, fraction - result_fraction, true).lo;
        unsigned quiet = leading >> 1;
        if (wide_nonzero(significand)) {
            if (!(payload & quiet))
                *flags |= FP_INVALID;
            payload |= quiet;
        }
        return (uint16_t)(sign | infinity | payload);
    }
    if (!exponent && !wide_nonzero(significand))
        return (uint16_t)sign;
    bool away = mode == (sign ? FP_DOWNWARD : FP_UPWARD);
    int power = (int)exponent - 16383;
    unsigned magnitude = 0;
    bool inexact = true, halfway = false, above = false;
    if (power > bias) {
        magnitude = infinity;
    } else if (power >= min - (int)result_fraction - 1) {
        Integer128 implicit = wide_shift_left((Integer128){1, 0}, fraction);
        significand.lo |= implicit.lo;
        significand.hi |= implicit.hi;
        unsigned discard = fraction - result_fraction + (power < min ? (unsigned)(min - power) : 0);
        magnitude = (unsigned)wide_shift_right(significand, discard, true).lo;
        Integer128 mask = wide_mask(discard);
        Integer128 tail = {significand.lo & mask.lo, significand.hi & mask.hi};
        int relation = wide_compare(tail, wide_shift_left((Integer128){1, 0}, discard - 1), true);
        inexact = wide_nonzero(tail);
        halfway = relation == 0;
        above = relation > 0;
        if (power >= min)
            magnitude += (unsigned)(power - min) << result_fraction;
    }
    if (inexact && magnitude < infinity &&
        (away || (mode == FP_NEAREST && (above || (halfway && (magnitude & 1))))))
        magnitude++;
    if (magnitude == infinity) {
        *flags |= FP_OVERFLOW | FP_INEXACT;
        if (mode != FP_NEAREST && !away)
            magnitude = infinity - 1;
    } else if (inexact) {
        *flags |= FP_INEXACT;
        if (magnitude < leading)
            *flags |= FP_UNDERFLOW;
    }
    return (uint16_t)(sign | magnitude);
}

static inline uint32_t float16_expand(uint16_t half, unsigned format)
{
    if (format == FP_BFLOAT16)
        return (uint32_t)half << 16;
    uint32_t exponent = (half >> 10) & 31, fraction = half & 1023;
    uint32_t sign = (uint32_t)(half & 0x8000) << 16;
    if (exponent == 31)
        return sign | UINT32_C(0x7f800000) | (fraction << 13);
    if (exponent) {
        exponent += 112;
    } else if (fraction) {
        exponent = 113;
        while (!(fraction & 1024)) {
            fraction <<= 1;
            exponent--;
        }
        fraction &= 1023;
    }
    return sign | (exponent << 23) | (fraction << 13);
}

#endif
