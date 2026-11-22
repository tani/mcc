#ifndef MCC_RT_TYPES_H
#define MCC_RT_TYPES_H

#include <stdbool.h>
#include <stddef.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef signed int int32_t;
typedef signed long int64_t;
typedef unsigned long uintptr_t;

#define UINT64_C(n) n ## UL
#define UINT64_MAX UINT64_C(0xffffffffffffffff)
#define UINT32_MAX 0xffffffffU
#define INT64_MAX 0x7fffffffffffffffL
#define INT32_MAX 0x7fffffff

void *memcpy(void *restrict, const void *restrict, size_t);

#endif
