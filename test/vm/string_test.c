// The string type and its literals. A 'string' is a header by value -- the
// address of the characters and their count -- so it copies like a struct and
// owns nothing: a literal's characters live as long as the unit that declared
// it.

#include "object.h"
#include "support/run.h"

#include <assert.h>
#include <string.h>

// 'string' is a builtin type name, resolvable wherever a type is written.
static void test_string_names_a_type() {
    assert(test_compiles("func f(s: string): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: string }\n"));
}

// A literal is a 'string', so it may initialise one and nothing else.
static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): int { let s: string = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));
}

// The header is what a host sees: the address of the characters and their
// count, laid out so a script's 'string' and the C struct are the same bytes.
static void test_a_string_is_an_address_and_a_length() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    Type *string_type = type_registry_get_builtin(scope.type_registry, TYPE_STRING);

    assert(string_type->size == sizeof(GabStringValue));
    assert(string_type->alignment == _Alignof(GabStringValue));

    // Padded to the address's alignment, so it tiles whole slots.
    assert(string_type->size == VM_STRING_SLOTS * VM_SLOT_SIZE);

    test_context_free(&ctx);
}

// Running a literal writes the header the type describes: the characters where
// they were interned, and their count. The escape is decoded by then, so the
// length counts characters rather than the source's two.
static void test_a_literal_loads_its_characters_and_length() {
    char text[8];
    int32_t length = 0;

    test_run_string("let s: string = \"a\\nb\";", text, sizeof(text), &length);

    assert(length == 3);
    assert(memcmp(text, "a\nb", 3) == 0);
}

// Two strings are equal when they spell the same characters, whoever allocated
// them. Interning makes equal literals one String *, so a comparison that only
// ever compared addresses would pass this and still be wrong.
static void test_equal_strings_compare_equal() {
    assert(test_run_bool("func f(): bool { let a: string = \"hi\"; let b: string = \"hi\"; return a == b; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(): bool { let a: string = \"hi\"; let b: string = \"ho\"; return a == b; }\n"
                         "let r: bool = f();") == false);
}

// Length is part of the comparison, so a prefix is not the string it prefixes.
static void test_a_prefix_is_not_equal() {
    assert(
        test_run_bool("func f(): bool { let a: string = \"hi\"; let b: string = \"hit\"; return a == b; }\n"
                      "let r: bool = f();") == false);
}

// '!=' answers what '==' does not.
static void test_strings_compare_unequal() {
    assert(test_run_bool("func f(): bool { let a: string = \"hi\"; let b: string = \"ho\"; return a != b; }\n"
                         "let r: bool = f();") == true);
}

// A '\0' is a character, so the comparison reads the whole length rather than
// stopping where C would.
static void test_a_null_is_compared_like_any_character() {
    assert(test_run_bool(
               "func f(): bool { let a: string = \"a\\0b\"; let b: string = \"a\\0c\"; return a == b; }\n"
               "let r: bool = f();") == false);
}

// Ordering asks something equality does not, and no answer is defined for it.
static void test_strings_are_not_ordered() {
    assert(!test_compiles("func f(): bool { let a: string = \"a\"; return a < a; }\n"));
}

// Arithmetic on strings is refused: '+' would allocate, which is a different
// feature than this one.
static void test_strings_do_not_add() {
    assert(!test_compiles("func f(): int { let a: string = \"a\"; let b: string = a + a; return 0; }\n"));
}

// 'len' answers how many characters a string denotes, which is what its header
// already carries. A method, so it is reached through the receiver rather than
// by a name a variable could shadow.
static void test_len_answers_the_character_count() {
    assert(test_run_int("func f(): int { let s: string = \"hello\"; return s.len(); }\n"
                        "let r: int = f();") == 5);

    assert(test_run_int("func f(): int { let s: string = \"\"; return s.len(); }\n"
                        "let r: int = f();") == 0);

    // The escape is one character, not the two that spell it.
    assert(test_run_int("func f(): int { let s: string = \"a\\nb\"; return s.len(); }\n"
                        "let r: int = f();") == 3);
}

// A method is keyed by its receiver, so nothing declares 'len' as a free name.
static void test_len_is_not_a_free_function() {
    assert(!test_compiles("func f(): int { let s: string = \"a\"; return len(s); }\n"));
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

int main(void) {
    test_string_names_a_type();
    test_a_literal_is_a_string();
    test_a_string_is_an_address_and_a_length();
    test_a_literal_loads_its_characters_and_length();
    test_equal_strings_compare_equal();
    test_a_prefix_is_not_equal();
    test_strings_compare_unequal();
    test_a_null_is_compared_like_any_character();
    test_strings_are_not_ordered();
    test_strings_do_not_add();
    test_len_answers_the_character_count();
    test_len_is_not_a_free_function();
    test_at_answers_the_character_at_an_index();
    test_at_takes_exactly_one_argument();
    test_at_outside_the_string_fails_the_run();

    return 0;
}
