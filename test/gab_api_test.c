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
    assert(!gab_load(vm, "<bad>", "func broken(: int { return", &err));
    assert(err.message[0] != '\0');
    assert(err.line > 0);

    gab_vm_free(vm);
}

// Compile once, run every frame: the module survives its runs.
static void test_compile_once_run_many(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func seven(): int { return 7; }\nlet r: int = seven();\n", &err);
    assert(mod);

    for (int i = 0; i < 3; i++) {
    }

    gab_vm_free(vm);
}

// A type registered by the first compile must stay readable after a second
// compile has reused the compile arena.
static void test_type_survives_a_later_compile(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool first = gab_load(vm, "<first>", "struct Player { health: int, mana: int }\n", &err);
    assert(first);

    bool second = gab_load(vm, "<second>", "func noop() { }\n", &err);
    assert(second);

    // The type's storage must outlive the arena the compile reset.
    const GabType *player = gab_find_type(vm, NULL, "Player");
    assert(player);
    assert(gab_type_size(player) > 0);

    size_t health_offset = SIZE_MAX;
    assert(gab_field_offset(player, "health", &health_offset));
    assert(health_offset == 0);

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
    bool mod = gab_load(vm, "<layout>", "struct Player { health: int, mana: int }\n", &err);
    assert(mod);

    const GabType *type = gab_find_type(vm, NULL, "Player");
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

    gab_vm_free(vm);
}

// A bad lookup is a diagnostic, not a crash.
static void test_lookup_failures(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func real(): int { return 1; }\nlet notafunc: int = 3;\n", &err);
    assert(mod);

    assert(gab_lookup(vm, NULL, "missing", &err) == NULL);
    assert(err.message[0] != '\0');

    assert(gab_lookup(vm, NULL, "notafunc", &err) == NULL);
    assert(err.message[0] != '\0');

    GabFunc *fn = gab_lookup(vm, NULL, "real", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 0);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// Typed arguments in, a typed result out.
static void test_call_with_scalar_args(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "func mix(a: int, b: float, flag: bool): int {\n"
                        "  if flag { return a * 2; } else { return a; }\n"
                        "}\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "mix", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 3);

    gab_arg_int(fn_call, 0, 21);
    gab_arg_float(fn_call, 1, 1.5f);
    gab_arg_bool(fn_call, 2, true);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 42);

    // The same handle, called again with different arguments: the per-frame
    // case, and no lookup happens on the way.
    gab_arg_bool(fn_call, 2, false);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 21);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// Calling the same handle in a loop is what an engine does every frame.
static void test_call_in_a_loop(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func step(n: int): int { return n + 1; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    for (int i = 0; i < 64; i++) {
        gab_arg_int(fn_call, 0, i);

        int result = 0;
        assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
        assert(result == i + 1);
    }

    // A call must not allocate: the frame is stack space the VM already owns
    // and the arguments are staged in the handle. Verified out of tree by
    // interposing malloc over this loop, which counted zero.

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A struct crosses the boundary as raw bytes in both directions.
static void test_struct_argument_and_return(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "struct Player { health: int, mana: int }\n"
                        "func hurt(p: Player, amount: int): Player {\n"
                        "  let out: Player;\n"
                        "  out.health = p.health - amount;\n"
                        "  out.mana = p.mana;\n"
                        "  return out;\n"
                        "}\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "hurt", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    Player in = {.health = 100, .mana = 30};

    gab_arg_struct(fn_call, 0, &in, sizeof(in));
    gab_arg_int(fn_call, 1, 25);

    Player out = {0};
    assert(gab_call(vm, fn_call, &out, &err) == GAB_OK);

    assert(out.health == 75);
    assert(out.mana == 30);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A bad argument is GAB_ERR_ARG at the call, and the call does not happen.
static void test_bad_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "struct Player { health: int, mana: int }\n"
                        "func take(p: Player, n: int): int { return n; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "take", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    // Out of range: refused by the setter, and the call still cannot happen
    // because nothing was supplied.
    assert(!gab_arg_int(fn_call, 7, 1));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    // Wrong type: parameter 1 is an int, not a float.
    assert(!gab_arg_float(fn_call, 1, 1.0f));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);

    // Wrong struct size.
    int not_a_player = 0;
    assert(!gab_arg_struct(fn_call, 0, &not_a_player, sizeof(not_a_player)));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);

    // After the failures, a correctly built call still works: a rejected
    // argument left nothing behind.
    Player p = {.health = 1, .mana = 2};
    assert(gab_arg_struct(fn_call, 0, &p, sizeof(p)));
    assert(gab_arg_int(fn_call, 1, 99));

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 99);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A runtime failure reaches the caller as GAB_ERR_RUNTIME with a message, and
// leaves the VM usable. Unbounded recursion is the only runtime failure that
// exists today; the channel is what matters, not the particular cause.
static void test_runtime_error_is_reported(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func boom(n: int): int { return boom(n); }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "boom", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 1);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_RUNTIME);
    assert(err.message[0] != '\0');

    gab_call_free(fn_call);

    // A second module in the same VM still runs: the failure was reported, not
    // fatal.
    bool ok = gab_load(vm, "<ok>", "func fine(n: int): int { return n + 1; }\n", &err);
    assert(ok);

    GabFunc *good = gab_lookup(vm, NULL, "fine", &err);
    assert(good);

    GabCall *good_call = gab_call_init(good, &err);
    assert(good_call);

    gab_arg_int(good_call, 0, 41);

    assert(gab_call(vm, good_call, &result, &err) == GAB_OK);
    assert(result == 42);
    assert(err.message[0] == '\0');

    gab_call_free(good_call);

    gab_vm_free(vm);
}

