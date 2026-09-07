#include "compile.h"
#include "support/run.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>

static void test_simple_call() {
    assert(test_run_int("func add(a: i32, b: i32): i32 { return a + b; }\n"
                        "func main(): i32 { return add(2, 3); }\n"
                        "let r: i32 = main();") == 5);
}

static void test_call_with_no_arguments() {
    assert(test_run_int("func answer(): i32 { return 42; }\n"
                        "func main(): i32 { return answer(); }\n"
                        "let r: i32 = main();") == 42);
}

static void test_nested_call_arguments() {
    assert(test_run_int("func add(a: i32, b: i32): i32 { return a + b; }\n"
                        "func main(): i32 { return add(add(1, 2), add(3, 4)); }\n"
                        "let r: i32 = main();") == 10);
}

static void test_recursion() {
    assert(test_run_int("func fact(n: i32): i32 { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                        "func main(): i32 { return fact(5); }\n"
                        "let r: i32 = main();") == 120);
}

static void test_tree_recursion() {
    assert(test_run_int("func fib(n: i32): i32 { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n"
                        "func main(): i32 { return fib(10); }\n"
                        "let r: i32 = main();") == 55);
}

static void test_callee_locals_do_not_clobber_caller() {
    assert(test_run_int("func inner(x: i32): i32 { let a = 100; let b = 200; return x + a + b; }\n"
                        "func outer(y: i32): i32 { let keep = 7; return inner(y) + keep; }\n"
                        "func main(): i32 { return outer(1); }\n"
                        "let r: i32 = main();") == 308);
}

static void test_deep_recursion_grows_the_stack() {
    assert(test_run_int("func down(n: i32): i32 { if n <= 0 { return 0; } return 1 + down(n - 1); }\n"
                        "func main(): i32 { return down(100); }\n"
                        "let r: i32 = main();") == 100);
}

static void test_call_depth_limit() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    assert(compile_unit(vm,
                        "module test;\n"
                        "func forever(n: i32): i32 { return forever(n + 1); }\n"
                        "func main(): i32 { return forever(0); }\n"
                        "let r: i32 = main();",
                        &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(interp_run_top_level(vm, &script) == VM_RUN_ERR_CALL_DEPTH);
    assert(vm->error.status == VM_RUN_ERR_CALL_DEPTH);
    assert(vm->error.message);

    assert(vm->frame_count == 0);

    assert(interp_run_top_level(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    func_proto_free(&script);
    vm_free(vm);
}

static void test_recursion_from_a_later_prototype() {
    assert(test_run_int("func unused_a(x: i32): i32 { return x; }\n"
                        "func unused_b(x: i32): i32 { return x; }\n"
                        "func fact(n: i32): i32 { if n <= 1 { return 1; } return n * fact(n - 1); }\n"
                        "func main(): i32 { return fact(5); }\n"
                        "let r: i32 = main();") == 120);
}

static void test_calls_across_several_prototypes() {
    assert(test_run_int("func double_it(x: i32): i32 { return x * 2; }\n"
                        "func triple_it(x: i32): i32 { return x * 3; }\n"
                        "func combine(x: i32): i32 { return double_it(x) + triple_it(x); }\n"
                        "func main(): i32 { return combine(4); }\n"
                        "let r: i32 = main();") == 20);
}

static void test_call_a_function_declared_below() {
    assert(test_run_int("func main(): i32 { return helper(7); }\n"
                        "func helper(n: i32): i32 { return n * 3; }\n"
                        "let r: i32 = main();") == 21);
}

static void test_mutual_recursion() {
    assert(test_run_int("func is_even(n: i32): bool { if n == 0 { return true; } return is_odd(n - 1); }\n"
                        "func is_odd(n: i32): bool { if n == 0 { return false; } return is_even(n - 1); }\n"
                        "func main(): i32 { if is_even(10) { return 1; } return 0; }\n"
                        "let r: i32 = main();") == 1);
}

static void test_signature_names_a_struct_declared_below() {
    assert(test_run_int("func health_of(p: Player): i32 { return p.health; }\n"
                        "struct Player { health: i32 }\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 42 };\n"
                        "    return health_of(p);\n"
                        "}\n"
                        "let r: i32 = main();") == 42);
}

static void test_more_functions_than_an_8_bit_index_holds() {
    char source[64 * 1024];
    size_t used = 0;

    for (int i = 0; i < 300; i++) {
        used +=
            (size_t)snprintf(source + used, sizeof(source) - used, "func f%d(): i32 { return %d; }\n", i, i);
    }

    used += (size_t)snprintf(source + used, sizeof(source) - used,
                             "func main(): i32 { return f299(); }\n"
                             "let r: i32 = main();");

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
