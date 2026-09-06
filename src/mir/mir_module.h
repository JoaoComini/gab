#ifndef GAB_MIR_MODULE_H
#define GAB_MIR_MODULE_H

#include "memory/arena.h"
#include "mir/mir.h"
#include "util/list.h"

typedef struct {
    Function *function;
    MIRFunction *ir;
} MIRModuleEntry;

GAB_LIST(MIRModuleEntryList, mir_module_entry_list, MIRModuleEntry)

/* The lowered body of every function a unit resolved, which codegen reads instead of lowering again. */
typedef struct {
    MIRModuleEntryList entries;
} MIRModule;

MIRModule *mir_module_create(Arena *arena);

void mir_module_add(MIRModule *unit, Function *function, MIRFunction *ir);

MIRFunction *mir_module_lookup(const MIRModule *unit, const Function *function);

#endif
