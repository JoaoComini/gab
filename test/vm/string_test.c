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
    assert(test_compiles("func f(s: String): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: String }\n"));
}

// A literal is a string, so it may initialise one and nothing else.
static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): int { let s: str = \"hi\"; return 0; }\n"));

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

// The two fields are what the layout comes from: the characters the string
// names and how many there are. An owning string owns its characters, so the
// field that names them owns; a borrow's field does not.
static void test_a_string_is_two_fields() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    Type *string_type = type_registry_get_builtin(scope.type_registry, TYPE_STRING);

    const TypeField *data = type_find_field(string_type, string_from_cstr(&ctx.strings, "data"));
    const TypeField *length = type_find_field(string_type, string_from_cstr(&ctx.strings, "length"));

    assert(data && length);

    // The C mirror the VM and the host both read is these fields' layout, so
    // the two statements of it must agree.
    assert(data->offset == offsetof(GabStringValue, data));
    assert(length->offset == offsetof(GabStringValue, length));

    // The header owns its characters, not the field naming them: a raw address
    // carries no ownership, so the question is answered by the string itself.
    assert(!type_is_owned(data->type));
    assert(!type_is_owned(length->type));

    test_context_free(&ctx);
}

// Running a literal writes the header the type describes: the characters where
// they were interned, and their count. The escape is decoded by then, so the
// length counts characters rather than the source's two.
static void test_a_literal_loads_its_characters_and_length() {
    char text[8];
    int32_t length = 0;

    test_run_string("let s: str = \"a\\nb\";", text, sizeof(text), &length);

    assert(length == 3);
    assert(memcmp(text, "a\nb", 3) == 0);
}

// Two strings are equal when they spell the same characters, whoever allocated
// them. Interning makes equal literals one String *, so a comparison that only
// ever compared addresses would pass this and still be wrong.
static void test_equal_strings_compare_equal() {
    assert(test_run_bool("func f(): bool { let a: str = \"hi\"; let b: str = \"hi\"; return a == b; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(): bool { let a: str = \"hi\"; let b: str = \"ho\"; return a == b; }\n"
                         "let r: bool = f();") == false);
}

// Length is part of the comparison, so a prefix is not the string it prefixes.
static void test_a_prefix_is_not_equal() {
    assert(test_run_bool("func f(): bool { let a: str = \"hi\"; let b: str = \"hit\"; return a == b; }\n"
                         "let r: bool = f();") == false);
}

// '!=' answers what '==' does not.
static void test_strings_compare_unequal() {
    assert(test_run_bool("func f(): bool { let a: str = \"hi\"; let b: str = \"ho\"; return a != b; }\n"
                         "let r: bool = f();") == true);
}

// A '\0' is a character, so the comparison reads the whole length rather than
// stopping where C would.
static void test_a_null_is_compared_like_any_character() {
    assert(test_run_bool("func f(): bool { let a: str = \"a\\0b\"; let b: str = \"a\\0c\"; "
                         "return a == b; }\n"
                         "let r: bool = f();") == false);
}

// Ordering asks something equality does not, and no answer is defined for it.
static void test_strings_are_not_ordered() {
    assert(!test_compiles("func f(): bool { let a: str = \"a\"; return a < a; }\n"));
}

// '+' spells the characters of one string after the other, in a string that
// owns them: neither operand's characters can be extended in place.
static void test_concatenation_joins_the_characters() {
    assert(test_run_int("func f(a: str): int { let s: String = a .. \"cd\"; return s.len(); }\n"
                        "let r: int = f(\"ab\");") == 4);

    assert(test_run_bool("func f(a: str): bool { let s: String = a .. \"cd\"; return s == \"abcd\"; }\n"
                         "let r: bool = f(\"ab\");") == true);
}

// The result owns, so it may initialise an owning string and a borrow of one
// may not take it back without saying so.
static void test_concatenation_yields_an_owning_string() {
    assert(test_compiles("func f(a: str): int { let s: String = a .. \"b\"; return 0; }\n"));

    // A borrow may not take it: the slot that allocated the characters is the
    // one that frees them, and a second name for them would outlive the free.
    assert(!test_compiles("func f(a: str): int { let s: str = a .. \"b\"; return 0; }\n"));
}

