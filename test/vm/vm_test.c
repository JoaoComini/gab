#include "vm/vm.h"

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

int main() {
    test_vm_execute();
    test_two_vms_are_independent();
    test_empty_function_body();
    test_struct_typed_local();

    return 0;
}
