#include "mir/mir_fold.h"

#include <stdint.h>

typedef struct {
    Constant constant;

    bool known;
} KnownValue;

static bool inst_is_constant(MIROp op) {
    return op == MIR_CONST_INT || op == MIR_CONST_FLOAT || op == MIR_CONST_BOOL;
}

static KnownValue *known_values(Arena *arena, const MIRFunction *ir) {
    KnownValue *known = arena_alloc(arena, (ir->value_count + 1) * sizeof(KnownValue));

    for (size_t i = 0; i < ir->value_count; i++) {
        known[i] = (KnownValue){0};
    }

    for (size_t i = 0; i < ir->block_count; i++) {
        const MIRBlock *block = ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            if (!inst_is_constant(inst->op) || mir_value_is_none(inst->result)) {
                continue;
            }

            known[inst->result.id] = (KnownValue){.constant = inst->constant, .known = true};
        }
    }

    return known;
}

/* What an operand holds: the constant it names outright, or what its value was defined by. */
static bool known_operand(const KnownValue *known, MIROperand operand, KnownValue *out) {
    if (operand.kind == OPERAND_CONST) {
        *out = (KnownValue){.constant = operand.constant, .known = true};

        return true;
    }

    if (mir_value_is_none(operand.value) || !known[operand.value.id].known) {
        return false;
    }

    *out = known[operand.value.id];

    return true;
}

static bool fold_neg(const KnownValue *operand, MIRInst *inst) {
    if (constant_is_float(operand->constant)) {
        inst->op = MIR_CONST_FLOAT;
        inst->constant = constant_float(operand->constant.type, -operand->constant.as_float);

        return true;
    }

    if (constant_is_int(operand->constant)) {
        inst->op = MIR_CONST_INT;
        inst->constant =
            constant_int(operand->constant.type, (int32_t)(0u - (uint32_t)operand->constant.as_int));

        return true;
    }

    return false;
}

static bool fold_not(const KnownValue *operand, MIRInst *inst) {
    if (!constant_is_bool(operand->constant)) {
        return false;
    }

    inst->op = MIR_CONST_BOOL;
    inst->constant = constant_bool(operand->constant.type, !operand->constant.as_bool);

    return true;
}

/* A constant no instruction reads is what folding leaves behind, and emitting it would cost a load. */
static void drop_unread_constants(Arena *arena, MIRFunction *ir) {
    bool *read = arena_alloc(arena, (ir->value_count + 1) * sizeof(bool));

    for (size_t i = 0; i < ir->value_count; i++) {
        read[i] = false;
    }

    for (size_t i = 0; i < ir->block_count; i++) {
        const MIRBlock *block = ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            for (size_t a = 0; a < inst->arg_count; a++) {
                MIRValueId arg = mir_operand_as_value(inst->args[a]);

                if (!mir_value_is_none(arg)) {
                    read[arg.id] = true;
                }
            }

            /* A place reads its base and every index along its path, which no argument names. */
            if (!mir_op_has_place(inst->op)) {
                continue;
            }

            if (!mir_value_is_none(inst->place.base)) {
                read[inst->place.base.id] = true;
            }

            for (size_t p = 0; p < inst->place.projection_count; p++) {
                const Projection *projection = &inst->place.projections[p];

                if (projection->kind == PROJ_INDEX && !mir_value_is_none(projection->index)) {
                    read[projection->index.id] = true;
                }
            }
        }
    }

    for (size_t i = 0; i < ir->block_count; i++) {
        MIRBlock *block = ir->blocks[i];

        size_t kept = 0;

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            bool unread =
                inst_is_constant(inst->op) && !mir_value_is_none(inst->result) && !read[inst->result.id];

            if (!unread) {
                block->insts[kept++] = *inst;
            }
        }

        block->inst_count = kept;
    }
}

/* Division that traps is left for the VM to reach, so a program that divides by zero still fails its run. */
static bool fold_int_binary(MIROp op, int32_t a, int32_t b, int32_t *out) {
    switch (op) {
    case MIR_ADD:
        *out = (int32_t)((uint32_t)a + (uint32_t)b);
        return true;
    case MIR_SUB:
        *out = (int32_t)((uint32_t)a - (uint32_t)b);
        return true;
    case MIR_MUL:
        *out = (int32_t)((uint32_t)a * (uint32_t)b);
        return true;
    case MIR_DIV:
    case MIR_MOD:
        if (b == 0 || (a == INT32_MIN && b == -1)) {
            return false;
        }

        *out = op == MIR_DIV ? a / b : a % b;
        return true;
    default:
        return false;
    }
}

static bool fold_float_binary(MIROp op, float a, float b, float *out) {
    switch (op) {
    case MIR_ADD:
        *out = a + b;
        return true;
    case MIR_SUB:
        *out = a - b;
        return true;
    case MIR_MUL:
        *out = a * b;
        return true;
    case MIR_DIV:
        *out = a / b;
        return true;
    default:
        return false;
    }
}

static bool fold_binary(const KnownValue *left, const KnownValue *right, MIRInst *inst) {
    if (!left->constant.type || left->constant.type != right->constant.type) {
        return false;
    }

    if (constant_is_int(left->constant)) {
        int32_t folded;

        /* This folds signed, so a count's division is left for the backend, which divides it unsigned. */
        if (type_is_unsigned(left->constant.type) && (inst->op == MIR_DIV || inst->op == MIR_MOD)) {
            return false;
        }

        if (!fold_int_binary(inst->op, left->constant.as_int, right->constant.as_int, &folded)) {
            return false;
        }

        inst->op = MIR_CONST_INT;
        inst->constant = constant_int(left->constant.type, folded);

        return true;
    }

    if (constant_is_float(left->constant)) {
        float folded;

        if (!fold_float_binary(inst->op, left->constant.as_float, right->constant.as_float, &folded)) {
            return false;
        }

        inst->op = MIR_CONST_FLOAT;
        inst->constant.as_float = folded;

        return true;
    }

    return false;
}

void mir_fold(Arena *arena, MIRFunction *ir) {
    KnownValue *known = known_values(arena, ir);

    for (size_t i = 0; i < ir->block_count; i++) {
        MIRBlock *block = ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            MIRInst *inst = &block->insts[j];

            if (inst->arg_count < 1 || inst->arg_count > 2) {
                continue;
            }

            KnownValue operands[2] = {0};

            bool operands_known = true;

            for (size_t a = 0; a < inst->arg_count; a++) {
                operands_known = operands_known && known_operand(known, inst->args[a], &operands[a]);
            }

            if (!operands_known) {
                continue;
            }

            const KnownValue *operand = &operands[0];

            bool folded = false;

            switch (inst->op) {
            case MIR_NEG:
                folded = inst->arg_count == 1 && fold_neg(operand, inst);
                break;

            case MIR_NOT:
                folded = inst->arg_count == 1 && fold_not(operand, inst);
                break;

            case MIR_ADD:
            case MIR_SUB:
            case MIR_MUL:
            case MIR_DIV:
            case MIR_MOD:
                folded = inst->arg_count == 2 && fold_binary(operand, &operands[1], inst);
                break;

            default:
                break;
            }

            if (!folded) {
                continue;
            }

            inst->args = NULL;
            inst->arg_count = 0;

            known[inst->result.id] = (KnownValue){.constant = inst->constant, .known = true};
        }
    }

    drop_unread_constants(arena, ir);
}
