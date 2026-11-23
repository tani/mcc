#include "types.h"
#include "fp-round.h"
#include "../float16.h"

#include <float.h>

typedef float QuadABI __attribute__((mode(TF)));
unsigned short __mcc_float16_from_long_double(long double, unsigned);
unsigned short __mcc_float16_from_quad(QuadABI, unsigned);
float __mcc_float16_to_float(unsigned short, unsigned);
void __mcc_int128_to_float16(void *, const Integer128 *, unsigned, bool);

static unsigned short float16_from_wide(Integer128 bits, bool x87, unsigned format)
{
    unsigned flags = 0;
    unsigned short result = float16_encode(bits, x87, format, fp_rounding_mode(), &flags);
    if (flags)
        mcc_raise_fp(flags);
    return result;
}

unsigned short __mcc_float16_from_long_double(long double value, unsigned format)
{
    Integer128 bits;
    memcpy(&bits, &value, sizeof bits);
    return float16_from_wide(bits, LDBL_MANT_DIG == 64, format);
}

unsigned short __mcc_float16_from_quad(QuadABI value, unsigned format)
{
    Integer128 bits;
    memcpy(&bits, &value, sizeof bits);
    return float16_from_wide(bits, false, format);
}

void __mcc_int128_to_float16(void *out, const Integer128 *value, unsigned format, bool uns)
{
    unsigned flags = 0;
    Integer128 bits = round_wide_float_bits(*value, format, uns, fp_rounding_mode(), &flags);
    memcpy(out, &bits, sizeof(uint16_t));
    if (flags)
        mcc_raise_fp(flags);
}

float __mcc_float16_to_float(unsigned short raw, unsigned format)
{
    uint32_t bits = float16_expand(raw, format);
    if ((bits & UINT32_C(0x7fc00000)) == UINT32_C(0x7f800000) && (bits & UINT32_C(0x003fffff))) {
        bits |= UINT32_C(0x00400000);
        mcc_raise_fp(FP_INVALID);
    }
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

typedef float Half __attribute__((mode(HF)));
typedef float BFloat __attribute__((mode(BF)));
typedef int WideSigned __attribute__((mode(TI)));
typedef unsigned int WideUnsigned __attribute__((mode(TI)));

BFloat __truncsfbf2(float);
BFloat __truncdfbf2(double);
BFloat __trunchfbf2(Half);
float __extendbfsf2(BFloat);
BFloat __floatdibf(int64_t);
BFloat __floatundibf(uint64_t);
BFloat __floattibf(WideSigned);
BFloat __floatuntibf(WideUnsigned);

BFloat __truncsfbf2(float value) { return (BFloat)value; }
BFloat __truncdfbf2(double value) { return (BFloat)value; }
BFloat __trunchfbf2(Half value) { return (BFloat)value; }
float __extendbfsf2(BFloat value) { return (float)value; }
BFloat __floatdibf(int64_t value) { return (BFloat)value; }
BFloat __floatundibf(uint64_t value) { return (BFloat)value; }
BFloat __floattibf(WideSigned value) { return (BFloat)value; }
BFloat __floatuntibf(WideUnsigned value) { return (BFloat)value; }

float __extendhfsf2(Half);
double __extendhfdf2(Half);
Half __truncsfhf2(float);
Half __truncdfhf2(double);
int __eqhf2(Half, Half);
int __nehf2(Half, Half);
int __lehf2(Half, Half);
int __lthf2(Half, Half);
int __gehf2(Half, Half);
int __gthf2(Half, Half);
int __unordhf2(Half, Half);

float __extendhfsf2(Half value) { return (float)value; }
double __extendhfdf2(Half value) { return (double)value; }
Half __truncsfhf2(float value) { return (Half)value; }
Half __truncdfhf2(double value) { return (Half)value; }

QuadABI __extendhftf2(Half);
Half __trunctfhf2(QuadABI);
BFloat __trunctfbf2(QuadABI);
QuadABI __extendhftf2(Half value) { return (QuadABI)value; }
Half __trunctfhf2(QuadABI value) { return (Half)value; }
BFloat __trunctfbf2(QuadABI value) { return (BFloat)value; }

static int half_order(Half a, Half b, int unordered)
{
    float x = (float)a, y = (float)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return x == y ? 0 : unordered;
}

int __eqhf2(Half a, Half b) { return half_order(a, b, 1); }
int __nehf2(Half a, Half b) { return half_order(a, b, 1); }
int __lehf2(Half a, Half b) { return half_order(a, b, 1); }
int __lthf2(Half a, Half b) { return half_order(a, b, 1); }
int __gehf2(Half a, Half b) { return half_order(a, b, -1); }
int __gthf2(Half a, Half b) { return half_order(a, b, -1); }
int __unordhf2(Half a, Half b)
{
    float x = (float)a, y = (float)b;
    return x != x || y != y;
}

#if defined(__x86_64__)
long double __extendhfxf2(Half);
Half __truncxfhf2(long double);
BFloat __truncxfbf2(long double);
long double __extendhfxf2(Half value) { return (long double)value; }
Half __truncxfhf2(long double value) { return (Half)value; }
BFloat __truncxfbf2(long double value) { return (BFloat)value; }
#endif
