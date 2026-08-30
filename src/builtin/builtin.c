#include "builtin/builtin.h"

#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

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

static Symbol *method_symbol(VM *vm, const Type *receiver, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count) {
    Arena *arena = vm->env.arena;

    Symbol *symbol = arena_alloc(arena, sizeof(Symbol));
    symbol->kind = SYMBOL_FUNC;
    symbol->func.return_type = return_type;
    symbol->func.param_count = param_count + 1;
    symbol->func.params = arena_alloc(arena, sizeof(const Type *) * (param_count + 1));
    symbol->func.is_extern = true;
    symbol->func.name = string_from_cstr(&vm->env.strings, name);
    symbol->func.module = NULL;

    symbol->func.params[0] = receiver;

    for (size_t i = 0; i < param_count; i++) {
        symbol->func.params[i + 1] = params[i];
    }

    symbol->func.func_index = vm->program.extern_protos.size;

    extern_proto_list_add(&vm->program.extern_protos, (ExternProto){.body = body, .symbol = symbol});

    return symbol;
}

void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count) {
    Symbol *symbol = method_symbol(vm, receiver, name, body, return_type, params, param_count);

    type_registry_add_method(vm->env.global_scope.type_registry, declared_on,
                             string_from_cstr(&vm->env.strings, name), symbol);
}

void builtin_declare_method(VM *vm, const TypeDef *declared_on, const char *name, GabExternFn body,
                            const Type *receiver, const Type *result, const Type *const *params,
                            size_t param_count) {
    Arena *arena = vm->env.arena;

    TypeDef *def = (TypeDef *)declared_on;

    GenericMethod *methods = arena_alloc(arena, (def->method_count + 1) * sizeof(GenericMethod));

    for (size_t i = 0; i < def->method_count; i++) {
        methods[i] = def->methods[i];
    }

    const Type **owned = NULL;

    if (param_count > 0) {
        const Type **copy = arena_alloc(arena, param_count * sizeof(const Type *));

        for (size_t i = 0; i < param_count; i++) {
            copy[i] = params[i];
        }

        owned = copy;
    }

    methods[def->method_count] = (GenericMethod){
        .name = string_from_cstr(&vm->env.strings, name),
        .body = (void *)body,
        .receiver = receiver,
        .result = result,
        .params = owned,
        .param_count = param_count,
    };

    def->methods = methods;
    def->method_count++;
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

    symbol->func.func_index = vm->program.extern_protos.size;

    extern_proto_list_add(&vm->program.extern_protos, (ExternProto){.body = body, .symbol = symbol});

    type_registry_add_method(vm->env.global_scope.type_registry, declared_on,
                             string_from_cstr(&vm->env.strings, name), symbol);
}

void builtin_register_all(VM *vm) {
    builtin_register_string(vm);
    builtin_register_vec(vm);
}
