#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_a_literal_gives_each_field_its_value() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func main(): int { let v = V { x: 3, y: 4 }; return v.x * 10 + v.y; }\n"
                        "let r: int = main();") == 34);
}

static void test_fields_may_be_written_in_any_order() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func main(): int { let v = V { y: 4, x: 3 }; return v.x * 10 + v.y; }\n"
                        "let r: int = main();") == 34);
}

static void test_a_missing_field_is_refused() {
    assert(!test_compiles("struct V { x: int, y: int }\n"
                          "func main(): int { let v = V { x: 1 }; return v.x; }\n"));
}

static void test_an_unknown_field_is_refused() {
    assert(!test_compiles("struct V { x: int }\n"
                          "func main(): int { let v = V { x: 1, z: 2 }; return v.x; }\n"));
}

static void test_a_field_given_twice_is_refused() {
    assert(!test_compiles("struct V { x: int }\n"
                          "func main(): int { let v = V { x: 1, x: 2 }; return v.x; }\n"));
}

static void test_a_field_value_of_the_wrong_type_is_refused() {
    assert(!test_compiles("struct V { x: int }\n"
                          "func main(): int { let v = V { x: true }; return v.x; }\n"));
}

static void test_a_literal_nests() {
    assert(
        test_run_int("struct Inner { n: int }\n"
                     "struct Outer { a: Inner, b: int }\n"
                     "func main(): int { let o = Outer { a: Inner { n: 7 }, b: 2 }; return o.a.n * o.b; }\n"
                     "let r: int = main();") == 14);
}

static void test_a_literal_is_an_argument() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func sum(v: V): int { return v.x + v.y; }\n"
                        "func main(): int { return sum(V { x: 5, y: 6 }); }\n"
                        "let r: int = main();") == 11);
}

static void test_a_literal_is_returned() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func make(): V { return V { x: 8, y: 1 }; }\n"
                        "func main(): int { let v = make(); return v.x + v.y; }\n"
                        "let r: int = main();") == 9);
}

static void test_a_field_holds_a_computed_value() {
    assert(test_run_int(
               "struct V { x: int, y: int }\n"
               "func main(): int { let n = 4; let v = V { x: n + 1, y: n * 2 }; return v.x * 10 + v.y; }\n"
               "let r: int = main();") == 58);
}

static void test_a_literal_in_a_condition_needs_parentheses() {
    assert(test_run_int("struct V { x: int }\n"
                        "func main(): int { if (V { x: 1 }).x == 1 { return 5; } return 0; }\n"
                        "let r: int = main();") == 5);
}

static void test_a_loop_body_is_not_read_as_a_literal() {
    assert(test_run_int(
               "struct V { x: int }\n"
               "func main(): int { let t = 0; for let i = 0; i < 3; i = i + 1 { t = t + 1; } return t; }\n"
               "let r: int = main();") == 3);
}

static void test_a_loop_clause_is_not_read_as_a_literal() {
    assert(!test_compiles(
        "struct V { x: int }\n"
        "func main(): int { let v = V { x: 0 }; for let i = 0; i < 3; v = V { x: i } { } return 0; }\n"));

    assert(test_compiles(
        "struct V { x: int }\n"
        "func main(): int { let v = V { x: 0 }; for let i = 0; i < 3; v = (V { x: i }) { } return 0; }\n"));

    assert(!test_compiles(
        "struct V { x: int }\n"
        "func main(): int { for let v = V { x: 0 }; v.x < 3; v.x = v.x + 1 { } return 0; }\n"));

    assert(test_compiles(
        "struct V { x: int }\n"
        "func main(): int { for let v = (V { x: 0 }); v.x < 3; v.x = v.x + 1 { } return 0; }\n"));
}

static void test_a_literal_takes_an_owning_field() {
    assert(
        test_run_int("struct Box { n: int }\n"
                     "struct Holder { b: *Box }\n"
                     "func main(): int { let h = Holder { b: box Box { n: 0 } }; h.b.n = 7; return h.b.n; }\n"
                     "let r: int = main();") == 7);
}

static void test_a_literal_with_an_owning_field_is_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: *Box }\n"
                        "func make(): Holder { return Holder { b: box Box { n: 0 } }; }\n"
                        "func main(): int { let h = make(); h.b.n = 4; return h.b.n; }\n"
                        "let r: int = main();") == 4);
}

