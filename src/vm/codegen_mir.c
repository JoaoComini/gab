#include "vm/codegen_mir.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "binding.h"
#include "mir/mir_fold.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "type/type_registry.h"
#include "util/list.h"
#include "vm/opcode.h"
#include "vm/regalloc.h"

typedef struct {
    size_t at;

    MIRBlockId target;
} PendingJump;

GAB_LIST(PendingJumpList, pending_jump_list, PendingJump)

typedef struct {
    Chunk *chunk;

    MIRFunction *ir;
    const RegAlloc *alloc;

    /* Where each block starts once emitted, so a jump to one already placed needs no fixup. */
    size_t *block_at;

    PendingJumpList pending;

    FrameRefList refs;

    /* Address arithmetic needs registers no value owns, so they sit above the allocated frame. */
    unsigned int scratch;
    unsigned int scratch_high;

    const MIRUnitNumbering *numbering;

    /* The slot each value is written to, where a store lets it be computed straight into a local
     * rather than into a temporary the store would then copy out of. */
    unsigned int *redirect;

    Diagnostics *diagnostics;
    bool failed;
} MIREmitter;

#define BLOCK_NOT_EMITTED ((size_t)-1)

/* A field of a local sits at a fixed offset in the frame; a deref or an index needs an address. */
/* A path of fields lands on a slot only where every offset along it does; one reaching inside a slot
 * must be addressed by byte instead, since a slot copy would take its neighbours with it. */
static bool place_is_direct(TypeRegistry *registry, const Place *place, const Type *base_type) {
    const Type *type = base_type;

    for (size_t i = 0; i < place->projection_count; i++) {
        if (place->projections[i].kind != PROJ_FIELD) {
            return false;
        }

        const TypeLayout *layout = type ? type_registry_layout_of(registry, type) : NULL;

        if (!layout || place->projections[i].field.id >= layout->offset_count) {
            return false;
        }

        if (layout->offsets[place->projections[i].field.id] % VM_SLOT_SIZE != 0) {
            return false;
        }

        type = place->projections[i].type;

        /* A field narrower than a slot shares it with its neighbours, so a slot copy would take them. */
        if (type && type_registry_size_of(registry, type) % VM_SLOT_SIZE != 0) {
            return false;
        }
    }

    return true;
}

static const Type *place_base_type(const MIRFunction *ir, const Place *place) {
    const MIRValueInfo *info = mir_value_info(ir, place->base);

    return info ? info->type : NULL;
}

/* An element or a pointee is reached by address, one narrow value at its width and a wide one by slots. */
static bool place_is_reachable(TypeRegistry *registry, const MIRFunction *ir, const Place *place,
                               const Type *type) {
    if (place_is_direct(registry, place, place_base_type(ir, place))) {
        return true;
    }

    if (!type) {
        return false;
    }

    size_t size = type_registry_size_of(registry, type);

    if (size == 1 || size == 2 || size == 4) {
        return true;
    }

    return (size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE <= VM_MAX_STRUCT_SLOTS;
}

bool codegen_mir_supports(const MIRFunction *ir) {
    for (size_t i = 0; i < ir->block_count; i++) {
        const MIRBlock *block = ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            switch (block->insts[j].op) {
            case MIR_CONST_INT:
            case MIR_CONST_FLOAT:
            case MIR_CONST_BOOL:
            case MIR_CONST_STR:
            case MIR_ADD:
            case MIR_SUB:
            case MIR_MUL:
            case MIR_DIV:
            case MIR_MOD:
            case MIR_NEG:
            case MIR_ITOF:
            case MIR_FTOI:
            case MIR_CMP:
            case MIR_NOT:
            case MIR_COPY:
                break;

            case MIR_STORE:
            case MIR_LOAD:
                if (!place_is_reachable(ir->registry, ir, &block->insts[j].place, block->insts[j].type)) {
                    return false;
                }
                break;

            case MIR_CALL:
            case MIR_CALL_EXTERN:
                if (!block->insts[j].callee) {
                    return false;
                }
                break;

            case MIR_BOX:
            case MIR_MAKE_SLICE:
            case MIR_SLICE_LEN:
                break;

            /* A part starting inside a slot cannot be copied by slots, which is all a lend emits. */
            case MIR_LEND:
                if (block->insts[j].lend.part_count == 0) {
                    return false;
                }

                for (size_t k = 0; k < block->insts[j].lend.part_count; k++) {
                    if (block->insts[j].lend.parts[k].offset % VM_SLOT_SIZE != 0) {
                        return false;
                    }
                }
                break;

            case MIR_DROP:
            case MIR_NULL:
                if (!place_is_direct(ir->registry, &block->insts[j].place,
                                     place_base_type(ir, &block->insts[j].place))) {
                    return false;
                }
                break;

            case MIR_BOUNDS:
                if (!block->insts[j].type) {
                    return false;
                }
                break;

            case MIR_REF:
                break;

            case MIR_STORAGE_INIT:
                if (!place_is_direct(ir->registry, &block->insts[j].place,
                                     place_base_type(ir, &block->insts[j].place))) {
                    return false;
                }
                break;

            case MIR_STORAGE_LIVE:
            case MIR_STORAGE_DEAD:
                if (!place_is_direct(ir->registry, &block->insts[j].place,
                                     place_base_type(ir, &block->insts[j].place))) {
                    return false;
                }
                break;

            case MIR_JMP:
            case MIR_BRANCH:
            case MIR_RETURN:
                break;

            default:
                return false;
            }
        }
    }

    return true;
}

