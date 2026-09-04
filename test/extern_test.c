#include "gab.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t health;
    int32_t mana;
} Player;

typedef struct {
    int32_t value;
} Counter;

typedef struct {
    int32_t reading;
} Gauge;

static int32_t last_logged = 0;

static void host_log(GabCtx *ctx) { last_logged = gab_ctx_int(ctx, 0); }

static void twice(GabCtx *ctx) { gab_ctx_return_int(ctx, gab_ctx_int(ctx, 0) * 2); }

static void scale(GabCtx *ctx) { gab_ctx_return_float(ctx, gab_ctx_float(ctx, 0) * 2.0f); }

static void negate(GabCtx *ctx) { gab_ctx_return_bool(ctx, !gab_ctx_bool(ctx, 0)); }

static void heal(GabCtx *ctx) {
    Player p;
    gab_ctx_struct(ctx, 0, &p, sizeof p);

    p.health += 10;

    gab_ctx_return_struct(ctx, &p, sizeof p);
}

static void boost(GabCtx *ctx) {
    Player *p = gab_ctx_pointer(ctx, 0);

    p->mana += 5;
}

static void refuse(GabCtx *ctx) { gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, "the host refused"); }

static void test_an_extern_returns_to_its_caller(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", twice, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func twice(x: int): int;\n"
                       "func run(): int { return twice(21); }\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 42);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_may_return_nothing(void) {
    GabVM *vm = gab_vm_new();

    last_logged = 0;

    GabError err;
    assert(gab_extern(vm, "test", NULL, "log", host_log, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func log(amount: int);\n"
                       "func run() { log(7); }\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(last_logged == 7);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_scalars_cross_the_boundary(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "scale", scale, &err));
    assert(gab_extern(vm, "test", NULL, "negate", negate, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func scale(x: float): float;\n"
                       "extern func negate(b: bool): bool;\n"
                       "func f(): float { return scale(1.5); }\n"
                       "func b(): bool { return negate(true); }\n",
                       &err));

    GabCall *fcall = gab_call_init(gab_vm_lookup(vm, "test", "f", &err), &err);
    float f = 0.0f;
    assert(gab_call(vm, fcall, &f, &err) == GAB_OK);
    assert(f == 3.0f);

    GabCall *bcall = gab_call_init(gab_vm_lookup(vm, "test", "b", &err), &err);
    bool b = true;
    assert(gab_call(vm, bcall, &b, &err) == GAB_OK);
    assert(b == false);

    gab_call_free(fcall);
    gab_call_free(bcall);
    gab_vm_free(vm);
}

static void test_a_struct_crosses_by_value(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "heal", heal, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "struct Player { health: int, mana: int }\n"
                       "extern func heal(p: Player): Player;\n"
                       "func run(p: Player): Player { return heal(p); }\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player in = {.health = 30, .mana = 4};
    assert(gab_call_struct(call, 0, &in, sizeof in));

    Player out = {0};
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out.health == 40);
    assert(out.mana == 4);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void after_a_wide_parameter(GabCtx *ctx) {
    Player first;
    Player second;

    gab_ctx_struct(ctx, 0, &first, sizeof first);
    int32_t middle = gab_ctx_int(ctx, 1);
    gab_ctx_struct(ctx, 2, &second, sizeof second);
    int32_t last = gab_ctx_int(ctx, 3);

    gab_ctx_return_int(ctx, first.health * 1000 + middle * 100 + second.health * 10 + last);
}

static void test_a_parameter_after_a_wide_one_is_found(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "f", after_a_wide_parameter, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "struct Player { health: int, mana: int }\n"
                       "extern func f(p: Player, x: int, q: Player, y: int): int;\n"
                       "func run(): int {\n"
                       "    return f(Player { health: 1, mana: 0 }, 2, Player { health: 3, mana: 0 }, 4);\n"
                       "}\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 1234);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_borrow_is_written_through(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "boost", boost, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "struct Player { health: int, mana: int }\n"
                       "extern func boost(p: &Player);\n"
                       "func run(p: &Player) { boost(p); }\n",
                       &err));

    const GabType *type = gab_vm_find_type(vm, "test", "Player");
    assert(type);

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player p = {.health = 1, .mana = 2};
    assert(gab_call_pointer(call, 0, &p, type));

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(p.mana == 7);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_unbound_extern_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_vm_load(vm, "<m>",
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
    assert(!gab_vm_load(vm, "<m>",
                        "module test;\n"
                        "extern func twice(x: int): int;\n",
                        &err));

    assert(gab_extern(vm, "test", NULL, "twice", twice, &err));
    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func twice(x: int): int;\n"
                       "func run(): int { return twice(2); }\n",
                       &err));

    GabCall *call = gab_call_init(gab_vm_lookup(vm, "test", "run", &err), &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 4);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_host_may_call_an_extern_directly(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", twice, &err));
    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func twice(x: int): int;\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "twice", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(gab_call_int(call, 0, 8));

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 16);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_may_fail_the_run(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "refuse", refuse, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func refuse(): int;\n"
                       "func run(): int { return refuse(); }\n",
                       &err));

    GabCall *call = gab_call_init(gab_vm_lookup(vm, "test", "run", &err), &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_ERR_RUNTIME);
    assert(strstr(err.message, "the host refused"));

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_may_not_have_a_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "twice", twice, &err));
    assert(!gab_vm_load(vm, "<m>",
                        "module test;\n"
                        "extern func twice(x: int): int { return x; }\n",
                        &err));

    gab_vm_free(vm);
}

