#include "ast/ast.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// 'ref T' and '*T' are different types, so that freeing an object can tell from
// a field's type alone whether it owns what the field names.
static void test_ref_is_a_distinct_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = test_resolve(&ctx, scope, script,
                           "struct Node { n: int }\n"
                           "let o: *Node;\n"
                           "let b: ref Node;\n");
    assert(ok);

    Symbol *owning = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "o"));
    Symbol *borrow = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(owning && borrow);
    assert(owning->var.type != borrow->var.type);
    assert(!owning->var.type->is_ref);
    assert(borrow->var.type->is_ref);

    // Same pointee, and both are still ordinary pointers: a borrow is the same
    // address, differing only in who frees the pointee.
    assert(owning->var.type->pointee == borrow->var.type->pointee);
    assert(borrow->var.type->size == sizeof(void *));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// Interned like every other type, so two mentions of 'ref Node' are one Type.
static void test_ref_pointers_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = test_resolve(&ctx, scope, script,
                           "struct Node { n: int }\n"
                           "let a: ref Node;\n"
                           "let b: ref Node;\n");
    assert(ok);

    Symbol *a = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "a"));
    Symbol *b = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(a->var.type == b->var.type);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// The shape a scene graph is made of, and the reason 'ref' exists: a child
// knows its parent without owning it.
static void test_a_self_referential_struct_declares() {
    assert(test_compiles("struct Node { parent: ref Node, child: *Node }\n"));

    // A pointer to self is not containment, borrowed or owned.
    assert(test_compiles("struct Node { child: *Node }\n"));

    // Containing itself by value still is.
    assert(!test_compiles("struct Node { self: Node }\n"));
}

// An owned pointer may be stored where a borrow is expected: giving something
// up to be named costs nothing. The reverse would hand out ownership nobody
// granted, so it is refused.
static void test_conversion_is_owned_to_ref_only() {
    assert(test_compiles("struct Node { n: int }\n"
                         "func f(): int { let o: *Node = new Node; let b: ref Node = o; return 0; }\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "func f(): int { let b: ref Node; let o: *Node = b; return 0; }\n"));
}

// 'ref' stands in place of the '*', so combining the two has nothing to mean:
// the flag qualifies one pointer, and there would be two.
static void test_ref_does_not_combine_with_a_star() {
    assert(!test_compiles("struct Node { n: int }\nlet x: ref *Node;\n"));
}

// A 'ref' field is still a field: it reads and writes like any other pointer,
// since the address is identical and only ownership differs.
static void test_a_ref_field_reads_and_writes() {
    assert(test_run_int("struct Node { n: int, parent: ref Node }\n"
                        "func main(): int {\n"
                        "    let a: *Node = new Node;\n"
                        "    let b: *Node = new Node;\n"
                        "    a.n = 7;\n"
                        "    b.parent = a;\n"
                        "    return b.parent.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// The shape a scene graph actually is: a child names its parent without owning
// it, and both ends free. With two owning edges this would double-free.
//
// Correctness here is that it runs clean under LeakSanitizer and
// AddressSanitizer, which is what the suite is built with — the returned value
// only proves the graph was walkable.
static void test_a_parent_child_graph_frees_once() {
    assert(test_run_int("struct Node { n: int, parent: ref Node, child: *Node }\n"
                        "func main(): int {\n"
                        "    let root: *Node = new Node;\n"
                        "    root.n = 1;\n"
                        "    root.child = new Node;\n"
                        "    root.child.n = 2;\n"
                        "    root.child.parent = root;\n"
                        "    return root.child.parent.n;\n"
                        "}\n"
                        "let r: int = main();") == 1);
}

// A failure unwinds past every free codegen emitted, so the frames drop what
// they own on the way out.
static void test_an_abnormal_unwind_frees_what_it_held() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    assert(vm_compile(vm,
                      "struct Node { n: int }\n"
                      "func deep(n: int): int { return deep(n + 1); }\n"
                      "func main(): int { let p: *Node = new Node; return deep(0); }\n"
                      "let r: int = main();",
                      &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(vm_run(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    vm_compiled_script_free(&script);
    vm_free(vm);
}

// The sample in the README. Kept compiling here so the documentation cannot
// drift from the language.
static void test_the_readme_sample_compiles() {
    assert(test_compiles("module game;\n"
                         "struct World { tick: int }\n"
                         "struct Player { health: int, mana: int, world: ref World }\n"
                         "func heal(p: ref Player, amount: int) {\n"
                         "    p.health = p.health + amount;\n"
                         "}\n"
                         "func (p: ref Player) is_alive(): bool {\n"
                         "    return p.health > 0;\n"
                         "}\n"));
}

// A method called on a 'ref T'. The receiver is a borrow, and a method that
// only reads through it neither owns nor frees it, so the call is ordinary.
static void test_a_method_on_a_ref_receiver() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) get(): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 6;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    return borrowed.get();\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// Writing through a 'ref T' receiver reaches the same object its owner names:
// a borrow is the same address, so the mutation is visible to the owner.
static void test_a_method_through_a_ref_mutates_the_owned_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) bump(): int { b.n = b.n + 1; return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 4;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    borrowed.bump();\n"
                        "    return owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// A 'ref T' argument where the parameter declares one: a borrow passed on as a
// borrow, which is what most behaviour code does.
static void test_a_ref_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 8;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    return peek(borrowed);\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

// An owned '*T' where a 'ref T' parameter is declared: giving something up to
// be named costs nothing, and the caller goes on owning it.
static void test_an_owned_pointer_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 3;\n"
                        "    return peek(owner);\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

// A parameter is 'ref T' or it is not a pointer, so every argument is lent and
// none is handed over: which pointer may fill which parameter is settled where
// the signature is written rather than at the call.
//
// Where the one-way conversion still has something to say is a variable: a
// borrow may not fill an owning one.
static void test_a_ref_does_not_fill_an_owning_variable() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func take(b: ref Box): int {\n"
                          "    let owned: *Box = b;\n"
                          "    return owned.n;\n"
                          "}\n"));
}

// A call returning 'ref T' hands back a borrow, so nothing at the call site
// frees it and the object stays its owner's.
static void test_a_call_returning_a_ref_is_not_freed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: ref Box): ref Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 2;\n"
                        "    let got: ref Box = borrow(owner);\n"
                        "    return got.n + owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

// Returning a borrow is safe exactly when it names something that outlives the
// call, which is C++'s rule for returning a reference: a member or something
// the caller passed in, never a local.
//
// A parameter is the safe case, covered above. This is the unsafe one, and the
// lifetime check already refuses it — a 'ref' to a local would name a frame
// slot the caller reuses the moment it returns.
static void test_a_ref_to_a_local_cannot_be_returned() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func bad(): ref Box {\n"
                          "    let local: Box;\n"
                          "    return &local;\n"
                          "}\n"));
}

// A borrow handed back by a call is only as long-lived as what it was made
// from. Nothing says which argument it borrows without a per-function summary,
// so the result is treated as borrowing from the shortest-lived of them --
// which is what makes this checkable at all.
//
// Here the borrow outlives its pointee: 'inner' dies at the brace and 'escaped'
// is declared outside it.
static void test_a_ref_returned_from_a_call_cannot_outlive_its_argument() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: ref Box): ref Box { return b; }\n"
                          "func main(): int {\n"
                          "    let escaped: ref Box;\n"
                          "    { let inner: Box; escaped = borrow(&inner); }\n"
                          "    return escaped.n;\n"
                          "}\n"));
}

