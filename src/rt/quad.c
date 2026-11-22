#include "types.h"

/* Binary128 uses the long double ABI on AArch64 and the SSE ABI for
   _Float128 on x86-64. Keep the arithmetic in the integer representation. */
typedef struct { uint64_t lo, hi; } Wide;

#if defined(__aarch64__)
typedef long double QuadValue;
#define QUAD_ABI(name) __ ## name
#else
typedef Wide QuadValue;
#define QUAD_ABI(name) mcc_ ## name
#endif

QuadValue QUAD_ABI(addtf3)(QuadValue, QuadValue);
QuadValue QUAD_ABI(subtf3)(QuadValue, QuadValue);
QuadValue QUAD_ABI(multf3)(QuadValue, QuadValue);
QuadValue QUAD_ABI(divtf3)(QuadValue, QuadValue);
QuadValue QUAD_ABI(negtf2)(QuadValue);
int QUAD_ABI(eqtf2)(QuadValue, QuadValue);
int QUAD_ABI(netf2)(QuadValue, QuadValue);
int QUAD_ABI(letf2)(QuadValue, QuadValue);
int QUAD_ABI(lttf2)(QuadValue, QuadValue);
int QUAD_ABI(getf2)(QuadValue, QuadValue);
int QUAD_ABI(gttf2)(QuadValue, QuadValue);
int QUAD_ABI(unordtf2)(QuadValue, QuadValue);
QuadValue QUAD_ABI(extendsftf2)(float);
QuadValue QUAD_ABI(extenddftf2)(double);
float QUAD_ABI(trunctfsf2)(QuadValue);
double QUAD_ABI(trunctfdf2)(QuadValue);
QuadValue QUAD_ABI(floatditf)(int64_t);
QuadValue QUAD_ABI(floatunditf)(uint64_t);
QuadValue QUAD_ABI(floatsitf)(int32_t);
QuadValue QUAD_ABI(floatunsitf)(uint32_t);
int64_t QUAD_ABI(fixtfdi)(QuadValue);
uint64_t QUAD_ABI(fixunstfdi)(QuadValue);
int32_t QUAD_ABI(fixtfsi)(QuadValue);
uint32_t QUAD_ABI(fixunstfsi)(QuadValue);
uint64_t mcc_get_fpcr(void);
void mcc_raise_fp(unsigned);

typedef struct { Wide significand; int exponent; bool negative; } Quad;

enum { BIAS = 16383, MIN_EXP = 1 - BIAS, MAX_EXP = BIAS, FRACTION = 112, ROUND_BITS = 3 };
enum { NEAREST, UPWARD, DOWNWARD, TOWARD_ZERO };
enum { INVALID = 1, DIVISION_BY_ZERO = 2, OVERFLOW = 4, UNDERFLOW = 8, INEXACT = 16 };

#define SIGN (UINT64_C(1) << 63)
#define LEADING (UINT64_C(1) << 48)
#define QUIET (LEADING >> 1)

static Wide representation(QuadValue x)
{
#if defined(__aarch64__)
    Wide bits;
    memcpy(&bits, &x, sizeof bits);
    return bits;
#else
    return x;
#endif
}

static QuadValue value(Wide bits)
{
#if defined(__aarch64__)
    QuadValue x;
    memcpy(&x, &bits, sizeof x);
    return x;
#else
    return bits;
#endif
}

static bool nonzero(Wide x)
{
    return (x.hi | x.lo) != 0;
}

static int order(Wide a, Wide b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    return (a.lo > b.lo) - (a.lo < b.lo);
}

static Wide plus(Wide a, Wide b)
{
    uint64_t lo = a.lo + b.lo;
    return (Wide){lo, a.hi + b.hi + (lo < a.lo)};
}

static Wide minus(Wide a, Wide b)
{
    return (Wide){a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo)};
}

static Wide left(Wide a, unsigned n)
{
    if (!n)
        return a;
    if (n >= 64)
        return (Wide){0, a.lo << (n - 64)};
    return (Wide){a.lo << n, a.hi << n | a.lo >> (64 - n)};
}

static Wide right(Wide a, unsigned n)
{
    if (!n)
        return a;
    if (n >= 128)
        return (Wide){0, 0};
    if (n >= 64)
        return (Wide){a.hi >> (n - 64), 0};
    return (Wide){a.lo >> n | a.hi << (64 - n), a.hi >> n};
}

