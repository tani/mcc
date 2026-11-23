#ifndef MCC_FLOAT_BUILTINS_H
#define MCC_FLOAT_BUILTINS_H

#include <stdbool.h>
#include <stdint.h>
#include "int128.h"

enum { FP_DECIMAL_FORMAT = 32 };
enum {
    FP_CLASS_NAN, FP_CLASS_INFINITY, FP_CLASS_NORMAL, FP_CLASS_SUBNORMAL, FP_CLASS_ZERO,
    FP_CLASS_MASK = 7, FP_NEGATIVE = 8, FP_SIGNALING = 16
};

typedef enum {
    FP_INFINITY, FP_NAN, FP_SNAN, FP_ABS, FP_COPYSIGN,
    FP_SIGNBIT, FP_ISNAN, FP_ISSIGNALING, FP_ISINF, FP_ISINF_SIGN, FP_ISFINITE, FP_ISNORMAL, FP_CLASSIFY,
    FP_ISUNORDERED, FP_ISGREATER, FP_ISGREATEREQUAL, FP_ISLESS, FP_ISLESSEQUAL, FP_ISLESSGREATER
} FloatingBuiltin;

unsigned decimal_properties(unsigned, Integer128);
Integer128 decimal_special(unsigned, FloatingBuiltin);
Integer128 floating_special(unsigned, FloatingBuiltin, const char *);
Integer128 floating_sign(unsigned, Integer128, Integer128);
int floating_ordered_predicate(FloatingBuiltin, int);
int floating_predicate(unsigned, FloatingBuiltin, Integer128, Integer128);

void __mcc_fp_nan(void *, const char *, unsigned, unsigned);
void __mcc_fp_sign(void *, const void *, const void *, unsigned);
int __mcc_fp_predicate(const void *, const void *, unsigned, unsigned);
int __mcc_fp_classify(const void *, unsigned, int, int, int, int, int);

#endif
