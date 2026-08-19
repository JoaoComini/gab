#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>

static void test_simple_call() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(2, 3); }\n"
                        "let r: int = main();") == 5);
}

static void test_call_with_no_arguments() {
    assert(test_run_int("func answer(): int { return 42; }\n"
                        "func main(): int { return answer(); }\n"
                        "let r: int = main();") == 42);
}

// An argument that is itself a call must not allocate registers in the middle
// of the outer call's argument slots.
static void test_nested_call_arguments() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(add(1, 2), add(3, 4)); }\n"
                        "let r: int = main();") == 10);
}

// The case a flat register file cannot express: each invocation needs its own
// copy of n, so this only works once frames exist.
static void test_recursion() {
    assert(test_run_int("func fact(n: int): int { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                        "func main(): int { return fact(5); }\n"
                        "let r: int = main();") == 120);
}

// Two recursive calls per level, so a frame that leaked registers into its
// caller would corrupt the pending addition.
static void test_tree_recursion() {
    assert(test_run_int("func fib(n: int): int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n"
                        "func main(): int { return fib(10); }\n"
                        "let r: int = main();") == 55);
}

// The callee's own locals live above its parameters; they must not reach back
// into the caller's frame.
static void test_callee_locals_do_not_clobber_caller() {
    assert(test_run_int("func inner(x: int): int { let a = 100; let b = 200; return x + a + b; }\n"
                        "func outer(y: int): int { let keep = 7; return inner(y) + keep; }\n"
                        "func main(): int { return outer(1); }\n"
                        "let r: int = main();") == 308);
}

static void test_deep_recursion_grows_the_stack() {
    // Deep enough to force the stack past its initial capacity while staying
    // under the call-depth limit.
    assert(test_run_int("func down(n: int): int { if n <= 0 { return 0; } return 1 + down(n - 1); }\n"
                        "func main(): int { return down(100); }\n"
                        "let r: int = main();") == 100);
}

// Exceeding the depth limit must unwind cleanly rather than corrupt memory,
// and must say why. vm_run is used rather than vm_execute so the failure is
// read from the status instead of being printed.
static void test_call_depth_limit() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    assert(vm_compile(vm,
                      "func forever(n: int): int { return forever(n + 1); }\n"
                      "func main(): int { return forever(0); }\n"
                      "let r: int = main();",
                      &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(vm_run(vm, &script) == VM_RUN_ERR_CALL_DEPTH);
    assert(vm->error.status == VM_RUN_ERR_CALL_DEPTH);
    assert(vm->error.message);

    assert(vm->frame_count == 0);

    // The VM is still usable: a later run reports its own outcome rather than
    // the stale failure.
    assert(vm_run(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    vm_compiled_script_free(&script);
    vm_free(vm);
}

// A recursive function that is not prototype 0, so its recursive call has to
// encode a non-zero index that codegen only knows before the body is built.
static void test_recursion_from_a_later_prototype() {
    assert(test_run_int("func unused_a(x: int): int { return x; }\n"
                        "func unused_b(x: int): int { return x; }\n"
                        "func fact(n: int): int { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                        "func main(): int { return fact(5); }\n"
                        "let r: int = main();") == 120);
}

// Mutual-looking chains still have to resolve to the right prototypes when
// several functions are declared before the ones doing the calling.
static void test_calls_across_several_prototypes() {
    assert(test_run_int("func double_it(x: int): int { return x * 2; }\n"
                        "func triple_it(x: int): int { return x * 3; }\n"
                        "func combine(x: int): int { return double_it(x) + triple_it(x); }\n"
                        "func main(): int { return combine(4); }\n"
                        "let r: int = main();") == 20);
}

// Declarations are hoisted, so a function may call one written below it. Every
// other statically-typed language with top-level functions works this way, and
// methods need it: a receiver names a struct that may be declared further down.
static void test_call_a_function_declared_below() {
    assert(test_run_int("func main(): int { return helper(7); }\n"
                        "func helper(n: int): int { return n * 3; }\n"
                        "let r: int = main();") == 21);
}

// Mutual recursion is the case that a single ordered pass cannot express at
// all: whichever of the two comes first would call an undeclared name.
static void test_mutual_recursion() {
    assert(test_run_int("func is_even(n: int): bool { if n == 0 { return true; } return is_odd(n - 1); }\n"
                        "func is_odd(n: int): bool { if n == 0 { return false; } return is_even(n - 1); }\n"
                        "func main(): int { if is_even(10) { return 1; } return 0; }\n"
                        "let r: int = main();") == 1);
}

// A signature may name a struct declared below it, which is why types are
// declared before functions rather than in one interleaved pass.
static void test_signature_names_a_struct_declared_below() {
    assert(test_run_int("func health_of(p: Player): int { return p.health; }\n"
                        "struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 42;\n"
                        "    return health_of(p);\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

// A prototype index rides in OP_CALL's 17-bit field, not in a register-sized
// one. While it was 8 bits a single VM could hold only 255 functions across
// every module it loaded, which is far too few for a real project: this builds
// past that and calls the last one, so a regression to an 8-bit field fails
// here rather than in someone's game.
static void test_more_functions_than_an_8_bit_index_holds() {
    // 300 trivial functions, then one that calls the last of them.
    char source[64 * 1024];
    size_t used = 0;

    for (int i = 0; i < 300; i++) {
        used +=
            (size_t)snprintf(source + used, sizeof(source) - used, "func f%d(): int { return %d; }\n", i, i);
    }

    used += (size_t)snprintf(source + used, sizeof(source) - used,
                             "func main(): int { return f299(); }\n"
                             "let r: int = main();");

    assert(used < sizeof(source));
    assert(test_run_int(source) == 299);
}

int main(void) {
    test_simple_call();
    test_more_functions_than_an_8_bit_index_holds();
    test_call_a_function_declared_below();
    test_mutual_recursion();
    test_signature_names_a_struct_declared_below();
    test_call_with_no_arguments();
    test_nested_call_arguments();
    test_recursion();
    test_tree_recursion();
    test_callee_locals_do_not_clobber_caller();
    test_deep_recursion_grows_the_stack();
    test_call_depth_limit();
    test_recursion_from_a_later_prototype();
    test_calls_across_several_prototypes();

    printf("All call tests passed\n");
    return 0;
}
