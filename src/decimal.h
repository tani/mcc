#ifndef MCC_DECIMAL_H
#define MCC_DECIMAL_H

#include <stdbool.h>
#include <stdint.h>
#include "int128.h"

typedef enum {
    DEC_ADD, DEC_SUB, DEC_MUL, DEC_DIV,
    DEC_LT, DEC_LE, DEC_GT, DEC_GE, DEC_EQ, DEC_NE
} DecimalOperation;
enum { DEC_UNORDERED = 2 };

Integer128 decimal_literal(unsigned, const char *, const char *);
Integer128 decimal_negate(unsigned, Integer128);
Integer128 decimal_convert(unsigned, unsigned, Integer128);
Integer128 decimal_from_integer(unsigned, Integer128, bool);
bool decimal_to_integer(unsigned, Integer128, unsigned, bool, Integer128 *);
Integer128 decimal_from_binary(unsigned, unsigned, Integer128);
Integer128 decimal_to_binary(unsigned, unsigned, Integer128);
bool decimal_nonzero(unsigned, Integer128);
Integer128 decimal_binary(unsigned, DecimalOperation, Integer128, Integer128);
int decimal_compare(unsigned, Integer128, Integer128);
bool decimal_relation(unsigned, DecimalOperation, Integer128, Integer128);

void __mcc_decimal_binary(void *, const void *, const void *, unsigned, unsigned);
int __mcc_decimal_compare(const void *, const void *, unsigned);
int __mcc_decimal_predicate(const void *, const void *, unsigned, unsigned);
int __mcc_decimal_relation(const void *, const void *, unsigned, unsigned);
void __mcc_decimal_convert(void *, const void *, unsigned, unsigned);
void __mcc_decimal_from_integer(void *, const void *, unsigned, unsigned, bool);
void __mcc_decimal_to_integer(void *, const void *, unsigned, unsigned, bool);
void __mcc_decimal_from_binary(void *, const void *, unsigned, unsigned);
void __mcc_decimal_to_binary(void *, const void *, unsigned, unsigned);

void __dfp_set_round(int);
int __dfp_get_round(void);
void __dfp_clear_except(void);
int __dfp_test_except(int);
void __dfp_raise_except(int);
int fe_dec_getround(void);
int fe_dec_setround(int);

#endif
