#include "ast/pending.h"
#include <assert.h>

#include "type/type.h"

#define GAB_MAX_INSTANTIATIONS 256

bool type_args_are_concrete(const TypeArg *args, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (args[i].kind == TYPE_ARG_TYPE ? type_has_param(args[i].type)
                                          : args[i].constant.kind == CONST_PARAM) {
            return false;
        }
    }

    return true;
}

PendingBodies pending_bodies_create(Arena *arena) {
    return (PendingBodies){
        .bodies = pending_body_list_create(arena_allocator(arena)),
        .instances = instantiation_list_create(arena_allocator(arena)),
    };
}

void pending_bodies_instantiate(PendingBodies *work, Function *method, Diagnostics *diagnostics) {
    if (function_lowers_no_body(method)) {
        return;
    }

    if (method->decl->type_param_count == 0 || method->type_arg_count == 0) {
        return;
    }

    assert(type_args_are_concrete(method->type_args, method->type_arg_count) &&
           "an instance wanted here is fixed to arguments with a width");

    for (size_t i = 0; i < work->instances.size; i++) {
        if (work->instances.data[i] == method) {
            return;
        }
    }

    if (work->instances.size >= GAB_MAX_INSTANTIATIONS) {
        if (!work->instantiation_overflowed) {
            work->instantiation_overflowed = true;

            diag_error(diagnostics, GAB_ERR_TYPE, (Span){0}, "'%s' instantiates itself without end",
                       method->decl->id.name->data);
        }

        return;
    }

    instantiation_list_add(&work->instances, method);
}
