#include "mir/mir_instantiate.h"

#include <string.h>

typedef struct {
    Arena *arena;
    TypeRegistry *registry;
    FunctionRegistry *functions;

    const TypeArg *args;
    size_t arg_count;
} Instantiation;

static const Type *subst_type(Instantiation *in, const Type *type) {
    return type_registry_substitute(in->registry, type, in->args, in->arg_count);
}

static Constant subst_constant(Instantiation *in, Constant constant) {
    constant.type = subst_type(in, constant.type);

    /* A count the declaration could not take is one its instance can, since the type now states it. */
    if (constant.type && type_kind(constant.type) == TYPE_ARRAY) {
        constant.as_int = type_array_length(constant.type);
        constant.type = type_registry_get_primitive(in->registry, TYPE_I32);
    }

    return constant;
}

static TypeArg subst_arg(Instantiation *in, TypeArg arg) {
    if (arg.kind == TYPE_ARG_TYPE) {
        return (TypeArg){.kind = TYPE_ARG_TYPE, .type = subst_type(in, arg.type)};
    }

    if (arg.constant.kind != CONST_PARAM) {
        return arg;
    }

    return arg.constant.param < in->arg_count ? in->args[arg.constant.param] : arg;
}

/* A generic body names its callee in its own parameters, so the instance names one of its own. */
static Function *subst_callee(Instantiation *in, Function *callee) {
    if (!callee) {
        return callee;
    }

    /* A method standing on a bounded parameter becomes the one the implementor actually owns. */
    if (callee->bound_self) {
        const Type *self = subst_type(in, callee->bound_self);

        Function *owned =
            function_registry_owned_for(in->functions, in->registry, self, callee->decl->id.name);

        return owned ? owned : callee;
    }

    if (callee->type_arg_count == 0) {
        return callee;
    }

    TypeArg args[GAB_MAX_TYPE_PARAMS];
    bool moved = false;

    for (size_t i = 0; i < callee->type_arg_count && i < GAB_MAX_TYPE_PARAMS; i++) {
        args[i] = subst_arg(in, callee->type_args[i]);

        moved = moved || memcmp(&args[i], &callee->type_args[i], sizeof(TypeArg)) != 0;
    }

    if (!moved) {
        return callee;
    }

    return function_registry_specialize(in->functions, (Function *)callee, args, callee->type_arg_count);
}

static void subst_place(Instantiation *in, Place *place) {
    for (size_t i = 0; i < place->projection_count; i++) {
        place->projections[i].type = subst_type(in, place->projections[i].type);
    }
}

static Projection *clone_projections(Instantiation *in, const Projection *projections, size_t count) {
    if (count == 0) {
        return NULL;
    }

    Projection *out = arena_alloc(in->arena, count * sizeof(Projection));
    memcpy(out, projections, count * sizeof(Projection));

    return out;
}

static MIROperand *clone_args(Instantiation *in, const MIROperand *args, size_t count) {
    if (count == 0) {
        return NULL;
    }

    MIROperand *out = arena_alloc(in->arena, count * sizeof(MIROperand));

    for (size_t i = 0; i < count; i++) {
        out[i] = args[i];

        if (out[i].kind == OPERAND_CONST) {
            out[i].constant = subst_constant(in, out[i].constant);
        }
    }

    return out;
}

