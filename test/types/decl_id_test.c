#include "decl_id.h"
#include "support/run.h"
#include "type/type_registry.h"

#include <assert.h>

static DeclId method_id(TestContext *ctx, const char *source, const char *type, const char *name) {
    Scope *scope = scope_create(ctx->arena, &ctx->strings, NULL);
    ASTUnit *unit;
    ResolvedUnit *resolved;

    bool ok = test_resolve_ir(ctx, scope, &unit, NULL, &resolved, source);
    assert(ok);

    const Type *owner = scope_type_lookup(scope, string_from_cstr(&ctx->strings, type));
    assert(owner);

    Function *found =
        function_registry_find_owned(resolved->functions, owner, string_from_cstr(&ctx->strings, name));
    assert(found);

    return found->decl->id;
}

static void a_method_is_named_the_same_whatever_precedes_its_impl(void) {
    TestContext ctx;
    test_context_init(&ctx);

    DeclId alone = method_id(&ctx,
                             "struct Cup { n: i32 }\n"
                             "impl Cup {\n"
                             "    func count(self: &Self): i32 { return self.n; }\n"
                             "}\n",
                             "Cup", "count");

    DeclId preceded = method_id(&ctx,
                                "struct Other { n: i32 }\n"
                                "impl Other {\n"
                                "    func count(self: &Self): i32 { return self.n; }\n"
                                "}\n"
                                "struct Cup { n: i32 }\n"
                                "impl Cup {\n"
                                "    func count(self: &Self): i32 { return self.n; }\n"
                                "}\n",
                                "Cup", "count");

    assert(decl_id_is_set(alone));
    assert(decl_id_equals(alone, preceded));

    test_context_free(&ctx);
}

int main(void) {
    a_method_is_named_the_same_whatever_precedes_its_impl();

    return 0;
}
