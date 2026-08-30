#include "compile.h"
#include "diagnostics.h"
#include "support/run.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <assert.h>

#include <stdio.h>

static void test_new_allocates_a_usable_object() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: box Player = new Player;\n"
                        "    p.health = 42;\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

static void test_new_zeroes_the_payload() {
    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func main(): int {\n"
                        "    let p: box Player = new Player;\n"
                        "    return p.health + p.mana;\n"
                        "}\n"
                        "let r: int = main();") == 0);
}

static void test_a_heap_pointer_may_be_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): box Box {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 7;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: box Box = make(); return b.n; }\n"
                        "let r: int = main();") == 7);
}

static void test_two_objects_are_distinct() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    let b: box Box = new Box;\n"
                        "    a.n = 1;\n"
                        "    b.n = 2;\n"
                        "    return a.n * 10 + b.n;\n"
                        "}\n"
                        "let r: int = main();") == 12);
}

static void test_method_on_a_heap_object() {
    assert(test_run_int(
               "struct Player { health: int }\n"
               "func Player::hurt(p: ref Player, n: int): int { p.health = p.health - n; return p.health; }\n"
               "func main(): int {\n"
               "    let p: box Player = new Player;\n"
               "    p.health = 50;\n"
               "    return p.hurt(8);\n"
               "}\n"
               "let r: int = main();") == 42);
}

static void test_an_object_can_hold_another() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 9;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_an_alias_is_borrowed_not_owned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    a.n = 5;\n"
                        "    let b: ref Box = a;\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);

    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    let b: box Box = a;\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_released_at_the_end_of_its_block() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let t: int = 0;\n"
                        "    { let a: box Box = new Box; a.n = 3; t = a.n; }\n"
                        "    return t;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_a_reused_slot_is_not_released_twice() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    { let a: box Box = new Box; a.n = 1; }\n"
                        "    { let x: int = 7; let y: int = 8; return x + y; }\n"
                        "}\n"
                        "let r: int = main();") == 15);
}

static void test_a_bare_new_does_not_leak() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int { new Box; return 4; }\n"
                        "let r: int = main();") == 4);
}

static void test_an_abnormal_unwind_frees_what_it_held() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    assert(compile_unit(vm,
                        "module test;\n"
                        "struct Node { n: int }\n"
                        "func deep(n: int): int { return deep(n + 1); }\n"
                        "func main(): int { let p: box Node = new Node; return deep(0); }\n"
                        "let r: int = main();",
                        &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(interp_run_top_level(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    func_proto_free(&script);
    vm_free(vm);
}

static void test_storing_an_owning_value_into_a_field_transfers_it() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    let i: box Inner = new Inner;\n"
                        "    i.n = 6;\n"
                        "    o.child = i;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);

    assert(!test_compiles("struct Inner { n: int }\n"
                          "struct Outer { child: box Inner }\n"
                          "func main(): int {\n"
                          "    let o: box Outer = new Outer;\n"
                          "    let i: box Inner = new Inner;\n"
                          "    o.child = i;\n"
                          "    return i.n;\n"
                          "}\n"));
}

static void test_storing_a_parameter_into_a_field_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: box Inner }\n"
                          "func adopt(o: ref Outer, i: ref Inner): int {\n"
                          "    o.child = i;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = 0;"));
}

static void test_storing_a_call_result_into_a_field_is_allowed() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func make(): box Inner {\n"
                        "    let i: box Inner = new Inner;\n"
                        "    i.n = 6;\n"
                        "    return i;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    o.child = make();\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_an_owned_argument_is_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func take(b: ref Box): int { return b.n + 1; }\n"
                        "func main(): int { return take(new Box); }\n"
                        "let r: int = main();") == 1);
}

static void test_an_owned_receiver_is_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::get(b: ref Box): int { return b.n + 2; }\n"
                        "func main(): int { return (new Box).get(); }\n"
                        "let r: int = main();") == 2);
}

static void test_a_borrowed_argument_is_not_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func take(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 5;\n"
                        "    take(b);\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

static void test_overwriting_a_field_releases_the_old_value() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 1;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 2;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

static void test_reassigning_a_variable_releases_the_old_value() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let p: box Box = new Box;\n"
                        "    p.n = 1;\n"
                        "    p = new Box;\n"
                        "    p.n = 9;\n"
                        "    return p.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_field_self_assignment_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: box Inner }\n"
                          "func main(): int {\n"
                          "    let o: box Outer = new Outer;\n"
                          "    o.child = new Inner;\n"
                          "    o.child.n = 5;\n"
                          "    o.child = o.child;\n"
                          "    return o.child.n;\n"
                          "}\n"
                          "let r: int = main();"));
}

static void test_variable_self_assignment_is_refused() {
    assert(!test_codegens("struct Box { n: int }\n"
                          "func main(): int { let p: box Box = new Box; p.n = 3; p = p; return p.n; }\n"
                          "let r: int = main();"));
}

static void test_reassigning_an_owning_variable_frees_the_old_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let p: box Box = new Box;\n"
                        "    p.n = 1;\n"
                        "    p = new Box;\n"
                        "    p.n = 9;\n"
                        "    return p.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

int main(void) {
    test_new_allocates_a_usable_object();
    test_overwriting_a_field_releases_the_old_value();
    test_reassigning_a_variable_releases_the_old_value();
    test_field_self_assignment_is_refused();
    test_variable_self_assignment_is_refused();
    test_reassigning_an_owning_variable_frees_the_old_object();
    test_storing_an_owning_value_into_a_field_transfers_it();
    test_storing_a_parameter_into_a_field_is_refused();
    test_storing_a_call_result_into_a_field_is_allowed();
    test_an_owned_argument_is_freed_by_the_call_site();
    test_an_owned_receiver_is_freed_by_the_call_site();
    test_a_borrowed_argument_is_not_freed_by_the_call_site();
    test_an_alias_is_borrowed_not_owned();
    test_released_at_the_end_of_its_block();
    test_a_reused_slot_is_not_released_twice();
    test_a_bare_new_does_not_leak();
    test_an_abnormal_unwind_frees_what_it_held();
    test_new_zeroes_the_payload();
    test_a_heap_pointer_may_be_returned();
    test_two_objects_are_distinct();
    test_method_on_a_heap_object();
    test_an_object_can_hold_another();

    printf("All heap tests passed\n");
    return 0;
}
