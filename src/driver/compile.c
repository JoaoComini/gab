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

typedef struct {
    Resolver resolver;

    MIRModule *generics;
} Compilation;

static void keep_templates(const Compilation *compilation, const MIRModule *bodies) {
    for (size_t i = 0; i < bodies->entries.size; i++) {
        MIRFunction *ir = bodies->entries.data[i].ir;

        if (ir && mir_function_is_template(ir)) {
            mir_module_add(compilation->generics, bodies->entries.data[i].function, ir);
        }
    }
}

static bool compile_declarations(const Compilation *compilation, const char *text, Module **into) {
    ASTModule *module = NULL;

    const char *sources[1] = {text};

    if (!parse_module(sources, 1, NULL, compilation->resolver.arena, compilation->resolver.strings, &module,
                      compilation->resolver.diagnostics)) {
        return false;
    }

    ResolvedModule *resolved = NULL;

    ModulePrivileges privileges = {.intrinsics = true};

    Scope *into_scope =
        scope_create_kind(compilation->resolver.arena, compilation->resolver.global, SCOPE_MODULE);

    if (!resolve_module(&compilation->resolver, module, into_scope, privileges, &resolved)) {
        return false;
    }

    *into = resolved->declared;

    MIRModule *bodies = NULL;

    if (!mir_build(compilation->resolver.arena, resolved, compilation->generics, &bodies,
                   compilation->resolver.diagnostics)) {
        return false;
    }

    keep_templates(compilation, bodies);

    return true;
}

static bool compile_module(const Compilation *compilation, ASTModule *module, Scope *into,
                           ModulePrivileges privileges, LLVMUnit *out, const Facts **facts) {
    ResolvedModule *resolved = NULL;

    if (!resolve_module(&compilation->resolver, module, into, privileges, &resolved)) {
        return false;
    }

    *facts = &resolved->facts;

    MIRModule *bodies = NULL;

    if (!mir_build(compilation->resolver.arena, resolved, compilation->generics, &bodies,
                   compilation->resolver.diagnostics)) {
        return false;
    }

    keep_templates(compilation, bodies);

    for (size_t i = 0; i < bodies->entries.size; i++) {
        MIRFunction *ir = bodies->entries.data[i].ir;

        if (!ir || mir_function_is_template(ir)) {
            continue;
        }

        mir_fold(compilation->resolver.arena, ir);
        mir_drop_elaborate(compilation->resolver.arena, compilation->resolver.types,
                           compilation->resolver.functions, ir);

        llvm_unit_add(out, ir);
    }

    return true;
}

bool gab_module_name(const char *source, Arena *arena, StringPool *strings, char *out, size_t capacity,
                     Diagnostics *diagnostics) {
    ASTFile *file = NULL;

    if (!parse_header(source, arena, strings, &file, diagnostics)) {
        return false;
    }

    snprintf(out, capacity, "%s", file->module_name->name->data);

    return true;
}

bool gab_compile(const GabCompile *request, GabCompiled *out, Diagnostics *diagnostics) {
    Arena *arena = arena_create(GAB_COMPILE_BLOCK_SIZE);

    StringPool strings;
    string_pool_init(&strings, arena);

    const KnownNames names = known_names(&strings);

    TypeRegistry *types = type_registry_create(arena, &names);
    FunctionRegistry *functions = function_registry_create(arena, types);

    Scope *scope = global_scope_create(arena, types);

    LLVMUnit *unit = llvm_unit_open(arena);

    bool declares_core = request->writes_core;

    ModuleMap *modules = module_map_create_alloc(arena_allocator(arena), 8);

    Compilation compilation = {
        .resolver =
            {
                .arena = arena,
                .strings = &strings,
                .types = types,
                .functions = functions,
                .global = scope,
                .modules = modules,
                .diagnostics = diagnostics,
            },

        .generics = mir_module_create(arena),
    };

    ASTModule *declaring = NULL;

    bool ok = true;

    if (ok && !parse_module(request->sources, request->source_count, request->names, arena, &strings,
                            &declaring, diagnostics)) {
        ok = false;
    }

    for (size_t i = 0; ok && i < request->dependency_count; i++) {
        const GabDependency *dependency = &request->dependencies[i];

        Module *imported = NULL;

        ok = compile_declarations(&compilation, dependency->text, &imported);

        if (!ok) {
            break;
        }

        if (dependency->direct) {
            module_map_insert(modules, string_from_cstr(&strings, dependency->name), imported);
        }

        if (strcmp(dependency->name, GAB_CORE_MODULE) == 0) {
            compilation.resolver.core = imported;
        }

        char symbol[512];
        gab_interface_symbol(symbol, sizeof(symbol), dependency->name,
                             gab_interface_digest(dependency->text));

        llvm_unit_requires(unit, symbol);
    }

    const Facts *facts = NULL;

    if (ok) {
        ModulePrivileges privileges = {.intrinsics = declares_core};

        Scope *into = scope_create_kind(arena, scope, SCOPE_MODULE);

        ok = compile_module(&compilation, declaring, into, privileges, unit, &facts);
    }

    if (ok) {
        snprintf(out->module_name, sizeof(out->module_name), "%s", declaring->name->name->data);
    }

    if (ok && request->interface) {
        ok = gab_interface_write(declaring, facts, request->interface);

        if (!ok) {
            diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0, 0}, "could not write %s", request->interface);
        }

        char *written = ok ? gab_interface_read(request->interface) : NULL;

        if (written) {
            char module[128];
            snprintf(module, sizeof(module), "%s", declaring->name->name->data);

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

    string_pool_free(&strings);
    arena_destroy(arena);

    return ok;
}
