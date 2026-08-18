// The embedding API from a host's point of view: this file includes gab.h and
// nothing else from the project. If it ever needs another header, the API has
// leaked something it should have kept opaque.
#include "gab.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// A compile error is reported through GabError rather than printed, so a host
// can put the message in its own console.
static void test_compile_error_is_reported_not_printed(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<bad>", "func broken(: int { return", &err);

    assert(mod == NULL);
    assert(err.message[0] != '\0');
    assert(err.line > 0);

    gab_vm_free(vm);
}

// Compile once, run every frame: the module survives its runs.
static void test_compile_once_run_many(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func seven(): int { return 7; }\nlet r: int = seven();\n", &err);
    assert(mod);

    for (int i = 0; i < 3; i++) {
        assert(gab_module_run(vm, mod, &err) == GAB_OK);
    }

    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// The commit-1 regression: a type registered by the first compile must still
// be readable after a second compile has reused the compile arena.
static void test_type_survives_a_later_compile(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *first = gab_compile(vm, "<first>", "struct Player { health: int, mana: int }\n", &err);
    assert(first);

    GabModule *second = gab_compile(vm, "<second>", "func noop() { }\n", &err);
    assert(second);

    // Reading the first module's type back after the second compile is what
    // used to read reset arena memory.
    const GabType *player = gab_find_type(vm, first, "Player");
    assert(player);
    assert(gab_type_size(player) > 0);

    size_t health_offset = SIZE_MAX;
    assert(gab_field_offset(player, "health", &health_offset));
    assert(health_offset == 0);

    gab_module_free(vm, second);
    gab_module_free(vm, first);
    gab_vm_free(vm);
}

// A script struct is laid out exactly like the equivalent C struct, which is
// the whole zero-marshalling claim. This is that claim, checked.
typedef struct {
    int health;
    int mana;
} Player;

static void test_layout_matches_c(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<layout>", "struct Player { health: int, mana: int }\n", &err);
    assert(mod);

    const GabType *type = gab_find_type(vm, mod, "Player");
    assert(type);

    assert(gab_type_size(type) == sizeof(Player));
    assert(gab_type_align(type) == _Alignof(Player));

    size_t offset = SIZE_MAX;

    assert(gab_field_offset(type, "health", &offset));
    assert(offset == offsetof(Player, health));

    assert(gab_field_offset(type, "mana", &offset));
    assert(offset == offsetof(Player, mana));

    // A missing field is false, not offset 0 — which is why this reports
    // through an out-parameter at all.
    assert(!gab_field_offset(type, "stamina", &offset));

    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A bad lookup is a diagnostic, not a crash.
static void test_lookup_failures(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func real(): int { return 1; }\nlet notafunc: int = 3;\n", &err);
    assert(mod);

    assert(gab_lookup(vm, mod, "missing", &err) == NULL);
    assert(err.message[0] != '\0');

    assert(gab_lookup(vm, mod, "notafunc", &err) == NULL);
    assert(err.message[0] != '\0');

    GabFunc *fn = gab_lookup(vm, mod, "real", &err);
    assert(fn);
    assert(gab_func_arity(fn) == 0);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// Typed arguments in, a typed result out.
static void test_call_with_scalar_args(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>",
                                 "func mix(a: int, b: float, flag: bool): int {\n"
                                 "  if flag { return a * 2; } else { return a; }\n"
                                 "}\n",
                                 &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "mix", &err);
    assert(fn);
    assert(gab_func_arity(fn) == 3);

    gab_arg_int(vm, fn, 0, 21);
    gab_arg_float(vm, fn, 1, 1.5f);
    gab_arg_bool(vm, fn, 2, true);

    int result = 0;
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 42);

    // The same handle, called again with different arguments: the per-frame
    // case, and no lookup happens on the way.
    gab_arg_bool(vm, fn, 2, false);
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 21);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// Calling the same handle in a loop is what an engine does every frame.
static void test_call_in_a_loop(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func step(n: int): int { return n + 1; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "step", &err);
    assert(fn);

    for (int i = 0; i < 64; i++) {
        gab_arg_int(vm, fn, 0, i);

        int result = 0;
        assert(gab_call(vm, fn, &result, &err) == GAB_OK);
        assert(result == i + 1);
    }

    // A call must not allocate: the frame is stack space the VM already owns
    // and the arguments are staged in the handle. Verified out of tree by
    // interposing malloc over this loop, which counted zero.

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A struct crosses the boundary as raw bytes in both directions.
static void test_struct_argument_and_return(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>",
                                 "struct Player { health: int, mana: int }\n"
                                 "func hurt(p: Player, amount: int): Player {\n"
                                 "  let out: Player;\n"
                                 "  out.health = p.health - amount;\n"
                                 "  out.mana = p.mana;\n"
                                 "  return out;\n"
                                 "}\n",
                                 &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "hurt", &err);
    assert(fn);

    Player in = {.health = 100, .mana = 30};

    gab_arg_struct(vm, fn, 0, &in, sizeof(in));
    gab_arg_int(vm, fn, 1, 25);

    Player out = {0};
    assert(gab_call(vm, fn, &out, &err) == GAB_OK);

    assert(out.health == 75);
    assert(out.mana == 30);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A bad argument is GAB_ERR_ARG at the call, and the call does not happen.
static void test_bad_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>",
                                 "struct Player { health: int, mana: int }\n"
                                 "func take(p: Player, n: int): int { return n; }\n",
                                 &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "take", &err);
    assert(fn);

    // Out of range.
    gab_arg_int(vm, fn, 7, 1);
    assert(gab_call(vm, fn, NULL, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    // Wrong type: parameter 1 is an int, not a float.
    gab_arg_float(vm, fn, 1, 1.0f);
    assert(gab_call(vm, fn, NULL, &err) == GAB_ERR_ARG);

    // Wrong struct size.
    int not_a_player = 0;
    gab_arg_struct(vm, fn, 0, &not_a_player, sizeof(not_a_player));
    assert(gab_call(vm, fn, NULL, &err) == GAB_ERR_ARG);

    // After the failures, a correctly built call still works: a rejected
    // argument left nothing behind.
    Player p = {.health = 1, .mana = 2};
    gab_arg_struct(vm, fn, 0, &p, sizeof(p));
    gab_arg_int(vm, fn, 1, 99);

    int result = 0;
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 99);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A runtime failure reaches the caller as GAB_ERR_RUNTIME with a message, and
// leaves the VM usable. Unbounded recursion is the only runtime failure that
// exists today; the channel is what matters, not the particular cause.
static void test_runtime_error_is_reported(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func boom(n: int): int { return boom(n); }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "boom", &err);
    assert(fn);

    gab_arg_int(vm, fn, 0, 1);

    int result = 0;
    assert(gab_call(vm, fn, &result, &err) == GAB_ERR_RUNTIME);
    assert(err.message[0] != '\0');

    gab_func_free(fn);
    gab_module_free(vm, mod);

    // A second module in the same VM still runs: the failure was reported, not
    // fatal.
    GabModule *ok = gab_compile(vm, "<ok>", "func fine(n: int): int { return n + 1; }\n", &err);
    assert(ok);

    GabFunc *good = gab_lookup(vm, ok, "fine", &err);
    gab_arg_int(vm, good, 0, 41);

    assert(gab_call(vm, good, &result, &err) == GAB_OK);
    assert(result == 42);
    assert(err.message[0] == '\0');

    gab_func_free(good);
    gab_module_free(vm, ok);
    gab_vm_free(vm);
}

// The same failure through gab_module_run, which is the other way in.
static void test_runtime_error_from_module_run(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>",
                                 "func boom(n: int): int { return boom(n); }\n"
                                 "let r: int = boom(1);\n",
                                 &err);
    assert(mod);

    assert(gab_module_run(vm, mod, &err) == GAB_ERR_RUNTIME);
    assert(err.message[0] != '\0');

    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

int main(void) {
    test_compile_error_is_reported_not_printed();
    test_runtime_error_is_reported();
    test_runtime_error_from_module_run();
    test_compile_once_run_many();
    test_type_survives_a_later_compile();
    test_layout_matches_c();
    test_lookup_failures();
    test_call_with_scalar_args();
    test_call_in_a_loop();
    test_struct_argument_and_return();
    test_bad_arguments_are_rejected();

    printf("gab_api_test: all tests passed\n");

    return 0;
}
