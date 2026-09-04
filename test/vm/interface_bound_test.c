#include "support/run.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const char COUNTABLE[] = "interface Countable {\n"
                                "    func count(self: &Self): int;\n"
                                "}\n"
                                "struct Bag { n: int }\n"
                                "impl Bag as Countable {\n"
                                "    func count(self: &Self): int { return self.n; }\n"
                                "}\n";

static bool compiles_with(const char *rest) {
    char source[2048];
    snprintf(source, sizeof(source), "%s%s", COUNTABLE, rest);

    return test_compiles(source);
}

static int runs_with(const char *rest) {
    char source[2048];
    snprintf(source, sizeof(source), "%s%s", COUNTABLE, rest);

    return test_run_int(source);
}

static void test_a_bound_lets_the_body_call_what_the_interface_declares() {
    assert(compiles_with("func total<T: Countable>(x: &T): int { return x.count(); }\n"));
}

static void test_a_body_calling_what_no_bound_declares_is_refused() {
    assert(!compiles_with("func total<T: Countable>(x: &T): int { return x.missing(); }\n"));
}

static void test_an_unbounded_parameter_has_no_methods() {
    assert(!compiles_with("func total<T>(x: &T): int { return x.count(); }\n"));
}

static void test_a_bounded_call_runs_on_an_implementor() {
    assert(runs_with("func total<T: Countable>(x: &T): int { return x.count(); }\n"
                     "func main(): int {\n"
                     "    let b = Bag { n: 6 };\n"
                     "    return total(b);\n"
                     "}\n"
                     "let r: int = main();") == 6);
}

static void test_a_type_that_does_not_implement_is_refused_at_the_call() {
    assert(!compiles_with("struct Plain { m: int }\n"
                          "func total<T: Countable>(x: &T): int { return x.count(); }\n"
                          "func main(): int {\n"
                          "    let p = Plain { m: 1 };\n"
                          "    return total(p);\n"
                          "}\n"));
}

static void test_a_type_supplying_the_methods_without_declaring_it_is_refused() {
    assert(!compiles_with("struct Sneak { n: int }\n"
                          "impl Sneak { func count(self: &Sneak): int { return self.n; } }\n"
                          "func total<T: Countable>(x: &T): int { return x.count(); }\n"
                          "func main(): int {\n"
                          "    let s = Sneak { n: 1 };\n"
                          "    return total(s);\n"
                          "}\n"));
}

static void test_a_bound_names_an_interface() {
    assert(!compiles_with("func total<T: Bag>(x: &T): int { return 0; }\n"));
    assert(!compiles_with("func total<T: Missing>(x: &T): int { return 0; }\n"));
}

static void test_a_bound_is_checked_once_not_per_instantiation() {
    assert(!compiles_with("func total<T: Countable>(x: &T): int { return x.count() + x.missing(); }\n"));
}

static void test_a_generic_body_is_checked_though_nothing_calls_it() {
    assert(!compiles_with("func total<T>(x: &T): int { return x.anything_at_all(); }\n"));
    assert(!compiles_with("func total<T>(x: &T): int { let n: int = \"text\"; return n; }\n"));
}

static void test_checking_a_body_does_not_emit_a_specialization() {
    TestProgram program = test_compile("func id<T>(x: T): T { return x; }\n"
                                       "func main(): int { return id(1); }\n"
                                       "let r: int = main();");

    /* The script, and one instantiation from the call: checking the body emits nothing. */
    assert(test_func_count(&program) == 3);

    test_program_free(&program);
}

static void test_an_uncalled_generic_emits_nothing() {
    TestProgram program = test_compile("func id<T>(x: T): T { return x; }\n"
                                       "func main(): int { return 1; }\n"
                                       "let r: int = main();");

    assert(test_func_count(&program) == 2);

    test_program_free(&program);
}

static void test_each_instantiation_gets_its_own_body() {
    TestProgram program = test_compile("func id<T>(x: T): T { return x; }\n"
                                       "func main(): int {\n"
                                       "    let a: int = id(1);\n"
                                       "    let b: float = id(2.5);\n"
                                       "    return a;\n"
                                       "}\n"
                                       "let r: int = main();");

    /* The script, main, and one body per type the call site fixes. */
    assert(test_func_count(&program) == 4);

    test_program_free(&program);
}

static void test_a_bounded_generic_emits_per_instantiation_too() {
    char source[2048];
    snprintf(source, sizeof(source), "%s%s", COUNTABLE,
             "struct Sack { n: int }\n"
             "impl Sack as Countable {\n"
             "    func count(self: &Self): int { return self.n; }\n"
             "}\n"
             "func total<T: Countable>(x: &T): int { return x.count(); }\n"
             "func main(): int {\n"
             "    let b = Bag { n: 1 };\n"
             "    let s = Sack { n: 2 };\n"
             "    return total(b) + total(s);\n"
             "}\n"
             "let r: int = main();");

    TestProgram program = test_compile(source);

    /* The script, main, both count methods, and one 'total' per bounded type. */
    assert(test_func_count(&program) == 6);

    test_program_free(&program);
}

static void test_a_generic_that_instantiates_itself_without_end_is_refused() {
    assert(test_diagnostic_mentions("func f<T>(x: &T): int { return f(x); }\n", "without end"));
}

int main(void) {
    test_a_bound_lets_the_body_call_what_the_interface_declares();
    test_a_body_calling_what_no_bound_declares_is_refused();
    test_an_unbounded_parameter_has_no_methods();
    test_a_bounded_call_runs_on_an_implementor();
    test_a_type_that_does_not_implement_is_refused_at_the_call();
    test_a_type_supplying_the_methods_without_declaring_it_is_refused();
    test_a_bound_names_an_interface();
    test_a_bound_is_checked_once_not_per_instantiation();
    test_a_generic_body_is_checked_though_nothing_calls_it();
    test_checking_a_body_does_not_emit_a_specialization();
    test_an_uncalled_generic_emits_nothing();
    test_each_instantiation_gets_its_own_body();
    test_a_bounded_generic_emits_per_instantiation_too();
    test_a_generic_that_instantiates_itself_without_end_is_refused();

    return 0;
}