static unsigned int slot_of(const MIREmitter *emitter, MIRValueId value) {
    if (emitter->redirect && !mir_value_is_none(value) && value.id < emitter->ir->value_count &&
        emitter->redirect[value.id] != REGALLOC_NO_SLOT) {
        return emitter->redirect[value.id];
    }

    return regalloc_slot_of(emitter->alloc, value);
}

static unsigned int take_scratch(MIREmitter *emitter, unsigned int count);

/* The machine word a constant becomes: one slot, untagged, since the opcode reading it knows its
 * type. This is the only place the IR's value meets the pool the chunk indexes. */
static SlotWord machine_constant(Constant constant) {
    if (constant_is_float(constant)) {
        return (SlotWord){.as_float = constant.as_float};
    }

    if (constant_is_bool(constant)) {
        return (SlotWord){.as_int = constant.as_bool ? 1 : 0};
    }

    return (SlotWord){.as_int = constant.as_int};
}

/* The slot an operand reads from. The IR names a constant wherever one was written, which is more
 * than this encoding can hold, so one no instruction here can name is loaded into a slot first. */
static unsigned int operand_slot(MIREmitter *emitter, MIROperand operand) {
    if (operand.kind == OPERAND_VALUE) {
        return slot_of(emitter, operand.value);
    }

    unsigned int slot = take_scratch(emitter, 1);

    size_t index = constpool_add(emitter->chunk->const_pool, machine_constant(operand.constant));

    chunk_add_instruction(emitter->chunk, VM_ENCODE_I(OP_LOAD_CONST, slot, (uint32_t)index));

    return slot;
}

/* The text a literal names lives in the unit, so a load reaches it by an index linking then fixes. */
static void emit_string(MIREmitter *emitter, const MIRInst *inst) {
    unsigned int index;

    if (!emitter->numbering || !emitter->numbering->string ||
        !emitter->numbering->string(emitter->numbering->context, inst->constant.as_string, &index)) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "this text has no place in the unit");

        emitter->failed = true;

        return;
    }

    size_t offset = chunk_add_instruction(emitter->chunk,
                                          VM_ENCODE_I(OP_LOAD_STR, slot_of(emitter, inst->result), index));

    if (emitter->numbering->relocate_string) {
        emitter->numbering->relocate_string(emitter->numbering->context, emitter->chunk, offset);
    }
}

static const Type *operand_type(const MIREmitter *emitter, const MIRInst *inst);
static OpCode binary_opcode(const MIRInst *inst, bool *ok);
static bool binary_constant_opcode(MIROp op, OpCode *out);

static void emit_constant(MIREmitter *emitter, const MIRInst *inst) {
    size_t index = constpool_add(emitter->chunk->const_pool, machine_constant(inst->constant));

    chunk_add_instruction(emitter->chunk,
                          VM_ENCODE_I(OP_LOAD_CONST, slot_of(emitter, inst->result), (uint32_t)index));
}

static bool is_float(const MIRInst *inst) { return inst->type && type_kind(inst->type) == TYPE_FLOAT; }

static OpCode binary_opcode(const MIRInst *inst, bool *ok) {
    *ok = true;

    if (is_float(inst)) {
        switch (inst->op) {
        case MIR_ADD:
            return OP_ADDF;
        case MIR_SUB:
            return OP_SUBF;
        case MIR_MUL:
            return OP_MULF;
        case MIR_DIV:
            return OP_DIVF;
        default:
            break;
        }

        *ok = false;

        return OP_ADDF;
    }

    switch (inst->op) {
    case MIR_ADD:
        return OP_ADDI;
    case MIR_SUB:
        return OP_SUBI;
    case MIR_MUL:
        return OP_MULI;
    case MIR_DIV:
        return OP_DIVI;
    case MIR_MOD:
        return OP_MODI;
    default:
        break;
    }

    *ok = false;

    return OP_ADDI;
}

/* The non-negative int a value holds, where it is small enough to sit in the instruction itself. */
/* The immediate an operand names, where it is an int small enough to sit in an instruction. */
static bool int_immediate_of(MIROperand operand, unsigned int *out) {
    if (operand.kind != OPERAND_CONST || !constant_is_int(operand.constant)) {
        return false;
    }

    if (operand.constant.as_int < 0 || operand.constant.as_int > VM_MAX_IMMEDIATE) {
        return false;
    }

    *out = (unsigned int)operand.constant.as_int;

    return true;
}

/* The float constant an operand names, which a comparison can sit in its own instruction. */
static bool float_constant_of(MIROperand operand, SlotWord *out) {
    if (operand.kind != OPERAND_CONST || !constant_is_float(operand.constant)) {
        return false;
    }

    *out = machine_constant(operand.constant);

    return true;
}

static OpCode compare_constant_opcode(CmpPredicate predicate) {
    switch (predicate) {
    case MIR_CMP_LT:
        return OP_CMP_LTFK;
    case MIR_CMP_GT:
        return OP_CMP_GTFK;
    case MIR_CMP_EQ:
        return OP_CMP_EQFK;
    case MIR_CMP_NE:
        return OP_CMP_NEFK;
    case MIR_CMP_LE:
        return OP_CMP_LEFK;
    case MIR_CMP_GE:
        return OP_CMP_GEFK;
    }

    return OP_CMP_LTFK;
}

