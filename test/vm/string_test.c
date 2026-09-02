#include "object.h"
#include "support/run.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_string_names_a_type() {
    assert(test_compiles_on_vm("import std;\n"
                               "func f(s: &String): int { return 0; }\n"));
    assert(test_compiles_on_vm("import std;\n"
                               "struct Person { name: String }\n"));
}

static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): int { let s: &str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));
}

static void test_a_string_is_an_address_and_a_length() {
    TestContext ctx;
    test_context_init(&ctx);

    VM *vm = vm_create();

    Scope *scope_ptr = test_std_scope(vm);

    const Type *string_type = scope_type_lookup(scope_ptr, string_from_cstr(&vm->env.strings, "String"));

    const TypeLayout *layout = type_registry_layout_of(scope_ptr->type_registry, string_type);

    assert(layout->size == sizeof(GabStringValue));
    assert(layout->alignment == _Alignof(GabStringValue));

    assert(layout->size == VM_STRING_SLOTS * VM_SLOT_SIZE);

    test_context_free(&ctx);
    vm_free(vm);
}

static void test_a_string_is_one_owning_field() {
    TestContext ctx;
    test_context_init(&ctx);

    VM *vm = vm_create();

    Scope *scope_ptr = test_std_scope(vm);

    const Type *string_type = scope_type_lookup(scope_ptr, string_from_cstr(&vm->env.strings, "String"));

    assert(type_registry_fields_of(scope_ptr->type_registry, string_type)->count == 1);

    const TypeField *data = type_registry_find_field(scope_ptr->type_registry, string_type,
                                                     string_from_cstr(&vm->env.strings, "data"));

    assert(data);

    const TypeLayout *layout = type_registry_layout_of(scope_ptr->type_registry, string_type);

    assert(layout->offsets[data - type_registry_fields_of(scope_ptr->type_registry, string_type)->fields] ==
           offsetof(GabStringValue, block));

    assert(type_registry_owns(scope_ptr->type_registry, data->type));

    test_context_free(&ctx);
    vm_free(vm);
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

static void test_equal_characters_at_different_addresses() {
    assert(test_run_bool("import std;\n"
                         "func f(): bool {\n"
                         "    let o: String = String::from(\"hi\");\n"
                         "    let a: &str = o;\n"
                         "    let b: &str = \"hi\";\n"
                         "    return a == b;\n"
                         "}\n"
                         "let r: bool = f();") == true);
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
    TestProgram program = test_compile("func f(): int { let s: &str = \"a\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_a_struct_field_borrows_its_characters() {
    assert(test_compiles("struct Person { name: &str }\n"));

    TestProgram program = test_compile("struct Person { name: &str }\n"
                                       "func f(): int { let p = Person { name: \"\" }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_an_owning_string_field_is_released() {
    TestProgram program =
        test_compile("import std;\n"
                     "struct Doc { body: String }\n"
                     "func f(a: &str): int { let d = Doc { body: String::from(a) }; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) > 0);

    test_program_free(&program);

    assert(test_run_bool(
               "import std;\n"
               "struct Doc { body: String }\n"
               "func f(a: &str): bool { let d = Doc { body: String::from(a) }; return d.body == \"ab\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

static void test_binding_an_owning_string_transfers_it() {
    assert(!test_compiles_on_vm("import std;\n"
                                "func f(v: &str): int { let a: String = String::from(v); let b: String = a; "
                                "return a.len(); }\n"));

    assert(test_compiles_on_vm(
        "import std;\n"
        "func f(v: &str): int { let a: String = String::from(v); let b: String = a; return b.len(); }\n"));
}

static void test_reassigning_a_string_frees_the_old_characters() {
    assert(test_run_bool(
               "import std;\n"
               "func f(a: &str): bool { let s: String = String::from(a); s = String::from(\"d\"); return s "
               "== \"d\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

static void test_a_string_declared_empty_is_freed_once() {
    assert(test_run_int("import std;\n"
                        "func f(): int {\n"
                        "    let s: String = String::from(\"\");\n"
                        "    s = String::from(\"ab\");\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

static void test_a_new_string_is_empty() {
    assert(test_run_int("import std;\n"
                        "func f(): int { let s: *String = box String::from(\"\"); return (*s).len(); }\n"
                        "let r: int = f();") == 0);

    assert(
        test_run_bool("import std;\n"
                      "func f(): bool { let s: *String = box String::from(\"\"); return (*s).is_empty(); }\n"
                      "let r: bool = f();") == true);
}

static void test_a_boxed_string_holds_what_is_stored_through_it() {
    assert(test_run_bool(
               "import std;\n"
               "func f(a: &str): bool { let s: *String = box String::from(\"\"); *s = String::from(a); "
               "return *s == \"ab\"; }\n"
               "let r: bool = f(\"ab\");") == true);

    assert(test_run_bool(
               "import std;\n"
               "func f(a: &str): bool { let s: *String = box String::from(\"\"); *s = String::from(a); *s = "
               "String::from(\"d\"); "
               "return *s == \"d\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

static void test_a_heap_struct_frees_its_string_field() {
    assert(test_run_bool("import std;\n"
                         "struct D { b: String }\n"
                         "func f(a: &str): bool { let d: *D = box D { b: String::from(\"\") }; d.b = "
                         "String::from(a); return d.b == \"ab\"; }\n"
                         "let r: bool = f(\"ab\");") == true);
}

static void test_an_owning_string_slot_refuses_a_borrow() {
    assert(!test_compiles_on_vm(
        "import std;\n"
        "func f(): int { let s: *String = box String::from(\"\"); *s = \"ab\"; return 0; }\n"));

    assert(!test_compiles_on_vm(
        "import std;\n"
        "func f(a: &str): int { let s: *String = box String::from(\"\"); *s = a; return 0; }\n"));
}

static void test_refusing_a_borrow_names_the_remedy() {
    assert(!test_compiles_on_vm("import std;\n"
                                "func f(): int { let a: String = \"hi\"; return 0; }\n"));

    assert(test_compiles("func f(): int { let a: &str = \"hi\"; return 0; }\n"));
}

static void test_a_returnable_borrow_outlives_its_frame() {
    assert(test_compiles("func f(a: &str): &str { return a; }\n"));

    assert(test_compiles("func f(): &str { return \"hi\"; }\n"));

    assert(test_run_bool("func f(a: &str): &str { return a; }\n"
                         "func g(): bool { return f(\"hi\") == \"hi\"; }\n"
                         "let r: bool = g();") == true);
}

static void test_a_literal_borrows() {
    assert(test_compiles("func f(): int { let s: &str = \"hi\"; return 0; }\n"));
}

static void test_an_owning_string_refuses_a_borrow() {
    assert(!test_compiles_on_vm("import std;\n"
                                "func f(): int { let s: String = \"hi\"; return 0; }\n"));
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
    test_binding_an_owning_string_transfers_it();
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
