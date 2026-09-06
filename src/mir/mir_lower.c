#include "mir/mir_lower.h"
#include "ast/facts.h"

#include "ast/expr.h"

#include <assert.h>

typedef struct {
    MIRFunction *ir;
    MIRBlock *block;

    TypeRegistry *registry;
    const Facts *facts;
    Arena *arena;

    /* The value each local was given, so a variable expression names it rather than looking it up. */
    Binding **locals;
    MIRValueId *local_values;
    size_t local_count;
    size_t local_capacity;

    MIRBlockId break_target;
    MIRBlockId continue_target;

    /* How many locals stood when the enclosing loop was entered, so a jump out closes the rest. */
    size_t loop_local_floor;

    /* The constant each value was defined by, so reading one names it rather than a slot. */
    MIROperand *constants;
    size_t constant_capacity;

    /* Where the body's own locals begin; a parameter's object is the caller's and is never ended. */
} Lowering;

static MIRValueId lower_expr(Lowering *lowering, ASTExpr *expr);
static void lower_stmt(Lowering *lowering, ASTStmt *stmt);
static void lower_struct_lit_into(Lowering *lowering, ASTExpr *expr, Place base);

/* An index and a length are counted values, which the allocator sizes only when they say so. */
static const Type *int_type(Lowering *lowering) {
    return type_registry_get_primitive(lowering->registry, TYPE_INT);
}

static MIRValueId lower_temp(Lowering *lowering, const Type *type, Span span) {
    return mir_value_create(lowering->ir, type, NULL, span);
}

static MIRInst *emit(Lowering *lowering, MIRInst inst) {
    return mir_emit(lowering->ir, lowering->block, inst);
}

static void bind_local(Lowering *lowering, Binding *binding, MIRValueId value) {
    if (lowering->local_count == lowering->local_capacity) {
        size_t next = lowering->local_capacity == 0 ? 8 : lowering->local_capacity * 2;

        Binding **bindings = arena_alloc(lowering->arena, next * sizeof(Binding *));
        MIRValueId *values = arena_alloc(lowering->arena, next * sizeof(MIRValueId));

        for (size_t i = 0; i < lowering->local_count; i++) {
            bindings[i] = lowering->locals[i];
            values[i] = lowering->local_values[i];
        }

        lowering->locals = bindings;
        lowering->local_values = values;
        lowering->local_capacity = next;
    }

    lowering->locals[lowering->local_count] = binding;
    lowering->local_values[lowering->local_count] = value;
    lowering->local_count++;
}

static MIRValueId local_value(Lowering *lowering, const Binding *binding) {
    for (size_t i = lowering->local_count; i > 0; i--) {
        if (lowering->locals[i - 1] == binding) {
            return lowering->local_values[i - 1];
        }
    }

    return MIR_NO_VALUE;
}

/* Records that a value holds a constant, so a later read of it can name the constant outright. */
static void note_constant(Lowering *lowering, MIRValueId value, Constant constant) {
    if (mir_value_is_none(value)) {
        return;
    }

    if (value.id >= lowering->constant_capacity) {
        size_t next = lowering->constant_capacity == 0 ? 16 : lowering->constant_capacity * 2;

        while (value.id >= next) {
            next *= 2;
        }

        MIROperand *grown = arena_alloc(lowering->arena, next * sizeof(MIROperand));

        for (size_t i = 0; i < next; i++) {
            grown[i] = mir_operand_value(MIR_NO_VALUE);
        }

        for (size_t i = 0; i < lowering->constant_capacity; i++) {
            grown[i] = lowering->constants[i];
        }

        lowering->constants = grown;
        lowering->constant_capacity = next;
    }

    lowering->constants[value.id] = mir_operand_const(constant);
}

/* A value a constant defined is read as that constant, which costs no register. */
static MIROperand lower_operand(Lowering *lowering, MIRValueId value) {
    if (!mir_value_is_none(value) && value.id < lowering->constant_capacity &&
        lowering->constants[value.id].kind == OPERAND_CONST) {
        return lowering->constants[value.id];
    }

    return mir_operand_value(value);
}

/* A binary instruction reads its right operand as a constant where one defined it; which constants a
 * backend can name in an instruction, rather than load first, is that backend's own question. */
static MIROperand *lower_binary_args(Lowering *lowering, const MIRValueId *values) {
    MIROperand *args = mir_args_alloc(lowering->ir, 2);

    args[0] = mir_operand_value(values[0]);
    args[1] = lower_operand(lowering, values[1]);

    return args;
}

static MIROperand *lower_args(Lowering *lowering, const MIRValueId *values, size_t count) {
    MIROperand *args = mir_args_alloc(lowering->ir, count);

    for (size_t i = 0; i < count; i++) {
        args[i] = mir_operand_value(values[i]);
    }

    return args;
}

