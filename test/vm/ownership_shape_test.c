// Game-shaped script under the unique-ownership rule, written to find out how
// often a field has to hold something it does not own. The rule is that a
// strong field may only be given a value nothing else owns -- 'new', or a call
// handing its result over -- so every other field is a borrow, and a borrow
// stored in a field has to be spelled 'ref' today.
//
// The counts these tests establish are the point. Each is named for the shape
// it represents rather than for the assertion it makes, because what is being
// measured is how much of an ordinary object graph the strict rule can express.
//
// No arrays and no loops yet, so a collection is a linked list and iteration is
// recursion. That does not change what is being counted: the ownership edges
// are the same ones a vector of entities would have.

#include "support/run.h"

#include <assert.h>

// The spine of the graph: a world owns a list of players, each player owns its
// inventory. Every one of these edges is an owning edge, and every one is fed
// by 'new' or by a call -- so the strict rule expresses the whole tree with no
// borrow at all.
//
// This is the shape the rule is good at, and it is most of a game's data.
static void test_an_owning_tree_needs_no_borrows() {
    assert(test_run_int("struct Item { damage: int }\n"
                        "struct Player { weapon: *Item, health: int }\n"
                        "struct World { player: *Player }\n"
                        "func make_item(d: int): *Item {\n"
                        "    let i: *Item = new Item;\n"
                        "    i.damage = d;\n"
                        "    return i;\n"
                        "}\n"
                        "func make_player(): *Player {\n"
                        "    let p: *Player = new Player;\n"
                        "    p.health = 100;\n"
                        "    p.weapon = make_item(7);\n"
                        "    return p;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let w: *World = new World;\n"
                        "    w.player = make_player();\n"
                        "    return w.player.weapon.damage;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// A list of entities, which is what an array would be if there were arrays.
// 'next' is an owning edge like any other -- a node owns its tail -- so a
// collection costs no borrows either.
//
// Every 'new' here is bound to a slot that owns it. That is not incidental:
// see the leak below for what happens when one is not.
static void test_a_list_is_owned_end_to_end() {
    assert(test_run_int("struct Node { value: int, next: *Node }\n"
                        "func sum(n: ref Node): int {\n"
                        "    return n.value;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let head: *Node = new Node;\n"
                        "    head.value = 5;\n"
                        "    head.next = new Node;\n"
                        "    return sum(head);\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// An owned value passed straight into a parameter belongs to nobody once the
// call returns: a parameter borrows, so the callee does not free it, and the
// argument was never bound to a slot that would. The call site frees it, the
// same way a statement whose value is owned and unclaimed does.
//
// Correctness here is that it runs clean under LeakSanitizer; the returned
// value only proves the call happened.
static void test_an_owned_argument_is_freed_by_the_call_site() {
    assert(test_run_int("struct Node { value: int, next: *Node }\n"
                        "func take(tail: ref Node): int { return tail.value + 1; }\n"
                        "func main(): int { return take(new Node); }\n"
                        "let r: int = main();") == 1);
}

// The same, for a method call: the receiver is parameter zero, so an owned one
// arrives the same way and is nobody's afterwards either.
static void test_an_owned_receiver_is_freed_by_the_call_site() {
    assert(test_run_int("struct Node { value: int }\n"
                        "func (n: ref Node) get(): int { return n.value + 2; }\n"
                        "func main(): int { return (new Node).get(); }\n"
                        "let r: int = main();") == 2);
}

// A borrowed argument is left alone: its own slot still owns it, and freeing at
// the call site would free it out from under the variable that goes on naming
// it. This is the case the fix must not break.
static void test_a_borrowed_argument_is_not_freed_by_the_call_site() {
    assert(test_run_int("struct Node { value: int }\n"
                        "func take(n: ref Node): int { return n.value; }\n"
                        "func main(): int {\n"
                        "    let n: *Node = new Node;\n"
                        "    n.value = 5;\n"
                        "    take(n);\n"
                        "    return n.value;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// Reading through the graph without storing anything is a borrow, and borrows
// that stay in slots are free: a parameter is not in the frame's ref list, so
// passing a pointer down costs no retain and no release.
//
// This is the other thing the rule is good at. Behaviour -- the code that walks
// the graph and acts on it -- is all borrows, and none of them are stored.
static void test_walking_the_graph_costs_nothing() {
    assert(test_run_int("struct Item { damage: int }\n"
                        "struct Player { weapon: *Item, health: int }\n"
                        "func hit(p: ref Player, i: ref Item): int {\n"
                        "    p.health = p.health - i.damage;\n"
                        "    return p.health;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let p: *Player = new Player;\n"
                        "    p.health = 100;\n"
                        "    p.weapon = new Item;\n"
                        "    p.weapon.damage = 30;\n"
                        "    return hit(p, p.weapon);\n"
                        "}\n"
                        "let r: int = main();") == 70);
}

// The first shape that does not fit. A player pointing back at the world that
// owns it is a borrow stored in a field, and the strict rule refuses it: the
// world is owned by main's slot, so storing it in 'p.world' would make two
// owners.
//
// The ordinary shape of game data — a child knowing its parent — and exactly
// what a stored borrow is for.
static void test_a_back_pointer_is_refused() {
    assert(!test_codegens("struct World { tick: int }\n"
                          "struct Player { world: *World }\n"
                          "func main(): int {\n"
                          "    let w: *World = new World;\n"
                          "    let p: *Player = new Player;\n"
                          "    p.world = w;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = main();"));
}

// The same shape spelled 'ref' is accepted, and costs nothing: a borrow is a
// plain address, stored and read like any other pointer.
static void test_a_back_pointer_spelled_ref_is_accepted() {
    assert(test_run_int("struct World { tick: int }\n"
                        "struct Player { world: ref World }\n"
                        "func main(): int {\n"
                        "    let w: *World = new World;\n"
                        "    w.tick = 3;\n"
                        "    let p: *Player = new Player;\n"
                        "    p.world = w;\n"
                        "    return p.world.tick;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

// A second non-owning edge, and the one that makes the count matter: an entity
// referring to another entity it does not own. A weapon's owner, a projectile's
// shooter, an AI's target -- none of these own what they point at, and every
// one has to be 'ref'.
static void test_a_sibling_reference_is_refused() {
    assert(!test_codegens("struct Player { health: int }\n"
                          "struct Projectile { shooter: *Player, damage: int }\n"
                          "func fire(shooter: ref Player): *Projectile {\n"
                          "    let bullet: *Projectile = new Projectile;\n"
                          "    bullet.shooter = shooter;\n"
                          "    return bullet;\n"
                          "}\n"
                          "let r: int = 0;"));
}

// And the same edge as 'ref'. Note this is a *parameter* being stored, which is
// the case escape analysis would have to reason about: 'fire' stores a borrow
// its caller owns into an object that outlives the call.
static void test_a_sibling_reference_spelled_ref_is_accepted() {
    assert(test_run_int("struct Player { health: int }\n"
                        "struct Projectile { shooter: ref Player, damage: int }\n"
                        "func fire(shooter: ref Player): *Projectile {\n"
                        "    let bullet: *Projectile = new Projectile;\n"
                        "    bullet.shooter = shooter;\n"
                        "    bullet.damage = 9;\n"
                        "    return bullet;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let p: *Player = new Player;\n"
                        "    let b: *Projectile = fire(p);\n"
                        "    return b.shooter.health + b.damage;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

int main(void) {
    test_an_owning_tree_needs_no_borrows();
    test_a_list_is_owned_end_to_end();
    test_an_owned_argument_is_freed_by_the_call_site();
    test_an_owned_receiver_is_freed_by_the_call_site();
    test_a_borrowed_argument_is_not_freed_by_the_call_site();
    test_walking_the_graph_costs_nothing();
    test_a_back_pointer_is_refused();
    test_a_back_pointer_spelled_ref_is_accepted();
    test_a_sibling_reference_is_refused();
    test_a_sibling_reference_spelled_ref_is_accepted();

    return 0;
}
