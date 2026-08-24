// A type that owns cannot be copied implicitly, but it may say how it is
// duplicated: a 'clone' method returning a new value of the type. The name is
// reserved so that the remedy the copy diagnostic names is one the compiler
// can check for rather than a convention.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// The duplicate is a separate object: writing through one does not reach the
// other, which is the whole point of cloning rather than borrowing.
static void test_a_clone_yields_an_independent_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func (h: ref Holder) clone(): Holder {\n"
                        "    let out: Holder;\n"
                        "    out.b = new Box;\n"
                        "    out.b.n = h.b.n;\n"
                        "    return out;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 1;\n"
                        "    let g: Holder = h.clone();\n"
                        "    g.b.n = 2;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 1);
}

// Cloning reads the source rather than consuming it, so the original is still
// live afterwards -- the difference from 'move'.
static void test_the_source_of_a_clone_stays_live() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func (h: ref Holder) clone(): Holder {\n"
                        "    let out: Holder;\n"
                        "    out.b = new Box;\n"
                        "    out.b.n = h.b.n;\n"
                        "    return out;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 4;\n"
                        "    let g: Holder = h.clone();\n"
                        "    return h.b.n + g.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

// A 'clone' returning something other than its own type would not duplicate
// the receiver, so the signature is checked where it is declared rather than
// where a caller is surprised by it.
static void test_a_clone_returning_another_type_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: *Box }\n"
                          "func (h: ref Holder) clone(): int {\n"
                          "    return 0;\n"
                          "}\n"));
}

// Duplicating takes nothing but the receiver.
static void test_a_clone_taking_parameters_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: *Box }\n"
                          "func (h: ref Holder) clone(deep: bool): Holder {\n"
                          "    let out: Holder;\n"
                          "    out.b = new Box;\n"
                          "    return out;\n"
                          "}\n"));
}

// Declaring a clone does not make the type implicitly copyable: the allocation
// stays visible at the point it happens.
static void test_a_type_with_a_clone_still_refuses_an_implicit_copy() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: *Box }\n"
                          "func (h: ref Holder) clone(): Holder {\n"
                          "    let out: Holder;\n"
                          "    out.b = new Box;\n"
                          "    return out;\n"
                          "}\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    h.b = new Box;\n"
                          "    let g: Holder = h;\n"
                          "    return 0;\n"
                          "}\n"));
}

// The remedy the copy diagnostic names is only available where the type says
// how it is duplicated, so a type without one is told that rather than sent to
// call a method it does not have.
static void test_the_copy_diagnostic_names_clone_only_where_one_exists() {
    const char *without = "struct Box { n: int }\n"
                          "struct Holder { b: *Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    let g: Holder = h;\n"
                          "    return 0;\n"
                          "}\n";

    assert(!test_compiles(without));
    assert(test_diagnostic_mentions(without, "declares no 'clone'"));
}

int main(void) {
    test_a_clone_yields_an_independent_object();
    test_the_source_of_a_clone_stays_live();
    test_a_clone_returning_another_type_is_refused();
    test_a_clone_taking_parameters_is_refused();
    test_a_type_with_a_clone_still_refuses_an_implicit_copy();
    test_the_copy_diagnostic_names_clone_only_where_one_exists();

    printf("All clone tests passed\n");
    return 0;
}
