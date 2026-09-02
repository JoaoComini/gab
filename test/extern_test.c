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

static void host_log(GabCtx *ctx, int32_t value) {
    (void)ctx;

    last_logged = value;
}

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

static Player heal(GabCtx *ctx, Player p) {
    (void)ctx;

    p.health += 10;

    return p;
}

static void boost(GabCtx *ctx, Player *p) {
    (void)ctx;

    p->mana += 5;
}

static void refuse(GabCtx *ctx) { gab_ctx_fail(ctx, "the host refused"); }

static void test_an_extern_returns_to_its_caller(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "twice", (void *)(uintptr_t)twice, &err));

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

static void test_an_extern_may_return_nothing(void) {
    GabVM *vm = gab_vm_new();

    last_logged = 0;

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "log", (void *)(uintptr_t)host_log, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func log(amount: int);\n"
                    "func run() { log(7); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(last_logged == 7);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_scalars_cross_the_boundary(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "scale", (void *)(uintptr_t)scale, &err));
    assert(gab_extern_c(vm, "test", NULL, "negate", (void *)(uintptr_t)negate, &err));

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

static void test_a_struct_crosses_by_value(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "heal", (void *)(uintptr_t)heal, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Player { health: int, mana: int }\n"
                    "extern func heal(p: Player): Player;\n"
                    "func run(p: Player): Player { return heal(p); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player in = {.health = 30, .mana = 4};
    assert(gab_arg_struct(call, 0, &in, sizeof in));

    Player out = {0};
    assert(gab_call(vm, call, &out, &err) == GAB_OK);
    assert(out.health == 40);
    assert(out.mana == 4);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_borrow_is_written_through(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "boost", (void *)(uintptr_t)boost, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Player { health: int, mana: int }\n"
                    "extern func boost(p: &Player);\n"
                    "func run(p: &Player) { boost(p); }\n",
                    &err));

    const GabType *type = gab_find_type(vm, "test", "Player");
    assert(type);

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    Player p = {.health = 1, .mana = 2};
    assert(gab_arg_pointer(call, 0, &p, type));

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(p.mana == 7);

    gab_call_free(call);
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

    assert(gab_extern_c(vm, "test", NULL, "twice", (void *)(uintptr_t)twice, &err));
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
    assert(gab_extern_c(vm, "test", NULL, "twice", (void *)(uintptr_t)twice, &err));
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

static void test_an_extern_may_fail_the_run(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "refuse", (void *)(uintptr_t)refuse, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "extern func refuse(): int;\n"
                    "func run(): int { return refuse(); }\n",
                    &err));

    GabCall *call = gab_call_init(gab_lookup(vm, "test", "run", &err), &err);

    int32_t out = 0;
    assert(gab_call(vm, call, &out, &err) == GAB_ERR_RUNTIME);
    assert(strstr(err.message, "the host refused"));

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_may_not_have_a_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "test", NULL, "twice", (void *)(uintptr_t)twice, &err));
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
    assert(gab_extern_c(vm, "game", NULL, "twice", (void *)(uintptr_t)twice, &err));

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

    assert(gab_extern_c(vm, "test", NULL, "refuse", (void *)(uintptr_t)refuse_silently, &err));

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

    assert(gab_extern_c(vm, "test", NULL, "refuse", (void *)(uintptr_t)refuse_at_length, &err));

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
    assert(gab_extern_c(vm, "test", NULL, "twice", (void *)(uintptr_t)twice, &err));
    assert(gab_extern_c(vm, "test", NULL, "negate", (void *)(uintptr_t)negate, &err));
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
    assert(gab_extern_c(vm, "test", "Counter", "get", (void *)(uintptr_t)counter_get, &err));

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
    assert(gab_extern_c(vm, "test", "Counter", "get", (void *)(uintptr_t)counter_get, &err));
    assert(gab_extern_c(vm, "test", "Gauge", "get", (void *)(uintptr_t)gauge_get, &err));

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

static int32_t count_of(GabCtx *ctx, const void *self) {
    (void)ctx;
    (void)self;

    return 7;
}

static void test_each_specialization_of_a_host_method_reaches_one_body(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern_c(vm, "m", "Pair", "count", (void *)(uintptr_t)count_of, &err));

    assert(gab_load(vm, "<m>",
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

    GabFunc *fn = gab_lookup(vm, "m", "run", &err);
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
    assert(gab_extern_c(vm, "test", "str", "length", (void *)(uintptr_t)str_len, &err));

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
    assert(gab_extern_c(vm, "core", "str", "length", (void *)(uintptr_t)str_len, &err));

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
    assert(gab_extern_c(vm, "core", "str", "length", (void *)(uintptr_t)str_len, &err));

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

typedef struct {
    int32_t a;
    int32_t b;
} Owned;

static int32_t dropped_sum = 0;

static void consume(GabCtx *ctx, Owned *p) {
    (void)ctx;

    dropped_sum = p->a + p->b;

    gab_drop_pointer(p);
}

static void test_a_host_body_drops_an_owning_parameter(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    dropped_sum = 0;

    assert(gab_extern_c(vm, "test", NULL, "consume", (void *)(uintptr_t)consume, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Owned { a: int, b: int }\n"
                    "extern func consume(p: *Owned);\n"
                    "func run() { consume(box Owned { a: 3, b: 4 }); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "test", "run", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(gab_call(vm, call, NULL, &err) == GAB_OK);
    assert(dropped_sum == 7);

    gab_call_free(call);
    gab_vm_free(vm);
}

int main(void) {
    test_an_extern_returns_to_its_caller();
    test_an_extern_may_return_nothing();
    test_scalars_cross_the_boundary();
    test_a_struct_crosses_by_value();
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
    test_a_primitive_is_owned_only_by_a_host_body();
    test_only_the_core_library_owns_a_primitive();
    test_naming_the_core_library_does_not_own_a_primitive();
    test_a_qualified_name_does_not_declare_a_method();
    test_an_extern_does_not_claim_a_type_from_another_module();
    test_a_host_body_drops_an_owning_parameter();

    printf("extern_test: all tests passed\n");

    return 0;
}
