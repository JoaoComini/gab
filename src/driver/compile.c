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
/* Every file of one module, parsed into a single unit: a module is one namespace however many files
 * write it, so what they declare is resolved together rather than one file at a time. */
static bool parse_module(Arena *arena, StringPool *strings, const char *const *sources, size_t count,
                         const char *const *names, ASTUnit **out, Diagnostics *diagnostics) {
    ASTUnit *whole = ast_unit_create(arena);

    for (size_t i = 0; i < count; i++) {
        ASTUnit *part = ast_unit_create(arena);

        if (!parse_unit(sources[i], arena, strings, &part, diagnostics)) {
            return false;
        }

        if (i == 0) {
            whole->module_name = part->module_name;
            whole->module_span = part->module_span;
        } else if (!string_ref_equals(whole->module_name, part->module_name)) {
            diag_error(diagnostics, GAB_ERR_NAME, part->module_span,
                       "%s declares module '%.*s', which is compiled as part of '%.*s'",
                       names && names[i] ? names[i] : "this file", (int)part->module_name.length,
                       part->module_name.data, (int)whole->module_name.length, whole->module_name.data);
            return false;
        }

        for (size_t j = 0; j < part->statements.size; j++) {
            ast_unit_add_statement(whole, part->statements.data[j]);
        }

        for (size_t j = 0; j < part->imports.size; j++) {
            ast_import_list_add(&whole->imports, part->imports.data[j]);
        }
    }

    *out = whole;

    return true;
}

static bool compile_unit(Arena *arena, StringPool *strings, Scope *scope, ModuleScopeMap *modules,
                         const char *const *sources, size_t source_count, bool allow_primitive_impls,
                         LLVMUnit *out, Diagnostics *diagnostics, char *module_name, size_t module_capacity,
                         ASTUnit **out_ast, const char *const *names, ASTUnit *parsed) {
    ASTUnit *ast = parsed;

    if (!ast && !parse_module(arena, strings, sources, source_count, names, &ast, diagnostics)) {
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

    ASTUnit *declaring = NULL;

    ASTUnit *core_ast = NULL;
    ASTUnit *unit_ast = NULL;

    /* The core's declarations are resolved into the scope a program then names, and its bodies are
     * emitted only when the core is what was asked for. */
    const char *core_sources[1] = {
        interface ? interface : (request->source_count ? request->sources[0] : NULL)};

    bool ok = compile_unit(arena, &strings, scope, NULL, core_sources, 1, true,
                           request->is_core ? unit : NULL, diagnostics, NULL, 0, &core_ast, NULL, NULL);

    module_scope_map_insert(modules, string_from_cstr(&strings, GAB_CORE_MODULE), scope);

    /* Each import is its own compilation: its declarations land in a scope of their own, which the
     * unit then names, so a symbol keeps the module that defines it rather than taking this one's. */
    if (ok && !request->is_core &&
        !parse_module(arena, &strings, request->sources, request->source_count, request->names, &declaring,
                      diagnostics)) {
        ok = false;
    }

    if (ok && !request->is_core) {
        GabSearchPath path = {.directories = request->search,
                              .count = request->search_count,
                              .source_directory = request->source_directory};

        /* The list grows as interfaces are read: an import's imports are linked too, though nothing
         * here names them. */
        ASTImportList reached = declaring->imports;

        for (size_t i = 0; ok && i < reached.size; i++) {
            StringRef named = reached.data[i].name;

            bool seen = false;

            for (size_t j = 0; j < i; j++) {
                seen = seen || string_ref_equals(reached.data[j].name, named);
            }

            if (seen) {
                continue;
            }

            char module[128];
            snprintf(module, sizeof(module), "%.*s", (int)named.length, named.data);

            char found[512];

            if (!gab_find_interface(&path, module, found, sizeof(found))) {
                diag_error(diagnostics, GAB_ERR_NAME, declaring->imports.data[i].span,
                           "no interface for '%s' on the search path", module);
                ok = false;
                break;
            }

            char *text = gab_interface_read(found);

            if (!text) {
                diag_error(diagnostics, GAB_ERR_NAME, declaring->imports.data[i].span,
                           "the interface for '%s' could not be read", module);
                ok = false;
                break;
            }

            ASTUnit *stated = ast_unit_create(arena);

            /* Read for what it imports, which the diagnostics of a failed parse have already named. */
            Diagnostics quiet;
            diagnostics_init(&quiet, arena, found);

            if (parse_unit(text, arena, &strings, &stated, &quiet)) {
                for (size_t j = 0; j < stated->imports.size; j++) {
                    ast_import_list_add(&reached, stated->imports.data[j]);
                }
            }

            diagnostics_free(&quiet);

            Scope *imported = arena_alloc(arena, sizeof(Scope));
            scope_init_module(imported, arena, &strings, scope);

            const char *one[1] = {text};

            ok = compile_unit(arena, &strings, imported, NULL, one, 1, true, NULL, diagnostics, NULL, 0, NULL,
                              NULL, NULL);

            if (ok) {
                if (i < declaring->imports.size) {
                    module_scope_map_insert(modules, string_from_cstr(&strings, module), imported);
                }

                char symbol[512];
                gab_interface_symbol(symbol, sizeof(symbol), module, gab_interface_digest(text));

                llvm_unit_requires(unit, symbol);

                /* The object beside it is what the link needs, which the caller could not have known. */
                if (request->resolved_count < 8) {
                    gab_object_beside(found, request->resolved[request->resolved_count],
                                      sizeof(request->resolved[0]));
                    request->resolved_count++;
                }
            }

            free(text);
        }
    }

    if (ok && !request->is_core) {
        ok = compile_unit(arena, &strings, scope, modules, request->sources, request->source_count, false,
                          unit, diagnostics, request->module_name, sizeof(request->module_name), &unit_ast,
                          request->names, declaring);
    }

    if (ok && request->interface) {
        ok = gab_interface_write(request->is_core ? core_ast : unit_ast, request->interface);

        if (!ok) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0, 0}, "could not write %s", request->interface);
        }

        /* The object states the interface just written, which its importers require by the same name. */
        char *written = ok ? gab_interface_read(request->interface) : NULL;

        if (written) {
            const ASTUnit *ast = request->is_core ? core_ast : unit_ast;

            char module[128];
            snprintf(module, sizeof(module), "%.*s", (int)ast->module_name.length, ast->module_name.data);

            char symbol[512];
            gab_interface_symbol(symbol, sizeof(symbol), module, gab_interface_digest(written));

            llvm_unit_declares(unit, symbol);

            free(written);
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
