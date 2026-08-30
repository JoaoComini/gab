#include "gab.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_compile_error_is_reported_not_printed(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    assert(!gab_load(vm, "<bad>",
                     "module test;\n"
                     "func broken(: int { return",
                     &err));
    assert(err.message[0] != '\0');
    assert(err.line > 0);

    gab_vm_free(vm);
}

static void test_compile_once_run_many(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func seven(): int { return 7; }\nlet r: int = seven();\n",
                        &err);
    assert(mod);

    for (int i = 0; i < 3; i++) {
    }

    gab_vm_free(vm);
}

static void test_type_survives_a_later_compile(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool first = gab_load(vm, "<first>",
                          "module test;\n"
                          "struct Player { health: int, mana: int }\n",
                          &err);
    assert(first);

    bool second = gab_load(vm, "<second>",
                           "module test;\n"
                           "func noop() { }\n",
                           &err);
    assert(second);

    const GabType *player = gab_find_type(vm, "test", "Player");
    assert(player);
    assert(gab_type_size(vm, player) > 0);

    size_t health_offset = SIZE_MAX;
    assert(gab_field_offset(vm, player, "health", &health_offset));
    assert(health_offset == 0);

    gab_vm_free(vm);
}

typedef struct {
    int health;
    int mana;
} Player;

static void test_layout_matches_c(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<layout>",
                        "module test;\n"
                        "struct Player { health: int, mana: int }\n",
                        &err);
    assert(mod);

    const GabType *type = gab_find_type(vm, "test", "Player");
    assert(type);

    assert(gab_type_size(vm, type) == sizeof(Player));
    assert(gab_type_align(vm, type) == _Alignof(Player));

    size_t offset = SIZE_MAX;

    assert(gab_field_offset(vm, type, "health", &offset));
    assert(offset == offsetof(Player, health));

    assert(gab_field_offset(vm, type, "mana", &offset));
    assert(offset == offsetof(Player, mana));

    assert(!gab_field_offset(vm, type, "stamina", &offset));

    gab_vm_free(vm);
}

static void test_lookup_failures(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func real(): int { return 1; }\nlet notafunc: int = 3;\n",
                        &err);
    assert(mod);

    assert(gab_lookup(vm, "test", "missing", &err) == NULL);
    assert(err.message[0] != '\0');

    assert(gab_lookup(vm, "test", "notafunc", &err) == NULL);
    assert(err.message[0] != '\0');

    GabFunc *fn = gab_lookup(vm, "test", "real", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 0);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_a_host_call_reaches_a_builtin_method(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func size(): int { let s: ref str = \"abcd\"; return s.len(); }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "size", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 4);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_call_with_scalar_args(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func mix(a: int, b: float, flag: bool): int {\n"
                        "  if flag { return a * 2; } else { return a; }\n"
                        "}\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "mix", &err);
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

    gab_arg_bool(fn_call, 2, false);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 21);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_call_in_a_loop(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func step(n: int): int { return n + 1; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    for (int i = 0; i < 64; i++) {
        gab_arg_int(fn_call, 0, i);

        int result = 0;
        assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
        assert(result == i + 1);
    }

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_struct_argument_and_return(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "struct Player { health: int, mana: int }\n"
                        "func hurt(p: Player, amount: int): Player {\n"
                        "  let out: Player;\n"
                        "  out.health = p.health - amount;\n"
                        "  out.mana = p.mana;\n"
                        "  return out;\n"
                        "}\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "hurt", &err);
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

static void test_bad_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "struct Player { health: int, mana: int }\n"
                        "func take(p: Player, n: int): int { return n; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "take", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    assert(!gab_arg_int(fn_call, 7, 1));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    assert(!gab_arg_float(fn_call, 1, 1.0f));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);

    int not_a_player = 0;
    assert(!gab_arg_struct(fn_call, 0, &not_a_player, sizeof(not_a_player)));
    assert(gab_call(vm, fn_call, NULL, &err) == GAB_ERR_ARG);

    Player p = {.health = 1, .mana = 2};
    assert(gab_arg_struct(fn_call, 0, &p, sizeof(p)));
    assert(gab_arg_int(fn_call, 1, 99));

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 99);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_runtime_error_is_reported(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func boom(n: int): int { return boom(n); }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "boom", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 1);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_RUNTIME);
    assert(err.message[0] != '\0');

    gab_call_free(fn_call);

    bool ok = gab_load(vm, "<ok>",
                       "module test;\n"
                       "func fine(n: int): int { return n + 1; }\n",
                       &err);
    assert(ok);

    GabFunc *good = gab_lookup(vm, "test", "fine", &err);
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

static void test_runtime_error_in_a_top_level_fails_the_load(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "<m>",
                     "module test;\n"
                     "func boom(n: int): int { return boom(n); }\n"
                     "let r: int = boom(1);\n",
                     &err));

    assert(err.message[0] != '\0');

    gab_vm_free(vm);
}

