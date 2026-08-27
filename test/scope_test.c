#include "arena.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "symbol_table.h"
#include "type.h"
#include <assert.h>

static TestContext ctx;
static Arena *arena = NULL;

static void test_create_and_free() {
    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    assert(scope->symbol_table != NULL);
    assert(scope->parent == NULL);
}

static void test_nested_scopes() {
    Scope *parent = scope_create(arena, &ctx.strings, NULL);
    Scope *child = scope_create(arena, &ctx.strings, parent);

    assert(child->parent == parent);
}

static void test_var_declaration() {
    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    String *name = string_from_cstr(&ctx.strings, "x");
    TypeHandle type = type_registry_get_builtin(scope->type_registry, TYPE_INT);

    Symbol *sym = scope_decl_var(scope, name, type);
    assert(sym != NULL);

    // Check lookup
    Symbol *found = scope_symbol_lookup(scope, name);
    assert(found == sym);
}

static void test_shadowing() {
    Scope *parent = scope_create(arena, &ctx.strings, NULL);

    String *name = string_from_cstr(&ctx.strings, "x");
    TypeHandle int_type = type_registry_get_builtin(parent->type_registry, TYPE_INT);
    TypeHandle float_type = type_registry_get_builtin(parent->type_registry, TYPE_FLOAT);

    // Declare in parent
    Symbol *parent_sym = scope_decl_var(parent, name, int_type);

    Scope *child = scope_create(arena, &ctx.strings, parent);

    // Declare same name in child
    Symbol *child_sym = scope_decl_var(child, name, float_type);

    // Lookup in child should find child's version
    assert(scope_symbol_lookup(child, name) == child_sym);

    // Shadowing hides the parent's symbol without replacing it: the parent
    // still resolves the name to its own, which is what makes the shadow end
    // at the closing brace.
    assert(child_sym != parent_sym);
    assert(scope_symbol_lookup(parent, name) == parent_sym);
}

int main(void) {
    test_context_init(&ctx);
    arena = ctx.arena;

    test_create_and_free();
    test_nested_scopes();
    test_var_declaration();
    test_shadowing();

    test_context_free(&ctx);

    return 0;
}
