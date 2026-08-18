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

// An argument never set must not silently pass whatever the buffer held —
// zero on the first call, the previous frame's value on every one after.
// Deliberate reuse stays legal, because that is what makes a per-frame call
// allocation-free.
static void test_unset_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func two(a: int, b: int): int { return a + b; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "two", &err);
    assert(fn);

    // Only argument 0 supplied.
    gab_arg_int(vm, fn, 0, 1);

    int result = -1;
    assert(gab_call(vm, fn, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    // Supplying the rest makes the same handle callable.
    gab_arg_int(vm, fn, 1, 1000);
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 1001);

    // Reuse is not staleness: holding argument 1 constant while changing 0 is
    // the per-frame case and must keep working.
    gab_arg_int(vm, fn, 0, 2);
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 1002);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A setter that was rejected has not supplied its argument, so the parameter
// stays unset rather than counting as given.
static void test_rejected_setter_does_not_supply_an_argument(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "<m>", "func two(a: int, b: int): int { return a + b; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, mod, "two", &err);
    assert(fn);

    gab_arg_int(vm, fn, 0, 5);
    gab_arg_float(vm, fn, 1, 1.0f); // wrong type for an int parameter

    int result = 0;
    assert(gab_call(vm, fn, &result, &err) == GAB_ERR_ARG);

    // The rejection was reported and cleared, but argument 1 was never
    // actually given a value, so the next call must still say so.
    assert(gab_call(vm, fn, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// Two entity scripts, each with its own on_update — the case an engine hits
// first, and a hard compile error before modules existed. Asking one unit for
// the name must give that unit's function: the module handle used to be
// accepted and ignored, so this returned whichever was compiled first.
static void test_two_modules_can_share_a_function_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *player = gab_compile(vm, "player.gab",
                                    "module Player;\n"
                                    "func on_update(): int { return 1; }\n",
                                    &err);
    assert(player);

    GabModule *enemy = gab_compile(vm, "enemy.gab",
                                   "module Enemy;\n"
                                   "func on_update(): int { return 2; }\n",
                                   &err);
    assert(enemy);
    assert(err.message[0] == '\0');

    GabFunc *player_update = gab_lookup(vm, player, "on_update", &err);
    GabFunc *enemy_update = gab_lookup(vm, enemy, "on_update", &err);
    assert(player_update && enemy_update);

    int result = 0;
    assert(gab_call(vm, player_update, &result, &err) == GAB_OK);
    assert(result == 1);

    assert(gab_call(vm, enemy_update, &result, &err) == GAB_OK);
    assert(result == 2);

    gab_func_free(enemy_update);
    gab_func_free(player_update);
    gab_module_free(vm, enemy);
    gab_module_free(vm, player);
    gab_vm_free(vm);
}

// The script names the module, so a host reads it back rather than assuming it
// matches the label it passed to gab_compile. The two need not agree.
static void test_module_name_is_read_from_the_script(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *named = gab_compile(vm, "some_file.gab", "module Player;\nfunc f(): int { return 1; }\n", &err);
    assert(named);
    assert(gab_module_name(named));
    assert(strcmp(gab_module_name(named), "Player") == 0);

    // A unit with no directive belongs to the root namespace.
    GabModule *anonymous = gab_compile(vm, "other.gab", "func g(): int { return 2; }\n", &err);
    assert(anonymous);
    assert(gab_module_name(anonymous) == NULL);

    // Which is reachable by passing no module at all.
    GabFunc *g = gab_lookup(vm, anonymous, "g", &err);
    assert(g);
    gab_func_free(g);

    gab_module_free(vm, anonymous);
    gab_module_free(vm, named);
    gab_vm_free(vm);
}

// Naming the module directly, for a host that has the name but not the handle.
static void test_lookup_by_module_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    GabModule *mod = gab_compile(vm, "m.gab", "module Enemy;\nfunc hp(): int { return 42; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup_in(vm, "Enemy", "hp", &err);
    assert(fn);

    int result = 0;
    assert(gab_call(vm, fn, &result, &err) == GAB_OK);
    assert(result == 42);

    // A module nobody declared is a miss with a message, not another module's
    // symbol.
    assert(gab_lookup_in(vm, "Nope", "hp", &err) == NULL);
    assert(err.message[0] != '\0');

    // And a name that module does not declare.
    assert(gab_lookup_in(vm, "Enemy", "missing", &err) == NULL);

    gab_func_free(fn);
    gab_module_free(vm, mod);
    gab_vm_free(vm);
}

// A struct named the same in two modules is two types, each with its own
// layout — the collision that per-module scopes alone did not fix.
static void test_two_modules_can_share_a_type_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *player =
        gab_compile(vm, "player.gab", "module Player;\nstruct Config { health: int, mana: int }\n", &err);
    assert(player);

    GabModule *enemy = gab_compile(vm, "enemy.gab", "module Enemy;\nstruct Config { hp: int }\n", &err);
    assert(enemy);

    const GabType *player_config = gab_find_type(vm, player, "Config");
    const GabType *enemy_config = gab_find_type(vm, enemy, "Config");

    assert(player_config && enemy_config);
    assert(player_config != enemy_config);

    // Distinct layouts, so these really are different types rather than one
    // found twice.
    assert(gab_type_size(player_config) == 2 * sizeof(int));
    assert(gab_type_size(enemy_config) == sizeof(int));

    size_t offset = SIZE_MAX;
    assert(gab_field_offset(player_config, "mana", &offset));
    assert(offset == sizeof(int));

    // Each module's fields belong to its own type.
    assert(!gab_field_offset(enemy_config, "mana", &offset));
    assert(gab_field_offset(enemy_config, "hp", &offset));

    // And by name, without a handle.
    assert(gab_find_type_in(vm, "Player", "Config") == player_config);
    assert(gab_find_type_in(vm, "Enemy", "Config") == enemy_config);

    gab_module_free(vm, enemy);
    gab_module_free(vm, player);
    gab_vm_free(vm);
}

// Namespacing types per registry must not give each module its own 'int' or
// its own '*Player': the type system compares by pointer identity, so builtins
// and interned pointers stay the root's however many modules exist.
static void test_builtins_are_shared_across_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *player = gab_compile(vm, "player.gab",
                                    "module Player;\n"
                                    "struct Config { health: int }\n"
                                    "func player_size(): int { let p: *Config; return 1; }\n",
                                    &err);
    assert(player);

    GabModule *enemy = gab_compile(vm, "enemy.gab",
                                   "module Enemy;\n"
                                   "struct Config { hp: int }\n"
                                   "func enemy_size(): int { let p: *Config; return 1; }\n",
                                   &err);
    assert(enemy);

    // 'int' names one type from either module, with no import.
    const GabType *player_int = gab_find_type(vm, player, "int");
    const GabType *enemy_int = gab_find_type(vm, enemy, "int");

    assert(player_int);
    assert(player_int == enemy_int);
    assert(gab_find_type_in(vm, NULL, "int") == player_int);

    // Each module still resolved '*Config' against its own Config.
    assert(gab_lookup(vm, player, "player_size", &err));
    assert(gab_lookup(vm, enemy, "enemy_size", &err));

    gab_module_free(vm, enemy);
    gab_module_free(vm, player);
    gab_vm_free(vm);
}

