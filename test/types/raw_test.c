#include "scope.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "llvm/llvm_symbol.h"

#include <assert.h>
#include <string.h>

static void a_raw_run_owns_nothing_its_element_owns(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *owning = type_registry_box_to(registry, type_registry_get_primitive(registry, TYPE_I32));

    assert(type_registry_owns(registry, owning));
    assert(!type_registry_owns(registry, type_registry_raw_of(registry, owning)));

    test_context_free(&ctx);
}

static void a_raw_run_copies_and_borrows_nothing(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *borrowing = type_registry_ref_to(registry, type_registry_get_primitive(registry, TYPE_I32));
    const Type *raw = type_registry_raw_of(registry, borrowing);

    assert(type_registry_borrows(registry, borrowing));

    assert(type_registry_copies(registry, raw));
    assert(!type_registry_borrows(registry, raw));

    test_context_free(&ctx);
}

static void a_raw_run_interns_by_its_element(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *i32 = type_registry_get_primitive(registry, TYPE_I32);
    const Type *u8 = type_registry_get_primitive(registry, TYPE_U8);

    assert(type_registry_raw_of(registry, i32) == type_registry_raw_of(registry, i32));
    assert(type_registry_raw_of(registry, i32) != type_registry_raw_of(registry, u8));

    assert(type_registry_raw_of(registry, i32) != type_registry_box_to(registry, i32));

    test_context_free(&ctx);
}

static void a_raw_run_and_an_owning_pointer_mangle_apart(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *i32 = type_registry_get_primitive(registry, TYPE_I32);

    const char *raw = llvm_type_symbol(ctx.arena, type_registry_raw_of(registry, i32));
    const char *owning = llvm_type_symbol(ctx.arena, type_registry_box_to(registry, i32));

    assert(strcmp(raw, owning) != 0);

    test_context_free(&ctx);
}

static void a_counting_type_is_unsigned_and_a_measuring_one_is_not(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    assert(type_is_unsigned(type_registry_get_primitive(registry, TYPE_USIZE)));
    assert(type_is_unsigned(type_registry_get_primitive(registry, TYPE_U8)));

    assert(!type_is_unsigned(type_registry_get_primitive(registry, TYPE_I32)));
    assert(!type_is_unsigned(type_registry_get_primitive(registry, TYPE_F32)));

    test_context_free(&ctx);
}

int main(void) {
    a_raw_run_owns_nothing_its_element_owns();
    a_raw_run_copies_and_borrows_nothing();
    a_raw_run_interns_by_its_element();
    a_raw_run_and_an_owning_pointer_mangle_apart();
    a_counting_type_is_unsigned_and_a_measuring_one_is_not();

    return 0;
}
