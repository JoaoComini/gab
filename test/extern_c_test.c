#include "gab.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int32_t call_int(const char *decl, const char *body, const char *name, void *symbol, bool wants_ctx) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(wants_ctx ? gab_extern_c_ctx(vm, "test", NULL, name, symbol, &err)
                     : gab_extern_c(vm, "test", NULL, name, symbol, &err));

    char source[512];
    snprintf(source, sizeof source, "module test;\n%s\n%s\n", decl, body);

    assert(gab_load(vm, "<m>", source, &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);

    gab_call_free(call);
    gab_vm_free(vm);

    return result;
}

static void test_an_int_argument_and_result_cross_to_c(void) {
    assert(call_int("extern func abs(x: int): int;", "func run(): int { return abs(0 - 7); }", "abs",
                    (void *)(uintptr_t)abs, false) == 7);
}

static int32_t subtract(int32_t a, int32_t b) { return a - b; }

static void test_arguments_arrive_in_order(void) {
    assert(call_int("extern func sub(a: int, b: int): int;", "func run(): int { return sub(10, 3); }", "sub",
                    (void *)(uintptr_t)subtract, false) == 7);
}

static void test_a_float_crosses_to_c(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "root", (void *)(uintptr_t)sqrtf, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func root(x: float): float;\n"
                    "func run(): float { return root(9.0); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    float result = 0.0f;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 3.0f);

    gab_call_free(call);
    gab_vm_free(vm);
}

static int32_t counted = 0;

static void bump(void) { counted++; }

static void test_a_c_symbol_may_return_nothing(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    counted = 0;

    assert(gab_extern_c(vm, "test", NULL, "bump", (void *)(uintptr_t)bump, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func bump();\n"
                    "func run() { bump(); bump(); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(counted == 2);

    gab_call_free(call);
    gab_vm_free(vm);
}

typedef struct {
    int32_t a;
    int32_t b;
} Pair;

static int32_t pair_sum(Pair p) { return p.a + p.b; }

static void test_a_struct_crosses_by_value(void) {
    assert(call_int("struct Pair { a: int, b: int }\n"
                    "extern func sum(p: Pair): int;",
                    "func run(): int { return sum(Pair { a: 4, b: 5 }); }", "sum",
                    (void *)(uintptr_t)pair_sum, false) == 9);
}

typedef struct {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
} Quad;

static Quad quad_reversed(Quad q) { return (Quad){.a = q.d, .b = q.c, .c = q.b, .d = q.a}; }

static void test_a_struct_comes_back_by_value(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "reverse", (void *)(uintptr_t)quad_reversed, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Quad { a: int, b: int, c: int, d: int }\n"
                    "extern func reverse(q: Quad): Quad;\n"
                    "func run(): Quad { return reverse(Quad { a: 1, b: 2, c: 3, d: 4 }); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    Quad result = {0};
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result.a == 4 && result.b == 3 && result.c == 2 && result.d == 1);

    gab_call_free(call);
    gab_vm_free(vm);
}

typedef struct {
    float x;
    float y;
    float z;
} Vec3;

static float vec3_sum(Vec3 v) { return v.x + v.y + v.z; }

static void test_a_struct_of_floats_uses_its_own_registers(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "total", (void *)(uintptr_t)vec3_sum, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Vec3 { x: float, y: float, z: float }\n"
                    "extern func total(v: Vec3): float;\n"
                    "func run(): float { return total(Vec3 { x: 1.0, y: 2.0, z: 4.0 }); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    float result = 0.0f;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 7.0f);

    gab_call_free(call);
    gab_vm_free(vm);
}

typedef struct {
    Pair inner;
    int32_t tag;
} Tagged;

static int32_t tagged_sum(Tagged t) { return t.inner.a + t.inner.b + t.tag; }

static void test_a_nested_struct_crosses_by_value(void) {
    assert(call_int("struct Pair { a: int, b: int }\n"
                    "struct Tagged { inner: Pair, tag: int }\n"
                    "extern func total(t: Tagged): int;",
                    "func run(): int { return total(Tagged { inner: Pair { a: 1, b: 2 }, tag: 4 }); }",
                    "total", (void *)(uintptr_t)tagged_sum, false) == 7);
}

static int32_t pair_first(const Pair *p) { return p->a; }

static void test_a_borrow_of_a_struct_is_a_pointer(void) {
    assert(
        call_int("struct Pair { a: int, b: int }\n"
                 "extern func first(p: &Pair): int;",
                 "func run(): int { let p: Pair = Pair { a: 8, b: 9 }; let r: &Pair = p; return first(r); }",
                 "first", (void *)(uintptr_t)pair_first, false) == 8);
}

typedef struct {
    const char *data;
    int32_t length;
} StrRef;

static int32_t count_char(StrRef s, int32_t target) {
    int32_t seen = 0;

    for (int32_t i = 0; i < s.length; i++) {
        seen += s.data[i] == (char)target;
    }

    return seen;
}

static void test_a_str_arrives_as_its_value_struct(void) {
    assert(call_int("extern func count(s: &str, c: int): int;",
                    "func run(): int { return count(\"banana\", 97); }", "count",
                    (void *)(uintptr_t)count_char, false) == 3);
}

static int32_t sum_four(const int32_t *xs) { return xs[0] + xs[1] + xs[2] + xs[3]; }

static void test_an_array_decays_to_a_pointer(void) {
    assert(call_int("extern func total(xs: [int; 4]): int;",
                    "func run(): int { let xs: [int; 4] = [1, 2, 3, 4]; return total(xs); }", "total",
                    (void *)(uintptr_t)sum_four, false) == 10);
}

static int32_t deref_int(const int32_t *p) { return *p; }

static void test_a_borrow_of_a_scalar_is_a_pointer(void) {
    assert(call_int("extern func peek(p: &int): int;",
                    "func run(): int { let x: int = 5; let p: &int = x; return peek(p); }", "peek",
                    (void *)(uintptr_t)deref_int, false) == 5);
}

static int32_t write_int(int32_t *p) {
    *p = 42;

    return 0;
}

static void test_c_writes_through_a_borrow(void) {
    assert(call_int("extern func poke(p: &int): int;",
                    "func run(): int { let x: int = 1; let p: &int = x; poke(p); return x; }", "poke",
                    (void *)(uintptr_t)write_int, false) == 42);
}

static int32_t first_field(const Pair *p) { return p->a; }

static void test_a_borrow_of_a_box_is_a_pointer(void) {
    assert(
        call_int(
            "struct Pair { a: int, b: int }\n"
            "extern func first(p: &Pair): int;",
            "func run(): int { let p: *Pair = box Pair { a: 3, b: 4 }; let r: &Pair = p; return first(r); }",
            "first", (void *)(uintptr_t)first_field, false) == 3);
}

static int32_t pair_total(Pair *p) {
    int32_t total = p->a + p->b;

    gab_drop_pointer(p);

    return total;
}

static void test_a_c_symbol_drops_an_owning_parameter(void) {
    assert(call_int("struct Pair { a: int, b: int }\n"
                    "extern func total(p: *Pair): int;",
                    "func run(): int { return total(box Pair { a: 3, b: 4 }); }", "total",
                    (void *)(uintptr_t)pair_total, false) == 7);
}

static void test_an_owning_return_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "make", (void *)(uintptr_t)first_field, &err));

    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "struct Pair { a: int, b: int }\n"
                     "extern func make(n: int): *Pair;\n",
                     &err));

    gab_vm_free(vm);
}

