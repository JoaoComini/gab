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

static char *unit_text(TestEmission *emission, const char *source) {
    MIRFunction *bodies[16];
    size_t count = 0;

    *emission = test_lower_unit(source, bodies, 16, &count);

    LLVMUnit *unit = llvm_unit_open(emission->ctx.arena);

    for (size_t i = 0; i < count; i++) {
        llvm_unit_add(unit, bodies[i]);
    }

    char *text = llvm_unit_text(unit);

    llvm_unit_close(unit);

    return text;
}

static void a_type_owning_nothing_emits_no_drop_glue(void) {
    TestEmission emission;
    char *text = unit_text(&emission, "struct Node { n: i32 }\n"
                                      "func f(): i32 { let a = Node { n: 1 }; return a.n; }\n");

    assert(!strstr(text, "drop."));

    test_emission_free(&emission);
}

static void a_type_that_owns_is_dropped_by_glue_named_for_it(void) {
    TestEmission emission;
    char *text = unit_text(&emission, "struct Node { n: i32 }\n"
                                      "func f(): i32 { let a: *Node = box Node { n: 1 }; return a.n; }\n");

    assert(strstr(text, "define linkonce_odr void @drop.box.Node"));
    assert(strstr(text, "call void @free"));

    test_emission_free(&emission);
}

/* Counts how many times 'needle' occurs, so a definition emitted twice is distinguishable from one. */
static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0;

    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
        count++;
    }

    return count;
}

static void a_type_dropped_from_many_places_is_glued_once(void) {
    TestEmission emission;
    char *text = unit_text(&emission, "struct Inner { n: i32 }\n"
                                      "struct Outer { child: *Inner }\n"
                                      "func made(): *Inner { return box Inner { n: 6 }; }\n"
                                      "func f(): i32 {\n"
                                      "    let a: *Inner = box Inner { n: 1 };\n"
                                      "    let o: *Outer = box Outer { child: box Inner { n: 2 } };\n"
                                      "    o.child = made();\n"
                                      "    return a.n;\n"
                                      "}\n");

    assert(occurrences(text, "define linkonce_odr void @drop.box.Inner(") == 1);
    assert(occurrences(text, "call void @drop.box.Inner(") > 1);

    test_emission_free(&emission);
}

static void a_box_drops_what_its_object_owns_before_freeing_it(void) {
    TestEmission emission;
    char *text = unit_text(&emission, "struct Inner { n: i32 }\n"
                                      "struct Outer { child: *Inner }\n"
                                      "func f(): i32 {\n"
                                      "    let o: *Outer = box Outer { child: box Inner { n: 1 } };\n"
                                      "    return 0;\n"
                                      "}\n");

    const char *glue = strstr(text, "define linkonce_odr void @drop.box.Outer(");

    assert(glue);

    const char *inner = strstr(glue, "call void @drop.Outer(");
    const char *freed = strstr(glue, "call void @free(");

    assert(inner && freed && inner < freed);

    test_emission_free(&emission);
}

/* A slot the compiler proves moved-from is not read, so what a box holds need not start zeroed. */
static void a_box_allocates_without_zeroing(void) {
    TestEmission emission;
    char *text = emitted_named(&emission,
                               "struct Node { n: i32 }\n"
                               "func f(): i32 {\n"
                               "    let a: *Node = box Node { n: 7 };\n"
                               "    return a.n;\n"
                               "}\n",
                               "f");

    assert(strstr(text, "@malloc"));
    assert(!strstr(text, "@calloc"));

    test_emission_free(&emission);
}

int main(void) {
    a_body_elsewhere_is_declared_rather_than_defined();
    a_method_body_elsewhere_is_declared_too();
    a_generic_instance_names_what_it_was_given();
    a_type_owning_nothing_emits_no_drop_glue();
    a_type_that_owns_is_dropped_by_glue_named_for_it();
    a_type_dropped_from_many_places_is_glued_once();
    a_box_drops_what_its_object_owns_before_freeing_it();

    a_box_allocates_without_zeroing();

    return 0;
}
