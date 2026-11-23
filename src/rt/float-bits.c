#include "types.h"
#include "../decimal.h"
#include "../float_builtins.h"
#include "../int128_float.h"

static unsigned format_bytes(unsigned format)
{
    return format & FP_DECIMAL_FORMAT ? format - FP_DECIMAL_FORMAT : wide_fp_bytes(format);
}

static Integer128 sign_mask(unsigned format)
{
    return wide_shift_left((Integer128){1, 0}, format_bytes(format) * 8 - 1);
}

Integer128 floating_sign(unsigned format, Integer128 value, Integer128 sign)
{
    Integer128 mask = sign_mask(format);
    return (Integer128){(value.lo & ~mask.lo) | (sign.lo & mask.lo),
                        (value.hi & ~mask.hi) | (sign.hi & mask.hi)};
}

static bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static Integer128 parse_payload(const char *p)
{
    Integer128 value = {0, 0};
    if (!p)
        return value;
    while (whitespace(*p))
        p++;
    if (*p == '+' || *p == '-')
        p++;
    int base = *p == '0' ? 8 : 10;
    if (base == 8 && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; *p; p++) {
        int digit = digit_value(*p);
        if (digit < 0 || digit >= base)
            return (Integer128){0, 0};
        value = wide_binary(value, (Integer128){(unsigned)base, 0}, WIDE_MUL, true);
        value = wide_binary(value, (Integer128){(unsigned)digit, 0}, WIDE_ADD, true);
    }
    return value;
}

Integer128 floating_special(unsigned format, FloatingBuiltin operation, const char *payload)
{
    if (format & FP_DECIMAL_FORMAT)
        return decimal_special(format - FP_DECIMAL_FORMAT, operation);
    unsigned fraction = wide_fp_precision(format) - 1;
    Integer128 bits = {0, 0};
    if (operation != FP_INFINITY) {
        bits = wide_binary(parse_payload(payload), wide_mask(fraction - 1), WIDE_AND, true);
        if (operation == FP_NAN || !wide_nonzero(bits)) {
            unsigned bit = fraction - (operation == FP_NAN ? 1u : 2u);
            bits = wide_binary(bits, wide_shift_left((Integer128){1, 0}, bit), WIDE_OR, true);
        }
    }
    unsigned exponent = 2 * wide_fp_bias(format) + 1;
    if (format == 10)
        return (Integer128){bits.lo | (UINT64_C(1) << 63), exponent};
    return wide_binary(bits, wide_shift_left((Integer128){exponent, 0}, fraction), WIDE_OR, true);
}

static unsigned binary_properties(unsigned format, Integer128 value)
{
    unsigned fraction = wide_fp_precision(format) - 1;
    unsigned maximum = 2 * wide_fp_bias(format) + 1;
    uint64_t sign_exp = format == 10 ? value.hi : wide_shift_right(value, fraction, true).lo;
    unsigned sign = sign_exp & (maximum + 1) ? FP_NEGATIVE : 0;
    unsigned exponent = (unsigned)sign_exp & maximum;
    Integer128 trailing = wide_binary(value, wide_mask(fraction), WIDE_AND, true);
    bool nonzero = wide_nonzero(trailing);
    if (exponent == maximum) {
        if (!nonzero)
            return sign | FP_CLASS_INFINITY;
        return sign | FP_CLASS_NAN |
               (wide_nonzero(wide_shift_right(trailing, fraction - 1, true)) ? 0 : FP_SIGNALING);
    }
    if (exponent || (format == 10 && value.lo >> 63))
        return sign | FP_CLASS_NORMAL;
    return sign | (nonzero ? FP_CLASS_SUBNORMAL : FP_CLASS_ZERO);
}

static unsigned properties(unsigned format, Integer128 value)
{
    return format & FP_DECIMAL_FORMAT ? decimal_properties(format - FP_DECIMAL_FORMAT, value) :
                                       binary_properties(format, value);
}

