#include "mir/mir.h"

#include <string.h>

#define MIR_INITIAL_CAPACITY 8

static void *mir_grow(Arena *arena, void *data, size_t used, size_t *capacity, size_t element) {
    size_t next = *capacity == 0 ? MIR_INITIAL_CAPACITY : *capacity * 2;
    void *grown = arena_alloc(arena, next * element);

    if (data) {
        memcpy(grown, data, used * element);
    }

    *capacity = next;

    return grown;
}

MIRFunction *mir_function_create(Arena *arena, TypeRegistry *registry, Function *function) {
    MIRFunction *ir = arena_alloc(arena, sizeof(MIRFunction));

    *ir = (MIRFunction){.arena = arena, .registry = registry, .function = function, .entry = MIR_NO_BLOCK};

    ir->entry = mir_block_create(ir)->id;

    return ir;
}

MIRBlock *mir_block_create(MIRFunction *ir) {
    if (ir->block_count == ir->block_capacity) {
        ir->blocks =
            mir_grow(ir->arena, ir->blocks, ir->block_count, &ir->block_capacity, sizeof(MIRBlock *));
    }

    MIRBlock *block = arena_alloc(ir->arena, sizeof(MIRBlock));

    *block = (MIRBlock){.id = {(uint32_t)ir->block_count}};

    ir->blocks[ir->block_count++] = block;

    return block;
}

MIRBlock *mir_block_at(const MIRFunction *ir, MIRBlockId id) {
    if (mir_block_is_none(id) || id.id >= ir->block_count) {
        return NULL;
    }

    return ir->blocks[id.id];
}

MIRValueId mir_value_create(MIRFunction *ir, const Type *type, Binding *binding, Span span) {
    if (ir->value_count == ir->value_capacity) {
        ir->values =
            mir_grow(ir->arena, ir->values, ir->value_count, &ir->value_capacity, sizeof(MIRValueInfo));
    }

    ir->values[ir->value_count] = (MIRValueInfo){.type = type, .binding = binding, .span = span};

    return (MIRValueId){(uint32_t)ir->value_count++};
}

const MIRValueInfo *mir_value_info(const MIRFunction *ir, MIRValueId value) {
    if (mir_value_is_none(value) || value.id >= ir->value_count) {
        return NULL;
    }

    return &ir->values[value.id];
}

MIRInst *mir_emit(MIRFunction *ir, MIRBlock *block, MIRInst inst) {
    if (block->inst_count == block->inst_capacity) {
        block->insts =
            mir_grow(ir->arena, block->insts, block->inst_count, &block->inst_capacity, sizeof(MIRInst));
    }

    block->insts[block->inst_count] = inst;

    return &block->insts[block->inst_count++];
}

MIROperand *mir_args_alloc(MIRFunction *ir, size_t count) {
    if (count == 0) {
        return NULL;
    }

    return arena_alloc(ir->arena, count * sizeof(MIROperand));
}

Place mir_place_of(MIRValueId base, Binding *binding) { return (Place){.base = base, .binding = binding}; }

Place mir_place_project(MIRFunction *ir, Place place, Projection projection) {
    Projection *grown = arena_alloc(ir->arena, (place.projection_count + 1) * sizeof(Projection));

    if (place.projection_count > 0) {
        memcpy(grown, place.projections, place.projection_count * sizeof(Projection));
    }

    grown[place.projection_count] = projection;

    place.projections = grown;
    place.projection_count++;

    return place;
}

const Type *mir_indexed_container(const Type *type) {
    while (type_is_indirect(type)) {
        type = type_pointee(type);
    }

    return type;
}

Place mir_place_index(MIRFunction *ir, Place base, const Type *container, MIRValueId index,
                      const Type *element) {
    if (type_is_indirect(container)) {
        base = mir_place_project(ir, base, (Projection){.kind = PROJ_DEREF, .type = type_pointee(container)});
    }

    return mir_place_project(ir, base, (Projection){.kind = PROJ_INDEX, .index = index, .type = element});
}

