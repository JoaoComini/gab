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

/* What every stage of one compilation shares: the storage it allocates from, the scope its
 * declarations land in, and the generics a later module instantiates from. */
typedef struct {
    Arena *arena;
    StringPool *strings;

    Scope *global;
    ModuleScopeMap *modules;

    MIRModule *generics;

    Diagnostics *diagnostics;
} Compilation;

/* A generic's body is what a reader instantiates, so it is kept where every later module finds it. */
static void keep_templates(const Compilation *compilation, const MIRModule *bodies) {
    for (size_t i = 0; i < bodies->entries.size; i++) {
        MIRFunction *ir = bodies->entries.data[i].ir;

        if (ir && mir_function_is_template(ir)) {
            mir_module_add(compilation->generics, bodies->entries.data[i].function, ir);
        }
    }
}

/* What an interface declares, resolved into a scope of its own so a symbol keeps the module that
 * defines it. Nothing is emitted: the bodies live in the object beside it. */
static bool compile_declarations(const Compilation *compilation, const char *text, bool is_prelude,
                                 Scope **into) {
    ASTModule *module = NULL;

    const char *sources[1] = {text};

    if (!parse_module(sources, 1, NULL, compilation->arena, compilation->strings, &module,
                      compilation->diagnostics)) {
        return false;
    }

    ResolvedModule *resolved = NULL;

    /* An interface restates the declarations it was written from, intrinsics included, so re-reading
     * one declares what its source was allowed to. It names the prelude as its source did, so the
     * scopes are what a '@caller()' in it resolves 'Location' through. */
    ModulePrivileges privileges = {.intrinsics = true, .global = is_prelude};

    if (!resolve_module(compilation->arena, module, compilation->global, compilation->modules, privileges,
                        &resolved, compilation->diagnostics)) {
        return false;
    }

    *into = resolved->scope;

    MIRModule *bodies = NULL;

    if (!mir_build(compilation->arena, resolved, compilation->generics, &bodies, compilation->diagnostics)) {
        return false;
    }

    keep_templates(compilation, bodies);

    return true;
}

/* This module's source, resolved and lowered into 'out'. Only source someone wrote is held to what a
 * program may declare; 'declares_intrinsics' is what the prelude is granted. What the interface states
 * a body as is what was written, which only resolution's facts recover, so they are left in 'facts'. */
static bool compile_module(const Compilation *compilation, ASTModule *module, ModulePrivileges privileges,
                           LLVMUnit *out, const Facts **facts) {
    ResolvedModule *resolved = NULL;

    if (!resolve_module(compilation->arena, module, compilation->global, compilation->modules, privileges,
                        &resolved, compilation->diagnostics)) {
        return false;
    }

    *facts = &resolved->facts;

    MIRModule *bodies = NULL;

    if (!mir_build(compilation->arena, resolved, compilation->generics, &bodies, compilation->diagnostics)) {
        return false;
    }

    keep_templates(compilation, bodies);

    for (size_t i = 0; i < bodies->entries.size; i++) {
        MIRFunction *ir = bodies->entries.data[i].ir;

        /* A generic's own body stands for its instances, which are emitted in its place. */
        if (!ir || mir_function_is_template(ir)) {
            continue;
        }

        mir_fold(compilation->arena, ir);
        mir_drop_elaborate(compilation->arena, compilation->global->type_registry,
                           compilation->global->functions, ir);

        llvm_unit_add(out, ir);
    }

    return true;
}

bool gab_compile(const GabCompile *request, GabCompiled *out, Diagnostics *diagnostics) {
    Arena *arena = arena_create(GAB_COMPILE_BLOCK_SIZE);

    StringPool strings;
    string_pool_init(&strings, arena);

    Scope *scope = scope_create(arena, &strings, NULL);

    LLVMUnit *unit = llvm_unit_open(arena);

    char *interface = NULL;

    /* The prelude is a module like any other; this compilation reads its declarations unless it is
     * the one writing them. */
    bool declares_prelude = request->declares_intrinsics;

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

    Compilation compilation = {
        .arena = arena,
        .strings = &strings,
        .global = scope,
        .modules = modules,

        /* Every generic the prelude and the imports declare, which this module instantiates from
         * rather than links. */
        .generics = mir_module_create(arena),

        .diagnostics = diagnostics,
    };

    ASTModule *declaring = NULL;

    bool ok = true;

    /* The prelude declares into the global scope, so what it states is reached the way a primitive's
     * name is: by an ordinary walk, without an import and without a scope of its own. */
    if (!declares_prelude) {
        Scope *prelude = NULL;

        ok = compile_declarations(&compilation, interface, true, &prelude);
    }

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

            /* The prelude declares into the global scope, so importing it would declare it twice. */
            if (string_ref_equals_cstr(named, GAB_CORE_MODULE)) {
                diag_error(diagnostics, GAB_ERR_NAME, reached.data[i].span,
                           "'%s' is what every module names without importing it", GAB_CORE_MODULE);
                ok = false;
                break;
            }

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

            Scope *imported = NULL;

            ok = compile_declarations(&compilation, text, false, &imported);

            if (ok) {
                if (i < direct) {
                    module_scope_map_insert(modules, string_from_cstr(&strings, module), imported);
                }

                char symbol[512];
                gab_interface_symbol(symbol, sizeof(symbol), module, gab_interface_digest(text));

                llvm_unit_requires(unit, symbol);

                /* The object beside it is what the link needs, which the caller could not have known. */
                if (out->resolved.count < out->resolved.capacity) {
                    gab_object_beside(found, out->resolved.objects[out->resolved.count],
                                      sizeof(out->resolved.objects[0]));
                    out->resolved.count++;
                } else {
                    diag_error(diagnostics, GAB_ERR_NAME, reached.data[i].span,
                               "'%s' is more than the %zu imports this compilation can link", module,
                               out->resolved.capacity);
                    ok = false;
                }
            }

            /* What was parsed from this text names it still: an import's own imports were added to the
             * list, and the names they carry point into it rather than copying out of it. */
        }
    }

    const Facts *facts = NULL;

    if (ok) {
        ModulePrivileges privileges = {.intrinsics = request->declares_intrinsics,
                                       .global = request->declares_intrinsics};

        ok = compile_module(&compilation, declaring, privileges, unit, &facts);
    }

    if (ok) {
        snprintf(out->module_name, sizeof(out->module_name), "%.*s", (int)declaring->name.length,
                 declaring->name.data);
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