static MIRValueId lower_unary(Lowering *lowering, MIROp op, ASTExpr *expr, ASTExpr *operand) {
    MIRValueId value = lower_expr(lowering, operand);
    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    emit(lowering, (MIRInst){.op = op,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = result,
                             .args = lower_args(lowering, &value, 1),
                             .arg_count = 1,
                             .span = expr->span});

    return result;
}

static void lower_terminator(Lowering *lowering, MIRInst inst) {
    if (mir_block_is_terminated(lowering->block)) {
        return;
    }

    emit(lowering, inst);
}

static void lower_jump(Lowering *lowering, MIRBlockId target) {
    lower_terminator(lowering, (MIRInst){.op = MIR_JMP, .result = MIR_NO_VALUE, .targets = {target}});
}

static MIROp bin_op_to_ir(BinOp op) {
    switch (op) {
    case BIN_OP_ADD:
        return MIR_ADD;
    case BIN_OP_SUB:
        return MIR_SUB;
    case BIN_OP_MUL:
        return MIR_MUL;
    case BIN_OP_DIV:
        return MIR_DIV;
    case BIN_OP_MOD:
        return MIR_MOD;
    default:
        return MIR_CMP;
    }
}

static bool bin_op_is_comparison(BinOp op, CmpPredicate *out) {
    switch (op) {
    case BIN_OP_LESS:
        *out = MIR_CMP_LT;
        return true;
    case BIN_OP_GREATER:
        *out = MIR_CMP_GT;
        return true;
    case BIN_OP_EQUAL:
        *out = MIR_CMP_EQ;
        return true;
    case BIN_OP_NEQUAL:
        *out = MIR_CMP_NE;
        return true;
    case BIN_OP_LEQUAL:
        *out = MIR_CMP_LE;
        return true;
    case BIN_OP_GEQUAL:
        *out = MIR_CMP_GE;
        return true;
    default:
        return false;
    }
}

static MIRValueId lower_load_from(Lowering *lowering, Place place, const Type *type, ReadKind read,
                                  Span span) {
    MIRValueId result = lower_temp(lowering, type, span);

    emit(lowering,
         (MIRInst){
             .op = MIR_LOAD, .type = type, .result = result, .place = place, .read = read, .span = span});

    return result;
}

static Place lower_place(Lowering *lowering, ASTExpr *expr);

/* Checks the index against the container, then names the element the check guarantees is there. */
static Place lower_indexed_place(Lowering *lowering, ASTExpr *target, ASTExpr *index_expr,
                                 const Type *element, Span span) {
    Place base = lower_place(lowering, target);
    MIRValueId index = lower_expr(lowering, index_expr);

    Place place = mir_place_index(lowering->ir, base, fact_type_of(lowering->facts, target), index, element);

    MIRValueId checked[2];
    size_t checked_count = mir_bounds_operands(place, fact_type_of(lowering->facts, target), index, checked);

    /* The check guards the access, so it is emitted before the place that reaches it. */
    emit(lowering, (MIRInst){.op = MIR_BOUNDS,
                             .type = mir_indexed_container(fact_type_of(lowering->facts, target)),
                             .result = MIR_NO_VALUE,
                             .args = lower_args(lowering, checked, checked_count),
                             .arg_count = checked_count,
                             .span = span});

    return place;
}

/* An lvalue's path, built by descending it; a base the IR has no place for is evaluated as a value. */
static Place lower_place(Lowering *lowering, ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE: {
        Binding *binding = fact_use_of(lowering->facts, expr);
        MIRValueId base = local_value(lowering, binding);

        if (mir_value_is_none(base)) {
            base = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);
        }

        return mir_place_of(base, binding);
    }

    case EXPR_FIELD: {
        Place place = lower_place(lowering, expr->field.target);

        /* Reaching a field through a pointer is a hop the source does not spell, and a pointer to a
         * pointer is as many hops as it has levels. */
        for (const Type *reached = fact_type_of(lowering->facts, expr->field.target);
             type_is_indirect(reached); reached = type_pointee(reached)) {
            place = mir_place_project(lowering->ir, place,
                                      (Projection){.kind = PROJ_DEREF, .type = type_pointee(reached)});
        }

        return mir_place_project(lowering->ir, place,
                                 (Projection){.kind = PROJ_FIELD,
                                              .field = {(uint32_t)expr->field.index},
                                              .type = fact_type_of(lowering->facts, expr)});
    }

    case EXPR_DEREF: {
        Place place = lower_place(lowering, expr->unary.target);

        return mir_place_project(
            lowering->ir, place,
            (Projection){.kind = PROJ_DEREF, .type = fact_type_of(lowering->facts, expr)});
    }

    case EXPR_INDEX:
        return lower_indexed_place(lowering, expr->index.target, expr->index.index,
                                   fact_type_of(lowering->facts, expr), expr->span);

    default:
        break;
    }

    return mir_place_of(lower_expr(lowering, expr), NULL);
}

