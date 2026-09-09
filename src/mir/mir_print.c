#include "mir/mir_print.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    FILE *file;

    char *buffer;
    size_t capacity;

    /* What the text would have taken, which keeps growing past the buffer so a caller can size one. */
    size_t written;

    TypeRegistry *registry;
} MIRPrinter;

static void mir_printf(MIRPrinter *printer, const char *format, ...) {
    va_list args;
    va_start(args, format);

    if (printer->file) {
        vfprintf(printer->file, format, args);
        va_end(args);
        return;
    }

    size_t left = printer->written < printer->capacity ? printer->capacity - printer->written : 0;

    int n = vsnprintf(printer->buffer + printer->written, left, format, args);

    va_end(args);

    if (n > 0) {
        printer->written += (size_t)n;
    }
}

static void mir_print_type(MIRPrinter *printer, const Type *type) {
    if (!type) {
        mir_printf(printer, "?");
        return;
    }

    switch (type_kind(type)) {
    case TYPE_REF:
        mir_printf(printer, "&");
        mir_print_type(printer, type_pointee(type));
        return;
    case TYPE_RAW:
    case TYPE_BOX:
        mir_printf(printer, "*");
        mir_print_type(printer, type_pointee(type));
        return;
    case TYPE_SLICE:
        mir_printf(printer, "slice<");
        mir_print_type(printer, type_slice_element(type));
        mir_printf(printer, ">");
        return;
    default:
        break;
    }

    const String *name = type_name_of(type);

    mir_printf(printer, "%s", name ? name->data : "?");
}

static void mir_print_value(MIRPrinter *printer, MIRValueId value) {
    if (mir_value_is_none(value)) {
        mir_printf(printer, "_");
        return;
    }

    mir_printf(printer, "%%%u", value.id);
}

/* A constant says what it is, so printing one asks its type rather than the instruction holding it. */
static void mir_print_constant(MIRPrinter *printer, Constant constant) {
    if (constant_is_float(constant)) {
        mir_printf(printer, "%g", (double)constant.as_float);
        return;
    }

    if (constant_is_bool(constant)) {
        mir_printf(printer, "%s", constant.as_bool ? "true" : "false");
        return;
    }

    if (constant_is_string(constant)) {
        mir_printf(printer, "\"%s\"", constant.as_string ? constant.as_string->data : "");
        return;
    }

    mir_printf(printer, "%" PRId64, constant.as_int);
}

static void mir_print_operand(MIRPrinter *printer, MIROperand operand) {
    if (operand.kind == OPERAND_VALUE) {
        mir_print_value(printer, operand.value);
        return;
    }

    mir_print_constant(printer, operand.constant);
}

static void mir_print_place(MIRPrinter *printer, const Place *place) {
    mir_print_value(printer, place->base);

    for (size_t i = 0; i < place->projection_count; i++) {
        const Projection *projection = &place->projections[i];

        switch (projection->kind) {
        case PROJ_FIELD:
            mir_printf(printer, ".%zu", projection->field);
            break;
        case PROJ_DEREF:
            mir_printf(printer, ".*");
            break;
        case PROJ_INDEX:
            mir_printf(printer, "[");
            mir_print_value(printer, projection->index);
            mir_printf(printer, "]");
            break;
        }
    }
}

static void mir_print_args(MIRPrinter *printer, const MIRInst *inst) {
    for (size_t i = 0; i < inst->arg_count; i++) {
        mir_printf(printer, i == 0 ? " " : ", ");
        mir_print_operand(printer, inst->args[i]);
    }
}

