#include "gab.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t health;
    int32_t mana;
} Player;

typedef struct {
    int32_t tick;
    int32_t spawned;
    int32_t logged;
} World;

static World world;

static void host_tick(GabArgs *args) { gab_return_int(args, world.tick); }

static void host_spawn(GabArgs *args) {
    int32_t count = gab_arg_get_int(args, 0);

    world.spawned += count;

    gab_return_int(args, world.spawned);
}

static void host_buff(GabArgs *args) {
    Player p;
    gab_arg_get_struct(args, 0, &p, sizeof p);

    p.health += gab_arg_get_int(args, 1);

    gab_return_struct(args, &p, sizeof p);
}

static void host_log(GabArgs *args) {
    int32_t length = 0;
    const char *text = gab_arg_get_string(args, 0, &length);

    assert(text != NULL);
    world.logged += length;
}

static void host_refuse(GabArgs *args) { gab_error(args, "refused by the host"); }

static const char *const SOURCE = "module game;\n"
                                  "struct Player { health: int, mana: int }\n"
                                  "extern func tick(): int;\n"
                                  "extern func spawn(count: int): int;\n"
                                  "extern func buff(p: Player, amount: int): Player;\n"
                                  "extern func log(text: ref str);\n"
                                  "extern func refuse(): int;\n"
                                  "func update(p: Player): int {\n"
                                  "    log(\"tick\");\n"
                                  "    let t: int = tick();\n"
                                  "    let b: Player = buff(p, t);\n"
                                  "    return b.health + spawn(1);\n"
                                  "}\n"
                                  "func fails(): int { return refuse(); }\n";

static GabVM *harness_vm(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "game", "tick", host_tick, &err));
    assert(gab_extern(vm, "game", "spawn", host_spawn, &err));
    assert(gab_extern(vm, "game", "buff", host_buff, &err));
    assert(gab_extern(vm, "game", "log", host_log, &err));
    assert(gab_extern(vm, "game", "refuse", host_refuse, &err));

    if (!gab_load(vm, "game.gab", SOURCE, &err)) {
        fprintf(stderr, "load failed: line %d: %s\n", err.line, err.message);
        assert(0);
    }

    return vm;
}

static void test_script_layout_matches_the_host_struct(void) {
    GabVM *vm = harness_vm();

    const GabType *type = gab_find_type(vm, "game", "Player");
    assert(type != NULL);
    assert(gab_type_size(vm, type) == sizeof(Player));
    assert(gab_type_align(vm, type) == _Alignof(Player));

    size_t offset = 0;
    assert(gab_field_offset(vm, type, "health", &offset) && offset == offsetof(Player, health));
    assert(gab_field_offset(vm, type, "mana", &offset) && offset == offsetof(Player, mana));

    gab_vm_free(vm);
}

static void test_a_frame_loop_calls_script_which_calls_back(void) {
    GabVM *vm = harness_vm();
    GabError err;

    world = (World){0};

    GabFunc *update = gab_lookup(vm, "game", "update", &err);
    assert(update != NULL);
    assert(gab_func_arity(update) == 1);

    GabCall *call = gab_call_init(update, &err);
    assert(call != NULL);

    Player p = {.health = 100, .mana = 50};

    for (int32_t frame = 0; frame < 4; frame++) {
        world.tick = frame;

        assert(gab_arg_struct(call, 0, &p, sizeof p));

        int32_t result = 0;
        assert(gab_call(vm, call, &result, &err) == GAB_OK);

        assert(result == 100 + frame + (frame + 1));
    }

    assert(world.spawned == 4);
    assert(world.logged == 4 * 4);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_an_extern_can_fail_the_run(void) {
    GabVM *vm = harness_vm();
    GabError err;

    GabFunc *fails = gab_lookup(vm, "game", "fails", &err);
    assert(fails != NULL);

    GabCall *call = gab_call_init(fails, &err);
    int32_t result = 0;

    assert(gab_call(vm, call, &result, &err) != GAB_OK);
    assert(strstr(err.message, "refused") != NULL);

    gab_call_free(call);
    gab_vm_free(vm);
}

int main(void) {
    test_script_layout_matches_the_host_struct();
    test_a_frame_loop_calls_script_which_calls_back();
    test_an_extern_can_fail_the_run();

    return 0;
}
