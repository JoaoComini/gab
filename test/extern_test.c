#include "gab.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t value;
} Counter;

typedef struct {
    int32_t reading;
} Gauge;

static int32_t twice(GabCtx *ctx, int32_t x) {
    (void)ctx;

    return x * 2;
}

static float scale(GabCtx *ctx, float x) {
    (void)ctx;

    return x * 2.0f;
}

static int32_t negate(GabCtx *ctx, int32_t x) {
    (void)ctx;

    return !x;
}

static void refuse(GabCtx *ctx) { gab_ctx_fail(ctx, "the host refused"); }

static void test_an_extern_returns_to_its_caller(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", (GabExternFn)twice, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func twice(x: int): int;\n"
                    "func run(): int { return twice(21); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 42);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_scalars_cross_the_boundary(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "scale", (GabExternFn)scale, &err));
    assert(gab_extern(vm, "test", NULL, "negate", (GabExternFn)negate, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func scale(x: float): float;\n"
                    "extern func negate(b: bool): bool;\n"
                    "func f(): float { return scale(1.5); }\n"
                    "func b(): bool { return negate(true); }\n",
                    &err));

    GabCall *fcall = gab_call_init(gab_lookup(vm, "test", "f", &err), &err);
    float f = 0.0f;
    assert(gab_call(vm, fcall, &f, &err) == GAB_OK);
    assert(f == 3.0f);

    GabCall *bcall = gab_call_init(gab_lookup(vm, "test", "b", &err), &err);
    bool b = true;
    assert(gab_call(vm, bcall, &b, &err) == GAB_OK);
    assert(b == false);

    gab_call_free(fcall);
    gab_call_free(bcall);
    gab_vm_free(vm);
}

static void test_an_unbound_extern_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "func first(): int { return 1; }\n"
                     "extern func missing(x: int): int;\n",
                     &err));

    assert(strstr(err.message, "missing"));
    assert(err.line == 3);

    gab_vm_free(vm);
}

static void test_an_extern_must_be_registered_before_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "extern func twice(x: int): int;\n",
                     &err));

    assert(gab_extern(vm, "test", NULL, "twice", (GabExternFn)twice, &err));
    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func twice(x: int): int;\n"
                    "func run(): int { return twice(2); }\n",
                    &err));

    GabCall *call = gab_call_init(gab_lookup(vm, "test", "run", &err), &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 4);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_host_may_call_an_extern_directly(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", (GabExternFn)twice, &err));
    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func twice(x: int): int;\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "twice", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(gab_arg_int(call, 0, 8));

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 16);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_may_not_have_a_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", (GabExternFn)twice, &err));
    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "extern func twice(x: int): int { return x; }\n",
                     &err));

    gab_vm_free(vm);
}

static void test_a_plain_func_still_needs_a_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "func twice(x: int): int;\n",
                     &err));

    gab_vm_free(vm);
}

static void test_an_extern_lives_in_its_module(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "game", NULL, "twice", (GabExternFn)twice, &err));

    assert(gab_load(vm, "<m>",
                    "module game;\n"
                    "extern func twice(x: int): int;\n",
                    &err));

    assert(gab_lookup(vm, "game", "twice", &err));
    assert(!gab_lookup(vm, "test", "twice", &err));

    gab_vm_free(vm);
}

static void refuse_silently(GabCtx *ctx) { gab_ctx_fail(ctx, NULL); }

static void test_an_extern_may_fail_without_a_message(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", NULL, "refuse", (GabExternFn)refuse_silently, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func refuse(): int;\n"
                    "func run(): int { return refuse(); }\n",
                    &err));

    GabCall *call = gab_call_init(gab_lookup(vm, "test", "run", &err), &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_ERR_RUNTIME);
    assert(err.message[0] != '\0');

    gab_call_free(call);
    gab_vm_free(vm);
}

static void refuse_at_length(GabCtx *ctx) {
    char message[512];

    memset(message, 'x', sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    memcpy(message, "far too long", strlen("far too long"));

    gab_ctx_fail(ctx, message);
}

static void test_a_long_extern_message_is_truncated(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", NULL, "refuse", (GabExternFn)refuse_at_length, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func refuse(): int;\n"
                    "func run(): int { return refuse(); }\n",
                    &err));

    GabCall *call = gab_call_init(gab_lookup(vm, "test", "run", &err), &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_ERR_RUNTIME);

    assert(strncmp(err.message, "far too long", strlen("far too long")) == 0);
    assert(strlen(err.message) < sizeof(err.message));

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_call_reaches_the_body_its_name_declares(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", (GabExternFn)twice, &err));
    assert(gab_extern(vm, "test", NULL, "negate", (GabExternFn)negate, &err));
    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func twice(x: int): int;\n"
                    "func add_one(x: int): int { return x + 1; }\n"
                    "extern func negate(b: bool): bool;\n"
                    "func triple(x: int): int { return x * 3; }\n"
                    "func mixed(x: int): int { return twice(add_one(triple(x))); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "mixed", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(gab_arg_int(call, 0, 2));

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 14);

    gab_call_free(call);
    gab_vm_free(vm);
}

