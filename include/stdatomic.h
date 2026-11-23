#ifndef MCC_STDATOMIC_H
#define MCC_STDATOMIC_H

#include <stddef.h>
#include <stdint.h>

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(uint_least16_t) atomic_char16_t;
typedef _Atomic(uint_least32_t) atomic_char32_t;
typedef _Atomic(wchar_t) atomic_wchar_t;
typedef _Atomic(int_least8_t) atomic_int_least8_t;
typedef _Atomic(uint_least8_t) atomic_uint_least8_t;
typedef _Atomic(int_least16_t) atomic_int_least16_t;
typedef _Atomic(uint_least16_t) atomic_uint_least16_t;
typedef _Atomic(int_least32_t) atomic_int_least32_t;
typedef _Atomic(uint_least32_t) atomic_uint_least32_t;
typedef _Atomic(int_least64_t) atomic_int_least64_t;
typedef _Atomic(uint_least64_t) atomic_uint_least64_t;
typedef _Atomic(int_fast8_t) atomic_int_fast8_t;
typedef _Atomic(uint_fast8_t) atomic_uint_fast8_t;
typedef _Atomic(int_fast16_t) atomic_int_fast16_t;
typedef _Atomic(uint_fast16_t) atomic_uint_fast16_t;
typedef _Atomic(int_fast32_t) atomic_int_fast32_t;
typedef _Atomic(uint_fast32_t) atomic_uint_fast32_t;
typedef _Atomic(int_fast64_t) atomic_int_fast64_t;
typedef _Atomic(uint_fast64_t) atomic_uint_fast64_t;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(size_t) atomic_size_t;
typedef _Atomic(ptrdiff_t) atomic_ptrdiff_t;
typedef _Atomic(intmax_t) atomic_intmax_t;
typedef _Atomic(uintmax_t) atomic_uintmax_t;

typedef struct { _Atomic(_Bool) __value; } atomic_flag;

#define ATOMIC_VAR_INIT(value) (value)
#define ATOMIC_FLAG_INIT { 0 }
#define atomic_init(object, value) atomic_store_explicit((object), (value), memory_order_relaxed)
#define kill_dependency(value) (value)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)
#define atomic_is_lock_free(object) __mcc_atomic_object_lock_free(object)

#define atomic_load_explicit(object, order) __mcc_atomic_value_load((object), (order))
#define atomic_store_explicit(object, value, order) __mcc_atomic_value_store((object), (value), (order))
#define atomic_exchange_explicit(object, value, order) __mcc_atomic_value_exchange((object), (value), (order))

#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_store(object, value) atomic_store_explicit((object), (value), memory_order_seq_cst)
#define atomic_exchange(object, value) atomic_exchange_explicit((object), (value), memory_order_seq_cst)

#define atomic_compare_exchange_strong_explicit(object, expected, desired, success, failure) \
    __mcc_atomic_value_compare((object), (expected), (desired), 0, (success), (failure))
#define atomic_compare_exchange_weak_explicit(object, expected, desired, success, failure) \
    __mcc_atomic_value_compare((object), (expected), (desired), 1, (success), (failure))
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)

#define atomic_fetch_add_explicit(object, value, order) __mcc_atomic_value_add((object), (value), (order))
#define atomic_fetch_sub_explicit(object, value, order) __mcc_atomic_value_sub((object), (value), (order))
#define atomic_fetch_and_explicit(object, value, order) __mcc_atomic_value_and((object), (value), (order))
#define atomic_fetch_or_explicit(object, value, order) __mcc_atomic_value_or((object), (value), (order))
#define atomic_fetch_xor_explicit(object, value, order) __mcc_atomic_value_xor((object), (value), (order))

#define atomic_fetch_add(object, value) atomic_fetch_add_explicit((object), (value), memory_order_seq_cst)
#define atomic_fetch_sub(object, value) atomic_fetch_sub_explicit((object), (value), memory_order_seq_cst)
#define atomic_fetch_and(object, value) atomic_fetch_and_explicit((object), (value), memory_order_seq_cst)
#define atomic_fetch_or(object, value) atomic_fetch_or_explicit((object), (value), memory_order_seq_cst)
#define atomic_fetch_xor(object, value) atomic_fetch_xor_explicit((object), (value), memory_order_seq_cst)

#define atomic_flag_test_and_set_explicit(object, order) __atomic_test_and_set(&(object)->__value, (order))
#define atomic_flag_clear_explicit(object, order) __atomic_clear(&(object)->__value, (order))
#define atomic_flag_test_and_set(object) atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear(object) atomic_flag_clear_explicit((object), memory_order_seq_cst)

#endif
