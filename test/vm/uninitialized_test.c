#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_reading_an_uninitialized_owning_pointer_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box;\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_reading_an_uninitialized_borrow_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let b: &Box;\n"
                          "    return b.n;\n"
                          "}\n"));
}

static void test_a_pointer_assigned_before_use_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box;\n"
                        "    a = box Box { n: 0 };\n"
                        "    a.n = 5;\n"
                        "    return a.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

static void test_a_pointer_assigned_on_one_arm_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box;\n"
                          "    if 1 < 2 { a = box Box { n: 0 }; }\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_pointer_assigned_on_both_arms_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box;\n"
                        "    if 1 < 2 { a = box Box { n: 0 }; } else { a = box Box { n: 0 }; }\n"
                        "    a.n = 6;\n"
                        "    return a.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_an_uninitialized_int_is_accepted() {
    assert(test_compiles("func main(): int {\n"
                         "    let n: int;\n"
                         "    return n;\n"
                         "}\n"));
}

int main(void) {
    test_reading_an_uninitialized_owning_pointer_is_refused();
    test_reading_an_uninitialized_borrow_is_refused();
    test_a_pointer_assigned_before_use_is_accepted();
    test_a_pointer_assigned_on_one_arm_is_refused();
    test_a_pointer_assigned_on_both_arms_is_accepted();
    test_an_uninitialized_int_is_accepted();

    printf("All uninitialized use tests passed\n");
    return 0;
}