/* The form of a float operation that names its right operand in the constant pool. */
static bool binary_constant_opcode(MIROp op, OpCode *out) {
    switch (op) {
    case MIR_ADD:
        *out = OP_ADDFK;
        return true;
    case MIR_SUB:
        *out = OP_SUBFK;
        return true;
    case MIR_MUL:
        *out = OP_MULFK;
        return true;
    case MIR_DIV:
        *out = OP_DIVFK;
        return true;
    default:
        return false;
    }
}

static void emit_binary(MIREmitter *emitter, const MIRInst *inst) {
    bool ok;
    OpCode op = binary_opcode(inst, &ok);

    if (!ok) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "'%s' has no opcode on this type",
                   mir_op_name(inst->op));

        emitter->failed = true;

        return;
    }

    /* A small non-negative int operand sits in the instruction rather than in a slot of its own. */
    const Type *type = operand_type(emitter, inst);

    unsigned int immediate;

    if (type && type_kind(type) == TYPE_INT && int_immediate_of(inst->args[1], &immediate)) {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_RK(op, slot_of(emitter, inst->result),
                                           operand_slot(emitter, inst->args[0]), immediate, 1));

        return;
    }

    /* A float literal operand is named in the constant pool rather than loaded into a slot first. */
    OpCode constant_op;
    SlotWord constant;

    if (type && type_kind(type) == TYPE_FLOAT && binary_constant_opcode(inst->op, &constant_op) &&
        float_constant_of(inst->args[1], &constant)) {
        size_t index = constpool_add(emitter->chunk->const_pool, constant);

        if (index <= VM_MAX_IMMEDIATE) {
            chunk_add_instruction(emitter->chunk,
                                  VM_ENCODE_RK(constant_op, slot_of(emitter, inst->result),
                                               operand_slot(emitter, inst->args[0]), (unsigned int)index, 0));

            return;
        }
    }

    unsigned int saved = emitter->scratch;

    unsigned int left = operand_slot(emitter, inst->args[0]);
    unsigned int right = operand_slot(emitter, inst->args[1]);

    chunk_add_instruction(emitter->chunk, VM_ENCODE_R(op, slot_of(emitter, inst->result), left, right));

    emitter->scratch = saved;
}

static void emit_negate(MIREmitter *emitter, const MIRInst *inst) {
    chunk_add_instruction(emitter->chunk,
                          VM_ENCODE_R(is_float(inst) ? OP_NEGF : OP_NEGI, slot_of(emitter, inst->result),
                                      operand_slot(emitter, inst->args[0]), 0));
}

/* A comparison yields bool, so the operand type is what picks the opcode. */
static const Type *operand_type(const MIREmitter *emitter, const MIRInst *inst) {
    if (inst->args[0].kind == OPERAND_CONST) {
        return inst->type;
    }

    const MIRValueInfo *info = mir_value_info(emitter->ir, inst->args[0].value);

    return info ? info->type : NULL;
}

static OpCode compare_opcode(const Type *type, CmpPredicate predicate, bool *ok) {
    *ok = true;

    if (type && type_is_str_ref(type)) {
        switch (predicate) {
        case MIR_CMP_EQ:
            return OP_CMP_EQS;
        case MIR_CMP_NE:
            return OP_CMP_NES;
        default:
            *ok = false;
            return OP_CMP_EQS;
        }
    }

    bool floating = type && type_kind(type) == TYPE_FLOAT;

    switch (predicate) {
    case MIR_CMP_LT:
        return floating ? OP_CMP_LTF : OP_CMP_LTI;
    case MIR_CMP_GT:
        return floating ? OP_CMP_GTF : OP_CMP_GTI;
    case MIR_CMP_EQ:
        return floating ? OP_CMP_EQF : OP_CMP_EQI;
    case MIR_CMP_NE:
        return floating ? OP_CMP_NEF : OP_CMP_NEI;
    case MIR_CMP_LE:
        return floating ? OP_CMP_LEF : OP_CMP_LEI;
    case MIR_CMP_GE:
        return floating ? OP_CMP_GEF : OP_CMP_GEI;
    }

    *ok = false;

    return OP_CMP_EQI;
}

static void emit_compare(MIREmitter *emitter, const MIRInst *inst) {
    const Type *type = operand_type(emitter, inst);

    bool ok;
    OpCode op = compare_opcode(type, inst->predicate, &ok);

    if (!ok) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "'%s' has no opcode on this type",
                   mir_cmp_predicate_name(inst->predicate));

        emitter->failed = true;

        return;
    }

    /* A float compared to a literal names it in the instruction rather than loading it first. */
    SlotWord constant;

    if (type && type_kind(type) == TYPE_FLOAT && float_constant_of(inst->args[1], &constant)) {
        size_t index = constpool_add(emitter->chunk->const_pool, constant);

        if (index <= VM_MAX_IMMEDIATE) {
            chunk_add_instruction(emitter->chunk,
                                  VM_ENCODE_RK(compare_constant_opcode(inst->predicate),
                                               slot_of(emitter, inst->result),
                                               operand_slot(emitter, inst->args[0]), (unsigned int)index, 0));

            return;
        }
    }

    unsigned int saved = emitter->scratch;

    unsigned int left = operand_slot(emitter, inst->args[0]);
    unsigned int right = operand_slot(emitter, inst->args[1]);

    chunk_add_instruction(emitter->chunk, VM_ENCODE_R(op, slot_of(emitter, inst->result), left, right));

    emitter->scratch = saved;
}

