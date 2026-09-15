#include "scope.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>

static void a_raw_run_copies_and_borrows_nothing(void) {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

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

    TypeRegistry *registry = ctx.types;

    const Type *i32 = type_registry_get_primitive(registry, TYPE_I32);
    const Type *u8 = type_registry_get_primitive(registry, TYPE_U8);

    assert(type_registry_raw_of(registry, i32) == type_registry_raw_of(registry, i32));
    assert(type_registry_raw_of(registry, i32) != type_registry_raw_of(registry, u8));

    test_context_free(&ctx);
}

static void a_counting_type_is_unsigned_and_a_measuring_one_is_not(void) {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

    assert(type_is_unsigned(type_registry_get_primitive(registry, TYPE_USIZE)));
    assert(type_is_unsigned(type_registry_get_primitive(registry, TYPE_U8)));

    assert(!type_is_unsigned(type_registry_get_primitive(registry, TYPE_I32)));
    assert(!type_is_unsigned(type_registry_get_primitive(registry, TYPE_F32)));

    test_context_free(&ctx);
}

int main(void) {
    a_raw_run_copies_and_borrows_nothing();
    a_raw_run_interns_by_its_element();
    a_counting_type_is_unsigned_and_a_measuring_one_is_not();

    return 0;
}
