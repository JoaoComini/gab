#include "vm/vm.h"

#include <assert.h>

/*
    'new' allocates and nothing releases yet: release-on-frame-pop is the next
    commit, and until it lands every object these tests allocate is a leak by
    construction rather than by mistake.

    LeakSanitizer is turned off for this file only, and only for that window —
    the other ASan checks stay on, which is the point, since use-after-free and
    corruption are what matter while the release path is being built. Delete
    this the moment frames release what they own; if the suite then still leaks,
    that is a real bug this would otherwise have hidden.
*/
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GAB_HEAP_TEST_LEAKS_EXPECTED 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define GAB_HEAP_TEST_LEAKS_EXPECTED 1
#endif

#ifdef GAB_HEAP_TEST_LEAKS_EXPECTED
const char *__lsan_default_options(void);
const char *__lsan_default_options(void) { return "detect_leaks=0"; }
#endif
#include <stdio.h>

// Runs a script and returns whatever ended up in r0, which is where a
// top-level return leaves its result.
static int run_int(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    int result = (*vm_slot(vm, 0)).as_int;

    vm_free(vm);

    return result;
}

// 'new T' yields a '*T' into the heap: writing through it and reading back is
// what proves the allocation is real and the pointer addresses the payload.
static void test_new_allocates_a_usable_object() {
    assert(run_int("struct Player { health: int }\n"
                   "func main(): int {\n"
                   "    let p: *Player = new Player;\n"
                   "    p.health = 42;\n"
                   "    return p.health;\n"
                   "}\n"
                   "let r: int = main();") == 42);
}

// A fresh object is zeroed, so a field nobody assigned reads as 0 rather than
// whatever the allocator left behind. Release depends on this for pointers.
static void test_new_zeroes_the_payload() {
    assert(run_int("struct Player { health: int, mana: int }\n"
                   "func main(): int {\n"
                   "    let p: *Player = new Player;\n"
                   "    return p.health + p.mana;\n"
                   "}\n"
                   "let r: int = main();") == 0);
}

// A heap pointer outlives every frame, so returning one is allowed where
// returning '&local' is not.
static void test_a_heap_pointer_may_be_returned() {
    assert(run_int("struct Box { n: int }\n"
                   "func make(): *Box {\n"
                   "    let b: *Box = new Box;\n"
                   "    b.n = 7;\n"
                   "    return b;\n"
                   "}\n"
                   "func main(): int { let b: *Box = make(); return b.n; }\n"
                   "let r: int = main();") == 7);
}

// Two allocations of the same type share one entry in the VM's type list, and
// two objects that do not alias.
static void test_two_objects_are_distinct() {
    assert(run_int("struct Box { n: int }\n"
                   "func main(): int {\n"
                   "    let a: *Box = new Box;\n"
                   "    let b: *Box = new Box;\n"
                   "    a.n = 1;\n"
                   "    b.n = 2;\n"
                   "    return a.n * 10 + b.n;\n"
                   "}\n"
                   "let r: int = main();") == 12);
}

// A method on a pointer receiver is called through a heap pointer with no
// address-taking, since the receiver already is one.
static void test_method_on_a_heap_object() {
    assert(run_int("struct Player { health: int }\n"
                   "func (p: *Player) hurt(n: int): int { p.health = p.health - n; return p.health; }\n"
                   "func main(): int {\n"
                   "    let p: *Player = new Player;\n"
                   "    p.health = 50;\n"
                   "    return p.hurt(8);\n"
                   "}\n"
                   "let r: int = main();") == 42);
}

// A heap object holding a pointer to another: the field is a '*T' like any
// other, which is what release will later walk.
static void test_an_object_can_hold_another() {
    assert(run_int("struct Inner { n: int }\n"
                   "struct Outer { child: *Inner }\n"
                   "func main(): int {\n"
                   "    let o: *Outer = new Outer;\n"
                   "    o.child = new Inner;\n"
                   "    o.child.n = 9;\n"
                   "    return o.child.n;\n"
                   "}\n"
                   "let r: int = main();") == 9);
}

int main(void) {
    test_new_allocates_a_usable_object();
    test_new_zeroes_the_payload();
    test_a_heap_pointer_may_be_returned();
    test_two_objects_are_distinct();
    test_method_on_a_heap_object();
    test_an_object_can_hold_another();

    printf("All heap tests passed\n");
    return 0;
}
