#include "types.h"
#define MCC_COMPLEX_QUAD
#include "../complex_ops.h"

typedef float QuadABI __attribute__((mode(TF)));
typedef float _Complex ComplexQuadABI __attribute__((mode(TC)));
ComplexQuadABI __multc3(QuadABI, QuadABI, QuadABI, QuadABI);
ComplexQuadABI __divtc3(QuadABI, QuadABI, QuadABI, QuadABI);

void __mcc_quad_complex_binary(void *out, const void *left, const void *right, bool divide)
{
    ComplexNumber a, b;
    memcpy(&a, left, sizeof a);
    memcpy(&b, right, sizeof b);
    ComplexNumber result = divide ? complex_quotient(a, b) : complex_product(a, b);
    memcpy(out, &result, sizeof result);
}

ComplexQuadABI __multc3(QuadABI ar, QuadABI ai, QuadABI br, QuadABI bi)
{
    ComplexNumber result = complex_product((ComplexNumber){ar, ai}, (ComplexNumber){br, bi});
    return __builtin_complex((QuadABI)result.real, (QuadABI)result.imaginary);
}

ComplexQuadABI __divtc3(QuadABI ar, QuadABI ai, QuadABI br, QuadABI bi)
{
    ComplexNumber result = complex_quotient((ComplexNumber){ar, ai}, (ComplexNumber){br, bi});
    return __builtin_complex((QuadABI)result.real, (QuadABI)result.imaginary);
}
