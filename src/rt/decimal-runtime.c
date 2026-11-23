#include "types.h"
#include "decimal-internal.h"
#include "../float_builtins.h"
#include "../int128_float.h"

/* A linked shared object and its executable use the same per-thread environment. */
_Thread_local DecimalStatus __mcc_decimal_environment;

void __dfp_set_round(int mode)
{
    __mcc_decimal_environment.rounding = mode;
}

int __dfp_get_round(void)
{
    return __mcc_decimal_environment.rounding;
}

void __dfp_clear_except(void)
{
    __mcc_decimal_environment.flags = 0;
}

static unsigned bid_exceptions(unsigned mask)
{
#ifdef __aarch64__
    return (mask & 1u) | ((mask & 0x1eu) << 1);
#else
    return mask & 0x3du;
#endif
}

int __dfp_test_except(int mask)
{
    unsigned flags = __mcc_decimal_environment.flags & bid_exceptions((unsigned)mask);
#ifdef __aarch64__
    flags = (flags & 1u) | ((flags & 0x3cu) >> 1);
#endif
    return (int)flags;
}

void __dfp_raise_except(int mask)
{
    __mcc_decimal_environment.flags |= bid_exceptions((unsigned)mask);
}

int fe_dec_getround(void)
{
    return __dfp_get_round();
}

int fe_dec_setround(int mode)
{
    if (mode < DEC_NEAREST || mode > DEC_NEAREST_AWAY)
        return 1;
    __dfp_set_round(mode);
    return 0;
}

static Integer128 read_bits(const void *input, unsigned size)
{
    Integer128 bits = {0, 0};
    memcpy(&bits, input, size);
    return bits;
}

static void write_bits(void *out, unsigned size, Integer128 bits)
{
    unsigned char *bytes = out;
    if (size > sizeof bits.lo) {
        memcpy(bytes + sizeof bits.lo, &bits.hi, sizeof bits.hi);
        size = sizeof bits.lo;
    }
    memcpy(bytes, &bits.lo, size);
}

void __mcc_decimal_binary(void *out, const void *left, const void *right, unsigned width, unsigned op)
{
    Integer128 bits = decimal_binary_status(&__mcc_decimal_environment, width, (DecimalOperation)op,
                                            read_bits(left, width), read_bits(right, width));
    write_bits(out, width, bits);
}

int __mcc_decimal_compare(const void *left, const void *right, unsigned width)
{
    return decimal_compare_status(&__mcc_decimal_environment, width, read_bits(left, width), read_bits(right, width));
}

int __mcc_decimal_relation(const void *left, const void *right, unsigned width, unsigned op)
{
    return decimal_relation_result((DecimalOperation)op, __mcc_decimal_compare(left, right, width));
}

int __mcc_decimal_predicate(const void *left, const void *right, unsigned width, unsigned op)
{
    return floating_ordered_predicate((FloatingBuiltin)op, __mcc_decimal_compare(left, right, width));
}

void __mcc_decimal_convert(void *out, const void *input, unsigned width, unsigned source_width)
{
    Integer128 bits = decimal_convert_status(&__mcc_decimal_environment, width, source_width, read_bits(input, source_width));
    write_bits(out, width, bits);
}

void __mcc_decimal_from_integer(void *out, const void *input, unsigned width, unsigned size, bool uns)
{
    Integer128 bits = read_bits(input, size);
    if (!uns && size < 16) {
        unsigned shift = 64 - 8 * size;
        bits.lo = (uint64_t)((int64_t)(bits.lo << shift) >> shift);
        bits.hi = bits.lo >> 63 ? UINT64_MAX : 0;
    }
    bits = decimal_from_integer_status(&__mcc_decimal_environment, width, bits, uns);
    write_bits(out, width, bits);
}

void __mcc_decimal_to_integer(void *out, const void *input, unsigned width, unsigned size, bool uns)
{
    Integer128 integer = {0, 0};
    if (!decimal_to_integer_status(&__mcc_decimal_environment, width, read_bits(input, width), size * 8, uns, &integer)) {
        __mcc_decimal_environment.flags |= DEC_INVALID;
        integer = wide_shift_left((Integer128){1, 0}, size * 8 - 1);
    }
    write_bits(out, size, integer);
}

void __mcc_decimal_from_binary(void *out, const void *input, unsigned width, unsigned format)
{
    Integer128 bits = decimal_from_binary_status(&__mcc_decimal_environment, width, format, read_bits(input, wide_fp_bytes(format)));
    write_bits(out, width, bits);
}

void __mcc_decimal_to_binary(void *out, const void *input, unsigned width, unsigned format)
{
    Integer128 bits = decimal_to_binary_status(&__mcc_decimal_environment, format, width, read_bits(input, width));
    write_bits(out, format == 10 ? 16u : wide_fp_bytes(format), bits);
}
