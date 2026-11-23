#ifndef MCC_INLINE_ASM_H
#define MCC_INLINE_ASM_H

#include "compiler.h"

typedef enum { ASM_UNBOUND, ASM_REGISTER, ASM_MEMORY, ASM_IMMEDIATE, ASM_FLAGS } AsmLocation;
typedef struct {
    AsmLocation location;
    unsigned reg, width, register_binding;
    size_t load, result, address;
    Symbol *symbol;
    uint64_t immediate;
    const char *condition;
    bool read_write, early, global_register;
    int match;
} AsmBinding;

struct AsmPlan {
    InlineAssembly *assembly;
    AsmBinding bindings[ASM_MAX_OPERANDS];
    size_t labels[ASM_MAX_OPERANDS];
    size_t save_slots[64];
    uint64_t preserve;
    unsigned serial;
    Loc loc;
    bool file_scope;
};

bool plan_inline_asm(Context *, AsmPlan *);
unsigned asm_register_binding(Context *, const char *, Type *, bool, Loc);
bool asm_call_clobbered(Context *, unsigned);
uint64_t asm_call_saved_registers(Context *);
size_t asm_register_spill_layout(uint64_t, size_t [64]);
void emit_asm_template(Context *, FILE *, const AsmPlan *, unsigned);
const char *x86_asm_register(unsigned, unsigned);

#endif
