#include "vm/link.h"

#include "arena.h"
#include "binding.h"
#include "scope.h"
#include "string/string.h"
#include "vm/chunk.h"
#include "vm/ffi.h"
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

void unit_free(Unit *unit) {
    if (!unit) {
        return;
    }

    func_proto_free(&unit->top_level);

    for (size_t i = 0; i < unit->prototypes.size; i++) {
        func_proto_free(unit->prototypes.data[i]);
    }
    func_proto_list_free(&unit->prototypes);
    extern_proto_list_free(&unit->extern_protos);
    type_list_free(&unit->types);
    heap_shape_list_free(&unit->type_shapes);
    string_list_free(&unit->strings);
    relocation_list_free(&unit->proto_relocations);
    relocation_list_free(&unit->extern_relocations);
    relocation_list_free(&unit->type_relocations);
    relocation_list_free(&unit->string_relocations);
    proto_binding_list_free(&unit->bindings);
    extern_request_list_free(&unit->externs);

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

static const ExternBinding *find_extern(const Program *program, const Function *function) {
    for (size_t i = 0; i < program->extern_bindings.size; i++) {
        const ExternBinding *binding = &program->extern_bindings.data[i];

        if (binding->name == function->decl->name && binding->module == function->decl->module &&
            binding->owner == function->decl->owner) {
            return binding;
        }
    }

    return NULL;
}

bool link_check(Program *program, Unit *unit, Arena *arena, TypeRegistry *types, Diagnostics *diagnostics) {
    if (program->prototypes.size + unit->prototypes.size > VM_MAX_PROTOTYPES) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many functions in one program");
        return false;
    }

    if (program->extern_protos.size + unit->extern_protos.size > VM_MAX_EXTERN_PROTOS) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "too many extern functions in one program");
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

    for (size_t i = 0; i < unit->externs.size; i++) {
        const ExternRequest *request = &unit->externs.data[i];
        const ExternBinding *binding = find_extern(program, request->function);

        if (!binding) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, request->span,
                       "extern function '%s' was never registered", request->function->decl->name->data);
            return false;
        }

        ExternProto *proto = &unit->extern_protos.data[request->local_index];

        if (binding->fn) {
            proto->body = binding->fn;
            continue;
        }

        const char *reason = "the declaration cannot be expressed to C";
        const FfiSignature *signature =
            ffi_signature_prepare(arena, types, request->function, binding->symbol, &reason);

        if (!signature) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, request->span, "extern function '%s': %s",
                       request->function->decl->name->data, reason);
            return false;
        }

        proto->signature = signature;
    }

    return true;
}

void link_install(Program *program, Unit *unit) {
    size_t proto_base = program->prototypes.size;
    size_t extern_base = program->extern_protos.size;

    for (size_t i = 0; i < unit->prototypes.size; i++) {
        func_proto_list_add(&program->prototypes, unit->prototypes.data[i]);
    }

    for (size_t i = 0; i < unit->extern_protos.size; i++) {
        extern_proto_list_add(&program->extern_protos, unit->extern_protos.data[i]);
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
    relocate(&unit->extern_relocations, extern_base);
    remap_indices(&unit->type_relocations, unit->type_map);
    remap_indices(&unit->string_relocations, unit->string_map);

    for (size_t i = 0; i < unit->bindings.size; i++) {
        const ProtoBinding *binding = &unit->bindings.data[i];
        size_t base = function_runs_native(binding->function) ? extern_base : proto_base;

        binding->function->func_index = base + binding->local_index;
    }
}
