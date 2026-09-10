#include "mir/mir_print.h"
#include "support/test_context.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIR_TEST_ARENA_BLOCK_SIZE 4096

static void assert_prints(const MIRFunction *ir, const char *expected) {
    char buffer[2048];

    size_t written = mir_print_to_buffer(ir, NULL, buffer, sizeof(buffer));

    assert(written < sizeof(buffer));

    if (strcmp(buffer, expected) != 0) {
        fprintf(stderr, "expected:\n%s\ngot:\n%s\n", expected, buffer);
        assert(false);
    }
}

static void test_an_empty_function_prints_its_entry_block(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);

    mir_emit(ir, mir_block_at(ir, ir->entry), (MIRInst){.op = MIR_RETURN, .result = MIR_NO_VALUE});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    return\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_an_instruction_prints_its_result_and_arguments(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *block = mir_block_at(ir, ir->entry);

    MIRValueId left = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId right = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId sum = mir_value_create(ir, NULL, NULL, (Span){0});

    mir_emit(ir, block, (MIRInst){.op = MIR_CONST_INT, .result = left, .constant = {.as_int = 1}});
    mir_emit(ir, block, (MIRInst){.op = MIR_CONST_INT, .result = right, .constant = {.as_int = 2}});

    MIROperand *args = mir_args_alloc(ir, 2);
    args[0] = mir_operand_value(left);
    args[1] = mir_operand_value(right);

    mir_emit(ir, block, (MIRInst){.op = MIR_ADD, .result = sum, .args = args, .arg_count = 2});
    mir_emit(ir, block, (MIRInst){.op = MIR_RETURN, .result = MIR_NO_VALUE, .args = args, .arg_count = 1});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    %0: ? = const.int 1\n"
                      "    %1: ? = const.int 2\n"
                      "    %2: ? = add %0, %1\n"
                      "    return %0\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_a_comparison_prints_its_predicate(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *block = mir_block_at(ir, ir->entry);

    MIRValueId operand = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId result = mir_value_create(ir, NULL, NULL, (Span){0});

    MIROperand *args = mir_args_alloc(ir, 1);
    args[0] = mir_operand_value(operand);

    mir_emit(
        ir, block,
        (MIRInst){.op = MIR_CMP, .result = result, .predicate = MIR_CMP_LE, .args = args, .arg_count = 1});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    %1: ? = cmp.le %0\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_a_place_prints_the_path_taken_through_its_base(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *block = mir_block_at(ir, ir->entry);

    MIRValueId base = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId index = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId loaded = mir_value_create(ir, NULL, NULL, (Span){0});

    Place place = mir_place_of(base, NULL);

    place = mir_place_project(ir, place, (Projection){.kind = PROJ_DEREF});
    place = mir_place_project(ir, place, (Projection){.kind = PROJ_FIELD, .field = {2}});
    place = mir_place_project(ir, place, (Projection){.kind = PROJ_INDEX, .index = index});

    mir_emit(ir, block, (MIRInst){.op = MIR_LOAD, .result = loaded, .place = place});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    %2: ? = load %0.*.2[%1]\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_projecting_leaves_the_shorter_place_alone(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);

    MIRValueId base = mir_value_create(ir, NULL, NULL, (Span){0});

    Place outer = mir_place_of(base, NULL);
    Place inner = mir_place_project(ir, outer, (Projection){.kind = PROJ_FIELD, .field = {0}});

    Place other = mir_place_project(ir, inner, (Projection){.kind = PROJ_FIELD, .field = {1}});
    Place sibling = mir_place_project(ir, inner, (Projection){.kind = PROJ_FIELD, .field = {7}});

    assert(outer.projection_count == 0);
    assert(inner.projection_count == 1);

    assert(other.projections[1].field.id == 1);
    assert(sibling.projections[1].field.id == 7);

    arena_destroy(arena);
}

static void test_a_store_prints_its_place_before_its_value(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *block = mir_block_at(ir, ir->entry);

    MIRValueId base = mir_value_create(ir, NULL, NULL, (Span){0});
    MIRValueId value = mir_value_create(ir, NULL, NULL, (Span){0});

    MIROperand *args = mir_args_alloc(ir, 1);
    args[0] = mir_operand_value(value);

    Place place =
        mir_place_project(ir, mir_place_of(base, NULL), (Projection){.kind = PROJ_FIELD, .field = {1}});

    mir_emit(
        ir, block,
        (MIRInst){.op = MIR_STORE, .result = MIR_NO_VALUE, .place = place, .args = args, .arg_count = 1});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    store %0.1, %1\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_a_branch_names_both_of_its_targets(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *entry = mir_block_at(ir, ir->entry);

    MIRBlock *then_block = mir_block_create(ir);
    MIRBlock *else_block = mir_block_create(ir);

    MIRValueId condition = mir_value_create(ir, NULL, NULL, (Span){0});

    MIROperand *args = mir_args_alloc(ir, 1);
    args[0] = mir_operand_value(condition);

    mir_emit(ir, entry,
             (MIRInst){.op = MIR_BRANCH,
                       .result = MIR_NO_VALUE,
                       .args = args,
                       .arg_count = 1,
                       .targets = {then_block->id, else_block->id}});

    mir_emit(ir, then_block, (MIRInst){.op = MIR_JMP, .result = MIR_NO_VALUE, .targets = {else_block->id}});
    mir_emit(ir, else_block, (MIRInst){.op = MIR_UNREACHABLE, .result = MIR_NO_VALUE});

    assert_prints(ir, "func ?() {\n"
                      "  bb0:\n"
                      "    branch %0, bb1, bb2\n"
                      "  bb1:\n"
                      "    jmp bb2\n"
                      "  bb2:\n"
                      "    unreachable\n"
                      "}\n");

    arena_destroy(arena);
}

static void test_a_terminator_names_the_blocks_it_reaches(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);
    MIRBlock *entry = mir_block_at(ir, ir->entry);

    MIRBlock *then_block = mir_block_create(ir);
    MIRBlock *else_block = mir_block_create(ir);

    MIRBlockId successors[2];

    assert(!mir_block_is_terminated(entry));
    assert(mir_block_successors(entry, successors) == 0);

    mir_emit(
        ir, entry,
        (MIRInst){.op = MIR_BRANCH, .result = MIR_NO_VALUE, .targets = {then_block->id, else_block->id}});

    assert(mir_block_is_terminated(entry));
    assert(mir_block_successors(entry, successors) == 2);
    assert(successors[0].id == then_block->id.id);
    assert(successors[1].id == else_block->id.id);

    mir_emit(ir, then_block, (MIRInst){.op = MIR_JMP, .result = MIR_NO_VALUE, .targets = {else_block->id}});

    assert(mir_block_successors(then_block, successors) == 1);
    assert(successors[0].id == else_block->id.id);

    mir_emit(ir, else_block, (MIRInst){.op = MIR_RETURN, .result = MIR_NO_VALUE});

    assert(mir_block_is_terminated(else_block));
    assert(mir_block_successors(else_block, successors) == 0);

    arena_destroy(arena);
}

static void test_a_value_carries_the_binding_it_came_from(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);

    Symbol binding = {.kind = SYMBOL_VAR};

    MIRValueId value = mir_value_create(ir, NULL, &binding, (Span){.line = 7, .column = 3});

    const MIRValueInfo *info = mir_value_info(ir, value);

    assert(info->binding == &binding);
    assert(info->span.line == 7);

    assert(mir_value_info(ir, MIR_NO_VALUE) == NULL);

    arena_destroy(arena);
}

static void test_a_printed_function_reports_what_it_would_take(void) {
    Arena *arena = arena_create(MIR_TEST_ARENA_BLOCK_SIZE);

    MIRFunction *ir = mir_function_create(arena, NULL, NULL);

    mir_emit(ir, mir_block_at(ir, ir->entry), (MIRInst){.op = MIR_RETURN, .result = MIR_NO_VALUE});

    char small[8];

    size_t written = mir_print_to_buffer(ir, NULL, small, sizeof(small));

    assert(written > sizeof(small));
    assert(small[sizeof(small) - 1] == '\0');

    arena_destroy(arena);
}

int main(void) {
    test_an_empty_function_prints_its_entry_block();
    test_an_instruction_prints_its_result_and_arguments();
    test_a_comparison_prints_its_predicate();
    test_a_place_prints_the_path_taken_through_its_base();
    test_projecting_leaves_the_shorter_place_alone();
    test_a_store_prints_its_place_before_its_value();
    test_a_branch_names_both_of_its_targets();
    test_a_terminator_names_the_blocks_it_reaches();
    test_a_value_carries_the_binding_it_came_from();
    test_a_printed_function_reports_what_it_would_take();

    printf("mir_print_test passed\n");

    return 0;
}