static void test_unset_arguments_are_rejected(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func two(a: int, b: int): int { return a + b; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "two", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    gab_arg_int(fn_call, 0, 1);

    int result = -1;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    gab_arg_int(fn_call, 1, 1000);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 1001);

    gab_arg_int(fn_call, 0, 2);
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 1002);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_rejected_setter_does_not_supply_an_argument(void) {
    GabVM *vm = gab_vm_new();

    GabError err;
    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func two(a: int, b: int): int { return a + b; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "two", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);

    assert(gab_arg_int(fn_call, 0, 5));

    assert(!gab_arg_float(fn_call, 1, 1.0f));

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);
    assert(err.message[0] != '\0');

    assert(gab_call(vm, fn_call, &result, &err) == GAB_ERR_ARG);

    assert(gab_arg_int(fn_call, 1, 7));
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 12);

    assert(!gab_arg_int(fn_call, 9, 1));
    assert(!gab_arg_int(fn_call, -1, 1));

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

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

static void test_the_script_names_its_module_not_the_filename(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool named = gab_load(vm, "some_file.gab", "module Player;\nfunc f(): int { return 1; }\n", &err);
    assert(named);

    GabFunc *f = gab_lookup(vm, "Player", "f", &err);
    assert(f);

    GabCall *f_call = gab_call_init(f, &err);
    assert(f_call);

    assert(gab_lookup(vm, "some_file", "f", &err) == NULL);
    assert(gab_lookup(vm, "test", "f", &err) == NULL);

    bool anonymous = gab_load(vm, "other.gab",
                              "module test;\n"
                              "func g(): int { return 2; }\n",
                              &err);
    assert(anonymous);

    GabFunc *g = gab_lookup(vm, "test", "g", &err);
    assert(g);

    GabCall *g_call = gab_call_init(g, &err);
    assert(g_call);

    gab_call_free(f_call);
    gab_call_free(g_call);

    gab_vm_free(vm);
}

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

    assert(gab_lookup(vm, "Nope", "hp", &err) == NULL);
    assert(err.message[0] != '\0');

    assert(gab_lookup(vm, "Enemy", "missing", &err) == NULL);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

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

    assert(gab_type_size(vm, player_config) == 2 * sizeof(int));
    assert(gab_type_size(vm, enemy_config) == sizeof(int));

    size_t offset = SIZE_MAX;
    assert(gab_field_offset(vm, player_config, "mana", &offset));
    assert(offset == sizeof(int));

    assert(!gab_field_offset(vm, enemy_config, "mana", &offset));
    assert(gab_field_offset(vm, enemy_config, "hp", &offset));

    assert(gab_find_type(vm, "Player", "Config") == player_config);
    assert(gab_find_type(vm, "Enemy", "Config") == enemy_config);

    gab_vm_free(vm);
}