static int32_t counter_get_calls = 0;

static int32_t counter_get(GabCtx *ctx, const Counter *c) {
    (void)ctx;

    counter_get_calls++;

    return c->value;
}

static int32_t gauge_get_calls = 0;

static int32_t gauge_get(GabCtx *ctx, const Gauge *g) {
    (void)ctx;

    gauge_get_calls++;

    return g->reading;
}

static void test_an_extern_may_be_owned_by_a_struct(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", "Counter", "get", (GabExternFn)counter_get, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Counter { value: int }\n"
                    "impl Counter {\n"
                    "    extern func get(self: &Counter): int;\n"
                    "}\n"
                    "func run(): int {\n"
                    "    let c = Counter { value: 9 };\n"
                    "    return c.get();\n"
                    "}\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    counter_get_calls = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(counter_get_calls == 1);
    assert(out == 9);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_two_types_may_own_an_extern_of_one_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", "Counter", "get", (GabExternFn)counter_get, &err));
    assert(gab_extern(vm, "test", "Gauge", "get", (GabExternFn)gauge_get, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Counter { value: int }\n"
                    "struct Gauge { reading: int }\n"
                    "impl Counter {\n"
                    "    extern func get(self: &Counter): int;\n"
                    "}\n"
                    "impl Gauge {\n"
                    "    extern func get(self: &Gauge): int;\n"
                    "}\n"
                    "func run(): int {\n"
                    "    let c = Counter { value: 3 };\n"
                    "    let g = Gauge { reading: 4 };\n"
                    "    return c.get() * 10 + g.get();\n"
                    "}\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    counter_get_calls = 0;
    gauge_get_calls = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(counter_get_calls == 1);
    assert(gauge_get_calls == 1);
    assert(out == 34);

    gab_call_free(call);
    gab_vm_free(vm);
}

static int32_t str_len(GabCtx *ctx, GabStrRef text) {
    (void)ctx;

    return text.length;
}

static void test_a_core_method_on_a_primitive_reaches_its_host_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_load(vm, "<m>", "module test;\nfunc run(): int { return \"hello\".len(); }\n", &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 5);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_primitive_is_owned_only_by_a_host_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "impl str {\n"
                     "    func length(self: &str): int { return 0; }\n"
                     "}\n",
                     &err));

    gab_vm_free(vm);
}

static void test_only_the_core_library_owns_a_primitive(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", "str", "length", (GabExternFn)str_len, &err));

    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "impl str {\n"
                     "    extern func length(self: &str): int;\n"
                     "}\n",
                     &err));

    gab_vm_free(vm);
}

static void test_naming_the_core_library_does_not_own_a_primitive(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "core", "str", "length", (GabExternFn)str_len, &err));

    assert(!gab_load(vm, "<m>",
                     "module core;\n"
                     "impl str {\n"
                     "    extern func length(self: &str): int;\n"
                     "}\n",
                     &err));

    gab_vm_free(vm);
}

static void test_a_qualified_name_does_not_declare_a_method(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "core", "str", "length", (GabExternFn)str_len, &err));

    assert(!gab_load(vm, "<m>",
                     "module core;\n"
                     "extern func str::length(self: &str): int;\n",
                     &err));

    gab_vm_free(vm);
}

static void test_an_extern_does_not_claim_a_type_from_another_module(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_load(vm, "a.gab", "module A;\nstruct Counter { value: int }\n", &err));

    assert(!gab_load(vm, "b.gab",
                     "module B;\n"
                     "import A;\n"
                     "impl A::Counter {\n"
                     "    extern func get(self: &A::Counter): int;\n"
                     "}\n",
                     &err));

    gab_vm_free(vm);
}

static int32_t call_int(const char *decl, const char *body, const char *name, GabExternFn symbol) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, name, symbol, &err));

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

static int32_t absolute(GabCtx *ctx, int32_t x) {
    (void)ctx;

    return x < 0 ? -x : x;
}