/* The low bit records whether any discarded bit was set. */
static Wide sticky_right(Wide a, unsigned n)
{
    Wide b = right(a, n);
    b.lo |= n >= 128 ? nonzero(a) : order(left(b, n), a) != 0;
    return b;
}

static int top_bit(Wide a)
{
    uint64_t word = a.hi ? a.hi : a.lo;
    int bit = a.hi ? 64 : 0;
    for (; word > 1; word >>= 1)
        bit++;
    return bit;
}

static unsigned rounding_mode(void)
{
#if defined(__aarch64__)
    return (unsigned)(mcc_get_fpcr() >> 22) & 3;
#else
    unsigned mode = (unsigned)(mcc_get_fpcr() >> 13) & 3;
    return mode == 1 ? DOWNWARD : mode == 2 ? UPWARD : mode;
#endif
}

static bool directed_away(unsigned mode, bool negative)
{
    return negative ? mode == DOWNWARD : mode == UPWARD;
}

static Wide rounded(Wide a, bool negative)
{
    Wide result = right(a, ROUND_BITS);
    unsigned tail = (unsigned)(a.lo & 7);
    if (!tail)
        return result;
    unsigned mode = rounding_mode();
    bool increment = mode == NEAREST ? tail > 4 || (tail == 4 && (result.lo & 1)) : directed_away(mode, negative);
    return increment ? plus(result, (Wide){1, 0}) : result;
}

static Wide signed_zero(bool negative)
{
    return (Wide){0, negative ? SIGN : 0};
}

static Wide infinity(bool negative)
{
    return (Wide){0, (negative ? SIGN : 0) | (UINT64_C(0x7fff) << 48)};
}

static bool special(Wide x)
{
    return ((x.hi >> 48) & 0x7fff) == 0x7fff;
}

static bool nan_bits(Wide x)
{
    return special(x) && ((x.hi & (LEADING - 1)) || x.lo);
}

static Wide quiet_nan(Wide x)
{
    x.hi |= QUIET;
    return x;
}

static bool signaling_nan(Wide x)
{
    return nan_bits(x) && !(x.hi & QUIET);
}

static Wide propagate_nan(Wide a, Wide b)
{
    if (signaling_nan(a) || signaling_nan(b))
        mcc_raise_fp(INVALID);
    return quiet_nan(nan_bits(a) ? a : b);
}

static Wide invalid(void)
{
    mcc_raise_fp(INVALID);
    return quiet_nan(infinity(false));
}

static bool overflow_to_infinity(bool negative)
{
    mcc_raise_fp(OVERFLOW | INEXACT);
    unsigned mode = rounding_mode();
    return mode == NEAREST || directed_away(mode, negative);
}

static Quad decode(Wide bits)
{
    int biased = (int)((bits.hi >> 48) & 0x7fff);
    Quad q = {{bits.lo, bits.hi & (LEADING - 1)}, biased ? biased - BIAS : MIN_EXP, (bits.hi & SIGN) != 0};
    if (biased) {
        q.significand.hi |= LEADING;
    } else if (nonzero(q.significand)) {
        unsigned shift = (unsigned)(FRACTION - top_bit(q.significand));
        q.significand = left(q.significand, shift);
        q.exponent -= (int)shift;
    }
    return q;
}

/* The exponent is relative to bit 115, including the three rounding bits. */
static Wide encode(Wide sig, int exponent, bool negative)
{
    if (!nonzero(sig))
        return signed_zero(negative);
    int shift = top_bit(sig) - (FRACTION + ROUND_BITS);
    exponent += shift;
    sig = shift < 0 ? left(sig, (unsigned)-shift) : sticky_right(sig, (unsigned)shift);
    if (exponent < MIN_EXP) {
        sig = sticky_right(sig, (unsigned)(MIN_EXP - exponent));
        exponent = MIN_EXP;
    }
    bool inexact = (sig.lo & 7) != 0;
    sig = rounded(sig, negative);
    if (sig.hi >= 2 * LEADING) {
        sig = right(sig, 1);
        exponent++;
    }
    if (exponent > MAX_EXP) {
        Wide inf = infinity(negative);
        return overflow_to_infinity(negative) ? inf : (Wide){UINT64_MAX, inf.hi - 1};
    }
    uint64_t biased = sig.hi & LEADING ? (uint64_t)(exponent + BIAS) : 0;
    if (inexact)
        mcc_raise_fp(INEXACT | (biased ? 0u : UNDERFLOW));
    return (Wide){sig.lo, (negative ? SIGN : 0) | biased << 48 | (sig.hi & (LEADING - 1))};
}

