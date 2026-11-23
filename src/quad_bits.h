#ifndef MCC_QUAD_BITS_H
#define MCC_QUAD_BITS_H

#include "int128_float.h"

static inline Integer128 quad_from_x87(Integer128 bits, unsigned *flags)
{
    unsigned sign_exponent = (unsigned)bits.hi & 0xffff;
    unsigned exponent = sign_exponent & 0x7fff;
    uint64_t fraction = bits.lo & (UINT64_MAX >> 1);
    if (exponent == 0x7fff && fraction && !(fraction & (UINT64_C(1) << 62))) {
        *flags |= FP_INVALID;
        fraction |= UINT64_C(1) << 62;
    }
    if (!exponent && (bits.lo >> 63))
        sign_exponent |= 1;
    return (Integer128){fraction << 49, ((uint64_t)sign_exponent << 48) | (fraction >> 15)};
}

static inline Integer128 quad_to_x87(Integer128 bits, unsigned mode, unsigned *flags)
{
    uint64_t leading = UINT64_C(1) << 63;
    unsigned sign = (unsigned)(bits.hi >> 63) << 15;
    unsigned exponent = (unsigned)(bits.hi >> 48) & 0x7fff;
    uint64_t fraction = ((bits.hi & UINT64_C(0xffffffffffff)) << 15) | (bits.lo >> 49);
    uint64_t tail = bits.lo & ((UINT64_C(1) << 49) - 1);
    uint64_t significand = fraction | (exponent ? leading : 0);
    if (exponent == 0x7fff) {
        uint64_t quiet = leading >> 1;
        if ((fraction || tail) && !(fraction & quiet)) {
            *flags |= FP_INVALID;
            significand |= quiet;
        }
        return (Integer128){significand, sign | exponent};
    }
    bool away = mode == (sign ? FP_DOWNWARD : FP_UPWARD);
    uint64_t midpoint = UINT64_C(1) << 48;
    if ((away && tail) || (mode == FP_NEAREST && (tail > midpoint || (tail == midpoint && (significand & 1))))) {
        if (++significand == 0) {
            significand = leading;
            exponent++;
        } else if (!exponent && significand == leading) {
            exponent = 1;
        }
    }
    if (tail)
        *flags |= FP_INEXACT | (!exponent ? FP_UNDERFLOW : 0u) | (exponent == 0x7fff ? FP_OVERFLOW : 0u);
    return (Integer128){significand, sign | exponent};
}

#endif
