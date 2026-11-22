#include "types.h"

int __popcountdi2(uint64_t);

int __popcountdi2(uint64_t value)
{
    int count = 0;
    for (; value; value &= value - 1)
        count++;
    return count;
}
