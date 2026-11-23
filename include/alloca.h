#ifndef MCC_ALLOCA_H
#define MCC_ALLOCA_H

#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#endif