static Wide add(Wide a, Wide b)
{
    if (nan_bits(a) || nan_bits(b))
        return propagate_nan(a, b);
    if (special(a) && special(b) && ((a.hi ^ b.hi) & SIGN))
        return invalid();
    if (special(a) || special(b))
        return special(a) ? a : b;
    Quad x = decode(a), y = decode(b);
    if (!nonzero(x.significand) && !nonzero(y.significand))
        return signed_zero(x.negative == y.negative ? x.negative : rounding_mode() == DOWNWARD);
    if (!nonzero(x.significand) || !nonzero(y.significand))
        return nonzero(x.significand) ? a : b;
    if (x.exponent < y.exponent || (x.exponent == y.exponent && order(x.significand, y.significand) < 0)) {
        Quad swap = x;
        x = y;
        y = swap;
    }
    Wide u = left(x.significand, ROUND_BITS);
    Wide v = sticky_right(left(y.significand, ROUND_BITS), (unsigned)(x.exponent - y.exponent));
    Wide sum = x.negative == y.negative ? plus(u, v) : minus(u, v);
    return encode(sum, x.exponent, nonzero(sum) ? x.negative : rounding_mode() == DOWNWARD);
}

static Wide multiply_significands(Wide a, Wide b)
{
    uint32_t x[] = {(uint32_t)a.lo, (uint32_t)(a.lo >> 32), (uint32_t)a.hi, (uint32_t)(a.hi >> 32)};
    uint32_t y[] = {(uint32_t)b.lo, (uint32_t)(b.lo >> 32), (uint32_t)b.hi, (uint32_t)(b.hi >> 32)};
    uint32_t product[8] = {0};
    for (unsigned i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < 4; j++) {
            uint64_t sum = (uint64_t)x[i] * y[j] + product[i + j] + carry;
            product[i + j] = (uint32_t)sum;
            carry = sum >> 32;
        }
        product[i + 4] = (uint32_t)carry;
    }
    /* Shift right by 112 - 3 bits, keeping the discarded bits as sticky. */
    uint64_t lo = (product[3] >> 13) | (uint64_t)product[4] << 19 | (uint64_t)product[5] << 51;
    uint64_t hi = (product[5] >> 13) | (uint64_t)product[6] << 19 | (uint64_t)product[7] << 51;
    lo |= (product[0] | product[1] | product[2] | (product[3] & 0x1fff)) != 0;
    return (Wide){lo, hi};
}

QuadValue QUAD_ABI(addtf3)(QuadValue a, QuadValue b)
{
    return value(add(representation(a), representation(b)));
}

QuadValue QUAD_ABI(subtf3)(QuadValue a, QuadValue b)
{
    Wide rhs = representation(b);
    rhs.hi ^= SIGN;
    return value(add(representation(a), rhs));
}

QuadValue QUAD_ABI(multf3)(QuadValue a, QuadValue b)
{
    Wide u = representation(a), v = representation(b);
    bool negative = ((u.hi ^ v.hi) & SIGN) != 0;
    if (nan_bits(u) || nan_bits(v))
        return value(propagate_nan(u, v));
    Quad x = decode(u), y = decode(v);
    bool zero = !nonzero(x.significand) || !nonzero(y.significand);
    if (special(u) || special(v))
        return value(zero ? invalid() : infinity(negative));
    return value(encode(multiply_significands(x.significand, y.significand), x.exponent + y.exponent, negative));
}

