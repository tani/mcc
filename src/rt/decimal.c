#include "types.h"
#include "decimal-internal.h"
#include "../float_builtins.h"
#include "../int128_float.h"

typedef struct {
    int digits, bias, max_exp;
    unsigned coefficient_bits, exponent_bits, sign_bit;
    bool wide;
} DecimalFormat;

typedef enum { FINITE, INFINITY_VALUE, NAN_VALUE } DecimalClass;
typedef struct {
    Integer128 coefficient;
    int exponent;
    bool negative;
    DecimalClass kind;
} Decimal;

enum { DECIMAL_DIGITS = 34, INTEGER_DIGITS = 39, DECIMAL_EXP_SPAN = 6176 + 6111 };

static bool round_away(int mode, bool negative)
{
    return mode == (negative ? DEC_DOWNWARD : DEC_UPWARD);
}

static bool increment_rounding(int mode, bool negative, int half, bool odd)
{
    if (mode == DEC_DOWNWARD || mode == DEC_UPWARD || mode == DEC_TOWARD_ZERO)
        return round_away(mode, negative);
    return half > 0 || (half == 0 && (odd || mode == DEC_NEAREST_AWAY));
}

static bool overflow_to_infinity(int mode, bool negative)
{
    return (mode != DEC_DOWNWARD && mode != DEC_UPWARD && mode != DEC_TOWARD_ZERO) ||
           round_away(mode, negative);
}

static DecimalFormat decimal_format(unsigned width)
{
    if (width == 4)
        return (DecimalFormat){7, 101, 90, 23, 8, 31, false};
    if (width == 8)
        return (DecimalFormat){16, 398, 369, 53, 10, 63, false};
    return (DecimalFormat){34, 6176, 6111, 49, 14, 63, true};
}

static Integer128 times_ten(Integer128 a, unsigned digit)
{
    Integer128 shifted = wide_shift_left(a, 3);
    Integer128 twice = wide_shift_left(a, 1);
    uint64_t sum = shifted.lo + twice.lo;
    return (Integer128){sum + digit, shifted.hi + twice.hi + (sum < shifted.lo) +
                                   (sum + digit < sum)};
}

static unsigned divide_ten(Integer128 *a)
{
    uint64_t remainder = a->hi % 10;
    a->hi /= 10;
    uint64_t upper = (remainder << 32) | (a->lo >> 32);
    uint64_t lower = ((upper % 10) << 32) | (a->lo & UINT32_MAX);
    a->lo = ((upper / 10) << 32) | (lower / 10);
    return (unsigned)(lower % 10);
}

static Integer128 ten_power(int exponent)
{
    Integer128 value = {1, 0};
    for (int i = 0; i < exponent; i++)
        value = times_ten(value, 0);
    return value;
}

static int coefficient_digits(Integer128 value, unsigned char *digits)
{
    int length = 0;
    while (wide_nonzero(value))
        digits[length++] = (unsigned char)divide_ten(&value);
    return length;
}

static uint64_t decimal_header(DecimalFormat fmt, Integer128 bits)
{
    return fmt.wide ? bits.hi : bits.lo;
}

static Integer128 decimal_bits(DecimalFormat fmt, uint64_t header, uint64_t lower)
{
    return fmt.wide ? (Integer128){lower, header} : (Integer128){header, 0};
}

static Integer128 pack_decimal(DecimalFormat fmt, Decimal n)
{
    uint64_t header = (uint64_t)n.negative << fmt.sign_bit;
    if (n.kind != FINITE) {
        uint64_t special = n.kind == INFINITY_VALUE ? 30 : 31;
        return decimal_bits(fmt, header | (special << (fmt.sign_bit - 5)), 0);
    }
    uint64_t coefficient = decimal_header(fmt, n.coefficient);
    unsigned shift = fmt.coefficient_bits;
    if (coefficient >> shift) {
        /* The steering bits supply the leading coefficient bit in BID. */
        header |= UINT64_C(3) << (fmt.sign_bit - 2);
        shift -= 2;
        coefficient &= (UINT64_C(1) << shift) - 1;
    }
    header |= ((uint64_t)(n.exponent + fmt.bias) << shift) | coefficient;
    return decimal_bits(fmt, header, n.coefficient.lo);
}

