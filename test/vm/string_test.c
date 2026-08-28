// The string type and its literals. A 'string' is a header by value -- the
// address of the characters and their count -- so it copies like a struct and
// owns nothing: a literal's characters live as long as the unit that declared
// it.

#include "object.h"
#include "support/run.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

// 'string' is a builtin type name, resolvable wherever a type is written.
static void test_string_names_a_type() {
    assert(test_compiles("func f(s: ref String): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: String }\n"));
}

// A literal is a string, so it may initialise one and nothing else.
static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));
}

// The header is what a host sees: the address of the characters and their
// count, laid out so a script's 'string' and the C struct are the same bytes.
static void test_a_string_is_an_address_and_a_length() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    const Type *string_type = type_registry_string(scope.type_registry);

    const TypeLayout *layout = type_registry_layout_of(scope.type_registry, string_type);

    assert(layout->size == sizeof(GabStringValue));
    assert(layout->alignment == _Alignof(GabStringValue));

    // Padded to the address's alignment, so it tiles whole slots.
    assert(layout->size == VM_STRING_SLOTS * VM_SLOT_SIZE);

    test_context_free(&ctx);
}

// One field is what the layout comes from: the block holding the characters,
// which carries how many of them are live. That the block owns and answers for
// itself is what lets a string be freed by the walk any struct gets.
static void test_a_string_is_one_owning_field() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    const Type *string_type = type_registry_string(scope.type_registry);

    assert(type_field_count(string_type) == 1);

    const TypeField *data = type_find_field(string_type, string_from_cstr(&ctx.strings, "data"));

    assert(data);

    const TypeLayout *layout = type_registry_layout_of(scope.type_registry, string_type);

    // The C mirror the VM and the host both read is this field's layout, so the
    // two statements of it must agree.
    assert(layout->offsets[data - type_fields(string_type)] == offsetof(GabStringValue, block));

    assert(type_is_owned(data->type));

    test_context_free(&ctx);
}

// Running a literal writes the header the type describes: the characters where
// they were interned, and their count. The escape is decoded by then, so the
// length counts characters rather than the source's two.
static void test_a_literal_loads_its_characters_and_length() {
    char text[8];
    int32_t length = 0;

    test_run_string("let s: ref str = \"a\\nb\";", text, sizeof(text), &length);

    assert(length == 3);
    assert(memcmp(text, "a\nb", 3) == 0);
}

// Two strings are equal when they spell the same characters, whoever allocated
// them. Interning makes equal literals one String *, so a comparison that only
// ever compared addresses would pass this and still be wrong.
static void test_equal_strings_compare_equal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"hi\"; return a == b; }\n"
                      "let r: bool = f();") == true);

    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"ho\"; return a == b; }\n"
                      "let r: bool = f();") == false);
}

// Two references naming equal characters at different addresses. Interning
// makes equal literals one address, so a comparison that read only the address
// would answer this one wrongly.
static void test_equal_characters_at_different_addresses() {
    assert(test_run_bool("func f(): bool {\n"
                         "    let o: String = \"hi\".to_owned();\n"
                         "    let a: ref str = o;\n"
                         "    let b: ref str = \"hi\";\n"
                         "    return a == b;\n"
                         "}\n"
                         "let r: bool = f();") == true);
}

// Length is part of the comparison, so a prefix is not the string it prefixes.
static void test_a_prefix_is_not_equal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"hit\"; return a == b; }\n"
                      "let r: bool = f();") == false);
}

// '!=' answers what '==' does not.
static void test_strings_compare_unequal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"ho\"; return a != b; }\n"
                      "let r: bool = f();") == true);
}

// A '\0' is a character, so the comparison reads the whole length rather than
// stopping where C would.
static void test_a_null_is_compared_like_any_character() {
    assert(test_run_bool("func f(): bool { let a: ref str = \"a\\0b\"; let b: ref str = \"a\\0c\"; "
                         "return a == b; }\n"
                         "let r: bool = f();") == false);
}

// Ordering asks something equality does not, and no answer is defined for it.
static void test_strings_are_not_ordered() {
    assert(!test_compiles("func f(): bool { let a: ref str = \"a\"; return a < a; }\n"));
}

