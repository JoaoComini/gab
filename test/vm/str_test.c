// The borrowing string type and the name it goes by. 'str' names characters
// someone else owns: a literal's, or an owning 'string' lending its own. It is
// the same two slots as a 'string' and copies like one, so it is held by value
// rather than reached through an indirection.

#include "support/run.h"

#include <assert.h>

// 'str' is a builtin type name, resolvable wherever a type is written.
static void test_str_names_a_type() {
    assert(test_compiles("func f(s: str): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: str }\n"));
}

// A literal borrows the characters its unit holds, so 'str' is what it is.
static void test_a_literal_is_a_str() {
    assert(test_compiles("func f(): int { let s: str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));
}

// 'ref' means one thing everywhere: an indirection to what it qualifies.
static void test_ref_string_is_an_ordinary_indirection() {
    // 'ref' wraps whatever it qualifies, here as anywhere else: a 'ref String'
    // is the address of a slot holding a header, not the header itself. A
    // literal is a 'str' and no slot holds it, so it cannot fill one.
    assert(!test_compiles_on_vm("func f(): int { let s: ref String = \"hi\"; return 0; }\n"));

    assert(test_compiles_on_vm("func f(): int {\n"
                               "    let o: String = \"hi\".to_owned();\n"
                               "    let p: ref String = o;\n"
                               "    return 0;\n"
                               "}\n"));
}

// An owning string lends to a 'str': giving something up to be named costs
// nothing. The reverse would free characters the borrow never allocated.
static void test_a_string_lends_to_a_str() {
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let b: str = o;\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);

    assert(!test_compiles("func f(): int { let s: String = \"hi\"; return 0; }\n"));
}

// The lend reaches an argument too, so a function taking 'str' accepts either.
static void test_a_string_lends_to_a_str_parameter() {
    assert(test_run_int("func g(s: str): int { return s.len(); }\n"
                        "func f(): int { let o: String = \"hi\".to_owned(); return g(o); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func g(s: str): int { return s.len(); }\n"
                        "func f(): int { return g(\"abc\"); }\n"
                        "let r: int = f();") == 3);
}

// A borrow of characters nothing holds names memory freed where the expression
// ends, so the join has to be given a home before it can be lent.
static void test_a_str_cannot_borrow_a_temporary() {
    assert(!test_compiles("func f(): int { let s: str = \"a\" .. \"b\"; return 0; }\n"));
}

// A borrow may not outlive the string whose characters it names.
static void test_a_str_may_not_outlive_what_it_borrows() {
    assert(!test_compiles("func f(): str {\n"
                          "    let o: String = \"a\" .. \"b\";\n"
                          "    return o;\n"
                          "}\n"));
}

// 'clone' duplicates its receiver, so it lives on the type where that holds: a
// 'string' clones to a 'string'. A 'str' has none, since a borrow already
// copies by assignment and duplicating one would name the same characters.
static void test_clone_belongs_to_the_owning_string() {
    assert(test_run_bool("func f(): bool {\n"
                         "    let o: String = \"hi\".to_owned();\n"
                         "    let c: String = o.clone();\n"
                         "    return c == \"hi\";\n"
                         "}\n"
                         "let r: bool = f();") == true);

    assert(!test_compiles_on_vm("func f(): int {\n"
                                "    let s: str = \"hi\";\n"
                                "    let c: String = s.clone();\n"
                                "    return 0;\n"
                                "}\n"));
}

// 'to_owned' is the borrow's, since it is what a borrow needs and what an
// owning string already is.
static void test_to_owned_belongs_to_the_borrow() {
    assert(!test_compiles_on_vm("func f(): int {\n"
                                "    let o: String = \"hi\".to_owned();\n"
                                "    let c: String = o.to_owned();\n"
                                "    return 0;\n"
                                "}\n"));
}

// A 'str' and a 'String' are unrelated types, so nothing bridges their
// indirections: the two point at slots holding different headers, and no
// conversion between the pointers could be sound.
static void test_no_conversion_between_the_two_indirections() {
    assert(!test_compiles_on_vm("func g(s: ref str): int { return 0; }\n"
                                "func f(): int {\n"
                                "    let o: String = \"hi\".to_owned();\n"
                                "    let p: ref String = o;\n"
                                "    return g(p);\n"
                                "}\n"));

    // Nor may a 'String' fill one directly: no slot holds a 'str' header for
    // the pointer to name.
    assert(!test_compiles_on_vm("func g(s: ref str): int { return 0; }\n"
                                "func f(): int { let o: String = \"hi\".to_owned(); return g(o); }\n"));
}

// Reaching through a 'ref String' is spelled the way it is for any pointer: the
// deref names the 'String', which lends its characters from there.
static void test_a_ref_string_reaches_a_str_by_dereferencing() {
    assert(test_run_int("func g(s: str): int { return s.len(); }\n"
                        "func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let p: ref String = o;\n"
                        "    return g(*p);\n"
                        "}\n"
                        "let r: int = f();") == 2);

    // A receiver derefs on its own, as it does for any pointer to a value with
    // methods.
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let p: ref String = o;\n"
                        "    return p.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

int main(void) {
    test_str_names_a_type();
    test_a_literal_is_a_str();
    test_ref_string_is_an_ordinary_indirection();
    test_a_string_lends_to_a_str();
    test_a_string_lends_to_a_str_parameter();
    test_a_str_cannot_borrow_a_temporary();
    test_a_str_may_not_outlive_what_it_borrows();
    test_clone_belongs_to_the_owning_string();
    test_to_owned_belongs_to_the_borrow();
    test_no_conversion_between_the_two_indirections();
    test_a_ref_string_reaches_a_str_by_dereferencing();

    return 0;
}