// The same shape at a declaration rather than an assignment, since both record
// what a variable now points at.
static void test_a_ref_declared_from_a_call_carries_the_argument_lifetime() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: ref Box): ref Box { return b; }\n"
                          "func leak(): ref Box {\n"
                          "    let local: Box;\n"
                          "    let got: ref Box = borrow(&local);\n"
                          "    return got;\n"
                          "}\n"));
}

// A borrow of something that outlives the call is fine, which is the useful
// half: a heap object outlives every frame, so the result may go anywhere.
static void test_a_ref_borrowed_from_a_heap_object_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: ref Box): ref Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 5;\n"
                        "    let got: ref Box = borrow(owner);\n"
                        "    return got.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// An owned '*T' returned by a call is a heap object whatever it was made from,
// so it outlives every frame and the argument's lifetime does not follow it.
static void test_an_owned_return_does_not_inherit_argument_lifetimes() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(seed: ref Box): *Box {\n"
                        "    let fresh: *Box = new Box;\n"
                        "    fresh.n = seed.n + 1;\n"
                        "    return fresh;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let out: *Box = new Box;\n"
                        "    { let tmp: *Box = new Box; tmp.n = 1; out = make(tmp); }\n"
                        "    return out.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

// A method may declare a 'ref T' receiver, meaning it only borrows. An owned
// '*T' satisfies that: lending is what the method asked for.
static void test_an_owned_receiver_calls_a_ref_method() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) peek(): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 7;\n"
                        "    return owner.peek();\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// A borrowed receiver reaches the same object its owner names, so mutation
// through one is visible to the owner.
static void test_a_ref_receiver_mutates_the_owned_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) bump(): int { b.n = b.n + 1; return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = new Box;\n"
                        "    owner.n = 1;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    borrowed.bump();\n"
                        "    return owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

// A method never owns its receiver, so declaring one '*T' spells an ownership
// it cannot have -- and would let the same method be written two ways that
// behave identically. 'ref T' is the one form, and 'T' by value the other.
static void test_an_owning_receiver_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func (b: *Box) peek(): int { return b.n; }\n"));
}

// A parameter never owns what it is given either: the caller keeps owning it
// across the call, no callee frees one, and none may be stored where something
// else would own it. So '*T' is refused there for the same reason.
static void test_an_owning_parameter_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func take(b: *Box): int { return b.n; }\n"));
}

// The same for a method's own parameters, which are parameters like any other.
static void test_an_owning_method_parameter_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func (b: ref Box) adopt(other: *Box): int { return other.n; }\n"));
}

