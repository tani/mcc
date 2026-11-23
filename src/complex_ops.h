#ifndef MCC_COMPLEX_OPS_H
#define MCC_COMPLEX_OPS_H

#include <float.h>
#include <stdbool.h>

#ifdef MCC_COMPLEX_QUAD
#include "quad.h"
typedef Float128 ComplexReal;
enum { COMPLEX_MANTISSA = 113 };
#else
typedef long double ComplexReal;
enum { COMPLEX_MANTISSA = LDBL_MANT_DIG };
#endif

typedef struct { ComplexReal real, imaginary; } ComplexNumber;
typedef struct { ComplexReal significand; int exponent; } ScaledReal;

typedef union {
    ComplexReal value;
    unsigned char bytes[sizeof(ComplexReal)];
} ComplexRealBits;

/* The supported Linux targets use little-endian binary128 or 80-bit extended
   precision, with the latter stored in 16 bytes. */
enum {
    COMPLEX_EXP_BYTE = COMPLEX_MANTISSA == 64 ? 8 : 14,
    COMPLEX_EXP_BIAS = 16383,
    COMPLEX_EXP_MAX = 32767,
    COMPLEX_SCALE_STEP = 8192
};

static inline unsigned complex_exponent(ComplexRealBits bits)
{
    return ((unsigned)(bits.bytes[COMPLEX_EXP_BYTE + 1] & 127) << 8) |
           bits.bytes[COMPLEX_EXP_BYTE];
}

static inline ComplexRealBits complex_set_exponent(ComplexRealBits bits, unsigned exponent)
{
    bits.bytes[COMPLEX_EXP_BYTE] = (unsigned char)exponent;
    bits.bytes[COMPLEX_EXP_BYTE + 1] =
        (unsigned char)((bits.bytes[COMPLEX_EXP_BYTE + 1] & 128) | (exponent >> 8));
    return bits;
}

static inline ComplexReal complex_with_sign(ComplexReal magnitude, ComplexReal sign)
{
    ComplexRealBits output = {.value = magnitude}, source = {.value = sign};
    unsigned index = COMPLEX_EXP_BYTE + 1;
    output.bytes[index] = (unsigned char)((output.bytes[index] & 127) | (source.bytes[index] & 128));
    return output.value;
}

static inline ComplexReal complex_binary_power(unsigned biased_exponent)
{
    ComplexRealBits bits = {.bytes = {0}};
    if (COMPLEX_MANTISSA == 64)
        bits.bytes[7] = 128;
    return complex_set_exponent(bits, biased_exponent).value;
}

static inline ComplexReal complex_infinity(void)
{
    return complex_binary_power(COMPLEX_EXP_MAX);
}

static inline bool complex_is_infinite(ComplexReal x)
{
    ComplexReal infinity = complex_infinity();
    return x == infinity || x == -infinity;
}

static inline bool complex_is_finite(ComplexReal x)
{
    return complex_exponent((ComplexRealBits){.value = x}) != COMPLEX_EXP_MAX;
}

static inline ComplexReal complex_scale(ComplexReal significand, int exponent)
{
    if (significand == (ComplexReal)0)
        return significand;
    while (exponent > COMPLEX_SCALE_STEP) {
        significand *= complex_binary_power(COMPLEX_EXP_BIAS + COMPLEX_SCALE_STEP);
        exponent -= COMPLEX_SCALE_STEP;
    }
    while (exponent < -COMPLEX_SCALE_STEP) {
        significand *= complex_binary_power(COMPLEX_EXP_BIAS - COMPLEX_SCALE_STEP);
        exponent += COMPLEX_SCALE_STEP;
    }
    return significand * complex_binary_power((unsigned)(COMPLEX_EXP_BIAS + exponent));
}

static inline ScaledReal complex_split_exponent(ComplexReal number)
{
    if (number == (ComplexReal)0)
        return (ScaledReal){number, 0};
    ComplexRealBits bits = {.value = number};
    int adjustment = 0;
    unsigned exponent = complex_exponent(bits);
    if (!exponent) {
        bits.value *= complex_binary_power(COMPLEX_EXP_BIAS + COMPLEX_MANTISSA);
        exponent = complex_exponent(bits);
        adjustment = -COMPLEX_MANTISSA;
    }
    bits = complex_set_exponent(bits, COMPLEX_EXP_BIAS - 1);
    return (ScaledReal){bits.value, (int)exponent - (COMPLEX_EXP_BIAS - 1) + adjustment};
}

static inline ScaledReal complex_scaled_product(ComplexReal x, ComplexReal y)
{
    ScaledReal a = complex_split_exponent(x), b = complex_split_exponent(y);
    return (ScaledReal){a.significand * b.significand, a.exponent + b.exponent};
}

/* Align the two products individually. Scaling a whole complex operand by
   its larger component would discard smaller components that can contribute
   to the final result. */
