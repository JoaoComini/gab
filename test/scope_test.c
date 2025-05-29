#include "arena.h"
#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type.h"
#include <assert.h>

Arena *arena = NULL;

static void test_create_and_free() {
    Scope *scope = scope_create(arena, NULL);
    assert(scope->symbol_table != NULL);
    assert(scope->parent == NULL);
}

static void test_nested_scopes() {
    Scope *parent = scope_create(arena, NULL);
    Scope *child = scope_create(arena, parent);

    assert(child->parent == parent);
}

static void test_var_declaration() {
    Scope *scope = scope_create(arena, NULL);
    String *name = string_from_cstr("x");
    Type *type = type_create(arena, TYPE_INT, string_from_cstr("int"));

    Symbol *sym = scope_decl_var(scope, name, type);
    assert(sym != NULL);

    // Check lookup
    Symbol *found = scope_symbol_lookup(scope, name);
    assert(found == sym);
}

static void test_shadowing() {
    Scope *parent = scope_create(arena, NULL);

    String *name = string_from_cstr("x");
    Type *int_type = type_create(arena, TYPE_INT, string_from_cstr("int"));
    Type *float_type = type_create(arena, TYPE_FLOAT, string_from_cstr("float"));

    // Declare in parent
    Symbol *parent_sym = scope_decl_var(parent, name, int_type);

    Scope *child = scope_create(arena, parent);

    // Declare same name in child
    Symbol *child_sym = scope_decl_var(child, name, float_type);

    // Lookup in child should find child's version
    assert(scope_symbol_lookup(child, name) == child_sym);
}

int main(void) {
    string_init();
    arena = arena_create(128);

    test_create_and_free();
    test_nested_scopes();
    test_var_declaration();
    test_shadowing();

    arena_destroy(arena);
    string_deinit();

    return 0;
}