static void test_sub_word_fields_keep_their_own_values() {
    assert(test_run_int("struct Flags { a: bool, b: bool, c: bool, d: bool }\n"
                        "func f(): int { let v = Flags { a: true, b: false, c: true, d: true };\n"
                        "let n: int = 0;\n"
                        "if v.a { n = n + 1000; }\n"
                        "if v.b { n = n + 100; }\n"
                        "if v.c { n = n + 10; }\n"
                        "if v.d { n = n + 1; }\n"
                        "return n; }\n"
                        "let r: int = f();") == 1011);
}

static void test_a_generic_struct_takes_a_literal() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func f(): int { let h = Holder<int> { value: 4 }; return h.value; }\n"
                        "let r: int = f();") == 4);
}

static void test_a_comparison_is_not_read_as_type_arguments() {
    assert(test_run_bool("struct Pair { a: int, b: int }\n"
                         "func f(): bool { let p = Pair { a: 1, b: 2 }; return p.a < p.b; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_int("func f(): int { let a = 1; let b = 2; let c = 3;\n"
                        "if a < b { if b < c { return 9; } } return 0; }\n"
                        "let r: int = f();") == 9);
}

static void test_a_field_holds_an_array() {
    assert(test_run_int("struct Buf { xs: array<int, 3> }\n"
                        "func f(): int { let b = Buf { xs: [4, 9, 2] }; return b.xs[1]; }\n"
                        "let r: int = f();") == 9);
}

static void test_a_struct_local_is_refused_without_a_literal() {
    assert(!test_compiles("struct Point { x: int, y: int }\n"
                          "func f(): int { let v: Point; return v.x; }\n"));

    assert(test_compiles("struct Point { x: int, y: int }\n"
                         "func f(): int { let v = Point { x: 1, y: 2 }; return v.x; }\n"));
}

static void test_a_ref_cannot_borrow_a_literal() {
    assert(!test_compiles("struct V { x: int }\n"
                          "func f(): int { let r: &V = V { x: 1 }; return r.x; }\n"));

    assert(test_compiles("struct V { x: int }\n"
                         "func f(): int { let v = V { x: 1 }; let r: &V = v; return r.x; }\n"));
}

static void test_a_pointer_local_still_needs_no_initializer() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func f(): int { let b: *Box; b = box Box { n: 0 }; return b.n; }\n"));

    assert(test_compiles("func f(): int { let n: int; return 0; }\n"));
}

static void test_a_struct_with_no_fields_takes_an_empty_literal() {
    assert(test_compiles("struct E { }\n"
                         "func f(): int { let e = E { }; return 0; }\n"));
}

static void test_a_malformed_literal_is_refused() {
    assert(!test_compiles("struct V { x: int, y: int }\n"
                          "func f(): int { let v = V { x: 1, 2 }; return v.x; }\n"));

    assert(!test_compiles("struct V { x: int, y: int }\n"
                          "func f(): int { let v = V { x: 1, y }; return v.x; }\n"));

    assert(!test_compiles("struct V { x: int, y: int }\n"
                          "func f(): int { let v = V { x: 1, y: }; return v.x; }\n"));

    assert(!test_compiles("struct V { x: int, y: int }\n"
                          "func f(): int { let v = V { x: 1, y: 2 ; return v.x; }\n"));
}

int main(void) {
    test_a_malformed_literal_is_refused();
    test_a_struct_with_no_fields_takes_an_empty_literal();
    test_a_literal_gives_each_field_its_value();
    test_fields_may_be_written_in_any_order();
    test_a_missing_field_is_refused();
    test_an_unknown_field_is_refused();
    test_a_field_given_twice_is_refused();
    test_a_field_value_of_the_wrong_type_is_refused();
    test_a_literal_nests();
    test_a_literal_is_an_argument();
    test_a_literal_is_returned();
    test_a_field_holds_a_computed_value();
    test_a_literal_in_a_condition_needs_parentheses();
    test_a_loop_body_is_not_read_as_a_literal();
    test_a_loop_clause_is_not_read_as_a_literal();
    test_a_literal_takes_an_owning_field();
    test_a_literal_with_an_owning_field_is_returned();
    test_sub_word_fields_keep_their_own_values();
    test_a_generic_struct_takes_a_literal();
    test_a_comparison_is_not_read_as_type_arguments();
    test_a_field_holds_an_array();
    test_a_struct_local_is_refused_without_a_literal();
    test_a_ref_cannot_borrow_a_literal();
    test_a_pointer_local_still_needs_no_initializer();

    printf("All struct literal tests passed\n");
    return 0;
}
