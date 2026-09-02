#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_an_owning_parameter_takes_a_moved_argument() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func consume(b: *Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = box Box { n: 0 };\n"
                        "    a.n = 6;\n"
                        "    return consume(a);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_an_owning_parameter_takes_its_argument() {
    const char *source = "struct Box { n: int }\n"
                         "func consume(b: *Box): int { return b.n; }\n"
                         "func main(): int {\n"
                         "    let a: *Box = box Box { n: 0 };\n"
                         "    consume(a);\n"
                         "    return a.n;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "no longer holds a value"));
}

static void test_a_moved_argument_is_dead_after_the_call() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func consume(b: *Box): int { return b.n; }\n"
                          "func main(): int {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    consume(a);\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_ref_parameter_still_borrows() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: &Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = box Box { n: 0 };\n"
                        "    a.n = 4;\n"
                        "    return peek(a) + a.n;\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

static void test_a_value_reaches_a_ref_parameter_by_address() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: &Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let a = Box { n: 6 };\n"
                        "    return peek(a);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_an_owning_parameter_frees_what_it_was_given() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func consume(b: *Box): int { return b.n; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_a_ref_parameter_frees_nothing() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func peek(b: &Box): int { return b.n; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_only_the_callee_frees_an_owned_argument() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func consume(b: *Box): int { return b.n; }\n"
                                       "func main(): int {\n"
                                       "    let a: *Box = box Box { n: 0 };\n"
                                       "    return consume(a);\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 1), OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_an_owned_parameter_may_be_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func through(b: *Box): *Box { return b; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = box Box { n: 0 };\n"
                        "    a.n = 9;\n"
                        "    let back: *Box = through(a);\n"
                        "    return back.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_a_borrow_cannot_be_laundered_into_an_owned_return() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func launder(b: &Box): *Box { return b; }\n"));
}

static void test_parameter_zero_may_own() {
    assert(test_run_int("struct Box { n: int }\n"
                        "impl Box {\n"
                        "    func take(b: *Box): int { return b.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let a: *Box = box Box { n: 0 };\n"
                        "    a.n = 4;\n"
                        "    return Box::take(a);\n"
                        "}\n"
                        "let r: int = main();") == 4);

    assert(!test_compiles("struct Box { n: int }\n"
                          "impl Box {\n"
                          "    func take(b: *Box): int { return b.n; }\n"
                          "}\n"
                          "func main(): int {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    a.take();\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_method_parameter_may_own() {
    assert(test_run_int("struct Box { n: int }\n"
                        "impl Box {\n"
                        "    func adopt(b: &Box, other: *Box): int { return other.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let host: *Box = box Box { n: 0 };\n"
                        "    let gift: *Box = box Box { n: 0 };\n"
                        "    gift.n = 3;\n"
                        "    return host.adopt(gift);\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_a_by_value_parameter_takes_ownership() {
    assert(test_compiles_on_vm("func take(s: String): int { return 0; }\n"
                               "func main(): int {\n"
                               "    let s: String = String::from(\"hi\");\n"
                               "    return take(s);\n"
                               "}\n"));

    assert(!test_compiles_on_vm("func take(s: String): int { return 0; }\n"
                                "func main(): int {\n"
                                "    let s: String = String::from(\"hi\");\n"
                                "    take(s);\n"
                                "    return take(s);\n"
                                "}\n"));

    assert(test_compiles_on_vm("func take(s: *String): int { return 0; }\n"
                               "func main(): int {\n"
                               "    let s: *String = box String::from(\"\");\n"
                               "    return take(s);\n"
                               "}\n"));
}

int main(void) {
    test_an_owning_parameter_takes_a_moved_argument();
    test_an_owning_parameter_takes_its_argument();
    test_a_moved_argument_is_dead_after_the_call();
    test_a_ref_parameter_still_borrows();
    test_a_value_reaches_a_ref_parameter_by_address();
    test_an_owning_parameter_frees_what_it_was_given();
    test_a_ref_parameter_frees_nothing();
    test_only_the_callee_frees_an_owned_argument();
    test_an_owned_parameter_may_be_returned();
    test_a_borrow_cannot_be_laundered_into_an_owned_return();
    test_parameter_zero_may_own();
    test_a_by_value_parameter_takes_ownership();
    test_a_method_parameter_may_own();

    printf("All owning parameter tests passed\n");
    return 0;
}
