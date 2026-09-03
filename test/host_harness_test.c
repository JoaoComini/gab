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

static void host_tick(GabCtx *ctx) { gab_ctx_return_int(ctx, world.tick); }

static void host_spawn(GabCtx *ctx) {
    int32_t count = gab_ctx_int(ctx, 0);

    world.spawned += count;

    gab_ctx_return_int(ctx, world.spawned);
}

static void host_buff(GabCtx *ctx) {
    Player p;
    gab_ctx_struct(ctx, 0, &p, sizeof p);

    p.health += gab_ctx_int(ctx, 1);

    gab_ctx_return_struct(ctx, &p, sizeof p);
}

static void host_log(GabCtx *ctx) {
    int32_t length = 0;
    const char *text = gab_ctx_string(ctx, 0, &length);

    assert(text != NULL);
    world.logged += length;
}

static void host_refuse(GabCtx *ctx) { gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, "refused by the host"); }

static GabVM *reentrant_vm;

static void host_reenter(GabCtx *ctx) {
    GabError err;
    GabFunc *doubled = gab_vm_lookup(reentrant_vm, "game", "doubled", &err);
    assert(doubled != NULL);

    GabCall *call = gab_call_init(doubled, &err);
    assert(gab_call_int(call, 0, 21));

    int32_t out = 0;
    assert(gab_call(reentrant_vm, call, &out, &err) == GAB_OK);

    gab_call_free(call);

    gab_ctx_return_int(ctx, out);
}

static const char *const SOURCE = "module game;\n"
                                  "struct Player { health: int, mana: int }\n"
                                  "extern func tick(): int;\n"
                                  "extern func spawn(count: int): int;\n"
                                  "extern func buff(p: Player, amount: int): Player;\n"
                                  "extern func log(text: &str);\n"
                                  "extern func refuse(): int;\n"
                                  "func update(p: Player): int {\n"
                                  "    log(\"tick\");\n"
                                  "    let t: int = tick();\n"
                                  "    let b: Player = buff(p, t);\n"
                                  "    return b.health + spawn(1);\n"
                                  "}\n"
                                  "func fails(): int { return refuse(); }\n"
                                  "extern func reenter(): int;\n"
                                  "func doubled(n: int): int { return n * 2; }\n"
                                  "func through_host(): int { return reenter() + 1; }\n";

static GabVM *harness_vm(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    assert(gab_extern(vm, "game", NULL, "tick", host_tick, &err));
    assert(gab_extern(vm, "game", NULL, "spawn", host_spawn, &err));
    assert(gab_extern(vm, "game", NULL, "buff", host_buff, &err));
    assert(gab_extern(vm, "game", NULL, "log", host_log, &err));
    assert(gab_extern(vm, "game", NULL, "refuse", host_refuse, &err));
    assert(gab_extern(vm, "game", NULL, "reenter", host_reenter, &err));

    if (!gab_vm_load(vm, "game.gab", SOURCE, &err)) {
        fprintf(stderr, "load failed: line %d: %s\n", err.line, err.message);
        assert(0);
    }

    return vm;
}

static void test_script_layout_matches_the_host_struct(void) {
    GabVM *vm = harness_vm();

    const GabType *type = gab_vm_find_type(vm, "game", "Player");
    assert(type != NULL);
    assert(gab_type_size(vm, type) == sizeof(Player));
    assert(gab_type_align(vm, type) == _Alignof(Player));

    size_t offset = 0;
    assert(gab_type_field_offset(vm, type, "health", &offset) && offset == offsetof(Player, health));
    assert(gab_type_field_offset(vm, type, "mana", &offset) && offset == offsetof(Player, mana));

    gab_vm_free(vm);
}

static void test_a_frame_loop_calls_script_which_calls_back(void) {
    GabVM *vm = harness_vm();
    GabError err;

    world = (World){0};

    GabFunc *update = gab_vm_lookup(vm, "game", "update", &err);
    assert(update != NULL);
    assert(gab_func_arity(update) == 1);

    GabCall *call = gab_call_init(update, &err);
    assert(call != NULL);

    Player p = {.health = 100, .mana = 50};

    for (int32_t frame = 0; frame < 4; frame++) {
        world.tick = frame;

        assert(gab_call_struct(call, 0, &p, sizeof p));

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

    GabFunc *fails = gab_vm_lookup(vm, "game", "fails", &err);
    assert(fails != NULL);

    GabCall *call = gab_call_init(fails, &err);
    int32_t result = 0;

    assert(gab_call(vm, call, &result, &err) != GAB_OK);
    assert(strstr(err.message, "refused") != NULL);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void host_reenter_failing(GabCtx *ctx) {
    GabError err;
    GabFunc *fails = gab_vm_lookup(reentrant_vm, "game", "fails", &err);
    assert(fails != NULL);

    GabCall *call = gab_call_init(fails, &err);
    int32_t out = 0;

    assert(gab_call(reentrant_vm, call, &out, &err) != GAB_OK);

    gab_call_free(call);

    gab_ctx_return_int(ctx, 5);
}

static void test_a_nested_failure_leaves_its_caller_running(void) {
    GabVM *vm = gab_vm_new();
    GabError err;

    reentrant_vm = vm;

    assert(gab_extern(vm, "game", NULL, "tick", host_tick, &err));
    assert(gab_extern(vm, "game", NULL, "spawn", host_spawn, &err));
    assert(gab_extern(vm, "game", NULL, "buff", host_buff, &err));
    assert(gab_extern(vm, "game", NULL, "log", host_log, &err));
    assert(gab_extern(vm, "game", NULL, "refuse", host_refuse, &err));
    assert(gab_extern(vm, "game", NULL, "reenter", host_reenter_failing, &err));
    assert(gab_vm_load(vm, "game.gab", SOURCE, &err));

    GabFunc *through = gab_vm_lookup(vm, "game", "through_host", &err);
    assert(through != NULL);

    GabCall *call = gab_call_init(through, &err);
    int32_t result = 0;

    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 6);

    gab_call_free(call);
    gab_vm_free(vm);
}

static void test_a_host_body_calls_back_into_the_vm(void) {
    GabVM *vm = harness_vm();
    GabError err;

    reentrant_vm = vm;

    GabFunc *through = gab_vm_lookup(vm, "game", "through_host", &err);
    assert(through != NULL);

    GabCall *call = gab_call_init(through, &err);
    int32_t result = 0;

    assert(gab_call(vm, call, &result, &err) == GAB_OK);
    assert(result == 43);

    gab_call_free(call);
    gab_vm_free(vm);
}

int main(void) {
    test_script_layout_matches_the_host_struct();
    test_a_frame_loop_calls_script_which_calls_back();
    test_an_extern_can_fail_the_run();
    test_a_host_body_calls_back_into_the_vm();
    test_a_nested_failure_leaves_its_caller_running();

    return 0;
}
