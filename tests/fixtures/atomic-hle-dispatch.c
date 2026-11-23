#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
static unsigned acquire_cas, release_cas, acquire_store, release_store, plain_cas;
static uint64_t read_bytes(const volatile void *p, unsigned size) {
    const volatile unsigned char *b = p; uint64_t v = 0;
    for (unsigned i=0;i<size;i++) v |= (uint64_t)b[i] << (i*8);
    return v;
}
static void write_bytes(volatile void *p, uint64_t v, unsigned size) {
    volatile unsigned char *b = p;
    for (unsigned i=0;i<size;i++) b[i] = (unsigned char)(v >> (i*8));
}
uint64_t __mcc_atomic_load_word(const volatile void *p, unsigned size) { return read_bytes(p,size); }
void __mcc_atomic_store_word(volatile void *p, uint64_t v, unsigned size) { write_bytes(p,v,size); }
static uint64_t compare(volatile void *p, uint64_t expected, uint64_t desired, unsigned size) {
    uint64_t old=read_bytes(p,size);
    if (old==expected) write_bytes(p,desired,size);
    return old;
}
uint64_t __mcc_atomic_cas_word(volatile void *p,uint64_t a,uint64_t b,unsigned size) {
    ++plain_cas; return compare(p,a,b,size);
}
uint64_t __mcc_atomic_hle_acquire_cas(volatile void *p,uint64_t a,uint64_t b,unsigned size) {
    ++acquire_cas; return compare(p,a,b,size);
}
uint64_t __mcc_atomic_hle_release_cas(volatile void *p,uint64_t a,uint64_t b,unsigned size) {
    ++release_cas; return compare(p,a,b,size);
}
void __mcc_atomic_hle_acquire_store(volatile void *p,uint64_t v,unsigned size) {
    ++acquire_store; write_bytes(p,v,size);
}
void __mcc_atomic_hle_release_store(volatile void *p,uint64_t v,unsigned size) {
    ++release_store; write_bytes(p,v,size);
}
void __mcc_atomic_fence(void) { }
int main(void) {
    unsigned v=0, expected=0;
    if (__atomic_exchange_n(&v,1,__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE)!=0 || acquire_cas!=1) return 1;
    __atomic_store_n(&v,0,__ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    if (release_store!=1 || v) return 2;
    if (__atomic_fetch_add(&v,1,__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE)!=0 || acquire_cas!=2) return 3;
    if (__atomic_exchange_n(&v,2,__ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE)!=1 || release_cas!=1) return 4;
    expected=2;
    if (!__atomic_compare_exchange_n(&v,&expected,3,0,__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE,__ATOMIC_RELAXED) || acquire_cas!=3) return 5;
    __atomic_store_n(&v,0,__ATOMIC_SEQ_CST|__ATOMIC_HLE_ACQUIRE);
    if (acquire_store!=1 || v) return 6;
    if (__atomic_load_n(&v,__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) || acquire_cas!=3) return 7;
    int order=__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE;
    if (__atomic_exchange_n(&v,1,order) || acquire_cas!=3 || plain_cas!=1) return 8;
    if (__sync_fetch_and_add(&v,65536)!=1 || acquire_cas!=3 || plain_cas!=2) return 9;
    unsigned char byte = 0;
    unsigned short half = 0;
    unsigned long wide = 0;
    if (__atomic_exchange_n(&byte, 1, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) ||
        __atomic_exchange_n(&half, 1, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) ||
        __atomic_exchange_n(&wide, 1, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) || acquire_cas != 6)
        return 10;
    __atomic_store_n(&byte, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    __atomic_store_n(&half, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    __atomic_store_n(&wide, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    if (release_store != 4 || byte || half || wide) return 11;
    unsigned desired = 7, old = 0;
    v = 0;
    __atomic_exchange(&v, &desired, &old, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE);
    if (old || v != 7 || acquire_cas != 7) return 12;
    expected = 7;
    if (!__atomic_compare_exchange(&v, &expected, &desired, 0,
                                   __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE, __ATOMIC_RELAXED) ||
        release_cas != 2) return 13;
    _Alignas(8) unsigned char unaligned[8] = {0};
    unsigned prior_acquire = acquire_cas;
    if (__atomic_exchange_n((unsigned *)(unaligned + 1), 9,
                            __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) ||
        acquire_cas != prior_acquire || unaligned[1] != 9) return 14;
    _Atomic unsigned c11 = 0;
    if (atomic_exchange_explicit(&c11, 1, (memory_order)(__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE)) ||
        acquire_cas != 8) return 15;
    atomic_store_explicit(&c11, 0, (memory_order)(__ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE));
    if (release_store != 5 || c11) return 16;
    atomic_flag flag = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set_explicit(&flag, (memory_order)(__ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE)) ||
        acquire_cas != 9) return 17;
    atomic_flag_clear_explicit(&flag, (memory_order)(__ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE));
    if (release_store != 6) return 18;
    v = 0xf0;
    if (__atomic_fetch_nand(&v, 15, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) != 0xf0 ||
        v != ~0u || acquire_cas != 10) return 19;
    v = 1;
    if (__atomic_add_fetch(&v, 5, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE) != 6 ||
        release_cas != 3) return 20;
    expected = 123;
    if (__atomic_compare_exchange_n(&v, &expected, 3, 0,
                                    __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE, __ATOMIC_RELAXED) ||
        expected != 6 || acquire_cas != 11) return 21;
    return 0;
}