static unsigned int take_scratch(MIREmitter *emitter, unsigned int count) {
    unsigned int at = emitter->scratch;

    emitter->scratch += count;

    if (emitter->scratch > emitter->scratch_high) {
        emitter->scratch_high = emitter->scratch;
    }

    return at;
}

/* '!a' is 'a == false', since the VM compares rather than negating. */
static void emit_not(MIREmitter *emitter, const MIRInst *inst) {
    size_t index = constpool_add(emitter->chunk->const_pool, (SlotWord){.as_int = 0});

    unsigned int result = slot_of(emitter, inst->result);
    unsigned int operand = operand_slot(emitter, inst->args[0]);

    unsigned int saved = emitter->scratch;

    /* The zero lands before the operand is read, so it cannot land on the operand itself. */
    unsigned int zero = result == operand ? take_scratch(emitter, 1) : result;

    chunk_add_instruction(emitter->chunk, VM_ENCODE_I(OP_LOAD_CONST, zero, (uint32_t)index));

    chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_CMP_EQI, result, operand, zero));

    emitter->scratch = saved;
}

/* A jump is written with its offset unknown, since the block it names may not be placed yet. */
static void emit_jump(MIREmitter *emitter, OpCode op, unsigned int reg, MIRBlockId target) {
    size_t at = chunk_add_instruction(emitter->chunk, VM_ENCODE_I(op, reg, 0));

    pending_jump_list_add(&emitter->pending, (PendingJump){.at = at, .target = target});
}

static void emit_branch(MIREmitter *emitter, const MIRInst *inst, MIRBlockId next) {
    MIRBlockId when_true = inst->targets[0];
    MIRBlockId when_false = inst->targets[1];

    unsigned int condition = operand_slot(emitter, inst->args[0]);

    /* Falling into a target costs no jump, so the one already next is the one left untaken. */
    if (!mir_block_is_none(next) && when_false.id == next.id) {
        emit_jump(emitter, OP_JMP_IF_TRUE, condition, when_true);

        return;
    }

    if (!mir_block_is_none(next) && when_true.id == next.id) {
        emit_jump(emitter, OP_JMP_IF_FALSE, condition, when_false);

        return;
    }

    emit_jump(emitter, OP_JMP_IF_FALSE, condition, when_false);
    emit_jump(emitter, OP_JMP, 0, when_true);
}

static unsigned int slots_of(const MIREmitter *emitter, const Type *type) {
    if (!type) {
        return 1;
    }

    size_t size = type_registry_size_of(emitter->ir->registry, type);

    return (unsigned int)((size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

static void emit_copy(MIREmitter *emitter, unsigned int dest, unsigned int src, unsigned int slots) {
    if (dest == src) {
        return;
    }

    if (slots > 1 && slots <= VM_MAX_MOVE_SLOTS) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_MOVE_N, dest, src, slots));

        return;
    }

    for (unsigned int i = 0; i < slots; i++) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_MOVE, dest + i, src + i, 0));
    }
}

/* Walks a path of fields to the slot it names, which is the base plus each field's offset. */
static unsigned int place_slot(const MIREmitter *emitter, const Place *place) {
    unsigned int slot = slot_of(emitter, place->base);

    const MIRValueInfo *info = mir_value_info(emitter->ir, place->base);
    const Type *type = info ? info->type : NULL;

    for (size_t i = 0; i < place->projection_count; i++) {
        const Projection *projection = &place->projections[i];

        /* Only a field is a fixed offset; anything else should have been refused before emitting. */
        assert(projection->kind == PROJ_FIELD);

        const TypeLayout *layout = type ? type_registry_layout_of(emitter->ir->registry, type) : NULL;

        if (layout && projection->field.id < layout->offset_count) {
            slot += (unsigned int)(layout->offsets[projection->field.id] / VM_SLOT_SIZE);
        }

        type = projection->type;
    }

    return slot;
}

static OpCode indirect_opcode(size_t size, bool load, bool *ok) {
    *ok = true;

    switch (size) {
    case 1:
        return load ? OP_LOAD_FIELD_PTR_1 : OP_STORE_FIELD_PTR_1;
    case 2:
        return load ? OP_LOAD_FIELD_PTR_2 : OP_STORE_FIELD_PTR_2;
    case 4:
        return load ? OP_LOAD_FIELD_PTR_4 : OP_STORE_FIELD_PTR_4;
    default:
        break;
    }

    *ok = false;

    return load ? OP_LOAD_FIELD_PTR_4 : OP_STORE_FIELD_PTR_4;
}

