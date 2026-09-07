#include "ast/pending.h"

#include "type/type.h"

#define GAB_MAX_INSTANTIATIONS 256

/* True when an argument the specialization was given is itself still a parameter, so it has no width. */
static bool instantiated_abstractly(const Function *method) {
    for (size_t i = 0; i < method->type_arg_count; i++) {
        const TypeArg arg = method->type_args[i];

        if (arg.kind == TYPE_ARG_TYPE ? type_has_param(arg.type) : arg.constant.kind == CONST_PARAM) {
            return true;
        }
    }

    return false;
}

PendingBodies pending_bodies_create(Arena *arena) {
    return (PendingBodies){
        .bodies = pending_body_list_create(arena_allocator(arena)),
        .instances = instantiation_list_create(arena_allocator(arena)),
    };
}

void pending_bodies_instantiate(PendingBodies *work, Function *generic, Function *method,
                                Diagnostics *diagnostics) {
    if (function_runs_native(method)) {
        return;
    }

    if (method->decl->type_param_count == 0 || method == generic || instantiated_abstractly(method)) {
        return;
    }

    for (size_t i = 0; i < work->instances.size; i++) {
        if (work->instances.data[i].instance == method) {
            return;
        }
    }

    if (work->instances.size >= GAB_MAX_INSTANTIATIONS) {
        if (!work->instantiation_overflowed) {
            work->instantiation_overflowed = true;

            diag_error(diagnostics, GAB_ERR_TYPE, (Span){0}, "'%s' instantiates itself without end",
                       method->decl->name->data);
        }

        return;
    }

    instantiation_list_add(&work->instances, (Instantiation){.generic = generic, .instance = method});
}
