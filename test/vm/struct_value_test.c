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

// Each field must be independently addressable: writing one must not disturb
// the others.
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

    // And all three at once, so a clobber that happens to preserve one field
    // still shows up.
    snprintf(source, sizeof(source), body, "v.x + v.y + v.z");
    assert(test_run_float(source) == 7.0f);
}

// Four bools share a single slot. This is the case a slot-granular store gets
// wrong: writing one field would blank its three neighbours.
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

// The padding case from step 3, now exercised at runtime: 'value' sits on the
// next slot boundary, not immediately after the bool.
static void test_mixed_widths() {
    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): bool { let v: M; v.flag = true; v.value = 77; return v.flag; }\n"
                        "let r: bool = f();") == 1);

    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): int { let v: M; v.flag = true; v.value = 77; return v.value; }\n"
                        "let r: int = f();") == 77);

    // Writing the wide field must not disturb the narrow one sharing the
    // struct, in either order.
    assert(test_run_int("struct M { flag: bool, value: int }\n"
                        "func f(): bool { let v: M; v.value = 77; v.flag = true; return v.flag; }\n"
                        "let r: bool = f();") == 1);
}

// A nested struct is stored inline, so the chain resolves to one base slot plus
// a summed byte offset.
static void test_nested_field_access() {
    assert(test_run_int("struct In { x: int, y: int }\n"
                        "struct Out { a: int, inner: In }\n"
                        "func f(): int { let v: Out;\n"
                        "v.a = 1; v.inner.x = 5; v.inner.y = 9;\n"
                        "return v.a + v.inner.x + v.inner.y; }\n"
                        "let r: int = f();") == 15);
}

// Whole-struct assignment copies every field and leaves the source alone: the
// by-value guarantee.
static void test_whole_struct_assignment() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                        "let a: V = b;\n"
                        "return a.x + a.y; }\n"
                        "let r: int = f();") == 7);

    // Mutating the copy must not reach back into the original.
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                        "let a: V = b; a.x = 99;\n"
                        "return b.x; }\n"
                        "let r: int = f();") == 3);
}

// Each frame owns its slots, so a struct local in a recursive function must not
// be shared between invocations.
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

// The by-value guarantee on the way in: the callee gets its own copy, so
// mutating the parameter must not reach the caller's struct.
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

// The overlap case: the callee's parameters and its return slots share the
// space above dest. Safe only because the parameters are dead by the time
// OP_RETURN copies the result down to r0.
static void test_function_takes_and_returns_structs() {
    assert(test_run_int("struct V { x: int, y: int }\n"
                        "func twice(v: V): V { let o: V; o.x = v.x + v.x; o.y = v.y + v.y; return o; }\n"
                        "func main(): int { let a: V; a.x = 3; a.y = 4;\n"
                        "let b: V = twice(a);\n"
                        "return b.x + b.y; }\n"
                        "let r: int = main();") == 14);
}

// A return wider than the argument block must still fit what the caller
// reserved at dest, which is why the reservation takes the max of the two.
static void test_struct_return_larger_than_arguments() {
    assert(test_run_int("struct Big { a: int, b: int, c: int, d: int }\n"
                        "func make(n: int): Big { let v: Big;\n"
                        "v.a = n; v.b = n + 1; v.c = n + 2; v.d = n + 3;\n"
                        "return v; }\n"
                        "func main(): int { let g: Big = make(10); return g.a + g.b + g.c + g.d; }\n"
                        "let r: int = main();") == 46);
}

// Several arguments of different widths: each must land at its own running
// offset rather than at a fixed one-slot-per-argument stride.
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

// A struct handed down and back up a recursion: every frame needs its own copy
// on both legs.
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

// Finds the struct's slots in the stack and compares them, byte for byte, with
// the equivalent C value. The function's frame is based at stack[0] with r0 as
// the return slot, so a single struct local starts at slot 1; searching rather
// than hard-coding keeps the test from pinning the allocator's exact choices.
static bool slots_match(VM *vm, const void *expected, size_t size) {
    size_t slots = (size + sizeof(Value) - 1) / sizeof(Value);

    for (size_t base = 0; base + slots <= vm->stack_capacity; base++) {
        if (memcmp(vm_slot(vm, base), expected, size) == 0) {
            return true;
        }
    }

    return false;
}

static void assert_slots_match(VM *vm, const void *expected, size_t size) {
    assert(slots_match(vm, expected, size) && "no run of slots matches the equivalent C struct");
}

// The whole point of untagging Value: a struct spread over consecutive slots is
// byte-identical to what C lays out, so gab_struct_data can hand a host a
// pointer into the stack with no marshalling at all.
static void test_layout_agrees_with_c() {
    struct Vec3 {
        float x;
        float y;
        float z;
    };

    VM *vm = vm_create();

    vm_execute(vm, "struct Vec3 { x: float, y: float, z: float }\n"
                   "func f(): float { let v: Vec3;\n"
                   "v.x = 1.5; v.y = 2.25; v.z = 7.0;\n"
                   "return v.x; }\n"
                   "let r: float = f();");

    struct Vec3 expected = {.x = 1.5f, .y = 2.25f, .z = 7.0f};
    assert_slots_match(vm, &expected, sizeof expected);

    vm_free(vm);
}

// The padding case: C puts 'value' on the next 4-byte boundary, leaving three
// bytes of padding after 'flag'. The Gab slots must agree, padding included.
static void test_mixed_width_layout_agrees_with_c() {
    struct Mixed {
        bool flag;
        int32_t value;
    };

    VM *vm = vm_create();

    // The padding bytes are whatever the zeroed stack left them, so the C value
    // is zeroed first to match rather than carrying stack garbage.
    vm_execute(vm, "struct M { flag: bool, value: int }\n"
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

// The positive layout tests only mean something if the search discriminates:
// a struct whose bytes differ from Gab's must match nowhere in the stack.
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

    vm_execute(vm, "struct Vec3 { x: float, y: float, z: float }\n"
                   "struct M { flag: bool, value: int }\n"
                   "func f(): float { let v: Vec3; let m: M;\n"
                   "v.x = 1.5; v.y = 2.25; v.z = 7.0;\n"
                   "m.flag = true; m.value = 305419896;\n"
                   "return v.x; }\n"
                   "let r: float = f();");

    // Same three values, wrong field order.
    struct Shuffled shuffled = {.z = 7.0f, .x = 1.5f, .y = 2.25f};
    assert(!slots_match(vm, &shuffled, sizeof shuffled));

    // Same two values, no padding after 'flag'.
    struct Packed packed;
    memset(&packed, 0, sizeof packed);
    packed.flag = true;
    packed.value = 305419896;
    assert(sizeof packed == 5);
    assert(!slots_match(vm, &packed, sizeof packed));

    vm_free(vm);
}

// A struct assigned from a struct that shares its slots. Batching the copy into
// one instruction makes the source and destination ranges overlap, which a
// memcpy is free to get wrong; this is why OP_MOVE_N uses memmove. The nested
// copy walks the same slots in both directions, so a wrong one shears the value
// rather than failing outright.
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
