#include "scope.h"
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>

static void test_a_primitive_needs_no_library() {
    assert(test_compiles("func f(): int { let n: int = 1 + 2; return n; }\n"));
    assert(test_compiles("func f(): float { let x: float = 1.5; return x; }\n"));
    assert(test_compiles("func f(): bool { let b: bool = true; return b; }\n"));
    assert(test_compiles("func f(): int { let s: &str = \"hi\"; return 0; }\n"));
    assert(test_compiles("func f(): int { let a: [int; 2]; return 0; }\n"));
}

static void test_a_registered_type_needs_the_library() {
    assert(!test_compiles("import std;\n"
                          "func f(): int { let s: String = String::from(\"\"); return 0; }\n"));
    assert(!test_compiles("import std;\n"
                          "func f(): int { let v: Vec<int> = Vec<int>::new(0); return 0; }\n"));
}

static void test_a_library_type_needs_its_import() {
    assert(!test_compiles_on_vm("func f(): int { let s: String = String::from(\"\"); return 0; }\n"));
    assert(!test_compiles_on_vm("func f(): int { let v: Vec<int> = Vec<int>::new(0); return 0; }\n"));

    assert(test_compiles_on_vm("import std;\n"
                               "func f(): int { let s: String = String::from(\"\"); return 0; }\n"));
    assert(test_compiles_on_vm("import std;\n"
                               "func f(): int { let v: Vec<int> = Vec<int>::new(0); return 0; }\n"));
}

static void test_each_vm_declares_its_own() {
    VM *first = vm_create();
    VM *second = vm_create();

    const Type *here = scope_type_lookup(
        environment_module_scope(&first->env, string_from_cstr(&first->env.strings, GAB_STD_MODULE)),
        string_from_cstr(&first->env.strings, "String"));
    const Type *there = scope_type_lookup(
        environment_module_scope(&second->env, string_from_cstr(&second->env.strings, GAB_STD_MODULE)),
        string_from_cstr(&second->env.strings, "String"));

    assert(here && there);

    assert(here != there);

    assert(type_registry_deref_of(first->env.global_scope.type_registry, here) ==
           type_registry_get_primitive(first->env.global_scope.type_registry, TYPE_STR));
    assert(type_registry_deref_of(second->env.global_scope.type_registry, there) ==
           type_registry_get_primitive(second->env.global_scope.type_registry, TYPE_STR));

    vm_free(first);
    vm_free(second);
}

int main(void) {
    test_a_primitive_needs_no_library();
    test_a_registered_type_needs_the_library();
    test_a_library_type_needs_its_import();
    test_each_vm_declares_its_own();

    return 0;
}
