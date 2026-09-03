#include "library.h"

#include <stdio.h>

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

Library library_open(VM *vm, const char *module, bool is_prelude) {
    Scope *scope = is_prelude
                       ? &vm->env.global_scope
                       : environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, module));

    return (Library){.vm = vm, .scope = scope, .module = module, .is_prelude = is_prelude};
}

void library_extern(Library *lib, const char *type, const char *name, GabExternFn symbol) {
    GabError err;

    bool bound = gab_extern((GabVM *)lib->vm, lib->module, type, name, symbol, &err);

    assert(bound && "a library binds each of its externs once");
    (void)bound;
}

void library_declare_source(Library *lib, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, lib->vm->env.compile_arena, lib->module);

    bool loaded = compile_load_library(lib->vm, source, lib->is_prelude, &diagnostics);

    if (!loaded && diagnostics_count(&diagnostics))
        fprintf(stderr, "LIBFAIL %s\n", diagnostics_get(&diagnostics, 0)->message);
    assert(loaded && "a library's declarations compile");
    (void)loaded;

    diagnostics_free(&diagnostics);
}

const TypeDecl *library_type(Library *lib, const LibraryTypeSpec *spec) {
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