static MIRValueId lower_literal(Lowering *lowering, ASTExpr *expr) {
    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    MIROp op = MIR_CONST_INT;
    Constant constant = constant_int(fact_type_of(lowering->facts, expr), expr->lit.as_int);

    switch (expr->lit.kind) {
    case LITERAL_STRING:
        op = MIR_CONST_STR;
        constant = constant_string(fact_type_of(lowering->facts, expr), expr->lit.as_string);
        break;
    case LITERAL_FLOAT:
        op = MIR_CONST_FLOAT;
        constant = constant_float(fact_type_of(lowering->facts, expr), expr->lit.as_float);
        break;
    case LITERAL_BOOL:
        op = MIR_CONST_BOOL;
        constant = constant_bool(fact_type_of(lowering->facts, expr), expr->lit.as_bool);
        break;
    case LITERAL_INT:
        break;
    }

    emit(lowering, (MIRInst){.op = op,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = result,
                             .constant = constant,
                             .span = expr->span});

    /* Text is named where it is read, like any other constant, rather than loaded into a slot first. */
    if (op != MIR_CONST_STR) {
        note_constant(lowering, result, constant);
    }

    return result;
}

/* 'a && b' evaluates 'b' only where 'a' allows it, so each operand needs its own block. */
static MIRValueId lower_logical(Lowering *lowering, ASTExpr *expr) {
    bool is_and = expr->bin_op.op == BIN_OP_AND;

    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);
    MIRValueId left = lower_expr(lowering, expr->bin_op.left);

    Place slot = mir_place_of(result, NULL);

    emit(lowering, (MIRInst){.op = MIR_STORE,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = MIR_NO_VALUE,
                             .place = slot,
                             .args = lower_args(lowering, &left, 1),
                             .arg_count = 1,
                             .span = expr->span});

    MIRBlock *rhs_block = mir_block_create(lowering->ir);
    MIRBlock *join = mir_block_create(lowering->ir);

    lower_terminator(
        lowering, (MIRInst){.op = MIR_BRANCH,
                            .result = MIR_NO_VALUE,
                            .args = lower_args(lowering, &left, 1),
                            .arg_count = 1,
                            .targets = {is_and ? rhs_block->id : join->id, is_and ? join->id : rhs_block->id},
                            .span = expr->span});

    lowering->block = rhs_block;

    MIRValueId right = lower_expr(lowering, expr->bin_op.right);

    emit(lowering, (MIRInst){.op = MIR_STORE,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = MIR_NO_VALUE,
                             .place = slot,
                             .args = lower_args(lowering, &right, 1),
                             .arg_count = 1,
                             .span = expr->span});

    lower_jump(lowering, join->id);

    lowering->block = join;

    return result;
}

static MIRValueId lower_bin_op(Lowering *lowering, ASTExpr *expr) {
    if (expr->bin_op.op == BIN_OP_AND || expr->bin_op.op == BIN_OP_OR) {
        return lower_logical(lowering, expr);
    }

    MIRValueId operands[2];
    operands[0] = lower_expr(lowering, expr->bin_op.left);
    operands[1] = lower_expr(lowering, expr->bin_op.right);

    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    CmpPredicate predicate;

    MIRInst inst = {.op = bin_op_to_ir(expr->bin_op.op),
                    .type = fact_type_of(lowering->facts, expr),
                    .result = result,
                    .args = lower_binary_args(lowering, operands),
                    .arg_count = 2,
                    .span = expr->span};

    if (bin_op_is_comparison(expr->bin_op.op, &predicate)) {
        inst.op = MIR_CMP;
        inst.predicate = predicate;
    }

    emit(lowering, inst);

    return result;
}