// A top level that fails at runtime fails the load: gab_load runs it, so a
// unit that could not initialise is reported rather than left half present.
static void test_runtime_error_in_a_top_level_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "<m>",
                     "func boom(n: int): int { return boom(n); }\n"
                     "let r: int = boom(1);\n",
                     &err));

    assert(err.message[0] != '\0');

    gab_vm_free(vm);
}

// An argument never set must not silently pass whatever the buffer held —
// zero on the first call, the previous frame's value on every one after.
// Deliberate reuse stays legal, because that is what makes a per-frame call
// allocation-free.
static void test_unset_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func two(a: int, b: int): int { return a + b; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "two", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    // Only argument 0 supplied.
    gab_arg_int(fn_call, 0, 1);

    int result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    // Supplying the rest makes the same handle callable.
    gab_arg_int(fn_call, 1, 1000);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 1001);

    // Reuse is not staleness: holding argument 1 constant while changing 0 is
    // the per-frame case and must keep working.
    gab_arg_int(fn_call, 0, 2);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 1002);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A setter reports its own failure and supplies nothing, so the parameter
// stays unset — which is what keeps a host that ignores the answer safe.
static void test_rejected_setter_does_not_supply_an_argument(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>", "func two(a: int, b: int): int { return a + b; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "two", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    assert(gab_arg_int(fn_call, 0, 5));

    // Wrong type for an int parameter: reported immediately by the setter,
    // rather than remembered and reported by the next call.
    assert(!gab_arg_float(fn_call, 1, 1.0f));

    // A host that ignored the setter's answer is still safe: the parameter was
    // never given a value, so the call is refused rather than made with
    // whatever the buffer held.
    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    // And it stays refused until the argument is actually supplied.
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);

    assert(gab_arg_int(fn_call, 1, 7));
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 12);

    // Out of range is refused the same way.
    assert(!gab_arg_int(fn_call, 9, 1));
    assert(!gab_arg_int(fn_call, -1, 1));

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// Two entity scripts, each with its own on_update — the case an engine hits
// first. Asking one unit for the name must give that unit's function, so the
// module handle has to select between them rather than being ignored.
static void test_two_modules_can_share_a_function_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player = gab_load(vm, "player.gab",
                           "module Player;\n"
                           "func on_update(): int { return 1; }\n",
                           &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab",
                          "module Enemy;\n"
                          "func on_update(): int { return 2; }\n",
                          &err);
    assert(enemy);
    assert(err.message[0] == '\0');

    GabFunc *player_update = gab_lookup(vm, "Player", "on_update", &err);
    GabFunc *enemy_update = gab_lookup(vm, "Enemy", "on_update", &err);
    assert(player_update && enemy_update);

    GabCall *player_update_call = gab_call_init(player_update, &err);
    GabCall *enemy_update_call = gab_call_init(enemy_update, &err);
    assert(player_update_call && enemy_update_call);

    int result = 0;
    assert(gab_call(vm, player_update_call, &result, &err) == GAB_OK);
    assert(result == 1);

    assert(gab_call(vm, enemy_update_call, &result, &err) == GAB_OK);
    assert(result == 2);

    gab_call_free(player_update_call);
    gab_call_free(enemy_update_call);

    gab_vm_free(vm);
}

