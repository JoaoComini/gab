#include "llvm/llvm_emit.h"

#include "llvm/llvm_symbol.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GAB_MAX_STRUCT_FIELDS 64
#define GAB_MAX_CALL_ARGS 64

typedef struct {
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;

    const MIRFunction *ir;
    TypeRegistry *registry;
    Arena *arena;

    /* What each virtual register holds, indexed by its id. */
    LLVMValueRef *values;

    /* What a slot's storage holds, which a GEP must be given rather than infer from a pointer. */
    LLVMTypeRef *value_types;

    /* Allocas belong at the head of the entry block, wherever the instruction naming one sits. */
    LLVMBuilderRef entry;

    LLVMBasicBlockRef *blocks;
} LLVMEmitter;

static LLVMTypeRef llvm_type_of(LLVMEmitter *emitter, const Type *type) {
    if (!type) {
        return LLVMInt32TypeInContext(emitter->context);
    }

    switch (type_kind(type)) {
    case TYPE_F32:
        return LLVMFloatTypeInContext(emitter->context);
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(emitter->context);
    case TYPE_U8:
        return LLVMInt8TypeInContext(emitter->context);

    case TYPE_BOX:
    case TYPE_PTR:
    case TYPE_REF: {
        LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);

        /* What a run of elements names carries its count beside the address, as its layout states. */
        if (type_metadata_of(type_pointee(type)) == TYPE_META_LENGTH) {
            LLVMTypeRef members[] = {pointer, LLVMInt32TypeInContext(emitter->context)};

            return LLVMStructTypeInContext(emitter->context, members, 2, false);
        }

        return pointer;
    }

    case TYPE_ARRAY:
        return LLVMArrayType(llvm_type_of(emitter, type_array_element(type)),
                             (unsigned)type_array_length(type));

    case TYPE_STRUCT: {
        const TypeFields *fields = type_registry_fields_of(emitter->registry, type);

        LLVMTypeRef members[GAB_MAX_STRUCT_FIELDS];

        for (size_t i = 0; i < fields->count && i < GAB_MAX_STRUCT_FIELDS; i++) {
            members[i] = llvm_type_of(emitter, fields->fields[i].type);
        }

        return LLVMStructTypeInContext(emitter->context, members, (unsigned)fields->count, false);
    }

    default:
        return LLVMInt32TypeInContext(emitter->context);
    }
}

/* Frees one object, which the runtime null-checks and whose header carries any nested plan. */
static LLVMValueRef free_function(LLVMEmitter *emitter, LLVMTypeRef *out_signature) {
    LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);

    *out_signature = LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);

    LLVMValueRef declared = LLVMGetNamedFunction(emitter->module, "gab_free");

    return declared ? declared : LLVMAddFunction(emitter->module, "gab_free", *out_signature);
}

static LLVMTypeRef trap_signature(LLVMEmitter *emitter) {
    LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);

    return LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);
}

static LLVMValueRef trap_function(LLVMEmitter *emitter) {
    LLVMValueRef declared = LLVMGetNamedFunction(emitter->module, "gab_trap");

    if (declared) {
        return declared;
    }

    LLVMValueRef trap = LLVMAddFunction(emitter->module, "gab_trap", trap_signature(emitter));

    LLVMSetFunctionCallConv(trap, LLVMCCallConv);

    return trap;
}

static LLVMValueRef trap_message(LLVMEmitter *emitter) {
    LLVMValueRef existing = LLVMGetNamedGlobal(emitter->module, "gab.out_of_range");

    if (existing) {
        return existing;
    }

    return LLVMBuildGlobalStringPtr(emitter->builder, "index is out of range", "gab.out_of_range");
}

/* A slice is a pointer followed by its length, so the length is its second field. */
static LLVMValueRef slice_length(LLVMEmitter *emitter, MIROperand operand) {
    LLVMTypeRef held = emitter->value_types[operand.value.id];

    if (!held) {
        return LLVMBuildExtractValue(emitter->builder, emitter->values[operand.value.id], 1, "");
    }

    LLVMValueRef address =
        LLVMBuildStructGEP2(emitter->builder, held, emitter->values[operand.value.id], 1, "");

    return LLVMBuildLoad2(emitter->builder, LLVMInt32TypeInContext(emitter->context), address, "");
}

