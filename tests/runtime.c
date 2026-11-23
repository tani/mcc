#include <execinfo.h>
#include <fenv.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static int quad(void)
{
    volatile _Float128 one = 1.0F128, half_ulp = 0x1p-113F128, three = 3.0F128;
    const _Float128 next = 0x1.0000000000000000000000000001p0F128;
    if (fesetround(FE_UPWARD) || feclearexcept(FE_ALL_EXCEPT))
        return 1;
    _Float128 sum = one + half_ulp;
    if (memcmp(&sum, &next, sizeof sum) || !fetestexcept(FE_INEXACT))
        return 2;
    if (fesetround(FE_TONEAREST))
        return 3;
    _Float128 result = (three * three - one) / (one + one);
    const _Float128 four = 4.0F128;
    return memcmp(&result, &four, sizeof result) || result < three || result == three ? 4 : 0;
}

static int ready[2];
static int cleaned;

static void cleanup(void *argument)
{
    ++*(int *)argument;
}

static void *worker(void *unused)
{
    (void)unused;
    pthread_cleanup_push(cleanup, &cleaned);
    if (write(ready[1], "1", 1) == 1)
        for (;;)
            pause();
    pthread_cleanup_pop(0);
    return NULL;
}

int main(void)
{
    int status = quad();
    if (status)
        return status;
    void *frames[8];
    if (backtrace(frames, sizeof frames / sizeof frames[0]) < 2)
        return 5;
    pthread_t thread;
    void *result;
    char start;
    if (pipe(ready) || pthread_create(&thread, NULL, worker, NULL) || read(ready[0], &start, 1) != 1 ||
        pthread_cancel(thread) || pthread_join(thread, &result))
        return 6;
    return result != PTHREAD_CANCELED || cleaned != 1 ? 7 : 0;
}
