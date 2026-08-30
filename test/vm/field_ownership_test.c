// An owning field holds what is stored into it, frees what it held before, and
// is freed itself when the struct holding it goes out of scope.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// A struct local's owning field starts holding nothing, so the store that
// initializes it has no previous occupant to free.
static void test_a_fresh_owning_field_keeps_what_is_stored() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: box Box }\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 4;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

// Ownership moved out of a local lands in the field and stays readable there.
static void test_a_move_into_an_owning_field_keeps_the_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: box Box }\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    let a: box Box = new Box;\n"
                        "    a.n = 7;\n"
                        "    h.b = a;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// Storing over a field that already owns frees the old object and keeps the
// new one.
static void test_storing_over_an_owning_field_keeps_the_new_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: box Box }\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 1;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 9;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A struct going out of scope frees what its fields own, wherever the struct
// itself lives. One release per owning field, emitted where the block closes.
static void test_a_struct_local_frees_what_its_fields_own() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "struct Holder { b: box Box }\n"
                                       "func main(): int { let h: Holder; h.b = new Box; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // Two: the store's release of what the field held before, and the scope's
    // release of what it holds at the end.
    assert(test_count_opcode(chunk, OP_RELEASE) == 2);

    test_program_free(&program);
}

// A struct holding another by value owns through it, so the inner struct's
// fields are freed too.
static void test_a_nested_struct_frees_through_its_inner_fields() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "struct Inner { b: box Box }\n"
                                       "struct Outer { inner: Inner }\n"
                                       "func main(): int { let o: Outer; o.inner.b = new Box; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 2);

    test_program_free(&program);
}

// A 'ref' field names something it does not own, so nothing frees it.
static void test_a_ref_field_is_not_freed() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "struct Watcher { b: ref Box }\n"
                                       "func main(): int { let w: Watcher; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

// An owning field is nulled where it is declared, so the first store into it
// frees nothing. One instruction per field, whatever a pointer's width.
static void test_an_owning_field_is_nulled_at_its_declaration() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "struct Holder { b: box Box }\n"
                                       "func main(): int { let h: Holder; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_NULL) == 1);

    test_program_free(&program);
}

// A 'ref' field owns nothing, so nothing reads it as an owner and nothing
// nulls it.
static void test_a_ref_field_is_not_nulled() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "struct Watcher { b: ref Box }\n"
                                       "func main(): int { let w: Watcher; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_NULL) == 0);

    test_program_free(&program);
}

// Storing a value another slot owns would make two owners of one object, so it
// is refused -- and the refusal names the two ways to say what was meant.
static void test_an_owning_field_refuses_a_value_another_slot_owns() {
    const char *source = "struct Box { n: int }\n"
                         "struct Holder { b: box Box }\n"
                         "func main(): int {\n"
                         "    let h: Holder;\n"
                         "    let a: box Box = new Box;\n"
                         "    h.b = a;\n"
                         "    return a.n;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "no longer holds a value"));
}

// A struct moves whole or not at all. Moving one field would leave the rest
// behind, and what a half-moved struct means is not something the language
// says, so the diagnostic points at moving the whole thing.
static void test_moving_a_field_is_refused() {
    const char *source = "struct Box { n: int }\n"
                         "struct Holder { b: box Box }\n"
                         "func main(): int {\n"
                         "    let g: Holder;\n"
                         "    let h: Holder;\n"
                         "    h.b = new Box;\n"
                         "    g.b = h.b;\n"
                         "    return 0;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "whole"));
}

// The struct itself still moves, which is what replaces moving a field.
static void test_a_whole_struct_still_moves() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: box Box }\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 3;\n"
                        "    let g: Holder = h;\n"
                        "    return g.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

int main(void) {
    test_an_owning_field_refuses_a_value_another_slot_owns();
    test_moving_a_field_is_refused();
    test_a_whole_struct_still_moves();
    test_an_owning_field_is_nulled_at_its_declaration();
    test_a_ref_field_is_not_nulled();
    test_a_fresh_owning_field_keeps_what_is_stored();
    test_a_move_into_an_owning_field_keeps_the_object();
    test_storing_over_an_owning_field_keeps_the_new_object();
    test_a_struct_local_frees_what_its_fields_own();
    test_a_nested_struct_frees_through_its_inner_fields();
    test_a_ref_field_is_not_freed();

    printf("All field ownership tests passed\n");
    return 0;
}
