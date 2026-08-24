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

// Whether 'module' imports 'other', directly. One hop is enough: a cycle is
// refused as it forms, so no chain of imports can already contain one.
static bool module_imports_module(const VM *vm, const String *module, const String *other) {
    for (size_t i = 0; i < vm->env.module_imports.size; i++) {
        const ModuleImport *edge = &vm->env.module_imports.data[i];

        if (edge->from == module && edge->to == other) {
            return true;
        }
    }

    return false;
}

// Whether every module this unit imported is one the VM has. Asked before the
// unit resolves, so a missing module is reported once rather than as one
// unknown type per mention of it.
//
// A cycle is refused here too: a module already importing this one cannot also
// be imported by it, because linking installs a unit whole and two units that
// each need the other have no order in which that is possible.
static bool check_imports(VM *vm, const ASTScript *script, Diagnostics *diagnostics) {
    String *self = string_from_ref(&vm->env.strings, script->module_name);
    bool ok = true;

    for (size_t i = 0; i < script->imports.size; i++) {
        const ASTImport *import = &script->imports.data[i];
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
    // Reclaimed at the start of a compile rather than the end of one, so
    // everything a compile produced — diagnostics included — stays readable
    // until the next compile begins.
    arena_reset(vm->env.compile_arena);

    Lexer lexer = lexer_create(source, vm->env.compile_arena, &vm->env.strings, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTScript *script = ast_script_create();

    // Each stage is a precondition for the next: a failure must stop the
    // pipeline rather than let a malformed AST reach codegen.
    Unit *unit = NULL;
    String *module_name = NULL;
    Scope *target = NULL;
    Scope *staging = NULL;

    // Interned before the AST goes, since the edges are recorded only once the
    // unit has linked and the StringRefs point into the source by then.
    StringList imported = string_list_create();

    if (parser_parse(&parser, script) && check_imports(vm, script, diagnostics)) {
        module_name = string_from_ref(&vm->env.strings, script->module_name);
        target = environment_module_scope(&vm->env, module_name);

        // Declared into a scope of its own, merged into the target only once the
        // whole compile has succeeded: a name is declared once and never
        // replaced, so a compile that fails partway must leave nothing behind
        // for the retry to collide with.
        //
        // Allocated from the target's arena rather than the compile arena, since
        // what a unit declares outlives the compile that declared it. Only the
        // staging scope's own struct is short-lived.
        staging = arena_alloc(vm->env.compile_arena, sizeof(Scope));
        scope_init_staging(staging, target->arena, &vm->env.strings, target);

        for (size_t i = 0; i < script->imports.size; i++) {
            string_list_add(&imported, string_from_ref(&vm->env.strings, script->imports.data[i].name));
        }

        if (ast_script_resolve(vm->env.compile_arena, script, staging, vm->env.module_scopes, diagnostics)) {
            unit = codegen_generate(script, vm->env.arena, &vm->env.strings, diagnostics);
        }
    }

    // Nothing reads the AST once codegen has run, so the compile owns it end to
    // end and only the unit outlives this call.
    ast_script_destroy(script);

    if (!unit) {
        return false;
    }

    if (!link_check(&vm->program, unit, diagnostics)) {
        unit_free(unit);
        return false;
    }

    // Both installs, once neither can refuse.
    link_install(&vm->program, unit);
    scope_merge_staged(target, staging);

    // Recorded only now: an edge from a unit that did not load would refuse an
    // import that should be allowed.
    for (size_t i = 0; i < imported.size; i++) {
        module_import_list_add(&vm->env.module_imports,
                               (ModuleImport){.from = module_name, .to = imported.data[i]});
    }

    string_list_free(&imported);

    // Linking took the prototypes and types; what is left is the top-level
    // frame, which belongs to the caller.
    *out = unit->top_level;

    // Cleared so freeing the unit does not take what the caller now owns.
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

    // The convenience path is the one caller that still reports for itself; a
    // host uses gab_module_run and gets the status instead.
    if (interp_run_top_level(vm, &top_level) != VM_RUN_OK) {
        fprintf(stderr, "<script>: %s\n", vm->error.message);
    }

    func_proto_free(&top_level);
}