/* An intrinsic stands for instructions rather than a body, so the call never survives lowering. */
static bool lower_intrinsic_call(Lowering *lowering, ASTExpr *expr, MIRValueId *out) {
    Function *callee = fact_callee_of(lowering->facts, expr);

    if (!callee || callee->decl->body_kind != BODY_INTRINSIC || expr->call.args.size == 0) {
        return false;
    }

    ASTExpr *receiver = expr->call.args.data[0];

    if (expr->call.args.size == 1) {
        const Type *base = fact_type_of(lowering->facts, receiver);

        while (type_is_indirect(base)) {
            base = type_pointee(base);
        }

        /* An array's length is the count in its type, so it is known without reading the array. */
        if (type_kind(base) == TYPE_ARRAY) {
            MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

            emit(lowering,
                 (MIRInst){.op = MIR_CONST_INT,
                           .type = fact_type_of(lowering->facts, expr),
                           .result = result,
                           .constant = type_array_length_is_known(base)
                                           ? constant_int(int_type(lowering), type_array_length(base))
                                           : constant_int(base, 0),
                           .span = expr->span});

            *out = result;

            return true;
        }

        /* A slice states its length in the word past the address it holds, so reading it is no call. */
        MIRValueId source = lower_expr(lowering, receiver);
        MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

        MIROperand *args = mir_args_alloc(lowering->ir, 1);
        args[0] = mir_operand_value(source);

        emit(lowering, (MIRInst){.op = MIR_SLICE_LEN,
                                 .type = fact_type_of(lowering->facts, expr),
                                 .result = result,
                                 .args = args,
                                 .arg_count = 1,
                                 .span = expr->span});

        *out = result;

        return true;
    }

    const Type *container = fact_type_of(lowering->facts, receiver);

    while (type_is_indirect(container)) {
        container = type_pointee(container);
    }

    assert((type_kind(container) == TYPE_SLICE || type_kind(container) == TYPE_ARRAY) &&
           "only an array and a slice supply the indexing intrinsic");

    const Type *element =
        type_kind(container) == TYPE_SLICE ? type_slice_element(container) : type_array_element(container);

    Place place = lower_indexed_place(lowering, receiver, expr->call.args.data[1], element, expr->span);

    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    emit(lowering, (MIRInst){.op = MIR_REF,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = result,
                             .place = place,
                             .span = expr->span});

    *out = result;

    return true;
}

static MIRValueId lower_call(Lowering *lowering, ASTExpr *expr) {
    MIRValueId intrinsic;

    if (lower_intrinsic_call(lowering, expr, &intrinsic)) {
        return intrinsic;
    }

    /* A conversion is written as a call, and names a type where a call names a function. */
    if (fact_call_kind(lowering->facts, expr) == CALL_CONVERSION) {
        ASTExpr *operand = expr->call.args.data[0];
        const Type *to = fact_type_of(lowering->facts, expr);

        /* A conversion to what the operand already is names the same value rather than converting it. */
        if (type_kind(to) == type_kind(fact_type_of(lowering->facts, operand))) {
            return lower_expr(lowering, operand);
        }

        return lower_unary(lowering, type_kind(to) == TYPE_FLOAT ? MIR_ITOF : MIR_FTOI, expr, operand);
    }

    size_t count = expr->call.args.size;

    MIROperand *args = mir_args_alloc(lowering->ir, count);

    for (size_t i = 0; i < count; i++) {
        args[i] = mir_operand_value(lower_expr(lowering, expr->call.args.data[i]));
    }

    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    Function *callee = fact_callee_of(lowering->facts, expr);

    emit(lowering, (MIRInst){.op = callee && function_runs_native(callee) ? MIR_CALL_EXTERN : MIR_CALL,
                             .type = fact_type_of(lowering->facts, expr),
                             .result = result,
                             .args = args,
                             .arg_count = count,
                             .callee = callee,
                             .span = expr->span});

    return result;
}

/* Fills a place with a struct literal's fields, so no temporary stands between it and where it goes. */
static void lower_struct_lit_into(Lowering *lowering, ASTExpr *expr, Place base) {
    for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
        const ASTFieldInit *init = &expr->struct_lit.fields.data[i];

        Place field = mir_place_project(lowering->ir, base,
                                        (Projection){.kind = PROJ_FIELD,
                                                     .field = {(uint32_t)init->index},
                                                     .type = fact_type_of(lowering->facts, init->value)});

        /* A literal inside a literal fills its own field, so each stays its own tracked place. */
        if (init->value->kind == EXPR_STRUCT_LIT) {
            lower_struct_lit_into(lowering, init->value, field);
            continue;
        }

        MIRValueId value = lower_expr(lowering, init->value);

        emit(lowering, (MIRInst){.op = MIR_STORE,
                                 .type = fact_type_of(lowering->facts, init->value),
                                 .result = MIR_NO_VALUE,
                                 .place = field,
                                 .args = lower_args(lowering, &value, 1),
                                 .arg_count = 1,
                                 .span = init->span});
    }
}

static MIRValueId lower_struct_lit(Lowering *lowering, ASTExpr *expr) {
    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    lower_struct_lit_into(lowering, expr, mir_place_of(result, NULL));

    return result;
}

