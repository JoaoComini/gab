#include "support/run.h"
#include "type/type_registry.h"

#include <assert.h>

static void test_characters_are_reached_through_a_reference() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let s: &str = \"abc\";\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: i32 = f();") == 3);
}

static void test_nothing_holds_the_characters_themselves() {
    assert(!test_compiles("func f(s: str): i32 { return 0; }\n"));
    assert(!test_compiles("struct Person { name: str }\n"));
    assert(!test_compiles("func f(): i32 { let s: str = \"hi\"; return 0; }\n"));
    assert(!test_compiles("func f(): str { return \"hi\"; }\n"));

    assert(test_compiles("func f(s: &str): i32 { return 0; }\n"));
    assert(test_compiles("struct Person { name: &str }\n"));
}

static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): i32 { let s: &str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): i32 { let n: i32 = \"hi\"; return 0; }\n"));
}

static void test_a_literal_loads_its_characters_and_length() {
    char text[8];
    int32_t length = 0;

    test_run_string("let s: &str = \"a\\nb\";", text, sizeof(text), &length);

    assert(length == 3);
    assert(memcmp(text, "a\nb", 3) == 0);
}

static void test_equal_strings_compare_equal() {
    assert(test_run_bool("func f(): bool { let a: &str = \"hi\"; let b: &str = \"hi\"; return a == b; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(): bool { let a: &str = \"hi\"; let b: &str = \"ho\"; return a == b; }\n"
                         "let r: bool = f();") == false);
}

static void test_a_prefix_is_not_equal() {
    assert(test_run_bool("func f(): bool { let a: &str = \"hi\"; let b: &str = \"hit\"; return a == b; }\n"
                         "let r: bool = f();") == false);
}

static void test_strings_compare_unequal() {
    assert(test_run_bool("func f(): bool { let a: &str = \"hi\"; let b: &str = \"ho\"; return a != b; }\n"
                         "let r: bool = f();") == true);
}

static void test_a_null_is_compared_like_any_character() {
    assert(test_run_bool("func f(): bool { let a: &str = \"a\\0b\"; let b: &str = \"a\\0c\"; "
                         "return a == b; }\n"
                         "let r: bool = f();") == false);
}

static void test_strings_are_not_ordered() {
    assert(!test_compiles("func f(): bool { let a: &str = \"a\"; return a < a; }\n"));
}

static void test_a_literal_is_not_released() {
    TestProgram program = test_compile("func f(): i32 { let s: &str = \"a\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_a_struct_field_borrows_its_characters() {
    assert(test_compiles("struct Person { name: &str }\n"));

    TestProgram program = test_compile("struct Person { name: &str }\n"
                                       "func f(): i32 { let p = Person { name: \"\" }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_a_returnable_borrow_outlives_its_frame() {
    assert(test_compiles("func f(a: &str): &str { return a; }\n"));

    assert(test_compiles("func f(): &str { return \"hi\"; }\n"));

    assert(test_run_bool("func f(a: &str): &str { return a; }\n"
                         "func g(): bool { return f(\"hi\") == \"hi\"; }\n"
                         "let r: bool = g();") == true);
}

static void test_a_literal_borrows() {
    assert(test_compiles("func f(): i32 { let s: &str = \"hi\"; return 0; }\n"));
}

static void test_characters_are_read_as_bytes() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let s: &str = \"abc\";\n"
                        "    let b: &slice<u8> = s.as_bytes();\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: i32 = f();") == 3);
}

int main(void) {
    test_characters_are_read_as_bytes();
    test_characters_are_reached_through_a_reference();
    test_nothing_holds_the_characters_themselves();
    test_a_literal_is_a_string();
    test_a_literal_loads_its_characters_and_length();
    test_equal_strings_compare_equal();
    test_a_prefix_is_not_equal();
    test_strings_compare_unequal();
    test_a_null_is_compared_like_any_character();
    test_strings_are_not_ordered();
    test_a_struct_field_borrows_its_characters();
    test_a_literal_is_not_released();
    test_a_returnable_borrow_outlives_its_frame();
    test_a_literal_borrows();

    return 0;
}
