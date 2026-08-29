#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stddef.h>

// A declaration's field is an ordinary type written over the parameters, so it
// nests as any type does: instantiating one substitutes at every depth rather
// than at the single constructor a field could name.
static void test_a_declared_field_nests_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    // 'block box T': the constructor a single-slot field cannot spell.
    const Type *field_type = type_registry_block_of(registry, type_registry_box_to(registry, param));

    TypeField field = {.name = string_from_cstr(&ctx.strings, "data"), .type = field_type};

    GenericDecl generic = {.param_count = 1, .fields = &field, .field_count = 1};

    const TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .generic = &generic};

    const Type *holder = type_registry_declare(registry, &decl);

    const Type *instance = type_registry_instantiate(registry, holder, &int_type, 1);

    assert(type_field_count(instance) == 1);

    const Type *data = type_fields(instance)[0].type;

    assert(type_kind(data) == TYPE_BLOCK);

    const Type *element = type_pointee(data);

    assert(type_kind(element) == TYPE_BOX);
    assert(type_pointee(element) == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

int main(void) {
    test_a_declared_field_nests_constructors();
    return 0;
}
