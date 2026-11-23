#ifndef __ATOMIC_HLE_ACQUIRE
#error missing HLE macros
#endif
#include <pthread.h>
static unsigned lockword;
static unsigned long counter;
static void *run(void *arg) {
    (void)arg;
    for (unsigned i=0;i<3000;i++) {
        while (__atomic_exchange_n(&lockword, 1, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE)) { }
        counter++;
        __atomic_store_n(&lockword, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    }
    return 0;
}
int main(void) {
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) if (pthread_create(&threads[i], 0, run, 0)) return 1;
    for (unsigned i=0;i<4;i++) if (pthread_join(threads[i], 0)) return 2;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&lockword, &expected, 1, 0,
        __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE, __ATOMIC_RELAXED)) return 3;
    __atomic_store_n(&lockword, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    if (__atomic_fetch_add(&lockword, 2, __ATOMIC_ACQUIRE|__ATOMIC_HLE_ACQUIRE) != 0) return 4;
    __atomic_store_n(&lockword, 0, __ATOMIC_RELEASE|__ATOMIC_HLE_RELEASE);
    return counter != 12000 || lockword;
}
