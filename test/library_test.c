// What the language is written in terms of, and what a standard library adds to
// it. A primitive is named by a compile that never had a VM; a registered type
// is not, because nothing registered it.

#include "support/run.h"

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

int main(void) {
    test_a_primitive_needs_no_library();
    test_a_registered_type_needs_the_library();

    return 0;
}
