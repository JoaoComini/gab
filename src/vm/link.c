#include "vm/link.h"

#include "arena.h"
#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include "vm/chunk.h"
#include "vm/interp.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <stdlib.h>

// Frees what a prototype allocated. The prototype itself is arena-owned and is
// reclaimed with the VM.
//
// A cleared chunk means there is nothing to free, which is how a unit hands its
// top level to the caller: compile_unit clears the chunk it gave away, so
// unit_free walks the same prototype and drops none of it.
void func_proto_free(FuncPrototype *proto) {
    if (!proto->chunk) {
        return;
    }

    chunk_free(proto->chunk);
    frame_ref_list_free(&proto->refs);
    proto->chunk = NULL;
}

// Frees a unit nothing linked. The prototypes come from an arena and are not
// freed here; what each one allocated is, because a unit that never linked is
// the only owner those chunks ever had.
void unit_free(Unit *unit) {
    if (!unit) {
        return;
    }

    func_proto_free(&unit->top_level);
    func_proto_list_free(&unit->prototypes);
    type_list_free(&unit->types);
    string_list_free(&unit->strings);
    relocation_list_free(&unit->proto_relocations);
    relocation_list_free(&unit->type_relocations);
    relocation_list_free(&unit->string_relocations);
    proto_binding_list_free(&unit->bindings);
    extern_request_list_free(&unit->externs);

    free(unit);
}

// Rewrites one I-type operand to what it means now the unit has a base.
static void relocate(const RelocationList *relocations, size_t base) {
    for (size_t i = 0; i < relocations->size; i++) {
        const Relocation *reloc = &relocations->data[i];
        Instruction instruction = instruction_list_get(&reloc->chunk->instructions, reloc->offset);

        chunk_patch_instruction(reloc->chunk, reloc->offset,
                                VM_ENCODE_I(VM_DECODE_OPCODE(instruction), VM_DECODE_I_RD(instruction),
                                            (unsigned int)(VM_DECODE_I_KX(instruction) + base)));
    }
}

// Rewrites each index operand to the index the program gave what it names.
// Unlike a prototype the mapping is not a single base: something an earlier unit
// already registered keeps that unit's index, so a unit's entries can land out
// of order and each one is looked up.
static void remap_indices(const RelocationList *relocations, const size_t *index_map) {
    for (size_t i = 0; i < relocations->size; i++) {
        const Relocation *reloc = &relocations->data[i];
        Instruction instruction = instruction_list_get(&reloc->chunk->instructions, reloc->offset);

        chunk_patch_instruction(reloc->chunk, reloc->offset,
                                VM_ENCODE_I(VM_DECODE_OPCODE(instruction), VM_DECODE_I_RD(instruction),
                                            (unsigned int)index_map[VM_DECODE_I_KX(instruction)]));
    }
}

// The host body bound to a name, or NULL if none is. Both are interned, so
// identity is the comparison.
static GabExternFn find_extern(const Program *program, const Symbol *symbol) {
    for (size_t i = 0; i < program->externs.size; i++) {
        const ExternBinding *binding = &program->externs.data[i];

        if (binding->name == symbol->func.name && binding->module == symbol->func.module) {
            return binding->fn;
        }
    }

    return NULL;
}

// Whether this unit could be installed: the indices fit their operand fields
// once rebased, and every extern names a host body that exists.
//
// Answers without touching the program, which is what lets a caller compile a unit
// to find out whether it would load and then walk away. The externs it resolves
// are written into the unit's own prototypes, not the program's.
bool link_check(Program *program, Unit *unit, Diagnostics *diagnostics) {
    if (program->prototypes.size + unit->prototypes.size > VM_MAX_PROTOTYPES) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many functions in one program");
        return false;
    }

    // An upper bound: interning may add fewer than the unit declared, never
    // more.
    if (program->heap_types.size + unit->types.size > VM_MAX_HEAP_TYPES) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many allocated types in one program");
        return false;
    }

    // Allocated here rather than while installing, so that installing has
    // nothing left in it that can fail.
    if (unit->types.size && !unit->type_map) {
        unit->type_map = arena_alloc(unit->arena, unit->types.size * sizeof(size_t));

        if (!unit->type_map) {
            return false;
        }
    }

    if (program->strings.size + unit->strings.size > VM_MAX_STRINGS) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many string literals in one program");
        return false;
    }

    if (unit->strings.size && !unit->string_map) {
        unit->string_map = arena_alloc(unit->arena, unit->strings.size * sizeof(size_t));

        if (!unit->string_map) {
            return false;
        }
    }

    for (size_t i = 0; i < unit->externs.size; i++) {
        const ExternRequest *request = &unit->externs.data[i];
        GabExternFn native = find_extern(program, request->symbol);

        if (!native) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, request->span,
                       "extern function '%s' was never registered", request->symbol->func.name->data);
            return false;
        }

        unit->prototypes.data[request->local_index]->extern_body = native;
        unit->prototypes.data[request->local_index]->native = vm_call_extern;
    }

    return true;
}

// Installs a unit the caller has already checked with link_check.
//
// Nothing here can fail, which is the point: by the time anything is appended,
// every question that could have refused the unit has been asked.
void link_install(Program *program, Unit *unit) {
    size_t proto_base = program->prototypes.size;

    for (size_t i = 0; i < unit->prototypes.size; i++) {
        func_proto_list_add(&program->prototypes, unit->prototypes.data[i]);
    }

    // Interned against what the program already holds, so a type two units both
    // allocate is registered once. A prototype cannot be shared this way --
    // each declaration is a distinct function -- which is why only types are
    // looked up rather than appended outright.
    for (size_t i = 0; i < unit->types.size; i++) {
        size_t found = program->heap_types.size;

        for (size_t j = 0; j < program->heap_types.size; j++) {
            if (program->heap_types.data[j] == unit->types.data[i]) {
                found = j;
                break;
            }
        }

        if (found == program->heap_types.size) {
            type_list_add(&program->heap_types, unit->types.data[i]);
        }

        unit->type_map[i] = found;
    }

    // Interned in the pool already, so equality is pointer identity and a
    // literal two units share is registered once.
    for (size_t i = 0; i < unit->strings.size; i++) {
        size_t found = program->strings.size;

        for (size_t j = 0; j < program->strings.size; j++) {
            if (program->strings.data[j] == unit->strings.data[i]) {
                found = j;
                break;
            }
        }

        if (found == program->strings.size) {
            string_list_add(&program->strings, unit->strings.data[i]);
        }

        unit->string_map[i] = found;
    }

    relocate(&unit->proto_relocations, proto_base);
    remap_indices(&unit->type_relocations, unit->type_map);
    remap_indices(&unit->string_relocations, unit->string_map);

    // Last, because a symbol stamped with an index is a symbol a later compile
    // will call through: nothing may carry one until the prototype it names is
    // installed.
    for (size_t i = 0; i < unit->bindings.size; i++) {
        const ProtoBinding *binding = &unit->bindings.data[i];
        binding->symbol->func.func_index = proto_base + binding->local_index;
    }
}
