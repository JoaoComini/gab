#include "builtin/builtin.h"

#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

const Type *builtin_declare_type(VM *vm, const TypeDecl *decl) {
    Scope *scope = &vm->env.global_scope;

    const Type *type = type_registry_declare(scope->type_registry, decl);

    scope_decl_type(scope, type_name_of(type), type);

    return type;
}

void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count) {
    Arena *arena = vm->env.arena;

    Symbol *symbol = arena_alloc(arena, sizeof(Symbol));
    symbol->kind = SYMBOL_FUNC;
    symbol->func.return_type = return_type;
    symbol->func.param_count = param_count + 1;
    symbol->func.params = arena_alloc(arena, sizeof(const Type *) * (param_count + 1));
    symbol->func.is_extern = true;
    symbol->func.name = string_from_cstr(&vm->env.strings, name);
    symbol->func.module = NULL;

    // The receiver is parameter zero, by value: a string is a header that
    // copies, and a method that only reads it wants no indirection.
    symbol->func.params[0] = receiver;

    for (size_t i = 0; i < param_count; i++) {
        symbol->func.params[i + 1] = params[i];
    }

    symbol->func.func_index = vm->program.extern_protos.size;

    extern_proto_list_add(&vm->program.extern_protos, (ExternProto){.body = body, .symbol = symbol});

    type_registry_add_method(vm->env.global_scope.type_registry, declared_on,
                             string_from_cstr(&vm->env.strings, name), symbol);
}

// As builtin_register_method, for a function the type owns rather than one a
// value reaches: 'params' are every parameter, since there is no receiver to be
// parameter zero.
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

    symbol->func.func_index = vm->program.extern_protos.size;

    extern_proto_list_add(&vm->program.extern_protos, (ExternProto){.body = body, .symbol = symbol});

    type_registry_add_method(vm->env.global_scope.type_registry, declared_on,
                             string_from_cstr(&vm->env.strings, name), symbol);
}

// These land at the bottom of the extern table, before any unit loads. That
// costs a unit's own externs nothing: the two tables are numbered apart, so a
// script function's index is unaffected by how many builtins exist.
void builtin_register_all(VM *vm) {
    builtin_register_string(vm);
    builtin_register_array(vm);
    builtin_register_vec(vm);
}
