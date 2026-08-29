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

    TypeDef def = {
        .name = string_from_cstr(&ctx.strings, "Holder"),
        .param_count = 1,
        .fields = &field,
        .field_count = 1,
    };

    const Type *instance = type_registry_instantiate(registry, &def, &int_type, 1);

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

// A declaration taking no parameters is its own instantiation: supplying none
// is what lays it out, so a plain struct and a generic reach a type the same
// way.
static void test_a_declaration_taking_no_parameters_is_its_own_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    TypeField field = {.name = string_from_cstr(&ctx.strings, "value"), .type = int_type};

    TypeDef def = {
        .name = string_from_cstr(&ctx.strings, "Plain"),
        .param_count = 0,
        .fields = &field,
        .field_count = 1,
    };

    const Type *type = type_registry_instantiate(registry, &def, NULL, 0);

    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_field_count(type) == 1);
    assert(type_fields(type)[0].type == int_type);

    // Interned like any other application, so a second mention is the same type
    // rather than a second one laid out alike.
    assert(type_registry_instantiate(registry, &def, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// Two declarations with the same fields are two types: a nominal name is what
// tells them apart, so what they are laid out as does not merge them.
static void test_two_declarations_alike_are_two_types() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    TypeField field = {.name = string_from_cstr(&ctx.strings, "value"), .type = int_type};

    TypeDef first = {
        .name = string_from_cstr(&ctx.strings, "First"),
        .fields = &field,
        .field_count = 1,
    };

    TypeDef second = {
        .name = string_from_cstr(&ctx.strings, "Second"),
        .fields = &field,
        .field_count = 1,
    };

    assert(type_registry_instantiate(registry, &first, NULL, 0) !=
           type_registry_instantiate(registry, &second, NULL, 0));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A declaration applied to no arguments keeps no fields of its own: it reads
// its declaration's. So the type may be interned while the declaration is still
// empty, and the fields it answers with are whatever the declaration holds when
// it is asked -- which is what a struct naming one declared below it needs.
static void test_an_instantiation_reads_fields_declared_after_it() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Config")};

    const Type *type = type_registry_instantiate(registry, &def, NULL, 0);

    assert(type_field_count(type) == 0);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "width"),
        .type = type_registry_get_primitive(registry, TYPE_INT),
    };

    def.fields = &field;
    def.field_count = 1;

    assert(type_field_count(type) == 1);
    assert(type_fields(type)[0].type == type_registry_get_primitive(registry, TYPE_INT));

    // The same type, not a second interned once the fields arrived.
    assert(type_registry_instantiate(registry, &def, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A declaration's own fields are written over its parameters, and an
// instantiation's are those substituted. So the two disagree by construction:
// the declaration holds 'block T' whatever anything is instantiated with, and
// each instantiation holds the block its own argument names.
static void test_an_instantiation_does_not_share_the_declarations_fields() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "data"),
        .type = type_registry_block_of(registry, type_registry_param(registry, 0)),
    };

    TypeDef def = {
        .name = string_from_cstr(&ctx.strings, "Holder"),
        .param_count = 1,
        .fields = &field,
        .field_count = 1,
    };

    const Type *of_int = type_registry_instantiate(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_instantiate(registry, &def, &bool_type, 1);

    assert(type_fields(of_int)[0].type == type_registry_block_of(registry, int_type));
    assert(type_fields(of_bool)[0].type == type_registry_block_of(registry, bool_type));

    // The declaration still says what it said, over the parameter.
    assert(def.fields[0].type == type_registry_block_of(registry, type_registry_param(registry, 0)));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

int main(void) {
    test_a_declared_field_nests_constructors();
    test_a_declaration_taking_no_parameters_is_its_own_instantiation();
    test_two_declarations_alike_are_two_types();
    test_an_instantiation_reads_fields_declared_after_it();
    test_an_instantiation_does_not_share_the_declarations_fields();
    return 0;
}
