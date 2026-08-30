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

static int32_t last_logged = 0;

static void host_log(GabArgs *args) { last_logged = gab_arg_get_int(args, 0); }

static void twice(GabArgs *args) { gab_return_int(args, gab_arg_get_int(args, 0) * 2); }

static void scale(GabArgs *args) { gab_return_float(args, gab_arg_get_float(args, 0) * 2.0f); }

static void negate(GabArgs *args) { gab_return_bool(args, !gab_arg_get_bool(args, 0)); }

static void heal(GabArgs *args) {
    Player p;
    gab_arg_get_struct(args, 0, &p, sizeof p);

    p.health += 10;

    gab_return_struct(args, &p, sizeof p);
}

static void boost(GabArgs *args) {
    Player *p = gab_arg_get_pointer(args, 0);

    p->mana += 5;
}

static void refuse(GabArgs *args) { gab_error(args, "the host refused"); }

static void test_an_extern_returns_to_its_caller(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(gab_extern(vm, "test", "twice", twice, &err));

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
    assert(gab_extern(vm, "test", "log", host_log, &err));

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
    assert(gab_extern(vm, "test", "scale", scale, &err));
    assert(gab_extern(vm, "test", "negate", negate, &err));

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
    assert(gab_extern(vm, "test", "heal", heal, &err));

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
    assert(gab_extern(vm, "test", "boost", boost, &err));

    assert(gab_load(vm, "<m>",
                    "module test;\n"
                    "struct Player { health: int, mana: int }\n"
                    "extern func boost(p: ref Player);\n"
                    "func run(p: ref Player) { boost(p); }\n",
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

    assert(gab_extern(vm, "test", "twice", twice, &err));
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
    assert(gab_extern(vm, "test", "twice", twice, &err));
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
    assert(gab_extern(vm, "test", "refuse", refuse, &err));

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
    assert(gab_extern(vm, "test", "twice", twice, &err));
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
    assert(gab_extern(vm, "game", "twice", twice, &err));

    assert(gab_load(vm, "<m>",
                    "module game;\n"
                    "extern func twice(x: int): int;\n",
                    &err));

    assert(gab_lookup(vm, "game", "twice", &err));
    assert(!gab_lookup(vm, "test", "twice", &err));

    gab_vm_free(vm);
}

static void refuse_silently(GabArgs *args) { gab_error(args, NULL); }

static void test_an_extern_may_fail_without_a_message(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", "refuse", refuse_silently, &err));

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

static void refuse_at_length(GabArgs *args) {
    char message[512];

    memset(message, 'x', sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    memcpy(message, "far too long", strlen("far too long"));

    gab_error(args, message);
}

static void test_a_long_extern_message_is_truncated(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "test", "refuse", refuse_at_length, &err));

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
    assert(gab_extern(vm, "test", "twice", twice, &err));
    assert(gab_extern(vm, "test", "negate", negate, &err));
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

    printf("extern_test: all tests passed\n");

    return 0;
}
