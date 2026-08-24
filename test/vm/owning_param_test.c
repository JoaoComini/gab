// A parameter may own what it is given, and only when the call site said so.
// Bare '*T' owns, 'ref T' borrows -- the same spelling locals and fields use --
// so a call site can tell from the declaration alone whether a move is needed.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// An owning parameter is declared '*T', and the call site moves into it.
static void test_an_owning_parameter_takes_a_moved_argument() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func consume(b: *Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    a.n = 6;\n"
                        "    return consume(move a);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// Ownership is part of the signature, so passing a non-copyable argument to an
// owning parameter without saying where it goes is refused -- and the refusal
// teaches the same two remedies a binding does.
static void test_an_owning_parameter_refuses_an_unmoved_argument() {
    const char *source = "struct Box { n: int }\n"
                         "func consume(b: *Box): int { return b.n; }\n"
                         "func main(): int {\n"
                         "    let a: *Box = new Box;\n"
                         "    return consume(a);\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "move"));
    assert(test_diagnostic_mentions(source, "clone"));
}
// The argument is dead after the call, exactly as it would be after any move.
static void test_a_moved_argument_is_dead_after_the_call() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func consume(b: *Box): int { return b.n; }\n"
                          "func main(): int {\n"
                          "    let a: *Box = new Box;\n"
                          "    consume(move a);\n"
                          "    return a.n;\n"
                          "}\n"));
}

// A 'ref T' parameter borrows, so it takes an argument without a move and the
// caller goes on owning it.
static void test_a_ref_parameter_still_borrows() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    a.n = 4;\n"
                        "    return peek(a) + a.n;\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

// The callee frees what it was given, so an owned argument dies with the call.
static void test_an_owning_parameter_frees_what_it_was_given() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func consume(b: *Box): int { return b.n; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 1);

    test_program_free(&program);
}

// A borrow parameter frees nothing: what it names belongs to the caller.
static void test_a_ref_parameter_frees_nothing() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func peek(b: ref Box): int { return b.n; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 0);

    test_program_free(&program);
}

// The callee owns what it was moved, so the caller does not free the argument
// too: exactly one release covers it, and it is the callee's.
static void test_only_the_callee_frees_an_owned_argument() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func consume(b: *Box): int { return b.n; }\n"
                                       "func main(): int {\n"
                                       "    let a: *Box = new Box;\n"
                                       "    return consume(move a);\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 1), OP_RELEASE) == 0);

    test_program_free(&program);
}

// Ownership moved in may be handed back out: the caller gave it up, so
// returning it transfers rather than duplicating.
static void test_an_owned_parameter_may_be_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func through(b: *Box): *Box { return b; }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    a.n = 9;\n"
                        "    let back: *Box = through(move a);\n"
                        "    return back.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A borrow still cannot become an owned return: nobody gave that ownership up.
static void test_a_borrow_cannot_be_laundered_into_an_owned_return() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func launder(b: ref Box): *Box { return b; }\n"));
}

// A method borrows its receiver. Ownership of the receiver is not something a
// call site can spell, so '*T' there stays refused.
static void test_a_method_receiver_still_cannot_own() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func (b: *Box) get(): int { return b.n; }\n"));
}

// A method's own parameters are parameters like any other, so one may own.
static void test_a_method_parameter_may_own() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) adopt(other: *Box): int { return other.n; }\n"
                        "func main(): int {\n"
                        "    let host: *Box = new Box;\n"
                        "    let gift: *Box = new Box;\n"
                        "    gift.n = 3;\n"
                        "    return host.adopt(move gift);\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

int main(void) {
    test_an_owning_parameter_takes_a_moved_argument();
    test_an_owning_parameter_refuses_an_unmoved_argument();
    test_a_moved_argument_is_dead_after_the_call();
    test_a_ref_parameter_still_borrows();
    test_an_owning_parameter_frees_what_it_was_given();
    test_a_ref_parameter_frees_nothing();
    test_only_the_callee_frees_an_owned_argument();
    test_an_owned_parameter_may_be_returned();
    test_a_borrow_cannot_be_laundered_into_an_owned_return();
    test_a_method_receiver_still_cannot_own();
    test_a_method_parameter_may_own();

    printf("All owning parameter tests passed\n");
    return 0;
}
