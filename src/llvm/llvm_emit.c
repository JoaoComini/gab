#include "llvm/llvm_emit.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>

#include <assert.h>
#include <string.h>

typedef struct {
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;

    const MIRFunction *ir;

    /* What each virtual register holds, indexed by its id. */
    LLVMValueRef *values;

    LLVMBasicBlockRef *blocks;
} LLVMEmitter;

static LLVMTypeRef llvm_type_of(LLVMEmitter *emitter, const Type *type) {
    if (!type) {
        return LLVMInt32TypeInContext(emitter->context);
    }

    switch (type_kind(type)) {
    case TYPE_FLOAT:
        return LLVMFloatTypeInContext(emitter->context);
    case TYPE_BOOL:
        return LLVMInt1TypeInContext(emitter->context);
    case TYPE_BYTE:
        return LLVMInt8TypeInContext(emitter->context);
    default:
        return LLVMInt32TypeInContext(emitter->context);
    }
}

static LLVMValueRef operand_value(LLVMEmitter *emitter, MIROperand operand) {
    if (operand.kind == OPERAND_CONST) {
        LLVMTypeRef type = llvm_type_of(emitter, operand.constant.type);

        if (operand.constant.type && type_kind(operand.constant.type) == TYPE_FLOAT) {
            return LLVMConstReal(type, (double)operand.constant.as_float);
        }

        return LLVMConstInt(type, (unsigned long long)operand.constant.as_int, true);
    }

    return emitter->values[operand.value.id];
}

/* Whether an instruction's operands are floating, which the opcode alone does not say. */
static bool operands_are_float(LLVMEmitter *emitter, const MIRInst *inst) {
    if (inst->arg_count == 0) {
        return false;
    }

    if (inst->args[0].kind == OPERAND_CONST) {
        return inst->args[0].constant.type && type_kind(inst->args[0].constant.type) == TYPE_FLOAT;
    }

    const MIRValueInfo *info = mir_value_info(emitter->ir, inst->args[0].value);

    return info && info->type && type_kind(info->type) == TYPE_FLOAT;
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

    case MIR_JMP:
        LLVMBuildBr(builder, emitter->blocks[inst->targets[0].id]);
        break;

    case MIR_BRANCH:
        LLVMBuildCondBr(builder, operand_value(emitter, inst->args[0]), emitter->blocks[inst->targets[0].id],
                        emitter->blocks[inst->targets[1].id]);
        break;

    case MIR_RETURN:
        if (inst->arg_count == 0) {
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

char *llvm_emit_function(Arena *arena, const MIRFunction *ir) {
    LLVMEmitter emitter = {.ir = ir};

    emitter.context = LLVMContextCreate();
    emitter.module = LLVMModuleCreateWithNameInContext("gab", emitter.context);
    emitter.builder = LLVMCreateBuilderInContext(emitter.context);

    emitter.values = arena_alloc(arena, (ir->value_count + 1) * sizeof(LLVMValueRef));
    emitter.blocks = arena_alloc(arena, (ir->block_count + 1) * sizeof(LLVMBasicBlockRef));

    LLVMTypeRef *params = arena_alloc(arena, (ir->param_count + 1) * sizeof(LLVMTypeRef));

    for (size_t i = 0; i < ir->param_count; i++) {
        params[i] = llvm_type_of(&emitter, mir_value_info(ir, ir->params[i])->type);
    }

    LLVMTypeRef signature = LLVMFunctionType(llvm_type_of(&emitter, ir->function->return_type), params,
                                             (unsigned)ir->param_count, false);

    LLVMValueRef function = LLVMAddFunction(emitter.module, ir->function->decl->name->data, signature);

    for (size_t i = 0; i < ir->param_count; i++) {
        emitter.values[ir->params[i].id] = LLVMGetParam(function, (unsigned)i);
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        char label[32];
        snprintf(label, sizeof(label), "b%u", ir->blocks[b]->id.id);

        emitter.blocks[ir->blocks[b]->id.id] =
            LLVMAppendBasicBlockInContext(emitter.context, function, label);
    }

    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        LLVMPositionBuilderAtEnd(emitter.builder, emitter.blocks[block->id.id]);

        for (size_t i = 0; i < block->inst_count; i++) {
            emit_inst(&emitter, &block->insts[i]);
        }
    }

    /* A malformed body is a compiler bug, and the verifier names it here rather than at link time. */
    assert(LLVMVerifyModule(emitter.module, LLVMReturnStatusAction, NULL) == 0 &&
           "an emitted module verifies");

    char *printed = LLVMPrintModuleToString(emitter.module);

    size_t length = strlen(printed);
    char *text = arena_alloc(arena, length + 1);
    memcpy(text, printed, length + 1);

    LLVMDisposeMessage(printed);
    LLVMDisposeBuilder(emitter.builder);
    LLVMDisposeModule(emitter.module);
    LLVMContextDispose(emitter.context);

    return text;
}
