#include "mir/mir_module.h"

MIRModule *mir_module_create(Arena *arena) {
    MIRModule *unit = arena_alloc(arena, sizeof(MIRModule));

    unit->entries = mir_module_entry_list_create(arena_allocator(arena));

    return unit;
}

void mir_module_add(MIRModule *unit, Function *function, MIRFunction *ir) {
    mir_module_entry_list_add(
        &unit->entries,
        (MIRModuleEntry){.function = function, .id = instance_id_of_function(function), .ir = ir});
}

MIRFunction *mir_module_lookup_id(const MIRModule *unit, InstanceId id) {
    /* A record standing for no declaration matches nothing, rather than the first unset id held. */
    if (!decl_id_is_set(id.decl)) {
        return NULL;
    }

    for (size_t i = 0; i < unit->entries.size; i++) {
        if (instance_id_equals(unit->entries.data[i].id, id)) {
            return unit->entries.data[i].ir;
        }
    }

    return NULL;
}

MIRFunction *mir_module_lookup(const MIRModule *unit, const Function *function) {
    return mir_module_lookup_id(unit, instance_id_of_function(function));
}
