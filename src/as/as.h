#ifndef MCC_AS_H
#define MCC_AS_H

#include "elf_tools.h"

typedef struct AsmSymbol AsmSymbol;
typedef struct AsmDifference AsmDifference;
typedef struct { AsmSymbol *symbol; uint64_t value; } AsmExpr;
typedef struct { char *data[32]; size_t len; } AsmArgs;
typedef struct {
    uint64_t offset;
    uint32_t type;
    AsmExpr expr;
} AsmReloc;

typedef struct {
    SectionKind kind;
    Bytes bytes;
    VECTOR(AsmReloc) relocations;
    size_t size, alignment;
    AsmSymbol *linked_to;
    Loc link_location;
} AsmSection;

typedef struct {
    Context *context;
    Loc loc;
    uint16_t machine;
    unsigned section;
    VECTOR(AsmSection) sections;
    VECTOR(AsmSymbol *) symbols;
    VECTOR(AsmDifference) differences;
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
