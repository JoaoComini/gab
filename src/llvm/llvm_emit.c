#include "llvm/llvm_emit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Arena *arena;

    char *text;
    size_t length;
    size_t capacity;

    TypeRegistry *registry;

    const MIRFunction *ir;
} LLVMEmitter;

static void emit_text(LLVMEmitter *emitter, const char *format, ...) {
    va_list args;
    va_start(args, format);

    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(NULL, 0, format, measure);
    va_end(measure);

    if (needed < 0) {
        va_end(args);
        return;
    }

    if (emitter->length + (size_t)needed + 1 > emitter->capacity) {
        size_t capacity = emitter->capacity ? emitter->capacity * 2 : 256;

        while (capacity < emitter->length + (size_t)needed + 1) {
            capacity *= 2;
        }

        char *grown = arena_alloc(emitter->arena, capacity);

        if (emitter->length) {
            memcpy(grown, emitter->text, emitter->length);
        }

        emitter->text = grown;
        emitter->capacity = capacity;
    }

    vsnprintf(emitter->text + emitter->length, emitter->capacity - emitter->length, format, args);
    emitter->length += (size_t)needed;

    va_end(args);
}

/* A scalar's LLVM type, which its width and whether it is floating decide. */
static const char *llvm_type_of(const Type *type) {
    if (!type) {
        return "i32";
    }

    switch (type_kind(type)) {
    case TYPE_INT:
        return "i32";
    case TYPE_FLOAT:
        return "float";
    case TYPE_BOOL:
        return "i1";
    case TYPE_BYTE:
        return "i8";
    default:
        return "i32";
    }
}

/* A value is named by its virtual register, which SSA lets LLVM take verbatim. */
static void emit_operand(LLVMEmitter *emitter, MIROperand operand) {
    if (operand.kind == OPERAND_CONST) {
        if (type_kind(operand.constant.type) == TYPE_FLOAT) {
            emit_text(emitter, "%f", (double)operand.constant.as_float);
        } else {
            emit_text(emitter, "%d", operand.constant.as_int);
        }

        return;
    }

    emit_text(emitter, "%%%u", operand.value.id);
}

static const char *binary_mnemonic(MIROp op, bool floating) {
    switch (op) {
    case MIR_ADD:
        return floating ? "fadd" : "add";
    case MIR_SUB:
        return floating ? "fsub" : "sub";
    case MIR_MUL:
        return floating ? "fmul" : "mul";
    case MIR_DIV:
        return floating ? "fdiv" : "sdiv";
    case MIR_MOD:
        return floating ? "frem" : "srem";
    default:
        return NULL;
    }
}

/* An integer predicate is signed, since every integer this emits is. */
static const char *predicate_mnemonic(CmpPredicate predicate, bool floating) {
    switch (predicate) {
    case MIR_CMP_LT:
        return floating ? "olt" : "slt";
    case MIR_CMP_GT:
        return floating ? "ogt" : "sgt";
    case MIR_CMP_EQ:
        return floating ? "oeq" : "eq";
    case MIR_CMP_NE:
        return floating ? "one" : "ne";
    case MIR_CMP_LE:
        return floating ? "ole" : "sle";
    case MIR_CMP_GE:
        return floating ? "oge" : "sge";
    }

    return "eq";
}

static void emit_inst(LLVMEmitter *emitter, const MIRInst *inst) {
    const char *type = llvm_type_of(inst->type);

    bool floating = inst->type && type_kind(inst->type) == TYPE_FLOAT;

    switch (inst->op) {
    case MIR_CONST_INT:
    case MIR_CONST_BOOL:
        /* LLVM names no constant, so one is materialised by an operation that yields it. */
        emit_text(emitter, "  %%%u = add %s 0, %d\n", inst->result.id, type, inst->constant.as_int);
        break;

    case MIR_CONST_FLOAT:
        emit_text(emitter, "  %%%u = fadd %s 0.0, %f\n", inst->result.id, type,
                  (double)inst->constant.as_float);
        break;

    case MIR_ADD:
    case MIR_SUB:
    case MIR_MUL:
    case MIR_DIV:
    case MIR_MOD:
        emit_text(emitter, "  %%%u = %s %s ", inst->result.id, binary_mnemonic(inst->op, floating), type);
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, ", ");
        emit_operand(emitter, inst->args[1]);
        emit_text(emitter, "\n");
        break;

    case MIR_NEG:
        emit_text(emitter, "  %%%u = %s %s ", inst->result.id, floating ? "fneg" : "sub", type);

        if (!floating) {
            emit_text(emitter, "0, ");
        }

        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, "\n");
        break;

    case MIR_NOT:
        emit_text(emitter, "  %%%u = xor i1 ", inst->result.id);
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, ", true\n");
        break;

    case MIR_CMP: {
        const MIRValueInfo *left = mir_value_info(emitter->ir, mir_operand_as_value(inst->args[0]));

        bool compares_floats = left && type_kind(left->type) == TYPE_FLOAT;

        emit_text(emitter, "  %%%u = %s %s %s ", inst->result.id, compares_floats ? "fcmp" : "icmp",
                  predicate_mnemonic(inst->predicate, compares_floats),
                  left ? llvm_type_of(left->type) : "i32");
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, ", ");
        emit_operand(emitter, inst->args[1]);
        emit_text(emitter, "\n");
        break;
    }

    case MIR_ITOF:
        emit_text(emitter, "  %%%u = sitofp i32 ", inst->result.id);
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, " to float\n");
        break;

    case MIR_FTOI:
        emit_text(emitter, "  %%%u = fptosi float ", inst->result.id);
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, " to i32\n");
        break;

    case MIR_COPY:
        emit_text(emitter, "  %%%u = add %s 0, ", inst->result.id, type);
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, "\n");
        break;

    case MIR_JMP:
        emit_text(emitter, "  br label %%b%u\n", inst->targets[0].id);
        break;

    case MIR_BRANCH:
        emit_text(emitter, "  br i1 ");
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, ", label %%b%u, label %%b%u\n", inst->targets[0].id, inst->targets[1].id);
        break;

    case MIR_RETURN:
        if (inst->arg_count == 0) {
            emit_text(emitter, "  ret void\n");
            break;
        }

        emit_text(emitter, "  ret %s ", llvm_type_of(inst->type));
        emit_operand(emitter, inst->args[0]);
        emit_text(emitter, "\n");
        break;

    case MIR_UNREACHABLE:
        emit_text(emitter, "  unreachable\n");
        break;

    default:
        emit_text(emitter, "  ; unhandled %s\n", mir_op_name(inst->op));
        break;
    }
}

char *llvm_emit_function(Arena *arena, const MIRFunction *ir) {
    LLVMEmitter emitter = {.arena = arena, .registry = ir->registry, .ir = ir};

    const char *name = ir->function->decl->name->data;

    emit_text(&emitter, "define %s @%s(", llvm_type_of(ir->function->return_type), name);

    for (size_t i = 0; i < ir->param_count; i++) {
        const MIRValueInfo *info = mir_value_info(ir, ir->params[i]);

        emit_text(&emitter, "%s%s %%%u", i ? ", " : "", llvm_type_of(info->type), ir->params[i].id);
    }

    emit_text(&emitter, ") {\n");

    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        emit_text(&emitter, "b%u:\n", block->id.id);

        for (size_t i = 0; i < block->inst_count; i++) {
            emit_inst(&emitter, &block->insts[i]);
        }
    }

    emit_text(&emitter, "}\n");

    return emitter.text ? emitter.text : (char *)"";
}
