#include "decl.h"
#include "memory/arena.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "type/type.h"
#include <assert.h>

static TestContext ctx;
static Arena *arena = NULL;

static void test_create_and_free() {
    Scope *scope = scope_create(arena, NULL);
    assert(scope->symbols != NULL);
    assert(scope->parent == NULL);
}

static void test_nested_scopes() {
    Scope *parent = scope_create(arena, NULL);
    Scope *child = scope_create(arena, parent);

    assert(child->parent == parent);
}

static void test_var_declaration() {
    Scope *scope = scope_create_kind(arena, ctx.global, SCOPE_MODULE);
    String *name = string_from_cstr(&ctx.strings, "x");
    const Type *type = type_registry_get_primitive(ctx.types, TYPE_I32);

    Symbol *sym = scope_decl_var(scope, name, type);
    assert(sym != NULL);

    Symbol *found = scope_lookup(scope, name);
    assert(found == sym);
}

/* A module's declarations are its files', so a redeclaration in one file collides with the other's. */
static void a_name_a_module_declares_collides_across_its_files() {
    Scope *global = scope_create_kind(arena, ctx.global, SCOPE_MODULE);

    Scope *module = scope_create(arena, global);
    scope_init_kind(module, arena, global, SCOPE_MODULE);

    Scope *file = scope_create(arena, module);
    scope_init_kind(file, arena, module, SCOPE_FILE);

    String *name = string_from_cstr(&ctx.strings, "shared");
    const Type *type = type_registry_get_primitive(ctx.types, TYPE_I32);

    assert(scope_decl_var(module, name, type));
    assert(!scope_decl_var(file, name, type));
}

/* An 'impl' names what its own module declares, so the walk stops where the module does. */
static void a_type_lookup_that_declares_stops_at_the_module() {
    Scope *global = scope_create_kind(arena, ctx.global, SCOPE_MODULE);

    Scope *module = scope_create(arena, global);
    scope_init_kind(module, arena, global, SCOPE_MODULE);

    Scope *file = scope_create(arena, module);
    scope_init_kind(file, arena, module, SCOPE_FILE);

    String *i32 = string_from_cstr(&ctx.strings, "i32");

    assert(scope_type_lookup(ctx.types, file, i32));
    assert(!scope_type_lookup_declaring(file, i32));
}

static void test_shadowing() {
    Scope *parent = scope_create_kind(arena, ctx.global, SCOPE_MODULE);

    String *name = string_from_cstr(&ctx.strings, "x");
    const Type *i32_type = type_registry_get_primitive(ctx.types, TYPE_I32);
    const Type *f32_type = type_registry_get_primitive(ctx.types, TYPE_F32);

    Symbol *parent_sym = scope_decl_var(parent, name, i32_type);

    Scope *child = scope_create(arena, parent);

    Symbol *child_sym = scope_decl_var(child, name, f32_type);

    assert(scope_lookup(child, name) == child_sym);

    assert(child_sym != parent_sym);
    assert(scope_lookup(parent, name) == parent_sym);
}

int main(void) {
    test_context_init(&ctx);
    arena = ctx.arena;

    test_create_and_free();
    test_nested_scopes();
    test_var_declaration();
    a_name_a_module_declares_collides_across_its_files();
    a_type_lookup_that_declares_stops_at_the_module();
    test_shadowing();

    test_context_free(&ctx);

    return 0;
}
