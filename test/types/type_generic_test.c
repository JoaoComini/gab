#include "decl.h"
#include "function_registry.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stddef.h>

static TypeDecl named_decl(String *name) { return (TypeDecl){.id = {.name = name}}; }

static void test_a_declared_field_nests_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    const Type *field_type = type_registry_raw_of(registry, type_registry_box_to(registry, param));

    TypeField field = {.name = string_from_cstr(&ctx.strings, "data"), .type = field_type};

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;
    decl.fields = &field;
    decl.field_count = 1;

    const Type *instance = type_registry_apply(registry, &decl, &i32_type, 1);

    assert(type_registry_fields_of(registry, instance)->count == 1);

    const Type *data = type_registry_fields_of(registry, instance)->fields[0].type;

    assert(type_kind(data) == TYPE_RAW);

    const Type *element = type_pointee(data);

    assert(type_kind(element) == TYPE_BOX);
    assert(type_pointee(element) == i32_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_declaration_taking_no_parameters_is_its_own_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    TypeField field = {.name = string_from_cstr(&ctx.strings, "value"), .type = i32_type};

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Plain"));

    decl.param_count = 0;
    decl.fields = &field;
    decl.field_count = 1;

    const Type *type = type_registry_apply(registry, &decl, NULL, 0);

    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type == i32_type);

    assert(type_registry_apply(registry, &decl, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_two_declarations_alike_are_two_types() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    TypeField field = {.name = string_from_cstr(&ctx.strings, "value"), .type = i32_type};

    TypeDecl first = named_decl(string_from_cstr(&ctx.strings, "First"));
    TypeDecl second = named_decl(string_from_cstr(&ctx.strings, "Second"));

    first.fields = second.fields = &field;
    first.field_count = second.field_count = 1;

    assert(type_registry_apply(registry, &first, NULL, 0) != type_registry_apply(registry, &second, NULL, 0));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_reads_fields_declared_after_it() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Config"));

    const Type *type = type_registry_apply(registry, &decl, NULL, 0);

    assert(type_registry_fields_of(registry, type)->count == 0);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "width"),
        .type = type_registry_get_primitive(registry, TYPE_I32),
    };

    decl.fields = &field;
    decl.field_count = 1;

    assert(type_registry_fields_of(registry, type)->count == 1);
    assert(type_registry_fields_of(registry, type)->fields[0].type ==
           type_registry_get_primitive(registry, TYPE_I32));

    assert(type_registry_apply(registry, &decl, NULL, 0) == type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_does_not_share_the_declarations_fields() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    TypeField field = {
        .name = string_from_cstr(&ctx.strings, "data"),
        .type = type_registry_raw_of(registry, type_registry_param(registry, 0)),
    };

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;
    decl.fields = &field;
    decl.field_count = 1;

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    assert(type_registry_fields_of(registry, of_int)->fields[0].type ==
           type_registry_raw_of(registry, i32_type));
    assert(type_registry_fields_of(registry, of_bool)->fields[0].type ==
           type_registry_raw_of(registry, bool_type));

    assert(decl.fields[0].type == type_registry_raw_of(registry, type_registry_param(registry, 0)));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_an_instantiation_carries_its_arguments() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);

    assert(type_arg_count(of_int) == 1);
    assert(type_args(of_int)[0].kind == TYPE_ARG_TYPE);
    assert(type_args(of_int)[0].type == i32_type);

    TypeDecl plain = named_decl(string_from_cstr(&ctx.strings, "Point"));

    assert(type_arg_count(type_registry_apply(registry, &plain, NULL, 0)) == 0);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_declared_method_is_substituted_per_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.id = {.name = at},
                            .signature = {.return_type = param, .params = receiver, .param_count = 1}};

    Function method = {.decl = &method_decl, .signature = method_decl.signature};

    function_registry_declare_owned(functions, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    const Function *from_int = function_registry_owned_for(functions, of_int, at);
    const Function *from_bool = function_registry_owned_for(functions, of_bool, at);

    assert(from_int && from_bool);

    assert(from_int->signature.return_type == i32_type);
    assert(from_bool->signature.return_type == bool_type);

    assert(from_int->signature.params[0] == of_int);
    assert(from_bool->signature.params[0] == of_bool);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_method_reaches_an_instantiation_interned_before_it() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);

    assert(function_registry_find_owned(functions, of_int, at) == NULL);

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.id = {.name = at},
                            .signature = {.return_type = param, .params = receiver, .param_count = 1}};

    Function method = {.decl = &method_decl, .signature = method_decl.signature};

    function_registry_declare_owned(functions, type_registry_apply(registry, &decl, &param, 1), &method);

    const Function *found = function_registry_owned_for(functions, of_int, at);

    assert(found);
    assert(found->signature.return_type == i32_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_declared_method_takes_the_name_on_every_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.id = {.name = at},
                            .signature = {.return_type = param, .params = receiver, .param_count = 1}};

    Function method = {.decl = &method_decl, .signature = method_decl.signature};

    function_registry_declare_owned(functions, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);

    FuncDecl other_decl = {.id = {.name = at}};
    Function other = {.decl = &other_decl};

    assert(!function_registry_declare_owned(functions, of_int, &other));

    assert(function_registry_owned_for(functions, of_int, at)->signature.return_type == i32_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_method_declared_on_one_instantiation_answers_on_every_one() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *name = string_from_cstr(&ctx.strings, "spill");

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);
    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    FuncDecl method_decl = {.id = {.name = name}};
    Function method = {.decl = &method_decl};

    assert(function_registry_declare_owned(functions, of_int, &method));

    assert(function_registry_find_owned(functions, of_bool, name)->decl->id.name == name);

    assert(!function_registry_declare_owned(functions, of_bool, &method));

    TypeDecl other = named_decl(string_from_cstr(&ctx.strings, "Other"));

    other.param_count = 1;

    assert(function_registry_find_owned(functions, type_registry_apply(registry, &other, &i32_type, 1),
                                        name) == NULL);

    function_registry_destroy(functions);
    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_substituted_signature_is_read_once_per_type() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *receiver[] = {type_registry_apply(registry, &decl, &param, 1)};

    FuncDecl method_decl = {.id = {.name = at},
                            .signature = {.return_type = param, .params = receiver, .param_count = 1}};

    Function method = {.decl = &method_decl, .signature = method_decl.signature};

    function_registry_declare_owned(functions, type_registry_apply(registry, &decl, &param, 1), &method);

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);

    assert(function_registry_owned_for(functions, of_int, at) ==
           function_registry_owned_for(functions, of_int, at));

    const Type *of_bool = type_registry_apply(registry, &decl, &bool_type, 1);

    assert(function_registry_owned_for(functions, of_int, at) !=
           function_registry_owned_for(functions, of_bool, at));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_two_instantiations_share_one_generic_form(void) {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);
    const Type *param = type_registry_param(registry, 0);

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);
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

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    TypeDecl decl = named_decl(string_from_cstr(&ctx.strings, "Holder"));

    decl.param_count = 1;

    const Type *of_int = type_registry_apply(registry, &decl, &i32_type, 1);

    const TypeField fields[] = {{.name = string_from_cstr(&ctx.strings, "value"), .type = param}};
    decl.fields = fields;
    decl.field_count = 1;

    assert(type_registry_fields_of(registry, of_int)->count == 1);
    assert(type_registry_fields_of(registry, of_int)->fields[0].type == i32_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

static void test_a_specialization_does_not_inherit_a_summary() {
    TestContext ctx;
    test_context_init(&ctx);

    const KnownNames names = known_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);
    FunctionRegistry *functions = function_registry_create(ctx.arena, registry);

    const Type *param = type_registry_param(registry, 0);
    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    const Type *params[] = {type_registry_ref_to(registry, param)};

    FuncDecl generic_decl = {
        .id = {.name = string_from_cstr(&ctx.strings, "pick")},
        .signature = {.return_type = type_registry_ref_to(registry, param),
                      .params = params,
                      .param_count = 1},
        .type_param_count = 1,
    };

    TypeArg argument = {.kind = TYPE_ARG_TYPE, .type = i32_type};

    Function *specialized = function_registry_instance(functions, &generic_decl, &argument, 1);

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