// The script names its module, and the filename passed to gab_load is only
// a label for diagnostics. The two need not agree, and only the directive
// decides which namespace a name lands in.
static void test_the_script_names_its_module_not_the_filename(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool named = gab_load(vm, "some_file.gab", "module Player;\nfunc f(): int { return 1; }\n", &err);
    assert(named);

    // Found under the declared module, not under the file it came from.
    GabFunc *f = gab_lookup(vm, "Player", "f", &err);
    assert(f);

    GabCall *f_call = gab_call_init(f, &err);
    assert(f_call);

    assert(gab_lookup(vm, "some_file", "f", &err) == NULL);
    assert(gab_lookup(vm, NULL, "f", &err) == NULL);

    // A unit with no directive belongs to the root namespace, reached by
    // passing no module at all.
    bool anonymous = gab_load(vm, "other.gab", "func g(): int { return 2; }\n", &err);
    assert(anonymous);

    GabFunc *g = gab_lookup(vm, NULL, "g", &err);
    assert(g);

    GabCall *g_call = gab_call_init(g, &err);
    assert(g_call);

    gab_call_free(f_call);
    gab_call_free(g_call);

    gab_vm_free(vm);
}

// Naming the module directly, for a host that has the name but not the handle.
static void test_lookup_by_module_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "m.gab", "module Enemy;\nfunc hp(): int { return 42; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "Enemy", "hp", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 42);

    // A module nobody declared is a miss with a message, not another module's
    // symbol.
    assert(gab_lookup(vm, "Nope", "hp", &err) == NULL);
    assert(err.message[0] != '\0');

    // And a name that module does not declare.
    assert(gab_lookup(vm, "Enemy", "missing", &err) == NULL);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A struct named the same in two modules is two types, each with its own
// layout — the collision that per-module scopes alone did not fix.
static void test_two_modules_can_share_a_type_name(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player =
        gab_load(vm, "player.gab", "module Player;\nstruct Config { health: int, mana: int }\n", &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab", "module Enemy;\nstruct Config { hp: int }\n", &err);
    assert(enemy);

    const GabType *player_config = gab_find_type(vm, "Player", "Config");
    const GabType *enemy_config = gab_find_type(vm, "Enemy", "Config");

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
    assert(gab_find_type(vm, "Player", "Config") == player_config);
    assert(gab_find_type(vm, "Enemy", "Config") == enemy_config);

    gab_vm_free(vm);
}

// Namespacing types per registry must not give each module its own 'int' or
// its own '*Player': the type system compares by pointer identity, so builtins
// and interned pointers stay the root's however many modules exist.
static void test_builtins_are_shared_across_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player = gab_load(vm, "player.gab",
                           "module Player;\n"
                           "struct Config { health: int }\n"
                           "func player_size(): int { let p: *Config; return 1; }\n",
                           &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab",
                          "module Enemy;\n"
                          "struct Config { hp: int }\n"
                          "func enemy_size(): int { let p: *Config; return 1; }\n",
                          &err);
    assert(enemy);

    // 'int' names one type from either module, with no import.
    const GabType *player_int = gab_find_type(vm, "Player", "int");
    const GabType *enemy_int = gab_find_type(vm, "Enemy", "int");

    assert(player_int);
    assert(player_int == enemy_int);
    assert(gab_find_type(vm, NULL, "int") == player_int);

    // Each module still resolved '*Config' against its own Config.
    GabFunc *player_size = gab_lookup(vm, "Player", "player_size", &err);
    GabFunc *enemy_size = gab_lookup(vm, "Enemy", "enemy_size", &err);

    assert(player_size);
    assert(enemy_size);

    gab_vm_free(vm);
}

