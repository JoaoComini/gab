#ifndef GAB_VM_DISPATCH_H
#define GAB_VM_DISPATCH_H

#include "vm/opcode.h"

/*
    Dispatch. Two spellings of the same interpreter: a jump through a table of
    label addresses where the compiler has the extension for it, and a switch
    everywhere else.

    The table costs one indirect jump per instruction where the switch costs a
    bounds check and then the same jump. What actually makes it faster is the
    branch predictor: the switch has a single indirect jump that every opcode
    shares, so one history entry has to predict the whole instruction stream,
    while a jump at the end of each case is predicted on what tends to follow
    that opcode -- and bytecode is full of pairs that follow each other.

    The two must stay in step. Every opcode needs a label in the table and a
    case in the body, which _Static_assert on OP__COUNT and -Wswitch
    respectively enforce.

    The interpreter is written once, and carries no #if of its own:

        for (;;) {
            VM_FETCH();

            VM_DISPATCH(op) {
                VM_CASE(OP_...) { ...; VM_NEXT(); }
            }
        }

    In the goto form VM_DISPATCH is a jump and the braces after it are an
    ordinary block only ever entered by that jump; in the switch form it is the
    switch itself. Either way the handlers indent and brace-match as ordinary
    code.

    These macros read and write locals of the function that uses them --
    'vm', 'frame', 'chunk', 'code', 'code_size', 'instruction' and 'op' -- and
    the goto form also needs a 'vm_dispatch_table' of label addresses and a
    'vm_done' label. That is the contract: a function spelling its interpreter
    with these declares all of them. Only vm_run_loop does.
*/

// Builds the switch interpreter even where the computed-goto extension is
// available, for testing that the portable spelling still works:
//
//   cmake -S . -B build-switch -DCMAKE_C_FLAGS=-DGAB_FORCE_SWITCH
//
// Prefixed GAB_ because it is set from outside the source, as GAB_SANITIZE is.
// The VM_ names below are the interpreter's own vocabulary and are not knobs.
#if defined(GAB_FORCE_SWITCH)
#define VM_COMPUTED_GOTO 0
#elif defined(__GNUC__) || defined(__clang__)
#define VM_COMPUTED_GOTO 1
#else
#define VM_COMPUTED_GOTO 0
#endif

// Reloads what the running frame's bytecode is, for the handlers that change
// which frame that is: a call, a return, or an unwind.
#define VM_RELOAD()                                                                                          \
    do {                                                                                                     \
        if (vm->frame_count > 0) {                                                                           \
            frame = &vm->frames[vm->frame_count - 1];                                                        \
            chunk = frame->proto->chunk;                                                                     \
            code = chunk->instructions.data;                                                                 \
            code_size = chunk->instructions.size;                                                            \
        }                                                                                                    \
    } while (0)

// Reads the instruction the pointer names, leaving the loop once it has run
// past the frame's code. The pointer is signed, so a jump that went too far
// back reads as negative here rather than as a huge index that would pass for
// a normal end of function.
#define VM_FETCH()                                                                                           \
    do {                                                                                                     \
        if (vm->frame_count == 0 || vm->instruction_pointer < 0 ||                                           \
            vm->instruction_pointer >= (ptrdiff_t)code_size) {                                               \
            goto vm_done;                                                                                    \
        }                                                                                                    \
                                                                                                             \
        instruction = code[vm->instruction_pointer];                                                         \
        op = VM_DECODE_OPCODE(instruction);                                                                  \
    } while (0)

#if VM_COMPUTED_GOTO

#define VM_DISPATCH(o) goto *vm_dispatch_table[o];
#define VM_CASE(name) name##_label:

// An enum member that is not an opcode, so no instruction ever decodes to it.
// It carries its own exit rather than a body at the call site: reaching it at
// all means a decoded opcode outside the enum, and the run has to stop there.
// Falling out of the switch instead would re-fetch the same instruction and
// spin.
//
// The switch form needs the case because -Wswitch counts every member. The
// goto form has no table entry to name, so this is unreachable code the
// compiler drops rather than an unused label.
#define VM_CASE_UNREACHABLE(name)                                                                            \
    if (0)                                                                                                   \
        goto vm_done;

// Reaches the next handler by jumping straight to it, so the enclosing loop is
// never gone back around.
#define VM_NEXT()                                                                                            \
    do {                                                                                                     \
        vm->instruction_pointer += 1;                                                                        \
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

// 'break' leaves the switch and the enclosing loop fetches the next
// instruction. It must not be wrapped in a do-while: the break would leave
// that instead, and the handler would fall through into the case below it.
#define VM_NEXT()                                                                                            \
    {                                                                                                        \
        vm->instruction_pointer += 1;                                                                        \
        break;                                                                                               \
    }

#define VM_RETRY()                                                                                           \
    {                                                                                                        \
        VM_RELOAD();                                                                                         \
        break;                                                                                               \
    }

#endif

// Opens the interpreter loop. In the goto form this also declares the table of
// label addresses the dispatch jumps through, which is why it is a macro rather
// than a bare 'for': '&&label' is only valid inside the function that declares
// the label, so the table cannot live in a file of its own.
//
// One entry per opcode, in enum order: the index is the opcode itself. An
// opcode with no VM_CASE in the body fails the build here, where the entry
// names a label that does not exist.
#if VM_COMPUTED_GOTO

#define VM_LOOP()                                                                                            \
    static void *const vm_dispatch_table[] = {                                                               \
        [OP_LOAD_CONST] = &&OP_LOAD_CONST_label,                                                             \
        [OP_LOAD_TRUE] = &&OP_LOAD_TRUE_label,                                                               \
        [OP_LOAD_FALSE] = &&OP_LOAD_FALSE_label,                                                             \
        [OP_MOVE] = &&OP_MOVE_label,                                                                         \
        [OP_MOVE_N] = &&OP_MOVE_N_label,                                                                     \
        [OP_ADDI] = &&OP_ADDI_label,                                                                         \
        [OP_SUBI] = &&OP_SUBI_label,                                                                         \
        [OP_MULI] = &&OP_MULI_label,                                                                         \
        [OP_DIVI] = &&OP_DIVI_label,                                                                         \
        [OP_MODI] = &&OP_MODI_label,                                                                         \
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
        [OP_CMP_EQF] = &&OP_CMP_EQF_label,                                                                   \
        [OP_CMP_NEF] = &&OP_CMP_NEF_label,                                                                   \
        [OP_CMP_LEF] = &&OP_CMP_LEF_label,                                                                   \
        [OP_CMP_GEF] = &&OP_CMP_GEF_label,                                                                   \
        [OP_JMP] = &&OP_JMP_label,                                                                           \
        [OP_JMP_IF_FALSE] = &&OP_JMP_IF_FALSE_label,                                                         \
        [OP_JMP_IF_TRUE] = &&OP_JMP_IF_TRUE_label,                                                           \
        [OP_CALL] = &&OP_CALL_label,                                                                         \
        [OP_NEW] = &&OP_NEW_label,                                                                           \
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
        [OP_LOAD_PTR_N] = &&OP_LOAD_PTR_N_label,                                                             \
        [OP_STORE_PTR_N] = &&OP_STORE_PTR_N_label,                                                           \
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

// Where every exit from the interpreter lands: the loop running past the end
// of a frame's code, and an opcode that decoded to something no case names.
// Same in both spellings, since only the ways of reaching a handler differ.
#define VM_EXIT()                                                                                            \
    vm_done:;

#endif
