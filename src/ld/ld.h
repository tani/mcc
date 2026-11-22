#ifndef MCC_LD_H
#define MCC_LD_H

#include "elf_tools.h"

typedef struct {
    ElfOptions elf;
    Strings libraries, runpaths;
    const char *entry, *root, *interpreter, *soname;
    bool shared, pie, export_all;
} LinkOptions;

typedef struct Linker Linker;
typedef enum { PATCH_OK, PATCH_UNSUPPORTED, PATCH_RANGE, PATCH_TRUNCATED } PatchResult;

bool mld_link(Context *, Strings args);
bool link_elf(Context *, LinkOptions *);
bool ld_inputs(Context *, Linker *, LinkOptions *);
void ld_take_object(Linker *, const char *, Bytes);
void ld_take_shared(Linker *, const char *path, const char *link_name, Bytes, bool as_needed);
bool ld_needs(Linker *, const char *);
PatchResult ld_patch(uint16_t machine, unsigned char *out, size_t len, uint32_t type,
                     uint64_t value, uint64_t place);

#endif
