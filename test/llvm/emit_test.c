#include "support/emit.h"

#include "llvm/llvm_emit.h"

#include <assert.h>
#include <string.h>

static char *emitted(TestEmission *emission, const char *source) {
    *emission = test_lower_ir(source);

    return llvm_emit_function(emission->ctx.arena, emission->ir);
}

static char *emitted_named(TestEmission *emission, const char *source, const char *name) {
    *emission = test_lower_ir_named(source, name);

    return llvm_emit_function(emission->ctx.arena, emission->ir);
}

static void a_body_becomes_a_function_of_its_signature(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func add(a: i32, b: i32): i32 { return a + b; }\n");

    assert(strstr(text, "define i32 @test.add(i32 %0, i32 %1)"));
    assert(strstr(text, "add i32"));
    assert(strstr(text, "ret i32"));

    test_emission_free(&emission);
}

static void a_float_body_names_the_float_type(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func scale(a: f32): f32 { return a * 2.0; }\n");

    assert(strstr(text, "define float @test.scale(float %0)"));
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

static void a_local_is_a_stack_slot(void) {
    TestEmission emission;
    char *text = emitted(&emission, "func f(): i32 { let a: i32 = 1; return a; }\n");

    assert(strstr(text, "alloca i32"));
    assert(strstr(text, "store i32"));
    assert(strstr(text, "load i32"));

    test_emission_free(&emission);
}

static void a_field_is_reached_by_its_index(void) {
    TestEmission emission;
    char *text = emitted(&emission, "struct P { x: i32, y: i32 }\n"
                                    "func f(): i32 { let p = P { x: 1, y: 2 }; return p.y; }\n");

    assert(strstr(text, "alloca"));
    assert(strstr(text, "getelementptr"));
    assert(strstr(text, "i32 0, i32 1"));

    test_emission_free(&emission);
}

static void a_struct_names_its_fields_in_order(void) {
    TestEmission emission;
    char *text = emitted(&emission, "struct P { x: i32, y: f32 }\n"
                                    "func f(): f32 { let p = P { x: 1, y: 2.0 }; return p.y; }\n");

    assert(strstr(text, "{ i32, float }"));

    test_emission_free(&emission);
}

static void a_call_names_the_function_it_reaches(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "func other(): i32 { return 3; }\n"
                               "func f(): i32 { return other(); }\n",
                               "f");
    assert(strstr(text, "call i32 @test.other()"));

    test_emission_free(&emission);
}

static void a_call_passes_its_arguments_in_order(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "func sub(a: i32, b: i32): i32 { return a - b; }\n"
                               "func f(): i32 { return sub(9, 4); }\n",
                               "f");

    assert(strstr(text, "call i32 @test.sub(i32 9, i32 4)"));

    test_emission_free(&emission);
}

static void a_body_elsewhere_is_declared_rather_than_defined(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "extern \"C\" func host(a: i32): i32;\n"
                               "func f(): i32 { return host(7); }\n",
                               "f");

    assert(strstr(text, "declare i32 @host(i32)"));
    assert(strstr(text, "call i32 @host(i32 7)"));

    test_emission_free(&emission);
}

static void a_method_body_elsewhere_is_declared_too(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "struct Grid { n: i32 }\n"
                               "impl Grid {\n"
                               "    extern \"C\" func width(self: &Self): i32;\n"
                               "}\n"
                               "func f(g: &Grid): i32 { return g.width(); }\n",
                               "f");

    assert(strstr(text, "declare i32 @width(ptr)"));

    test_emission_free(&emission);
}

static void a_generic_instance_names_what_it_was_given(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "struct Holder<T> { value: T }\n"
                               "impl<T> Holder<T> {\n"
                               "    func get(self: &Self): T { return self.value; }\n"
                               "}\n"
                               "func f(h: &Holder<i32>): i32 { return h.get(); }\n",
                               "f");

    assert(strstr(text, "@\"test.Holder$i32.get\""));

    test_emission_free(&emission);
}

int main(void) {
    a_body_becomes_a_function_of_its_signature();
    a_local_is_a_stack_slot();
    a_call_names_the_function_it_reaches();
    a_call_passes_its_arguments_in_order();
    a_body_elsewhere_is_declared_rather_than_defined();
    a_method_body_elsewhere_is_declared_too();
    a_generic_instance_names_what_it_was_given();
    a_field_is_reached_by_its_index();
    a_struct_names_its_fields_in_order();
    a_float_body_names_the_float_type();
    each_arithmetic_operator_names_its_instruction();
    a_comparison_names_its_predicate();
    a_conversion_names_the_width_it_crosses();
    a_branch_names_both_of_its_blocks();

    return 0;
}
