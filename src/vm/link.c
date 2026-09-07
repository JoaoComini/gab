#include "vm/link.h"

#include "binding.h"
#include "memory/arena.h"
#include "scope.h"
#include "string/string.h"
#include "vm/chunk.h"
#include "vm/interp.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <stdlib.h>

void func_proto_free(FuncPrototype *proto) {
    if (!proto->chunk) {
        return;
    }

    chunk_free(proto->chunk);
    frame_ref_list_free(&proto->refs);
    proto->chunk = NULL;
}

void object_file_take_top_level(ObjectFile *unit, FuncPrototype *out) {
    *out = unit->top_level;

    unit->top_level.chunk = NULL;
    unit->top_level.refs = frame_ref_list_create(DEFAULT_ALLOCATOR);
}

void object_file_free(ObjectFile *unit) {
    if (!unit) {
        return;
    }

    func_proto_free(&unit->top_level);

    for (size_t i = 0; i < unit->prototypes.size; i++) {
        func_proto_free(unit->prototypes.data[i]);
    }
    func_proto_list_free(&unit->prototypes);
    type_list_free(&unit->types);
    heap_shape_list_free(&unit->type_shapes);
    string_list_free(&unit->strings);
    relocation_list_free(&unit->proto_relocations);
    relocation_list_free(&unit->type_relocations);
    relocation_list_free(&unit->string_relocations);
    proto_binding_list_free(&unit->bindings);

    free(unit);
}

static void relocate(const RelocationList *relocations, size_t base) {
    for (size_t i = 0; i < relocations->size; i++) {
        const Relocation *reloc = &relocations->data[i];
        Instruction instruction = instruction_list_get(&reloc->chunk->instructions, reloc->offset);

        chunk_patch_instruction(reloc->chunk, reloc->offset,
                                VM_ENCODE_I(VM_DECODE_OPCODE(instruction), VM_DECODE_I_RD(instruction),
                                            (unsigned int)(VM_DECODE_I_KX(instruction) + base)));
    }
}

static void remap_indices(const RelocationList *relocations, const size_t *index_map) {
    for (size_t i = 0; i < relocations->size; i++) {
        const Relocation *reloc = &relocations->data[i];
        Instruction instruction = instruction_list_get(&reloc->chunk->instructions, reloc->offset);

        chunk_patch_instruction(reloc->chunk, reloc->offset,
                                VM_ENCODE_I(VM_DECODE_OPCODE(instruction), VM_DECODE_I_RD(instruction),
                                            (unsigned int)index_map[VM_DECODE_I_KX(instruction)]));
    }
}

bool link_check(Program *program, ObjectFile *unit, Diagnostics *diagnostics) {
    if (program->prototypes.size + unit->prototypes.size > VM_MAX_PROTOTYPES) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many functions in one program");
        return false;
    }

    if (program->heap_shapes.size + unit->types.size > VM_MAX_HEAP_TYPES) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many allocated types in one program");
        return false;
    }

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

    return true;
}

void link_install(Program *program, ObjectFile *unit) {
    size_t proto_base = program->prototypes.size;

    for (size_t i = 0; i < unit->prototypes.size; i++) {
        func_proto_list_add(&program->prototypes, unit->prototypes.data[i]);
    }

    for (size_t i = 0; i < unit->types.size; i++) {
        const Type *type = unit->types.data[i];
        size_t found = program->heap_shapes.size;

        for (size_t j = 0; j < program->heap_shapes.size; j++) {
            if (program->shape_types.data[j] == type) {
                found = j;
                break;
            }
        }

        if (found == program->heap_shapes.size) {
            heap_shape_list_add(&program->heap_shapes, unit->type_shapes.data[i]);

            type_list_add(&program->shape_types, type);
        }

        unit->type_map[i] = found;
    }

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

    for (size_t i = 0; i < unit->bindings.size; i++) {
        const ProtoBinding *binding = &unit->bindings.data[i];
        binding->function->func_index = proto_base + binding->local_index;
    }

    /* The program owns the prototypes now, so freeing the unit must not walk them. */
    unit->prototypes.size = 0;
}
