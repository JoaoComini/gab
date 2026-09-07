#include "driver/compile.h"

#include "driver/interface.h"
#include "driver/link.h"

#include "ast/resolve.h"
#include "mir/mir_build.h"
#include "mir/mir_drop.h"
#include "mir/mir_fold.h"
#include "mir/mir_module.h"
#include "scope.h"
#include "string/string_pool.h"
#include "syntax/parser.h"
#include "llvm/llvm_emit.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAB_COMPILE_BLOCK_SIZE 4096

/* Resolves one unit into 'scope' and lowers every body it declares, appending them to 'unit'. */
static bool compile_unit(Arena *arena, StringPool *strings, Scope *scope, ModuleScopeMap *modules,
                         const char *source, bool allow_primitive_impls, LLVMUnit *out,
                         Diagnostics *diagnostics, char *module_name, size_t module_capacity,
                         ASTUnit **out_ast) {
    ASTUnit *ast = ast_unit_create(arena);

    if (!parse_unit(source, arena, strings, &ast, diagnostics)) {
        return false;
    }

    if (out_ast) {
        *out_ast = ast;
    }

    if (module_name) {
        snprintf(module_name, module_capacity, "%.*s", (int)ast->module_name.length, ast->module_name.data);
    }

    ResolvedUnit *resolved = NULL;

    if (!resolve_unit(arena, ast, scope, modules, allow_primitive_impls, &resolved, diagnostics)) {
        return false;
    }

    MIRModule *bodies = NULL;

    if (!mir_build(arena, resolved, &bodies, diagnostics)) {
        return false;
    }

    if (!out) {
        return true;
    }

    for (size_t i = 0; i < bodies->entries.size; i++) {
        MIRFunction *ir = bodies->entries.data[i].ir;

        /* A generic's own body stands for its instances, which are emitted in its place. */
        if (!ir || mir_function_is_template(ir)) {
            continue;
        }

        mir_fold(arena, ir);
        mir_drop_elaborate(arena, scope->type_registry, ir);

        llvm_unit_add(out, ir);
    }

    return true;
}

bool gab_compile(GabCompile *request, Diagnostics *diagnostics) {
    Arena *arena = arena_create(GAB_COMPILE_BLOCK_SIZE);

    StringPool strings;
    string_pool_init(&strings, arena);

    Scope *scope = scope_create(arena, &strings, NULL);

    LLVMUnit *unit = llvm_unit_open(arena);

    char *interface = NULL;

    if (!request->is_core) {
        /* A program reads what the core declares, never the source those declarations came from. */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.gabi", gab_libdir(), GAB_CORE_MODULE);

        interface = gab_interface_read(path);

        if (!interface) {
            diag_error(diagnostics, GAB_ERR_NAME, (Span){0, 0}, "the core is not installed: no %s", path);

            llvm_unit_close(unit);
            string_pool_free(&strings);
            arena_destroy(arena);

            return false;
        }
    }

    ModuleScopeMap *modules = module_scope_map_create_alloc(arena_allocator(arena), 8);

    ASTUnit *core_ast = NULL;
    ASTUnit *unit_ast = NULL;

    /* The core's declarations are resolved into the scope a program then names, and its bodies are
     * emitted only when the core is what was asked for. */
    bool ok = compile_unit(arena, &strings, scope, NULL, interface ? interface : request->source, true,
                           request->is_core ? unit : NULL, diagnostics, NULL, 0, &core_ast);

    module_scope_map_insert(modules, string_from_cstr(&strings, GAB_CORE_MODULE), scope);

    /* Each import is its own compilation: its declarations land in a scope of their own, which the
     * unit then names, so a symbol keeps the module that defines it rather than taking this one's. */
    for (size_t i = 0; ok && i < request->import_count; i++) {
        const char *spec = request->imports[i];
        const char *equals = strchr(spec, '=');

        if (!equals) {
            diag_error(diagnostics, GAB_ERR_NAME, (Span){0, 0},
                       "an import is written '<module>=<path.gabi>', not '%s'", spec);
            ok = false;
            break;
        }

        char *text = gab_interface_read(equals + 1);

        if (!text) {
            diag_error(diagnostics, GAB_ERR_NAME, (Span){0, 0}, "no interface at %s", equals + 1);
            ok = false;
            break;
        }

        /* A module of its own, sharing the type registry so its 'i32' is this compilation's 'i32'. */
        Scope *imported = arena_alloc(arena, sizeof(Scope));
        scope_init_module(imported, arena, &strings, scope);

        ok = compile_unit(arena, &strings, imported, NULL, text, true, NULL, diagnostics, NULL, 0, NULL);

        free(text);

        if (ok) {
            StringRef name = {.data = spec, .length = (size_t)(equals - spec)};

            module_scope_map_insert(modules, string_from_ref(&strings, name), imported);
        }
    }

    if (ok && !request->is_core) {
        ok = compile_unit(arena, &strings, scope, modules, request->source, false, unit, diagnostics,
                          request->module_name, sizeof(request->module_name), &unit_ast);
    }

    if (ok && request->interface) {
        ok = gab_interface_write(request->is_core ? core_ast : unit_ast, request->interface);

        if (!ok) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0, 0}, "could not write %s", request->interface);
        }
    }

    if (ok) {
        const char *error = NULL;

        if (!llvm_unit_write_object(unit, request->object, &error)) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0, 0}, "%s", error);
            ok = false;
        }
    }

    llvm_unit_close(unit);

    free(interface);

    string_pool_free(&strings);
    arena_destroy(arena);

    return ok;
}
