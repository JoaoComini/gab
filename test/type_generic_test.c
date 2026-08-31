#include "binding.h"
#include "function_registry.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stddef.h>

/* Lookup then specialization, as a caller performs them. */
static Function *owned_for(TypeRegistry *registry, FunctionRegistry *functions, const Type *type,
                           const String *name) {
    Function *declaration = type_registry_find_owned(registry, type, name);

    if (!declaration || type_registry_owned_is_shared(declaration, type)) {
        return declaration;
    }

    const Type *args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < type_arg_count(type); i++) {
        args[i] = type_args(type)[i].type;
    }

    return function_registry_specialize(functions, declaration, args, type_arg_count(type));
}

static void test_a_declared_field_nests_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const Type *field_type = type_registry_block_of(registry, type_registry_box_to(registry, param));

    TypeField field = {.name = string_from_cstr(&ctx.strings, "data"), .type = field_type};

    TypeDecl decl = {
        .name = string_from_cstr(&ctx.strings, "Holder"),
        .param_count = 1,
        .fields = &field,
        .field_count = 1,
    };

    const Type *instance = type_registry_apply(registry, &decl, &int_type, 1);

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

    TypeDecl decl = {
        .name = string_from_cstr(&ctx.strings, "Plain"),
        .param_count = 0,
        .fields = &field,
        .field_count = 1,
    };

    const Type *type = type_registry_apply(registry, &decl, NULL, 0);

    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type == int_type);

    assert(type_registry_apply(registry, &decl, NULL, 0) == type);

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

    TypeDecl first = {
        .name = string_from_cstr(&ctx.strings, "First"),
        .fields = &field,
        .field_count = 1,
    };

    TypeDecl second = {
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

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Config")};

    const Type *type = type_registry_apply(registry, &decl, NULL, 0);

    assert(type_registry_fields_of(registry, type)->count == 0);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "width"),
        .type = type_registry_get_primitive(registry, TYPE_INT),
    };

    decl.fields = &field;
    decl.field_count = 1;

    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type ==
           type_registry_get_primitive(registry, TYPE_INT));

    assert(type_registry_apply(registry, &decl, NULL, 0) == type);

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

    TypeDecl decl = {
        .name = string_from_cstr(&ctx.strings, "Holder"),
        .param_count = 1,
        .fields = &field,
        .field_count = 1,
    };

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    assert(type_registry_fields_of(registry, of_int)->fields[0].type ==
           type_registry_block_of(registry, int_type));
    assert(type_registry_fields_of(registry, of_bool)->fields[0].type ==
           type_registry_block_of(registry, bool_type));

    assert(decl.fields[0].type == type_registry_block_of(registry, type_registry_param(registry, 0)));

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

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);

    assert(type_arg_count(of_int) == 1);
    assert(type_args(of_int)[0].kind == TYPE_ARG_TYPE);
    assert(type_args(of_int)[0].type == int_type);

    TypeDecl plain = {.name = string_from_cstr(&ctx.strings, "Point")};

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
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.name = at};

    Function method = {
        .decl = &method_decl,
        .return_type = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_owned(registry, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    const Function *from_int = owned_for(registry, functions, of_int, at);
    const Function *from_bool = owned_for(registry, functions, of_bool, at);

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
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);

    assert(type_registry_find_owned(registry, of_int, at) == NULL);

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.name = at};

    Function method = {
        .decl = &method_decl,
        .return_type = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_owned(registry, type_registry_apply(registry, &decl, &param, 1), &method);

    const Function *found = owned_for(registry, functions, of_int, at);

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
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.name = at};

    Function method = {
        .decl = &method_decl,
        .return_type = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_owned(registry, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);

    FuncDecl other_decl = {.name = at};
    Function other = {.decl = &other_decl};

    assert(!type_registry_declare_owned(registry, of_int, &other));

    assert(owned_for(registry, functions, of_int, at)->return_type == int_type);

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

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    FuncDecl method_decl = {.name = name};
    Function method = {.decl = &method_decl};

    assert(type_registry_declare_owned(registry, of_int, &method));

    assert(type_registry_find_owned(registry, of_bool, name)->decl->name == name);

    assert(!type_registry_declare_owned(registry, of_bool, &method));

    TypeDecl other = {.name = string_from_cstr(&ctx.strings, "Other"), .param_count = 1};

    assert(type_registry_find_owned(registry, type_registry_apply(registry, &other, &int_type, 1), name) ==
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
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.name = at};

    Function method = {
        .decl = &method_decl,
        .return_type = param,
        .params = receiver,
        .param_count = 1,
    };

    type_registry_declare_owned(registry, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);

    assert(owned_for(registry, functions, of_int, at) == owned_for(registry, functions, of_int, at));

    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    assert(owned_for(registry, functions, of_int, at) != owned_for(registry, functions, of_bool, at));

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

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);
    const Type *generic = type_registry_apply(registry, &decl, &param, 1);

    assert(of_int != of_bool);
    assert(generic == type_registry_apply(registry, &decl, &param, 1));
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

    TypeDecl decl = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    const Type *of_int = type_registry_apply(registry, &decl, &int_type, 1);

    const TypeField fields[] = {{.name = string_from_cstr(&ctx.strings, "value"), .type = param}};
    decl.fields = fields;
    decl.field_count = 1;

    assert(type_registry_fields_of(registry, of_int)->count == 1);
    assert(type_registry_fields_of(registry, of_int)->fields[0].type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_specialization_does_not_inherit_a_summary() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const Type *params[] = {type_registry_ref_to(registry, param)};

    FuncDecl generic_decl = {.name = string_from_cstr(&ctx.strings, "pick"), .type_param_count = 1};

    Function *generic = arena_alloc(ctx.arena, sizeof(Function));

    *generic = (Function){
        .decl = &generic_decl,
        .return_type = type_registry_ref_to(registry, param),
        .params = params,
        .param_count = 1,
        .func_index = FUNCTION_NO_BODY,
    };

    generic->borrowed_params = 1;
    generic->borrowed_params_known = true;

    Function *specialized = function_registry_specialize(functions, generic, &int_type, 1);

    assert(!specialized->borrowed_params_known);

    function_registry_destroy(functions);
    test_context_free(&ctx);
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
    test_a_specialization_does_not_inherit_a_summary();
    return 0;
}