// The slot a concatenation lands in owns its characters, so the block that
// declared it releases them where it closes.
static void test_a_concatenation_is_released_where_its_slot_dies() {
    TestProgram program = test_compile("func f(a: str): int { let s: String = a .. \"b\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

// A borrow of a literal allocates nothing, so nothing is released for it.
static void test_a_literal_is_not_released() {
    TestProgram program = test_compile("func f(): int { let s: str = \"a\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

// A host lays a string field out as the two words it declares in C, and the
// script borrows those characters rather than freeing them.
static void test_a_struct_field_borrows_its_characters() {
    assert(test_compiles("struct Person { name: str }\n"));

    TestProgram program = test_compile("struct Person { name: str }\n"
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
                     "func f(a: str): int { let d: Doc; d.body = a .. \"b\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) > 0);

    test_program_free(&program);

    // And the release reaches the characters: a run that leaked them fails the
    // sanitized build rather than this assertion.
    assert(
        test_run_bool("struct Doc { body: String }\n"
                      "func f(a: str): bool { let d: Doc; d.body = a .. \"b\"; return d.body == \"ab\"; }\n"
                      "let r: bool = f(\"a\");") == true);
}

// An owning string is a unique owner like any other, so binding it to a second
// name must say which one frees the characters.
static void test_an_owning_string_needs_a_move_or_a_clone() {
    assert(
        !test_compiles("func f(v: str): int { let a: String = v .. \"y\"; let b: String = a; return 0; }\n"));

    assert(test_compiles(
        "func f(v: str): int { let a: String = v .. \"y\"; let b: String = move a; return 0; }\n"));
}

// A concatenation may be returned: ownership passes to the caller, which is
// what a returned owning value means everywhere else.
static void test_a_concatenation_may_be_returned() {
    assert(test_run_bool("func greet(name: str): String { return \"hi, \" .. name; }\n"
                         "func f(): bool { let g: String = greet(\"gab\"); return g == \"hi, gab\"; }\n"
                         "let r: bool = f();") == true);
}

// Arithmetic is for numbers. A string joins with '..' and answers nothing to
// '+', which would otherwise hide an allocation behind an arithmetic spelling.
static void test_strings_do_not_add() {
    assert(!test_compiles("func f(): int { let a: str = \"a\"; let b: String = a + a; return 0; }\n"));

    assert(!test_compiles("func f(): int { let a: str = \"a\"; let b: String = a - a; return 0; }\n"));
}

// Joining binds looser than arithmetic and tighter than comparison, so a sum
// is joined whole and the join is what gets compared.
static void test_join_binds_between_arithmetic_and_comparison() {
    // Tighter than '==': the join happens, then the result is compared. Were it
    // looser, this would compare "b" to "ab" and join the bool.
    assert(test_run_bool("func f(a: str): bool { return a .. \"b\" == \"ab\"; }\n"
                         "let r: bool = f(\"a\");") == true);
}

// '..' joins what can be joined, which so far is strings alone.
static void test_numbers_do_not_join() {
    assert(!test_compiles("func f(): int { let n: int = 1 .. 2; return 0; }\n"));
}

// A join in an operand position is bound to nothing, so the statement that
// produced it is what frees it.
static void test_an_unbound_join_is_freed_by_its_statement() {
    assert(test_run_bool("func f(a: str): bool { return a .. \"b\" == \"ab\"; }\n"
                         "let r: bool = f(\"a\");") == true);
}

// Storing a join into an owning field hands the field the characters: the
// statement must not also free the register they were built in.
static void test_a_join_stored_into_a_field_is_freed_once() {
    assert(
        test_run_bool("struct Doc { body: String }\n"
                      "func f(a: str): bool { let d: Doc; d.body = a .. \"b\"; return d.body == \"ab\"; }\n"
                      "let r: bool = f(\"a\");") == true);
}

// Returning a join hands its characters to the caller, so the frame that built
// them frees nothing.
static void test_a_returned_join_survives_its_frame() {
    assert(test_run_bool("func greet(name: str): String { return \"hi, \" .. name; }\n"
                         "func f(): bool { let g: String = greet(\"gab\"); return g == \"hi, gab\"; }\n"
                         "let r: bool = f();") == true);
}

// Reassigning a string frees what the slot held and keeps what it was given.
static void test_reassigning_a_string_frees_the_old_characters() {
    assert(test_run_bool("func f(a: str): bool { let s: String = a .. \"b\"; s = a .. \"d\"; return s "
                         "== \"ad\"; }\n"
                         "let r: bool = f(\"a\");") == true);
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
    assert(test_run_bool("func f(a: str): bool { let s: box String = new String; *s = a .. \"b\"; "
                         "return *s == \"ab\"; }\n"
                         "let r: bool = f(\"a\");") == true);

    assert(test_run_bool(
               "func f(a: str): bool { let s: box String = new String; *s = a .. \"b\"; *s = a .. \"d\"; "
               "return *s == \"ad\"; }\n"
               "let r: bool = f(\"a\");") == true);
}

// A string field of a heap struct owns its characters, and the struct's
// teardown reaches them through the header the field holds.
static void test_a_heap_struct_frees_its_string_field() {
    assert(test_run_bool(
               "struct D { b: String }\n"
               "func f(a: str): bool { let d: box D = new D; d.b = a .. \"b\"; return d.b == \"ab\"; }\n"
               "let r: bool = f(\"a\");") == true);
}

// Only an owned value may be stored where a string owns: a borrow would leave
// the slot naming characters it did not allocate and must not free.
static void test_an_owning_string_slot_refuses_a_borrow() {
    assert(!test_compiles("func f(): int { let s: box String = new String; *s = \"ab\"; return 0; }\n"));

    assert(!test_compiles("func f(a: str): int { let s: box String = new String; *s = a; return 0; }\n"));
}

// '..' yields a string that owns its characters however its operands were
// written. Two literals are joined at run time like any other pair, so what a
// join may initialise is decided by how it was written rather than by what the
// compiler could evaluate early.
static void test_a_join_owns_even_between_literals() {
    assert(test_compiles("func f(): int { let s: String = \"a\" .. \"b\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let s: str = \"a\" .. \"b\"; return 0; }\n"));

    assert(test_run_bool("func f(): bool { let s: String = \"ab\" .. \"cd\"; return s == \"abcd\"; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_bool("func f(): bool { let s: String = \"a\" .. \"b\" .. \"c\"; return s == \"abc\"; }\n"
                         "let r: bool = f();") == true);

    // A '\0' is an ordinary character, so a join copies by length rather than
    // stopping where C would.
    assert(test_run_int("func f(): int { let s: String = \"a\\0b\" .. \"c\"; return s.len(); }\n"
                        "let r: int = f();") == 4);

    TestProgram program = test_compile("func f(): int { let s: String = \"a\" .. \"b\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_CONCAT) == 1);
    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

// Refusing a borrow where a string owns names the two ways to say what was
// meant, since neither is guessable from the mismatch alone.
static void test_refusing_a_borrow_names_the_remedy() {
    assert(!test_compiles("func f(): int { let a: String = \"hi\"; return 0; }\n"));

    assert(test_compiles("func f(): int { let a: str = \"hi\"; return 0; }\n"));
}

// A borrow may be returned when its characters outlive the frame. A parameter's
// were allocated by the caller and a literal's belong to the unit's arena, so
// neither dies at the closing brace.
static void test_a_returnable_borrow_outlives_its_frame() {
    assert(test_compiles("func f(a: str): str { return a; }\n"));

    assert(test_compiles("func f(): str { return \"hi\"; }\n"));

    assert(test_run_bool("func f(a: str): str { return a; }\n"
                         "func g(): bool { return f(\"hi\") == \"hi\"; }\n"
                         "let r: bool = g();") == true);
}

// A literal borrows the characters its unit's arena holds, so it types as
// 'str' and nothing frees it.
static void test_a_literal_borrows() {
    assert(test_compiles("func f(): int { let s: str = \"hi\"; return 0; }\n"));
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
    test_a_string_is_two_fields();
    test_a_literal_loads_its_characters_and_length();
    test_equal_strings_compare_equal();
    test_a_prefix_is_not_equal();
    test_strings_compare_unequal();
    test_a_null_is_compared_like_any_character();
    test_strings_are_not_ordered();
    test_concatenation_joins_the_characters();
    test_concatenation_yields_an_owning_string();
    test_strings_do_not_add();
    test_numbers_do_not_join();
    test_join_binds_between_arithmetic_and_comparison();
    test_a_struct_field_borrows_its_characters();
    test_an_owning_string_needs_a_move_or_a_clone();
    test_an_owning_string_field_is_released();
    test_a_concatenation_may_be_returned();
    test_a_concatenation_is_released_where_its_slot_dies();
    test_a_literal_is_not_released();
    test_an_unbound_join_is_freed_by_its_statement();
    test_a_join_stored_into_a_field_is_freed_once();
    test_a_returned_join_survives_its_frame();
    test_reassigning_a_string_frees_the_old_characters();
    test_a_new_string_is_empty();
    test_a_boxed_string_holds_what_is_stored_through_it();
    test_a_heap_struct_frees_its_string_field();
    test_an_owning_string_slot_refuses_a_borrow();
    test_a_join_owns_even_between_literals();
    test_refusing_a_borrow_names_the_remedy();
    test_a_returnable_borrow_outlives_its_frame();
    test_a_literal_borrows();
    test_an_owning_string_refuses_a_borrow();

    return 0;
}