/* Forms the address a path names, leaving it in scratch slots the caller then loads or stores through. */
static unsigned int emit_address(MIREmitter *emitter, const Place *place) {
    const MIRValueInfo *info = mir_value_info(emitter->ir, place->base);
    const Type *type = info ? info->type : NULL;

    unsigned int address = take_scratch(emitter, VM_INDIRECT_SLOTS);
    unsigned int offset = 0;

    /* Whether the address register holds a pointer read from somewhere, rather than the address of
     * one; a deref reads through it only in the latter case. */
    bool holds_pointer = type && type_is_indirect(type);

    /* A base holding a pointer is already the address; one in the frame needs its address taken. */
    if (holds_pointer) {
        emit_copy(emitter, address, slot_of(emitter, place->base), VM_INDIRECT_SLOTS);
    } else {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(OP_ADDR_OF, address, slot_of(emitter, place->base), 0));
    }

    for (size_t i = 0; i < place->projection_count; i++) {
        const Projection *projection = &place->projections[i];

        switch (projection->kind) {
        case PROJ_FIELD: {
            const TypeLayout *layout = type ? type_registry_layout_of(emitter->ir->registry, type) : NULL;

            if (layout && projection->field.id < layout->offset_count) {
                offset += (unsigned int)layout->offsets[projection->field.id];
            }

            break;
        }

        /* An address that already holds a pointer names its pointee outright; one that names where a
         * pointer sits must read it first. */
        case PROJ_DEREF:
            if (offset > 0) {
                chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_ADD_PTR, address, address, offset));

                offset = 0;
            }

            if (!holds_pointer) {
                chunk_add_instruction(emitter->chunk,
                                      VM_ENCODE_R(OP_LOAD_PTR_N, address, address, VM_INDIRECT_SLOTS));
            }

            holds_pointer = false;

            break;

        case PROJ_INDEX: {
            chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_ADD_PTR, address, address, offset));

            offset = 0;

            size_t stride =
                projection->type ? type_registry_size_of(emitter->ir->registry, projection->type) : 1;

            unsigned int scaled = take_scratch(emitter, 1);

            chunk_add_instruction(
                emitter->chunk,
                VM_ENCODE_RK(OP_MULI, scaled, slot_of(emitter, projection->index), (unsigned int)stride, 1));

            chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_ADD_PTR_REG, address, address, scaled));

            break;
        }
        }

        type = projection->type;
    }

    if (offset > 0) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_ADD_PTR, address, address, offset));
    }

    return address;
}

static void emit_indirect(MIREmitter *emitter, const MIRInst *inst, bool load) {
    unsigned int saved = emitter->scratch;

    unsigned int address = emit_address(emitter, &inst->place);

    bool ok;
    OpCode op = indirect_opcode(type_registry_size_of(emitter->ir->registry, inst->type), load, &ok);

    /* A value wider than one indirect access is copied by slots, which is what the narrow ops lack. */
    if (!ok) {
        unsigned int slots = slots_of(emitter, inst->type);

        if (load) {
            chunk_add_instruction(emitter->chunk,
                                  VM_ENCODE_R(OP_LOAD_PTR_N, slot_of(emitter, inst->result), address, slots));
        } else {
            chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_STORE_PTR_N, address,
                                                              operand_slot(emitter, inst->args[0]), slots));
        }
    } else if (load) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(op, slot_of(emitter, inst->result), address, 0));
    } else {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(op, address, operand_slot(emitter, inst->args[0]), 0));
    }

    emitter->scratch = saved;
}

/* An array's length is in its type; a slice carries it in the slot after the pointer it was made from. */
static void emit_bounds(MIREmitter *emitter, const MIRInst *inst) {
    unsigned int index = operand_slot(emitter, inst->args[0]);

    if (type_kind(inst->type) == TYPE_SLICE) {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(OP_BOUNDS_CHECK_REG, 0, index,
                                          operand_slot(emitter, inst->args[1]) + VM_INDIRECT_SLOTS));

        return;
    }

    chunk_add_instruction(emitter->chunk, VM_ENCODE_RK(OP_BOUNDS_CHECK, 0, index,
                                                       (unsigned int)type_array_length(inst->type), 1));
}

static void emit_store(MIREmitter *emitter, const MIRInst *inst) {
    if (!place_is_direct(emitter->ir->registry, &inst->place, place_base_type(emitter->ir, &inst->place))) {
        emit_indirect(emitter, inst, false);

        return;
    }

    emit_copy(emitter, place_slot(emitter, &inst->place), operand_slot(emitter, inst->args[0]),
              slots_of(emitter, inst->type));
}

static void emit_load(MIREmitter *emitter, const MIRInst *inst) {
    if (!place_is_direct(emitter->ir->registry, &inst->place, place_base_type(emitter->ir, &inst->place))) {
        emit_indirect(emitter, inst, true);

        return;
    }

    emit_copy(emitter, slot_of(emitter, inst->result), place_slot(emitter, &inst->place),
              slots_of(emitter, inst->type));
}

/* The callee's frame starts at the window, taking its return from the first slot and its parameters
 * from those after, so the arguments are copied there in the order it reads them. */
static void emit_call(MIREmitter *emitter, const MIRInst *inst) {
    unsigned int saved = emitter->scratch;

    bool native = inst->op == MIR_CALL_EXTERN;

    /* A call yielding nothing still needs its window, but copies no result back out of it. */
    unsigned int returned = inst->type ? slots_of(emitter, inst->type) : 0;

    unsigned int width = 1;

    for (size_t i = 0; i < inst->arg_count; i++) {
        const MIRValueInfo *info = mir_value_info(emitter->ir, mir_operand_as_value(inst->args[i]));

        width += slots_of(emitter, info ? info->type : NULL);
    }

    if (returned > width) {
        width = returned;
    }

    unsigned int window = take_scratch(emitter, width);

    unsigned int offset = 1;

    for (size_t i = 0; i < inst->arg_count; i++) {
        const MIRValueInfo *info = mir_value_info(emitter->ir, mir_operand_as_value(inst->args[i]));

        unsigned int slots = slots_of(emitter, info ? info->type : NULL);

        emit_copy(emitter, window + offset, operand_slot(emitter, inst->args[i]), slots);

        offset += slots;
    }

    unsigned int index;
    bool relocates = false;

    if (!emitter->numbering ||
        !emitter->numbering->callee(emitter->numbering->context, inst->callee, native, &index, &relocates)) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "call to a function with no body");

        emitter->failed = true;

        emitter->scratch = saved;

        return;
    }

    size_t offset_of_call =
        chunk_add_instruction(emitter->chunk, VM_ENCODE_I(native ? OP_CALL_EXTERN : OP_CALL, window, index));

    /* A callee numbered within this unit is only final once linking adds the base it lands at. */
    if (relocates) {
        emitter->numbering->relocate_callee(emitter->numbering->context, emitter->chunk, offset_of_call,
                                            native);
    }

    if (returned > 0 && !mir_value_is_none(inst->result)) {
        emit_copy(emitter, slot_of(emitter, inst->result), window, returned);
    }

    emitter->scratch = saved;
}

