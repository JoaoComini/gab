#include "binding.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stddef.h>

static void test_a_declared_field_nests_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const Type *field_type = type_registry_block_of(registry, type_registry_box_to(registry, param));

    TypeField field = {.name = string_from_cstr(&ctx.strings, "data"), .type = field_type};

    TypeDef def = {
        .name = string_from_cstr(&ctx.strings, "Holder"),
        .param_count = 1,
        .fields = &field,
        .field_count = 1,
    };

    const Type *instance = type_registry_apply(registry, &def, &int_type, 1);

    assert(type_registry_fields_of(registry, instance)->count == 1);

    const Type *data = type_registry_fields_of(registry, instance)->fields[0].type;

    assert(type_kind(data) == TYPE_BLOCK);

    const Type *element = type_pointee(data);

    assert(type_kind(element) == TYPE_BOX);
    assert(type_pointee(element) == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

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

    const Type *type = type_registry_apply(registry, &def, NULL, 0);

    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type == int_type);

    assert(type_registry_apply(registry, &def, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

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

    assert(type_registry_apply(registry, &first, NULL, 0) != type_registry_apply(registry, &second, NULL, 0));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_reads_fields_declared_after_it() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Config")};

    const Type *type = type_registry_apply(registry, &def, NULL, 0);

    assert(type_registry_fields_of(registry, type)->count == 0);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "width"),
        .type = type_registry_get_primitive(registry, TYPE_INT),
    };

    def.fields = &field;
    def.field_count = 1;

    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type ==
           type_registry_get_primitive(registry, TYPE_INT));

    assert(type_registry_apply(registry, &def, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

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

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    assert(type_registry_fields_of(registry, of_int)->fields[0].type ==
           type_registry_block_of(registry, int_type));
    assert(type_registry_fields_of(registry, of_bool)->fields[0].type ==
           type_registry_block_of(registry, bool_type));

    assert(def.fields[0].type == type_registry_block_of(registry, type_registry_param(registry, 0)));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_carries_its_arguments() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    assert(type_arg_count(of_int) == 1);
    assert(type_args(of_int)[0].kind == TYPE_ARG_TYPE);
    assert(type_args(of_int)[0].type == int_type);

    TypeDef plain = {.name = string_from_cstr(&ctx.strings, "Point")};

    assert(type_arg_count(type_registry_apply(registry, &plain, NULL, 0)) == 0);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_declared_method_is_substituted_per_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &def, &param, 1)};

    MethodDecl method = {
        .name = at,
        .result = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_method(registry, type_registry_apply(registry, &def, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    const Function *from_int = type_registry_find_method(registry, of_int, at);
    const Function *from_bool = type_registry_find_method(registry, of_bool, at);

    assert(from_int && from_bool);

    assert(from_int->return_type == int_type);
    assert(from_bool->return_type == bool_type);

    assert(from_int->params[0] == of_int);
    assert(from_bool->params[0] == of_bool);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_method_reaches_an_instantiation_interned_before_it() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    assert(type_registry_find_method(registry, of_int, at) == NULL);

    const Type *receiver[] = {type_registry_apply(registry, &def, &param, 1)};

    MethodDecl method = {
        .name = at,
        .result = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_method(registry, type_registry_apply(registry, &def, &param, 1), &method);

    const Function *found = type_registry_find_method(registry, of_int, at);

    assert(found);
    assert(found->return_type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_declared_method_takes_the_name_on_every_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &def, &param, 1)};

    MethodDecl method = {
        .name = at,
        .result = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_method(registry, type_registry_apply(registry, &def, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    Function other = {0};

    assert(!type_registry_declare_method(registry, of_int, &(MethodDecl){.name = at, .function = &other}));

    assert(type_registry_find_method(registry, of_int, at)->return_type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_method_declared_on_one_instantiation_answers_on_every_one() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *name = string_from_cstr(&ctx.strings, "spill");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    Function method = {0};

    assert(type_registry_declare_method(registry, of_int, &(MethodDecl){.name = name, .function = &method}));

    assert(type_registry_find_method(registry, of_bool, name) == &method);

    assert(
        !type_registry_declare_method(registry, of_bool, &(MethodDecl){.name = name, .function = &method}));

    TypeDef other = {.name = string_from_cstr(&ctx.strings, "Other"), .param_count = 1};

    assert(type_registry_find_method(registry, type_registry_apply(registry, &other, &int_type, 1), name) ==
           NULL);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_substituted_signature_is_read_once_per_type() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &def, &param, 1)};

    MethodDecl method = {
        .name = at,
        .result = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_method(registry, type_registry_apply(registry, &def, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    assert(type_registry_find_method(registry, of_int, at) ==
           type_registry_find_method(registry, of_int, at));

    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    assert(type_registry_find_method(registry, of_int, at) !=
           type_registry_find_method(registry, of_bool, at));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_two_instantiations_share_one_generic_form(void) {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);
    const Type *param = type_registry_param(registry, 0);

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);
    const Type *generic = type_registry_apply(registry, &def, &param, 1);

    assert(of_int != of_bool);
    assert(generic == type_registry_apply(registry, &def, &param, 1));
    assert(generic != of_int && generic != of_bool);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_reads_fields_declared_after_it_is_applied(void) {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    const TypeField fields[] = {{.name = string_from_cstr(&ctx.strings, "value"), .type = param}};
    def.fields = fields;
    def.field_count = 1;

    assert(type_registry_fields_of(registry, of_int)->count == 1);
    assert(type_registry_fields_of(registry, of_int)->fields[0].type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

int main(void) {
    test_an_instantiation_reads_fields_declared_after_it_is_applied();
    test_two_instantiations_share_one_generic_form();
    test_a_declared_field_nests_constructors();
    test_a_declaration_taking_no_parameters_is_its_own_instantiation();
    test_two_declarations_alike_are_two_types();
    test_an_instantiation_reads_fields_declared_after_it();
    test_an_instantiation_does_not_share_the_declarations_fields();
    test_an_instantiation_carries_its_arguments();
    test_a_declared_method_is_substituted_per_instantiation();
    test_a_method_reaches_an_instantiation_interned_before_it();
    test_a_declared_method_takes_the_name_on_every_instantiation();
    test_a_method_declared_on_one_instantiation_answers_on_every_one();
    test_a_substituted_signature_is_read_once_per_type();
    return 0;
}
