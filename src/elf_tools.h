#ifndef MCC_ELF_TOOLS_H
#define MCC_ELF_TOOLS_H

#include "compiler.h"

#include <elf.h>

typedef VECTOR(unsigned char) Bytes;

typedef enum { SECTION_NONE, SECTION_TEXT, SECTION_RODATA, SECTION_DATA,
               SECTION_BSS, SECTION_COUNT } SectionId;

typedef struct {
    const char *name;
    uint32_t type, flags;
} SectionKind;

extern const SectionKind elf_sections[SECTION_COUNT];

typedef struct {
    Strings inputs;
    const char *output;
    uint16_t machine;
} ElfOptions;

uint16_t elf_machine(const char *target);
Elf64_Ehdr elf_header(uint16_t machine, uint16_t type);
void bytes_add(Context *, Bytes *, const void *, size_t);
void bytes_int(Context *, Bytes *, uint64_t value, unsigned width);
void bytes_align(Context *, Bytes *, size_t alignment);
uint32_t bytes_string(Context *, Bytes *, const char *);
bool elf_write(Context *, ElfOptions *, const Bytes *, bool executable);
bool fits_signed(int64_t, unsigned bits);

#endif