static Decimal unpack_decimal(DecimalFormat fmt, Integer128 bits)
{
    uint64_t header = decimal_header(fmt, bits);
    Decimal n = {.negative = (header >> fmt.sign_bit) != 0};
    unsigned combination = (unsigned)((header >> (fmt.sign_bit - 5)) & 31);
    if (combination >= 30) {
        n.kind = combination == 30 ? INFINITY_VALUE : NAN_VALUE;
        return n;
    }
    unsigned shift = fmt.coefficient_bits;
    uint64_t lead = 0;
    if (combination >= 24) {
        lead = UINT64_C(1) << shift;
        shift -= 2;
    }
    uint64_t mask = (UINT64_C(1) << shift) - 1;
    n.coefficient = decimal_bits(fmt, lead | (header & mask), bits.lo);
    if ((lead || fmt.wide) && wide_compare(n.coefficient, ten_power(fmt.digits), true) >= 0)
        n.coefficient = (Integer128){0, 0};
    n.exponent = (int)((header >> shift) & ((1u << fmt.exponent_bits) - 1)) - fmt.bias;
    return n;
}

static bool decimal_signaling(DecimalFormat fmt, Integer128 bits)
{
    return (decimal_header(fmt, bits) >> (fmt.sign_bit - 6)) & 1;
}

static unsigned decimal_payload_width(DecimalFormat fmt)
{
    return fmt.coefficient_bits + (fmt.wide ? 64u : 0u) - 3;
}

static Integer128 canonical_payload(DecimalFormat fmt, Integer128 payload)
{
    return wide_compare(payload, ten_power(fmt.digits - 1), true) < 0 ?
           payload : (Integer128){0, 0};
}

static Integer128 decimal_payload(DecimalFormat fmt, Integer128 bits)
{
    return canonical_payload(fmt, wide_binary(bits, wide_mask(decimal_payload_width(fmt)), WIDE_AND, true));
}

static Integer128 quiet_nan(DecimalFormat fmt, bool negative, Integer128 payload)
{
    Integer128 bits = pack_decimal(fmt, (Decimal){.negative = negative, .kind = NAN_VALUE});
    return wide_binary(bits, payload, WIDE_OR, true);
}

static Integer128 rounded_decimal(DecimalStatus *status, DecimalFormat fmt, bool negative, int64_t exponent,
                                 const unsigned char *digits, int length, bool sticky)
{
    bool tiny = length && exponent + length < fmt.digits - fmt.bias;
    int64_t discard = length > fmt.digits ? length - fmt.digits : 0;
    if (exponent + discard < -fmt.bias)
        discard = -fmt.bias - exponent;
    Integer128 coefficient = {0, 0};
    for (int i = length - 1; i >= discard; i--)
        coefficient = times_ten(coefficient, digits[i]);
    int kept = discard < length ? length - (int)discard : 0;
    exponent += discard;
    if (discard) {
        unsigned guard = discard <= length ? digits[discard - 1] : 0;
        for (int i = 0; i < length && i < discard - 1 && !sticky; i++)
            sticky = digits[i] != 0;
        if (guard || sticky) {
            status->flags |= DEC_INEXACT;
            if (tiny)
                status->flags |= DEC_UNDERFLOW;
        }
        int half = guard < 5 ? -1 : guard > 5 || sticky ? 1 : 0;
        if ((guard || sticky) && increment_rounding(status->rounding, negative, half, coefficient.lo & 1)) {
            coefficient = wide_binary(coefficient, (Integer128){1, 0}, WIDE_ADD, true);
            if (!kept)
                kept = 1;
            if (wide_compare(coefficient, ten_power(kept), true) == 0) {
                if (kept == fmt.digits) {
                    divide_ten(&coefficient);
                    exponent++;
                } else {
                    kept++;
                }
            }
        }
    }
    if (!kept && exponent > fmt.max_exp)
        exponent = fmt.max_exp;
    while (exponent > fmt.max_exp && kept < fmt.digits) {
        coefficient = times_ten(coefficient, 0);
        exponent--;
        kept++;
    }
    if (exponent > fmt.max_exp) {
        status->flags |= DEC_OVERFLOW | DEC_INEXACT;
        if (overflow_to_infinity(status->rounding, negative))
            return pack_decimal(fmt, (Decimal){.negative = negative, .kind = INFINITY_VALUE});
        coefficient = wide_subtract(ten_power(fmt.digits), (Integer128){1, 0});
        exponent = fmt.max_exp;
    }
    return pack_decimal(fmt, (Decimal){coefficient, (int)exponent, negative, FINITE});
}

