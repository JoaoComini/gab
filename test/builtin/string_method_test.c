#include "support/run.h"

#include <assert.h>

static void test_len_answers_the_character_count() {
    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.len(); }\n"
                        "let r: int = f();") == 5);

    assert(test_run_int("func f(): int { let s: &str = \"\"; return s.len(); }\n"
                        "let r: int = f();") == 0);

    assert(test_run_int("func f(): int { let s: &str = \"a\\nb\"; return s.len(); }\n"
                        "let r: int = f();") == 3);
}

static void test_at_answers_the_character_at_an_index() {
    assert(test_run_int("func f(): int { let s: &str = \"abc\"; return s.at(0); }\n"
                        "let r: int = f();") == 'a');

    assert(test_run_int("func f(): int { let s: &str = \"abc\"; return s.at(2); }\n"
                        "let r: int = f();") == 'c');
}

static void test_at_takes_exactly_one_argument() {
    assert(!test_compiles("func f(): int { let s: &str = \"abc\"; return s.at(); }\n"));

    assert(!test_compiles("func f(): int { let s: &str = \"abc\"; return s.at(0, 1); }\n"));

    assert(!test_compiles("func f(): int { let s: &str = \"abc\"; return s.at(\"x\"); }\n"));
}

static void test_at_outside_the_string_fails_the_run() {
    assert(test_run_status("func f(): int { let s: &str = \"abc\"; return s.at(3); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);

    assert(test_run_status("func f(): int { let s: &str = \"abc\"; return s.at(-1); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);

    assert(test_run_status("func f(): int { let s: &str = \"\"; return s.at(0); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);
}

static void test_is_empty_answers_whether_any_character_is_there() {
    assert(test_run_bool("func f(): bool { let s: &str = \"\"; return s.is_empty(); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"a\"; return s.is_empty(); }\n"
                          "let r: bool = f();"));
}

static void test_starts_with_answers_the_leading_characters() {
    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.starts_with(\"he\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.starts_with(\"lo\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.starts_with(\"\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"he\"; return s.starts_with(\"hello\"); }\n"
                          "let r: bool = f();"));
}

static void test_ends_with_answers_the_trailing_characters() {
    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.ends_with(\"lo\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.ends_with(\"he\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.ends_with(\"\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"lo\"; return s.ends_with(\"hello\"); }\n"
                          "let r: bool = f();"));
}

static void test_contains_answers_whether_the_characters_occur() {
    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.contains(\"ell\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.contains(\"z\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: &str = \"hello\"; return s.contains(\"\"); }\n"
                         "let r: bool = f();"));
}

static void test_index_of_answers_where_the_characters_first_occur() {
    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.index_of(\"l\"); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.index_of(\"z\"); }\n"
                        "let r: int = f();") == -1);

    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.index_of(\"\"); }\n"
                        "let r: int = f();") == 0);
}

static void test_count_answers_how_many_times_the_characters_occur() {
    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.count(\"\"); }\n"
                        "let r: int = f();") == 0);

    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.count(\"l\"); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func f(): int { let s: &str = \"aaa\"; return s.count(\"aa\"); }\n"
                        "let r: int = f();") == 1);

    assert(test_run_int("func f(): int { let s: &str = \"hello\"; return s.count(\"z\"); }\n"
                        "let r: int = f();") == 0);
}

static void test_string_from_gives_an_owning_copy() {
    assert(test_run_bool("func f(): bool { let s: String = String::from(\"hi\"); return s == \"hi\"; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(a: &str): bool { let s: String = String::from(a); return s == \"hi\"; }\n"
                         "let r: bool = f(\"hi\");") == true);
}

static void test_to_owned_gives_an_owning_copy() {
    assert(test_run_bool("func f(): bool { let s: String = \"hi\".to_owned(); return s == \"hi\"; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(a: &str): bool { let s: String = a.to_owned(); return s == \"hi\"; }\n"
                         "let r: bool = f(\"hi\");") == true);

    assert(test_run_bool("func f(): bool { let j: String = \"ab\".to_owned(); let s: String = j.clone(); "
                         "return s == \"ab\"; }\n"
                         "let r: bool = f();") == true);
}

static void test_an_owned_copy_is_released_where_its_slot_dies() {
    TestProgram program = test_compile("func f(): int { let s: String = \"hi\".to_owned(); return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_cloning_an_empty_string_is_empty() {
    assert(test_run_bool("func f(): bool { let s: String = \"\".to_owned(); return s.is_empty(); }\n"
                         "let r: bool = f();") == true);
}

static void test_push_adds_one_character() {
    assert(test_run_int("func f(): int { let s: String = \"ab\".to_owned(); s.push(99); return s.len(); }\n"
                        "let r: int = f();") == 3);

    assert(test_run_bool("func f(): bool { let s: String = \"ab\".to_owned(); s.push(99); "
                         "return s == \"abc\"; }\n"
                         "let r: bool = f();") == true);
}

static void test_pushing_past_the_capacity_keeps_the_characters() {
    assert(test_run_int("func f(): int {\n"
                        "    let s: String = \"\".to_owned();\n"
                        "    for let i: int = 0; i < 64; i = i + 1 { s.push(97); }\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 64);
}

static void test_append_spells_one_string_after_the_other() {
    assert(test_run_bool("func f(): bool { let s: String = \"ab\".to_owned(); s.append(\"cd\"); "
                         "return s == \"abcd\"; }\n"
                         "let r: bool = f();") == true);
}

static void test_appending_an_empty_string_changes_nothing() {
    assert(test_run_bool("func f(): bool { let s: String = \"ab\".to_owned(); s.append(\"\"); "
                         "return s == \"ab\"; }\n"
                         "let r: bool = f();") == true);
}

static void test_a_string_assembled_by_appending_owns_its_characters() {
    assert(test_run_bool("func greet(name: &str): String {\n"
                         "    let line: String = \"hello, \".to_owned();\n"
                         "    line.append(name);\n"
                         "    return line;\n"
                         "}\n"
                         "func f(): bool { let g: String = greet(\"gab\"); return g == \"hello, gab\"; }\n"
                         "let r: bool = f();") == true);
}

int main(void) {
    test_to_owned_gives_an_owning_copy();
    test_string_from_gives_an_owning_copy();
    test_cloning_an_empty_string_is_empty();
    test_an_owned_copy_is_released_where_its_slot_dies();
    test_len_answers_the_character_count();
    test_at_answers_the_character_at_an_index();
    test_at_takes_exactly_one_argument();
    test_at_outside_the_string_fails_the_run();
    test_is_empty_answers_whether_any_character_is_there();
    test_starts_with_answers_the_leading_characters();
    test_ends_with_answers_the_trailing_characters();
    test_contains_answers_whether_the_characters_occur();
    test_index_of_answers_where_the_characters_first_occur();
    test_count_answers_how_many_times_the_characters_occur();
    test_push_adds_one_character();
    test_pushing_past_the_capacity_keeps_the_characters();
    test_append_spells_one_string_after_the_other();
    test_appending_an_empty_string_changes_nothing();
    test_a_string_assembled_by_appending_owns_its_characters();

    return 0;
}
