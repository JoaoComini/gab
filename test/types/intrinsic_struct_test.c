#include "scope.h"
#include "support/run.h"
#include "type/type_registry.h"

#include <assert.h>

static void an_intrinsic_struct_names_one_the_compiler_knows(void) {
    assert(test_core_diagnostic_mentions("intrinsic struct Bag<T> { ptr: raw<T> }",
                                         "no intrinsic struct 'Bag'"));
}

static void the_core_declares_the_intrinsic_struct_the_compiler_knows(void) {
    assert(
        !test_core_diagnostic_mentions("intrinsic struct Unique<T> { ptr: raw<T> }", "no intrinsic struct"));
}

/* The type a 'Unique' names is the one that owns what it points at, which is what its drop is written on. */
static void a_unique_owns_what_it_points_at(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);
    ASTModule *unit;
    MIRModule *bodies;
    ResolvedModule *resolved;

    bool ok = test_resolve_ir_with(&ctx, scope, &unit, &bodies, &resolved,
                                   "intrinsic struct Unique<T> { ptr: raw<T> }\n"
                                   "struct Plain { ptr: raw<i32> }",
                                   true);
    assert(ok);

    TypeRegistry *registry = ctx.types;

    Symbol *declared = scope_lookup(resolved->declared->scope, type_registry_names(registry)->unique);

    assert(declared && declared->kind == SYMBOL_TYPE_DECL);

    const Type *i32 = type_registry_get_primitive(registry, TYPE_I32);
    const Type *unique = type_registry_apply(registry, declared->type_decl, &i32, 1);
    const Type *plain =
        scope_type_lookup(ctx.types, resolved->declared->scope, string_from_cstr(&ctx.strings, "Plain"));

    assert(unique && plain);

    assert(type_registry_is_unique(registry, unique));
    assert(!type_registry_is_unique(registry, plain));

    assert(type_registry_owns(registry, unique));
    assert(!type_registry_copies(registry, unique));

    /* A plain struct over the same field owns nothing, so the run alone is not what makes an owner. */
    assert(!type_registry_owns(registry, plain));

    test_context_free(&ctx);
}

int main(void) {
    an_intrinsic_struct_names_one_the_compiler_knows();
    the_core_declares_the_intrinsic_struct_the_compiler_knows();
    a_unique_owns_what_it_points_at();

    return 0;
}