static MIRValueId lower_array_lit(Lowering *lowering, ASTExpr *expr) {
    MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

    Place base = mir_place_of(result, NULL);

    for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
        ASTExpr *element = expr->array_lit.elements.data[i];

        MIRValueId index = lower_temp(lowering, int_type(lowering), element->span);

        emit(lowering, (MIRInst){.op = MIR_CONST_INT,
                                 .type = int_type(lowering),
                                 .result = index,
                                 .constant = {.as_int = (int32_t)i},
                                 .span = element->span});

        MIRValueId value = lower_expr(lowering, element);

        Place slot = mir_place_project(
            lowering->ir, base,
            (Projection){.kind = PROJ_INDEX, .index = index, .type = fact_type_of(lowering->facts, element)});

        emit(lowering, (MIRInst){.op = MIR_STORE,
                                 .type = fact_type_of(lowering->facts, element),
                                 .result = MIR_NO_VALUE,
                                 .place = slot,
                                 .args = lower_args(lowering, &value, 1),
                                 .arg_count = 1,
                                 .span = element->span});
    }

    return result;
}

/* A slice is where the elements start and how many there are, which the resolver already counted. */
static MIRValueId lower_unadjusted(Lowering *lowering, ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_LITERAL:
        return lower_literal(lowering, expr);

    case EXPR_BIN_OP:
        return lower_bin_op(lowering, expr);

    case EXPR_CALL:
        return lower_call(lowering, expr);

    case EXPR_STRUCT_LIT:
        return lower_struct_lit(lowering, expr);

    case EXPR_ARRAY_LIT:
        return lower_array_lit(lowering, expr);

    case EXPR_NEG:
        return lower_unary(lowering, MIR_NEG, expr, expr->unary.target);

    case EXPR_NOT:
        return lower_unary(lowering, MIR_NOT, expr, expr->unary.target);

    case EXPR_BOX:
        return lower_unary(lowering, MIR_BOX, expr, expr->box_expr.value);

    case EXPR_ADDR_OF: {
        MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, expr), expr->span);

        emit(lowering, (MIRInst){.op = MIR_REF,
                                 .type = fact_type_of(lowering->facts, expr),
                                 .result = result,
                                 .place = lower_place(lowering, expr->unary.target),
                                 .span = expr->span});

        return result;
    }

    case EXPR_VARIABLE: {
        MIRValueId value = local_value(lowering, fact_use_of(lowering->facts, expr));

        /* A moving read leaves the place holding nothing, so it cannot name the local directly. */
        if (!mir_value_is_none(value) && !fact_moves(lowering->facts, expr)) {
            return value;
        }

        break;
    }

    default:
        break;
    }

    return lower_load_from(lowering, lower_place(lowering, expr), fact_type_of(lowering->facts, expr),
                           fact_moves(lowering->facts, expr) ? READ_MOVE : READ_COPY, expr->span);
}

/* A place the source names, reached through the dereferences a coercion applies before it. */
static Place lower_adjusted_place(Lowering *lowering, ASTExpr *expr, const Adjustment *adjustment) {
    Place place = lower_place(lowering, expr);

    for (unsigned int i = 0; i < adjustment->derefs; i++) {
        place = mir_place_project(lowering->ir, place,
                                  (Projection){.kind = PROJ_DEREF, .type = adjustment->deref_types[i]});
    }

    return place;
}

/* The value a coercion's dereferences reach, which is the value itself where it applies none. */
static MIRValueId lower_derefs(Lowering *lowering, ASTExpr *expr, const Adjustment *adjustment) {
    if (adjustment->derefs == 0) {
        return lower_unadjusted(lowering, expr);
    }

    const Type *reached = adjustment->deref_types[adjustment->derefs - 1];
    MIRValueId result = lower_temp(lowering, reached, expr->span);

    emit(lowering, (MIRInst){.op = MIR_LOAD,
                             .type = reached,
                             .result = result,
                             .place = lower_adjusted_place(lowering, expr, adjustment),
                             .span = expr->span});

    return result;
}

