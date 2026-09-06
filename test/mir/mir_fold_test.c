#include "mir/mir_fold.h"
#include "support/emit.h"

#include <assert.h>
#include <stdio.h>

static size_t count_op(const MIRFunction *ir, MIROp op) {
    size_t count = 0;

    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            count += ir->blocks[i]->insts[j].op == op;
        }
    }

    return count;
}

static void negating_a_literal_leaves_no_negation(void) {
    TestEmission emission = test_lower_ir("func one(): int { return -42; }\n");

    assert(count_op(emission.ir, MIR_NEG) == 0);
    assert(count_op(emission.ir, MIR_CONST_INT) == 1);

    test_emission_free(&emission);
}

static void adding_two_literals_leaves_no_addition(void) {
    TestEmission emission = test_lower_ir("func one(): int { return 2 + 3; }\n");

    assert(count_op(emission.ir, MIR_ADD) == 0);
    assert(count_op(emission.ir, MIR_CONST_INT) == 1);

    test_emission_free(&emission);
}

static void a_nested_constant_expression_folds_wholly(void) {
    TestEmission emission = test_lower_ir("func one(): int { return 2 + 3 * 4; }\n");

    assert(count_op(emission.ir, MIR_ADD) == 0);
    assert(count_op(emission.ir, MIR_MUL) == 0);
    assert(count_op(emission.ir, MIR_CONST_INT) == 1);

    test_emission_free(&emission);
}

static void adding_two_float_literals_leaves_no_addition(void) {
    TestEmission emission = test_lower_ir("func one(): float { return 1.5 + 2.0; }\n");

    assert(count_op(emission.ir, MIR_ADD) == 0);

    test_emission_free(&emission);
}

static void a_division_by_zero_keeps_its_division(void) {
    TestEmission emission = test_lower_ir("func one(): int { return 1 / 0; }\n");

    assert(count_op(emission.ir, MIR_DIV) == 1);

    test_emission_free(&emission);
}

static void an_operand_no_instruction_reads_is_dropped(void) {
    TestEmission emission = test_lower_ir("func one(): int { return -42; }\n");

    assert(count_op(emission.ir, MIR_CONST_INT) == 1);

    test_emission_free(&emission);
}

static void a_variable_operand_keeps_its_operation(void) {
    TestEmission emission = test_lower_ir("func one(a: int): int { return a + 3; }\n");

    assert(count_op(emission.ir, MIR_ADD) == 1);

    test_emission_free(&emission);
}

static void an_index_a_place_reads_is_not_dropped(void) {
    TestEmission emission =
        test_lower_ir("func one(): int { let a: array<int, 3>; a[0] = 7; return a[1]; }\n");

    for (size_t i = 0; i < emission.ir->block_count; i++) {
        const MIRBlock *block = emission.ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            for (size_t p = 0; p < inst->place.projection_count; p++) {
                const Projection *projection = &inst->place.projections[p];

                if (projection->kind != PROJ_INDEX) {
                    continue;
                }

                bool defined = false;

                for (size_t b = 0; b < emission.ir->block_count && !defined; b++) {
                    for (size_t k = 0; k < emission.ir->blocks[b]->inst_count; k++) {
                        MIRValueId result = emission.ir->blocks[b]->insts[k].result;

                        if (!mir_value_is_none(result) && result.id == projection->index.id) {
                            defined = true;
                            break;
                        }
                    }
                }

                assert(defined);
            }
        }
    }

    test_emission_free(&emission);
}

int main(void) {
    negating_a_literal_leaves_no_negation();
    adding_two_literals_leaves_no_addition();
    a_nested_constant_expression_folds_wholly();
    adding_two_float_literals_leaves_no_addition();
    a_division_by_zero_keeps_its_division();
    an_operand_no_instruction_reads_is_dropped();
    a_variable_operand_keeps_its_operation();
    an_index_a_place_reads_is_not_dropped();

    printf("mir_fold_test passed\n");

    return 0;
}
