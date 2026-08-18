#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>

// Runs a script and returns whatever ended up in r0, which is where a
// top-level return leaves its result.
static int run_int(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    int result = (*vm_slot(vm, 0)).as_int;

    vm_free(vm);

    return result;
}

static void test_simple_call() {
    assert(run_int("func add(a: int, b: int): int { return a + b; }\n"
                   "func main(): int { return add(2, 3); }\n"
                   "let r: int = main();") == 5);
}

static void test_call_with_no_arguments() {
    assert(run_int("func answer(): int { return 42; }\n"
                   "func main(): int { return answer(); }\n"
                   "let r: int = main();") == 42);
}

// An argument that is itself a call must not allocate registers in the middle
// of the outer call's argument slots.
static void test_nested_call_arguments() {
    assert(run_int("func add(a: int, b: int): int { return a + b; }\n"
                   "func main(): int { return add(add(1, 2), add(3, 4)); }\n"
                   "let r: int = main();") == 10);
}

// The case a flat register file cannot express: each invocation needs its own
// copy of n, so this only works once frames exist.
static void test_recursion() {
    assert(run_int("func fact(n: int): int { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                   "func main(): int { return fact(5); }\n"
                   "let r: int = main();") == 120);
}

// Two recursive calls per level, so a frame that leaked registers into its
// caller would corrupt the pending addition.
static void test_tree_recursion() {
    assert(run_int("func fib(n: int): int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n"
                   "func main(): int { return fib(10); }\n"
                   "let r: int = main();") == 55);
}

// The callee's own locals live above its parameters; they must not reach back
// into the caller's frame.
static void test_callee_locals_do_not_clobber_caller() {
    assert(run_int("func inner(x: int): int { let a = 100; let b = 200; return x + a + b; }\n"
                   "func outer(y: int): int { let keep = 7; return inner(y) + keep; }\n"
                   "func main(): int { return outer(1); }\n"
                   "let r: int = main();") == 308);
}

static void test_deep_recursion_grows_the_stack() {
    // Deep enough to force the stack past its initial capacity while staying
    // under the call-depth limit.
    assert(run_int("func down(n: int): int { if n <= 0 { return 0; } return 1 + down(n - 1); }\n"
                   "func main(): int { return down(100); }\n"
                   "let r: int = main();") == 100);
}

// Exceeding the depth limit must unwind cleanly rather than corrupt memory.
static void test_call_depth_limit() {
    VM *vm = vm_create();

    vm_execute(vm, "func forever(n: int): int { return forever(n + 1); }\n"
                   "func main(): int { return forever(0); }\n"
                   "let r: int = main();");

    assert(vm->frame_count == 0);

    vm_free(vm);
}

// A recursive function that is not prototype 0, so its recursive call has to
// encode a non-zero index that codegen only knows before the body is built.
static void test_recursion_from_a_later_prototype() {
    assert(run_int("func unused_a(x: int): int { return x; }\n"
                   "func unused_b(x: int): int { return x; }\n"
                   "func fact(n: int): int { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                   "func main(): int { return fact(5); }\n"
                   "let r: int = main();") == 120);
}

// Mutual-looking chains still have to resolve to the right prototypes when
// several functions are declared before the ones doing the calling.
static void test_calls_across_several_prototypes() {
    assert(run_int("func double_it(x: int): int { return x * 2; }\n"
                   "func triple_it(x: int): int { return x * 3; }\n"
                   "func combine(x: int): int { return double_it(x) + triple_it(x); }\n"
                   "func main(): int { return combine(4); }\n"
                   "let r: int = main();") == 20);
}

// A nested declaration appends its own prototype while the enclosing function
// is still being generated, so the enclosing function's index cannot simply be
// the list size read on entry.
static void test_recursion_with_a_nested_declaration() {
    assert(run_int("func rec(n: int): int {\n"
                   "    func helper(): int { return 0; }\n"
                   "    if n <= 1 { return 1; }\n"
                   "    return n * rec(n - 1);\n"
                   "}\n"
                   "func main(): int { return rec(5); }\n"
                   "let r: int = main();") == 120);
}

int main(void) {
    test_simple_call();
    test_call_with_no_arguments();
    test_nested_call_arguments();
    test_recursion();
    test_tree_recursion();
    test_callee_locals_do_not_clobber_caller();
    test_deep_recursion_grows_the_stack();
    test_call_depth_limit();
    test_recursion_from_a_later_prototype();
    test_calls_across_several_prototypes();
    test_recursion_with_a_nested_declaration();

    printf("All call tests passed\n");
    return 0;
}