// What the refusal prevents: returning a parameter as owned would hand the
// caller a second owner of something it already owns, and both would free it.
// With no owning parameter to return, there is nothing to launder.
static void test_a_borrow_cannot_be_laundered_into_an_owned_return() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func launder(b: ref Box): *Box { return b; }\n"));
}

// An owned return is still how a function hands ownership out; only taking one
// in is gone.
static void test_an_owned_return_is_still_allowed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): *Box {\n"
                        "    let b: *Box = new Box;\n"
                        "    b.n = 4;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: *Box = make(); return b.n; }\n"
                        "let r: int = main();") == 4);
}

// A '*T' parameter may not be stored into an owning field. This is what makes
// passing an owned pointer to a borrowing callee safe at all: the callee cannot
// give the object a second owner.
static void test_an_owning_parameter_still_cannot_be_stored() {
    assert(!test_codegens("struct Box { n: int, child: *Box }\n"
                          "func adopt(parent: ref Box, other: ref Box): int {\n"
                          "    parent.child = other;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = 0;"));
}

// '&o' where 'o' owns yields a borrow of an owning pointer -- 'ref *Box' -- and
// that type cannot be written: 'ref' does not combine with '*'. Producing a
// value nothing can name is worse than refusing it, so this is refused.
//
// The type is what an out-parameter would need: a borrow of the caller's
// variable rather than of the object, so the callee could repoint it. That is
// more than syntax, since assigning through one would free the caller's old
// object from inside the callee -- an owning slot changing owner mid-call.
// Returning ownership says the same thing with the transfer visible.
static void test_taking_the_address_of_an_owning_pointer_is_refused() {
    // Nothing constrains the type here, so only the address-of itself can
    // refuse it. Written as a bare statement for that reason: given a
    // declaration to mismatch against, this would fail either way and the test
    // would not say which rule caught it.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let o: *Box = new Box;\n"
                          "    &o;\n"
                          "    return 0;\n"
                          "}\n"));

    // And as an argument, which is where an out-parameter would be attempted.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func replace(slot: ref ref Box): int { return 0; }\n"
                          "func main(): int {\n"
                          "    let o: *Box = new Box;\n"
                          "    return replace(&o);\n"
                          "}\n"));
}

// A borrow of a borrow is still fine: 'ref ref T' is writable, and '&' applied
// to a 'ref T' produces exactly it.
static void test_taking_the_address_of_a_borrow_is_allowed() {
    assert(test_run_int("func f(): int {\n"
                        "    let x: int = 5;\n"
                        "    let p: ref int = &x;\n"
                        "    let q: ref ref int = &p;\n"
                        "    **q = 11;\n"
                        "    return x;\n"
                        "}\n"
                        "let r: int = f();") == 11);
}

// Writing through a borrow is the out-parameter that does work: the callee
// fills in a struct the caller owns, which is what 'ref' is for.
static void test_a_ref_parameter_is_an_out_parameter_for_values() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func fill(b: ref Box): int { b.n = 42; return 0; }\n"
                        "func main(): int {\n"
                        "    let o: *Box = new Box;\n"
                        "    fill(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

int main(void) {
    test_taking_the_address_of_an_owning_pointer_is_refused();
    test_taking_the_address_of_a_borrow_is_allowed();
    test_a_ref_parameter_is_an_out_parameter_for_values();
    test_an_owned_receiver_calls_a_ref_method();
    test_a_ref_receiver_mutates_the_owned_object();
    test_an_owning_receiver_is_refused();
    test_an_owning_parameter_is_refused();
    test_an_owning_method_parameter_is_refused();
    test_a_borrow_cannot_be_laundered_into_an_owned_return();
    test_an_owned_return_is_still_allowed();
    test_an_owning_parameter_still_cannot_be_stored();
    test_a_ref_to_a_local_cannot_be_returned();
    test_a_ref_returned_from_a_call_cannot_outlive_its_argument();
    test_a_ref_declared_from_a_call_carries_the_argument_lifetime();
    test_a_ref_borrowed_from_a_heap_object_is_accepted();
    test_an_owned_return_does_not_inherit_argument_lifetimes();
    test_a_method_on_a_ref_receiver();
    test_a_method_through_a_ref_mutates_the_owned_object();
    test_a_ref_passes_to_a_ref_parameter();
    test_an_owned_pointer_passes_to_a_ref_parameter();
    test_a_ref_does_not_fill_an_owning_variable();
    test_a_call_returning_a_ref_is_not_freed();
    test_the_readme_sample_compiles();
    test_ref_is_a_distinct_type();
    test_ref_pointers_are_interned();
    test_a_self_referential_struct_declares();
    test_conversion_is_owned_to_ref_only();
    test_ref_does_not_combine_with_a_star();
    test_a_ref_field_reads_and_writes();
    test_a_parent_child_graph_frees_once();
    test_an_abnormal_unwind_frees_what_it_held();

    printf("All ref tests passed\n");
    return 0;
}