/* The heap copy owns what the operand held, so the box stores it at the width it was allocated at. */
static void emit_box(MIREmitter *emitter, const MIRInst *inst) {
    const Type *boxed = inst->type ? type_pointee(inst->type) : NULL;

    unsigned int shape;

    if (!boxed || !emitter->numbering ||
        !emitter->numbering->heap_shape(emitter->numbering->context, boxed, &shape)) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "this type has no heap shape");

        emitter->failed = true;

        return;
    }

    unsigned int result = slot_of(emitter, inst->result);
    unsigned int source = operand_slot(emitter, inst->args[0]);

    unsigned int saved = emitter->scratch;

    /* Allocating writes the result before the operand is copied out, so the two cannot share a slot. */
    unsigned int allocated = result;

    if (result + slots_of(emitter, inst->type) > source && source + slots_of(emitter, boxed) > result) {
        allocated = take_scratch(emitter, slots_of(emitter, inst->type));
    }

    size_t offset = chunk_add_instruction(emitter->chunk, VM_ENCODE_I(OP_BOX, allocated, shape));

    if (emitter->numbering->relocate_type) {
        emitter->numbering->relocate_type(emitter->numbering->context, emitter->chunk, offset);
    }

    size_t size = type_registry_size_of(emitter->ir->registry, boxed);

    bool ok;
    OpCode op = indirect_opcode(size, false, &ok);

    if (ok) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(op, allocated, source, 0));
    } else {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(OP_STORE_PTR_N, allocated, source, slots_of(emitter, boxed)));
    }

    emit_copy(emitter, result, allocated, slots_of(emitter, inst->type));

    emitter->scratch = saved;
}

/* A slot a body releases is one an unwinding frame must release too, since a trap reaches no drop. */
static void record_frame_ref(MIREmitter *emitter, unsigned int slot, const Type *type) {
    for (size_t i = 0; i < emitter->refs.size; i++) {
        if (emitter->refs.data[i].slot == slot) {
            return;
        }
    }

    frame_ref_list_add(&emitter->refs,
                       (FrameRef){
                           .slot = slot,
                           .drop = type_registry_drop_of(emitter->ir->registry, type),
                           .release_width = type_registry_size_of(emitter->ir->registry, type),
                       });
}

/* Releasing walks the shape of what the place holds, which the VM reads from the type's drop plan. */
static void emit_release(MIREmitter *emitter, const MIRInst *inst) {
    unsigned int shape;

    if (!inst->type || !emitter->numbering ||
        !emitter->numbering->heap_shape(emitter->numbering->context, inst->type, &shape)) {
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "this type has no heap shape");

        emitter->failed = true;

        return;
    }

    unsigned int slot = place_slot(emitter, &inst->place);

    /* A trap reaches no drop the body emitted, so what a drop names the unwind must name too. */
    record_frame_ref(emitter, slot, inst->type);

    size_t offset = chunk_add_instruction(emitter->chunk, VM_ENCODE_I(OP_RELEASE, slot, shape));

    if (emitter->numbering->relocate_type) {
        emitter->numbering->relocate_type(emitter->numbering->context, emitter->chunk, offset);
    }
}

static void emit_return(MIREmitter *emitter, const MIRInst *inst) {
    if (inst->arg_count == 0) {
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));

        return;
    }

    const MIRValueInfo *info = mir_value_info(emitter->ir, mir_operand_as_value(inst->args[0]));

    unsigned int slots = slots_of(emitter, info ? info->type : NULL);

    if (slots == 1) {
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(OP_RETURN, 0, operand_slot(emitter, inst->args[0]), 0));

        return;
    }

    chunk_add_instruction(emitter->chunk,
                          VM_ENCODE_R(OP_RETURN_N, 0, operand_slot(emitter, inst->args[0]), slots));
}

/* A store whose value the instruction just before it computed into a temporary can have that
 * instruction write the local instead, sparing the copy. An owning value is left alone: the store is
 * where it changes hands, and what a temporary still holds has to be dropped. A multi-slot one is
 * too, since a value filled part by part must find its own slots held throughout. */
static void plan_redirects(MIREmitter *emitter) {
    MIRFunction *ir = emitter->ir;

    emitter->redirect = arena_alloc(ir->arena, (ir->value_count + 1) * sizeof(unsigned int));

    for (size_t i = 0; i < ir->value_count; i++) {
        emitter->redirect[i] = REGALLOC_NO_SLOT;
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        for (size_t j = 1; j < block->inst_count; j++) {
            const MIRInst *store = &block->insts[j];

            if (store->op != MIR_STORE || store->arg_count != 1) {
                continue;
            }

            MIRValueId value = mir_operand_as_value(store->args[0]);

            if (mir_value_is_none(value) || value.id >= ir->value_count) {
                continue;
            }

            const MIRInst *computed = &block->insts[j - 1];

            if (mir_value_is_none(computed->result) || computed->result.id != value.id) {
                continue;
            }

            /* A call writes its result through the window its frame starts at, not into a slot. */
            if (computed->op == MIR_CALL || computed->op == MIR_CALL_EXTERN) {
                continue;
            }

            if (ir->values[value.id].binding || mir_type_needs_drop(ir->registry, store->type)) {
                continue;
            }

            if (!place_is_direct(ir->registry, &store->place, place_base_type(ir, &store->place))) {
                continue;
            }

            emitter->redirect[value.id] = place_slot(emitter, &store->place);
        }
    }
}