Integer128 decimal_literal(unsigned width, const char *begin, const char *end)
{
    DecimalFormat fmt = decimal_format(width);
    unsigned char digits[DECIMAL_DIGITS + 1];
    int length = 0;
    int64_t fraction = 0, omitted = 0, exponent = 0;
    bool point = false, sticky = false;
    const char *p = begin;
    for (; p != end && *p != 'e' && *p != 'E'; p++) {
        if (*p == '.') {
            point = true;
            continue;
        }
        unsigned digit = (unsigned)(*p - '0');
        fraction += point;
        if (!length && !digit)
            continue;
        if (length <= fmt.digits)
            digits[length++] = (unsigned char)digit;
        else {
            omitted++;
            sticky |= digit != 0;
        }
    }
    if (p != end) {
        p++;
        bool negative = *p == '-';
        if (*p == '+' || negative)
            p++;
        for (; p != end && exponent < INT64_C(1000000000000); p++)
            exponent = exponent * 10 + (*p - '0');
        if (negative)
            exponent = -exponent;
    }
    for (int i = 0; i < length / 2; i++) {
        unsigned char digit = digits[i];
        digits[i] = digits[length - 1 - i];
        digits[length - 1 - i] = digit;
    }
    DecimalStatus status = {0};
    return rounded_decimal(&status, fmt, false, exponent - fraction + omitted, digits, length, sticky);
}

Integer128 decimal_negate(unsigned width, Integer128 value)
{
    DecimalFormat fmt = decimal_format(width);
    return decimal_bits(fmt, decimal_header(fmt, value) ^ (UINT64_C(1) << fmt.sign_bit), value.lo);
}

Integer128 decimal_convert_status(DecimalStatus *status, unsigned width, unsigned source_width, Integer128 value)
{
    if (width == source_width)
        return value;
    DecimalFormat fmt = decimal_format(width);
    DecimalFormat source = decimal_format(source_width);
    Decimal n = unpack_decimal(source, value);
    if (n.kind == NAN_VALUE) {
        if (decimal_signaling(source, value))
            status->flags |= DEC_INVALID;
        Integer128 payload = decimal_payload(source, value);
        for (int digits = source.digits; digits < fmt.digits; digits++)
            payload = times_ten(payload, 0);
        for (int digits = source.digits; digits > fmt.digits; digits--)
            divide_ten(&payload);
        return quiet_nan(fmt, n.negative, payload);
    }
    if (n.kind == INFINITY_VALUE)
        return pack_decimal(fmt, n);
    unsigned char digits[DECIMAL_DIGITS];
    int length = coefficient_digits(n.coefficient, digits);
    return rounded_decimal(status, fmt, n.negative, n.exponent, digits, length, false);
}

Integer128 decimal_convert(unsigned width, unsigned source_width, Integer128 value)
{
    DecimalStatus status = {0};
    return decimal_convert_status(&status, width, source_width, value);
}

Integer128 decimal_from_integer_status(DecimalStatus *status, unsigned width, Integer128 value, bool uns)
{
    bool negative = !uns && (value.hi >> 63);
    unsigned char digits[INTEGER_DIGITS];
    int length = coefficient_digits(negative ? wide_negate(value) : value, digits);
    return rounded_decimal(status, decimal_format(width), negative, 0, digits, length, false);
}

Integer128 decimal_from_integer(unsigned width, Integer128 value, bool uns)
{
    DecimalStatus status = {0};
    return decimal_from_integer_status(&status, width, value, uns);
}

bool decimal_to_integer_status(DecimalStatus *status, unsigned width, Integer128 value, unsigned bits, bool uns,
                       Integer128 *result)
{
    Decimal n = unpack_decimal(decimal_format(width), value);
    *result = (Integer128){0, 0};
    if (n.kind != FINITE)
        return false;
    if (!wide_nonzero(n.coefficient))
        return true;
    if (n.exponent >= INTEGER_DIGITS)
        return false;
    bool inexact = false;
    for (int i = n.exponent; i < 0 && wide_nonzero(n.coefficient); i++)
        inexact |= divide_ten(&n.coefficient) != 0;
    Integer128 limit = wide_limit(bits, !uns, n.negative);
    Integer128 decade = limit;
    divide_ten(&decade);
    for (int i = 0; i < n.exponent; i++) {
        if (wide_compare(n.coefficient, decade, true) > 0)
            return false;
        n.coefficient = times_ten(n.coefficient, 0);
    }
    if (wide_compare(n.coefficient, limit, true) > 0)
        return false;
    if (inexact)
        status->flags |= DEC_INEXACT;
    *result = n.negative ? wide_negate(n.coefficient) : n.coefficient;
    return true;
}

bool decimal_to_integer(unsigned width, Integer128 value, unsigned bits, bool uns, Integer128 *result)
{
    DecimalStatus status = {0};
    return decimal_to_integer_status(&status, width, value, bits, uns, result);
}

/* A binary128 subnormal can require multiplying its significand by 5^16494.
   The limb bound uses log2(5) < 7/3 and leaves room for a final shift. */
enum {
    EXACT_LIMBS = (113 + 16494 * 7 / 3 + 31) / 32 + 2,
    EXACT_DECIMAL_DIGITS = 11600
};

