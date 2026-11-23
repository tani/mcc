#include "../complex_ops.h"
#include "types.h"

void __mcc_complex_binary(void *, const void *, const void *, unsigned, bool);

static ComplexNumber load_complex(const void *value, unsigned width)
{
    if (width == sizeof(float)) {
        float pair[2];
        memcpy(pair, value, sizeof pair);
        return (ComplexNumber){pair[0], pair[1]};
    }
    if (width == sizeof(double)) {
        double pair[2];
        memcpy(pair, value, sizeof pair);
        return (ComplexNumber){pair[0], pair[1]};
    }
    long double pair[2];
    memcpy(pair, value, sizeof pair);
    return (ComplexNumber){pair[0], pair[1]};
}

void __mcc_complex_binary(void *output, const void *left, const void *right,
                          unsigned width, bool divide)
{
    ComplexNumber a = load_complex(left, width), b = load_complex(right, width);
    ComplexNumber result = divide ? complex_quotient(a, b) : complex_product(a, b);
    if (width == sizeof(float)) {
        float pair[2] = {(float)result.real, (float)result.imaginary};
        memcpy(output, pair, sizeof pair);
    } else if (width == sizeof(double)) {
        double pair[2] = {(double)result.real, (double)result.imaginary};
        memcpy(output, pair, sizeof pair);
    } else {
        long double pair[2] = {result.real, result.imaginary};
        memcpy(output, pair, sizeof pair);
    }
}

#define COMPLEX_ABI(mode, real_type)                                                      \
    real_type _Complex __mul##mode##3(real_type ar, real_type ai, real_type br, real_type bi) \
    {                                                                                   \
        ComplexNumber result = complex_product((ComplexNumber){ar, ai}, (ComplexNumber){br, bi}); \
        return __builtin_complex((real_type)result.real, (real_type)result.imaginary);    \
    }                                                                                   \
    real_type _Complex __div##mode##3(real_type ar, real_type ai, real_type br, real_type bi) \
    {                                                                                   \
        ComplexNumber result = complex_quotient((ComplexNumber){ar, ai}, (ComplexNumber){br, bi}); \
        return __builtin_complex((real_type)result.real, (real_type)result.imaginary);    \
    }

COMPLEX_ABI(sc, float)
COMPLEX_ABI(dc, double)
#ifdef __x86_64__
COMPLEX_ABI(xc, long double)
#endif
