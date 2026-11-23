#ifndef MCC_FENV_H
#define MCC_FENV_H

#pragma GCC system_header
#include_next <fenv.h>

#ifdef __STDC_WANT_IEC_60559_DFP_EXT__
#define FE_DEC_TONEAREST 0
#define FE_DEC_DOWNWARD 1
#define FE_DEC_UPWARD 2
#define FE_DEC_TOWARDZERO 3
#define FE_DEC_TONEARESTFROMZERO 4

int fe_dec_getround(void);
int fe_dec_setround(int);
#endif

#endif