typedef struct {
    uint32_t limb[EXACT_LIMBS];
    unsigned used;
} ExactInteger;

static void exact_trim(ExactInteger *a)
{
    while (a->used && !a->limb[a->used - 1])
        a->used--;
}

static void exact_assign(ExactInteger *a, Integer128 value)
{
    a->limb[0] = (uint32_t)value.lo;
    a->limb[1] = (uint32_t)(value.lo >> 32);
    a->limb[2] = (uint32_t)value.hi;
    a->limb[3] = (uint32_t)(value.hi >> 32);
    a->used = 4;
    exact_trim(a);
}

static unsigned exact_bits(const ExactInteger *a)
{
    if (!a->used)
        return 0;
    return a->used * 32 - (unsigned)__builtin_clz(a->limb[a->used - 1]);
}

static void exact_multiply(ExactInteger *a, uint32_t multiplier)
{
    uint64_t carry = 0;
    for (unsigned i = 0; i < a->used; i++) {
        uint64_t product = (uint64_t)a->limb[i] * multiplier + carry;
        a->limb[i] = (uint32_t)product;
        carry = product >> 32;
    }
    if (carry)
        a->limb[a->used++] = (uint32_t)carry;
}

static void exact_power_five(ExactInteger *a, unsigned exponent)
{
    while (exponent >= 13) {
        exact_multiply(a, 1220703125U);
        exponent -= 13;
    }
    uint32_t rest = 1;
    for (unsigned i = 0; i < exponent; i++)
        rest *= 5;
    if (rest != 1)
        exact_multiply(a, rest);
}

static uint32_t exact_shifted_limb(const ExactInteger *a, unsigned i, unsigned shift)
{
    unsigned whole = shift / 32, part = shift % 32;
    if (i < whole)
        return 0;
    i -= whole;
    uint32_t limb = i < a->used ? a->limb[i] << part : 0;
    if (part && i && i - 1 < a->used)
        limb |= a->limb[i - 1] >> (32 - part);
    return limb;
}

static void exact_shift(ExactInteger *a, unsigned shift)
{
    if (!shift || !a->used)
        return;
    unsigned n = a->used + shift / 32 + (shift % 32 != 0);
    for (unsigned i = n; i > 0; i--)
        a->limb[i - 1] = exact_shifted_limb(a, i - 1, shift);
    a->used = n;
    exact_trim(a);
}

static int exact_compare_shifted(const ExactInteger *a, const ExactInteger *b, unsigned shift)
{
    unsigned a_bits = exact_bits(a), b_bits = exact_bits(b);
    if (b_bits)
        b_bits += shift;
    if (a_bits != b_bits)
        return a_bits < b_bits ? -1 : 1;
    for (unsigned i = a->used; i > 0; i--) {
        uint32_t left = a->limb[i - 1], right = exact_shifted_limb(b, i - 1, shift);
        if (left != right)
            return left < right ? -1 : 1;
    }
    return 0;
}

static void exact_subtract_shifted(ExactInteger *a, const ExactInteger *b, unsigned shift)
{
    uint64_t borrow = 0;
    for (unsigned i = shift / 32; i < a->used; i++) {
        uint64_t part = (uint64_t)exact_shifted_limb(b, i, shift) + borrow;
        borrow = a->limb[i] < part;
        a->limb[i] = (uint32_t)(a->limb[i] - part);
    }
    exact_trim(a);
}

static uint32_t exact_divide_billion(ExactInteger *a)
{
    uint64_t carry = 0;
    for (unsigned i = a->used; i > 0; i--) {
        uint64_t limb = (carry << 32) | a->limb[i - 1];
        a->limb[i - 1] = (uint32_t)(limb / 1000000000);
        carry = limb % 1000000000;
    }
    exact_trim(a);
    return (uint32_t)carry;
}

static int exact_decimal_digits(ExactInteger *a, unsigned char *digits)
{
    int length = 0;
    while (a->used) {
        uint32_t group = exact_divide_billion(a);
        for (unsigned i = 0; i < 9 && (group || a->used); i++) {
            digits[length++] = (unsigned char)(group % 10);
            group /= 10;
        }
    }
    return length;
}

static Integer128 shift_payload(Integer128 value, unsigned from, unsigned to)
{
    return to >= from ? wide_shift_left(value, to - from) :
                        wide_shift_right(value, from - to, true);
}