static void test_a_literal_is_not_released() {
    TestProgram program = test_compile("func f(): int { let s: ref str = \"a\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

// A host lays a string field out as the two words it declares in C, and the
// script borrows those characters rather than freeing them.
static void test_a_struct_field_borrows_its_characters() {
    assert(test_compiles("struct Person { name: ref str }\n"));

    TestProgram program = test_compile("struct Person { name: ref str }\n"
                                       "func f(): int { let p: Person; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

// An owning string field is freed with the struct that holds it, the way an
// owning pointer field is: one release per field, where the block closes.
static void test_an_owning_string_field_is_released() {
    TestProgram program =
        test_compile("struct Doc { body: String }\n"
                     "func f(a: ref str): int { let d: Doc; d.body = a.to_owned(); return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) > 0);

    test_program_free(&program);

    // And the release reaches the characters: a run that leaked them fails the
    // sanitized build rather than this assertion.
    assert(test_run_bool(
               "struct Doc { body: String }\n"
               "func f(a: ref str): bool { let d: Doc; d.body = a.to_owned(); return d.body == \"ab\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

// An owning string is a unique owner like any other, so binding it to a second
// name must say which one frees the characters.
static void test_an_owning_string_needs_a_move_or_a_clone() {
    assert(!test_compiles_on_vm(
        "func f(v: ref str): int { let a: String = v.to_owned(); let b: String = a; return 0; }\n"));

    assert(test_compiles_on_vm(
        "func f(v: ref str): int { let a: String = v.to_owned(); let b: String = move a; return 0; }\n"));
}

static void test_reassigning_a_string_frees_the_old_characters() {
    assert(test_run_bool(
               "func f(a: ref str): bool { let s: String = a.to_owned(); s = \"d\".to_owned(); return s "
               "== \"d\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

// A string declared without one owns its slot from the declaration, so the
// assignment that fills it frees nothing and the scope frees it once. Nothing
// else would record the slot: an assignment reaches into the header rather than
// replacing the value.
static void test_a_string_declared_empty_is_freed_once() {
    assert(test_run_int("func f(): int {\n"
                        "    let s: String;\n"
                        "    s = \"ab\".to_owned();\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

// 'new String' allocates a heap slot holding a header, which zeroed is the
// empty string -- the same thing 'new Player' does for a struct's layout.
static void test_a_new_string_is_empty() {
    assert(test_run_int("func f(): int { let s: box String = new String; return (*s).len(); }\n"
                        "let r: int = f();") == 0);

    assert(test_run_bool("func f(): bool { let s: box String = new String; return (*s).is_empty(); }\n"
                         "let r: bool = f();") == true);
}

// A heap slot holding a string takes what is stored through it, and frees what
// it held before.
static void test_a_boxed_string_holds_what_is_stored_through_it() {
    assert(test_run_bool("func f(a: ref str): bool { let s: box String = new String; *s = a.to_owned(); "
                         "return *s == \"ab\"; }\n"
                         "let r: bool = f(\"ab\");") == true);

    assert(test_run_bool("func f(a: ref str): bool { let s: box String = new String; *s = a.to_owned(); *s = "
                         "\"d\".to_owned(); "
                         "return *s == \"d\"; }\n"
                         "let r: bool = f(\"ab\");") == true);
}

// A string field of a heap struct owns its characters, and the struct's
// teardown reaches them through the header the field holds.
static void test_a_heap_struct_frees_its_string_field() {
    assert(
        test_run_bool(
            "struct D { b: String }\n"
            "func f(a: ref str): bool { let d: box D = new D; d.b = a.to_owned(); return d.b == \"ab\"; }\n"
            "let r: bool = f(\"ab\");") == true);
}

// Only an owned value may be stored where a string owns: a borrow would leave
// the slot naming characters it did not allocate and must not free.
static void test_an_owning_string_slot_refuses_a_borrow() {
    assert(!test_compiles("func f(): int { let s: box String = new String; *s = \"ab\"; return 0; }\n"));

    assert(!test_compiles("func f(a: ref str): int { let s: box String = new String; *s = a; return 0; }\n"));
}

static void test_refusing_a_borrow_names_the_remedy() {
    assert(!test_compiles("func f(): int { let a: String = \"hi\"; return 0; }\n"));

    assert(test_compiles("func f(): int { let a: ref str = \"hi\"; return 0; }\n"));
}

// A borrow may be returned when its characters outlive the frame. A parameter's
// were allocated by the caller and a literal's belong to the unit's arena, so
// neither dies at the closing brace.
static void test_a_returnable_borrow_outlives_its_frame() {
    assert(test_compiles("func f(a: ref str): ref str { return a; }\n"));

    assert(test_compiles("func f(): ref str { return \"hi\"; }\n"));

    assert(test_run_bool("func f(a: ref str): ref str { return a; }\n"
                         "func g(): bool { return f(\"hi\") == \"hi\"; }\n"
                         "let r: bool = g();") == true);
}

// A literal borrows the characters its unit's arena holds, so it types as
// 'str' and nothing frees it.
static void test_a_literal_borrows() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));
}

// An owning string may not take what a borrow names: the arena's characters
// would be freed by a slot that never allocated them.
static void test_an_owning_string_refuses_a_borrow() {
    assert(!test_compiles("func f(): int { let s: String = \"hi\"; return 0; }\n"));
}

int main(void) {
    test_string_names_a_type();
    test_a_literal_is_a_string();
    test_a_string_is_an_address_and_a_length();
    test_a_string_is_one_owning_field();
    test_a_literal_loads_its_characters_and_length();
    test_equal_strings_compare_equal();
    test_equal_characters_at_different_addresses();
    test_a_prefix_is_not_equal();
    test_strings_compare_unequal();
    test_a_null_is_compared_like_any_character();
    test_strings_are_not_ordered();
    test_a_struct_field_borrows_its_characters();
    test_an_owning_string_needs_a_move_or_a_clone();
    test_an_owning_string_field_is_released();
    test_a_literal_is_not_released();
    test_reassigning_a_string_frees_the_old_characters();
    test_a_string_declared_empty_is_freed_once();
    test_a_new_string_is_empty();
    test_a_boxed_string_holds_what_is_stored_through_it();
    test_a_heap_struct_frees_its_string_field();
    test_an_owning_string_slot_refuses_a_borrow();
    test_refusing_a_borrow_names_the_remedy();
    test_a_returnable_borrow_outlives_its_frame();
    test_a_literal_borrows();
    test_an_owning_string_refuses_a_borrow();

    return 0;
}
