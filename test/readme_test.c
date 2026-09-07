#include "support/run.h"

static void the_language_sample_compiles(void) {
    assert(test_compiles("struct World { tick: i32 }\n"
                         "struct Player { health: i32, world: &World }\n"
                         "impl Player {\n"
                         "    func is_alive(self: &Self): bool { return self.health > 0; }\n"
                         "}\n"
                         "func heal(p: &Player, amount: i32) { p.health = p.health + amount; }\n"));
}

static void the_interface_sample_compiles(void) {
    assert(test_compiles("interface Countable {\n"
                         "    func count(self: &Self): i32;\n"
                         "}\n"
                         "struct Bag { n: i32 }\n"
                         "impl Bag as Countable {\n"
                         "    func count(self: &Self): i32 { return self.n; }\n"
                         "}\n"
                         "func total<T: Countable>(x: &T): i32 { return x.count(); }\n"));
}

static void the_ownership_sample_compiles(void) {
    assert(test_compiles("struct Box { n: i32 }\n"
                         "func consume(b: *Box): i32 { return b.n; }\n"
                         "func peek(b: &Box): i32 { return b.n; }\n"
                         "func f(): i32 {\n"
                         "    let a: *Box = box Box { n: 1 };\n"
                         "    peek(a);\n"
                         "    let c: *Box = a;\n"
                         "    return consume(c);\n"
                         "}\n"));
}

static void the_loop_shapes_compile(void) {
    assert(test_compiles("func f(n: i32): i32 {\n"
                         "    for { break; }\n"
                         "    for n > 0 { break; }\n"
                         "    for let i: i32 = 0; i < n; i = i + 1 { }\n"
                         "    return 0;\n"
                         "}\n"));
}

int main(void) {
    the_language_sample_compiles();
    the_interface_sample_compiles();
    the_ownership_sample_compiles();
    the_loop_shapes_compile();

    return 0;
}
