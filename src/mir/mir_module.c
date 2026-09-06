#include "mir/mir_module.h"

MIRModule *mir_module_create(Arena *arena) {
    MIRModule *unit = arena_alloc(arena, sizeof(MIRModule));

    unit->entries = mir_module_entry_list_create(arena_allocator(arena));

    return unit;
}

void mir_module_add(MIRModule *unit, Function *function, MIRFunction *ir) {
    mir_module_entry_list_add(&unit->entries, (MIRModuleEntry){.function = function, .ir = ir});
}

MIRFunction *mir_module_lookup(const MIRModule *unit, const Function *function) {
    for (size_t i = 0; i < unit->entries.size; i++) {
        if (unit->entries.data[i].function == function) {
            return unit->entries.data[i].ir;
        }
    }

    return NULL;
}
