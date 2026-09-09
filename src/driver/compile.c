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

static bool compile_unit(Arena *arena, StringPool *strings, Scope *scope, ModuleScopeMap *modules,
                         const char *const *sources, size_t source_count, bool allow_primitive_impls,
                         LLVMUnit *out, Diagnostics *diagnostics, char *module_name, size_t module_capacity,
                         ASTModule **out_ast, const char *const *names, ASTModule *parsed,
                         MIRModule *generics, const Facts **out_facts) {
    ASTModule *ast = parsed;

    if (!ast && !parse_module(sources, source_count, names, arena, strings, &ast, diagnostics)) {
        return false;
    }

    if (out_ast) {
        *out_ast = ast;
    }

    if (module_name) {
        snprintf(module_name, module_capacity, "%.*s", (int)ast->name.length, ast->name.data);
    }

    ResolvedModule *resolved = NULL;

    if (!resolve_module(arena, ast, scope, modules, allow_primitive_impls, &resolved, diagnostics)) {
        return false;
    }

    /* What the interface states a body as is what was written, which only resolution's facts recover. */
    if (out_facts) {
        *out_facts = &resolved->facts;
    }

    MIRModule *bodies = NULL;

    if (!mir_build(arena, resolved, generics, &bodies, diagnostics)) {
        return false;
    }

    /* A generic's body is what a reader instantiates, so it is kept where every later unit can find it. */
    if (generics) {
        for (size_t i = 0; i < bodies->entries.size; i++) {
            MIRFunction *ir = bodies->entries.data[i].ir;

            if (ir && mir_function_is_template(ir)) {
                mir_module_add(generics, bodies->entries.data[i].function, ir);
            }
        }
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
        mir_drop_elaborate(arena, scope->type_registry, scope->functions, ir);

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

    const Facts *facts = NULL;

    /* The prelude is a module like any other; this compilation reads its declarations unless it is
     * the one writing them. */
    bool declares_prelude = request->allow_primitive_impls;

    if (!declares_prelude) {
        /* A program reads what the prelude declares, never the source those declarations came from. */
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

    /* Every generic the prelude and the imports declare, which this module instantiates from rather
     * than links. */
    MIRModule *generics = mir_module_create(arena);

    ASTModule *declaring = NULL;

    bool ok = true;

    if (!declares_prelude) {
        const char *sources[1] = {interface};

        ok = compile_unit(arena, &strings, scope, NULL, sources, 1, true, NULL, diagnostics, NULL, 0, NULL,
                          NULL, NULL, generics, NULL);
    }

    module_scope_map_insert(modules, string_from_cstr(&strings, GAB_CORE_MODULE), scope);

    /* Each import is its own compilation: its declarations land in a scope of their own, which this
     * module then names, so a symbol keeps the module that defines it rather than taking this one's. */
    if (ok && !parse_module(request->sources, request->source_count, request->names, arena, &strings,
                            &declaring, diagnostics)) {
        ok = false;
    }

    if (ok) {
        GabSearchPath path = {.directories = request->search,
                              .count = request->search_count,
                              .source_directory = request->source_directory};

        /* The list grows as interfaces are read: an import's imports are linked too, though nothing
         * here names them. */
        ASTImportList reached = ast_import_list_create(arena_allocator(arena));

        for (size_t f = 0; f < declaring->files.size; f++) {
            const ASTImportList *written = &declaring->files.data[f]->imports;

            for (size_t i = 0; i < written->size; i++) {
                ast_import_list_add(&reached, written->data[i]);
            }
        }

        /* What the module itself imports, ahead of what reading those interfaces appended: only a
         * direct import is a scope this unit may name. */
        size_t direct = reached.size;

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
                diag_error(diagnostics, GAB_ERR_NAME, reached.data[i].span,
                           "no interface for '%s' on the search path", module);
                ok = false;
                break;
            }

            char *read = gab_interface_read(found);

            /* Held for as long as the names parsed out of it, which point into the text rather than
             * copying out of it: an import's own imports join the list this loop is still walking. */
            char *text = NULL;

            if (read) {
                size_t length = strlen(read);

                text = arena_alloc(arena, length + 1);
                memcpy(text, read, length + 1);

                free(read);
            }

            if (!text) {
                diag_error(diagnostics, GAB_ERR_NAME, reached.data[i].span,
                           "the interface for '%s' could not be read", module);
                ok = false;
                break;
            }

            ASTFile *stated = NULL;

            /* Read for what it imports, which the diagnostics of a failed parse have already named. */
            Diagnostics quiet;
            diagnostics_init(&quiet, arena, found);

            if (parse_file(text, arena, &strings, &stated, &quiet)) {
                for (size_t j = 0; j < stated->imports.size; j++) {
                    ast_import_list_add(&reached, stated->imports.data[j]);
                }
            }

            diagnostics_free(&quiet);

            Scope *imported = arena_alloc(arena, sizeof(Scope));
            scope_init_module(imported, arena, &strings, scope);

            const char *one[1] = {text};

            ok = compile_unit(arena, &strings, imported, NULL, one, 1, true, NULL, diagnostics, NULL, 0, NULL,
                              NULL, NULL, generics, NULL);

            if (ok) {
                if (i < direct) {
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

            /* What was parsed from this text names it still: an import's own imports were added to the
             * list, and the names they carry point into it rather than copying out of it. */
        }
    }

    if (ok) {
        ok = compile_unit(arena, &strings, scope, modules, request->sources, request->source_count,
                          request->allow_primitive_impls, unit, diagnostics, request->module_name,
                          sizeof(request->module_name), NULL, request->names, declaring, generics, &facts);
    }

    if (ok && request->interface) {
        ok = gab_interface_write(declaring, facts, request->interface);

        if (!ok) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0, 0}, "could not write %s", request->interface);
        }

        /* The object states the interface just written, which its importers require by the same name. */
        char *written = ok ? gab_interface_read(request->interface) : NULL;

        if (written) {
            char module[128];
            snprintf(module, sizeof(module), "%.*s", (int)declaring->name.length, declaring->name.data);

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