static int32_t subtract(GabCtx *ctx, int32_t a, int32_t b) {
    (void)ctx;

    return a - b;
}

static void test_arguments_arrive_in_order(void) {
    assert(call_int("extern func sub(a: int, b: int): int;", "func run(): int { return sub(10, 3); }", "sub",
                    (GabExternFn)subtract) == 7);
}

static float root_of(GabCtx *ctx, float x) {
    (void)ctx;

    return sqrtf(x);
}

static int32_t counted = 0;

static void bump(GabCtx *ctx) {
    (void)ctx;

    counted++;
}

typedef struct {
    int32_t a;
    int32_t b;
} Pair;

static int32_t pair_sum(GabCtx *ctx, Pair p) {
    (void)ctx;

    return p.a + p.b;
}

typedef struct {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
} Quad;

static Quad quad_reversed(GabCtx *ctx, Quad q) {
    (void)ctx;

    return (Quad){.a = q.d, .b = q.c, .c = q.b, .d = q.a};
}

static void test_a_struct_comes_back_by_value(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "reverse", (GabExternFn)quad_reversed, &err));

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

static float vec3_sum(GabCtx *ctx, Vec3 v) {
    (void)ctx;

    return v.x + v.y + v.z;
}

static void test_a_struct_of_floats_uses_its_own_registers(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "total", (GabExternFn)vec3_sum, &err));

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

static int32_t tagged_sum(GabCtx *ctx, Tagged t) {
    (void)ctx;

    return t.inner.a + t.inner.b + t.tag;
}

static void test_a_nested_struct_crosses_by_value(void) {
    assert(call_int("struct Pair { a: int, b: int }\n"
                    "struct Tagged { inner: Pair, tag: int }\n"
                    "extern func total(t: Tagged): int;",
                    "func run(): int { return total(Tagged { inner: Pair { a: 1, b: 2 }, tag: 4 }); }",
                    "total", (GabExternFn)tagged_sum) == 7);
}

static int32_t pair_first(GabCtx *ctx, const Pair *p) {
    (void)ctx;

    return p->a;
}

static void test_a_borrow_of_a_struct_is_a_pointer(void) {
    assert(
        call_int("struct Pair { a: int, b: int }\n"
                 "extern func first(p: &Pair): int;",
                 "func run(): int { let p: Pair = Pair { a: 8, b: 9 }; let r: &Pair = p; return first(r); }",
                 "first", (GabExternFn)pair_first) == 8);
}

typedef struct {
    const char *data;
    int32_t length;
} StrRef;

static int32_t count_char(GabCtx *ctx, StrRef s, int32_t target) {
    (void)ctx;

    int32_t seen = 0;

    for (int32_t i = 0; i < s.length; i++) {
        seen += s.data[i] == (char)target;
    }

    return seen;
}

static void test_a_str_arrives_as_its_value_struct(void) {
    assert(call_int("extern func count(s: &str, c: int): int;",
                    "func run(): int { return count(\"banana\", 97); }", "count",
                    (GabExternFn)count_char) == 3);
}

static int32_t sum_four(GabCtx *ctx, const int32_t *xs) {
    (void)ctx;

    return xs[0] + xs[1] + xs[2] + xs[3];
}

static void test_an_array_decays_to_a_pointer(void) {
    assert(call_int("extern func total(xs: [int; 4]): int;",
                    "func run(): int { let xs: [int; 4] = [1, 2, 3, 4]; return total(xs); }", "total",
                    (GabExternFn)sum_four) == 10);
}

static int32_t deref_int(GabCtx *ctx, const int32_t *p) {
    (void)ctx;

    return *p;
}

static void test_a_borrow_of_a_scalar_is_a_pointer(void) {
    assert(call_int("extern func peek(p: &int): int;",
                    "func run(): int { let x: int = 5; let p: &int = x; return peek(p); }", "peek",
                    (GabExternFn)deref_int) == 5);
}

static int32_t write_int(GabCtx *ctx, int32_t *p) {
    (void)ctx;

    *p = 42;

    return 0;
}

static int32_t first_field(GabCtx *ctx, const Pair *p) {
    (void)ctx;

    return p->a;
}

static void test_a_borrow_of_a_box_is_a_pointer(void) {
    assert(
        call_int(
            "struct Pair { a: int, b: int }\n"
            "extern func first(p: &Pair): int;",
            "func run(): int { let p: *Pair = box Pair { a: 3, b: 4 }; let r: &Pair = p; return first(r); }",
            "first", (GabExternFn)first_field) == 3);
}