static LLVMValueRef drop_glue_of(LLVMEmitter *emitter, const Type *type);

/* Drops whatever a value of this type owns, reached from its address. */
static void emit_drop_body(LLVMEmitter *emitter, const Type *type, LLVMValueRef self) {
    LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);

    if (type_kind(type) == TYPE_BOX) {
        LLVMTypeRef signature;
        LLVMValueRef release = free_function(emitter, &signature);

        LLVMValueRef owned = LLVMBuildLoad2(emitter->builder, pointer, self, "");

        /* What the object holds ends before the object does, since freeing it loses the way to reach it. */
        const Type *pointee = type_pointee(type);

        if (pointee && type_registry_owns(emitter->registry, pointee)) {
            LLVMTypeRef glue_signature =
                LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);

            LLVMBuildCall2(emitter->builder, glue_signature, drop_glue_of(emitter, pointee), &owned, 1, "");
        }

        LLVMBuildCall2(emitter->builder, signature, release, &owned, 1, "");

        return;
    }

    if (type_kind(type) != TYPE_STRUCT) {
        return;
    }

    LLVMTypeRef held = llvm_type_of(emitter, type);
    const TypeFields *fields = type_registry_fields_of(emitter->registry, type);

    for (size_t i = 0; i < fields->count && i < GAB_MAX_STRUCT_FIELDS; i++) {
        const Type *field = fields->fields[i].type;

        if (!type_registry_owns(emitter->registry, field)) {
            continue;
        }

        LLVMValueRef address = LLVMBuildStructGEP2(emitter->builder, held, self, (unsigned)i, "");

        LLVMValueRef glue = drop_glue_of(emitter, field);

        LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);

        LLVMBuildCall2(emitter->builder, signature, glue, &address, 1, "");
    }
}

/* One function per type that owns something, emitted once and called wherever that type is dropped.
 * A named function is what makes a recursive type terminate rather than expanding forever. */
static LLVMValueRef drop_glue_of(LLVMEmitter *emitter, const Type *type) {
    char name[256];
    snprintf(name, sizeof(name), "drop.%s", llvm_type_symbol(emitter->arena, type));

    LLVMValueRef existing = LLVMGetNamedFunction(emitter->module, name);

    if (existing) {
        return existing;
    }

    LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);

    LLVMValueRef glue = LLVMAddFunction(emitter->module, name, signature);

    LLVMSetLinkage(glue, LLVMLinkOnceODRLinkage);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(emitter->context, glue, "entry");

    LLVMBuilderRef outer = emitter->builder;
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(emitter->context);

    LLVMPositionBuilderAtEnd(builder, entry);

    emitter->builder = builder;

    emit_drop_body(emitter, type, LLVMGetParam(glue, 0));

    LLVMBuildRetVoid(builder);
    LLVMDisposeBuilder(builder);

    emitter->builder = outer;

    return glue;
}

static LLVMValueRef operand_value(LLVMEmitter *emitter, MIROperand operand);

/* The address a place names, walked from its base through each projection. A base with no storage of
 * its own is a temporary the lowering stores into, which is given a slot the first time it is named. */
