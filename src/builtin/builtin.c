#include "builtin/builtin.h"

#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <assert.h>
#include <stddef.h>

const TypeDef *builtin_declare(VM *vm, const BuiltinTypeSpec *spec) {
    Arena *arena = vm->env.arena;

    TypeField *fields = spec->field_count ? arena_alloc(arena, spec->field_count * sizeof(TypeField)) : NULL;

    for (size_t i = 0; i < spec->field_count; i++) {
        fields[i] = (TypeField){.name = spec->fields[i].name, .type = spec->fields[i].type};
    }

    TypeDef *def = arena_alloc(arena, sizeof(TypeDef));

    *def = (TypeDef){
        .name = string_from_cstr(&vm->env.strings, spec->name),
        .param_count = spec->param_count,
        .fields = fields,
        .field_count = spec->field_count,
    };

    const TypeDecl decl = {
        .def = def,
        .derefs_to = spec->derefs_to,
        .lent_parts = spec->lent_parts,
        .lent_part_count = spec->lent_part_count,
    };

    Scope *scope = &vm->env.global_scope;

    type_registry_declare(scope->type_registry, &decl);

    scope_bind_decl(scope, def->name, def);

    return def;
}

static const Type **owned_params(Arena *arena, const Type *const *params, size_t param_count) {
    if (param_count == 0) {
        return NULL;
    }

    const Type **copy = arena_alloc(arena, param_count * sizeof(const Type *));

    for (size_t i = 0; i < param_count; i++) {
        copy[i] = params[i];
    }

    return copy;
}

void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count) {
    const MethodDecl method = {
        .name = string_from_cstr(&vm->env.strings, name),
        .body = (void *)body,
        .receiver = receiver,
        .result = return_type,
        .params = owned_params(vm->env.arena, params, param_count),
        .param_count = param_count,
    };

    bool declared = type_registry_declare_method(vm->env.global_scope.type_registry, declared_on, &method);

    assert(declared && "a builtin declares each of its methods once");
    (void)declared;
}

void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count) {
    Arena *arena = vm->env.arena;

    Function *function = arena_alloc(arena, sizeof(Function));

    *function = (Function){
        .return_type = return_type,
        .params = (const Type **)owned_params(arena, params, param_count),
        .param_count = param_count,
        .is_extern = true,
        .name = string_from_cstr(&vm->env.strings, name),
        .module = NULL,
        .func_index = SYMBOL_FUNC_NO_BODY,
        .body = (void *)body,
    };

    const MethodDecl declared = {
        .name = function->name,
        .body = (void *)body,
        .result = return_type,
        .params = function->params,
        .param_count = param_count,
        .function = function,
    };

    bool ok = type_registry_declare_method(vm->env.global_scope.type_registry, declared_on, &declared);

    assert(ok && "a builtin declares each of its functions once");
    (void)ok;
}

void builtin_register_all(VM *vm) {
    builtin_register_string(vm);
    builtin_register_vec(vm);
}
