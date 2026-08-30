#include "compile.h"
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_read_back_what_was_written() {
    assert(test_run_float("struct Vec3 { x: float, y: float, z: float }\n"
                          "func f(): float { let v: Vec3; v.x = 1.5; return v.x; }\n"
                          "let r: float = f();") == 1.5f);
}

static void test_every_field_is_independent() {
    const char *body = "struct Vec3 { x: float, y: float, z: float }\n"
                       "func f(): float { let v: Vec3;\n"
                       "v.x = 1.0; v.y = 2.0; v.z = 4.0;\n"
                       "return %s; }\n"
                       "let r: float = f();";

    char source[512];

    snprintf(source, sizeof(source), body, "v.x");
    assert(test_run_float(source) == 1.0f);

    snprintf(source, sizeof(source), body, "v.y");
    assert(test_run_float(source) == 2.0f);

    snprintf(source, sizeof(source), body, "v.z");
    assert(test_run_float(source) == 4.0f);

    snprintf(source, sizeof(source), body, "v.x + v.y + v.z");
    assert(test_run_float(source) == 7.0f);
}

static void test_sub_word_fields_do_not_clobber() {
    const char *body = "struct Flags { a: bool, b: bool, c: bool, d: bool }\n"
                       "func f(): bool { let v: Flags;\n"
                       "v.a = true; v.b = false; v.c = true; v.d = true;\n"
                       "return %s; }\n"
                       "let r: bool = f();";

    char source[512];

    snprintf(source, sizeof(source), body, "v.a");
    assert(test_run_int(source) == 1);

    snprintf(source, sizeof(source), body, "v.b");
    assert(test_run_int(source) == 0);

    snprintf(source, sizeof(source), body, "v.c");
    assert(test_run_int(source) == 1);

    snprintf(source, sizeof(source), body, "v.d");
    assert(test_run_int(source) == 1);
}

static void test_mixed_widths() {
    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): bool { let v: M; v.flag = true; v.value = 77; return v.flag; }\n"
                        "let r: bool = f();") == 1);

    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): int { let v: M; v.flag = true; v.value = 77; return v.value; }\n"
                        "let r: int = f();") == 77);

    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): bool { let v: M; v.value = 77; v.flag = true; return v.flag; }\n"
                        "let r: bool = f();") == 1);
}

static void test_nested_field_access() {
    assert(test_run_int("struct In { x: int, y: int }\n"
                        "struct Out { a: int, inner: In }\n"
                        "func f(): int { let v: Out;\n"
                        "v.a = 1; v.inner.x = 5; v.inner.y = 9;\n"
                        "return v.a + v.inner.x + v.inner.y; }\n"
                        "let r: int = f();") == 15);
}

static void test_whole_struct_assignment() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                        "let a: V = b;\n"
                        "return a.x + a.y; }\n"
                        "let r: int = f();") == 7);

    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                        "let a: V = b; a.x = 99;\n"
                        "return b.x; }\n"
                        "let r: int = f();") == 3);
}

static void test_struct_local_is_per_frame() {
    assert(test_run_int("struct Acc { total: int }\n"
                        "func sum(n: int): int {\n"
                        "let a: Acc;\n"
                        "a.total = n;\n"
                        "if n <= 0 { return 0; }\n"
                        "let rest: int = sum(n - 1);\n"
                        "return a.total + rest;\n"
                        "}\n"
                        "func main(): int { return sum(4); }\n"
                        "let r: int = main();") == 10);
}

static void test_struct_parameter() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func sum(v: V): int { return v.x + v.y; }\n"
                        "func main(): int { let a: V; a.x = 3; a.y = 4; return sum(a); }\n"
                        "let r: int = main();") == 7);
}

static void test_struct_parameter_is_by_value() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func bump(v: V): int { v.x = 999; return v.x; }\n"
                        "func main(): int { let a: V; a.x = 3; a.y = 4;\n"
                        "let ignored: int = bump(a);\n"
                        "return a.x; }\n"
                        "let r: int = main();") == 3);
}

static void test_struct_return() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func make(): V { let v: V; v.x = 11; v.y = 22; return v; }\n"
                        "func main(): int { let g: V = make(); return g.x + g.y; }\n"
                        "let r: int = main();") == 33);
}

static void test_function_takes_and_returns_structs() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func twice(v: V): V { let o: V; o.x = v.x + v.x; o.y = v.y + v.y; return o; }\n"
                        "func main(): int { let a: V; a.x = 3; a.y = 4;\n"
                        "let b: V = twice(a);\n"
                        "return b.x + b.y; }\n"
                        "let r: int = main();") == 14);
}

static void test_struct_return_larger_than_arguments() {
    assert(test_run_int("struct Big { a: int, b: int, c: int, d: int }\n"
                        "func make(n: int): Big { let v: Big;\n"
                        "v.a = n; v.b = n + 1; v.c = n + 2; v.d = n + 3;\n"
                        "return v; }\n"
                        "func main(): int { let g: Big = make(10); return g.a + g.b + g.c + g.d; }\n"
                        "let r: int = main();") == 46);
}