static void emit_inst(MIREmitter *emitter, const MIRInst *inst, MIRBlockId next) {
    switch (inst->op) {
    case MIR_CONST_INT:
    case MIR_CONST_FLOAT:
    case MIR_CONST_BOOL:
        emit_constant(emitter, inst);
        break;

    case MIR_CONST_STR:
        emit_string(emitter, inst);
        break;

    case MIR_ADD:
    case MIR_SUB:
    case MIR_MUL:
    case MIR_DIV:
    case MIR_MOD:
        emit_binary(emitter, inst);
        break;

    case MIR_NEG:
        emit_negate(emitter, inst);
        break;

    case MIR_ITOF:
    case MIR_FTOI:
        chunk_add_instruction(emitter->chunk, VM_ENCODE_R(inst->op == MIR_ITOF ? OP_ITOF : OP_FTOI,
                                                          slot_of(emitter, inst->result),
                                                          operand_slot(emitter, inst->args[0]), 0));
        break;

    case MIR_CMP:
        emit_compare(emitter, inst);
        break;

    case MIR_NOT:
        emit_not(emitter, inst);
        break;

    case MIR_CALL:
    case MIR_CALL_EXTERN:
        emit_call(emitter, inst);
        break;

    case MIR_BOX:
        emit_box(emitter, inst);
        break;

    /* A slice is where its elements start followed by how many, which is what its two slots hold. */
    case MIR_MAKE_SLICE: {
        unsigned int result = slot_of(emitter, inst->result);

        emit_copy(emitter, result, operand_slot(emitter, inst->args[0]), VM_INDIRECT_SLOTS);

        emit_copy(emitter, result + VM_INDIRECT_SLOTS, operand_slot(emitter, inst->args[1]), 1);

        break;
    }

    /* A view is gathered from the parts it borrows, which sit apart in what it borrows from. */
    case MIR_LEND: {
        unsigned int result = slot_of(emitter, inst->result);
        unsigned int source = operand_slot(emitter, inst->args[0]);

        unsigned int at = 0;

        for (size_t i = 0; i < inst->lend.part_count; i++) {
            const LentPart *part = &inst->lend.parts[i];

            unsigned int width = (unsigned int)((part->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);

            emit_copy(emitter, result + at, source + (unsigned int)(part->offset / VM_SLOT_SIZE), width);

            at += width;
        }

        break;
    }

    case MIR_DROP:
        emit_release(emitter, inst);
        break;

    /* A place given away holds nothing, which the release it still reaches must find. */
    case MIR_NULL:
        chunk_add_instruction(emitter->chunk, VM_ENCODE_I(OP_NULL, place_slot(emitter, &inst->place), 0));
        break;

    case MIR_BOUNDS:
        emit_bounds(emitter, inst);
        break;

    case MIR_SLICE_LEN:
        chunk_add_instruction(emitter->chunk,
                              VM_ENCODE_R(OP_MOVE, slot_of(emitter, inst->result),
                                          operand_slot(emitter, inst->args[0]) + VM_INDIRECT_SLOTS, 0));
        break;

    /* A path through a place forms an address, which is what naming it yields. */
    case MIR_REF:
        if (place_is_direct(emitter->ir->registry, &inst->place,
                            place_base_type(emitter->ir, &inst->place))) {
            chunk_add_instruction(emitter->chunk, VM_ENCODE_R(OP_ADDR_OF, slot_of(emitter, inst->result),
                                                              place_slot(emitter, &inst->place), 0));
        } else {
            unsigned int saved = emitter->scratch;

            emit_copy(emitter, slot_of(emitter, inst->result), emit_address(emitter, &inst->place),
                      VM_INDIRECT_SLOTS);

            emitter->scratch = saved;
        }
        break;

    case MIR_STORE:
        emit_store(emitter, inst);
        break;

    case MIR_LOAD:
        emit_load(emitter, inst);
        break;

    case MIR_COPY:
        emit_copy(emitter, slot_of(emitter, inst->result), operand_slot(emitter, inst->args[0]),
                  slots_of(emitter, inst->type));
        break;

    /* Storage markers say where a local lives, which the register allocation has already answered. */
    case MIR_STORAGE_LIVE:
    case MIR_STORAGE_DEAD:
    case MIR_STORAGE_INIT:
        break;

    case MIR_JMP:
        if (mir_block_is_none(next) || inst->targets[0].id != next.id) {
            emit_jump(emitter, OP_JMP, 0, inst->targets[0]);
        }
        break;

    case MIR_BRANCH:
        emit_branch(emitter, inst, next);
        break;

    case MIR_RETURN:
        emit_return(emitter, inst);
        break;

    default:
        diag_error(emitter->diagnostics, GAB_ERR_CODEGEN, inst->span, "'%s' has no emission",
                   mir_op_name(inst->op));

        emitter->failed = true;
        break;
    }
}

/* Reverse postorder, so a block is placed after the one that reaches it wherever the graph allows. */
static void order_blocks(Arena *arena, const MIRFunction *ir, MIRBlockId *order, size_t *count, bool *seen) {
    MIRBlockId *stack = arena_alloc(arena, (ir->block_count + 1) * sizeof(MIRBlockId));
    size_t depth = 0;

    /* How many blocks still to be placed jump here; a join waits for the last of them, so every arm
     * falls into it rather than jumping backwards to reach it. */
    unsigned int *pending = arena_alloc(arena, (ir->block_count + 1) * sizeof(unsigned int));

    for (size_t b = 0; b < ir->block_count; b++) {
        pending[ir->blocks[b]->id.id] = 0;
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlockId successors[2];
        size_t successor_count = mir_block_successors(ir->blocks[b], successors);

        for (size_t i = 0; i < successor_count; i++) {
            pending[successors[i].id]++;
        }
    }

    stack[depth++] = ir->entry;
    seen[ir->entry.id] = true;

    size_t placed = 0;

    while (depth > 0) {
        MIRBlockId id = stack[--depth];

        order[placed++] = id;

        MIRBlockId successors[2];
        size_t successor_count = mir_block_successors(mir_block_at(ir, id), successors);

        /* Pushed in reverse so the first successor is the one taken off next. */
        for (size_t i = successor_count; i > 0; i--) {
            MIRBlockId next = successors[i - 1];

            if (pending[next.id] > 0) {
                pending[next.id]--;
            }

            if (seen[next.id] || pending[next.id] > 0) {
                continue;
            }

            seen[next.id] = true;
            stack[depth++] = next;
        }

        /* A loop's header is jumped back to from inside it, so its count never falls to zero on its
         * own; with nothing left to take, whatever is still waiting is released in the order it sits. */
        if (depth == 0) {
            for (size_t b = 0; b < ir->block_count; b++) {
                MIRBlockId id_b = ir->blocks[b]->id;

                if (!seen[id_b.id] && pending[id_b.id] > 0) {
                    seen[id_b.id] = true;
                    stack[depth++] = id_b;
                    break;
                }
            }
        }
    }

    *count = placed;
}

MIREmission codegen_mir_emit(Arena *arena, MIRFunction *ir, const MIRUnitNumbering *numbering,
                             Diagnostics *diagnostics) {
    mir_fold(arena, ir);

    if (!codegen_mir_supports(ir)) {
        return (MIREmission){.failed = true};
    }

    RegAlloc *alloc = regalloc_run(arena, ir, diagnostics);

    if (alloc->failed) {
        return (MIREmission){.failed = true};
    }

    MIREmitter emitter = {
        .chunk = chunk_create(),
        .ir = ir,
        .alloc = alloc,
        .block_at = arena_alloc(arena, (ir->block_count + 1) * sizeof(size_t)),
        .pending = pending_jump_list_create(arena_allocator(arena)),
        .numbering = numbering,
        .refs = frame_ref_list_create(DEFAULT_ALLOCATOR),
        .diagnostics = diagnostics,
        .scratch = alloc->frame_slots,
        .scratch_high = alloc->frame_slots,
    };

    for (size_t i = 0; i < ir->block_count; i++) {
        emitter.block_at[i] = BLOCK_NOT_EMITTED;
    }

    plan_redirects(&emitter);

    MIRBlockId *order = arena_alloc(arena, (ir->block_count + 1) * sizeof(MIRBlockId));
    bool *seen = arena_alloc(arena, (ir->block_count + 1) * sizeof(bool));

    for (size_t i = 0; i < ir->block_count; i++) {
        seen[i] = false;
    }

    size_t ordered = 0;
    order_blocks(arena, ir, order, &ordered, seen);

    for (size_t i = 0; i < ordered; i++) {
        const MIRBlock *block = mir_block_at(ir, order[i]);

        emitter.block_at[block->id.id] = emitter.chunk->instructions.size;

        MIRBlockId next = i + 1 < ordered ? order[i + 1] : MIR_NO_BLOCK;

        for (size_t j = 0; j < block->inst_count; j++) {
            emit_inst(&emitter, &block->insts[j], next);
        }
    }

    for (size_t i = 0; i < emitter.pending.size; i++) {
        const PendingJump *jump = &emitter.pending.data[i];

        size_t target = emitter.block_at[jump->target.id];

        Instruction instruction = emitter.chunk->instructions.data[jump->at];
        OpCode op = VM_DECODE_OPCODE(instruction);

        long offset = (long)target - (long)(jump->at + 1);

        chunk_patch_instruction(emitter.chunk, jump->at,
                                VM_ENCODE_I(op, VM_DECODE_I_RD(instruction), (uint32_t)offset));
    }

    OpCode last = emitter.chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&emitter.chunk->instructions))
                      : OP_LOAD_CONST;

    if (emitter.chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(emitter.chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    if (emitter.failed) {
        chunk_free(emitter.chunk);
        frame_ref_list_free(&emitter.refs);

        return (MIREmission){.failed = true};
    }

    /* The first value a declaration named is where a script's result sits. */
    unsigned int first_local = 0;

    for (size_t v = 0; v < ir->value_count; v++) {
        if (ir->values[v].binding) {
            first_local = regalloc_slot_of(alloc, (MIRValueId){(uint32_t)v});
            break;
        }
    }

    return (MIREmission){
        .chunk = emitter.chunk,
        .max_registers = emitter.scratch_high,
        .refs = emitter.refs,
        .first_local_slot = first_local,
    };
}