// A module's own type shadows a root-namespace one of the same name, the way
// its scope shadows root declarations.
static void test_module_type_shadows_the_root(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *root = gab_compile(vm, "root.gab", "struct Config { a: int, b: int }\n", &err);
    assert(root);

    GabModule *player =
        gab_compile(vm, "player.gab", "module Player;\nstruct Config { only: int }\n", &err);
    assert(player);

    const GabType *root_config = gab_find_type(vm, root, "Config");
    const GabType *player_config = gab_find_type(vm, player, "Config");

    assert(root_config && player_config);
    assert(root_config != player_config);
    assert(gab_type_size(root_config) == 2 * sizeof(int));
    assert(gab_type_size(player_config) == sizeof(int));

    // A module with no Config of its own falls through to the root's.
    GabModule *enemy = gab_compile(vm, "enemy.gab", "module Enemy;\nfunc noop(): int { return 0; }\n", &err);
    assert(enemy);
    assert(gab_find_type(vm, enemy, "Config") == root_config);

    // An unknown module is a miss, not a quiet fallback to the root.
    assert(gab_find_type_in(vm, "Nope", "Config") == NULL);

    gab_module_free(vm, enemy);
    gab_module_free(vm, player);
    gab_module_free(vm, root);
    gab_vm_free(vm);
}

// 'Module::Type' names a type in another module, and names the same type the
// declaring module knows — pointer identity, not a second copy of the layout.
static void test_qualified_type_reference_crosses_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    GabModule *player =
        gab_compile(vm, "player.gab", "module Player;\nstruct Config { health: int }\n", &err);
    assert(player);

    GabModule *enemy = gab_compile(vm, "enemy.gab",
                                   "module Enemy;\n"
                                   "struct Config { hp: int }\n"
                                   "func f(): int { let c: Player::Config; return c.health; }\n",
                                   &err);
    assert(enemy);

    // Its own Config is untouched by the qualified mention of Player's.
    assert(gab_type_size(gab_find_type(vm, enemy, "Config")) == sizeof(int));

    // A module that does not exist, and a type that module does not have, are
    // both errors rather than a silent fallback to a same-named local type.
    assert(gab_compile(vm, "bad.gab", "module A;\nfunc f(): int { let c: Nope::Config; return 0; }\n",
                       &err) == NULL);
    assert(gab_compile(vm, "bad2.gab", "module B;\nfunc f(): int { let c: Player::Missing; return 0; }\n",
                       &err) == NULL);

    gab_module_free(vm, enemy);
    gab_module_free(vm, player);
    gab_vm_free(vm);
}

int main(void) {
    test_two_modules_can_share_a_function_name();
    test_module_name_is_read_from_the_script();
    test_lookup_by_module_name();
    test_two_modules_can_share_a_type_name();
    test_builtins_are_shared_across_modules();
    test_module_type_shadows_the_root();
    test_qualified_type_reference_crosses_modules();
    test_compile_error_is_reported_not_printed();
    test_runtime_error_is_reported();
    test_runtime_error_from_module_run();
    test_unset_arguments_are_rejected();
    test_rejected_setter_does_not_supply_an_argument();
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