static void test_mixed_scalar_and_struct_arguments() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func f(n: int, v: V, m: int): int { return n + v.x + v.y + m; }\n"
                        "func main(): int { let a: V; a.x = 5; a.y = 6; return f(1, a, 100); }\n"
                        "let r: int = main();") == 112);

    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func add(a: V, b: V): int { return a.x + a.y + b.x + b.y; }\n"
                        "func main(): int { let p: V; p.x = 1; p.y = 2;\n"
                        "let q: V; q.x = 10; q.y = 20;\n"
                        "return add(p, q); }\n"
                        "let r: int = main();") == 33);
}

static void test_struct_round_trip_through_recursion() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func go(n: int, v: V): V {\n"
                        "if n <= 0 { return v; }\n"
                        "let w: V; w.x = v.x + 1; w.y = v.y + 2;\n"
                        "return go(n - 1, w); }\n"
                        "func main(): int { let a: V; a.x = 0; a.y = 0;\n"
                        "let b: V = go(3, a);\n"
                        "return b.x * 100 + b.y; }\n"
                        "let r: int = main();") == 306);
}

static bool slots_match(VM *vm, const void *expected, size_t size) {
    size_t slots = (size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE;

    for (size_t base = 0; base + slots <= vm->stack_capacity; base++) {
        if (memcmp(vm_slot_at(vm, base), expected, size) == 0) {
            return true;
        }
    }

    return false;
}

static void assert_slots_match(VM *vm, const void *expected, size_t size) {
    assert(slots_match(vm, expected, size) && "no run of slots matches the equivalent C struct");
}

static void test_layout_agrees_with_c() {
    struct Vec3 {
        float x;
        float y;
        float z;
    };

    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct Vec3 { x: float, y: float, z: float }\n"
                        "func f(): float { let v: Vec3;\n"
                        "v.x = 1.5; v.y = 2.25; v.z = 7.0;\n"
                        "return v.x; }\n"
                        "let r: float = f();");

    struct Vec3 expected = {.x = 1.5f, .y = 2.25f, .z = 7.0f};
    assert_slots_match(vm, &expected, sizeof expected);

    vm_free(vm);
}

static void test_mixed_width_layout_agrees_with_c() {
    struct Mixed {
        bool flag;
        int32_t value;
    };

    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct M { flag: bool, value: int }\n"
                        "func f(): int { let v: M;\n"
                        "v.flag = true; v.value = 305419896;\n"
                        "return v.value; }\n"
                        "let r: int = f();");

    struct Mixed expected;
    memset(&expected, 0, sizeof expected);
    expected.flag = true;
    expected.value = 305419896;

    assert_slots_match(vm, &expected, sizeof expected);

    vm_free(vm);
}

static void test_layout_comparison_rejects_wrong_layouts() {
    struct Shuffled {
        float z;
        float x;
        float y;
    };

    struct __attribute__((packed)) Packed {
        bool flag;
        int32_t value;
    };

    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct Vec3 { x: float, y: float, z: float }\n"
                        "struct M { flag: bool, value: int }\n"
                        "func f(): float { let v: Vec3; let m: M;\n"
                        "v.x = 1.5; v.y = 2.25; v.z = 7.0;\n"
                        "m.flag = true; m.value = 305419896;\n"
                        "return v.x; }\n"
                        "let r: float = f();");

    struct Shuffled shuffled = {.z = 7.0f, .x = 1.5f, .y = 2.25f};
    assert(!slots_match(vm, &shuffled, sizeof shuffled));

    struct Packed packed;
    memset(&packed, 0, sizeof packed);
    packed.flag = true;
    packed.value = 305419896;
    assert(sizeof packed == 5);
    assert(!slots_match(vm, &packed, sizeof packed));

    vm_free(vm);
}

static void test_an_overlapping_struct_copy_is_exact() {
    assert(test_run_int("struct Inner { a: int, b: int, c: int }\n"
                        "struct Outer { head: int, inner: Inner }\n"
                        "func f(): int {\n"
                        "    let o: Outer;\n"
                        "    o.head = 1;\n"
                        "    o.inner.a = 2; o.inner.b = 3; o.inner.c = 4;\n"
                        "    let copy: Inner = o.inner;\n"
                        "    o.inner = copy;\n"
                        "    return o.head * 1000 + o.inner.a * 100 + o.inner.b * 10 + o.inner.c;\n"
                        "}\n"
                        "let r: int = f();") == 1234);
}

int main() {
    test_read_back_what_was_written();
    test_every_field_is_independent();
    test_sub_word_fields_do_not_clobber();
    test_mixed_widths();
    test_nested_field_access();
    test_whole_struct_assignment();
    test_an_overlapping_struct_copy_is_exact();
    test_struct_local_is_per_frame();
    test_struct_parameter();
    test_struct_parameter_is_by_value();
    test_struct_return();
    test_function_takes_and_returns_structs();
    test_struct_return_larger_than_arguments();
    test_mixed_scalar_and_struct_arguments();
    test_struct_round_trip_through_recursion();
    test_layout_agrees_with_c();
    test_mixed_width_layout_agrees_with_c();
    test_layout_comparison_rejects_wrong_layouts();

    printf("struct_value_test: all tests passed\n");

    return 0;
}