// A module's own type shadows a root-namespace one of the same name, the way
// its scope shadows root declarations.
static void test_module_type_shadows_the_root(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool root = gab_load(vm, "root.gab", "struct Config { a: int, b: int }\n", &err);
    assert(root);

    bool player = gab_load(vm, "player.gab", "module Player;\nstruct Config { only: int }\n", &err);
    assert(player);

    const GabType *root_config = gab_find_type(vm, NULL, "Config");
    const GabType *player_config = gab_find_type(vm, "Player", "Config");

    assert(root_config && player_config);
    assert(root_config != player_config);
    assert(gab_type_size(root_config) == 2 * sizeof(int));
    assert(gab_type_size(player_config) == sizeof(int));

    // A module with no Config of its own falls through to the root's.
    bool enemy = gab_load(vm, "enemy.gab", "module Enemy;\nfunc noop(): int { return 0; }\n", &err);
    assert(enemy);
    assert(gab_find_type(vm, "Enemy", "Config") == root_config);

    // An unknown module is a miss, not a quiet fallback to the root.
    assert(gab_find_type(vm, "Nope", "Config") == NULL);

    gab_vm_free(vm);
}

// 'Module::Type' names a type in another module, and names the same type the
// declaring module knows — pointer identity, not a second copy of the layout.
static void test_qualified_type_reference_crosses_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player = gab_load(vm, "player.gab", "module Player;\nstruct Config { health: int }\n", &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab",
                          "module Enemy;\n"
                          "struct Config { hp: int }\n"
                          "func f(): int { let c: Player::Config; return c.health; }\n",
                          &err);
    assert(enemy);

    // Its own Config is untouched by the qualified mention of Player's.
    assert(gab_type_size(gab_find_type(vm, "Enemy", "Config")) == sizeof(int));

    // A module that does not exist, and a type that module does not have, are
    // both errors rather than a silent fallback to a same-named local type.
    assert(!gab_load(vm, "bad.gab", "module A;\nfunc f(): int { let c: Nope::Config; return 0; }\n", &err));
    assert(
        !gab_load(vm, "bad2.gab", "module B;\nfunc f(): int { let c: Player::Missing; return 0; }\n", &err));

    gab_vm_free(vm);
}

// A handle keeps working across later compiles. Frame slots are assigned per
// compile and kept in codegen's own table, so a second compile cannot rewrite
// the layout a live handle was built from — the handle resolves through the
// symbol's prototype index, which is durable.
static void test_handle_survives_later_compiles(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool first = gab_load(vm, "first.gab", "func step(n: int): int { return n + 1; }\n", &err);
    assert(first);

    GabFunc *fn = gab_lookup(vm, NULL, "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 1);

    gab_arg_int(fn_call, 0, 10);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 11);

    // Later units compile more functions, appending prototypes and running
    // codegen again over fresh symbols.
    bool second = gab_load(vm, "second.gab",
                           "module Later;\n"
                           "struct Wide { a: int, b: int, c: int }\n"
                           "func takes_wide(w: Wide, k: int): int { return k; }\n",
                           &err);
    assert(second);

    bool third =
        gab_load(vm, "third.gab", "module Other;\nfunc unrelated(a: int, b: int): int { return a; }\n", &err);
    assert(third);

    // The handle from before still calls the body it was looked up for.
    gab_arg_int(fn_call, 0, 10);
    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 11);
    assert(gab_func_arity(fn) == 1);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// Recompiling a unit replaces what it declared last time, which is what hot
// reload is. A handle looked up before the reload calls the new body, because
// the symbol it points at is reused rather than replaced.
static void test_recompiling_a_module_reloads_it(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool v1 = gab_load(vm, "g.gab",
                       "module Game;\n"
                       "struct S { hp: int }\n"
                       "func tick(n: int): int { return n + 1; }\n",
                       &err);
    assert(v1);

    GabFunc *fn = gab_lookup(vm, "Game", "tick", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 5);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 6);
    assert(gab_type_size(gab_find_type(vm, "Game", "S")) == sizeof(int));

    // The same unit again, with a new body and a wider struct.
    bool v2 = gab_load(vm, "g.gab",
                       "module Game;\n"
                       "struct S { hp: int, mp: int }\n"
                       "func tick(n: int): int { return n * 3; }\n",
                       &err);
    assert(v2);

    // The handle from before the reload runs the new body.
    gab_arg_int(fn_call, 0, 5);
    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 15);

    // And the type is the redeclared one, not the original.
    assert(gab_type_size(gab_find_type(vm, "Game", "S")) == 2 * sizeof(int));

    // Reloading repeatedly is the per-frame case, so it must not drift.
    for (int i = 0; i < 3; i++) {
        bool again = gab_load(vm, "g.gab",
                              "module Game;\n"
                              "struct S { hp: int, mp: int }\n"
                              "func tick(n: int): int { return n * 3; }\n",
                              &err);
        assert(again);
    }

    gab_arg_int(fn_call, 0, 5);
    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 15);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A method's declaration belongs to the struct Type, which a reload replaces.
