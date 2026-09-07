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
    case TYPE_REF:
        return LLVMPointerTypeInContext(emitter->context, 0);

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

/* The address a place names, walked from its base through each projection. A base with no storage of
 * its own is a temporary the lowering stores into, which is given a slot the first time it is named. */
static LLVMValueRef place_address(LLVMEmitter *emitter, const Place *place, LLVMTypeRef *out_type) {
    if (!emitter->value_types[place->base.id] && !emitter->values[place->base.id]) {
        const MIRValueInfo *info = mir_value_info(emitter->ir, place->base);

        LLVMTypeRef held = llvm_type_of(emitter, info ? info->type : NULL);

        emitter->value_types[place->base.id] = held;
        emitter->values[place->base.id] = LLVMBuildAlloca(emitter->entry, held, "");
    }

    LLVMValueRef address = emitter->values[place->base.id];
    LLVMTypeRef type = emitter->value_types[place->base.id];

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
                address = LLVMBuildLoad2(emitter->builder, LLVMPointerTypeInContext(emitter->context, 0),
                                         address, "");
            }
            break;

        case PROJ_INDEX: {
            LLVMValueRef indices[2] = {
                LLVMConstInt(LLVMInt32TypeInContext(emitter->context), 0, false),
                emitter->values[projection->index.id],
            };

            address = LLVMBuildGEP2(emitter->builder, type, address, indices, 2, "");
            break;
        }
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

    case MIR_FTOI:
        values[inst->result.id] = LLVMBuildFPToSI(builder, operand_value(emitter, inst->args[0]),
                                                  LLVMInt32TypeInContext(emitter->context), "");
        break;

    case MIR_COPY:
        values[inst->result.id] = operand_value(emitter, inst->args[0]);
        break;

    case MIR_STORAGE_LIVE: {
        const MIRValueInfo *info = mir_value_info(emitter->ir, inst->place.base);

        LLVMTypeRef held = llvm_type_of(emitter, info ? info->type : NULL);

        emitter->value_types[inst->place.base.id] = held;
        values[inst->place.base.id] = LLVMBuildAlloca(emitter->entry, held, "");
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

    assert(LLVMVerifyModule(unit->module, LLVMReturnStatusAction, NULL) == 0 && "an emitted module verifies");
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
