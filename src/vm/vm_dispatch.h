#ifndef GAB_VM_DISPATCH_H
#define GAB_VM_DISPATCH_H

#include <assert.h>

#include "vm/opcode.h"

/* The interpreter's guards are all cold: telling the compiler so moves them out of the dispatch loop's
 * straight-line path, which is worth more than the branch prediction it also buys. */
#if defined(__GNUC__) || defined(__clang__)
#define VM_UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#define VM_LIKELY(cond) __builtin_expect(!!(cond), 1)
#else
#define VM_UNLIKELY(cond) (cond)
#define VM_LIKELY(cond) (cond)
#endif

#if defined(GAB_FORCE_SWITCH)
#define VM_COMPUTED_GOTO 0
#elif defined(__GNUC__) || defined(__clang__)
#define VM_COMPUTED_GOTO 1
#else
#define VM_COMPUTED_GOTO 0
#endif

#define VM_SAVE_IP() (vm->instruction_pointer = pc)
#define VM_LOAD_IP() (pc = vm->instruction_pointer)

#define VM_LOAD_REGS() (regs = vm->stack + (vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].base : 0))

#define VM_REG(r) (regs + (r) * VM_SLOT_SIZE)

#define VM_RELOAD()                                                                                          \
    do {                                                                                                     \
        if (vm->frame_count > 0) {                                                                           \
            chunk = vm->frames[vm->frame_count - 1].proto->chunk;                                            \
        }                                                                                                    \
                                                                                                             \
        VM_LOAD_REGS();                                                                                      \
        VM_LOAD_IP();                                                                                        \
    } while (0)

#define VM_FETCH()                                                                                           \
    do {                                                                                                     \
        if (vm->frame_count == frame_floor) {                                                                \
            goto vm_done;                                                                                    \
        }                                                                                                    \
                                                                                                             \
        assert(pc >= chunk->instructions.data && pc < chunk->instructions.data + chunk->instructions.size && \
               "a jump left the chunk");                                                                     \
                                                                                                             \
        instruction = *pc++;                                                                                 \
        op = VM_DECODE_OPCODE(instruction);                                                                  \
    } while (0)

#define VM_FETCH_NEXT()                                                                                      \
    do {                                                                                                     \
        assert(vm->frame_count > 0 && pc >= chunk->instructions.data &&                                      \
               pc < chunk->instructions.data + chunk->instructions.size &&                                   \
               "a straight-line step left the chunk; it needed VM_JUMPED or VM_RETRY");                      \
                                                                                                             \
        instruction = *pc++;                                                                                 \
        op = VM_DECODE_OPCODE(instruction);                                                                  \
    } while (0)

#if VM_COMPUTED_GOTO

#define VM_DISPATCH(o) goto *vm_dispatch_table[o];
#define VM_CASE(name) name##_label:

#define VM_CASE_UNREACHABLE(name)                                                                            \
    if (0)                                                                                                   \
        goto vm_done;

#define VM_NEXT()                                                                                            \
    do {                                                                                                     \
        VM_FETCH_NEXT();                                                                                     \
        VM_DISPATCH(op)                                                                                      \
    } while (0)

#define VM_JUMPED()                                                                                          \
    do {                                                                                                     \
        VM_FETCH();                                                                                          \
        VM_DISPATCH(op)                                                                                      \
    } while (0)

#define VM_RETRY()                                                                                           \
    do {                                                                                                     \
        VM_RELOAD();                                                                                         \
        VM_FETCH();                                                                                          \
        VM_DISPATCH(op)                                                                                      \
    } while (0)

#else

#define VM_DISPATCH(o) switch (o)
#define VM_CASE(name) case name:
#define VM_CASE_UNREACHABLE(name)                                                                            \
    case name:                                                                                               \
        goto vm_done;

#define VM_NEXT()                                                                                            \
    {                                                                                                        \
        break;                                                                                               \
    }

