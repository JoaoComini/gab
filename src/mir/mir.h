#ifndef GAB_MIR_H
#define GAB_MIR_H

#include "decl.h"
#include "constant.h"
#include "diagnostics.h"
#include "function_registry.h"
#include "memory/arena.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A virtual register, of which a body names as many as it needs. */
typedef struct {
    uint32_t id;
} MIRValueId;

typedef struct {
    uint32_t id;
} MIRBlockId;

/* One field of a struct, or the whole value where it names none. */
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

    /* Carries a predicate rather than splitting per comparison, since the operand types pick the opcode. */
    MIR_CMP,

    MIR_NOT,

    MIR_ITOF,
    MIR_FTOI,

    MIR_LOAD,
    MIR_STORE,
    MIR_REF,

    /* Traps unless the index is below the length, emitted ahead of the place that indexes. */
    MIR_BOUNDS,

    MIR_COPY,

    MIR_MAKE_SLICE,
    MIR_SLICE_LEN,

    MIR_CALL,

    /* Leaves a place holding nothing, so releasing it afterwards frees nothing. */
    MIR_NULL,
    MIR_BOX,
    /* What a place owns is released here, which the shape of its type says how to walk. */
    MIR_DROP,

    /* Says whether a slot still holds its object, for one whose paths disagree; the drop reads it. */
    MIR_DROP_FLAG,

    /* A local enters scope holding nothing, and leaves scope where its storage ends. */
    MIR_STORAGE_LIVE,
    MIR_STORAGE_DEAD,

    /* The place holds a value from here, for one filled field by field rather than by a store. */
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

    /* What projecting yields, so a place names its own type without walking back to the base. */
    const Type *type;
} Projection;

typedef enum {
    OPERAND_VALUE,
    OPERAND_CONST,
} OperandKind;

/* What an instruction reads: a value it must find in a slot, or a constant it can name outright.
 * A constant named here takes no virtual register, so it costs the allocator nothing. */
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

/* The value an operand reads, or none where it names a constant instead. */
static inline MIRValueId mir_operand_as_value(MIROperand operand) {
    return operand.kind == OPERAND_VALUE ? operand.value : MIR_NO_VALUE;
}

/* An lvalue: a base value and the path taken through it, which analysis reads without rebuilding it. */
typedef struct {
    MIRValueId base;

    Projection *projections;
    size_t projection_count;

    /* The local the base names, where it names one; NULL when the base is a computed address. */
    Binding *binding;
} Place;

/* Whether reading a place leaves it holding its value, which is what makes a read a move. */
typedef enum {
    READ_COPY,
    READ_MOVE,
} ReadKind;

typedef struct {
    MIROp op;

    /* What the result holds, which gives every instruction its width and its drop. */
    const Type *type;

    MIRValueId result;

    MIROperand *args;
    size_t arg_count;

    union {
        Constant constant;
        CmpPredicate predicate;
        struct {
            Place place;

            /* 'MIR_LOAD' only: a moving read leaves the place holding nothing. */
            ReadKind read;

            /* 'MIR_DROP' and 'MIR_DROP_FLAG': the flag this drop is guarded by, none where it is not. */
            MIRValueId flag;
        };

        Function *callee;

        /* 'MIR_JMP' names one; 'MIR_BRANCH' takes the first when its argument is true. */
        MIRBlockId targets[2];
    };

    /* 'MIR_DROP': the ending the dropped type declares, specialized for it, none where it declares none.
     * Resolved here rather than where the drop is emitted, so an instance of it is lowered like any callee.
     */
    Function *ending;

    Span span;
} MIRInst;

typedef struct {
    MIRBlockId id;

    MIRInst *insts;
    size_t inst_count;
    size_t inst_capacity;
} MIRBlock;

/* A virtual register's origin, which diagnostics name and the register allocator sizes. */
typedef struct {
    const Type *type;

    Binding *binding;

    Span span;
} MIRValueInfo;

typedef struct {
    Function *function;

    /* What a type holds, which the IR asks for field counts and widths rather than storing them. */
    TypeRegistry *registry;

    /* Set as drops are elaborated, so emission can find an ending no instruction resolved. */
    FunctionRegistry *functions;

    MIRBlock **blocks;
    size_t block_count;
    size_t block_capacity;

    MIRBlockId entry;

    MIRValueInfo *values;
    size_t value_count;
    size_t value_capacity;

    /* The value each parameter was given, in declaration order. */
    MIRValueId *params;
    size_t param_count;

    Arena *arena;
} MIRFunction;

MIRFunction *mir_function_create(Arena *arena, TypeRegistry *registry, Function *function);

/* Whether the body is a generic's own, which stands for its instances and has no code of its own. */
bool mir_function_is_template(const MIRFunction *ir);

MIRBlock *mir_block_create(MIRFunction *ir);
MIRBlock *mir_block_at(const MIRFunction *ir, MIRBlockId id);

MIRValueId mir_value_create(MIRFunction *ir, const Type *type, Binding *binding, Span span);
const MIRValueInfo *mir_value_info(const MIRFunction *ir, MIRValueId value);

/* Appends an instruction, returning the one now owned by the block so a caller can fill its payload. */
MIRInst *mir_emit(MIRFunction *ir, MIRBlock *block, MIRInst inst);

MIROperand *mir_args_alloc(MIRFunction *ir, size_t count);

Place mir_place_of(MIRValueId base, Binding *binding);
Place mir_place_project(MIRFunction *ir, Place place, Projection projection);

/* The place 'base[index]' names, hopping through a pointer where the container sits behind one. */
Place mir_place_index(MIRFunction *ir, Place base, const Type *container, MIRValueId index,
                      const Type *element);

/* What bounds-checking that access reads: the index, and a slice's own length where it states one. */
size_t mir_bounds_operands(Place indexed, const Type *container, MIRValueId index, MIRValueId out[2]);

/* The container an indexed place sits in, which is what its type says once pointers are followed. */
const Type *mir_indexed_container(const Type *type);

/* Whether an instruction's payload is a place, which the union makes unsafe to read otherwise. */
bool mir_op_has_place(MIROp op);

bool mir_op_is_terminator(MIROp op);

/* Whether ending a value of this type releases anything, which is what makes a drop worth marking. */
bool mir_type_needs_drop(TypeRegistry *registry, const Type *type);

/* Whether a block ends in a terminator, which every well-formed block must. */
bool mir_block_is_terminated(const MIRBlock *block);

/* The blocks a terminator can reach, written into 'out' and counted by the return. */
size_t mir_block_successors(const MIRBlock *block, MIRBlockId out[2]);

const char *mir_op_name(MIROp op);
const char *mir_cmp_predicate_name(CmpPredicate predicate);

#endif
