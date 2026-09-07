#include "mir/mir_build.h"

#include "ast/resolve.h"

#include "mir/mir_borrowck.h"
#include "mir/mir_instantiate.h"
#include "mir/mir_lower.h"

/* A substituted body names instances of its own, which are wanted the same way a source call's are. */
static void collect_called_instances(PendingBodies *work, const MIRFunction *body, Diagnostics *diagnostics) {
    for (size_t b = 0; b < body->block_count; b++) {
        const MIRBlock *block = body->blocks[b];

        for (size_t i = 0; i < block->inst_count; i++) {
            const MIRInst *inst = &block->insts[i];

            if (inst->op != MIR_CALL) {
                continue;
            }

            Function *callee = inst->callee;

            if (callee && callee->type_arg_count > 0) {
                if (callee->decl->generic) {
                    pending_bodies_instantiate(work, callee->decl->generic, callee, diagnostics);
                }
            }
        }
    }
}

bool mir_build(Arena *arena, ResolvedUnit *resolved, MIRModule **out, Diagnostics *diagnostics) {
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
    }

    /* An instance substitutes the body its declaration lowered, and its calls name further instances,
     * so the list is walked by index while it grows rather than iterated once. */
    for (size_t i = 0; i < work->instances.size; i++) {
        Function *generic = work->instances.data[i].generic;
        Function *instance = work->instances.data[i].instance;

        MIRFunction *from = mir_module_lookup(bodies, generic);

        if (!from || mir_module_lookup(bodies, instance)) {
            continue;
        }

        MIRFunction *body = mir_instantiate(arena, resolved->registry, resolved->functions, from, instance,
                                            instance->type_args, instance->type_arg_count);

        mir_module_add(bodies, instance, body);

        collect_called_instances(work, body, diagnostics);
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