QuadValue QUAD_ABI(divtf3)(QuadValue a, QuadValue b)
{
    Wide u = representation(a), v = representation(b);
    bool negative = ((u.hi ^ v.hi) & SIGN) != 0;
    if (nan_bits(u) || nan_bits(v))
        return value(propagate_nan(u, v));
    Quad x = decode(u), y = decode(v);
    bool x_zero = !nonzero(x.significand), y_zero = !nonzero(y.significand);
    if ((special(u) && special(v)) || (x_zero && y_zero))
        return value(invalid());
    if (special(u))
        return value(infinity(negative));
    if (y_zero) {
        mcc_raise_fp(DIVISION_BY_ZERO);
        return value(infinity(negative));
    }
    if (x_zero || special(v))
        return value(signed_zero(negative));

    int exponent = x.exponent - y.exponent;
    Wide remainder = x.significand, result = {0};
    if (order(remainder, y.significand) < 0) {
        remainder = left(remainder, 1);
        exponent--;
    }
    for (int i = 0; i <= FRACTION + ROUND_BITS; i++) {
        result = left(result, 1);
        if (order(remainder, y.significand) >= 0) {
            remainder = minus(remainder, y.significand);
            result.lo |= 1;
        }
        remainder = left(remainder, 1);
    }
    result.lo |= nonzero(remainder);
    return value(encode(result, exponent, negative));
}

QuadValue QUAD_ABI(negtf2)(QuadValue a)
{
    Wide bits = representation(a);
    bits.hi ^= SIGN;
    return value(bits);
}

static int compare(QuadValue a, QuadValue b, int unordered, bool ordered)
{
    Wide u = representation(a), v = representation(b);
    if (nan_bits(u) || nan_bits(v)) {
        if (ordered || signaling_nan(u) || signaling_nan(v))
            mcc_raise_fp(INVALID);
        return unordered;
    }
    bool neg_u = (u.hi & SIGN) != 0, neg_v = (v.hi & SIGN) != 0;
    u.hi &= ~SIGN;
    v.hi &= ~SIGN;
    if ((!nonzero(u) && !nonzero(v)) || neg_u == neg_v)
        return neg_u ? order(v, u) : order(u, v);
    return neg_u ? -1 : 1;
}

int QUAD_ABI(eqtf2)(QuadValue a, QuadValue b) { return compare(a, b, 1, false); }
int QUAD_ABI(netf2)(QuadValue a, QuadValue b) { return compare(a, b, 1, false); }
int QUAD_ABI(letf2)(QuadValue a, QuadValue b) { return compare(a, b, 1, true); }
int QUAD_ABI(lttf2)(QuadValue a, QuadValue b) { return compare(a, b, 1, true); }
int QUAD_ABI(getf2)(QuadValue a, QuadValue b) { return compare(a, b, -1, true); }
int QUAD_ABI(gttf2)(QuadValue a, QuadValue b) { return compare(a, b, -1, true); }

int QUAD_ABI(unordtf2)(QuadValue a, QuadValue b)
{
    Wide u = representation(a), v = representation(b);
    if (signaling_nan(u) || signaling_nan(v))
        mcc_raise_fp(INVALID);
    return nan_bits(u) || nan_bits(v);
}

static QuadValue extend(uint64_t bits, unsigned fraction, unsigned exp_bits)
{
    unsigned limit = (1u << exp_bits) - 1;
    int bias = (int)(limit >> 1);
    unsigned biased = (unsigned)(bits >> fraction) & limit;
    uint64_t leading = UINT64_C(1) << fraction;
    uint64_t mantissa = bits & (leading - 1);
    bool negative = bits >> (fraction + exp_bits) != 0;
    if (biased == limit) {
        Wide result = left((Wide){mantissa, 0}, FRACTION - fraction);
        result.hi |= infinity(negative).hi;
        if (signaling_nan(result))
            mcc_raise_fp(INVALID);
        return value(mantissa ? quiet_nan(result) : result);
    }
    int exponent = biased ? (int)biased - bias : 1 - bias;
    if (biased)
        mantissa |= leading;
    return value(encode(left((Wide){mantissa, 0}, FRACTION + ROUND_BITS - fraction), exponent, negative));
}

QuadValue QUAD_ABI(extendsftf2)(float a)
{
    uint32_t bits;
    memcpy(&bits, &a, sizeof bits);
    return extend(bits, 23, 8);
}

QuadValue QUAD_ABI(extenddftf2)(double a)
{
    uint64_t bits;
    memcpy(&bits, &a, sizeof bits);
    return extend(bits, 52, 11);
}

