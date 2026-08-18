#include "vm/vm.h"

#include "string/string.h"
#include "symbol_table.h"
#include "type_registry.h"
#include "vm/opcode.h"

#include <assert.h>
#include <string.h>

static void test_vm_execute() {
    VM *vm = vm_create();

    vm_execute(vm, "func execute(): bool {\n"
                   "let a = 2;\n"
                   "let b = 3;\n"
                   "let c = a + b * 5;\n"
                   "let d = (c - a) * ((b + 4) / (a + 1));\n"
                   "let e = d + (c * (a - b) + (b / (a + 2)));\n"
                   "let f = e - ((d + c) * (b - a));\n"
                   "let g = ((f + e) * (d - c)) / ((a + b) - (e / (d + 1)));\n"
                   "let h = g + f - e * (d + c - (b * a));\n"
                   "let i = (h / g) + (f - (e * (d / (c + (b - a)))));\n"
                   "let result : int = ((i + h) * (g - f) + (e / d)) - ((c + b) * (a - 1));\n"
                   "let compare = result == 13120;\n"
                   "if compare { let a = 10; let b = 2; return a * b == 20; } else { return false; }\n"
                   "}");

    vm_free(vm);
}

// Two VMs must be fully independent. With a process-global string pool this was
// a use-after-free: freeing the first VM released every interned String,
// including the ones the second VM's types and symbols still pointed at.
static void test_two_vms_are_independent() {
    VM *first = vm_create();
    VM *second = vm_create();

    // Each VM interns its own copy of the same identifiers and type names.
    vm_execute(first, "func run(): int { let value : int = 1; return value; }");
    vm_execute(second, "func run(): int { let value : int = 2; return value; }");

    assert(&first->strings != &second->strings);

    Type *first_int = type_registry_get_builtin(first->global_scope.type_registry, TYPE_INT);
    Type *second_int = type_registry_get_builtin(second->global_scope.type_registry, TYPE_INT);

    // Same name, different pools: distinct objects that must not be shared.
    assert(first_int->name != second_int->name);

    // Freeing one VM must leave the other's strings intact.
    vm_free(first);

    assert(strcmp(second_int->name->data, "int") == 0);

    vm_execute(second, "func again(): int { return 3; }");

    vm_free(second);
}

// A body that emits no instructions still needs its implicit return; without it
// codegen read past the start of an empty instruction list.
static void test_empty_function_body() {
    VM *vm = vm_create();

    vm_execute(vm, "func main() { }");

    assert(vm->global_funcs.size == 1);

    FuncPrototype *proto = &vm->global_funcs.data[0];
    assert(proto->chunk->instructions.size == 1);
    assert(VM_DECODE_OPCODE(instruction_list_get(&proto->chunk->instructions, 0)) == OP_RETURN);

    vm_free(vm);
}

// Declaring a struct-typed local emits nothing, which is the same empty-chunk
// path reached from a different direction.
static void test_struct_typed_local() {
    VM *vm = vm_create();

    vm_execute(vm, "struct Vec3 { x: float, y: float, z: float }\n"
                   "func main() { let v: Vec3; }");

    assert(vm->global_funcs.size == 1);

    vm_free(vm);
}

// The top level runs as frame zero, so execution leaves no frame behind and the
// VM is reusable for a second script.
static void test_top_level_runs_as_frame_zero() {
    VM *vm = vm_create();

    vm_execute(vm, "func main() { let a = 2; }");
    assert(vm->frame_count == 0);

    vm_execute(vm, "let x: int = 7;");
    assert(vm->frame_count == 0);

    vm_free(vm);
}

// A struct type registered by one compile is reachable from the TypeRegistry,
// which outlives every compile — so it must not be allocated from the arena a
// compile resets. arena_reset only rewinds each block, leaving the memory
// mapped, so a stale type reads fine until a later compile reuses those bytes.
// That makes this the shape of the failure rather than an immediate crash.
static void test_types_survive_a_later_compile() {
    VM *vm = vm_create();

    vm_execute(vm, "struct Player { health: int, mana: int }\n");

    Type *player = scope_type_lookup(&vm->global_scope, string_from_cstr(&vm->strings, "Player"));
    assert(player);

    size_t size = player->size;
    size_t field_count = player->field_count;

    // Enough of a second script to reuse the memory the first one released.
    vm_execute(vm, "func a(x: int, y: int): int { let q: int = x + y; let w: int = q * q; return w; }\n"
                   "func b(x: int, y: int): int { let q: int = x - y; let w: int = q * q; return w; }\n");

    assert(strcmp(player->name->data, "Player") == 0);
    assert(player->size == size);
    assert(player->field_count == field_count);

    vm_free(vm);
}