static int32_t generic_total(GabCtx *ctx, const void *p) {
    if (gab_ctx_type_kind(ctx, 0) == GAB_TYPE_FLOAT) {
        const float *f = p;

        return (int32_t)(f[0] + f[1]);
    }

    const int32_t *i = p;

    return i[0] + i[1];
}

static void test_one_symbol_serves_every_specialization(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c_ctx(vm, "m", "Pair", "total", (void *)(uintptr_t)generic_total, &err));

    assert(gab_load(vm, "<m>",
                    "module m;\n"
                    "struct Pair<T> { a: T, b: T }\n"
                    "impl<T> Pair<T> {\n"
                    "    extern func total(self: &Pair<T>): int;\n"
                    "}\n"
                    "func run(): int {\n"
                    "    let i = Pair<int> { a: 3, b: 4 };\n"
                    "    let f = Pair<float> { a: 1.0, b: 2.0 };\n"
                    "    return i.total() + f.total();\n"
                    "}\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "m", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 10);

    gab_call_free(call);
    gab_vm_free(vm);
}

static int32_t always_fails(GabCtx *ctx, int32_t x) {
    gab_ctx_fail(ctx, "the symbol refused");

    return x;
}

static void test_a_c_symbol_fails_the_run(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c_ctx(vm, "test", NULL, "refuse", (void *)(uintptr_t)always_fails, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func refuse(x: int): int;\n"
                    "func run(): int { return refuse(1); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_ERR_RUNTIME);

    gab_call_free(call);
    gab_vm_free(vm);
}

static Pair *make_pair(GabCtx *ctx, int32_t a, int32_t b) {
    Pair *p = gab_box(ctx);

    if (!p) {
        return NULL;
    }

    p->a = a;
    p->b = b;

    return p;
}

static void test_a_c_symbol_returns_a_box_the_script_owns(void) {
    assert(call_int("struct Pair { a: int, b: int }\n"
                    "extern func make(a: int, b: int): *Pair;",
                    "func run(): int { let p: *Pair = make(3, 4); return p.a + p.b; }", "make",
                    (void *)(uintptr_t)make_pair, true) == 7);
}

static void test_an_owning_return_without_a_context_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "make", (void *)(uintptr_t)make_pair, &err));

    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "struct Pair { a: int, b: int }\n"
                     "extern func make(a: int, b: int): *Pair;\n",
                     &err));

    gab_vm_free(vm);
}

int main(void) {
    test_an_int_argument_and_result_cross_to_c();
    test_arguments_arrive_in_order();
    test_a_float_crosses_to_c();
    test_a_c_symbol_may_return_nothing();
    test_a_struct_crosses_by_value();
    test_a_struct_comes_back_by_value();
    test_a_struct_of_floats_uses_its_own_registers();
    test_a_nested_struct_crosses_by_value();
    test_a_borrow_of_a_struct_is_a_pointer();
    test_a_str_arrives_as_its_value_struct();
    test_an_array_decays_to_a_pointer();
    test_a_borrow_of_a_scalar_is_a_pointer();
    test_c_writes_through_a_borrow();
    test_a_borrow_of_a_box_is_a_pointer();
    test_a_c_symbol_drops_an_owning_parameter();
    test_one_symbol_serves_every_specialization();
    test_a_c_symbol_fails_the_run();
    test_a_c_symbol_returns_a_box_the_script_owns();
    test_an_owning_return_without_a_context_fails_the_load();
    test_an_owning_return_fails_the_load();

    printf("extern_c_test: all tests passed\n");

    return 0;
}