static inline ScaledReal complex_sum_products(ComplexReal a, ComplexReal b,
                                              ComplexReal c, ComplexReal d)
{
    ScaledReal first = complex_scaled_product(a, b), second = complex_scaled_product(c, d);
    if (first.significand == (ComplexReal)0)
        return second.significand == (ComplexReal)0 ?
               (ScaledReal){first.significand + second.significand, 0} : second;
    if (second.significand == (ComplexReal)0)
        return first;
    if (second.exponent > first.exponent) {
        ScaledReal swap = first;
        first = second;
        second = swap;
    }
    int gap = second.exponent - first.exponent;
    /* A nonzero contribution this small can affect directed rounding but
       cannot change a nearest-rounded result. Keep it in the normal range. */
    if (gap < -COMPLEX_MANTISSA - 4)
        gap = -COMPLEX_MANTISSA - 4;
    first.significand += complex_scale(second.significand, gap);
    return first;
}

static inline bool complex_has_infinity(ComplexNumber z)
{
    return complex_is_infinite(z.real) || complex_is_infinite(z.imaginary);
}

static inline bool complex_is_finite_pair(ComplexNumber z)
{
    return complex_is_finite(z.real) && complex_is_finite(z.imaginary);
}

static inline ComplexNumber complex_infinite_direction(ComplexNumber z)
{
    return (ComplexNumber){
        complex_with_sign(complex_is_infinite(z.real) ? (ComplexReal)1 : (ComplexReal)0, z.real),
        complex_with_sign(complex_is_infinite(z.imaginary) ? (ComplexReal)1 : (ComplexReal)0, z.imaginary)
    };
}

static inline ComplexNumber complex_zero_nans(ComplexNumber z)
{
    if (z.real != z.real)
        z.real = complex_with_sign((ComplexReal)0, z.real);
    if (z.imaginary != z.imaginary)
        z.imaginary = complex_with_sign((ComplexReal)0, z.imaginary);
    return z;
}

static inline ComplexNumber complex_product(ComplexNumber a, ComplexNumber b)
{
    if (complex_is_finite_pair(a) && complex_is_finite_pair(b)) {
        ScaledReal real = complex_sum_products(a.real, b.real, -a.imaginary, b.imaginary);
        ScaledReal imag = complex_sum_products(a.real, b.imaginary, a.imaginary, b.real);
        return (ComplexNumber){complex_scale(real.significand, real.exponent),
                               complex_scale(imag.significand, imag.exponent)};
    }
    ComplexNumber product = {a.real * b.real - a.imaginary * b.imaginary,
                             a.real * b.imaginary + a.imaginary * b.real};
    if (product.real == product.real || product.imaginary == product.imaginary)
        return product;
    bool inf_a = complex_has_infinity(a), inf_b = complex_has_infinity(b);
    if (inf_a || inf_b) {
        a = inf_a ? complex_infinite_direction(a) : complex_zero_nans(a);
        b = inf_b ? complex_infinite_direction(b) : complex_zero_nans(b);
        ScaledReal real = complex_sum_products(a.real, b.real, -a.imaginary, b.imaginary);
        ScaledReal imag = complex_sum_products(a.real, b.imaginary, a.imaginary, b.real);
        ComplexReal infinity = complex_infinity();
        return (ComplexNumber){infinity * real.significand, infinity * imag.significand};
    }
    return product;
}

static inline ComplexNumber complex_quotient(ComplexNumber a, ComplexNumber b)
{
    bool finite_a = complex_is_finite_pair(a), finite_b = complex_is_finite_pair(b);
    if (b.real == (ComplexReal)0 && b.imaginary == (ComplexReal)0) {
        ComplexReal infinity = complex_with_sign(complex_infinity(), b.real);
        return (ComplexNumber){infinity * a.real, infinity * a.imaginary};
    }
    if (finite_a && finite_b) {
        ScaledReal real = complex_sum_products(a.real, b.real, a.imaginary, b.imaginary);
        ScaledReal imag = complex_sum_products(a.imaginary, b.real, -a.real, b.imaginary);
        ScaledReal norm = complex_sum_products(b.real, b.real, b.imaginary, b.imaginary);
        return (ComplexNumber){
            complex_scale(real.significand / norm.significand, real.exponent - norm.exponent),
            complex_scale(imag.significand / norm.significand, imag.exponent - norm.exponent)
        };
    }
    if (finite_b && complex_has_infinity(a)) {
        a = complex_infinite_direction(a);
        ScaledReal real = complex_sum_products(a.real, b.real, a.imaginary, b.imaginary);
        ScaledReal imag = complex_sum_products(a.imaginary, b.real, -a.real, b.imaginary);
        ComplexReal infinity = complex_infinity();
        return (ComplexNumber){infinity * real.significand, infinity * imag.significand};
    }
    if (finite_a && complex_has_infinity(b)) {
        b = complex_infinite_direction(b);
        ScaledReal real = complex_sum_products(a.real, b.real, a.imaginary, b.imaginary);
        ScaledReal imag = complex_sum_products(a.imaginary, b.real, -a.real, b.imaginary);
        return (ComplexNumber){complex_with_sign((ComplexReal)0, real.significand),
                               complex_with_sign((ComplexReal)0, imag.significand)};
    }
    ComplexReal norm = b.real * b.real + b.imaginary * b.imaginary;
    return (ComplexNumber){(a.real * b.real + a.imaginary * b.imaginary) / norm,
                           (a.imaginary * b.real - a.real * b.imaginary) / norm};
}

#endif
