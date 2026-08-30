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

    scope_decl_type_def(scope, def->name, def);

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

    bool declared =
        type_registry_declare_method_on_type(vm->env.global_scope.type_registry, declared_on, &method);

    assert(declared && "a builtin declares each of its methods once");
    (void)declared;
}

void builtin_declare_method(VM *vm, const TypeDef *declared_on, const char *name, GabExternFn body,
                            const Type *receiver, const Type *result, const Type *const *params,
                            size_t param_count) {
    const MethodDecl method = {
        .name = string_from_cstr(&vm->env.strings, name),
        .body = (void *)body,
        .receiver = receiver,
        .result = result,
        .params = owned_params(vm->env.arena, params, param_count),
        .param_count = param_count,
    };

    bool declared = type_registry_declare_method_on_decl(vm->env.global_scope.type_registry,
                                                         (TypeDef *)declared_on, &method);

    assert(declared && "a builtin declares each of its methods once");
    (void)declared;
}

void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count) {
    Arena *arena = vm->env.arena;

    Symbol *symbol = arena_alloc(arena, sizeof(Symbol));
    symbol->kind = SYMBOL_FUNC;
    symbol->func.return_type = return_type;
    symbol->func.param_count = param_count;
    symbol->func.params = param_count ? arena_alloc(arena, sizeof(const Type *) * param_count) : NULL;
    symbol->func.is_extern = true;
    symbol->func.name = string_from_cstr(&vm->env.strings, name);
    symbol->func.module = NULL;

    for (size_t i = 0; i < param_count; i++) {
        symbol->func.params[i] = params[i];
    }

    symbol->func.func_index = SYMBOL_FUNC_NO_BODY;
    symbol->func.body = (void *)body;

    type_registry_add_method(vm->env.global_scope.type_registry, declared_on,
                             string_from_cstr(&vm->env.strings, name), symbol);
}

void builtin_register_all(VM *vm) {
    builtin_register_string(vm);
    builtin_register_vec(vm);
}