/* A bound resolving to an intrinsic names instructions, not a body, so the call becomes them here. */
static bool expand_intrinsic(MIRFunction *out, MIRBlock *block, const MIRInst *inst) {
    if (inst->op != MIR_CALL || !inst->callee || !(inst->callee->decl->modifiers & FUNC_MOD_INTRINSIC) ||
        inst->arg_count != 2) {
        return false;
    }

    MIRValueId receiver = mir_operand_as_value(inst->args[0]);
    MIRValueId index = mir_operand_as_value(inst->args[1]);

    if (mir_value_is_none(receiver) || mir_value_is_none(index)) {
        return false;
    }

    const Type *held = out->values[receiver.id].type;
    const Type *container = mir_indexed_container(held);

    const Type *element =
        type_kind(container) == TYPE_SLICE ? type_slice_element(container) : type_array_element(container);

    Place place = mir_place_index(out, mir_place_of(receiver, NULL), held, index, element);

    MIRValueId checked[2];
    size_t checked_count = mir_bounds_operands(place, held, index, checked);

    MIROperand *args = arena_alloc(out->arena, checked_count * sizeof(MIROperand));

    for (size_t i = 0; i < checked_count; i++) {
        args[i] = mir_operand_value(checked[i]);
    }

    block->insts[block->inst_count++] = (MIRInst){.op = MIR_BOUNDS,
                                                  .type = container,
                                                  .result = MIR_NO_VALUE,
                                                  .args = args,
                                                  .arg_count = checked_count,
                                                  .span = inst->span};

    block->insts[block->inst_count++] = (MIRInst){
        .op = MIR_REF, .type = inst->type, .result = inst->result, .place = place, .span = inst->span};

    return true;
}

static MIRInst subst_inst(Instantiation *in, const MIRInst *inst) {
    MIRInst out = *inst;

    out.type = subst_type(in, inst->type);
    out.args = clone_args(in, inst->args, inst->arg_count);

    if (mir_op_has_place(inst->op)) {
        out.place.projections = clone_projections(in, inst->place.projections, inst->place.projection_count);
        subst_place(in, &out.place);
    } else if (inst->op == MIR_CALL) {
        out.callee = subst_callee(in, inst->callee);
    } else if (inst->op == MIR_CONST_INT || inst->op == MIR_CONST_FLOAT || inst->op == MIR_CONST_BOOL ||
               inst->op == MIR_CONST_STR) {
        out.constant = subst_constant(in, inst->constant);
    }

    return out;
}

MIRFunction *mir_instantiate(Arena *arena, TypeRegistry *registry, FunctionRegistry *functions,
                             const MIRFunction *generic, Function *instance, const TypeArg *args,
                             size_t arg_count) {
    Instantiation in = {
        .arena = arena,
        .registry = registry,
        .functions = functions,
        .args = args,
        .arg_count = arg_count,
    };

    MIRFunction *out = arena_alloc(arena, sizeof(MIRFunction));

    *out = *generic;

    out->arena = arena;
    out->registry = registry;
    out->function = instance;

    out->values =
        generic->value_count ? arena_alloc(arena, generic->value_count * sizeof(MIRValueInfo)) : NULL;
    out->value_capacity = generic->value_count;

    for (size_t i = 0; i < generic->value_count; i++) {
        out->values[i] = generic->values[i];
        out->values[i].type = subst_type(&in, generic->values[i].type);
    }

    out->blocks = generic->block_count ? arena_alloc(arena, generic->block_count * sizeof(MIRBlock *)) : NULL;
    out->block_capacity = generic->block_count;

    for (size_t b = 0; b < generic->block_count; b++) {
        const MIRBlock *from = generic->blocks[b];

        MIRBlock *block = arena_alloc(arena, sizeof(MIRBlock));

        /* An intrinsic reached through a bound expands into the two instructions it stands for. */
        *block = (MIRBlock){.id = from->id, .inst_capacity = from->inst_count * 2 + 1};

        block->insts = arena_alloc(arena, block->inst_capacity * sizeof(MIRInst));

        for (size_t i = 0; i < from->inst_count; i++) {
            MIRInst inst = subst_inst(&in, &from->insts[i]);

            if (!expand_intrinsic(out, block, &inst)) {
                block->insts[block->inst_count++] = inst;
            }
        }

        out->blocks[b] = block;
    }

    if (generic->param_count > 0) {
        out->params = arena_alloc(arena, generic->param_count * sizeof(MIRValueId));
        memcpy(out->params, generic->params, generic->param_count * sizeof(MIRValueId));
    }

    return out;
}
