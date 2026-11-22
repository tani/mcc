#ifndef MCC_STDDEF_H
#define MCC_STDDEF_H

#define __NEED_ptrdiff_t
#define __NEED_size_t
#define __NEED_wchar_t
#define __NEED_max_align_t
#include <bits/alltypes.h>

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