static void test_builtins_are_shared_across_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player = gab_load(vm, "player.gab",
                           "module Player;\n"
                           "struct Config { health: int }\n"
                           "func player_size(): int { let p: box Config; return 1; }\n",
                           &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab",
                          "module Enemy;\n"
                          "struct Config { hp: int }\n"
                          "func enemy_size(): int { let p: box Config; return 1; }\n",
                          &err);
    assert(enemy);

    const GabType *player_int = gab_find_type(vm, "Player", "int");
    const GabType *enemy_int = gab_find_type(vm, "Enemy", "int");

    assert(player_int);
    assert(player_int == enemy_int);

    GabFunc *player_size = gab_lookup(vm, "Player", "player_size", &err);
    GabFunc *enemy_size = gab_lookup(vm, "Enemy", "enemy_size", &err);

    assert(player_size);
    assert(enemy_size);

    gab_vm_free(vm);
}

static void test_module_type_shadows_the_root(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool root = gab_load(vm, "root.gab",
                         "module test;\n"
                         "struct Config { a: int, b: int }\n",
                         &err);
    assert(root);

    bool player = gab_load(vm, "player.gab", "module Player;\nstruct Config { only: int }\n", &err);
    assert(player);

    const GabType *root_config = gab_find_type(vm, "test", "Config");
    const GabType *player_config = gab_find_type(vm, "Player", "Config");

    assert(root_config && player_config);
    assert(root_config != player_config);
    assert(gab_type_size(vm, root_config) == 2 * sizeof(int));
    assert(gab_type_size(vm, player_config) == sizeof(int));

    bool enemy = gab_load(vm, "enemy.gab", "module Enemy;\nfunc noop(): int { return 0; }\n", &err);
    assert(enemy);
    assert(gab_find_type(vm, "Enemy", "Config") == NULL);

    assert(gab_find_type(vm, "Nope", "Config") == NULL);

    gab_vm_free(vm);
}

static void test_a_module_may_be_imported_twice(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "a.gab", "module A;\nstruct T { v: int }\n", &err));
    assert(gab_load(vm, "b.gab",
                    "module B;\n"
                    "import A;\n"
                    "import A;\n"
                    "func f(): int { let t: A::T; return t.v; }\n",
                    &err));

    gab_vm_free(vm);
}

static void test_an_import_names_a_loaded_module(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "a.gab", "module A;\nimport Absent;\nfunc f(): int { return 1; }\n", &err));
    assert(strstr(err.message, "Absent"));

    assert(!gab_load(vm, "b.gab", "module B;\nimport B;\nfunc f(): int { return 1; }\n", &err));
    assert(err.message[0] != '\0');

    gab_vm_free(vm);
}

static void test_modules_cannot_import_each_other(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "base.gab", "module Base;\nstruct T { v: int }\n", &err));
    assert(gab_load(vm, "mid.gab",
                    "module Mid;\n"
                    "import Base;\n"
                    "func f(): int { let t: Base::T; return t.v; }\n",
                    &err));

    assert(!gab_load(vm, "base2.gab", "module Base;\nimport Mid;\nfunc g(): int { return 1; }\n", &err));
    assert(strstr(err.message, "Mid"));

    gab_vm_free(vm);
}

static void test_a_qualified_name_needs_an_import(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "a.gab", "module A;\nstruct Thing { v: int }\n", &err));

    assert(!gab_load(vm, "b.gab", "module B;\nfunc f(): int { let t: A::Thing; return t.v; }\n", &err));
    assert(err.message[0] != '\0');

    assert(gab_load(vm, "b.gab",
                    "module B;\n"
                    "import A;\n"
                    "func f(): int { let t: A::Thing; return t.v; }\n",
                    &err));

    assert(gab_load(vm, "a2.gab",
                    "module A;\n"
                    "func g(): int { let t: A::Thing; return t.v; }\n",
                    &err));

    gab_vm_free(vm);
}

