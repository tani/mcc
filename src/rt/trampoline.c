#include "types.h"

void *mmap(void *, size_t, int, int, int, long);
int mprotect(void *, size_t, int);
int munmap(void *, size_t);
_Noreturn void abort(void);

void *__mcc_nested_pointer(void **, uint64_t, uint64_t);
void __mcc_release_nested_pointers(void *);
void *__mcc_lock_trampoline_list(void **);
void __mcc_unlock_trampoline_list(void **, void *);
#if defined(__aarch64__)
void __clear_cache(void *, void *);
#endif

enum {
    PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4,
    MAP_PRIVATE = 2, MAP_ANONYMOUS = 32
};

typedef struct Trampoline Trampoline;
struct Trampoline {
    Trampoline *next;
    uint64_t target;
    unsigned char code[32];
};

static void write_code(Trampoline *entry, uint64_t chain)
{
#if defined(__aarch64__)
    static const uint32_t instructions[] = {
        0xd503245fU, /* bti c */
        0x58000072U, /* ldr x18, chain */
        0x58000090U, /* ldr x16, target */
        0xd61f0200U  /* br x16 */
    };
    memcpy(entry->code, instructions, sizeof instructions);
    memcpy(entry->code + 16, &chain, sizeof chain);
    memcpy(entry->code + 24, &entry->target, sizeof entry->target);
#else
    static const unsigned char load_chain[] = {0xf3, 0x0f, 0x1e, 0xfa, 0x49, 0xba};
    static const unsigned char jump_target[] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(entry->code, load_chain, sizeof load_chain); /* endbr64; movabs into %r10 */
    memcpy(entry->code + 6, &chain, sizeof chain);
    memcpy(entry->code + 14, jump_target, sizeof jump_target); /* jmp *0(%rip) */
    memcpy(entry->code + 20, &entry->target, sizeof entry->target);
#endif
}

void *__mcc_nested_pointer(void **head, uint64_t target, uint64_t chain)
{
    Trampoline *first = __mcc_lock_trampoline_list(head);
    for (Trampoline *entry = first; entry; entry = entry->next)
        if (entry->target == target) {
            __mcc_unlock_trampoline_list(head, first);
            return entry->code;
        }
    Trampoline *entry = mmap(NULL, sizeof *entry, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (entry == (void *)(uintptr_t)UINT64_MAX)
        abort();
    entry->next = first;
    entry->target = target;
    write_code(entry, chain);
    if (mprotect(entry, sizeof *entry, PROT_READ | PROT_EXEC))
        abort();
#if defined(__aarch64__)
    __clear_cache(entry->code, entry->code + sizeof entry->code);
#endif
    __mcc_unlock_trampoline_list(head, entry);
    return entry->code;
}

void __mcc_release_nested_pointers(void *head)
{
    while (head) {
        Trampoline *entry = head;
        head = entry->next;
        munmap(entry, sizeof *entry);
    }
}
