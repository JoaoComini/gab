#include "object.h"
#include "support/run.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_string_names_a_type() {
    assert(test_compiles_on_vm("func f(s: ref String): int { return 0; }\n"));
    assert(test_compiles_on_vm("struct Person { name: String }\n"));
}

static void test_a_literal_is_a_string() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));

    assert(!test_compiles("func f(): int { let n: int = \"hi\"; return 0; }\n"));
}

static void test_a_string_is_an_address_and_a_length() {
    TestContext ctx;
    test_context_init(&ctx);

    VM *vm = vm_create();

    Scope *scope_ptr = &vm->env.global_scope;

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

    Scope *scope_ptr = &vm->env.global_scope;

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

    test_run_string("let s: ref str = \"a\\nb\";", text, sizeof(text), &length);

    assert(length == 3);
    assert(memcmp(text, "a\nb", 3) == 0);
}

static void test_equal_strings_compare_equal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"hi\"; return a == b; }\n"
                      "let r: bool = f();") == true);

    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"ho\"; return a == b; }\n"
                      "let r: bool = f();") == false);
}

static void test_equal_characters_at_different_addresses() {
    assert(test_run_bool("func f(): bool {\n"
                         "    let o: String = \"hi\".to_owned();\n"
                         "    let a: ref str = o;\n"
                         "    let b: ref str = \"hi\";\n"
                         "    return a == b;\n"
                         "}\n"
                         "let r: bool = f();") == true);
}

static void test_a_prefix_is_not_equal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"hit\"; return a == b; }\n"
                      "let r: bool = f();") == false);
}

static void test_strings_compare_unequal() {
    assert(
        test_run_bool("func f(): bool { let a: ref str = \"hi\"; let b: ref str = \"ho\"; return a != b; }\n"
                      "let r: bool = f();") == true);
}

static void test_a_null_is_compared_like_any_character() {
    assert(test_run_bool("func f(): bool { let a: ref str = \"a\\0b\"; let b: ref str = \"a\\0c\"; "
                         "return a == b; }\n"
                         "let r: bool = f();") == false);
}

static void test_strings_are_not_ordered() {
    assert(!test_compiles("func f(): bool { let a: ref str = \"a\"; return a < a; }\n"));
}

static void test_a_literal_is_not_released() {
    TestProgram program = test_compile("func f(): int { let s: ref str = \"a\"; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_a_struct_field_borrows_its_characters() {
    assert(test_compiles("struct Person { name: ref str }\n"));

    TestProgram program = test_compile("struct Person { name: ref str }\n"
                                       "func f(): int { let p: Person; return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) == 0);

    test_program_free(&program);
}

static void test_an_owning_string_field_is_released() {
    TestProgram program =
        test_compile("struct Doc { body: String }\n"
                     "func f(a: ref str): int { let d: Doc; d.body = a.to_owned(); return 0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_RELEASE) > 0);

    test_program_free(&program);

    assert(test_run_bool(
               "struct Doc { body: String }\n"
               "func f(a: ref str): bool { let d: Doc; d.body = a.to_owned(); return d.body == \"ab\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

static void test_binding_an_owning_string_transfers_it() {
    assert(!test_compiles_on_vm("func f(v: ref str): int { let a: String = v.to_owned(); let b: String = a; "
                                "return a.len(); }\n"));

    assert(test_compiles_on_vm(
        "func f(v: ref str): int { let a: String = v.to_owned(); let b: String = a; return b.len(); }\n"));
}

static void test_reassigning_a_string_frees_the_old_characters() {
    assert(test_run_bool(
               "func f(a: ref str): bool { let s: String = a.to_owned(); s = \"d\".to_owned(); return s "
               "== \"d\"; }\n"
               "let r: bool = f(\"ab\");") == true);
}

static void test_a_string_declared_empty_is_freed_once() {
    assert(test_run_int("func f(): int {\n"
                        "    let s: String;\n"
                        "    s = \"ab\".to_owned();\n"
                        "    return s.len();\n"
                        "}\n"
                        "let r: int = f();") == 2);
}

static void test_a_new_string_is_empty() {
    assert(test_run_int("func f(): int { let s: box String = new String; return (*s).len(); }\n"
                        "let r: int = f();") == 0);

    assert(test_run_bool("func f(): bool { let s: box String = new String; return (*s).is_empty(); }\n"
                         "let r: bool = f();") == true);
}

static void test_a_boxed_string_holds_what_is_stored_through_it() {
    assert(test_run_bool("func f(a: ref str): bool { let s: box String = new String; *s = a.to_owned(); "
                         "return *s == \"ab\"; }\n"
                         "let r: bool = f(\"ab\");") == true);

    assert(test_run_bool("func f(a: ref str): bool { let s: box String = new String; *s = a.to_owned(); *s = "
                         "\"d\".to_owned(); "
                         "return *s == \"d\"; }\n"
                         "let r: bool = f(\"ab\");") == true);
}

static void test_a_heap_struct_frees_its_string_field() {
    assert(
        test_run_bool(
            "struct D { b: String }\n"
            "func f(a: ref str): bool { let d: box D = new D; d.b = a.to_owned(); return d.b == \"ab\"; }\n"
            "let r: bool = f(\"ab\");") == true);
}

static void test_an_owning_string_slot_refuses_a_borrow() {
    assert(
        !test_compiles_on_vm("func f(): int { let s: box String = new String; *s = \"ab\"; return 0; }\n"));

    assert(!test_compiles_on_vm(
        "func f(a: ref str): int { let s: box String = new String; *s = a; return 0; }\n"));
}

static void test_refusing_a_borrow_names_the_remedy() {
    assert(!test_compiles_on_vm("func f(): int { let a: String = \"hi\"; return 0; }\n"));

    assert(test_compiles("func f(): int { let a: ref str = \"hi\"; return 0; }\n"));
}

static void test_a_returnable_borrow_outlives_its_frame() {
    assert(test_compiles("func f(a: ref str): ref str { return a; }\n"));

    assert(test_compiles("func f(): ref str { return \"hi\"; }\n"));

    assert(test_run_bool("func f(a: ref str): ref str { return a; }\n"
                         "func g(): bool { return f(\"hi\") == \"hi\"; }\n"
                         "let r: bool = g();") == true);
}

static void test_a_literal_borrows() {
    assert(test_compiles("func f(): int { let s: ref str = \"hi\"; return 0; }\n"));
}

static void test_an_owning_string_refuses_a_borrow() {
    assert(!test_compiles_on_vm("func f(): int { let s: String = \"hi\"; return 0; }\n"));
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