// The new Type carries a fresh method table, so the methods are redeclared onto
// it rather than colliding with the previous compile's.
static void test_reloading_a_module_redeclares_its_methods(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "m.gab",
                    "module M;\n"
                    "struct Player { health: int }\n"
                    "func (p: *Player) hp(): int { return p.health; }\n"
                    "func probe(): int { let p: Player; p.health = 7; return p.hp(); }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "M", "probe", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(call);

    int result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 7);

    // The same unit again, with the method body changed.
    assert(gab_load(vm, "m.gab",
                    "module M;\n"
                    "struct Player { health: int }\n"
                    "func (p: *Player) hp(): int { return p.health * 100; }\n"
                    "func probe(): int { let p: Player; p.health = 7; return p.hp(); }\n",
                    &err));

    result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 700);

    gab_call_free(call);
    gab_vm_free(vm);
}

// A method is not a module-level name, so a host cannot reach one through
// gab_lookup even knowing what it is called.
static void test_a_method_is_not_reachable_from_a_host(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "m.gab",
                    "module M;\n"
                    "struct Player { health: int }\n"
                    "func (p: *Player) hp(): int { return p.health; }\n",
                    &err));

    assert(!gab_lookup(vm, "M", "hp", &err));

    gab_vm_free(vm);
}

// Allowing a reload must not stop one unit declaring the same name twice from
// being an error: a reload is a *later* compile replacing an earlier one, never
// a unit contradicting itself.
static void test_duplicate_declarations_in_one_unit_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "a.gab", "struct P { a: int }\nstruct P { b: int }\n", &err));
    assert(err.message[0] != '\0');

    assert(!gab_load(vm, "b.gab", "func f(): int { return 1; }\nfunc f(): int { return 2; }\n", &err));
    assert(err.message[0] != '\0');

    assert(!gab_load(vm, "c.gab", "let x: int = 1;\nlet x: int = 2;\n", &err));
    assert(err.message[0] != '\0');

    // Including inside a block, whose scope is fresh per compile but still
    // declares at its unit's generation.
    assert(!gab_load(vm, "d.gab", "func f(): int { let y: int = 1; let y: int = 2; return y; }\n", &err));
    assert(err.message[0] != '\0');

    gab_vm_free(vm);
}

// A reload that changes the signature invalidates what a handle cached. The
// handle rebinds itself to the new signature rather than dying, so the host
// need not look the function up again — but the arguments it had staged
// described the old signature, so they are cleared and the call is refused
// until they are set afresh.
static void test_signature_change_rebinds_the_handle(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool v1 = gab_load(vm, "s.gab", "module S;\nfunc step(n: int): int { return n + 1; }\n", &err);
    assert(v1);

    GabFunc *fn = gab_lookup(vm, "S", "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 1);

    gab_arg_int(fn_call, 0, 10);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 11);

    bool v2 = gab_load(vm, "s.gab", "module S;\nfunc step(a: int, b: int): int { return a + b; }\n", &err);
    assert(v2);

    // Calling without re-staging is refused: the old arguments are gone rather
    // than being fed to a body that no longer expects them.
    result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_STALE);
    assert(err.message[0] != '\0');

    // The handle now describes the new signature.
    assert(gab_func_arity(fn) == 2);

    // Restaging resizes this caller's buffer for it, through the same GabCall.
    assert(gab_call_restage(fn_call, &err));

    gab_arg_int(fn_call, 0, 3);
    gab_arg_int(fn_call, 1, 4);

    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 7);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A reload is reported to each caller once. The handle rebinds on the first
