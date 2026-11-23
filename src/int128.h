#ifndef MCC_INT128_H
#define MCC_INT128_H

typedef struct { uint64_t lo, hi; } Integer128;

typedef enum {
    WIDE_ADD, WIDE_SUB, WIDE_MUL, WIDE_DIV, WIDE_REM, WIDE_AND, WIDE_OR, WIDE_XOR,
    WIDE_SHL, WIDE_SHR, WIDE_LT, WIDE_LE, WIDE_GT, WIDE_GE, WIDE_EQ, WIDE_NE
} WideOperation;

#endif