// Same rule for a function's signature: the Symbol lives in the global scope, so
// its parameter array has to live at least as long. This is what a host reads
// when it resolves a function once and calls it every frame.
static void test_function_signatures_survive_a_later_compile() {
    VM *vm = vm_create();

    vm_execute(vm, "struct Player { health: int, mana: int }\n"
                   "func on_update(p: Player, dt: float): int { return p.health; }\n");

    Symbol *on_update = scope_symbol_lookup(&vm->global_scope, string_from_cstr(&vm->strings, "on_update"));
    assert(on_update && on_update->kind == SYMBOL_FUNC);
    assert(on_update->func.param_count == 2);

    vm_execute(vm, "func a(x: int, y: int): int { let q: int = x + y; let w: int = q * q; return w; }\n"
                   "func b(x: int, y: int): int { let q: int = x - y; let w: int = q * q; return w; }\n");

    assert(on_update->func.param_count == 2);
    assert(strcmp(on_update->func.params[0]->name->data, "Player") == 0);
    assert(strcmp(on_update->func.params[1]->name->data, "float") == 0);
    assert(strcmp(on_update->func.return_type->name->data, "int") == 0);

    vm_free(vm);
}

// The whole point of separating compilation from execution: an engine compiles
// a script at load time and runs it every frame, without re-parsing.
static void test_compile_once_run_many() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    bool ok = vm_compile(vm, "func seven(): int { return 7; }\nlet r: int = seven();\n", &script, &diagnostics);
    assert(ok);

    diagnostics_free(&diagnostics);

    // Running repeatedly must be safe and must give the same answer each time:
    // a run leaves no frame behind and does not consume the chunk.
    for (int i = 0; i < 3; i++) {
        vm_run(vm, &script);

        assert(vm->frame_count == 0);
        assert(vm_slot(vm, 0)->as_int == 7);
    }

    vm_compiled_script_free(&script);

    vm_free(vm);
}

// A failed compile reports through the diagnostics it was given rather than
// printing, and hands back nothing to run.
static void test_compile_failure_is_reportable() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    bool ok = vm_compile(vm, "func broken(: int { return", &script, &diagnostics);

    assert(!ok);
    assert(diagnostics_has_errors(&diagnostics));

    diagnostics_free(&diagnostics);
    vm_free(vm);
}

// Compiles a unit, asserting it succeeded, and discards the chunk. Most module
// tests care only about whether resolution accepted the source.
static bool compile_ok(VM *vm, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    bool ok = vm_compile(vm, source, &script, &diagnostics);

    diagnostics_free(&diagnostics);

    if (ok) {
        vm_compiled_script_free(&script);
    }

    return ok;
}

// The reason modules exist: one script per entity type, each with its own
// on_update. Before per-module scopes this was a hard compile error.
static void test_modules_isolate_declarations() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "module Player;\nfunc on_update(): int { return 1; }\n"));
    assert(compile_ok(vm, "module Enemy;\nfunc on_update(): int { return 2; }\n"));

    vm_free(vm);
}

// A module spans compiles: a later unit naming the same module resolves against
// what earlier ones declared. The reverse does not hold, which is what makes
// compile order the host's lever rather than a hidden dependency.
static void test_modules_accumulate_across_units() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "module Player;\nfunc helper(): int { return 7; }\n"));
    assert(compile_ok(vm, "module Player;\nfunc uses(): int { return helper(); }\n"));

    // A different module cannot see either of them.
    assert(!compile_ok(vm, "module Enemy;\nfunc bad(): int { return helper(); }\n"));

    vm_free(vm);
}

// A unit that names no module declares into the root namespace, which is what
// keeps every script written before modules existed compiling unchanged.
static void test_root_namespace_and_modules() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "func shared(): int { return 7; }\n"));

    // A module parents to the root, so builtins and root declarations resolve
    // through it.
    assert(compile_ok(vm, "module M;\nfunc g(a: int, b: float): int { return shared() + a; }\n"));

    // The root does not see into a module: visibility goes one way only.
    assert(!compile_ok(vm, "func h(): int { return g(); }\n"));

    vm_free(vm);
}

// A module scope parents to the root for lookup but stays at depth 0, because
// depth drives the pointer-lifetime rule. At depth 1 a top-level variable would
// look like a block local, and taking its address would start being rejected.
static void test_module_scope_does_not_change_pointer_lifetimes() {
    VM *vm = vm_create();

    const char *takes_address = "func f(): int {\n"
                                "  let x: int = 1;\n"
                                "  let p: *int = &x;\n"
                                "  return *p;\n"
                                "}\n";

    const char *escapes = "func g(): int {\n"
                          "  let outer: *int;\n"
                          "  { let inner: int = 1; outer = &inner; }\n"
                          "  return 0;\n"
                          "}\n";

    char buffer[512];

    assert(compile_ok(vm, takes_address));
    snprintf(buffer, sizeof(buffer), "module A;\n%s", takes_address);
    assert(compile_ok(vm, buffer));

    // And the rule still bites inside a module.
    assert(!compile_ok(vm, escapes));
    snprintf(buffer, sizeof(buffer), "module B;\n%s", escapes);
    assert(!compile_ok(vm, buffer));

    vm_free(vm);
}

int main() {
    test_modules_isolate_declarations();
    test_modules_accumulate_across_units();
    test_root_namespace_and_modules();
    test_module_scope_does_not_change_pointer_lifetimes();

    test_vm_execute();
    test_top_level_runs_as_frame_zero();
    test_two_vms_are_independent();
    test_empty_function_body();
    test_struct_typed_local();
    test_types_survive_a_later_compile();
    test_function_signatures_survive_a_later_compile();
    test_compile_once_run_many();
    test_compile_failure_is_reportable();

    return 0;
}