static Integer128 binary_encoding(unsigned format, bool negative, unsigned biased, Integer128 significand)
{
    if (format == 10)
        return (Integer128){significand.lo, biased | ((uint64_t)negative << 15)};
    unsigned fraction = wide_fp_precision(format) - 1;
    Integer128 bits = wide_binary(significand, wide_mask(fraction), WIDE_AND, true);
    bits = wide_binary(bits, wide_shift_left((Integer128){biased, 0}, fraction), WIDE_OR, true);
    return wide_binary(bits, wide_shift_left((Integer128){negative, 0},
                                             wide_fp_bytes(format) * 8 - 1), WIDE_OR, true);
}

Integer128 decimal_from_binary_status(DecimalStatus *status, unsigned width, unsigned format, Integer128 bits)
{
    DecimalFormat fmt = decimal_format(width);
    unsigned fraction = wide_fp_precision(format) - 1, bias = wide_fp_bias(format);
    uint64_t sign_exponent = format == 10 ? bits.hi & 0xffff :
                             wide_shift_right(bits, fraction, true).lo;
    unsigned maximum = 2 * bias + 1;
    unsigned biased = (unsigned)sign_exponent & maximum;
    bool negative = (sign_exponent & (maximum + 1)) != 0;
    Integer128 mantissa = wide_binary(bits, wide_mask(fraction), WIDE_AND, true);
    if (biased == maximum) {
        if (!wide_nonzero(mantissa))
            return pack_decimal(fmt, (Decimal){.negative = negative, .kind = INFINITY_VALUE});
        if (!wide_nonzero(wide_shift_right(mantissa, fraction - 1, true)))
            status->flags |= DEC_INVALID;
        Integer128 payload = wide_binary(mantissa, wide_mask(fraction - 1), WIDE_AND, true);
        payload = shift_payload(payload, fraction - 1, decimal_payload_width(fmt));
        return quiet_nan(fmt, negative, canonical_payload(fmt, payload));
    }
    if (biased || (format == 10 && bits.lo >> 63))
        mantissa = wide_binary(mantissa, wide_shift_left((Integer128){1, 0}, fraction), WIDE_OR, true);
    if (!wide_nonzero(mantissa))
        return pack_decimal(fmt, (Decimal){.negative = negative});

    int exponent = (int)(biased ? biased : 1) - (int)bias - (int)fraction;
    /* Cancel factors of two so exact values use the quantum closest to zero. */
    while (exponent < 0 && !(mantissa.lo & 1)) {
        mantissa = wide_shift_right(mantissa, 1, true);
        exponent++;
    }
    ExactInteger exact;
    exact_assign(&exact, mantissa);
    if (exponent < 0)
        exact_power_five(&exact, (unsigned)-exponent);
    else
        exact_shift(&exact, (unsigned)exponent);
    unsigned char digits[EXACT_DECIMAL_DIGITS];
    int length = exact_decimal_digits(&exact, digits);
    return rounded_decimal(status, fmt, negative, exponent < 0 ? exponent : 0, digits, length, false);
}

Integer128 decimal_from_binary(unsigned width, unsigned format, Integer128 bits)
{
    DecimalStatus status = {0};
    return decimal_from_binary_status(&status, width, format, bits);
}

static Integer128 binary_overflow(DecimalStatus *status, unsigned format, bool negative)
{
    status->flags |= DEC_OVERFLOW | DEC_INEXACT;
    unsigned fraction = wide_fp_precision(format) - 1;
    unsigned maximum = 2 * wide_fp_bias(format) + 1;
    if (overflow_to_infinity(status->rounding, negative))
        return binary_encoding(format, negative, maximum, wide_shift_left((Integer128){1, 0}, fraction));
    return binary_encoding(format, negative, maximum - 1, wide_mask(fraction + 1));
}