size_t mir_bounds_operands(Place indexed, const Type *container, MIRValueId index, MIRValueId out[2]) {
    out[0] = index;
    out[1] = indexed.base;

    /* A slice states its own length, so the check reads it from the value rather than the type. */
    return type_kind(mir_indexed_container(container)) == TYPE_SLICE ? 2 : 1;
}

bool mir_type_needs_drop(TypeRegistry *registry, const Type *type) {
    return type && type_registry_owns(registry, type);
}

bool mir_op_has_place(MIROp op) {
    switch (op) {
    case MIR_LOAD:
    case MIR_STORE:
    case MIR_REF:
    case MIR_DROP:
    case MIR_NULL:
    case MIR_STORAGE_LIVE:
    case MIR_STORAGE_DEAD:
    case MIR_STORAGE_INIT:
        return true;
    default:
        return false;
    }
}

bool mir_op_is_terminator(MIROp op) {
    switch (op) {
    case MIR_JMP:
    case MIR_BRANCH:
    case MIR_RETURN:
    case MIR_UNREACHABLE:
        return true;
    default:
        return false;
    }
}

bool mir_block_is_terminated(const MIRBlock *block) {
    return block->inst_count > 0 && mir_op_is_terminator(block->insts[block->inst_count - 1].op);
}

size_t mir_block_successors(const MIRBlock *block, MIRBlockId out[2]) {
    if (!mir_block_is_terminated(block)) {
        return 0;
    }

    const MIRInst *terminator = &block->insts[block->inst_count - 1];

    switch (terminator->op) {
    case MIR_JMP:
        out[0] = terminator->targets[0];
        return 1;
    case MIR_BRANCH:
        out[0] = terminator->targets[0];
        out[1] = terminator->targets[1];
        return 2;
    default:
        return 0;
    }
}

static const char *const mir_op_names[MIR__COUNT] = {
    [MIR_CONST_INT] = "const.int",
    [MIR_CONST_FLOAT] = "const.float",
    [MIR_CONST_BOOL] = "const.bool",
    [MIR_CONST_STR] = "const.str",
    [MIR_ADD] = "add",
    [MIR_SUB] = "sub",
    [MIR_MUL] = "mul",
    [MIR_DIV] = "div",
    [MIR_MOD] = "mod",
    [MIR_NEG] = "neg",
    [MIR_CMP] = "cmp",
    [MIR_NOT] = "not",
    [MIR_ITOF] = "itof",
    [MIR_FTOI] = "ftoi",
    [MIR_LOAD] = "load",
    [MIR_STORE] = "store",
    [MIR_REF] = "ref",
    [MIR_BOUNDS] = "bounds",
    [MIR_COPY] = "copy",
    [MIR_MAKE_SLICE] = "make_slice",
    [MIR_SLICE_LEN] = "slice_len",
    [MIR_CALL] = "call",
    [MIR_NULL] = "null",
    [MIR_BOX] = "box",
    [MIR_DROP] = "drop",
    [MIR_STORAGE_LIVE] = "storage_live",
    [MIR_STORAGE_DEAD] = "storage_dead",
    [MIR_STORAGE_INIT] = "storage_init",
    [MIR_JMP] = "jmp",
    [MIR_BRANCH] = "branch",
    [MIR_RETURN] = "return",
    [MIR_UNREACHABLE] = "unreachable",
};

const char *mir_op_name(MIROp op) { return op < MIR__COUNT ? mir_op_names[op] : "?"; }

const char *mir_cmp_predicate_name(CmpPredicate predicate) {
    switch (predicate) {
    case MIR_CMP_LT:
        return "lt";
    case MIR_CMP_GT:
        return "gt";
    case MIR_CMP_EQ:
        return "eq";
    case MIR_CMP_NE:
        return "ne";
    case MIR_CMP_LE:
        return "le";
    case MIR_CMP_GE:
        return "ge";
    }

    return "?";
}
