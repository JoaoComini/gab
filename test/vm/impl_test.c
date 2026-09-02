#include "ast/ast.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type/type.h"

#include <assert.h>
#include <stdbool.h>

static const Type *lookup_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static Function *lookup_method(TestContext *ctx, Scope *scope, const char *type, const char *method) {
    return type_registry_find_owned(scope->type_registry, lookup_type(ctx, scope, type),
                                    string_from_cstr(&ctx->strings, method));
}

static bool resolves(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve(&ctx, scope, unit, source);

    test_context_free(&ctx);

    return ok;
}

static void test_an_impl_block_owns_its_members() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "impl Player {\n"
                           "    func damage(p: &Player, n: int): int { return p.health - n; }\n"
                           "}\n");
    assert(ok);

    Function *damage = lookup_method(&ctx, scope, "Player", "damage");

    assert(damage);
    assert(damage->param_count == 2);
    assert(type_pointee(damage->params[0]) == lookup_type(&ctx, scope, "Player"));

    test_context_free(&ctx);
}

static void test_an_impl_member_is_not_a_module_level_name() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "impl Player {\n"
                           "    func damage(p: &Player): int { return p.health; }\n"
                           "}\n");
    assert(ok);

    assert(!scope_binding_lookup(scope, string_from_cstr(&ctx.strings, "damage")));

    test_context_free(&ctx);
}

static void test_an_impl_block_holds_more_than_one_member() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "impl Player {\n"
                           "    func hp(p: &Player): int { return p.health; }\n"
                           "    func alive(p: &Player): bool { return p.health > 0; }\n"
                           "}\n");
    assert(ok);

    assert(lookup_method(&ctx, scope, "Player", "hp"));
    assert(lookup_method(&ctx, scope, "Player", "alive"));

    test_context_free(&ctx);
}

static void test_an_impl_block_precedes_the_struct_it_names() {
    assert(resolves("impl Player {\n"
                    "    func hp(p: &Player): int { return p.health; }\n"
                    "}\n"
                    "struct Player { health: int }\n"));
}

static void test_a_member_is_callable_above_the_impl_that_declares_it() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p = Player { health: 12 };\n"
                        "    return p.hp();\n"
                        "}\n"
                        "impl Player {\n"
                        "    func hp(p: &Player): int { return p.health; }\n"
                        "}\n"
                        "let r: int = main();") == 12);
}

static void test_an_impl_block_names_a_qualified_type() {
    assert(!resolves("impl Other::Thing {\n"
                     "    func hp(t: &Other::Thing): int { return 0; }\n"
                     "}\n"));
}

static void test_an_impl_names_a_type_the_module_declares() {
    assert(!resolves("impl Missing {\n"
                     "    func hp(p: &Missing): int { return 0; }\n"
                     "}\n"));
}

static void test_a_primitive_impl_needs_the_compilation_s_permission() {
    assert(!resolves("impl int {\n"
                     "    extern func double(n: int): int;\n"
                     "}\n"));
}

static void test_one_type_declares_a_name_once_across_its_impls() {
    assert(!resolves("struct Player { health: int }\n"
                     "impl Player {\n"
                     "    func hp(p: &Player): int { return p.health; }\n"
                     "    func hp(p: &Player): int { return 0; }\n"
                     "}\n"));
}

static void test_an_impl_member_dispatches_on_its_receiver() {
    assert(test_run_int(
               "struct Player { health: int }\n"
               "impl Player {\n"
               "    func damage(p: &Player, n: int): int { p.health = p.health - n; return p.health; }\n"
               "}\n"
               "func main(): int {\n"
               "    let p = Player { health: 100 };\n"
               "    let ignored: int = p.damage(30);\n"
               "    return p.health;\n"
               "}\n"
               "let r: int = main();") == 70);
}

static void test_an_impl_binds_its_type_parameters_for_every_member() {
    assert(test_run_int("struct Box<T> { value: T }\n"
                        "impl<T> Box<T> {\n"
                        "    func get(b: &Box<T>): T { return b.value; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let b = Box<int> { value: 7 };\n"
                        "    return b.get();\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

int main(void) {
    test_an_impl_block_owns_its_members();
    test_an_impl_member_is_not_a_module_level_name();
    test_an_impl_block_holds_more_than_one_member();
    test_an_impl_block_precedes_the_struct_it_names();
    test_a_member_is_callable_above_the_impl_that_declares_it();
    test_an_impl_block_names_a_qualified_type();
    test_an_impl_names_a_type_the_module_declares();
    test_a_primitive_impl_needs_the_compilation_s_permission();
    test_one_type_declares_a_name_once_across_its_impls();
    test_an_impl_member_dispatches_on_its_receiver();
    test_an_impl_binds_its_type_parameters_for_every_member();

    return 0;
}
