// What the language is written in terms of, and what a standard library adds to
// it. A primitive is named by a compile that never had a VM; a registered type
// is not, because nothing registered it.

#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>

// A primitive is the resolver's own: a literal produces one, an operator answers
// one, and a spec spells one without anything having been registered.
//
// Types only. What may be called on one is a method, and a method is numbered in
// a VM's extern table however primitive its receiver.
static void test_a_primitive_needs_no_library() {
    assert(test_compiles("func f(): int { let n: int = 1 + 2; return n; }\n"));
    assert(test_compiles("func f(): float { let x: float = 1.5; return x; }\n"));
    assert(test_compiles("func f(): bool { let b: bool = true; return b; }\n"));
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));
    assert(test_compiles("func f(): int { let a: [int; 2]; return 0; }\n"));
}

// A registered type is the library's. Naming one without the VM that provides
// it fails to resolve, which is what keeps the two apart rather than a
// convention that they are.
static void test_a_registered_type_needs_the_library() {
    assert(!test_compiles("func f(): int { let s: String; return 0; }\n"));
    assert(!test_compiles("func f(): int { let v: Vec<int>; return 0; }\n"));

    assert(test_compiles_on_vm("func f(): int { let s: String; return 0; }\n"));
    assert(test_compiles_on_vm("func f(): int { let v: Vec<int>; return 0; }\n"));
}

// Types are interned per registry, and a registry belongs to a VM. So what a
// library declares is that VM's: two of them each get their own 'String',
// deriving to their own 'str'.
static void test_each_vm_declares_its_own() {
    VM *first = vm_create();
    VM *second = vm_create();

    const Type *here =
        scope_type_lookup(&first->env.global_scope, string_from_cstr(&first->env.strings, "String"));
    const Type *there =
        scope_type_lookup(&second->env.global_scope, string_from_cstr(&second->env.strings, "String"));

    assert(here && there);

    // Never shared. Types compare by pointer identity, so one VM's 'String'
    // standing in for another's would silently answer for the wrong registry.
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
    test_each_vm_declares_its_own();

    return 0;
}