static MIRValueId lower_expr(Lowering *lowering, ASTExpr *expr) {
    Adjustment adjustment = fact_adjustment(lowering->facts, expr);

    if (adjustment.kind == ADJUST_NONE && adjustment.derefs == 0) {
        return lower_unadjusted(lowering, expr);
    }

    switch (adjustment.kind) {
    case ADJUST_UNSIZE: {
        MIRValueId operands[2];

        const Type *reached = adjustment.derefs ? adjustment.deref_types[adjustment.derefs - 1]
                                                : fact_type_of(lowering->facts, expr);

        /* The slice names where the elements start, which is a pointer rather than a slice itself. */
        const Type *elements = type_registry_ptr_to(lowering->registry, reached);

        operands[0] = lower_temp(lowering, elements, expr->span);

        emit(lowering, (MIRInst){.op = MIR_REF,
                                 .type = elements,
                                 .result = operands[0],
                                 .place = lower_adjusted_place(lowering, expr, &adjustment),
                                 .span = expr->span});

        operands[1] = lower_temp(lowering, int_type(lowering), expr->span);

        emit(lowering, (MIRInst){.op = MIR_CONST_INT,
                                 .type = int_type(lowering),
                                 .result = operands[1],
                                 .constant = {.as_int = adjustment.length},
                                 .span = expr->span});

        MIRValueId result = lower_temp(lowering, adjustment.to, expr->span);

        emit(lowering, (MIRInst){.op = MIR_MAKE_SLICE,
                                 .type = adjustment.to,
                                 .result = result,
                                 .args = lower_args(lowering, operands, 2),
                                 .arg_count = 2,
                                 .span = expr->span});

        return result;
    }

    case ADJUST_LEND: {
        MIRValueId source = lower_derefs(lowering, expr, &adjustment);
        MIRValueId result = lower_temp(lowering, adjustment.to, expr->span);

        emit(lowering,
             (MIRInst){.op = MIR_LEND,
                       .type = adjustment.to,
                       .result = result,
                       .args = lower_args(lowering, &source, 1),
                       .arg_count = 1,
                       .lend = {.parts = adjustment.lend.parts, .part_count = adjustment.lend.part_count},
                       .span = expr->span});

        return result;
    }

    case ADJUST_BORROW: {
        MIRValueId result = lower_temp(lowering, adjustment.to, expr->span);

        emit(lowering, (MIRInst){.op = MIR_REF,
                                 .type = adjustment.to,
                                 .result = result,
                                 .place = lower_adjusted_place(lowering, expr, &adjustment),
                                 .span = expr->span});

        return result;
    }

    case ADJUST_NONE:
        break;
    }

    return lower_derefs(lowering, expr, &adjustment);
}

/* A local's storage ends where its scope does, innermost first, so what it owns is dropped there. */
static void lower_scope_end(Lowering *lowering, size_t enclosing, Span span) {
    /* A block a jump already closed takes no further instructions; its locals end at that jump. */
    if (mir_block_is_terminated(lowering->block)) {
        lowering->local_count = enclosing;
        return;
    }

    while (lowering->local_count > enclosing) {
        size_t at = --lowering->local_count;

        Binding *binding = lowering->locals[at];
        MIRValueId value = lowering->local_values[at];

        Place place = mir_place_of(value, binding);

        if (binding && binding->kind == BINDING_VAR &&
            mir_type_needs_drop(lowering->registry, binding->var.type)) {
            emit(lowering, (MIRInst){.op = MIR_DROP,
                                     .type = binding->var.type,
                                     .result = MIR_NO_VALUE,
                                     .place = place,
                                     .span = span});
        }

        emit(lowering, (MIRInst){.op = MIR_STORAGE_DEAD,
                                 .type = binding ? binding->var.type : NULL,
                                 .result = MIR_NO_VALUE,
                                 .place = place,
                                 .span = span});
    }
}

static void lower_store_to(Lowering *lowering, Place place, MIRValueId value, const Type *type, Span span) {
    emit(lowering, (MIRInst){.op = MIR_STORE,
                             .type = type,
                             .result = MIR_NO_VALUE,
                             .place = place,
                             .args = lower_args(lowering, &value, 1),
                             .arg_count = 1,
                             .span = span});
}

static void lower_store(Lowering *lowering, ASTExpr *target, MIRValueId value, const Type *type, Span span) {
    lower_store_to(lowering, lower_place(lowering, target), value, type, span);
}

static void lower_var_decl(Lowering *lowering, ASTStmt *stmt) {
    ASTVarDecl *decl = &stmt->var_decl;

    MIRValueId local = mir_value_create(lowering->ir, decl->binding ? decl->binding->var.type : NULL,
                                        decl->binding, stmt->span);

    bind_local(lowering, decl->binding, local);

    emit(lowering, (MIRInst){.op = MIR_STORAGE_LIVE,
                             .type = decl->binding ? decl->binding->var.type : NULL,
                             .result = MIR_NO_VALUE,
                             .place = mir_place_of(local, decl->binding),
                             .span = stmt->span});

    if (!decl->initializer) {
        return;
    }

    if (decl->initializer->kind == EXPR_STRUCT_LIT) {
        Place place = mir_place_of(local, decl->binding);

        /* The local holds a value from here, whichever of its fields the literal went on to fill. */
        emit(lowering, (MIRInst){.op = MIR_STORAGE_INIT,
                                 .type = fact_type_of(lowering->facts, decl->initializer),
                                 .result = MIR_NO_VALUE,
                                 .place = place,
                                 .span = stmt->span});

        lower_struct_lit_into(lowering, decl->initializer, place);
        return;
    }

    MIRValueId value = lower_expr(lowering, decl->initializer);

    emit(lowering, (MIRInst){.op = MIR_STORE,
                             .type = fact_type_of(lowering->facts, decl->initializer),
                             .result = MIR_NO_VALUE,
                             .place = mir_place_of(local, decl->binding),
                             .args = lower_args(lowering, &value, 1),
                             .arg_count = 1,
                             .span = stmt->span});
}