static LLVMValueRef place_address(LLVMEmitter *emitter, const Place *place, LLVMTypeRef *out_type) {
    /* Storage is what 'value_types' records, so a base carrying only a value is spilled, not reused. */
    if (!emitter->value_types[place->base.id]) {
        const MIRValueInfo *info = mir_value_info(emitter->ir, place->base);

        LLVMTypeRef held = llvm_type_of(emitter, info ? info->type : NULL);

        LLVMValueRef held_value = emitter->values[place->base.id];
        LLVMValueRef slot = LLVMBuildAlloca(emitter->entry, held, "");

        if (held_value) {
            LLVMBuildStore(emitter->builder, held_value, slot);
        }

        emitter->value_types[place->base.id] = held;
        emitter->values[place->base.id] = slot;
    }

    LLVMValueRef address = emitter->values[place->base.id];
    LLVMTypeRef type = emitter->value_types[place->base.id];

    /* Whether the address reached is a bare run rather than an aggregate holding one. */
    bool viewed = false;

    for (size_t i = 0; i < place->projection_count; i++) {
        const Projection *projection = &place->projections[i];

        switch (projection->kind) {
        case PROJ_FIELD:
            address = LLVMBuildStructGEP2(emitter->builder, type, address, projection->field.id, "");
            break;

        /* A base that is storage holds the pointer, so it is read; one that is already the pointer is
         * the address this projection names. */
        case PROJ_DEREF:
            if (type) {
                /* A view holds its address beside its count, so the pointer is the first of the pair. */
                if (LLVMGetTypeKind(type) == LLVMStructTypeKind) {
                    address = LLVMBuildStructGEP2(emitter->builder, type, address, 0, "");
                }

                address = LLVMBuildLoad2(emitter->builder, LLVMPointerTypeInContext(emitter->context, 0),
                                         address, "");
            }

            /* What a view names is a run with no length in its type, so nothing is stepped over here. */
            viewed = type_metadata_of(projection->type) == TYPE_META_LENGTH;
            break;

        case PROJ_INDEX: {
            LLVMValueRef index = operand_value(emitter, mir_operand_value(projection->index));

            /* Indexing an aggregate steps over it before selecting; a bare run is stepped through. */
            if (viewed) {
                address = LLVMBuildGEP2(emitter->builder, llvm_type_of(emitter, projection->type), address,
                                        &index, 1, "");
                break;
            }

            LLVMValueRef indices[2] = {
                LLVMConstInt(LLVMInt32TypeInContext(emitter->context), 0, false),
                index,
            };

            address = LLVMBuildGEP2(emitter->builder, type, address, indices, 2, "");
            break;
        }
        }

        if (projection->kind != PROJ_DEREF) {
            viewed = false;
        }

        type = llvm_type_of(emitter, projection->type);
    }

    *out_type = type;

    return address;
}

/* A local is storage, so reading one as a value loads through it; every other register is the value. */
static LLVMValueRef operand_value(LLVMEmitter *emitter, MIROperand operand) {
    if (operand.kind == OPERAND_CONST) {
        LLVMTypeRef type = llvm_type_of(emitter, operand.constant.type);

        if (operand.constant.type && type_kind(operand.constant.type) == TYPE_F32) {
            return LLVMConstReal(type, (double)operand.constant.as_float);
        }

        return LLVMConstInt(type, (unsigned long long)operand.constant.as_int, true);
    }

    LLVMTypeRef held = emitter->value_types[operand.value.id];

    if (held) {
        return LLVMBuildLoad2(emitter->builder, held, emitter->values[operand.value.id], "");
    }

    return emitter->values[operand.value.id];
}

/* Whether an instruction's operands are floating, which the opcode alone does not say. */
static bool operands_are_float(LLVMEmitter *emitter, const MIRInst *inst) {
    if (inst->arg_count == 0) {
        return false;
    }

    if (inst->args[0].kind == OPERAND_CONST) {
        return inst->args[0].constant.type && type_kind(inst->args[0].constant.type) == TYPE_F32;
    }

    const MIRValueInfo *info = mir_value_info(emitter->ir, inst->args[0].value);

    return info && info->type && type_kind(info->type) == TYPE_F32;
}

static LLVMIntPredicate int_predicate(CmpPredicate predicate) {
    switch (predicate) {
    case MIR_CMP_LT:
        return LLVMIntSLT;
    case MIR_CMP_GT:
        return LLVMIntSGT;
    case MIR_CMP_EQ:
        return LLVMIntEQ;
    case MIR_CMP_NE:
        return LLVMIntNE;
    case MIR_CMP_LE:
        return LLVMIntSLE;
    case MIR_CMP_GE:
        return LLVMIntSGE;
    }

    return LLVMIntEQ;
}

static LLVMRealPredicate real_predicate(CmpPredicate predicate) {
    switch (predicate) {
    case MIR_CMP_LT:
        return LLVMRealOLT;
    case MIR_CMP_GT:
        return LLVMRealOGT;
    case MIR_CMP_EQ:
        return LLVMRealOEQ;
    case MIR_CMP_NE:
        return LLVMRealONE;
    case MIR_CMP_LE:
        return LLVMRealOLE;
    case MIR_CMP_GE:
        return LLVMRealOGE;
    }

    return LLVMRealOEQ;
}

