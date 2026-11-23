#include "types.h"
#include "fp-round.h"
#include "../integer_bits.h"
#include "../integer_overflow.h"
#include "../int128_float.h"

int __popcountdi2(uint64_t);
uint64_t __mcc_integer_bits(uint64_t, unsigned, unsigned);
bool __mcc_integer_overflow(uint64_t, uint64_t, unsigned, unsigned, unsigned);
bool __mcc_integer128_overflow(const Integer128 *, const Integer128 *, unsigned, unsigned, unsigned);
void __mcc_int128_binary(Integer128 *, const Integer128 *, const Integer128 *, unsigned, bool);
void __mcc_int128_to_float(void *, const Integer128 *, unsigned, bool);
void __mcc_int128_from_float(Integer128 *, const void *, unsigned, bool);
Integer128 __mcc_int128_swap(Integer128);
Integer128 __divti3(Integer128, Integer128);
Integer128 __modti3(Integer128, Integer128);
Integer128 __udivti3(Integer128, Integer128);
Integer128 __umodti3(Integer128, Integer128);
Integer128 __udivmodti4(Integer128, Integer128, Integer128 *);

int __popcountdi2(uint64_t value)
{
    return (int)integer_bits(value, BITS_POPCOUNT, 64);
}

uint64_t __mcc_integer_bits(uint64_t value, unsigned operation, unsigned width)
{
    return integer_bits(value, (IntegerBits)operation, width);
}

bool __mcc_integer_overflow(uint64_t a, uint64_t b, unsigned operation, unsigned width,
                            unsigned flags)
{
    return integer_overflow(a, b, operation, width, flags);
}

void __mcc_int128_binary(Integer128 *out, const Integer128 *a, const Integer128 *b,
                         unsigned op, bool uns)
{
    if ((op == WIDE_DIV || op == WIDE_REM) && !wide_nonzero(*b))
        __builtin_trap();
    *out = wide_binary(*a, *b, (WideOperation)op, uns);
}

bool __mcc_integer128_overflow(const Integer128 *a, const Integer128 *b, unsigned op,
                               unsigned width, unsigned flags)
{
    return integer128_overflow(*a, *b, op, width, flags);
}

void __mcc_int128_to_float(void *out, const Integer128 *a, unsigned format, bool uns)
{
    unsigned flags = 0;
    Integer128 bits = round_wide_float_bits(*a, format, uns, fp_rounding_mode(), &flags);
    memcpy(out, &bits, format >= 10 ? 16 : wide_fp_bytes(format));
    if (flags)
        mcc_raise_fp(flags);
}

void __mcc_int128_from_float(Integer128 *out, const void *a, unsigned format, bool uns)
{
    Integer128 bits = {0, 0};
    memcpy(&bits, a, wide_fp_bytes(format));
    wide_integer_bits(bits, format, uns, out);
}

typedef int WideSigned __attribute__((mode(TI)));
typedef unsigned int WideUnsigned __attribute__((mode(TI)));
typedef float QuadABI __attribute__((mode(TF)));

WideSigned __fixsfti(float);
WideUnsigned __fixunssfti(float);
QuadABI __floattitf(WideSigned);
QuadABI __floatuntitf(WideUnsigned);
WideSigned __fixtfti(QuadABI);
WideUnsigned __fixunstfti(QuadABI);

WideSigned __fixsfti(float value) { return (WideSigned)value; }
WideUnsigned __fixunssfti(float value) { return (WideUnsigned)value; }
QuadABI __floattitf(WideSigned value) { return (QuadABI)value; }
QuadABI __floatuntitf(WideUnsigned value) { return (QuadABI)value; }
WideSigned __fixtfti(QuadABI value) { return (WideSigned)value; }
WideUnsigned __fixunstfti(QuadABI value) { return (WideUnsigned)value; }

Integer128 __mcc_int128_swap(Integer128 value)
{
    return (Integer128){integer_bits(value.hi, BITS_SWAP, 64),
                        integer_bits(value.lo, BITS_SWAP, 64)};
}

/* A pair of doublewords uses the same argument and result registers as TI. */
static Integer128 division_result(Integer128 a, Integer128 b, WideOperation op, bool uns)
{
    Integer128 result;
    __mcc_int128_binary(&result, &a, &b, op, uns);
    return result;
}

Integer128 __divti3(Integer128 a, Integer128 b)
{
    return division_result(a, b, WIDE_DIV, false);
}

Integer128 __modti3(Integer128 a, Integer128 b)
{
    return division_result(a, b, WIDE_REM, false);
}

Integer128 __udivti3(Integer128 a, Integer128 b)
{
    return division_result(a, b, WIDE_DIV, true);
}

Integer128 __umodti3(Integer128 a, Integer128 b)
{
    return division_result(a, b, WIDE_REM, true);
}

Integer128 __udivmodti4(Integer128 a, Integer128 b, Integer128 *remainder)
{
    Integer128 quotient = __udivti3(a, b);
    *remainder = wide_subtract(a, wide_binary(quotient, b, WIDE_MUL, true));
    return quotient;
}
