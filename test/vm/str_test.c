// Characters and the two ways of naming them. 'str' is the characters
// themselves, which nothing holds; 'ref str' names them and carries how many
// there are; 'String' owns them.

#include "support/run.h"
#include "type_registry.h"

#include <assert.h>

// How far a run of characters goes is not in its type, so no slot reserves room
// for one: the reference is what carries the count, and it is two words.
static void test_characters_are_reached_through_a_reference() {
    assert(test_run_int("func f(): int {\n"
                        "    let s: ref str = \"abc\";\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 3);
}

// Nothing holds the characters themselves, wherever a value would be held.
static void test_nothing_holds_the_characters_themselves() {
    assert(!test_compiles("func f(s: str): int { return 0; }\n"));
    assert(!test_compiles("struct Person { name: str }\n"));
    assert(!test_compiles("func f(): int { let s: str = \"hi\"; return 0; }\n"));
    assert(!test_compiles("func f(): str { return \"hi\"; }\n"));

    assert(test_compiles("func f(s: ref str): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: ref str }\n"));
}

// A literal names characters the unit's arena holds, which outlive every value
// that reads them.
static void test_a_literal_names_borrowed_characters() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));

    // Not an owning string: a literal allocated nothing, so a slot that frees
    // cannot take one.
    assert(!test_compiles("func f(): int { let s: String = \"hi\"; return 0; }\n"));
}

// An owning string already holds the address and the count a reference carries,
// so it lends one as it stands rather than being pointed at.
static void test_a_string_lends_a_reference_to_its_characters() {
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let b: ref str = o;\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

// The lend reaches an argument too, so a function taking characters accepts
// either way of naming them.
static void test_a_string_lends_to_a_parameter() {
    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int { let o: String = \"hi\".to_owned(); return g(o); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int { return g(\"abc\"); }\n"
                        "let r: int = f();") == 3);
}

// A reference to characters nothing holds names memory freed where the
// expression ends, so an owned copy has to be given a home before it can be
// lent.
static void test_a_reference_cannot_borrow_a_temporary() {
    assert(!test_compiles("func f(): int { let s: ref str = \"ab\".to_owned(); return 0; }\n"));
}

// A reference may not outlive the string whose characters it names.
static void test_a_reference_may_not_outlive_what_it_borrows() {
    assert(!test_compiles("func f(): ref str {\n"
                          "    let o: String = \"ab\".to_owned();\n"
                          "    return o;\n"
                          "}\n"));
}

// 'clone' duplicates its receiver, so it lives where that holds: a 'String'
// clones to a 'String'. A reference has none, since it already copies by
// assignment and duplicating one would name the same characters.
static void test_clone_belongs_to_the_owning_string() {
    assert(test_run_bool("func f(): bool {\n"
                         "    let o: String = \"hi\".to_owned();\n"
                         "    let c: String = o.clone();\n"
                         "    return c == \"hi\";\n"
                         "}\n"
                         "let r: bool = f();") == true);

    // Not reachable at all from a borrow, rather than found and then refused:
    // 'clone' belongs to the owner, and a borrow does not deref to one.
    assert(!test_compiles_on_vm("func f(): int {\n"
                                "    let s: ref str = \"hi\";\n"
                                "    let c: String = s.clone();\n"
                                "    return 0;\n"
                                "}\n"));
}

// 'ref String' is the address of a slot holding a header, and the characters
// are reached through it: the header it names lends them, so an argument
// position takes one where the other was declared.
static void test_a_reference_to_a_header_reaches_the_characters() {
    assert(test_compiles_on_vm("func f(): int {\n"
                               "    let o: String = \"hi\".to_owned();\n"
                               "    let p: ref String = o;\n"
                               "    return 0;\n"
                               "}\n"));

    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let p: ref String = o;\n"
                        "    return g(p);\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

// Two levels out from what the parameter takes: a 'ref box String' reaches a
// 'ref str' by the 'box String' it names and the header inside that.
static void test_a_header_two_levels_out_reaches_the_characters() {
    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int {\n"
                        "    let o: box String = new String;\n"
                        "    *o = \"hey\".to_owned();\n"
                        "    let p: ref box String = o;\n"
                        "    return g(p);\n"
                        "}\n"
                        "let r: int = f();") == 3);
}

// Reaching through a 'ref String' is spelled the way it is for any pointer: the
// deref names the 'String', which lends its characters from there.
static void test_a_ref_string_reaches_the_characters_by_dereferencing() {
    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let p: ref String = o;\n"
                        "    return g(*p);\n"
                        "}\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let p: ref String = o;\n"
                        "    return p.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

// Comparison and joining read the same two words whichever way the characters
// are named.
static void test_either_naming_compares_and_joins() {
    assert(test_run_bool("func f(): bool { let a: ref str = \"hi\"; return a == \"hi\"; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"abcd\".to_owned();\n"
                        "    return o.len();\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

// A lend hands over the count of live characters, never the capacity they sit
// in. The two differ only once a string has room it has not filled, so the
// borrow is taken from one that grew past its first allocation.
static void test_a_lend_carries_the_length_not_the_capacity() {
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"ab\".to_owned();\n"
                        "    o.push(99);\n"
                        "    let b: ref str = o;\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: int = f();") == 3);
}

// What a lend copies is registered with the deref relation, so it says which of
// the lender's bytes name the view. The parts must fit inside what the
// reference occupies: a lend writing past that would run into whatever the
// frame put next.
static void test_a_lend_fits_the_reference_it_builds() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    test_scope_with_library(&ctx, &scope);

    TypeRegistry *registry = scope.type_registry;

    const Type *string_type = test_declared_type(registry, "String");
    const Type *reference = type_registry_ref_to(registry, registry->primitives.str_type);

    const Deref *deref = type_registry_deref(registry, string_type);

    assert(deref && deref->to == registry->primitives.str_type);

    // An address and the count naming the run, which is what a 'ref str' is.
    assert(deref->part_count == 2);

    size_t total = 0;

    for (size_t i = 0; i < deref->part_count; i++) {
        // Every part is somewhere inside the lender.
        assert(deref->parts[i].offset + deref->parts[i].size <= type_registry_size_of(registry, string_type));

        total += deref->parts[i].size;
    }

    assert(total <= type_registry_size_of(registry, reference));

    // A type nothing declared a view for lends nothing, however it is laid out.
    assert(!type_registry_deref(registry, reference));

    test_context_free(&ctx);
}

int main(void) {
    test_characters_are_reached_through_a_reference();
    test_nothing_holds_the_characters_themselves();
    test_a_literal_names_borrowed_characters();
    test_a_string_lends_a_reference_to_its_characters();
    test_a_lend_carries_the_length_not_the_capacity();
    test_a_lend_fits_the_reference_it_builds();
    test_a_string_lends_to_a_parameter();
    test_a_reference_cannot_borrow_a_temporary();
    test_a_reference_may_not_outlive_what_it_borrows();
    test_clone_belongs_to_the_owning_string();
    test_a_reference_to_a_header_reaches_the_characters();
    test_a_header_two_levels_out_reaches_the_characters();
    test_a_ref_string_reaches_the_characters_by_dereferencing();
    test_either_naming_compares_and_joins();

    return 0;
}