Integer128 decimal_to_binary_status(DecimalStatus *status, unsigned format, unsigned width, Integer128 bits)
{
    DecimalFormat fmt = decimal_format(width);
    Decimal n = unpack_decimal(fmt, bits);
    unsigned fraction = wide_fp_precision(format) - 1, bias = wide_fp_bias(format);
    Integer128 leading = wide_shift_left((Integer128){1, 0}, fraction);
    unsigned maximum = 2 * bias + 1;
    if (n.kind != FINITE) {
        Integer128 significand = leading;
        if (n.kind == NAN_VALUE) {
            if (decimal_signaling(fmt, bits))
                status->flags |= DEC_INVALID;
            unsigned payload_width = decimal_payload_width(fmt);
            Integer128 payload = shift_payload(decimal_payload(fmt, bits), payload_width, fraction - 1);
            significand = wide_binary(significand, payload, WIDE_OR, true);
            significand = wide_binary(significand, wide_shift_right(leading, 1, true), WIDE_OR, true);
        }
        return binary_encoding(format, n.negative, maximum, significand);
    }
    Integer128 quotient = {0, 0};
    if (!wide_nonzero(n.coefficient))
        return binary_encoding(format, n.negative, 0, quotient);

    /* c * 10^e = (numerator / denominator) * 2^e, with factors of five
       kept as exact integers until the binary rounding decision. */
    ExactInteger numerator, denominator;
    exact_assign(&numerator, n.coefficient);
    exact_assign(&denominator, (Integer128){1, 0});
    if (n.exponent < 0)
        exact_power_five(&denominator, (unsigned)-n.exponent);
    else
        exact_power_five(&numerator, (unsigned)n.exponent);
    int order = (int)exact_bits(&numerator) - (int)exact_bits(&denominator);
    bool below = order >= 0 ? exact_compare_shifted(&numerator, &denominator, (unsigned)order) < 0 :
                              exact_compare_shifted(&denominator, &numerator, (unsigned)-order) > 0;
    int exponent = n.exponent + order - below;
    int minimum = 1 - (int)bias;
    if (exponent > (int)bias)
        return binary_overflow(status, format, n.negative);
    if (exponent < minimum - (int)fraction - 1) {
        status->flags |= DEC_UNDERFLOW | DEC_INEXACT;
        return binary_encoding(format, n.negative, 0, (Integer128){round_away(status->rounding, n.negative), 0});
    }

    bool tiny = exponent < minimum;
    if (tiny)
        exponent = minimum;
    int scale = n.exponent - exponent + (int)fraction;
    if (scale >= 0)
        exact_shift(&numerator, (unsigned)scale);
    else
        exact_shift(&denominator, (unsigned)-scale);
    int bit = (int)exact_bits(&numerator) - (int)exact_bits(&denominator);
    for (; bit >= 0; bit--) {
        if (exact_compare_shifted(&numerator, &denominator, (unsigned)bit) >= 0) {
            exact_subtract_shifted(&numerator, &denominator, (unsigned)bit);
            quotient = wide_binary(quotient, wide_shift_left((Integer128){1, 0}, (unsigned)bit),
                                   WIDE_OR, true);
        }
    }
    if (numerator.used) {
        status->flags |= DEC_INEXACT;
        if (tiny)
            status->flags |= DEC_UNDERFLOW;
        int half = -exact_compare_shifted(&denominator, &numerator, 1);
        if (increment_rounding(status->rounding, n.negative, half, quotient.lo & 1)) {
            quotient = wide_binary(quotient, (Integer128){1, 0}, WIDE_ADD, true);
            if (wide_nonzero(wide_shift_right(quotient, fraction + 1, true))) {
                quotient = leading;
                exponent++;
            }
        }
    }
    if (exponent > (int)bias)
        return binary_overflow(status, format, n.negative);
    unsigned biased = wide_compare(quotient, leading, true) >= 0 ? (unsigned)(exponent + (int)bias) : 0;
    return binary_encoding(format, n.negative, biased, quotient);
}

Integer128 decimal_to_binary(unsigned format, unsigned width, Integer128 bits)
{
    DecimalStatus status = {0};
    return decimal_to_binary_status(&status, format, width, bits);
}

bool decimal_nonzero(unsigned width, Integer128 value)
{
    Decimal n = unpack_decimal(decimal_format(width), value);
    return n.kind != FINITE || wide_nonzero(n.coefficient);
}

unsigned decimal_properties(unsigned width, Integer128 bits)
{
    DecimalFormat fmt = decimal_format(width);
    Decimal n = unpack_decimal(fmt, bits);
    unsigned sign = n.negative ? FP_NEGATIVE : 0;
    if (n.kind == INFINITY_VALUE)
        return sign | FP_CLASS_INFINITY;
    if (n.kind == NAN_VALUE) {
        bool signaling = decimal_signaling(fmt, bits);
        return sign | FP_CLASS_NAN | (signaling ? FP_SIGNALING : 0);
    }
    if (!wide_nonzero(n.coefficient))
        return sign | FP_CLASS_ZERO;
    unsigned char digits[DECIMAL_DIGITS];
    int length = coefficient_digits(n.coefficient, digits);
    return sign | (n.exponent + length < fmt.digits - fmt.bias ? FP_CLASS_SUBNORMAL : FP_CLASS_NORMAL);
}

Integer128 decimal_special(unsigned width, FloatingBuiltin operation)
{
    DecimalFormat fmt = decimal_format(width);
    Decimal n = {.kind = operation == FP_INFINITY ? INFINITY_VALUE : NAN_VALUE};
    Integer128 bits = pack_decimal(fmt, n);
    if (operation == FP_SNAN) {
        uint64_t header = decimal_header(fmt, bits) | (UINT64_C(1) << (fmt.sign_bit - 6));
        return decimal_bits(fmt, header, bits.lo);
    }
    return bits;
}

