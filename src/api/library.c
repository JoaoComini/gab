#include "api/library.h"

#include "compile.h"
#include "diagnostics.h"
#include "memory/arena.h"
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