#define VM_JUMPED()                                                                                          \
    {                                                                                                        \
        break;                                                                                               \
    }

#define VM_RETRY()                                                                                           \
    {                                                                                                        \
        VM_RELOAD();                                                                                         \
        break;                                                                                               \
    }

#endif

#if VM_COMPUTED_GOTO

#define VM_LOOP()                                                                                            \
    static void *const vm_dispatch_table[] = {                                                               \
        [OP_LOAD_CONST] = &&OP_LOAD_CONST_label,                                                             \
        [OP_LOAD_STR] = &&OP_LOAD_STR_label,                                                                 \
        [OP_LOAD_TRUE] = &&OP_LOAD_TRUE_label,                                                               \
        [OP_LOAD_FALSE] = &&OP_LOAD_FALSE_label,                                                             \
        [OP_MOVE] = &&OP_MOVE_label,                                                                         \
        [OP_MOVE_N] = &&OP_MOVE_N_label,                                                                     \
        [OP_ADDI] = &&OP_ADDI_label,                                                                         \
        [OP_SUBI] = &&OP_SUBI_label,                                                                         \
        [OP_MULI] = &&OP_MULI_label,                                                                         \
        [OP_DIVI] = &&OP_DIVI_label,                                                                         \
        [OP_MODI] = &&OP_MODI_label,                                                                         \
        [OP_NEGI] = &&OP_NEGI_label,                                                                         \
        [OP_NEGF] = &&OP_NEGF_label,                                                                         \
        [OP_ITOF] = &&OP_ITOF_label,                                                                         \
        [OP_FTOI] = &&OP_FTOI_label,                                                                         \
        [OP_CMP_LTI] = &&OP_CMP_LTI_label,                                                                   \
        [OP_CMP_GTI] = &&OP_CMP_GTI_label,                                                                   \
        [OP_CMP_EQI] = &&OP_CMP_EQI_label,                                                                   \
        [OP_CMP_NEI] = &&OP_CMP_NEI_label,                                                                   \
        [OP_CMP_LEI] = &&OP_CMP_LEI_label,                                                                   \
        [OP_CMP_GEI] = &&OP_CMP_GEI_label,                                                                   \
        [OP_ADDF] = &&OP_ADDF_label,                                                                         \
        [OP_SUBF] = &&OP_SUBF_label,                                                                         \
        [OP_MULF] = &&OP_MULF_label,                                                                         \
        [OP_DIVF] = &&OP_DIVF_label,                                                                         \
        [OP_ADDFK] = &&OP_ADDFK_label,                                                                       \
        [OP_SUBFK] = &&OP_SUBFK_label,                                                                       \
        [OP_MULFK] = &&OP_MULFK_label,                                                                       \
        [OP_DIVFK] = &&OP_DIVFK_label,                                                                       \
        [OP_CMP_LTF] = &&OP_CMP_LTF_label,                                                                   \
        [OP_CMP_GTF] = &&OP_CMP_GTF_label,                                                                   \
        [OP_CMP_EQS] = &&OP_CMP_EQS_label,                                                                   \
        [OP_CMP_NES] = &&OP_CMP_NES_label,                                                                   \
        [OP_CMP_EQF] = &&OP_CMP_EQF_label,                                                                   \
        [OP_CMP_NEF] = &&OP_CMP_NEF_label,                                                                   \
        [OP_CMP_LEF] = &&OP_CMP_LEF_label,                                                                   \
        [OP_CMP_GEF] = &&OP_CMP_GEF_label,                                                                   \
        [OP_CMP_LTFK] = &&OP_CMP_LTFK_label,                                                                 \
        [OP_CMP_GTFK] = &&OP_CMP_GTFK_label,                                                                 \
        [OP_CMP_LEFK] = &&OP_CMP_LEFK_label,                                                                 \
        [OP_CMP_GEFK] = &&OP_CMP_GEFK_label,                                                                 \
        [OP_CMP_EQFK] = &&OP_CMP_EQFK_label,                                                                 \
        [OP_CMP_NEFK] = &&OP_CMP_NEFK_label,                                                                 \
        [OP_JMP] = &&OP_JMP_label,                                                                           \
        [OP_JMP_IF_FALSE] = &&OP_JMP_IF_FALSE_label,                                                         \
        [OP_JMP_IF_TRUE] = &&OP_JMP_IF_TRUE_label,                                                           \
        [OP_CALL] = &&OP_CALL_label,                                                                         \
        [OP_CALL_EXTERN] = &&OP_CALL_EXTERN_label,                                                           \
        [OP_BOX] = &&OP_BOX_label,                                                                           \
        [OP_NULL] = &&OP_NULL_label,                                                                         \
        [OP_RELEASE] = &&OP_RELEASE_label,                                                                   \
        [OP_RETURN] = &&OP_RETURN_label,                                                                     \
        [OP_RETURN_N] = &&OP_RETURN_N_label,                                                                 \
        [OP_LOAD_FIELD_1] = &&OP_LOAD_FIELD_1_label,                                                         \
        [OP_LOAD_FIELD_2] = &&OP_LOAD_FIELD_2_label,                                                         \
        [OP_LOAD_FIELD_4] = &&OP_LOAD_FIELD_4_label,                                                         \
        [OP_STORE_FIELD_1] = &&OP_STORE_FIELD_1_label,                                                       \
        [OP_STORE_FIELD_2] = &&OP_STORE_FIELD_2_label,                                                       \
        [OP_STORE_FIELD_4] = &&OP_STORE_FIELD_4_label,                                                       \
        [OP_ADDR_OF] = &&OP_ADDR_OF_label,                                                                   \
        [OP_LOAD_FIELD_PTR_1] = &&OP_LOAD_FIELD_PTR_1_label,                                                 \
        [OP_LOAD_FIELD_PTR_2] = &&OP_LOAD_FIELD_PTR_2_label,                                                 \
        [OP_LOAD_FIELD_PTR_4] = &&OP_LOAD_FIELD_PTR_4_label,                                                 \
        [OP_STORE_FIELD_PTR_1] = &&OP_STORE_FIELD_PTR_1_label,                                               \
        [OP_STORE_FIELD_PTR_2] = &&OP_STORE_FIELD_PTR_2_label,                                               \
        [OP_STORE_FIELD_PTR_4] = &&OP_STORE_FIELD_PTR_4_label,                                               \
        [OP_ADD_PTR] = &&OP_ADD_PTR_label,                                                                   \
        [OP_ADD_PTR_REG] = &&OP_ADD_PTR_REG_label,                                                           \
        [OP_ALLOC] = &&OP_ALLOC_label,                                                                       \
        [OP_FREE] = &&OP_FREE_label,                                                                         \
        [OP_BOUNDS_CHECK] = &&OP_BOUNDS_CHECK_label,                                                         \
        [OP_BOUNDS_CHECK_REG] = &&OP_BOUNDS_CHECK_REG_label,                                                 \
        [OP_LOAD_PTR_N] = &&OP_LOAD_PTR_N_label,                                                             \
        [OP_STORE_PTR_N] = &&OP_STORE_PTR_N_label,                                                           \
        [OP_LOOP_INIT] = &&OP_LOOP_INIT_label,                                                               \
        [OP_FOR_LOOP] = &&OP_FOR_LOOP_label,                                                                 \
    };                                                                                                       \
                                                                                                             \
    _Static_assert(sizeof(vm_dispatch_table) / sizeof(vm_dispatch_table[0]) == OP__COUNT,                    \
                   "the dispatch table must have an entry for every opcode");                                \
                                                                                                             \
    for (;;)

#else

#define VM_LOOP() for (;;)

#endif

#define VM_EXIT()                                                                                            \
    vm_done:;

#endif
