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

    Type *player = type_registry_get(vm->global_scope.type_registry, string_from_cstr(&vm->strings, "Player"));
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

int main() {
    test_vm_execute();
    test_top_level_runs_as_frame_zero();
    test_two_vms_are_independent();
    test_empty_function_body();
    test_struct_typed_local();
    test_types_survive_a_later_compile();
    test_function_signatures_survive_a_later_compile();

    return 0;
}
