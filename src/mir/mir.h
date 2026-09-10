#ifndef GAB_MIR_H
#define GAB_MIR_H

#include "constant.h"
#include "diagnostics.h"
#include "function_registry.h"
#include "memory/arena.h"
#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
} MIRValueId;

typedef struct {
    uint32_t id;
} MIRBlockId;

typedef struct {
    uint32_t id;
} MIRFieldId;

#define MIR_NO_VALUE ((MIRValueId){UINT32_MAX})
#define MIR_NO_BLOCK ((MIRBlockId){UINT32_MAX})
#define MIR_WHOLE_VALUE ((MIRFieldId){UINT32_MAX})

static inline bool mir_value_is_none(MIRValueId value) { return value.id == UINT32_MAX; }
static inline bool mir_block_is_none(MIRBlockId block) { return block.id == UINT32_MAX; }
static inline bool mir_field_is_whole(MIRFieldId field) { return field.id == UINT32_MAX; }

typedef enum {
    MIR_CONST_INT,
    MIR_CONST_FLOAT,
    MIR_CONST_BOOL,
    MIR_CONST_STR,

    MIR_ADD,
    MIR_SUB,
    MIR_MUL,
    MIR_DIV,
    MIR_MOD,
    MIR_NEG,

    MIR_CMP,

    MIR_NOT,

    MIR_ITOF,
    MIR_FTOI,

    MIR_LOAD,
    MIR_STORE,
    MIR_REF,

    MIR_BOUNDS,

    MIR_COPY,

    MIR_MAKE_SLICE,
    MIR_SLICE_LEN,

    MIR_CALL,

    MIR_NULL,
    MIR_BOX,

    MIR_DROP,

    MIR_DROP_FLAG,

    MIR_STORAGE_LIVE,
    MIR_STORAGE_DEAD,

    MIR_STORAGE_INIT,

    MIR_JMP,
    MIR_BRANCH,
    MIR_RETURN,
    MIR_UNREACHABLE,

    MIR__COUNT,
} MIROp;

typedef enum {
    MIR_CMP_LT,
    MIR_CMP_GT,
    MIR_CMP_EQ,
    MIR_CMP_NE,
    MIR_CMP_LE,
    MIR_CMP_GE,
} CmpPredicate;

typedef enum {
    PROJ_FIELD,
    PROJ_DEREF,
    PROJ_INDEX,
} ProjKind;

typedef struct {
    ProjKind kind;

    union {
        MIRFieldId field;
        MIRValueId index;
    };

    const Type *type;
} Projection;

typedef enum {
    OPERAND_VALUE,
    OPERAND_CONST,
} OperandKind;

typedef struct {
    OperandKind kind;

    union {
        MIRValueId value;
        Constant constant;
    };
} MIROperand;

static inline MIROperand mir_operand_value(MIRValueId value) {
    return (MIROperand){.kind = OPERAND_VALUE, .value = value};
}

static inline MIROperand mir_operand_const(Constant constant) {
    return (MIROperand){.kind = OPERAND_CONST, .constant = constant};
}

static inline MIRValueId mir_operand_as_value(MIROperand operand) {
    return operand.kind == OPERAND_VALUE ? operand.value : MIR_NO_VALUE;
}

typedef struct {
    MIRValueId base;

    Projection *projections;
    size_t projection_count;

    Symbol *binding;
} Place;

typedef enum {
    READ_COPY,
    READ_MOVE,
} ReadKind;

typedef struct {
    MIROp op;

    const Type *type;

    MIRValueId result;

    MIROperand *args;
    size_t arg_count;

    union {
        Constant constant;
        CmpPredicate predicate;
        struct {
            Place place;

            ReadKind read;

            MIRValueId flag;
        };

        Function *callee;

        MIRBlockId targets[2];
    };

    Function *ending;

    Span span;
} MIRInst;

typedef struct {
    MIRBlockId id;

    MIRInst *insts;
    size_t inst_count;
    size_t inst_capacity;
} MIRBlock;

typedef struct {
    const Type *type;

    Symbol *binding;

    Span span;
} MIRValueInfo;

typedef struct {
    Function *function;

    TypeRegistry *registry;

    FunctionRegistry *functions;

    MIRBlock **blocks;
    size_t block_count;
    size_t block_capacity;

    MIRBlockId entry;

    MIRValueInfo *values;
    size_t value_count;
    size_t value_capacity;

    MIRValueId *params;
    size_t param_count;

    Arena *arena;
} MIRFunction;

MIRFunction *mir_function_create(Arena *arena, TypeRegistry *registry, Function *function);

bool mir_function_is_template(const MIRFunction *ir);

MIRBlock *mir_block_create(MIRFunction *ir);
MIRBlock *mir_block_at(const MIRFunction *ir, MIRBlockId id);

MIRValueId mir_value_create(MIRFunction *ir, const Type *type, Symbol *binding, Span span);
const MIRValueInfo *mir_value_info(const MIRFunction *ir, MIRValueId value);

MIRInst *mir_emit(MIRFunction *ir, MIRBlock *block, MIRInst inst);

MIROperand *mir_args_alloc(MIRFunction *ir, size_t count);

Place mir_place_of(MIRValueId base, Symbol *binding);
Place mir_place_project(MIRFunction *ir, Place place, Projection projection);

Place mir_place_index(MIRFunction *ir, Place base, const Type *container, MIRValueId index,
                      const Type *element);

size_t mir_bounds_operands(Place indexed, const Type *container, MIRValueId index, MIRValueId out[2]);

const Type *mir_indexed_container(const Type *type);

bool mir_op_has_place(MIROp op);

bool mir_op_is_terminator(MIROp op);

bool mir_type_needs_drop(TypeRegistry *registry, const Type *type);

bool mir_block_is_terminated(const MIRBlock *block);

size_t mir_block_successors(const MIRBlock *block, MIRBlockId out[2]);

const char *mir_op_name(MIROp op);
const char *mir_cmp_predicate_name(CmpPredicate predicate);

#endif