static void test_qualified_type_reference_crosses_modules(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool player = gab_load(vm, "player.gab", "module Player;\nstruct Config { health: int }\n", &err);
    assert(player);

    bool enemy = gab_load(vm, "enemy.gab",
                          "module Enemy;\n"
                          "import Player;\n"
                          "struct Config { hp: int }\n"
                          "func f(): int { let c: Player::Config; return c.health; }\n",
                          &err);
    assert(enemy);

    assert(gab_type_size(vm, gab_find_type(vm, "Enemy", "Config")) == sizeof(int));

    assert(!gab_load(vm, "bad.gab",
                     "module A;\nimport Nope;\nfunc f(): int { let c: Nope::Config; return 0; }\n", &err));
    assert(
        !gab_load(vm, "bad2.gab", "module B;\nfunc f(): int { let c: Player::Missing; return 0; }\n", &err));

    gab_vm_free(vm);
}

static void test_handle_survives_later_compiles(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool first = gab_load(vm, "first.gab",
                          "module test;\n"
                          "func step(n: int): int { return n + 1; }\n",
                          &err);
    assert(first);

    GabFunc *fn = gab_lookup(vm, "test", "step", &err);
    assert(fn);

    GabCall *fn_call = gab_call_init(fn, &err);
    assert(fn_call);
    assert(gab_func_arity(fn) == 1);

    gab_arg_int(fn_call, 0, 10);

    int result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 11);

    bool second = gab_load(vm, "second.gab",
                           "module Later;\n"
                           "struct Wide { a: int, b: int, c: int }\n"
                           "func takes_wide(w: Wide, k: int): int { return k; }\n",
                           &err);
    assert(second);

    bool third =
        gab_load(vm, "third.gab", "module Other;\nfunc unrelated(a: int, b: int): int { return a; }\n", &err);
    assert(third);

    gab_arg_int(fn_call, 0, 10);
    result = 0;
    assert(gab_call(vm, fn_call, &result, &err) == GAB_OK);
    assert(result == 11);
    assert(gab_func_arity(fn) == 1);

    gab_call_free(fn_call);

    gab_vm_free(vm);
}

static void test_a_method_is_not_reachable_from_a_host(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "m.gab",
                    "module M;\n"
                    "struct Player { health: int }\n"
                    "func Player::hp(p: ref Player): int { return p.health; }\n",
                    &err));

    assert(!gab_lookup(vm, "M", "hp", &err));

    gab_vm_free(vm);
}

static void test_a_name_may_only_be_declared_once(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "a.gab",
                     "module test;\n"
                     "struct P { a: int }\nstruct P { b: int }\n",
                     &err));
    assert(err.message[0] != '\0');

    assert(!gab_load(vm, "b.gab",
                     "module test;\n"
                     "func f(): int { return 1; }\nfunc f(): int { return 2; }\n",
                     &err));
    assert(err.message[0] != '\0');

    assert(!gab_load(vm, "c.gab",
                     "module test;\n"
                     "let x: int = 1;\nlet x: int = 2;\n",
                     &err));
    assert(err.message[0] != '\0');

    assert(!gab_load(vm, "d.gab",
                     "module test;\n"
                     "func f(): int { let y: int = 1; let y: int = 2; return y; }\n",
                     &err));
    assert(err.message[0] != '\0');

    assert(gab_load(vm, "first.gab", "module M;\nfunc shared(): int { return 1; }\n", &err));
    assert(!gab_load(vm, "second.gab", "module M;\nfunc shared(): int { return 2; }\n", &err));

    assert(strstr(err.message, "shared"));
    assert(err.line == 2);

    assert(!gab_load(vm, "first.gab", "module M;\nfunc shared(): int { return 3; }\n", &err));
    assert(err.message[0] != '\0');

    gab_vm_free(vm);
}