static uint64_t truncate(QuadValue a, unsigned fraction, unsigned exp_bits)
{
    Wide bits = representation(a);
    Quad x = decode(bits);
    uint64_t sign = (uint64_t)x.negative << (fraction + exp_bits);
    unsigned limit = (1u << exp_bits) - 1;
    int bias = (int)(limit >> 1), minimum = 1 - bias;
    if (special(bits)) {
        uint64_t payload = right((Wide){bits.lo, bits.hi & (LEADING - 1)}, FRACTION - fraction).lo;
        if (nan_bits(bits)) {
            payload |= UINT64_C(1) << (fraction - 1);
            if (signaling_nan(bits))
                mcc_raise_fp(INVALID);
        }
        return sign | (uint64_t)limit << fraction | payload;
    }
    unsigned shift = FRACTION - fraction - ROUND_BITS;
    if (x.exponent < minimum) {
        shift += (unsigned)(minimum - x.exponent);
        x.exponent = minimum;
    }
    Wide extended = sticky_right(x.significand, shift);
    uint64_t significand = rounded(extended, x.negative).lo;
    uint64_t leading = UINT64_C(1) << fraction;
    if (significand >= 2 * leading) {
        x.exponent++;
        significand >>= 1;
    }
    if (x.exponent > bias) {
        uint64_t inf = (uint64_t)limit << fraction;
        return sign | (overflow_to_infinity(x.negative) ? inf : inf - 1);
    }
    uint64_t exponent = significand & leading ? (uint64_t)(x.exponent + bias) : 0;
    if (extended.lo & 7)
        mcc_raise_fp(INEXACT | (exponent ? 0u : UNDERFLOW));
    return sign | exponent << fraction | (significand & (leading - 1));
}

float QUAD_ABI(trunctfsf2)(QuadValue a)
{
    uint32_t bits = (uint32_t)truncate(a, 23, 8);
    float x;
    memcpy(&x, &bits, sizeof x);
    return x;
}

double QUAD_ABI(trunctfdf2)(QuadValue a)
{
    uint64_t bits = truncate(a, 52, 11);
    double x;
    memcpy(&x, &bits, sizeof x);
    return x;
}

QuadValue QUAD_ABI(floatunditf)(uint64_t a) { return value(encode((Wide){a, 0}, FRACTION + ROUND_BITS, false)); }

QuadValue QUAD_ABI(floatditf)(int64_t a)
{
    uint64_t magnitude = a < 0 ? 0 - (uint64_t)a : (uint64_t)a;
    return value(encode((Wide){magnitude, 0}, FRACTION + ROUND_BITS, a < 0));
}

QuadValue QUAD_ABI(floatsitf)(int32_t a) { return QUAD_ABI(floatditf)(a); }
QuadValue QUAD_ABI(floatunsitf)(uint32_t a) { return QUAD_ABI(floatunditf)(a); }

static uint64_t integer(Quad x, uint64_t limit, int bits)
{
    if (!nonzero(x.significand) || x.exponent < 0)
        return 0;
    if (x.exponent >= bits)
        return limit;
    return right(x.significand, (unsigned)(FRACTION - x.exponent)).lo;
}

int64_t QUAD_ABI(fixtfdi)(QuadValue a)
{
    Quad x = decode(representation(a));
    uint64_t magnitude = integer(x, x.negative ? SIGN : (uint64_t)INT64_MAX, 63);
    return (int64_t)(x.negative ? 0 - magnitude : magnitude);
}

uint64_t QUAD_ABI(fixunstfdi)(QuadValue a)
{
    Quad x = decode(representation(a));
    return x.negative ? 0 : integer(x, UINT64_MAX, 64);
}

int32_t QUAD_ABI(fixtfsi)(QuadValue a)
{
    Quad x = decode(representation(a));
    uint64_t magnitude = integer(x, x.negative ? UINT64_C(1) << 31 : (uint64_t)INT32_MAX, 31);
    return (int32_t)(x.negative ? 0 - magnitude : magnitude);
}

uint32_t QUAD_ABI(fixunstfsi)(QuadValue a)
{
    Quad x = decode(representation(a));
    return x.negative ? 0 : (uint32_t)integer(x, UINT32_MAX, 32);
}