static int magnitude_compare(Decimal a, Decimal b)
{
    if (a.kind == INFINITY_VALUE || b.kind == INFINITY_VALUE)
        return (a.kind == INFINITY_VALUE) - (b.kind == INFINITY_VALUE);
    if (a.exponent == b.exponent)
        return wide_compare(a.coefficient, b.coefficient, true);
    unsigned char ad[DECIMAL_DIGITS], bd[DECIMAL_DIGITS];
    int an = coefficient_digits(a.coefficient, ad), bn = coefficient_digits(b.coefficient, bd);
    if (!an || !bn)
        return (an != 0) - (bn != 0);
    int ah = an + a.exponent, bh = bn + b.exponent;
    if (ah != bh)
        return ah < bh ? -1 : 1;
    while (an || bn) {
        unsigned da = an ? ad[--an] : 0, db = bn ? bd[--bn] : 0;
        if (da != db)
            return da < db ? -1 : 1;
    }
    return 0;
}

int decimal_compare_status(DecimalStatus *status, unsigned width, Integer128 left, Integer128 right)
{
    DecimalFormat fmt = decimal_format(width);
    Decimal a = unpack_decimal(fmt, left), b = unpack_decimal(fmt, right);
    if (a.kind == NAN_VALUE || b.kind == NAN_VALUE) {
        if ((a.kind == NAN_VALUE && decimal_signaling(fmt, left)) ||
            (b.kind == NAN_VALUE && decimal_signaling(fmt, right)))
            status->flags |= DEC_INVALID;
        return DEC_UNORDERED;
    }
    if (a.kind == FINITE && !wide_nonzero(a.coefficient))
        a.negative = false;
    if (b.kind == FINITE && !wide_nonzero(b.coefficient))
        b.negative = false;
    return a.negative != b.negative ? (a.negative ? -1 : 1) :
           a.negative ? magnitude_compare(b, a) : magnitude_compare(a, b);
}

int decimal_compare(unsigned width, Integer128 left, Integer128 right)
{
    DecimalStatus status = {0};
    return decimal_compare_status(&status, width, left, right);
}

bool decimal_relation_result(DecimalOperation op, int cmp)
{
    if (cmp == DEC_UNORDERED)
        return op == DEC_NE;
    switch (op) {
    case DEC_LT: return cmp < 0;
    case DEC_LE: return cmp <= 0;
    case DEC_GT: return cmp > 0;
    case DEC_GE: return cmp >= 0;
    case DEC_EQ: return cmp == 0;
    default: return cmp != 0;
    }
}

bool decimal_relation(unsigned width, DecimalOperation op, Integer128 left, Integer128 right)
{
    return decimal_relation_result(op, decimal_compare(width, left, right));
}

static Integer128 add_decimal(DecimalStatus *status, DecimalFormat fmt, Decimal a, Decimal b)
{
    int exponent = a.exponent < b.exponent ? a.exponent : b.exponent;
    bool subtract = a.negative != b.negative;
    if (subtract) {
        int comparison = magnitude_compare(a, b);
        if (!comparison)
            return pack_decimal(fmt, (Decimal){.exponent = exponent, .negative = status->rounding == DEC_DOWNWARD});
        if (comparison < 0) {
            Decimal temp = a;
            a = b;
            b = temp;
        }
    }
    unsigned char ad[DECIMAL_DIGITS], bd[DECIMAL_DIGITS], digits[DECIMAL_EXP_SPAN + DECIMAL_DIGITS + 1];
    int an = coefficient_digits(a.coefficient, ad), bn = coefficient_digits(b.coefficient, bd);
    int as = a.exponent - exponent, bs = b.exponent - exponent;
    int length = as + an > bs + bn ? as + an : bs + bn;
    int carry = 0, used = 0;
    for (int i = 0; i < length; i++) {
        int da = i >= as && i < as + an ? ad[i - as] : 0;
        int db = i >= bs && i < bs + bn ? bd[i - bs] : 0;
        int digit = carry + da + (subtract ? -db : db);
        carry = digit < 0 ? -1 : digit >= 10;
        digits[i] = (unsigned char)(digit - 10 * carry);
        if (digits[i])
            used = i + 1;
    }
    if (carry > 0) {
        digits[length] = (unsigned char)carry;
        used = length + 1;
    }
    return rounded_decimal(status, fmt, a.negative, exponent, digits, used, false);
}