static void test_a_plain_func_still_needs_a_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_vm_load(vm, "<m>",
                        "module test;\n"
                        "func twice(x: int): int;\n",
                        &err));

    gab_vm_free(vm);
}

static void test_an_extern_lives_in_its_module(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "game", NULL, "twice", twice, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module game;\n"
                       "extern func twice(x: int): int;\n",
                       &err));

    assert(gab_vm_lookup(vm, "game", "twice", &err));
    assert(!gab_vm_lookup(vm, "test", "twice", &err));

    gab_vm_free(vm);
}

static void refuse_silently(GabCtx *ctx) { gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, NULL); }

static void test_an_extern_may_fail_without_a_message(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", NULL, "refuse", refuse_silently, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func refuse(): int;\n"
                       "func run(): int { return refuse(); }\n",
                       &err));

    GabCall *call = gab_call_init(gab_vm_lookup(vm, "test", "run", &err), &err);

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

    gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, message);
}

static void test_a_long_extern_message_is_truncated(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", NULL, "refuse", refuse_at_length, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func refuse(): int;\n"
                       "func run(): int { return refuse(); }\n",
                       &err));

    GabCall *call = gab_call_init(gab_vm_lookup(vm, "test", "run", &err), &err);

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
    assert(gab_extern(vm, "test", NULL, "twice", twice, &err));
    assert(gab_extern(vm, "test", NULL, "negate", negate, &err));
    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func twice(x: int): int;\n"
                       "func add_one(x: int): int { return x + 1; }\n"
                       "extern func negate(b: bool): bool;\n"
                       "func triple(x: int): int { return x * 3; }\n"
                       "func mixed(x: int): int { return twice(add_one(triple(x))); }\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "mixed", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(gab_call_int(call, 0, 2));

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 14);

    gab_call_free(call);
    gab_vm_free(vm);
}

static int32_t counter_get_calls = 0;

static void counter_get(GabCtx *ctx) {
    Counter *c = gab_ctx_pointer(ctx, 0);

    counter_get_calls++;

    gab_ctx_return_int(ctx, c->value);
}

static int32_t gauge_get_calls = 0;

static void gauge_get(GabCtx *ctx) {
    Gauge *g = gab_ctx_pointer(ctx, 0);

    gauge_get_calls++;

    gab_ctx_return_int(ctx, g->reading);
}

