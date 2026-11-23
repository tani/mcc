#include "types.h"
#include "../atomic.h"

uint64_t __mcc_atomic_load_word(const volatile void *, unsigned);
void __mcc_atomic_store_word(volatile void *, uint64_t, unsigned);
uint64_t __mcc_atomic_cas_word(volatile void *, uint64_t, uint64_t, unsigned);
void __mcc_atomic_fence(void);

void __mcc_atomic_access(size_t, volatile void *, const void *, void *, unsigned);
bool __mcc_atomic_compare(size_t, volatile void *, void *, const void *);
bool __mcc_atomic_lock_free(size_t, const volatile void *);
uint64_t __mcc_atomic_rmw(volatile void *, uint64_t, unsigned, unsigned, bool);

#if defined(__aarch64__)
uint8_t __aarch64_cas1_sync(uint8_t, uint8_t, volatile void *);
uint16_t __aarch64_cas2_sync(uint16_t, uint16_t, volatile void *);
uint32_t __aarch64_cas4_sync(uint32_t, uint32_t, volatile void *);
uint64_t __aarch64_cas8_sync(uint64_t, uint64_t, volatile void *);

uint64_t __mcc_atomic_cas_word(volatile void *address, uint64_t expected, uint64_t desired,
                              unsigned width)
{
    switch (width) {
    case 1: return __aarch64_cas1_sync((uint8_t)expected, (uint8_t)desired, address);
    case 2: return __aarch64_cas2_sync((uint16_t)expected, (uint16_t)desired, address);
    case 4: return __aarch64_cas4_sync((uint32_t)expected, (uint32_t)desired, address);
    default: return __aarch64_cas8_sync(expected, desired, address);
    }
}
#endif

static uint64_t object_lock;

static void acquire(void)
{
    while (__mcc_atomic_cas_word(&object_lock, 0, 1, 8))
        while (__mcc_atomic_load_word(&object_lock, 8))
            ;
}

static void release(void)
{
    __mcc_atomic_store_word(&object_lock, 0, 8);
}

bool __mcc_atomic_lock_free(size_t width, const volatile void *address)
{
    return (width == 1 || width == 2 || width == 4 || width == 8) &&
           !((uintptr_t)address & (width - 1));
}

static uint64_t take_bytes(const volatile void *address, unsigned width)
{
    const volatile unsigned char *bytes = address;
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void put_bytes(volatile void *address, uint64_t value, unsigned width)
{
    volatile unsigned char *bytes = address;
    for (unsigned i = 0; i < width; i++)
        bytes[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t rmw_value(uint64_t before, uint64_t operand, unsigned operation)
{
    switch (operation) {
    case AT_ADD: return before + operand;
    case AT_SUB: return before - operand;
    case AT_AND: return before & operand;
    case AT_OR: return before | operand;
    case AT_XOR: return before ^ operand;
    case AT_NAND: return ~(before & operand);
    default: return operand;
    }
}

uint64_t __mcc_atomic_rmw(volatile void *address, uint64_t operand, unsigned width,
                          unsigned operation, bool return_new)
{
    uint64_t before, next;
    if (__mcc_atomic_lock_free(width, address)) {
        uint64_t observed = __mcc_atomic_load_word(address, width);
        do {
            before = observed;
            next = rmw_value(before, operand, operation);
            observed = __mcc_atomic_cas_word(address, before, next, width);
        } while (before != observed);
    } else {
        acquire();
        before = take_bytes(address, width);
        next = rmw_value(before, operand, operation);
        put_bytes(address, next, width);
        release();
    }
    return return_new ? next : before;
}

void __mcc_atomic_access(size_t size, volatile void *address, const void *input, void *output,
                         unsigned operation)
{
    if (__mcc_atomic_lock_free(size, address)) {
        unsigned width = (unsigned)size;
        if (operation == AT_LOAD)
            put_bytes(output, __mcc_atomic_load_word(address, width), width);
        else if (operation == AT_STORE)
            __mcc_atomic_store_word(address, take_bytes(input, width), width);
        else
            put_bytes(output, __mcc_atomic_rmw(address, take_bytes(input, width), width,
                                                AT_EXCHANGE, false), width);
        return;
    }
    volatile unsigned char *object = address;
    const unsigned char *in = input;
    unsigned char *out = output;
    acquire();
    if (operation == AT_EXCHANGE) {
        for (size_t i = 0; i < size; i++) {
            unsigned char value = in[i];
            out[i] = object[i];
            object[i] = value;
        }
    } else if (operation == AT_LOAD) {
        for (size_t i = 0; i < size; i++)
            out[i] = object[i];
    } else {
        for (size_t i = 0; i < size; i++)
            object[i] = in[i];
    }
    release();
}

bool __mcc_atomic_compare(size_t size, volatile void *address, void *expected, const void *desired)
{
    if (__mcc_atomic_lock_free(size, address)) {
        unsigned width = (unsigned)size;
        uint64_t before = take_bytes(expected, width);
        uint64_t observed = __mcc_atomic_cas_word(address, before, take_bytes(desired, width), width);
        if (before == observed)
            return true;
        put_bytes(expected, observed, width);
        return false;
    }
    volatile unsigned char *object = address;
    unsigned char *prior = expected;
    const unsigned char *next = desired;
    bool match = true;
    acquire();
    for (size_t i = 0; i < size; i++) {
        if (object[i] != prior[i]) {
            match = false;
            break;
        }
    }
    if (match) {
        for (size_t i = 0; i < size; i++)
            object[i] = next[i];
    } else {
        for (size_t i = 0; i < size; i++)
            prior[i] = object[i];
    }
    release();
    return match;
}