static void test_the_vm_owns_its_handles(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool script = gab_load(vm, "m.gab",
                           "module test;\n"
                           "func a(n: int): int { return n + 1; }\n"
                           "func b(n: int): int { return n + 2; }\n"
                           "func c(n: int): int { return n + 3; }\n",
                           &err);
    assert(script);

    const char *names[] = {"a", "b", "c"};
    GabFunc *handles[9];
    GabCall *calls[9];

    for (int i = 0; i < 9; i++) {
        handles[i] = gab_lookup(vm, "test", names[i % 3], &err);
        assert(handles[i]);

        calls[i] = gab_call_init(handles[i], &err);
        assert(calls[i]);
    }

    for (int i = 0; i < 9; i++) {
        assert(gab_arg_int(calls[i], 0, 10));

        int result = -1;
        assert(gab_call(vm, calls[i], &result, &err) == GAB_OK);
        assert(result == 10 + (i % 3) + 1);
    }

    for (int i = 0; i < 9; i++) {
        gab_call_free(calls[i]);
    }

    gab_vm_free(vm);
}

static void test_two_callers_stage_independently(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    bool mod = gab_load(vm, "<m>",
                        "module test;\n"
                        "func damage(target: int, amount: int): int { return target - amount; }\n",
                        &err);
    assert(mod);

    GabFunc *fn = gab_lookup(vm, "test", "damage", &err);
    assert(fn);

    GabCall *ai = gab_call_init(fn, &err);
    GabCall *projectile = gab_call_init(fn, &err);
    assert(ai && projectile);

    assert(gab_arg_int(ai, 0, 100));

    assert(gab_arg_int(projectile, 0, 40));
    assert(gab_arg_int(projectile, 1, 10));

    int result = -1;
    assert(gab_call(vm, projectile, &result, &err) == GAB_OK);
    assert(result == 30);

    assert(gab_arg_int(ai, 1, 5));

    result = -1;
    assert(gab_call(vm, ai, &result, &err) == GAB_OK);
    assert(result == 95);

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

static void test_a_failed_load_declares_nothing(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(!gab_load(vm, "a.gab",
                     "module M;\n"
                     "func ready(): int { return 1; }\n"
                     "extern func absent(x: int): int;\n",
                     &err));

    assert(gab_load(vm, "a.gab",
                    "module M;\n"
                    "func ready(): int { return 1; }\n",
                    &err));

    GabFunc *fn = gab_lookup(vm, "M", "ready", &err);
    assert(fn);

    GabCall *call = gab_call_init(fn, &err);
    assert(call);

    int result = 0;
    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 1);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_failed_load_leaves_what_is_loaded(void) {
    GabVM *vm = gab_vm_new();

    GabError err;

    assert(gab_load(vm, "s.gab", "module S;\nfunc step(n: int): int { return n + 1; }\n", &err));

    assert(!gab_load(vm, "t.gab", "module T;\nfunc broken(n: int): int { return n +", &err));
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

static void test_a_host_pointer_reaches_a_script() {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_load(vm, "u",
                    "module test;\n"
                    "struct Player { health: int }\n"
                    "func hurt(p: ref Player, amount: int): int {\n"
                    "  p.health = p.health - amount;\n"
                    "  return p.health;\n"
                    "}\n",
                    &err));

    const GabType *player = gab_find_type(vm, "test", "Player");
    assert(player);

    void *object = calloc(1, gab_type_size(vm, player));
    assert(object);

    size_t health_at = 0;
    assert(gab_field_offset(vm, player, "health", &health_at));

    int32_t health = 100;
    memcpy((char *)object + health_at, &health, sizeof(health));

    GabFunc *fn = gab_lookup(vm, "test", "hurt", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(gab_arg_pointer(call, 0, object, player));
    assert(gab_arg_int(call, 1, 40));

    int32_t left = 0;
    assert(gab_call(vm, call, &left, &err) == GAB_OK);
    assert(left == 60);

    memcpy(&health, (char *)object + health_at, sizeof(health));
    assert(health == 60);

    gab_call_free(call);
    free(object);
    gab_vm_free(vm);
}

static void test_a_pointer_argument_checks_its_pointee() {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_load(vm, "u",
                    "module test;\n"
                    "struct Player { health: int }\n"
                    "struct Enemy { health: int }\n"
                    "func hurt(p: ref Player): int { return p.health; }\n",
                    &err));

    const GabType *player = gab_find_type(vm, "test", "Player");
    const GabType *enemy = gab_find_type(vm, "test", "Enemy");

    void *object = calloc(1, gab_type_size(vm, player));

    GabFunc *fn = gab_lookup(vm, "test", "hurt", &err);
    GabCall *call = gab_call_init(fn, &err);

    assert(!gab_arg_pointer(call, 0, object, enemy));

    assert(gab_call(vm, call, NULL, &err) == GAB_ERR_ARG);

    assert(gab_arg_pointer(call, 0, object, player));

    gab_call_free(call);
    free(object);
    gab_vm_free(vm);
}

static void test_a_type_reports_the_layout_a_host_allocates_by() {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_load(vm, "u",
                    "module test;\n"
                    "struct Player { health: int, mana: int }\n",
                    &err));

    const GabType *player = gab_find_type(vm, "test", "Player");

    typedef struct {
        int32_t health, mana;
    } PlayerC;

    assert(gab_type_size(vm, player) == sizeof(PlayerC));
    assert(gab_type_align(vm, player) == _Alignof(PlayerC));

    size_t mana_at = 0;
    assert(gab_field_offset(vm, player, "mana", &mana_at));
    assert(mana_at == offsetof(PlayerC, mana));

    gab_vm_free(vm);
}