static void lower_if(Lowering *lowering, ASTStmt *stmt) {
    MIRValueId condition = lower_expr(lowering, stmt->ifstmt.condition);

    MIRBlock *then_block = mir_block_create(lowering->ir);
    MIRBlock *join = mir_block_create(lowering->ir);

    /* Without an else there is nothing to run when the test fails, so the join is what it branches to
     * and no block stands between them forwarding a jump. */
    MIRBlock *else_block = stmt->ifstmt.else_block ? mir_block_create(lowering->ir) : join;

    lower_terminator(lowering, (MIRInst){.op = MIR_BRANCH,
                                         .result = MIR_NO_VALUE,
                                         .args = lower_args(lowering, &condition, 1),
                                         .arg_count = 1,
                                         .targets = {then_block->id, else_block->id},
                                         .span = stmt->span});

    lowering->block = then_block;
    lower_stmt(lowering, stmt->ifstmt.then_block);
    lower_jump(lowering, join->id);

    if (else_block != join) {
        lowering->block = else_block;
        lower_stmt(lowering, stmt->ifstmt.else_block);
        lower_jump(lowering, join->id);
    }

    lowering->block = join;
}

static void lower_for(Lowering *lowering, ASTStmt *stmt) {
    size_t enclosing = lowering->local_count;

    lower_stmt(lowering, stmt->forstmt.init);

    MIRBlock *header = mir_block_create(lowering->ir);
    MIRBlock *body = mir_block_create(lowering->ir);
    MIRBlock *post = mir_block_create(lowering->ir);
    MIRBlock *exit = mir_block_create(lowering->ir);

    lower_jump(lowering, header->id);

    lowering->block = header;

    if (stmt->forstmt.condition) {
        MIRValueId condition = lower_expr(lowering, stmt->forstmt.condition);

        lower_terminator(lowering, (MIRInst){.op = MIR_BRANCH,
                                             .result = MIR_NO_VALUE,
                                             .args = lower_args(lowering, &condition, 1),
                                             .arg_count = 1,
                                             .targets = {body->id, exit->id},
                                             .span = stmt->span});
    } else {
        lower_jump(lowering, body->id);
    }

    MIRBlockId enclosing_break = lowering->break_target;
    MIRBlockId enclosing_continue = lowering->continue_target;
    size_t enclosing_floor = lowering->loop_local_floor;

    lowering->break_target = exit->id;
    lowering->continue_target = post->id;
    lowering->loop_local_floor = lowering->local_count;

    lowering->block = body;
    lower_stmt(lowering, stmt->forstmt.body);
    lower_jump(lowering, post->id);

    lowering->break_target = enclosing_break;
    lowering->continue_target = enclosing_continue;
    lowering->loop_local_floor = enclosing_floor;

    lowering->block = post;
    lower_stmt(lowering, stmt->forstmt.post);
    lower_jump(lowering, header->id);

    lowering->block = exit;

    lower_scope_end(lowering, enclosing, stmt->span);
}

static void lower_return(Lowering *lowering, ASTStmt *stmt) {
    size_t count = stmt->ret.result ? 1 : 0;
    MIRValueId value = MIR_NO_VALUE;

    if (stmt->ret.result) {
        value = lower_expr(lowering, stmt->ret.result);
    }

    /* Returning leaves every scope the body opened, so each local it still holds is ended; a
     * parameter that owns what it was given ends with them, since the call handed it over. */
    for (size_t at = lowering->local_count; at > 0; at--) {
        Binding *binding = lowering->locals[at - 1];

        if (!binding || binding->kind != BINDING_VAR ||
            !mir_type_needs_drop(lowering->registry, binding->var.type)) {
            continue;
        }

        emit(lowering, (MIRInst){.op = MIR_DROP,
                                 .type = binding->var.type,
                                 .result = MIR_NO_VALUE,
                                 .place = mir_place_of(lowering->local_values[at - 1], binding),
                                 .span = stmt->span});
    }

    lower_terminator(lowering, (MIRInst){.op = MIR_RETURN,
                                         .result = MIR_NO_VALUE,
                                         .args = count ? lower_args(lowering, &value, 1) : NULL,
                                         .arg_count = count,
                                         .span = stmt->span});
}

