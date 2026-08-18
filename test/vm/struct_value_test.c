#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Runs a script and returns the slot the top-level result lands in.
static Value run(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    Value result = vm->stack[0];

    vm_free(vm);

    return result;
}

static int run_int(const char *source) { return run(source).as_int; }

static float run_float(const char *source) { return run(source).as_float; }

static void test_read_back_what_was_written() {
    assert(run_float("struct Vec3 { x: float, y: float, z: float }\n"
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
    assert(run_float(source) == 1.0f);

    snprintf(source, sizeof(source), body, "v.y");
    assert(run_float(source) == 2.0f);

    snprintf(source, sizeof(source), body, "v.z");
    assert(run_float(source) == 4.0f);

    // And all three at once, so a clobber that happens to preserve one field
    // still shows up.
    snprintf(source, sizeof(source), body, "v.x + v.y + v.z");
    assert(run_float(source) == 7.0f);
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
    assert(run_int(source) == 1);

    snprintf(source, sizeof(source), body, "v.b");
    assert(run_int(source) == 0);

    snprintf(source, sizeof(source), body, "v.c");
    assert(run_int(source) == 1);

    snprintf(source, sizeof(source), body, "v.d");
    assert(run_int(source) == 1);
}

// The padding case from step 3, now exercised at runtime: 'value' sits on the
// next slot boundary, not immediately after the bool.
static void test_mixed_widths() {
    assert(run_int("struct M { flag: bool, value: int }\n"
                   "func f(): bool { let v: M; v.flag = true; v.value = 77; return v.flag; }\n"
                   "let r: bool = f();") == 1);

    assert(run_int("struct M { flag: bool, value: int }\n"
                   "func f(): int { let v: M; v.flag = true; v.value = 77; return v.value; }\n"
                   "let r: int = f();") == 77);

    // Writing the wide field must not disturb the narrow one sharing the
    // struct, in either order.
    assert(run_int("struct M { flag: bool, value: int }\n"
                   "func f(): bool { let v: M; v.value = 77; v.flag = true; return v.flag; }\n"
                   "let r: bool = f();") == 1);
}

// A nested struct is stored inline, so the chain resolves to one base slot plus
// a summed byte offset.
static void test_nested_field_access() {
    assert(run_int("struct In { x: int, y: int }\n"
                   "struct Out { a: int, inner: In }\n"
                   "func f(): int { let v: Out;\n"
                   "v.a = 1; v.inner.x = 5; v.inner.y = 9;\n"
                   "return v.a + v.inner.x + v.inner.y; }\n"
                   "let r: int = f();") == 15);
}

// Whole-struct assignment copies every field and leaves the source alone: the
// by-value guarantee.
static void test_whole_struct_assignment() {
    assert(run_int("struct V { x: int, y: int }\n"
                   "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                   "let a: V = b;\n"
                   "return a.x + a.y; }\n"
                   "let r: int = f();") == 7);

    // Mutating the copy must not reach back into the original.
    assert(run_int("struct V { x: int, y: int }\n"
                   "func f(): int { let b: V; b.x = 3; b.y = 4;\n"
                   "let a: V = b; a.x = 99;\n"
                   "return b.x; }\n"
                   "let r: int = f();") == 3);
}

// Each frame owns its slots, so a struct local in a recursive function must not
// be shared between invocations.
static void test_struct_local_is_per_frame() {
    assert(run_int("struct Acc { total: int }\n"
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

// Finds the struct's slots in the stack and compares them, byte for byte, with
// the equivalent C value. The function's frame is based at stack[0] with r0 as
// the return slot, so a single struct local starts at slot 1; searching rather
// than hard-coding keeps the test from pinning the allocator's exact choices.
static void assert_slots_match(VM *vm, const void *expected, size_t size) {
    size_t slots = (size + sizeof(Value) - 1) / sizeof(Value);

    for (size_t base = 0; base + slots <= vm->stack_capacity; base++) {
        if (memcmp(&vm->stack[base], expected, size) == 0) {
            return;
        }
    }

    assert(0 && "no run of slots matches the equivalent C struct");
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

int main() {
    test_read_back_what_was_written();
    test_every_field_is_independent();
    test_sub_word_fields_do_not_clobber();
    test_mixed_widths();
    test_nested_field_access();
    test_whole_struct_assignment();
    test_struct_local_is_per_frame();
    test_layout_agrees_with_c();
    test_mixed_width_layout_agrees_with_c();

    printf("struct_value_test: all tests passed\n");

    return 0;
}
