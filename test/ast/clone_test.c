#include "ast/ast.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static const ASTStmt *find_func(const ASTStmtList *list, const char *name) {
    for (size_t i = 0; i < list->size; i++) {
        const ASTStmt *stmt = list->data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL &&
            strncmp(stmt->func_decl.name.data, name, stmt->func_decl.name.length) == 0) {
            return stmt;
        }
    }

    return NULL;
}

static void test_an_instance_shares_the_type_expressions_it_was_cloned_from() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Holder<T> { held: T }\n"
                           "func Holder<T>::get(h: &Holder<T>): T { return h.held; }\n"
                           "func main(): int { let h = Holder<int> { held: 7 }; return h.get(); }\n");
    assert(ok);

    const ASTStmt *generic = find_func(&unit->statements, "get");
    assert(generic);

    assert(unit->instances.size == 1);
    const ASTStmt *instance = unit->instances.data[0];

    assert(instance != generic);
    assert(instance->func_decl.return_type == generic->func_decl.return_type);

    assert(instance->func_decl.params.size == generic->func_decl.params.size);
    for (size_t i = 0; i < generic->func_decl.params.size; i++) {
        assert(instance->func_decl.params.data[i]->type_expr == generic->func_decl.params.data[i]->type_expr);
    }

    test_context_free(&ctx);
}

int main() {
    test_an_instance_shares_the_type_expressions_it_was_cloned_from();

    printf("All clone tests passed\n");

    return 0;
}
