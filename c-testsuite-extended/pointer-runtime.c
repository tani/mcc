#include <stddef.h>

static int calls;
static int values[] = {11, 22, 33};

static int *advance(void)
{
    calls++;
    return values + 1;
}

static const volatile int *fallback(void)
{
    calls++;
    return values + 2;
}

static int answer(void)
{
    return 42;
}

int main(void)
{
    int *plain = values;
    const int *read_only = plain;
    volatile int *last = values + 2;
    const volatile int *selected = 1 ? advance() : fallback();
    if (*selected != 22 || calls != 1 || read_only != plain || plain != read_only)
        return 1;
    selected = 0 ? advance() : fallback();
    if (selected != last || calls != 2 || !(read_only < last) || last - read_only != 2)
        return 2;
    if ((1 ? NULL : read_only) != NULL || (0 ? NULL : read_only) != plain)
        return 3;
    int (*function)(void) = 1 ? answer : (void *)0;
    return function() != 42 || (void *)0 == function;
}
