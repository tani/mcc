#include "types.h"
#include "../decimal.h"
#include "../float_builtins.h"
#include "../int128_float.h"

__extension__ typedef _Decimal32 Decimal32;
__extension__ typedef _Decimal64 Decimal64;
__extension__ typedef _Decimal128 Decimal128;
__extension__ typedef __int128 Signed128;
__extension__ typedef unsigned __int128 Unsigned128;

#define BID_BINARY(type, mode, name, op)                                  \
    type __bid_##name##mode##3(type a, type b)                              \
    {                                                                     \
        type result;                                                       \
        __mcc_decimal_binary(&result, &a, &b, sizeof result, op);            \
        return result;                                                     \
    }

#define BID_COMPARE(type, mode, name, unordered)                           \
    int __bid_##name##mode##2(type a, type b)                                \
    {                                                                     \
        int cmp = __mcc_decimal_compare(&a, &b, sizeof a);                   \
        return cmp == DEC_UNORDERED ? (unordered) : cmp;                    \
    }

#define BID_ARITHMETIC(type, mode)                                         \
    BID_BINARY(type, mode, add, DEC_ADD)                                    \
    BID_BINARY(type, mode, sub, DEC_SUB)                                    \
    BID_BINARY(type, mode, mul, DEC_MUL)                                    \
    BID_BINARY(type, mode, div, DEC_DIV)                                    \
    BID_COMPARE(type, mode, eq, 1)                                          \
    BID_COMPARE(type, mode, ne, 1)                                          \
    BID_COMPARE(type, mode, lt, 1)                                          \
    BID_COMPARE(type, mode, le, 1)                                          \
    BID_COMPARE(type, mode, gt, -1)                                         \
    BID_COMPARE(type, mode, ge, -1)                                         \
    int __bid_unord##mode##2(type a, type b)                                 \
    {                                                                     \
        return __mcc_decimal_compare(&a, &b, sizeof a) == DEC_UNORDERED;   \
    }

BID_ARITHMETIC(Decimal32, sd)
BID_ARITHMETIC(Decimal64, dd)
BID_ARITHMETIC(Decimal128, td)

#define BID_CONVERT(to, source, name)                                      \
    to __bid_##name(source value)                                          \
    {                                                                     \
        to result;                                                         \
        __mcc_decimal_convert(&result, &value, sizeof result, sizeof value);\
        return result;                                                     \
    }

BID_CONVERT(Decimal64, Decimal32, extendsddd2)
BID_CONVERT(Decimal128, Decimal32, extendsdtd2)
BID_CONVERT(Decimal128, Decimal64, extendddtd2)
BID_CONVERT(Decimal32, Decimal64, truncddsd2)
BID_CONVERT(Decimal32, Decimal128, trunctdsd2)
BID_CONVERT(Decimal64, Decimal128, trunctddd2)

typedef float Half __attribute__((mode(HF)));
typedef float BFloat __attribute__((mode(BF)));
typedef float Quad __attribute__((mode(TF)));

#define BID_FLOAT_PAIR(binary, bm, fmt, decimal, dm, to_binary, to_decimal)             \
    binary __bid_##to_binary##dm##bm(decimal value)                                      \
    {                                                                                   \
        binary result;                                                                   \
        __mcc_decimal_to_binary(&result, &value, sizeof value, fmt);                      \
        return result;                                                                   \
    }                                                                                   \
    decimal __bid_##to_decimal##bm##dm(binary value)                                     \
    {                                                                                   \
        decimal result;                                                                  \
        __mcc_decimal_from_binary(&result, &value, sizeof result, fmt);                  \
        return result;                                                                   \
    }

#define BID_SMALL_FLOAT(type, mode, format)                                    \
    BID_FLOAT_PAIR(type, mode, format, Decimal32, sd, trunc, extend)            \
    BID_FLOAT_PAIR(type, mode, format, Decimal64, dd, trunc, extend)            \
    BID_FLOAT_PAIR(type, mode, format, Decimal128, td, trunc, extend)

#define BID_LARGE_FLOAT(type, mode, format)                                    \
    BID_FLOAT_PAIR(type, mode, format, Decimal32, sd, extend, trunc)            \
    BID_FLOAT_PAIR(type, mode, format, Decimal64, dd, extend, trunc)            \
    BID_FLOAT_PAIR(type, mode, format, Decimal128, td, trunc, extend)

BID_SMALL_FLOAT(Half, hf, FP_BINARY16)
BID_SMALL_FLOAT(BFloat, bf, FP_BFLOAT16)
BID_SMALL_FLOAT(float, sf, 4)
BID_FLOAT_PAIR(double, df, 8, Decimal32, sd, extend, trunc)
BID_FLOAT_PAIR(double, df, 8, Decimal64, dd, trunc, extend)
BID_FLOAT_PAIR(double, df, 8, Decimal128, td, trunc, extend)
BID_LARGE_FLOAT(Quad, tf, 16)
#ifdef __x86_64__
BID_LARGE_FLOAT(long double, xf, 10)
#endif

#define BID_PUBLIC_PREDICATE(type, suffix, name, operation)                \
    int name##suffix(type value)                                           \
    {                                                                     \
        return __mcc_fp_predicate(&value, NULL, FP_DECIMAL_FORMAT + sizeof value, operation); \
    }

#define BID_PUBLIC(type, suffix)                                          \
    type nan##suffix(const char *payload)                                   \
    {                                                                     \
        type value;                                                        \
        __mcc_fp_nan(&value, payload, FP_DECIMAL_FORMAT + sizeof value, FP_NAN); \
        return value;                                                      \
    }                                                                     \
    type fabs##suffix(type value)                                          \
    {                                                                     \
        type result;                                                       \
        __mcc_fp_sign(&result, &value, NULL, FP_DECIMAL_FORMAT + sizeof value); \
        return result;                                                     \
    }                                                                     \
    BID_PUBLIC_PREDICATE(type, suffix, signbit, FP_SIGNBIT)                  \
    BID_PUBLIC_PREDICATE(type, suffix, isinf, FP_ISINF)                      \
    BID_PUBLIC_PREDICATE(type, suffix, isnan, FP_ISNAN)

BID_PUBLIC(Decimal32, d32)
BID_PUBLIC(Decimal64, d64)
BID_PUBLIC(Decimal128, d128)

#define BID_INT(type, mode, integer, imode, prefix, uns)                            \
    type __bid_float##prefix##imode##mode(integer value)                             \
    {                                                                             \
        type result;                                                               \
        __mcc_decimal_from_integer(&result, &value, sizeof result, sizeof value, uns);\
        return result;                                                             \
    }                                                                             \
    integer __bid_fix##prefix##mode##imode(type value)                               \
    {                                                                             \
        integer result;                                                            \
        __mcc_decimal_to_integer(&result, &value, sizeof value, sizeof result, uns); \
        return result;                                                             \
    }

#define BID_INTEGERS(type, mode)                            \
    BID_INT(type, mode, int, si, , false)                    \
    BID_INT(type, mode, unsigned, si, uns, true)             \
    BID_INT(type, mode, long, di, , false)                   \
    BID_INT(type, mode, unsigned long, di, uns, true)        \
    BID_INT(type, mode, Signed128, ti, , false)              \
    BID_INT(type, mode, Unsigned128, ti, uns, true)

BID_INTEGERS(Decimal32, sd)
BID_INTEGERS(Decimal64, dd)
BID_INTEGERS(Decimal128, td)
