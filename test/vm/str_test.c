#include "support/run.h"
#include "type/type_registry.h"

#include <assert.h>

static void test_characters_are_reached_through_a_reference() {
    assert(test_run_int("func f(): int {\n"
                        "    let s: ref str = \"abc\";\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 3);
}

static void test_nothing_holds_the_characters_themselves() {
    assert(!test_compiles("func f(s: str): int { return 0; }\n"));
    assert(!test_compiles("struct Person { name: str }\n"));
    assert(!test_compiles("func f(): int { let s: str = \"hi\"; return 0; }\n"));
    assert(!test_compiles("func f(): str { return \"hi\"; }\n"));

    assert(test_compiles("func f(s: ref str): int { return 0; }\n"));
    assert(test_compiles("struct Person { name: ref str }\n"));
}

static void test_a_literal_names_borrowed_characters() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let s: String = \"hi\"; return 0; }\n"));
}

static void test_a_string_lends_a_reference_to_its_characters() {
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"hi\".to_owned();\n"
                        "    let b: ref str = o;\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

static void test_a_string_lends_to_a_parameter() {
    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int { let o: String = \"hi\".to_owned(); return g(o); }\n"
                        "let r: int = f();") == 2);

    assert(test_run_int("func g(s: ref str): int { return s.len(); }\n"
                        "func f(): int { return g(\"abc\"); }\n"
                        "let r: int = f();") == 3);
}

static void test_a_reference_cannot_borrow_a_temporary() {
    assert(!test_compiles("func f(): int { let s: ref str = \"ab\".to_owned(); return 0; }\n"));
}

static void test_a_reference_may_not_outlive_what_it_borrows() {
    assert(!test_compiles("func f(): ref str {\n"
                          "    let o: String = \"ab\".to_owned();\n"
                          "    return o;\n"
                          "}\n"));
}

static void test_clone_belongs_to_the_owning_string() {
    assert(test_run_bool("func f(): bool {\n"
                         "    let o: String = \"hi\".to_owned();\n"
                         "    let c: String = o.clone();\n"
                         "    return c == \"hi\";\n"
                         "}\n"
                         "let r: bool = f();") == true);

    assert(!test_compiles_on_vm("func f(): int {\n"
                                "    let s: ref str = \"hi\";\n"
                                "    let c: String = s.clone();\n"
                                "    return 0;\n"
                                "}\n"));
}

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

static void test_either_naming_compares_and_joins() {
    assert(test_run_bool("func f(): bool { let a: ref str = \"hi\"; return a == \"hi\"; }\n"
                         "let r: bool = f();") == true);

    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"abcd\".to_owned();\n"
                        "    return o.len();\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

static void test_a_lend_carries_the_length_not_the_capacity() {
    assert(test_run_int("func f(): int {\n"
                        "    let o: String = \"ab\".to_owned();\n"
                        "    o.push(99);\n"
                        "    let b: ref str = o;\n"
                        "    return b.len();\n"
                        "}\n"
                        "let r: int = f();") == 3);
}

static void test_a_lend_fits_the_reference_it_builds() {
    TestContext ctx;
    test_context_init(&ctx);

    VM *vm = vm_create();

    Scope *scope_ptr = &vm->env.global_scope;

    TypeRegistry *registry = scope_ptr->type_registry;

    const Type *string_type = scope_type_lookup(scope_ptr, string_from_cstr(&vm->env.strings, "String"));
    const Type *reference = type_registry_ref_to(registry, type_registry_get_primitive(registry, TYPE_STR));

    const Deref *deref = type_registry_deref(registry, string_type);

    assert(deref && deref->to == type_registry_get_primitive(registry, TYPE_STR));

    assert(deref->part_count == 2);

    size_t total = 0;

    for (size_t i = 0; i < deref->part_count; i++) {
        assert(deref->parts[i].offset + deref->parts[i].size <= type_registry_size_of(registry, string_type));

        total += deref->parts[i].size;
    }

    assert(total <= type_registry_size_of(registry, reference));

    assert(!type_registry_deref(registry, reference));

    test_context_free(&ctx);
    vm_free(vm);
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
