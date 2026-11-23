#ifndef MCC_INTEGER_BITS_H
#define MCC_INTEGER_BITS_H

typedef enum { BITS_SWAP, BITS_POPCOUNT, BITS_PARITY, BITS_CLZ, BITS_CTZ,
               BITS_FFS, BITS_CLRSB } IntegerBits;

static inline uint64_t integer_bits(uint64_t value, IntegerBits operation, unsigned width)
{
    uint64_t mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
    value &= mask;
    if (operation == BITS_SWAP) {
        uint64_t result = 0;
        for (unsigned i = 0; i < width; i += 8) {
            result = (result << 8) | (value & 255);
            value >>= 8;
        }
        return result;
    }
    if (operation == BITS_POPCOUNT || operation == BITS_PARITY) {
        unsigned result = 0;
        for (; value; value &= value - 1)
            result++;
        return operation == BITS_PARITY ? result & 1 : result;
    }
    if (operation == BITS_FFS || operation == BITS_CTZ) {
        if (!value)
            return operation == BITS_FFS ? 0 : width;
        unsigned count = 0;
        for (; !(value & 1); value >>= 1)
            count++;
        return count + (operation == BITS_FFS);
    }
    uint64_t high = UINT64_C(1) << (width - 1);
    if (operation == BITS_CLRSB && (value & high))
        value ^= mask;
    unsigned count = 0;
    for (; high && !(value & high); high >>= 1)
        count++;
    return count - (operation == BITS_CLRSB);
}

#endif
