#include "support/test_context.h"
#include "symbol_table.h"
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

    const Type *instance = type_registry_apply(registry, &def, &int_type, 1);

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

    const Type *type = type_registry_apply(registry, &def, NULL, 0);

    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_field_count(type) == 1);
    assert(type_fields(type)[0].type == int_type);

    // Interned like any other application, so a second mention is the same type
    // rather than a second one laid out alike.
    assert(type_registry_apply(registry, &def, NULL, 0) == type);

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

    assert(type_registry_apply(registry, &first, NULL, 0) != type_registry_apply(registry, &second, NULL, 0));

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

    const Type *type = type_registry_apply(registry, &def, NULL, 0);

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
    assert(type_registry_apply(registry, &def, NULL, 0) == type);

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

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    assert(type_fields(of_int)[0].type == type_registry_block_of(registry, int_type));
    assert(type_fields(of_bool)[0].type == type_registry_block_of(registry, bool_type));

    // The declaration still says what it said, over the parameter.
    assert(def.fields[0].type == type_registry_block_of(registry, type_registry_param(registry, 0)));

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A type is a declaration applied to arguments, and it says which arguments:
// substituting what the declaration wrote needs them at every use, where a type
// is all a reader holds.
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

    // A plain struct is a declaration applied to nothing, and answers so.
    TypeDef plain = {.name = string_from_cstr(&ctx.strings, "Point")};

    assert(type_arg_count(type_registry_apply(registry, &plain, NULL, 0)) == 0);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A method is declared on a declaration, and every instantiation answers it
// with that declaration's parameters filled in by its own arguments. One
// statement of 'at' therefore answers 'int' for one instantiation and 'bool'
// for another.
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

    // 'at(self) -> T', over the parameter: what comes back is whatever the
    // instantiation was applied to.
    GenericMethod method = {
        .name = at,
        .receiver = type_registry_apply(registry, &def, &param, 1),
        .result = param,
    };

    def.methods = &method;
    def.method_count = 1;

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    const Symbol *from_int = type_registry_find_method(registry, of_int, at);
    const Symbol *from_bool = type_registry_find_method(registry, of_bool, at);

    assert(from_int && from_bool);

    assert(from_int->func.return_type == int_type);
    assert(from_bool->func.return_type == bool_type);

    // The receiver is parameter zero, and is this instantiation rather than the
    // declaration's own.
    assert(from_int->func.params[0] == of_int);
    assert(from_bool->func.params[0] == of_bool);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A method reaches an instantiation that already existed when it was declared.
// It hangs on the declaration, so what answers a call is read when the call is
// made rather than copied at every instantiation -- and one interned before the
// declaration said anything is not left behind.
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

    GenericMethod method = {
        .name = at,
        .receiver = type_registry_apply(registry, &def, &param, 1),
        .result = param,
    };

    def.methods = &method;
    def.method_count = 1;

    const Symbol *found = type_registry_find_method(registry, of_int, at);

    assert(found);
    assert(found->func.return_type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A name its declaration states is taken for every instantiation of it. What
// answers a call is one method, so declaring a second on one instantiation is
// refused rather than shadowing what the declaration says for the rest.
static void test_a_declared_method_takes_the_name_on_every_instantiation() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &names);

    const Type *param = type_registry_param(registry, 0);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *at = string_from_cstr(&ctx.strings, "at");

    TypeDef def = {.name = string_from_cstr(&ctx.strings, "Holder"), .param_count = 1};

    GenericMethod method = {
        .name = at,
        .receiver = type_registry_apply(registry, &def, &param, 1),
        .result = param,
    };

    def.methods = &method;
    def.method_count = 1;

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    Symbol other = {0};

    assert(!type_registry_add_method(registry, of_int, at, &other));

    // And what answers is still the declaration's, read under this
    // instantiation's argument.
    assert(type_registry_find_method(registry, of_int, at)->func.return_type == int_type);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// A method declared on one instantiation is declared on the declaration, so
// every instantiation of it answers the same one. What a declaration states is
// stated once however it was reached, since an instantiation is that
// declaration applied to arguments rather than a type of its own.
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

    Symbol method = {0};

    assert(type_registry_add_method(registry, of_int, name, &method));

    assert(type_registry_find_method(registry, of_bool, name) == &method);

    // And it is one statement, so a second on the other instantiation is a
    // collision rather than that instantiation's own.
    assert(!type_registry_add_method(registry, of_bool, name, &method));

    // A second declaration is untouched: the name is taken on this one, not on
    // every type that happens to be a struct.
    TypeDef other = {.name = string_from_cstr(&ctx.strings, "Other"), .param_count = 1};

    assert(type_registry_find_method(registry, type_registry_apply(registry, &other, &int_type, 1), name) ==
           NULL);

    type_registry_destroy(registry);
    string_pool_free(&ctx.strings);
    arena_destroy(ctx.arena);
}

// One type reads one signature. A substituted signature is derived rather than
// stated, so asking twice must hand back what the first ask built: a caller
// comparing methods by identity -- and codegen, which reserves a body slot per
// Symbol -- would otherwise see two methods where the declaration states one.
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

    GenericMethod method = {
        .name = at,
        .receiver = type_registry_apply(registry, &def, &param, 1),
        .result = param,
    };

    def.methods = &method;
    def.method_count = 1;

    const Type *of_int = type_registry_apply(registry, &def, &int_type, 1);

    assert(type_registry_find_method(registry, of_int, at) ==
           type_registry_find_method(registry, of_int, at));

    // And a second instantiation reads its own, since the two say different
    // things about what comes back.
    const Type *of_bool = type_registry_apply(registry, &def, &bool_type, 1);

    assert(type_registry_find_method(registry, of_int, at) !=
           type_registry_find_method(registry, of_bool, at));

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
    test_an_instantiation_carries_its_arguments();
    test_a_declared_method_is_substituted_per_instantiation();
    test_a_method_reaches_an_instantiation_interned_before_it();
    test_a_declared_method_takes_the_name_on_every_instantiation();
    test_a_method_declared_on_one_instantiation_answers_on_every_one();
    test_a_substituted_signature_is_read_once_per_type();
    return 0;
}
