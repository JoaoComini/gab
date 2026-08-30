#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_a_fresh_owning_field_keeps_what_is_stored() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func main(): int {\n"
                        "    let h = Holder { b: box Box { n: 0 } };\n"
                        "    h.b.n = 4;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

static void test_a_move_into_an_owning_field_keeps_the_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func main(): int {\n"
                        "    let h = Holder { b: box Box { n: 0 } };\n"
                        "    let a: *Box = box Box { n: 0 };\n"
                        "    a.n = 7;\n"
                        "    h.b = a;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

static void test_storing_over_an_owning_field_keeps_the_new_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func main(): int {\n"
                        "    let h = Holder { b: box Box { n: 0 } };\n"
                        "    h.b.n = 1;\n"
                        "    h.b = box Box { n: 0 };\n"
                        "    h.b.n = 9;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_a_struct_local_frees_what_its_fields_own() {
    TestProgram program =
        test_compile("struct Box { n: int }\n"
                     "struct Holder { b: *Box }\n"
                     "func main(): int { let h = Holder { b: box Box { n: 0 } }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_a_nested_struct_frees_through_its_inner_fields() {
    TestProgram program = test_compile(
        "struct Box { n: int }\n"
        "struct Inner { b: *Box }\n"
        "struct Outer { inner: Inner }\n"
        "func main(): int { let o = Outer { inner: Inner { b: box Box { n: 0 } } }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_a_ref_field_is_not_freed() {
    TestProgram program =
        test_compile("struct Box { n: int }\n"
                     "struct Watcher { b: &Box }\n"
                     "func main(): int { let b = Box { n: 0 }; let w = Watcher { b: b }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_an_owning_field_refuses_a_value_another_slot_owns() {
    const char *source = "struct Box { n: int }\n"
                         "struct Holder { b: *Box }\n"
                         "func main(): int {\n"
                         "    let h = Holder { b: box Box { n: 0 } };\n"
                         "    let a: *Box = box Box { n: 0 };\n"
                         "    h.b = a;\n"
                         "    return a.n;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "no longer holds a value"));
}

static void test_moving_a_field_is_refused() {
    const char *source = "struct Box { n: int }\n"
                         "struct Holder { b: *Box }\n"
                         "func main(): int {\n"
                         "    let g = Holder { b: box Box { n: 0 } };\n"
                         "    let h = Holder { b: box Box { n: 0 } };\n"
                         "    g.b = h.b;\n"
                         "    return 0;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "whole"));
}

static void test_a_whole_struct_still_moves() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func main(): int {\n"
                        "    let h = Holder { b: box Box { n: 0 } };\n"
                        "    h.b.n = 3;\n"
                        "    let g: Holder = h;\n"
                        "    return g.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_a_field_assigned_in_an_inner_scope_outlives_it() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func main(): int {\n"
                        "    let h = Holder { b: box Box { n: 0 } };\n"
                        "    if true {\n"
                        "        h.b = box Box { n: 0 };\n"
                        "        h.b.n = 7;\n"
                        "    }\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

int main(void) {
    test_a_field_assigned_in_an_inner_scope_outlives_it();
    test_an_owning_field_refuses_a_value_another_slot_owns();
    test_moving_a_field_is_refused();
    test_a_whole_struct_still_moves();
    test_a_fresh_owning_field_keeps_what_is_stored();
    test_a_move_into_an_owning_field_keeps_the_object();
    test_storing_over_an_owning_field_keeps_the_new_object();
    test_a_struct_local_frees_what_its_fields_own();
    test_a_nested_struct_frees_through_its_inner_fields();
    test_a_ref_field_is_not_freed();

    printf("All field ownership tests passed\n");
    return 0;
}
