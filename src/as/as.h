#ifndef MCC_AS_H
#define MCC_AS_H

#include "elf_tools.h"

typedef struct AsmSymbol AsmSymbol;
typedef struct { AsmSymbol *symbol; uint64_t value; } AsmExpr;
typedef struct { char *data[32]; size_t len; } AsmArgs;
typedef struct {
    uint64_t offset;
    uint32_t type;
    AsmExpr expr;
} AsmReloc;

typedef struct {
    Bytes bytes;
    VECTOR(AsmReloc) relocations;
    size_t size, alignment;
    bool present;
} AsmSection;

enum { SECTION_TDATA = SECTION_COUNT, SECTION_TBSS, SECTION_FINI_ARRAY, ASM_SECTION_COUNT };

typedef struct {
    Context *context;
    Loc loc;
    uint16_t machine;
    unsigned section;
    AsmSection sections[ASM_SECTION_COUNT];
    VECTOR(AsmSymbol *) symbols;
} Assembler;

bool mas_assemble(Context *, ElfOptions *);
char *mas_trim(char *);
bool mas_integer(const char *, uint64_t *);
bool mas_expression(Assembler *, char *, AsmExpr *);
void mas_emit(Assembler *, uint64_t, unsigned width);
void mas_relocate(Assembler *, uint32_t, AsmExpr);
bool mas_aarch64(Assembler *, const char *op, AsmArgs *);
bool mas_x86_64(Assembler *, const char *op, AsmArgs *);

#endif
