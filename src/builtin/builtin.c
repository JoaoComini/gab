#include "builtin/builtin.h"

#include "arena.h"
#include "binding.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <assert.h>
#include <stddef.h>

const TypeDecl *builtin_declare(VM *vm, const BuiltinTypeSpec *spec) {
    Arena *arena = vm->env.arena;

    TypeField *fields = spec->field_count ? arena_alloc(arena, spec->field_count * sizeof(TypeField)) : NULL;

    for (size_t i = 0; i < spec->field_count; i++) {
        fields[i] = (TypeField){.name = spec->fields[i].name, .type = spec->fields[i].type};
    }

    TypeDecl *decl = arena_alloc(arena, sizeof(TypeDecl));

    *decl = (TypeDecl){
        .name = string_from_cstr(&vm->env.strings, spec->name),
        .param_count = spec->param_count,
        .fields = fields,
        .field_count = spec->field_count,
    };

    const TypeDeclSpec declaration = {
        .decl = decl,
        .derefs_to = spec->derefs_to,
        .lent_parts = spec->lent_parts,
        .lent_part_count = spec->lent_part_count,
    };

    Scope *scope = &vm->env.global_scope;

    type_registry_declare(scope->type_registry, &declaration);

    scope_bind_decl(scope, decl->name, decl);

    return decl;
}

static const Type **owned_signature(Arena *arena, const Type *receiver, const Type *const *params,
                                    size_t param_count) {
    size_t leading = receiver ? 1 : 0;

    if (leading + param_count == 0) {
        return NULL;
    }

    const Type **copy = arena_alloc(arena, (leading + param_count) * sizeof(const Type *));

    if (receiver) {
        copy[0] = receiver;
    }

    for (size_t i = 0; i < param_count; i++) {
        copy[leading + i] = params[i];
    }

    return copy;
}

static FuncDecl *builtin_decl(VM *vm, const char *name, GabExternFn body) {
    FuncDecl *decl = arena_alloc(vm->env.arena, sizeof(FuncDecl));

    *decl = (FuncDecl){
        .name = string_from_cstr(&vm->env.strings, name),
        .body_kind = BODY_NATIVE,
        .body = (void *)body,
    };

    return decl;
}

void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count) {
    Function *method = arena_alloc(vm->env.arena, sizeof(Function));

    *method = (Function){
        .decl = builtin_decl(vm, name, body),
        .return_type = return_type,
        .params = owned_signature(vm->env.arena, receiver, params, param_count),
        .param_count = param_count + 1,
        .func_index = FUNCTION_NO_BODY,
    };

    bool declared = type_registry_declare_owned(vm->env.global_scope.type_registry, declared_on, method);

    assert(declared && "a builtin declares each of its methods once");
    (void)declared;
}

void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count) {
    Function *function = arena_alloc(vm->env.arena, sizeof(Function));

    *function = (Function){
        .decl = builtin_decl(vm, name, body),
        .return_type = return_type,
        .params = owned_signature(vm->env.arena, NULL, params, param_count),
        .param_count = param_count,
        .func_index = FUNCTION_NO_BODY,
    };

    bool ok = type_registry_declare_owned(vm->env.global_scope.type_registry, declared_on, function);

    assert(ok && "a builtin declares each of its functions once");
    (void)ok;
}

void builtin_register_all(VM *vm) {
    builtin_register_string(vm);
    builtin_register_vec(vm);
}