static Integer128 multiply_decimal(DecimalStatus *status, DecimalFormat fmt, Decimal a, Decimal b)
{
    unsigned char ad[DECIMAL_DIGITS], bd[DECIMAL_DIGITS], digits[2 * DECIMAL_DIGITS] = {0};
    int an = coefficient_digits(a.coefficient, ad), bn = coefficient_digits(b.coefficient, bd);
    for (int i = 0; i < an; i++) {
        unsigned carry = 0;
        for (int j = 0; j < bn; j++) {
            unsigned total = ad[i] * (unsigned)bd[j] + digits[i + j] + carry;
            digits[i + j] = (unsigned char)(total % 10);
            carry = total / 10;
        }
        digits[i + bn] = (unsigned char)carry;
    }
    int length = an + bn;
    while (length && !digits[length - 1])
        length--;
    return rounded_decimal(status, fmt, a.negative != b.negative, a.exponent + b.exponent,
                           digits, length, false);
}

static Integer128 divide_decimal(DecimalStatus *status, DecimalFormat fmt, Decimal a, Decimal b)
{
    Integer128 quotient = wide_divide(a.coefficient, b.coefficient, true, false);
    Integer128 remainder = wide_divide(a.coefficient, b.coefficient, true, true);
    unsigned char digits[DECIMAL_DIGITS + 1];
    int exponent = a.exponent - b.exponent;
    int length = coefficient_digits(quotient, digits);
    while (wide_nonzero(remainder) && length <= fmt.digits) {
        remainder = times_ten(remainder, 0);
        unsigned digit = 0;
        while (wide_compare(remainder, b.coefficient, true) >= 0) {
            remainder = wide_subtract(remainder, b.coefficient);
            digit++;
        }
        exponent--;
        if (!length && !digit)
            continue;
        for (int i = length; i > 0; i--)
            digits[i] = digits[i - 1];
        digits[0] = (unsigned char)digit;
        length++;
    }
    return rounded_decimal(status, fmt, a.negative != b.negative, exponent, digits, length,
                           wide_nonzero(remainder));
}

static Integer128 invalid_decimal(DecimalStatus *status, DecimalFormat fmt)
{
    status->flags |= DEC_INVALID;
    return pack_decimal(fmt, (Decimal){.kind = NAN_VALUE});
}

Integer128 decimal_binary_status(DecimalStatus *status, unsigned width, DecimalOperation op,
                                 Integer128 left, Integer128 right)
{
    DecimalFormat fmt = decimal_format(width);
    Decimal a = unpack_decimal(fmt, left), b = unpack_decimal(fmt, right);
    if (a.kind == NAN_VALUE || b.kind == NAN_VALUE) {
        if ((a.kind == NAN_VALUE && decimal_signaling(fmt, left)) ||
            (b.kind == NAN_VALUE && decimal_signaling(fmt, right)))
            status->flags |= DEC_INVALID;
        Integer128 bits = a.kind == NAN_VALUE ? left : right;
        bool negative = a.kind == NAN_VALUE ? a.negative : b.negative;
        return quiet_nan(fmt, negative, decimal_payload(fmt, bits));
    }
    bool zero_a = a.kind == FINITE && !wide_nonzero(a.coefficient);
    bool zero_b = b.kind == FINITE && !wide_nonzero(b.coefficient);
    bool infinity_a = a.kind == INFINITY_VALUE, infinity_b = b.kind == INFINITY_VALUE;
    if (op == DEC_SUB)
        b.negative = !b.negative;
    if (op == DEC_ADD || op == DEC_SUB) {
        if (infinity_a && infinity_b && a.negative != b.negative)
            return invalid_decimal(status, fmt);
        if (infinity_a || infinity_b)
            return pack_decimal(fmt, infinity_a ? a : b);
        return add_decimal(status, fmt, a, b);
    }
    bool negative = a.negative != b.negative;
    if (op == DEC_MUL) {
        if ((zero_a && infinity_b) || (infinity_a && zero_b))
            return invalid_decimal(status, fmt);
        if (infinity_a || infinity_b)
            return pack_decimal(fmt, (Decimal){.negative = negative, .kind = INFINITY_VALUE});
        return multiply_decimal(status, fmt, a, b);
    }
    if ((zero_a && zero_b) || (infinity_a && infinity_b))
        return invalid_decimal(status, fmt);
    if (zero_b && !infinity_a)
        status->flags |= DEC_DIVBYZERO;
    if (infinity_a || zero_b)
        return pack_decimal(fmt, (Decimal){.negative = negative, .kind = INFINITY_VALUE});
    if (infinity_b)
        return pack_decimal(fmt, (Decimal){.negative = negative});
    return divide_decimal(status, fmt, a, b);
}

Integer128 decimal_binary(unsigned width, DecimalOperation op, Integer128 left, Integer128 right)
{
    DecimalStatus status = {0};
    return decimal_binary_status(&status, width, op, left, right);
}
