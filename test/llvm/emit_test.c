#include "support/emit.h"

#include "llvm/llvm_emit.h"

#include <assert.h>
#include <string.h>

static char *emitted(TestEmission *emission, const char *source) {
    *emission = test_lower_ir(source);

    return llvm_emit_function(emission->ctx.arena, emission->ir);
}

static void a_body_becomes_a_function_of_its_signature(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func add(a: i32, b: i32): i32 { return a + b; }\n");

    assert(strstr(text, "define i32 @add(i32 %0, i32 %1)"));
    assert(strstr(text, "add i32"));
    assert(strstr(text, "ret i32"));

    test_emission_free(&emission);
}

static void a_float_body_names_the_float_type(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func scale(a: f32): f32 { return a * 2.0; }\n");

    assert(strstr(text, "define float @scale(float %0)"));
    assert(strstr(text, "fmul float"));

    test_emission_free(&emission);
}

static void each_arithmetic_operator_names_its_instruction(void) {
    static const struct {
        const char *source;
        const char *mnemonic;
    } CASES[] = {
        {"func f(a: i32, b: i32): i32 { return a - b; }\n", "sub i32"},
        {"func f(a: i32, b: i32): i32 { return a * b; }\n", "mul i32"},
        {"func f(a: i32, b: i32): i32 { return a / b; }\n", "sdiv i32"},
        {"func f(a: i32, b: i32): i32 { return a % b; }\n", "srem i32"},
        {"func f(a: f32, b: f32): f32 { return a + b; }\n", "fadd float"},
        {"func f(a: f32, b: f32): f32 { return a / b; }\n", "fdiv float"},
    };

    for (size_t i = 0; i < sizeof(CASES) / sizeof(*CASES); i++) {
        TestEmission emission;
        char *text = emitted(&emission, CASES[i].source);

        assert(strstr(text, CASES[i].mnemonic));

        test_emission_free(&emission);
    }
}

static void a_comparison_names_its_predicate(void) {
    static const struct {
        const char *source;
        const char *mnemonic;
    } CASES[] = {
        {"func f(a: i32, b: i32): bool { return a < b; }\n", "icmp slt i32"},
        {"func f(a: i32, b: i32): bool { return a >= b; }\n", "icmp sge i32"},
        {"func f(a: i32, b: i32): bool { return a == b; }\n", "icmp eq i32"},
        {"func f(a: f32, b: f32): bool { return a < b; }\n", "fcmp olt float"},
    };

    for (size_t i = 0; i < sizeof(CASES) / sizeof(*CASES); i++) {
        TestEmission emission;
        char *text = emitted(&emission, CASES[i].source);

        assert(strstr(text, CASES[i].mnemonic));

        test_emission_free(&emission);
    }
}

static void a_conversion_names_the_width_it_crosses(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func f(a: i32): f32 { return f32(a); }\n");

    assert(strstr(text, "sitofp i32"));
    assert(strstr(text, "to float"));

    test_emission_free(&emission);
}

static void a_branch_names_both_of_its_blocks(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func f(a: i32): i32 { if a < 1 { return 0; } return a; }\n");

    assert(strstr(text, "br i1 "));
    assert(strstr(text, ", label %b"));

    test_emission_free(&emission);
}

int main(void) {
    a_body_becomes_a_function_of_its_signature();
    a_float_body_names_the_float_type();
    each_arithmetic_operator_names_its_instruction();
    a_comparison_names_its_predicate();
    a_conversion_names_the_width_it_crosses();
    a_branch_names_both_of_its_blocks();

    return 0;
}
