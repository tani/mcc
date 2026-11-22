#if !defined(__need_ptrdiff_t) && !defined(__need_size_t) && \
    !defined(__need_wchar_t) && !defined(__need_wint_t) && !defined(__need_NULL)
#ifndef MCC_STDDEF_H
#define MCC_STDDEF_H

#define __need_ptrdiff_t
#define __need_size_t
#define __need_wchar_t
#define __need_NULL
typedef struct {
    long long __mcc_ll;
    long double __mcc_ld;
} max_align_t;
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#endif

#ifdef __need_ptrdiff_t
typedef long ptrdiff_t;
#undef __need_ptrdiff_t
#endif
#ifdef __need_size_t
typedef unsigned long size_t;
#undef __need_size_t
#endif
#ifdef __need_wchar_t
typedef unsigned int wchar_t;
#undef __need_wchar_t
#endif
#ifdef __need_wint_t
typedef unsigned int wint_t;
#undef __need_wint_t
#endif
#ifdef __need_NULL
#ifndef NULL
#define NULL ((void *)0)
#endif
#undef __need_NULL
#endif
