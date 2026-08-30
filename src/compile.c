#include "compile.h"

#include "arena.h"
#include "ast/resolve.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/interp.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static bool module_imports_module(const VM *vm, const String *module, const String *other) {
    for (size_t i = 0; i < vm->env.module_imports.size; i++) {
        const ModuleImport *edge = &vm->env.module_imports.data[i];

        if (edge->from == module && edge->to == other) {
            return true;
        }
    }

    return false;
}

static bool check_imports(VM *vm, const ASTUnit *ast, Diagnostics *diagnostics) {
    String *self = string_from_ref(&vm->env.strings, ast->module_name);
    bool ok = true;

    for (size_t i = 0; i < ast->imports.size; i++) {
        const ASTImport *import = &ast->imports.data[i];
        String *name = string_from_ref(&vm->env.strings, import->name);

        if (name == self) {
            diag_error(diagnostics, GAB_ERR_NAME, import->span,
                       "a unit needs no import to name its own module");
            ok = false;
            continue;
        }

        if (!module_scope_map_lookup(vm->env.module_scopes, name)) {
            diag_error(diagnostics, GAB_ERR_NAME, import->span, "no module '%s' is loaded", name->data);
            ok = false;
            continue;
        }

        if (module_imports_module(vm, name, self)) {
            diag_error(diagnostics, GAB_ERR_NAME, import->span,
                       "'%s' already imports this module, and two modules cannot import each other",
                       name->data);
            ok = false;
        }
    }

    return ok;
}

bool compile_unit(VM *vm, const char *source, FuncPrototype *out, Diagnostics *diagnostics) {
    arena_reset(vm->env.compile_arena);

    Lexer lexer = lexer_create(source, vm->env.compile_arena, &vm->env.strings, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTUnit *ast = ast_unit_create();

    Unit *unit = NULL;
    String *module_name = NULL;
    Scope *target = NULL;
    Scope *staging = NULL;

    StringList imported = string_list_create();

    if (parser_parse(&parser, ast) && check_imports(vm, ast, diagnostics)) {
        module_name = string_from_ref(&vm->env.strings, ast->module_name);
        target = environment_module_scope(&vm->env, module_name);

        staging = arena_alloc(vm->env.compile_arena, sizeof(Scope));
        scope_init_staging(staging, target->arena, &vm->env.strings, target);

        for (size_t i = 0; i < ast->imports.size; i++) {
            string_list_add(&imported, string_from_ref(&vm->env.strings, ast->imports.data[i].name));
        }

        if (resolve_unit(vm->env.compile_arena, ast, staging, vm->env.module_scopes, diagnostics)) {
            unit =
                codegen_generate(ast, vm->env.arena, &vm->env.strings, staging->type_registry, diagnostics);
        }
    }

    ast_unit_destroy(ast);

    if (!unit) {
        return false;
    }

    if (!link_check(&vm->program, unit, diagnostics)) {
        unit_free(unit);
        return false;
    }

    link_install(&vm->program, unit);
    scope_merge_staged(target, staging);

    for (size_t i = 0; i < imported.size; i++) {
        module_import_list_add(&vm->env.module_imports,
                               (ModuleImport){.from = module_name, .to = imported.data[i]});
    }

    string_list_free(&imported);

    *out = unit->top_level;

    unit->top_level.chunk = NULL;
    unit->top_level.refs = frame_ref_list_create();
    unit->prototypes.size = 0;

    unit_free(unit);

    return true;
}

void compile_and_run(VM *vm, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<script>");

    FuncPrototype top_level;

    if (!compile_unit(vm, source, &top_level, &diagnostics)) {
        diagnostics_print(&diagnostics, stderr);
        diagnostics_free(&diagnostics);
        return;
    }

    diagnostics_free(&diagnostics);

    if (interp_run_top_level(vm, &top_level) != VM_RUN_OK) {
        fprintf(stderr, "<script>: %s\n", vm->error.message);
    }

    func_proto_free(&top_level);
}
