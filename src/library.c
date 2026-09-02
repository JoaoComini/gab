#include "library.h"

#include "arena.h"
#include "binding.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

#include "compile.h"
#include "diagnostics.h"
#include "gab.h"

#include <assert.h>
#include <stddef.h>

GabLibrary library_open(VM *vm, const char *module, bool is_prelude) {
    Scope *scope = is_prelude
                       ? &vm->env.global_scope
                       : environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, module));

    return (GabLibrary){.vm = vm, .scope = scope, .module = module, .is_prelude = is_prelude};
}

void library_extern(GabLibrary *lib, const char *type, const char *name, GabExternFn body) {
    GabError err;

    bool bound = gab_extern((GabVM *)lib->vm, lib->module, type, name, body, &err);

    assert(bound && "a library binds each of its externs once");
    (void)bound;
}

void library_declare_source(GabLibrary *lib, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, lib->vm->env.compile_arena, lib->module);

    bool loaded = compile_load_library(lib->vm, source, lib->is_prelude, &diagnostics);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    diagnostics_free(&diagnostics);
}

const TypeDecl *library_type(GabLibrary *lib, const LibraryTypeSpec *spec) {
    VM *vm = lib->vm;
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

    type_registry_declare(vm->env.global_scope.type_registry, &declaration);

    scope_bind_decl(lib->scope, decl->name, decl);

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

static FuncDecl *library_func_decl(VM *vm, const char *name, GabExternFn body) {
    FuncDecl *decl = arena_alloc(vm->env.arena, sizeof(FuncDecl));

    *decl = (FuncDecl){
        .name = string_from_cstr(&vm->env.strings, name),
        .body_kind = BODY_NATIVE,
        .body = (void *)body,
    };

    return decl;
}

void library_method(GabLibrary *lib, const Type *declared_on, const Type *receiver, const char *name,
                    GabExternFn body, const Type *return_type, const Type *const *params,
                    size_t param_count) {
    VM *vm = lib->vm;

    Function *method = arena_alloc(vm->env.arena, sizeof(Function));

    *method = (Function){
        .decl = library_func_decl(vm, name, body),
        .return_type = return_type,
        .params = owned_signature(vm->env.arena, receiver, params, param_count),
        .param_count = param_count + 1,
        .func_index = FUNCTION_NO_BODY,
    };

    bool declared = type_registry_declare_owned(vm->env.global_scope.type_registry, declared_on, method);

    assert(declared && "a library declares each of its methods once");
    (void)declared;
}

void library_static(GabLibrary *lib, const Type *declared_on, const char *name, GabExternFn body,
                    const Type *return_type, const Type *const *params, size_t param_count) {
    VM *vm = lib->vm;

    Function *function = arena_alloc(vm->env.arena, sizeof(Function));

    *function = (Function){
        .decl = library_func_decl(vm, name, body),
        .return_type = return_type,
        .params = owned_signature(vm->env.arena, NULL, params, param_count),
        .param_count = param_count,
        .func_index = FUNCTION_NO_BODY,
    };

    bool ok = type_registry_declare_owned(vm->env.global_scope.type_registry, declared_on, function);

    assert(ok && "a library declares each of its functions once");
    (void)ok;
}
