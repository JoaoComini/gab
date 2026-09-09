#include "mir/mir_build.h"

#include "ast/resolve.h"

#include "mir/mir_borrowck.h"
#include "mir/mir_instantiate.h"
#include "mir/mir_lower.h"

/* A substituted body names instances of its own, which are wanted the same way a source call's are. */
static void instantiate_needed(PendingBodies *work, Function *callee, Diagnostics *diagnostics) {
    if (callee && callee->type_arg_count > 0) {
        pending_bodies_instantiate(work, callee, diagnostics);
    }
}

/* An ending is reached by the drop the backend writes rather than by a call, so what a drop needs is
 * collected beside what a call does or no instance of it is ever lowered. */
/* What a drop reaches through it ends too, so every level an owner reaches names the ending it declares. */
static void collect_dropped_endings(PendingBodies *work, ResolvedModule *resolved, const Type *type,
                                    Diagnostics *diagnostics) {
    if (!type) {
        return;
    }

    const String *name = type_registry_names(resolved->registry)->destroy_method;

    instantiate_needed(work, function_registry_owned_for(resolved->functions, type, name), diagnostics);

    collect_dropped_endings(work, resolved, type_pointee(type), diagnostics);

    const TypeFields *fields = type_registry_fields_of(resolved->registry, type);

    for (size_t i = 0; i < fields->count; i++) {
        if (type_registry_owns(resolved->registry, fields->fields[i].type)) {
            collect_dropped_endings(work, resolved, fields->fields[i].type, diagnostics);
        }
    }
}

static void collect_called_instances(PendingBodies *work, ResolvedModule *resolved, const MIRFunction *body,
                                     Diagnostics *diagnostics) {
    for (size_t b = 0; b < body->block_count; b++) {
        const MIRBlock *block = body->blocks[b];

        for (size_t i = 0; i < block->inst_count; i++) {
            const MIRInst *inst = &block->insts[i];

            if (inst->op == MIR_DROP) {
                collect_dropped_endings(work, resolved, inst->type, diagnostics);
                continue;
            }

            if (inst->op != MIR_CALL) {
                continue;
            }

            instantiate_needed(work, inst->callee, diagnostics);
        }
    }
}

bool mir_build(Arena *arena, ResolvedModule *resolved, MIRModule *imported, MIRModule **out,
               Diagnostics *diagnostics) {
    size_t errors = diagnostics_count(diagnostics);

    PendingBodies *work = &resolved->work;
    MIRModule *bodies = mir_module_create(arena);

    /* Lowered once, since the reporting sweep must see what the first sweep concluded. */
    MIRFunction **lowered = arena_alloc(arena, (work->bodies.size + 1) * sizeof(MIRFunction *));

    for (size_t i = 0; i < work->bodies.size; i++) {
        PendingBody *body = &work->bodies.data[i];

        lowered[i] = mir_build_function(arena, body->registry, &resolved->facts, body->function,
                                        body->param_fields, body->body);

        mir_module_add(bodies, body->function, lowered[i]);

        /* A body's own drops name endings too, so they are collected beside the instances its calls name. */
        collect_called_instances(work, resolved, lowered[i], diagnostics);
    }

    /* An instance substitutes the body its declaration lowered, and its calls name further instances,
     * so the list is walked by index while it grows rather than iterated once. */
    for (size_t i = 0; i < work->instances.size; i++) {
        Function *instance = work->instances.data[i];

        InstanceId generic = instance_id_of(instance->decl->id, NULL, 0);

        /* A generic this unit declared is lowered here; one an import declared came with its body. */
        MIRFunction *from = mir_module_lookup_id(bodies, generic);

        if (!from && imported) {
            from = mir_module_lookup_id(imported, generic);
        }

        if (!from || mir_module_lookup(bodies, instance)) {
            continue;
        }

        MIRFunction *body = mir_instantiate(arena, resolved->registry, resolved->functions, from, instance,
                                            instance->type_args, instance->type_arg_count);

        mir_module_add(bodies, instance, body);

        collect_called_instances(work, resolved, body, diagnostics);
    }

    for (size_t i = 0; i < work->bodies.size; i++) {
        mir_borrowck(arena, work->bodies.data[i].registry, lowered[i], diagnostics,
                     work->bodies.data[i].function, false);
    }

    for (size_t i = 0; i < work->bodies.size; i++) {
        mir_borrowck(arena, work->bodies.data[i].registry, lowered[i], diagnostics,
                     work->bodies.data[i].function, true);
    }

    *out = bodies;

    return diagnostics_count(diagnostics) == errors;
}
