#include "type_app.h"
#include "type_internal.h"

#include <stdint.h>

// Mixed rather than summed, so that '[int; 3]' and '[int; 4]' -- and an
// application whose arguments were given in the other order -- land in
// different buckets. dj2b's shift-and-add over each argument's bytes, which is
// what the string hash beside it does over characters.
size_t type_app_hash_of(TypeApp app) {
    size_t hash = 5381;

    hash = ((hash << 5) + hash) + (size_t)app.ctor;
    hash = ((hash << 5) + hash) + (size_t)(uintptr_t)app.decl;

    for (size_t i = 0; i < app.arg_count; i++) {
        const TypeArg *arg = &app.args[i];

        hash = ((hash << 5) + hash) + (size_t)arg->kind;

        // A type argument is compared by identity, so its address is what
        // distinguishes it; a const argument is its value.
        hash = ((hash << 5) + hash) +
               (arg->kind == TYPE_ARG_TYPE ? (size_t)(uintptr_t)arg->type : (size_t)(uint32_t)arg->value);
    }

    return hash;
}

bool type_app_equals(TypeApp app, TypeApp other) {
    if (app.ctor != other.ctor || app.decl != other.decl || app.arg_count != other.arg_count) {
        return false;
    }

    for (size_t i = 0; i < app.arg_count; i++) {
        if (app.args[i].kind != other.args[i].kind) {
            return false;
        }

        // An interned type is one Type, so identity is the whole comparison.
        if (app.args[i].kind == TYPE_ARG_TYPE) {
            if (app.args[i].type != other.args[i].type) {
                return false;
            }
        } else if (app.args[i].value != other.args[i].value) {
            return false;
        }
    }

    return true;
}
