#ifndef MCC_RT_FP_ROUND_H
#define MCC_RT_FP_ROUND_H

#include "types.h"
#include "../int128_float.h"

uint64_t mcc_get_fpcr(void);
void mcc_raise_fp(unsigned);

static inline unsigned fp_rounding_mode(void)
{
#if defined(__aarch64__)
    return (unsigned)(mcc_get_fpcr() >> 22) & 3;
#else
    unsigned mode = (unsigned)(mcc_get_fpcr() >> 13) & 3;
    return mode == 1 ? FP_DOWNWARD : mode == 2 ? FP_UPWARD : mode;
#endif
}

#endif