/* The signature a callee is reached through, which its declaration states whether or not a body is here. */
static LLVMTypeRef callee_signature(LLVMEmitter *emitter, const Function *callee) {
    LLVMTypeRef params[GAB_MAX_CALL_ARGS];

    size_t count = callee->param_count < GAB_MAX_CALL_ARGS ? callee->param_count : GAB_MAX_CALL_ARGS;

    for (size_t i = 0; i < count; i++) {
        params[i] = llvm_type_of(emitter, callee->params[i]);
    }

    /* A 'caller' function is reached through the location its call passes, which no declaration writes. */
    if ((callee->decl->modifiers & FUNC_MOD_CALLER) && callee->decl->location_type &&
        count < GAB_MAX_CALL_ARGS) {
        params[count++] = llvm_type_of(emitter, callee->decl->location_type);
    }

    LLVMTypeRef returns = callee->return_type ? llvm_type_of(emitter, callee->return_type)
                                              : LLVMVoidTypeInContext(emitter->context);

    return LLVMFunctionType(returns, params, (unsigned)count, false);
}

/* A callee is declared once per module, and the linker is what resolves one with no body here. */
static LLVMValueRef callee_value(LLVMEmitter *emitter, const Function *callee, LLVMTypeRef *out_signature) {
    const char *name = llvm_symbol_of(emitter->arena, callee);

    *out_signature = callee_signature(emitter, callee);

    LLVMValueRef declared = LLVMGetNamedFunction(emitter->module, name);

    return declared ? declared : LLVMAddFunction(emitter->module, name, *out_signature);
}