static int32_t pair_total(GabCtx *ctx, Pair *p) {
    (void)ctx;

    int32_t total = p->a + p->b;

    gab_drop_pointer(p);

    return total;
}

static int32_t generic_total(GabCtx *ctx, const void *p) {
    if (gab_ctx_type_kind(ctx, 0) == GAB_TYPE_FLOAT) {
        const float *f = p;

        return (int32_t)(f[0] + f[1]);
    }

    const int32_t *i = p;

    return i[0] + i[1];
}

static int32_t always_fails(GabCtx *ctx, int32_t x) {
    gab_ctx_fail(ctx, "the symbol refused");

    return x;
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
                    (GabExternFn)make_pair) == 7);
}

/* A method generic in its return type gets one signature per instantiation, so the same symbol
   answers at whatever width the specialization chose. */
static void holder_at(GabCtx *ctx, const void *self, int32_t index) {
    memcpy(gab_ctx_return(ctx), (const char *)self + (size_t)index * gab_ctx_type_size(ctx, 0),
           gab_ctx_type_size(ctx, 0));
}

static void test_a_generic_return_is_sized_by_its_specialization(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "m", "Holder", "at", (GabExternFn)holder_at, &err));

    assert(gab_load(vm, "<m>",
                    "module m;\n"
                    "struct Holder<T> { a: T, b: T }\n"
                    "impl<T> Holder<T> {\n"
                    "    extern func at(self: &Holder<T>, i: int): T;\n"
                    "}\n"
                    "func run(): int {\n"
                    "    let h = Holder<int> { a: 7, b: 8 };\n"
                    "    return h.at(1);\n"
                    "}\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "m", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 8);

    gab_call_free(call);
    gab_vm_free(vm);
}

/* One symbol over arrays of two lengths: the body reads how many elements it was given rather than
   being told, so the same C function answers for both. */
static int32_t sum_all(GabCtx *ctx, const void *xs) {
    int32_t total = 0;
    size_t stride = gab_ctx_array_stride(ctx, 0);

    for (int32_t i = 0; i < gab_ctx_array_length(ctx, 0); i++) {
        int32_t value = 0;

        memcpy(&value, (const char *)xs + (size_t)i * stride, stride);

        total += value;
    }

    return total;
}

static void test_a_body_reads_the_length_of_an_array_it_is_given(void) {
    assert(call_int("extern func total(xs: [int; 3]): int;",
                    "func run(): int { let xs: [int; 3] = [1, 2, 3]; return total(xs); }", "total",
                    (GabExternFn)sum_all) == 6);

    assert(call_int("extern func total(xs: [int; 5]): int;",
                    "func run(): int { let xs: [int; 5] = [1, 2, 3, 4, 5]; return total(xs); }", "total",
                    (GabExternFn)sum_all) == 15);
}

int main(void) {
    test_an_extern_returns_to_its_caller();
    test_scalars_cross_the_boundary();
    test_an_unbound_extern_fails_the_load();
    test_an_extern_must_be_registered_before_the_load();
    test_a_host_may_call_an_extern_directly();
    test_an_extern_may_fail_without_a_message();
    test_a_long_extern_message_is_truncated();
    test_an_extern_may_not_have_a_body();
    test_a_plain_func_still_needs_a_body();
    test_an_extern_lives_in_its_module();
    test_a_call_reaches_the_body_its_name_declares();
    test_an_extern_may_be_owned_by_a_struct();
    test_two_types_may_own_an_extern_of_one_name();
    test_a_core_method_on_a_primitive_reaches_its_host_body();
    test_a_primitive_is_owned_only_by_a_host_body();
    test_only_the_core_library_owns_a_primitive();
    test_naming_the_core_library_does_not_own_a_primitive();
    test_a_qualified_name_does_not_declare_a_method();
    test_an_extern_does_not_claim_a_type_from_another_module();

    test_arguments_arrive_in_order();
    test_a_struct_comes_back_by_value();
    test_a_struct_of_floats_uses_its_own_registers();
    test_a_nested_struct_crosses_by_value();
    test_a_borrow_of_a_struct_is_a_pointer();
    test_a_str_arrives_as_its_value_struct();
    test_an_array_decays_to_a_pointer();
    test_a_body_reads_the_length_of_an_array_it_is_given();
    test_a_borrow_of_a_scalar_is_a_pointer();
    test_a_borrow_of_a_box_is_a_pointer();
    test_a_generic_return_is_sized_by_its_specialization();
    test_a_c_symbol_returns_a_box_the_script_owns();

    printf("extern_test: all tests passed\n");

    return 0;
}