static Integer128 binary_magnitude(unsigned format, Integer128 value)
{
    if (format == 10) {
        value.hi &= 0x7fff;
        if (!value.hi && value.lo >> 63)
            value.hi = 1;
        return value;
    }
    return floating_sign(format, value, (Integer128){0, 0});
}

static int float_order(unsigned format, Integer128 a, Integer128 b, unsigned pa)
{
    if (format & FP_DECIMAL_FORMAT)
        return decimal_compare(format - FP_DECIMAL_FORMAT, a, b);
    unsigned pb = binary_properties(format, b);
    if ((pa & FP_CLASS_MASK) == FP_CLASS_NAN || (pb & FP_CLASS_MASK) == FP_CLASS_NAN)
        return DEC_UNORDERED;
    if ((pa & FP_CLASS_MASK) == FP_CLASS_ZERO && (pb & FP_CLASS_MASK) == FP_CLASS_ZERO)
        return 0;
    bool an = (pa & FP_NEGATIVE) != 0, bn = (pb & FP_NEGATIVE) != 0;
    if (an != bn)
        return an ? -1 : 1;
    int cmp = wide_compare(binary_magnitude(format, a), binary_magnitude(format, b), true);
    return an ? -cmp : cmp;
}

int floating_ordered_predicate(FloatingBuiltin op, int cmp)
{
    if (cmp == DEC_UNORDERED)
        return op == FP_ISUNORDERED;
    switch (op) {
    case FP_ISGREATER: return cmp > 0;
    case FP_ISGREATEREQUAL: return cmp >= 0;
    case FP_ISLESS: return cmp < 0;
    case FP_ISLESSEQUAL: return cmp <= 0;
    case FP_ISLESSGREATER: return cmp != 0;
    default: return 0;
    }
}

int floating_predicate(unsigned format, FloatingBuiltin op, Integer128 left, Integer128 right)
{
    unsigned prop = properties(format, left), class = prop & FP_CLASS_MASK;
    switch (op) {
    case FP_SIGNBIT: return (prop & FP_NEGATIVE) != 0;
    case FP_ISNAN: return class == FP_CLASS_NAN;
    case FP_ISSIGNALING: return (prop & FP_SIGNALING) != 0;
    case FP_ISINF: return class == FP_CLASS_INFINITY;
    case FP_ISINF_SIGN: return class == FP_CLASS_INFINITY ? (prop & FP_NEGATIVE ? -1 : 1) : 0;
    case FP_ISFINITE: return class != FP_CLASS_INFINITY && class != FP_CLASS_NAN;
    case FP_ISNORMAL: return class == FP_CLASS_NORMAL;
    case FP_CLASSIFY: return (int)class;
    default: break;
    }
    return floating_ordered_predicate(op, float_order(format, left, right, prop));
}

static Integer128 read_float(const void *p, unsigned format)
{
    Integer128 value = {0, 0};
    if (p)
        memcpy(&value, p, format_bytes(format));
    return value;
}

static void write_float(void *out, unsigned format, Integer128 value)
{
    unsigned char *bytes = out;
    unsigned size = format_bytes(format);
    if (size > sizeof value.lo) {
        memcpy(bytes + sizeof value.lo, &value.hi, sizeof value.hi);
        size = sizeof value.lo;
    }
    memcpy(bytes, &value.lo, size);
}

void __mcc_fp_nan(void *out, const char *payload, unsigned format, unsigned operation)
{
    write_float(out, format, floating_special(format, (FloatingBuiltin)operation, payload));
}

void __mcc_fp_sign(void *out, const void *left, const void *right, unsigned format)
{
    write_float(out, format, floating_sign(format, read_float(left, format), read_float(right, format)));
}

int __mcc_fp_predicate(const void *left, const void *right, unsigned format, unsigned operation)
{
    return floating_predicate(format, (FloatingBuiltin)operation, read_float(left, format), read_float(right, format));
}

int __mcc_fp_classify(const void *value, unsigned format, int nan, int infinity, int normal, int subnormal, int zero)
{
    int classes[] = {nan, infinity, normal, subnormal, zero};
    return classes[properties(format, read_float(value, format)) & FP_CLASS_MASK];
}
