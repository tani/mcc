#ifndef MCC_REGISTER_H
#define MCC_REGISTER_H

#include "compiler.h"

unsigned asm_register_binding(Context *, const char *, Type *, bool, Loc);
bool asm_call_clobbered(Context *, unsigned);
uint64_t asm_call_saved_registers(Context *);
size_t asm_register_spill_layout(uint64_t, size_t [64]);
const char *x86_asm_register(unsigned, unsigned);

#endif