static void test_a_builtin_type_may_not_be_redeclared(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(!gab_load(vm, "a.gab", "module A;\nstruct int { x: float }\n", &err));
    assert(strstr(err.message, "int"));
    assert(err.line == 2);

    assert(!gab_load(vm, "b.gab", "module B;\nstruct bool { x: float }\n", &err));
    assert(strstr(err.message, "bool"));

    assert(gab_load(vm, "c.gab", "module C;\nstruct Config { a: int }\n", &err));
    assert(gab_load(vm, "d.gab", "module D;\nstruct Config { b: int }\n", &err));

    gab_vm_free(vm);
}

int main(void) {
    test_a_host_pointer_reaches_a_script();
    test_a_pointer_argument_checks_its_pointee();
    test_a_type_reports_the_layout_a_host_allocates_by();
    test_two_modules_can_share_a_function_name();
    test_the_script_names_its_module_not_the_filename();
    test_the_vm_owns_its_handles();
    test_lookup_by_module_name();
    test_two_modules_can_share_a_type_name();
    test_builtins_are_shared_across_modules();
    test_module_type_shadows_the_root();
    test_a_module_may_be_imported_twice();
    test_an_import_names_a_loaded_module();
    test_modules_cannot_import_each_other();
    test_a_qualified_name_needs_an_import();
    test_qualified_type_reference_crosses_modules();
    test_handle_survives_later_compiles();
    test_a_failed_load_declares_nothing();
    test_a_failed_load_leaves_what_is_loaded();
    test_a_method_is_not_reachable_from_a_host();
    test_a_name_may_only_be_declared_once();
    test_a_builtin_type_may_not_be_redeclared();
    test_compile_error_is_reported_not_printed();
    test_runtime_error_is_reported();
    test_runtime_error_in_a_top_level_fails_the_load();
    test_unset_arguments_are_rejected();
    test_rejected_setter_does_not_supply_an_argument();
    test_compile_once_run_many();
    test_type_survives_a_later_compile();
    test_layout_matches_c();
    test_lookup_failures();
    test_a_host_call_reaches_a_builtin_method();
    test_call_with_scalar_args();
    test_call_in_a_loop();
    test_struct_argument_and_return();
    test_two_callers_stage_independently();
    test_bad_arguments_are_rejected();

    printf("gab_api_test: all tests passed\n");

    return 0;
}