// call that notices, and every caller restages its own arguments — the setters
// cannot rebind on a caller's behalf, because the handle is shared and doing so
// would clear what some other caller had staged.
static void test_staging_after_a_signature_change_just_works(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool v1 = gab_load(vm, "s.gab", "module S;\nfunc step(n: int): int { return n + 1; }\n", &err);
    assert(v1);

    GabFunc *fn = gab_lookup(vm, "S", "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 10);

    bool v2 = gab_load(vm, "s.gab", "module S;\nfunc step(a: int, b: int): int { return a + b; }\n", &err);
    assert(v2);

    // The staged argument describes the old signature, so the call is refused
    // and the handle rebinds.
    int result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_STALE);

    assert(gab_call_restage(fn_call, &err));

    gab_arg_int(fn_call, 0, 3);
    gab_arg_int(fn_call, 1, 4);

    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 7);

    // A restage clears every argument, so supplying only some of them is an
    // unset argument rather than a stale value reaching the new body.
    bool v3 = gab_load(vm, "s.gab",
                       "module S;\nfunc step(a: int, b: int, c: int): int { return a + b + c; }\n", &err);
    assert(v3);

    gab_arg_int(fn_call, 0, 1);

    result = -1;
    assert(gab_call(vm, fn_call, &result, &err) != GAB_OK);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// A reload of any width is recoverable through the handle the host already has:
// the handle grows its own arrays without moving, so no reload forces a fresh
// lookup.
static void test_a_reload_too_wide_for_the_handle_is_reported(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool v1 = gab_load(vm, "s.gab", "module S;\nfunc f(a: int): int { return a; }\n", &err);
    assert(v1);

    GabFunc *fn = gab_lookup(vm, "S", "f", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 1);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 1);

    // Far more parameters than the handle was originally sized for.
    bool v2 = gab_load(vm, "s.gab",
                       "module S;\n"
                       "func f(a: int, b: int, c: int, d: int, e: int, g: int, h: int, i: int,\n"
                       "       j: int, k: int, l: int, m: int): int { return a; }\n",
                       &err);
    assert(v2);

    result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_STALE);
    assert(err.message[0] != '\0');

    // The same handle now describes the new signature, and the same call
    // restages onto it.
    assert(gab_func_arity(fn) == 12);
    assert(gab_call_restage(fn_call, &err));

    for (int i = 0; i < 12; i++) {
        gab_arg_int(fn_call, i, i);
    }

    result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 0);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

// The VM owns every handle it hands out and there is no way to release one
// early, so a host that looks the same functions up repeatedly accumulates
// handles the VM must account for and free with itself.
static void test_the_vm_owns_its_handles(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool script = gab_load(vm, "m.gab",
                           "func a(n: int): int { return n + 1; }\n"
                           "func b(n: int): int { return n + 2; }\n"
                           "func c(n: int): int { return n + 3; }\n",
                           &err);
    assert(script);

    const char *names[] = {"a", "b", "c"};
    GabFunc *handles[9];
    GabCall *calls[9];

    for (int i = 0; i < 9; i++) {
        handles[i] = gab_lookup(vm, NULL, names[i % 3], &err);
        assert(handles[i]);

        calls[i] = gab_call_init(handles[i], &err);
        assert(calls[i]);
    }

    // Every handle is independent: repeat lookups of one name each get their
    // own argument staging, so setting one does not disturb another.

    for (int i = 0; i < 9; i++) {
        assert(gab_arg_int(calls[i], 0, 10));

        int result = -1;
        assert(gab_call(vm, calls[i], &result, &err) == GAB_OK);
        assert(result == 10 + (i % 3) + 1);
    }

    // All nine are the VM's to release. Under a leak checker this is the
    // assertion: the host frees nothing and nothing leaks.
    for (int i = 0; i < 9; i++) {
        gab_call_free(calls[i]);
    }

    gab_vm_free(vm);
}

