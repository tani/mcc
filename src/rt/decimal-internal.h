#ifndef MCC_DECIMAL_INTERNAL_H
#define MCC_DECIMAL_INTERNAL_H

#include "../decimal.h"

enum { DEC_NEAREST, DEC_DOWNWARD, DEC_UPWARD, DEC_TOWARD_ZERO, DEC_NEAREST_AWAY };
enum { DEC_INVALID = 1, DEC_DIVBYZERO = 4, DEC_OVERFLOW = 8, DEC_UNDERFLOW = 16, DEC_INEXACT = 32 };

typedef struct {
    int rounding;
    unsigned flags;
} DecimalStatus;

Integer128 decimal_convert_status(DecimalStatus *, unsigned, unsigned, Integer128);
Integer128 decimal_from_integer_status(DecimalStatus *, unsigned, Integer128, bool);
bool decimal_to_integer_status(DecimalStatus *, unsigned, Integer128, unsigned, bool, Integer128 *);
Integer128 decimal_from_binary_status(DecimalStatus *, unsigned, unsigned, Integer128);
Integer128 decimal_to_binary_status(DecimalStatus *, unsigned, unsigned, Integer128);
int decimal_compare_status(DecimalStatus *, unsigned, Integer128, Integer128);
bool decimal_relation_result(DecimalOperation, int);
Integer128 decimal_binary_status(DecimalStatus *, unsigned, DecimalOperation, Integer128, Integer128);

#endif
