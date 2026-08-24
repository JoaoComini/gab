// The methods a 'string' answers. Each is a C body the VM registers against the
// string type at startup, reached through the receiver rather than by a name a
// variable could shadow.
//
// The characters are borrowed for the call, so nothing here allocates: a method
// answers a number or a truth about the receiver it was given.

#include "support/run.h"

#include <assert.h>

// 'len' answers how many characters a string denotes, which is what its header
// already carries.
static void test_len_answers_the_character_count() {
    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.len(); }\n"
                        "let r: int = f();") == 5);

    assert(test_run_int("func f(): int { let s: string = \"\"; return s.len(); }\n"
                        "let r: int = f();") == 0);

    // The escape is one character, not the two that spell it.
    assert(test_run_int("func f(): int { let s: string = \"a\\nb\"; return s.len(); }\n"
                        "let r: int = f();") == 3);
}

// A method reads the character at an index, counting from zero.
static void test_at_answers_the_character_at_an_index() {
    assert(test_run_int("func f(): int { let s: string = \"abc\"; return s.at(0); }\n"
                        "let r: int = f();") == 'a');

    assert(test_run_int("func f(): int { let s: string = \"abc\"; return s.at(2); }\n"
                        "let r: int = f();") == 'c');
}

// The count a method declares is what a call must supply, receiver aside.
static void test_at_takes_exactly_one_argument() {
    assert(!test_compiles("func f(): int { let s: string = \"abc\"; return s.at(); }\n"));

    assert(!test_compiles("func f(): int { let s: string = \"abc\"; return s.at(0, 1); }\n"));

    assert(!test_compiles("func f(): int { let s: string = \"abc\"; return s.at(\"x\"); }\n"));
}

// An index no character sits at fails the run, at either end.
static void test_at_outside_the_string_fails_the_run() {
    assert(test_run_status("func f(): int { let s: string = \"abc\"; return s.at(3); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);

    assert(test_run_status("func f(): int { let s: string = \"abc\"; return s.at(-1); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);

    assert(test_run_status("func f(): int { let s: string = \"\"; return s.at(0); }\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);
}

// A string with no characters is empty, and any character makes it not.
static void test_is_empty_answers_whether_any_character_is_there() {
    assert(test_run_bool("func f(): bool { let s: string = \"\"; return s.is_empty(); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"a\"; return s.is_empty(); }\n"
                          "let r: bool = f();"));
}

// A string starts with a prefix when its leading characters spell it. Every
// string starts with the empty one, and none starts with something longer.
static void test_starts_with_answers_the_leading_characters() {
    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.starts_with(\"he\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"hello\"; return s.starts_with(\"lo\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.starts_with(\"\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"he\"; return s.starts_with(\"hello\"); }\n"
                          "let r: bool = f();"));
}

// A string ends with a suffix when its trailing characters spell it.
static void test_ends_with_answers_the_trailing_characters() {
    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.ends_with(\"lo\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"hello\"; return s.ends_with(\"he\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.ends_with(\"\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"lo\"; return s.ends_with(\"hello\"); }\n"
                          "let r: bool = f();"));
}

// A string contains another when it occurs anywhere in it.
static void test_contains_answers_whether_the_characters_occur() {
    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.contains(\"ell\"); }\n"
                         "let r: bool = f();"));

    assert(!test_run_bool("func f(): bool { let s: string = \"hello\"; return s.contains(\"z\"); }\n"
                          "let r: bool = f();"));

    assert(test_run_bool("func f(): bool { let s: string = \"hello\"; return s.contains(\"\"); }\n"
                         "let r: bool = f();"));
}

// Where the characters first occur, counting from zero, or -1 when they do not.
// The empty string occurs at the front of anything.
static void test_index_of_answers_where_the_characters_first_occur() {
    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.index_of(\"l\"); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.index_of(\"z\"); }\n"
                        "let r: int = f();") == -1);

    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.index_of(\"\"); }\n"
                        "let r: int = f();") == 0);
}

// How many times the characters occur, counting occurrences that do not
// overlap: the second 'aa' in 'aaa' starts where the first one ended.
static void test_count_answers_how_many_times_the_characters_occur() {
    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.count(\"\"); }\n"
                        "let r: int = f();") == 0);

    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.count(\"l\"); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func f(): int { let s: string = \"aaa\"; return s.count(\"aa\"); }\n"
                        "let r: int = f();") == 1);

    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.count(\"z\"); }\n"
                        "let r: int = f();") == 0);
}

int main(void) {
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

    return 0;
}