static void emit_inst(LLVMEmitter *emitter, const MIRInst *inst) {
    LLVMBuilderRef builder = emitter->builder;
    LLVMValueRef *values = emitter->values;

    bool floating = operands_are_float(emitter, inst);

    switch (inst->op) {
    case MIR_CONST_INT:
    case MIR_CONST_BOOL:
        values[inst->result.id] =
            LLVMConstInt(llvm_type_of(emitter, inst->type), (unsigned long long)inst->constant.as_int, true);
        break;

    case MIR_CONST_FLOAT:
        values[inst->result.id] =
            LLVMConstReal(llvm_type_of(emitter, inst->type), (double)inst->constant.as_float);
        break;

    case MIR_ADD:
        values[inst->result.id] = floating ? LLVMBuildFAdd(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildAdd(builder, operand_value(emitter, inst->args[0]),
                                                          operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_SUB:
        values[inst->result.id] = floating ? LLVMBuildFSub(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildSub(builder, operand_value(emitter, inst->args[0]),
                                                          operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_MUL:
        values[inst->result.id] = floating ? LLVMBuildFMul(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildMul(builder, operand_value(emitter, inst->args[0]),
                                                          operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_DIV:
        values[inst->result.id] = floating ? LLVMBuildFDiv(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildSDiv(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_MOD:
        values[inst->result.id] = floating ? LLVMBuildFRem(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildSRem(builder, operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_NEG:
        values[inst->result.id] = floating ? LLVMBuildFNeg(builder, operand_value(emitter, inst->args[0]), "")
                                           : LLVMBuildNeg(builder, operand_value(emitter, inst->args[0]), "");
        break;

    case MIR_NOT:
        values[inst->result.id] = LLVMBuildNot(builder, operand_value(emitter, inst->args[0]), "");
        break;

    case MIR_CMP:
        values[inst->result.id] = floating ? LLVMBuildFCmp(builder, real_predicate(inst->predicate),
                                                           operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "")
                                           : LLVMBuildICmp(builder, int_predicate(inst->predicate),
                                                           operand_value(emitter, inst->args[0]),
                                                           operand_value(emitter, inst->args[1]), "");
        break;

    case MIR_ITOF:
        values[inst->result.id] = LLVMBuildSIToFP(builder, operand_value(emitter, inst->args[0]),
                                                  LLVMFloatTypeInContext(emitter->context), "");
        break;

    /* Saturating, since a plain fptosi is undefined out of range and a cast is defined to clamp. */
    case MIR_FTOI: {
        LLVMTypeRef result = LLVMInt32TypeInContext(emitter->context);
        LLVMValueRef operand = operand_value(emitter, inst->args[0]);

        LLVMTypeRef overloads[2] = {result, LLVMTypeOf(operand)};

        unsigned id = LLVMLookupIntrinsicID("llvm.fptosi.sat", strlen("llvm.fptosi.sat"));

        LLVMValueRef saturating = LLVMGetIntrinsicDeclaration(emitter->module, id, overloads, 2);

        LLVMTypeRef signature = LLVMIntrinsicGetType(emitter->context, id, overloads, 2);

        values[inst->result.id] = LLVMBuildCall2(builder, signature, saturating, &operand, 1, "");
        break;
    }

    case MIR_COPY:
        values[inst->result.id] = operand_value(emitter, inst->args[0]);
        break;

    /* Text is one global per literal, named beside its length so it reads as any other view does. */
    case MIR_CONST_STR: {
        const String *text = inst->constant.as_string;

        LLVMValueRef characters = LLVMBuildGlobalStringPtr(emitter->entry, text ? text->data : "", "gab.str");

        LLVMValueRef length =
            LLVMConstInt(LLVMInt32TypeInContext(emitter->context), text ? text->length : 0, false);

        LLVMValueRef view = LLVMGetUndef(llvm_type_of(emitter, inst->type));

        view = LLVMBuildInsertValue(builder, view, characters, 0, "");

        values[inst->result.id] = LLVMBuildInsertValue(builder, view, length, 1, "");
        break;
    }

    /* A run reaches a view by naming where it starts beside how many it holds. */
    case MIR_MAKE_SLICE: {
        LLVMValueRef view = LLVMGetUndef(llvm_type_of(emitter, inst->type));

        view = LLVMBuildInsertValue(builder, view, operand_value(emitter, inst->args[0]), 0, "");

        values[inst->result.id] =
            LLVMBuildInsertValue(builder, view, operand_value(emitter, inst->args[1]), 1, "");
        break;
    }

    case MIR_SLICE_LEN:
        values[inst->result.id] = slice_length(emitter, inst->args[0]);
        break;

    /* Giving a value away empties the slot, so releasing it later frees nothing. */
    /* Giving a value away empties the slot, so releasing it later frees nothing. */
    case MIR_NULL: {
        LLVMTypeRef held;
        LLVMValueRef address = place_address(emitter, &inst->place, &held);

        LLVMBuildStore(builder, LLVMConstNull(held), address);
        break;
    }

    case MIR_STORAGE_LIVE: {
        const MIRValueInfo *info = mir_value_info(emitter->ir, inst->place.base);

        LLVMTypeRef held = llvm_type_of(emitter, info ? info->type : NULL);

        emitter->value_types[inst->place.base.id] = held;
        values[inst->place.base.id] = LLVMBuildAlloca(emitter->entry, held, "");

        /* A local holds nothing until it is given a value, and reading one is spelled as reading zero.
         * The store sits where scope opens rather than beside the alloca, so a loop clears each pass. */
        LLVMBuildStore(builder, LLVMConstNull(held), values[inst->place.base.id]);
        break;
    }

    /* A slot's storage ends with its frame, so nothing is emitted for it here. */
    case MIR_STORAGE_DEAD:
    case MIR_STORAGE_INIT:
        break;

    case MIR_STORE: {
        LLVMTypeRef held;
        LLVMValueRef address = place_address(emitter, &inst->place, &held);

        LLVMBuildStore(builder, operand_value(emitter, inst->args[0]), address);
        break;
    }

    case MIR_LOAD: {
        LLVMTypeRef held;
        LLVMValueRef address = place_address(emitter, &inst->place, &held);

        values[inst->result.id] = LLVMBuildLoad2(builder, llvm_type_of(emitter, inst->type), address, "");
        break;
    }

    case MIR_REF: {
        LLVMTypeRef held;

        values[inst->result.id] = place_address(emitter, &inst->place, &held);
        break;
    }

    /* An index outside its container ends the program, since there is no frame to unwind to. */
    case MIR_BOUNDS: {
        const Type *container = inst->type;

        LLVMTypeRef i32 = LLVMInt32TypeInContext(emitter->context);
        LLVMValueRef index = operand_value(emitter, inst->args[0]);

        LLVMValueRef length;

        if (inst->arg_count > 1) {
            /* A slice states its own length, which the second operand names. */
            length = slice_length(emitter, inst->args[1]);
        } else {
            length = LLVMConstInt(i32, (unsigned long long)type_array_length(container), false);
        }

        LLVMValueRef below = LLVMBuildICmp(builder, LLVMIntSLT, index, LLVMConstInt(i32, 0, true), "");
        LLVMValueRef above = LLVMBuildICmp(builder, LLVMIntSGE, index, length, "");
        LLVMValueRef outside = LLVMBuildOr(builder, below, above, "");

        LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder));

        LLVMBasicBlockRef trap = LLVMAppendBasicBlockInContext(emitter->context, function, "out_of_range");
        LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(emitter->context, function, "in_range");

        LLVMBuildCondBr(builder, outside, trap, ok);

        LLVMPositionBuilderAtEnd(builder, trap);
        LLVMBuildCall2(builder, trap_signature(emitter), trap_function(emitter),
                       (LLVMValueRef[]){trap_message(emitter)}, 1, "");
        LLVMBuildUnreachable(builder);

        /* What follows the check belongs to the path that passed it. */
        LLVMPositionBuilderAtEnd(builder, ok);
        break;
    }

    /* Dropping calls the glue for the type, which reaches whatever that type owns. */
    case MIR_DROP: {
        if (!inst->type || !type_registry_owns(emitter->registry, inst->type)) {
            break;
        }

        LLVMTypeRef held;
        LLVMValueRef address = place_address(emitter, &inst->place, &held);

        LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);
        LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(emitter->context), &pointer, 1, false);

        LLVMBuildCall2(builder, signature, drop_glue_of(emitter, inst->type), &address, 1, "");
        break;
    }

    /* Allocating is a call the linker resolves, so the emitted object embeds no host pointer. */
    case MIR_BOX: {
        const Type *boxed = inst->type ? type_pointee(inst->type) : NULL;

        LLVMTypeRef size_type = LLVMInt64TypeInContext(emitter->context);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(emitter->context, 0);

        LLVMTypeRef signature = LLVMFunctionType(pointer, &size_type, 1, false);

        LLVMValueRef box = LLVMGetNamedFunction(emitter->module, "gab_box");

        if (!box) {
            box = LLVMAddFunction(emitter->module, "gab_box", signature);
        }

        LLVMValueRef size =
            LLVMConstInt(size_type, type_registry_size_of(emitter->ir->registry, boxed), false);

        LLVMValueRef object = LLVMBuildCall2(builder, signature, box, &size, 1, "");

        LLVMBuildStore(builder, operand_value(emitter, inst->args[0]), object);

        values[inst->result.id] = object;
        break;
    }

    case MIR_CALL: {
        LLVMValueRef args[GAB_MAX_CALL_ARGS];

        size_t count = inst->arg_count < GAB_MAX_CALL_ARGS ? inst->arg_count : GAB_MAX_CALL_ARGS;

        for (size_t i = 0; i < count; i++) {
            args[i] = operand_value(emitter, inst->args[i]);
        }

        LLVMTypeRef signature;
        LLVMValueRef callee = callee_value(emitter, inst->callee, &signature);

        LLVMValueRef call = LLVMBuildCall2(builder, signature, callee, args, (unsigned)count, "");

        if (inst->type) {
            values[inst->result.id] = call;
        }

        break;
    }

    case MIR_JMP:
        LLVMBuildBr(builder, emitter->blocks[inst->targets[0].id]);
        break;

    case MIR_BRANCH:
        LLVMBuildCondBr(builder, operand_value(emitter, inst->args[0]), emitter->blocks[inst->targets[0].id],
                        emitter->blocks[inst->targets[1].id]);
        break;

    case MIR_RETURN:
        if (inst->arg_count == 0) {
            /* A body whose paths all return still ends with a block nothing reaches, which returns no
             * value however the signature reads. */
            if (emitter->ir->function->return_type) {
                LLVMBuildUnreachable(builder);
                break;
            }

            LLVMBuildRetVoid(builder);
            break;
        }

        LLVMBuildRet(builder, operand_value(emitter, inst->args[0]));
        break;

    case MIR_UNREACHABLE:
        LLVMBuildUnreachable(builder);
        break;

    default:
        break;
    }
}

struct LLVMUnit {
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;

    Arena *arena;
};

void llvm_unit_declares(LLVMUnit *unit, const char *symbol) {
    LLVMTypeRef byte = LLVMInt8TypeInContext(unit->context);
    LLVMValueRef global = LLVMAddGlobal(unit->module, byte, symbol);

    LLVMSetInitializer(global, LLVMConstInt(byte, 0, false));
    LLVMSetLinkage(global, LLVMExternalLinkage);
}

void llvm_unit_requires(LLVMUnit *unit, const char *symbol) {
    LLVMTypeRef byte = LLVMInt8TypeInContext(unit->context);

    /* Referenced from a global rather than a body, so nothing needs to run for the link to check it. */
    LLVMValueRef required = LLVMAddGlobal(unit->module, byte, symbol);

    LLVMSetLinkage(required, LLVMExternalLinkage);

    LLVMTypeRef pointer = LLVMPointerTypeInContext(unit->context, 0);

    char name[512];
    snprintf(name, sizeof(name), "%s.needed", symbol);

    LLVMValueRef anchor = LLVMAddGlobal(unit->module, pointer, name);

    LLVMSetInitializer(anchor, required);
    LLVMSetLinkage(anchor, LLVMInternalLinkage);
    LLVMSetGlobalConstant(anchor, true);

    /* Kept though nothing reads it, so the reference survives to the link. */
    LLVMSetSection(anchor, ".gab.imports");
}

LLVMUnit *llvm_unit_open(Arena *arena) {
    LLVMUnit *unit = arena_alloc(arena, sizeof(LLVMUnit));

    unit->arena = arena;
    unit->context = LLVMContextCreate();
    unit->module = LLVMModuleCreateWithNameInContext("gab", unit->context);
    unit->builder = LLVMCreateBuilderInContext(unit->context);

    return unit;
}

void llvm_unit_add(LLVMUnit *unit, const MIRFunction *ir) {
    Arena *arena = unit->arena;

    LLVMEmitter emitter = {.ir = ir,
                           .registry = ir->registry,
                           .arena = arena,
                           .context = unit->context,
                           .module = unit->module,
                           .builder = unit->builder};

    emitter.values = arena_alloc(arena, (ir->value_count + 1) * sizeof(LLVMValueRef));
    emitter.value_types = arena_alloc(arena, (ir->value_count + 1) * sizeof(LLVMTypeRef));

    /* A null entry is what says a register holds its value rather than storage for one, and what says
     * a temporary stored into has not been given a slot yet. */
    memset(emitter.values, 0, (ir->value_count + 1) * sizeof(LLVMValueRef));
    memset(emitter.value_types, 0, (ir->value_count + 1) * sizeof(LLVMTypeRef));

    emitter.blocks = arena_alloc(arena, (ir->block_count + 1) * sizeof(LLVMBasicBlockRef));

    LLVMTypeRef *params = arena_alloc(arena, (ir->param_count + 1) * sizeof(LLVMTypeRef));

    for (size_t i = 0; i < ir->param_count; i++) {
        params[i] = llvm_type_of(&emitter, mir_value_info(ir, ir->params[i])->type);
    }

    LLVMTypeRef returns = ir->function->return_type ? llvm_type_of(&emitter, ir->function->return_type)
                                                    : LLVMVoidTypeInContext(unit->context);

    LLVMTypeRef signature = LLVMFunctionType(returns, params, (unsigned)ir->param_count, false);

    const char *symbol = llvm_symbol_of(arena, ir->function);

    /* A callee declared before its body reached here already has the name, so it is filled in rather
     * than added a second time. */
    LLVMValueRef function = LLVMGetNamedFunction(unit->module, symbol);

    if (!function) {
        function = LLVMAddFunction(unit->module, symbol, signature);
    }

    for (size_t i = 0; i < ir->param_count; i++) {
        emitter.values[ir->params[i].id] = LLVMGetParam(function, (unsigned)i);
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        char label[32];
        snprintf(label, sizeof(label), "b%u", ir->blocks[b]->id.id);

        emitter.blocks[ir->blocks[b]->id.id] = LLVMAppendBasicBlockInContext(unit->context, function, label);
    }

    /* Slots live in a block of their own, so one allocated while a later block is being emitted still
     * runs before every use rather than after that block's terminator. */
    LLVMBasicBlockRef slots = LLVMAppendBasicBlockInContext(unit->context, function, "slots");

    emitter.entry = LLVMCreateBuilderInContext(unit->context);
    LLVMPositionBuilderAtEnd(emitter.entry, slots);

    /* A parameter reached through a projection needs an address, which an argument passed by value has
     * none of, so one that is projected is spilled to a slot of its own. */
    for (size_t i = 0; i < ir->param_count; i++) {
        const MIRValueInfo *info = mir_value_info(ir, ir->params[i]);

        if (!info || !info->type || type_kind(info->type) != TYPE_STRUCT) {
            continue;
        }

        LLVMTypeRef held = llvm_type_of(&emitter, info->type);
        LLVMValueRef slot = LLVMBuildAlloca(emitter.entry, held, "");

        LLVMBuildStore(emitter.entry, LLVMGetParam(function, (unsigned)i), slot);

        emitter.values[ir->params[i].id] = slot;
        emitter.value_types[ir->params[i].id] = held;
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        LLVMPositionBuilderAtEnd(unit->builder, emitter.blocks[block->id.id]);

        for (size_t i = 0; i < block->inst_count; i++) {
            emit_inst(&emitter, &block->insts[i]);
        }
    }

    /* A malformed body is a compiler bug, and the verifier names it here rather than at link time. */
    LLVMBuildBr(emitter.entry, emitter.blocks[ir->entry.id]);
    LLVMMoveBasicBlockBefore(slots, emitter.blocks[ir->entry.id]);

    LLVMDisposeBuilder(emitter.entry);

    char *invalid = NULL;

    /* The verifier allocates its message whether or not one is asked for, so it is always released. */
    bool verifies = LLVMVerifyModule(unit->module, LLVMReturnStatusAction, &invalid) == 0;

    if (!verifies) {
        fprintf(stderr, "%s: %s\n", symbol, invalid);
    }

    LLVMDisposeMessage(invalid);

    assert(verifies && "an emitted module verifies");
}

char *llvm_unit_text(LLVMUnit *unit) {
    char *printed = LLVMPrintModuleToString(unit->module);

    size_t length = strlen(printed);
    char *text = arena_alloc(unit->arena, length + 1);
    memcpy(text, printed, length + 1);

    LLVMDisposeMessage(printed);

    return text;
}

bool llvm_unit_write_object(LLVMUnit *unit, const char *path, const char **error) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = NULL;
    char *message = NULL;

    if (LLVMGetTargetFromTriple(triple, &target, &message) != 0) {
        *error = "no target for this host";
        LLVMDisposeMessage(message);
        LLVMDisposeMessage(triple);

        return false;
    }

    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        target, triple, "generic", "", LLVMCodeGenLevelNone, LLVMRelocPIC, LLVMCodeModelDefault);

    LLVMSetTarget(unit->module, triple);

    bool failed = LLVMTargetMachineEmitToFile(machine, unit->module, path, LLVMObjectFile, &message) != 0;

    if (failed) {
        *error = "the target machine emitted no object";
    }

    LLVMDisposeMessage(message);
    LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);

    return !failed;
}

void llvm_unit_close(LLVMUnit *unit) {
    LLVMDisposeBuilder(unit->builder);
    LLVMDisposeModule(unit->module);
    LLVMContextDispose(unit->context);
}

char *llvm_emit_function(Arena *arena, const MIRFunction *ir) {
    LLVMUnit *unit = llvm_unit_open(arena);

    llvm_unit_add(unit, ir);

    char *text = llvm_unit_text(unit);

    llvm_unit_close(unit);

    return text;
}