static void test_an_extern_may_be_owned_by_a_struct(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", "Counter", "get", counter_get, &err));

    assert(gab_vm_load(vm, "<m>",
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

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
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
    assert(gab_extern(vm, "test", "Counter", "get", counter_get, &err));
    assert(gab_extern(vm, "test", "Gauge", "get", gauge_get, &err));

    assert(gab_vm_load(vm, "<m>",
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

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
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

static void str_len(GabCtx *ctx) {
    int32_t length = 0;
    gab_ctx_string(ctx, 0, &length);

    gab_ctx_return_int(ctx, length);
}

static void count_of(GabCtx *ctx) { gab_ctx_return_int(ctx, 7); }

static void test_each_specialization_of_a_host_method_reaches_one_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "m", "Pair", "count", count_of, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module m;\n"
                       "struct Pair<T> { a: T, b: T }\n"
                       "impl<T> Pair<T> {\n"
                       "    extern func count(self: &Pair<T>): int;\n"
                       "}\n"
                       "func run(): int {\n"
                       "    let ints = Pair<int> { a: 1, b: 2 };\n"
                       "    let bools = Pair<bool> { a: true, b: false };\n"
                       "    return ints.count() + bools.count();\n"
                       "}\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "m", "run", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 14);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_core_method_on_a_primitive_reaches_its_host_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_vm_load(vm, "<m>", "module test;\nfunc run(): int { return \"hello\".len(); }\n", &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
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
    assert(!gab_vm_load(vm, "<m>",
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
    assert(gab_extern(vm, "test", "str", "length", str_len, &err));

    assert(!gab_vm_load(vm, "<m>",
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
    assert(gab_extern(vm, "core", "str", "length", str_len, &err));

    assert(!gab_vm_load(vm, "<m>",
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
    assert(gab_extern(vm, "core", "str", "length", str_len, &err));

    assert(!gab_vm_load(vm, "<m>",
                        "module core;\n"
                        "extern func str::length(self: &str): int;\n",
                        &err));

    gab_vm_free(vm);
}

static void test_an_extern_does_not_claim_a_type_from_another_module(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_vm_load(vm, "a.gab", "module A;\nstruct Counter { value: int }\n", &err));

    assert(!gab_vm_load(vm, "b.gab",
                        "module B;\n"
                        "import A;\n"
                        "impl A::Counter {\n"
                        "    extern func get(self: &A::Counter): int;\n"
                        "}\n",
                        &err));

    gab_vm_free(vm);
}

static void body_tells_specializations_apart(GabCtx *ctx) {
    gab_ctx_return_int(ctx, gab_ctx_type_kind(ctx, 0) == GAB_TYPE_FLOAT ? 1 : 0);
}

static void test_a_body_reads_what_its_specialization_chose(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "m", "Holder", "tag", body_tells_specializations_apart, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module m;\n"
                       "struct Holder<T> { a: T }\n"
                       "impl<T> Holder<T> {\n"
                       "    extern func tag(self: &Holder<T>): int;\n"
                       "}\n"
                       "func run(): int {\n"
                       "    let i = Holder<int> { a: 1 };\n"
                       "    let f = Holder<float> { a: 1.0 };\n"
                       "    return i.tag() * 10 + f.tag();\n"
                       "}\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "m", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 1);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void body_measures_its_array(GabCtx *ctx) {
    gab_ctx_return_int(ctx, gab_ctx_array_length(ctx, 0) * 100 + (int32_t)gab_ctx_array_stride(ctx, 0));
}

static void test_a_body_reads_the_shape_of_an_array_it_is_given(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", NULL, "shape", body_measures_its_array, &err));

    assert(gab_vm_load(vm, "<m>",
                       "module test;\n"
                       "extern func shape(xs: array<int, 3>): int;\n"
                       "func run(): int { return shape([1, 2, 3]); }\n",
                       &err));

    GabFunc *fn = gab_vm_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out == 304);

    gab_call_free(call);
    gab_vm_free(vm);
}

int main(void) {
    test_an_extern_returns_to_its_caller();
    test_an_extern_may_return_nothing();
    test_scalars_cross_the_boundary();
    test_a_struct_crosses_by_value();
    test_a_parameter_after_a_wide_one_is_found();
    test_a_borrow_is_written_through();
    test_an_unbound_extern_fails_the_load();
    test_an_extern_must_be_registered_before_the_load();
    test_a_host_may_call_an_extern_directly();
    test_an_extern_may_fail_the_run();
    test_an_extern_may_fail_without_a_message();
    test_a_long_extern_message_is_truncated();
    test_an_extern_may_not_have_a_body();
    test_a_plain_func_still_needs_a_body();
    test_an_extern_lives_in_its_module();
    test_a_call_reaches_the_body_its_name_declares();
    test_an_extern_may_be_owned_by_a_struct();
    test_two_types_may_own_an_extern_of_one_name();
    test_a_core_method_on_a_primitive_reaches_its_host_body();
    test_each_specialization_of_a_host_method_reaches_one_body();
    test_a_body_reads_what_its_specialization_chose();
    test_a_body_reads_the_shape_of_an_array_it_is_given();
    test_a_primitive_is_owned_only_by_a_host_body();
    test_only_the_core_library_owns_a_primitive();
    test_naming_the_core_library_does_not_own_a_primitive();
    test_a_qualified_name_does_not_declare_a_method();
    test_an_extern_does_not_claim_a_type_from_another_module();

    printf("extern_test: all tests passed\n");

    return 0;
}
