// Compound assignment: 'a += b' and its four siblings. Each is the assignment
// it desugars to, so what is worth stating is that the desugaring reaches the
// same targets ordinary assignment does, and inherits each operator's own
// rules rather than relaxing them.
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_add_assign() {
    assert(test_run_int("func f(): int { let a: int = 4; a += 3; return a; }\n"
                        "let r: int = f();\n") == 7);
}

static void test_sub_assign() {
    assert(test_run_int("func f(): int { let a: int = 4; a -= 3; return a; }\n"
                        "let r: int = f();\n") == 1);
}

static void test_mul_assign() {
    assert(test_run_int("func f(): int { let a: int = 4; a *= 3; return a; }\n"
                        "let r: int = f();\n") == 12);
}

static void test_div_assign() {
    assert(test_run_int("func f(): int { let a: int = 12; a /= 4; return a; }\n"
                        "let r: int = f();\n") == 3);
}

static void test_mod_assign() {
    assert(test_run_int("func f(): int { let a: int = 17; a %= 5; return a; }\n"
                        "let r: int = f();\n") == 2);
}

// The target is the left operand, not the right: subtraction and division are
// the two where swapping them still yields a value, so they are what pins the
// order down.
static void test_the_target_is_the_left_operand() {
    assert(test_run_int("func f(): int { let a: int = 10; a -= 4; return a; }\n"
                        "let r: int = f();\n") == 6);

    assert(test_run_int("func f(): int { let a: int = 20; a /= 5; return a; }\n"
                        "let r: int = f();\n") == 4);
}

// The right-hand side is a whole expression, so it is taken as one rather than
// binding only its first term: 'a += 2 * 3' adds 6, not 2.
static void test_the_operand_is_a_whole_expression() {
    assert(test_run_int("func f(): int { let a: int = 1; a += 2 * 3; return a; }\n"
                        "let r: int = f();\n") == 7);

    assert(test_run_int("func f(): int { let a: int = 1; let b: int = 4; a += b - 1; return a; }\n"
                        "let r: int = f();\n") == 4);
}

static void test_compound_assign_on_a_float() {
    assert(test_run_float("func f(): float { let a: float = 1.5; a += 2.0; return a; }\n"
                          "let r: float = f();\n") == 3.5f);
}

// Every target ordinary assignment reaches, compound assignment reaches too.
// Subtraction on each target kind, since it is the operator that tells the
// target from the operand: a field or a deref reading them the other way round
// would still produce a value, just the wrong one.
static void test_compound_assign_reaches_a_field() {
    assert(test_run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 4; v.x += 3; return v.x; }\n"
                        "let r: int = f();\n") == 7);

    assert(test_run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 10; v.x -= 4; return v.x; }\n"
                        "let r: int = f();\n") == 6);
}

static void test_compound_assign_reaches_through_a_pointer() {
    assert(test_run_int("func f(): int { let a: int = 4; let p: ref int = ref a; *p += 3; return a; }\n"
                        "let r: int = f();\n") == 7);

    assert(test_run_int("func f(): int { let a: int = 10; let p: ref int = ref a; *p -= 4; return a; }\n"
                        "let r: int = f();\n") == 6);
}

// Each compound form inherits its operator's type rule rather than relaxing
// it: '%=' is int-only because '%' is, and none of them apply to bool.
static void test_compound_assign_inherits_the_operator_type_rule() {
    assert(test_compiles("func f(): int { let a: int = 1; a %= 2; return a; }\n"));

    assert(!test_compiles("func f(): float { let a: float = 1.0; a %= 2.0; return a; }\n"));
    assert(!test_compiles("func f(): bool { let a: bool = true; a += true; return a; }\n"));
}

// And its runtime trap: '/=' by zero fails the run the way '/' does.
static void test_div_assign_by_zero_traps() {
    assert(test_run_status("func f(): int { let a: int = 1; let b: int = 0; a /= b; return a; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

// The target is evaluated once, so a call inside it runs once. 'bump' counts
// its own calls through a borrowed counter, which is what distinguishes one
// evaluation from two: a target named twice would leave 2 here.
static void test_the_target_is_evaluated_once() {
    assert(test_run_int("func bump(n: ref int, p: ref int): ref int { *n += 1; return p; }\n"
                        "func f(): int { let seen: int = 0; let a: int = 4;\n"
                        "                *bump(ref seen, ref a) += 3;\n"
                        "                return seen; }\n"
                        "let r: int = f();\n") == 1);
}

// ...and the single evaluation still produces the right value, not just the
// right number of calls.
static void test_a_call_in_the_target_still_assigns() {
    assert(test_run_int("func pick(p: ref int): ref int { return p; }\n"
                        "func f(): int { let a: int = 4; *pick(ref a) += 3; return a; }\n"
                        "let r: int = f();\n") == 7);
}

// A compound assignment is a statement, not an expression, exactly as '=' is.
static void test_compound_assign_is_not_an_expression() {
    assert(!test_compiles("func f(): int { let a: int = 1; let b: int = (a += 1); return b; }\n"));
}

// The target must still be assignable, so the operand of a compound assignment
// is no more of a home than the operand of a plain one.
static void test_the_target_must_be_assignable() {
    assert(!test_compiles("func f(): int { let a: int = 1; -a += 1; return a; }\n"));
    assert(!test_compiles("func f(): int { let a: int = 1; 2 += a; return a; }\n"));
}

int main() {
    test_add_assign();
    test_sub_assign();
    test_mul_assign();
    test_div_assign();
    test_mod_assign();
    test_the_target_is_the_left_operand();
    test_the_operand_is_a_whole_expression();
    test_compound_assign_on_a_float();
    test_compound_assign_reaches_a_field();
    test_compound_assign_reaches_through_a_pointer();
    test_compound_assign_inherits_the_operator_type_rule();
    test_div_assign_by_zero_traps();
    test_the_target_is_evaluated_once();
    test_a_call_in_the_target_still_assigns();
    test_compound_assign_is_not_an_expression();
    test_the_target_must_be_assignable();

    printf("compound_assign_test: all tests passed\n");
    return 0;
}
