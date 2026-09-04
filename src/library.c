#include "library.h"

#include "arena.h"
#include "compile.h"
#include "diagnostics.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GabLib {
    VM *vm;
    Scope *scope;
    const char *module;
    bool is_prelude;
};

static GabLib *library_new(VM *vm, const char *module, bool is_prelude) {
    GabLib *lib = calloc(1, sizeof(GabLib));
    if (!lib) {
        return NULL;
    }

    *lib = (GabLib){
        .vm = vm,
        .scope = is_prelude ? &vm->env.global_scope
                            : environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, module)),
        .module = module,
        .is_prelude = is_prelude,
    };

    return lib;
}

GabLib *library_open_prelude(VM *vm, const char *module) { return library_new(vm, module, true); }

static void lib_error(GabError *err, const char *message) {
    if (!err) {
        return;
    }

    snprintf(err->message, sizeof(err->message), "%s", message);
    err->line = 0;
    err->column = 0;
}

GabLib *gab_lib_open(GabVM *handle, const char *module, GabError *err) {
    if (!handle || !module || module[0] == '\0') {
        lib_error(err, "gab_lib_open requires a VM and a module name");
        return NULL;
    }

    GabLib *lib = library_new((VM *)handle, module, false);

    if (!lib) {
        lib_error(err, "out of memory");
    }

    return lib;
}

void gab_lib_close(GabLib *lib) { free(lib); }

/* A host kind is cast straight to a 'TypeKind', so the two enumerations must stay in step. */
_Static_assert((int)GAB_TYPE_BLOCK == (int)TYPE_BLOCK, "a host type kind is the VM's own");

static TypeRegistry *lib_registry(GabLib *lib) { return lib->vm->env.global_scope.type_registry; }

const GabType *gab_lib_primitive(GabLib *lib, GabTypeKind kind) {
    return lib ? (const GabType *)type_registry_get_primitive(lib_registry(lib), (TypeKind)kind) : NULL;
}

const GabType *gab_lib_param(GabLib *lib, size_t index) {
    return lib ? (const GabType *)type_registry_param(lib_registry(lib), index) : NULL;
}

const GabType *gab_lib_block_of(GabLib *lib, const GabType *element) {
    return lib ? (const GabType *)type_registry_block_of(lib_registry(lib), (const Type *)element) : NULL;
}

const GabType *gab_lib_array_of(GabLib *lib, const GabType *element, int32_t length) {
    return lib ? (const GabType *)type_registry_array_of(lib_registry(lib), (const Type *)element, length)
               : NULL;
}

const GabType *gab_lib_slice_of(GabLib *lib, const GabType *element) {
    return lib ? (const GabType *)type_registry_slice_of(lib_registry(lib), (const Type *)element) : NULL;
}

const GabType *gab_lib_ptr_to(GabLib *lib, const GabType *pointee) {
    return lib ? (const GabType *)type_registry_ptr_to(lib_registry(lib), (const Type *)pointee) : NULL;
}

const GabType *gab_lib_type(GabLib *lib, const GabTypeSpec *spec, GabError *err) {
    if (!lib || !spec || !spec->name) {
        lib_error(err, "gab_lib_type requires a library and a named type");
        return NULL;
    }

    if (spec->lend_count > GAB_MAX_LENT_PARTS) {
        lib_error(err, "a type lends more parts than the VM tracks");
        return NULL;
    }

    VM *vm = lib->vm;
    Arena *arena = vm->env.arena;

    TypeField *fields = spec->field_count ? arena_alloc(arena, spec->field_count * sizeof(TypeField)) : NULL;

    for (size_t i = 0; i < spec->field_count; i++) {
        fields[i] = (TypeField){
            .name = string_from_cstr(&vm->env.strings, spec->fields[i].name),
            .type = (const Type *)spec->fields[i].type,
        };
    }

    TypeDecl *decl = arena_alloc(arena, sizeof(TypeDecl));

    *decl = (TypeDecl){
        .name = string_from_cstr(&vm->env.strings, spec->name),
        .param_count = spec->params,
        .fields = fields,
        .field_count = spec->field_count,
    };

    LentPart lends[GAB_MAX_LENT_PARTS];

    for (size_t i = 0; i < spec->lend_count; i++) {
        lends[i] = (LentPart){.offset = spec->lends[i].offset, .size = spec->lends[i].size};
    }

    const TypeDeclSpec declaration = {
        .decl = decl,
        .derefs_to = (const Type *)spec->derefs_to,
        .lent_parts = spec->lend_count ? lends : NULL,
        .lent_part_count = spec->lend_count,
    };

    type_registry_declare(lib_registry(lib), &declaration);

    scope_bind_decl(lib->scope, decl->name, decl);

    /* A type with no parameters has one instantiation, and its layout is settled here. */
    const Type *type = spec->params == 0 ? type_registry_apply(lib_registry(lib), decl, NULL, 0) : NULL;

    return (const GabType *)type;
}

bool gab_lib_bind(GabLib *lib, const char *type, const char *name, GabExternFn body, GabError *err) {
    if (!lib) {
        lib_error(err, "gab_lib_bind requires a library");
        return false;
    }

    return gab_extern((GabVM *)lib->vm, lib->module, type, name, body, err);
}

bool gab_lib_source(GabLib *lib, const char *source, GabError *err) {
    if (!lib || !source) {
        lib_error(err, "gab_lib_source requires a library and a source string");
        return false;
    }

    size_t prefix = strlen("module ") + strlen(lib->module) + strlen(";\n");
    size_t bytes = prefix + strlen(source) + 1;

    char *unit = malloc(bytes);
    if (!unit) {
        lib_error(err, "out of memory");
        return false;
    }

    snprintf(unit, bytes, "module %s;\n%s", lib->module, source);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, lib->vm->env.compile_arena, lib->module);

    bool loaded = compile_load_library(lib->vm, unit, lib->is_prelude, &diagnostics);

    if (!loaded && err) {
        if (diagnostics_count(&diagnostics) > 0) {
            const Diagnostic *diag = diagnostics_get(&diagnostics, 0);

            snprintf(err->message, sizeof(err->message), "%s", diag->message);
            err->line = diag->span.line;
            err->column = diag->span.column;
        } else {
            lib_error(err, "a library's declarations failed to compile");
        }
    }

    diagnostics_free(&diagnostics);
    free(unit);

    return loaded;
}