static void mir_print_inst(MIRPrinter *printer, const MIRInst *inst) {
    mir_printf(printer, "    ");

    if (!mir_value_is_none(inst->result)) {
        mir_print_value(printer, inst->result);
        mir_printf(printer, ": ");
        mir_print_type(printer, inst->type);
        mir_printf(printer, " = ");
    }

    mir_printf(printer, "%s", mir_op_name(inst->op));

    switch (inst->op) {
    case MIR_CONST_INT:
        mir_printf(printer, " %" PRId64, inst->constant.as_int);
        break;
    case MIR_CONST_FLOAT:
        mir_printf(printer, " %g", (double)inst->constant.as_float);
        break;
    case MIR_CONST_BOOL:
    case MIR_CONST_STR:
        mir_printf(printer, " ");
        mir_print_constant(printer, inst->constant);
        break;

    case MIR_CMP:
        mir_printf(printer, ".%s", mir_cmp_predicate_name(inst->predicate));
        mir_print_args(printer, inst);
        break;

    case MIR_LOAD:
        mir_printf(printer, inst->read == READ_MOVE ? ".move " : " ");
        mir_print_place(printer, &inst->place);
        break;

    case MIR_REF:
    case MIR_DROP:
    case MIR_NULL:
    case MIR_STORAGE_LIVE:
    case MIR_STORAGE_DEAD:
    case MIR_STORAGE_INIT:
        mir_printf(printer, " ");
        mir_print_place(printer, &inst->place);
        break;

    case MIR_STORE:
        mir_printf(printer, " ");
        mir_print_place(printer, &inst->place);
        mir_printf(printer, ",");
        mir_print_args(printer, inst);
        break;

    case MIR_CALL: {
        const FuncDecl *decl = inst->callee ? inst->callee->decl : NULL;

        mir_printf(printer, " %s", decl && decl->id.name ? decl->id.name->data : "?");
        mir_print_args(printer, inst);
        break;
    }

    case MIR_JMP:
        mir_printf(printer, " bb%u", inst->targets[0].id);
        break;

    case MIR_BRANCH:
        mir_print_args(printer, inst);
        mir_printf(printer, ", bb%u, bb%u", inst->targets[0].id, inst->targets[1].id);
        break;

    default:
        mir_print_args(printer, inst);
        break;
    }

    mir_printf(printer, "\n");
}

static void mir_print_signature(MIRPrinter *printer, const MIRFunction *ir) {
    const FuncDecl *decl = ir->function ? ir->function->decl : NULL;

    mir_printf(printer, "func %s(", decl && decl->id.name ? decl->id.name->data : "?");

    for (size_t i = 0; i < ir->param_count; i++) {
        const MIRValueInfo *info = mir_value_info(ir, ir->params[i]);

        mir_printf(printer, i == 0 ? "" : ", ");
        mir_print_value(printer, ir->params[i]);
        mir_printf(printer, ": ");
        mir_print_type(printer, info ? info->type : NULL);
    }

    mir_printf(printer, ")");

    if (ir->function && ir->function->signature.return_type) {
        mir_printf(printer, ": ");
        mir_print_type(printer, ir->function->signature.return_type);
    }

    mir_printf(printer, " {\n");
}

static void mir_print_function(MIRPrinter *printer, const MIRFunction *ir) {
    mir_print_signature(printer, ir);

    for (size_t i = 0; i < ir->block_count; i++) {
        const MIRBlock *block = ir->blocks[i];

        mir_printf(printer, "  bb%u:\n", block->id.id);

        for (size_t j = 0; j < block->inst_count; j++) {
            mir_print_inst(printer, &block->insts[j]);
        }
    }

    mir_printf(printer, "}\n");
}

void mir_print(const MIRFunction *ir, TypeRegistry *registry, FILE *out) {
    MIRPrinter printer = {.file = out, .registry = registry};

    mir_print_function(&printer, ir);
}

size_t mir_print_to_buffer(const MIRFunction *ir, TypeRegistry *registry, char *buffer, size_t capacity) {
    MIRPrinter printer = {.buffer = buffer, .capacity = capacity, .registry = registry};

    mir_print_function(&printer, ir);

    if (capacity > 0) {
        buffer[printer.written < capacity ? printer.written : capacity - 1] = '\0';
    }

    return printer.written;
}
