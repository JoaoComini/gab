#include "mir/mir_drop.h"
#include "mir/mir_lower.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTModule *unit;
    ResolvedModule *resolved;
} Elaborated;

static MIRFunction *elaborate(Elaborated *out, const char *source, const char *name) {
    test_context_init(&out->ctx);

    out->scope = scope_create_kind(out->ctx.arena, out->ctx.global, SCOPE_MODULE);
    out->unit = ast_module_create(out->ctx.arena);

    assert(test_resolve_ir(&out->ctx, out->scope, &out->unit, NULL, &out->resolved, source));

    size_t length = strlen(name);

    for (size_t i = 0; i < ast_module_statements(out->unit)[0].size; i++) {
        ASTStmt *stmt = ast_module_statements(out->unit)[0].data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        if (stmt->func_decl.name.length != length || strncmp(stmt->func_decl.name.data, name, length) != 0) {
            continue;
        }

        MIRFunction *ir =
            mir_build_function(out->ctx.arena, out->ctx.types, &out->resolved->facts,
                               stmt->func_decl.function, &stmt->func_decl.params, stmt->func_decl.body);

        mir_drop_elaborate(out->ctx.arena, out->ctx.types, out->ctx.functions, ir);

        return ir;
    }

    assert(false && "the unit declares no such function");

    return NULL;
}

static void elaborated_free(Elaborated *out) { test_context_free(&out->ctx); }

static size_t count_op(const MIRFunction *ir, MIROp op) {
    size_t count = 0;

    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            count += ir->blocks[i]->insts[j].op == op;
        }
    }

    return count;
}

static void test_a_value_owning_nothing_is_never_marked_for_dropping(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out, "func f(): i32 { let a: i32 = 1; return a; }", "f");

    assert(count_op(ir, MIR_DROP) == 0);

    elaborated_free(&out);
}

static void test_a_drop_of_an_owning_value_survives(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func f(): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    return a.n;\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 1);

    elaborated_free(&out);
}

static void test_an_owned_temporary_is_dropped_where_it_is_last_read(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func f(): i32 { return (box Node { n: 1 }).n; }\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 1);

    elaborated_free(&out);
}

/* Giving a value away leaves its slot holding nothing, so the drop it reaches frees nothing. */
static void test_a_move_nulls_what_it_gave_away(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func take(p: *Node): i32 { return p.n; }\n"
                                "func f(): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    return take(a);\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_NULL) == 1);

    elaborated_free(&out);
}

/* A null follows the move that needs it, so nothing reads the slot in between. */
static void test_a_null_follows_the_move_it_answers_for(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func take(p: *Node): i32 { return p.n; }\n"
                                "func f(): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    return take(a);\n"
                                "}\n",
                                "f");

    bool found = false;

    for (size_t i = 0; i < ir->block_count; i++) {
        const MIRBlock *block = ir->blocks[i];

        for (size_t j = 0; j + 1 < block->inst_count; j++) {
            if (block->insts[j].op != MIR_LOAD || block->insts[j].read != READ_MOVE) {
                continue;
            }

            assert(block->insts[j + 1].op == MIR_NULL);
            assert(block->insts[j + 1].place.base.id == block->insts[j].place.base.id);

            found = true;
        }
    }

    assert(found);

    elaborated_free(&out);
}

/* A value moved on one path reaches the same drop as one that was not, so the null decides at runtime. */
static void test_a_value_moved_on_one_path_still_reaches_one_drop(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func take(p: *Node): i32 { return p.n; }\n"
                                "func f(c: bool): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    if c { let x: i32 = take(a); }\n"
                                "    return 0;\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 1);

    elaborated_free(&out);
}

static void test_a_move_of_a_value_owning_nothing_needs_no_null(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out, "func f(): i32 { let a: i32 = 1; let b: i32 = a; return b; }", "f");

    assert(count_op(ir, MIR_NULL) == 0);

    elaborated_free(&out);
}

/* A store over an owning field reads the old value out first, so the release still names it. */
static void a_call_result_stored_into_a_field_releases_what_it_replaced(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Inner { n: i32 }\n"
                                "struct Outer { child: *Inner }\n"
                                "func made(): *Inner { return box Inner { n: 6 }; }\n"
                                "func f(): i32 {\n"
                                "    let o: *Outer = box Outer { child: box Inner { n: 1 } };\n"
                                "    o.child = made();\n"
                                "    return 0;\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 2);

    elaborated_free(&out);
}

static void a_value_moved_on_every_path_reaches_no_drop(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func take(p: *Node): i32 { return p.n; }\n"
                                "func f(c: bool): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    if c { let x: i32 = take(a); } else { let y: i32 = take(a); }\n"
                                "    return 0;\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 0);

    elaborated_free(&out);
}

/* Paths that disagree are told apart by a flag, not by what the slot itself was left holding. */
static void a_value_moved_on_one_path_is_dropped_behind_a_flag(void) {
    Elaborated out;
    MIRFunction *ir = elaborate(&out,
                                "struct Node { n: i32 }\n"
                                "func take(p: *Node): i32 { return p.n; }\n"
                                "func f(c: bool): i32 {\n"
                                "    let a: *Node = box Node { n: 1 };\n"
                                "    if c { let x: i32 = take(a); }\n"
                                "    return 0;\n"
                                "}\n",
                                "f");

    assert(count_op(ir, MIR_DROP) == 1);
    assert(count_op(ir, MIR_DROP_FLAG) > 0);

    elaborated_free(&out);
}

int main(void) {
    test_a_value_owning_nothing_is_never_marked_for_dropping();
    test_a_drop_of_an_owning_value_survives();
    test_an_owned_temporary_is_dropped_where_it_is_last_read();
    test_a_move_nulls_what_it_gave_away();
    test_a_null_follows_the_move_it_answers_for();
    test_a_value_moved_on_one_path_still_reaches_one_drop();
    test_a_move_of_a_value_owning_nothing_needs_no_null();
    a_call_result_stored_into_a_field_releases_what_it_replaced();

    printf("mir_drop_test passed\n");

    a_value_moved_on_every_path_reaches_no_drop();

    a_value_moved_on_one_path_is_dropped_behind_a_flag();

    return 0;
}
