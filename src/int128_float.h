#ifndef MCC_INT128_FLOAT_H
#define MCC_INT128_FLOAT_H

#include "int128_math.h"

/* The formats use their payload width in bytes, with a separate ID for bfloat16. */
enum { FP_BINARY16 = 2, FP_BFLOAT16 = 3 };
enum { FP_NEAREST, FP_UPWARD, FP_DOWNWARD, FP_TOWARD_ZERO };
enum { FP_INVALID = 1, FP_OVERFLOW = 4, FP_UNDERFLOW = 8, FP_INEXACT = 16 };

static inline unsigned wide_fp_bytes(unsigned format)
{
    return format == FP_BFLOAT16 ? 2 : format;
}

static inline unsigned wide_fp_precision(unsigned format)
{
    return format == FP_BINARY16 ? 11 : format == FP_BFLOAT16 ? 8 :
           format == 4 ? 24 : format == 8 ? 53 : format == 10 ? 64 : 113;
}

static inline unsigned wide_fp_bias(unsigned format)
{
    return format == FP_BINARY16 ? 15 : format == FP_BFLOAT16 || format == 4 ? 127 :
           format == 8 ? 1023 : 16383;
}

static inline Integer128 round_wide_float_bits(Integer128 integer, unsigned format, bool uns,
                                               unsigned mode, unsigned *flags)
{
    bool negative = !uns && (integer.hi >> 63);
    Integer128 magnitude = negative ? wide_negate(integer) : integer;
    if (!wide_nonzero(magnitude))
        return magnitude;
    unsigned exponent = magnitude.hi ? 64 : 0;
    for (uint64_t i = magnitude.hi ? magnitude.hi : magnitude.lo; i > 1; i >>= 1)
        exponent++;
    unsigned fraction = wide_fp_precision(format) - 1, bias = wide_fp_bias(format);
    bool away = mode == (negative ? FP_DOWNWARD : FP_UPWARD);
    Integer128 significand;
    if (exponent > fraction) {
        unsigned discard = exponent - fraction;
        significand = wide_shift_right(magnitude, discard, true);
        Integer128 cutoff = wide_shift_left((Integer128){1, 0}, discard - 1);
        Integer128 low = wide_binary(magnitude, wide_mask(discard), WIDE_AND, true);
        int round = wide_compare(low, cutoff, true);
        if (wide_nonzero(low))
            *flags |= FP_INEXACT;
        if ((away && wide_nonzero(low)) ||
            (mode == FP_NEAREST && (round > 0 || (round == 0 && (significand.lo & 1))))) {
            significand = wide_binary(significand, (Integer128){1, 0}, WIDE_ADD, true);
            if (wide_nonzero(wide_shift_right(significand, fraction + 1, true))) {
                significand = wide_shift_right(significand, 1, true);
                exponent++;
            }
        }
    } else {
        significand = wide_shift_left(magnitude, fraction - exponent);
    }
    uint64_t biased = exponent + bias;
    if (format == 10)
        return (Integer128){significand.lo, biased | ((uint64_t)negative << 15)};
    significand = wide_binary(significand, wide_mask(fraction), WIDE_AND, true);
    if (format == 16)
        return (Integer128){significand.lo, significand.hi | (biased << 48) |
                                          ((uint64_t)negative << 63)};
    unsigned sign_bit = wide_fp_bytes(format) * 8 - 1;
    if (exponent > bias) {
        *flags |= FP_OVERFLOW | FP_INEXACT;
        uint64_t infinity = (uint64_t)(2 * bias + 1) << fraction;
        return (Integer128){((uint64_t)negative << sign_bit) |
                            (mode == FP_NEAREST || away ? infinity : infinity - 1), 0};
    }
    return (Integer128){significand.lo | (biased << fraction) | ((uint64_t)negative << sign_bit), 0};
}

static inline Integer128 wide_float_bits(Integer128 integer, unsigned format, bool uns)
{
    unsigned flags = 0;
    return round_wide_float_bits(integer, format, uns, FP_NEAREST, &flags);
}

static inline bool wide_integer_bits(Integer128 bits, unsigned format, bool uns, Integer128 *out)
{
    unsigned fraction = wide_fp_precision(format) - 1;
    unsigned bias = wide_fp_bias(format);
    uint64_t sign_exponent = format == 16 ? bits.hi >> 48 :
                             format == 10 ? bits.hi : bits.lo >> fraction;
    bool negative = (sign_exponent & (2u * (bias + 1u))) != 0;
    unsigned exponent = (unsigned)sign_exponent & (2u * bias + 1u);
    *out = (Integer128){0, 0};
    if (exponent < bias)
        return true;
    if (exponent - bias > 127)
        return false;
    Integer128 significand = format == 10 ? (Integer128){bits.lo, 0} :
        wide_binary(wide_binary(bits, wide_mask(fraction), WIDE_AND, true),
                    wide_shift_left((Integer128){1, 0}, fraction), WIDE_OR, true);
    unsigned integral = exponent - bias;
    Integer128 magnitude = integral >= fraction ? wide_shift_left(significand, integral - fraction) :
                                                   wide_shift_right(significand, fraction - integral, true);
    if (uns ? negative : (magnitude.hi >> 63) && (!negative || magnitude.lo ||
                                                  magnitude.hi != (UINT64_C(1) << 63)))
        return false;
    *out = negative ? wide_negate(magnitude) : magnitude;
    return true;
}

#endif