static void lower_stmt(Lowering *lowering, ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR:
        lower_expr(lowering, stmt->expr.value);
        break;

    case STMT_VAR_DECL:
        lower_var_decl(lowering, stmt);
        break;

    case STMT_ASSIGN: {
        ASTExpr *target = stmt->assign.target;

        /* Overwriting a place that owns ends what it held, which is read out before the store so the
         * release still names it; a borrow names what another owns, so replacing one ends nothing. */
        bool overwrites_owned =
            (target->kind == EXPR_FIELD || target->kind == EXPR_DEREF || target->kind == EXPR_INDEX) &&
            mir_type_needs_drop(lowering->registry, fact_type_of(lowering->facts, target));

        Place place = lower_place(lowering, target);

        MIRValueId held = MIR_NO_VALUE;

        if (overwrites_owned) {
            held = lower_load_from(lowering, place, fact_type_of(lowering->facts, target), READ_COPY,
                                   stmt->span);
        }

        lower_store_to(lowering, place, lower_expr(lowering, stmt->assign.value),
                       fact_type_of(lowering->facts, stmt->assign.value), stmt->span);

        if (overwrites_owned) {
            emit(lowering, (MIRInst){.op = MIR_DROP,
                                     .type = fact_type_of(lowering->facts, target),
                                     .result = MIR_NO_VALUE,
                                     .place = mir_place_of(held, NULL),
                                     .span = stmt->span});
        }

        break;
    }

    case STMT_COMPOUND_ASSIGN: {
        ASTExpr *target = stmt->compound_assign.target;

        /* The target is reached once: its place serves both the read and the store. */
        Place place = lower_place(lowering, target);

        MIRValueId operands[2];
        operands[0] =
            lower_load_from(lowering, place, fact_type_of(lowering->facts, target), READ_COPY, target->span);
        operands[1] = lower_expr(lowering, stmt->compound_assign.value);

        MIRValueId result = lower_temp(lowering, fact_type_of(lowering->facts, target), stmt->span);

        emit(lowering, (MIRInst){.op = bin_op_to_ir(stmt->compound_assign.op),
                                 .type = fact_type_of(lowering->facts, target),
                                 .result = result,
                                 .args = lower_binary_args(lowering, operands),
                                 .arg_count = 2,
                                 .span = stmt->span});

        lower_store_to(lowering, place, result, fact_type_of(lowering->facts, target), stmt->span);
        break;
    }

    case STMT_BLOCK: {
        size_t enclosing = lowering->local_count;

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            lower_stmt(lowering, stmt->block.list.data[i]);
        }

        lower_scope_end(lowering, enclosing, stmt->span);
        break;
    }

    case STMT_IF:
        lower_if(lowering, stmt);
        break;

    case STMT_FOR:
        lower_for(lowering, stmt);
        break;

    case STMT_JUMP: {
        /* Leaving the loop leaves every scope opened inside it, so those locals end here. */
        for (size_t at = lowering->local_count; at > lowering->loop_local_floor; at--) {
            Binding *binding = lowering->locals[at - 1];

            if (!binding || binding->kind != BINDING_VAR ||
                !mir_type_needs_drop(lowering->registry, binding->var.type)) {
                continue;
            }

            emit(lowering, (MIRInst){.op = MIR_DROP,
                                     .type = binding->var.type,
                                     .result = MIR_NO_VALUE,
                                     .place = mir_place_of(lowering->local_values[at - 1], binding),
                                     .span = stmt->span});
        }

        lower_jump(lowering, stmt->jump.is_break ? lowering->break_target : lowering->continue_target);
        break;
    }

    case STMT_RETURN:
        lower_return(lowering, stmt);
        break;

    default:
        break;
    }
}

MIRFunction *mir_build_function(Arena *arena, TypeRegistry *registry, const Facts *facts, Function *function,
                                const ASTFieldList *params, ASTStmt *body) {
    MIRFunction *ir = mir_function_create(arena, registry, function);

    Lowering lowering = {.ir = ir,
                         .block = mir_block_at(ir, ir->entry),
                         .registry = registry,
                         .facts = facts,
                         .arena = arena,
                         .break_target = MIR_NO_BLOCK,
                         .continue_target = MIR_NO_BLOCK};

    ir->param_count = params ? params->size : 0;
    ir->params = arena_alloc(arena, (ir->param_count + 1) * sizeof(MIRValueId));

    for (size_t i = 0; i < ir->param_count; i++) {
        Binding *binding = params->data[i]->binding;

        MIRValueId value =
            mir_value_create(ir, binding ? binding->var.type : NULL, binding, params->data[i]->span);

        ir->params[i] = value;

        bind_local(&lowering, binding, value);
    }

    lowering.loop_local_floor = lowering.local_count;

    lower_stmt(&lowering, body);

    /* A body that runs off its end still returns, so every block reaching here is terminated. */
    lower_terminator(&lowering, (MIRInst){.op = MIR_RETURN, .result = MIR_NO_VALUE});

    return ir;
}
