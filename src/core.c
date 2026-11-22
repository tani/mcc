#include "compiler.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct Arena {
    Arena *next;
    size_t used, capacity;
    max_align_t storage[];
};

void *alloc(Context *c, size_t bytes)
{
    size_t size = align_to(bytes ? bytes : 1, _Alignof(max_align_t));
    Arena *block = c->arena;
    if (!block || size > block->capacity - block->used) {
        size_t capacity = size > 32768 ? size : 32768;
        if (capacity > SIZE_MAX - sizeof *block)
            abort();
        block = calloc(1, sizeof *block + capacity);
        if (!block) {
            fputs("mcc: out of memory\n", stderr);
            exit(1);
        }
        block->capacity = capacity;
        block->next = c->arena;
        c->arena = block;
    }
    void *result = (char *)block->storage + block->used;
    block->used += size;
    return result;
}

void *grow(Context *c, const void *old, size_t *capacity, size_t width)
{
    size_t count = *capacity;
    if (count > SIZE_MAX / 2 / width)
        abort();
    *capacity = count ? count * 2 : 16;
    void *result = alloc(c, *capacity * width);
    if (old)
        memcpy(result, old, count * width);
    return result;
}

char *copy(Context *c, const char *text, size_t size)
{
    char *result = alloc(c, size + 1);
    memcpy(result, text, size);
    return result;
}

char *format(Context *c, const char *fmt, ...)
{
    va_list args, length_args;
    va_start(args, fmt);
    va_copy(length_args, args);
    int length = vsnprintf(NULL, 0, fmt, length_args);
    va_end(length_args);
    if (length < 0)
        abort();
    char *result = alloc(c, (size_t)length + 1);
    vsnprintf(result, (size_t)length + 1, fmt, args);
    va_end(args);
    return result;
}

void release(Context *c)
{
    while (c->arena) {
        Arena *block = c->arena;
        c->arena = block->next;
        free(block);
    }
}

void error(Context *c, Loc loc, const char *fmt, ...)
{
    c->failed = true;
    if (++c->errors > 20)
        return;
    if (loc.file)
        fprintf(stderr, "%s:%u:%u: error: ", loc.file, loc.line, loc.column);
    else
        fputs("mcc: error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

void extension(Context *c, Loc loc, const char *feature)
{
    if (!c->gnu || c->pedantic)
        error(c, loc, "%s requires GNU mode", feature);
}

size_t align_to(size_t number, size_t alignment)
{
    return (number + alignment - 1) / alignment * alignment;
}

bool equal(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

bool aggregate(Type *t)
{
    return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_UNION;
}