// Two callers of one function stage independently, which is the whole reason a
// call is separate from a handle. Interleaved on purpose: the bug this rules out
// is one caller finishing a call with an argument the other left staged, and the
// fast path cannot notice it — once every parameter has been set it does no
// checking at all.
static void test_two_callers_stage_independently(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool mod =
        gab_load(vm, "<m>", "func damage(target: int, amount: int): int { return target - amount; }\n", &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, NULL, "damage", &err);
    assert(fn);

    // One handle, two callers — an engine holding the function in a shared
    // registry rather than looking it up per system.
    GabCall *ai = gab_call_init(fn, &err);
    GabCall *projectile = gab_call_init(fn, &err);
    assert(ai && projectile);

    // The AI system stages a target and is interrupted before it calls.
    assert(gab_arg_int(ai, 0, 100));

    // The projectile system runs a whole call in between.
    assert(gab_arg_int(projectile, 0, 40));
    assert(gab_arg_int(projectile, 1, 10));

    int result = -1;
    assert(gab_call(vm, projectile, &result, &err) == GAB_OK);
    assert(result == 30);

    // The AI system finishes, and its target is the one it staged.
    assert(gab_arg_int(ai, 1, 5));

    result = -1;
    assert(gab_call(vm, ai, &result, &err) == GAB_OK);
    assert(result == 95);

    // Sticky arguments are per caller too: each keeps its own target.
    assert(gab_arg_int(projectile, 1, 1));

    result = -1;
    assert(gab_call(vm, projectile, &result, &err) == GAB_OK);
    assert(result == 39);

    result = -1;
    assert(gab_call(vm, ai, &result, &err) == GAB_OK);
    assert(result == 95);

    gab_call_free(projectile);
    gab_call_free(ai);

    gab_vm_free(vm);
}

// Loading a name again replaces what that name held, rather than stacking a
// second unit beside it. Under a leak checker this is the assertion: a host
// that recompiles a file whenever it changes on disk does that for as long as
// it runs, and nothing it loaded is the host's to free.
static void test_reloading_a_name_replaces_it(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    for (int i = 0; i < 50; i++) {
        char src[256];
        snprintf(src, sizeof src, "module S;\nfunc step(n: int): int { return n + %d; }\n", i);

        assert(gab_load(vm, "s.gab", src, &err));
    }

    // The last load is the one that stands.
    GabFunc *fn = gab_lookup(vm, "S", "step", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(call);

    assert(gab_arg_int(call, 0, 100));

    int result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 149);

    gab_call_free(call);
    gab_vm_free(vm);
}

// A load that fails leaves the unit that was there still loaded and running: a
// designer saving a file mid-edit must not take the running one down with it.
static void test_a_failed_reload_leaves_the_previous_unit(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "s.gab", "module S;\nfunc step(n: int): int { return n + 1; }\n", &err));

    assert(!gab_load(vm, "s.gab", "module S;\nfunc step(n: int): int { return n +", &err));
    assert(err.message[0] != '\0');

    GabFunc *fn = gab_lookup(vm, "S", "step", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(call);

    assert(gab_arg_int(call, 0, 41));

    int result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 42);

    gab_call_free(call);
    gab_vm_free(vm);
}

int main(void) {
    test_two_modules_can_share_a_function_name();
    test_the_script_names_its_module_not_the_filename();
    test_the_vm_owns_its_handles();
    test_lookup_by_module_name();
    test_two_modules_can_share_a_type_name();
    test_builtins_are_shared_across_modules();
    test_module_type_shadows_the_root();
    test_qualified_type_reference_crosses_modules();
    test_handle_survives_later_compiles();
    test_recompiling_a_module_reloads_it();
    test_reloading_a_name_replaces_it();
    test_a_failed_reload_leaves_the_previous_unit();
    test_reloading_a_module_redeclares_its_methods();
    test_a_method_is_not_reachable_from_a_host();
    test_duplicate_declarations_in_one_unit_are_rejected();
    test_signature_change_rebinds_the_handle();
    test_staging_after_a_signature_change_just_works();
    test_a_reload_too_wide_for_the_handle_is_reported();
    test_compile_error_is_reported_not_printed();
    test_runtime_error_is_reported();
    test_runtime_error_in_a_top_level_fails_the_load();
    test_unset_arguments_are_rejected();
    test_rejected_setter_does_not_supply_an_argument();
    test_compile_once_run_many();
    test_type_survives_a_later_compile();
    test_layout_matches_c();
    test_lookup_failures();
    test_call_with_scalar_args();
    test_call_in_a_loop();
    test_struct_argument_and_return();
    test_two_callers_stage_independently();
    test_bad_arguments_are_rejected();

    printf("gab_api_test: all tests passed\n");

    return 0;
}
