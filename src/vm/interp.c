#include "vm/interp.h"

#include "object.h"
#include "type/type.h"
#include "vm/chunk.h"
#include "vm/constant_pool.h"
#include "vm/ffi.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include "vm/vm_dispatch.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool vm_reserve_stack(const VM *vm, size_t needed) { return needed <= vm->stack_capacity; }

size_t vm_live_stack_end(const VM *vm) {
    if (vm->frame_count == 0) {
        return (vm->stack_capacity / 2) * VM_SLOT_SIZE;
    }

    const CallFrame *top = &vm->frames[vm->frame_count - 1];

    return top->base + (size_t)top->proto->max_registers * VM_SLOT_SIZE;
}

static bool vm_push_frame(VM *vm, const FuncPrototype *proto, size_t base, const Instruction *return_ip,
                          unsigned int dest) {
    if (VM_UNLIKELY(vm->frame_count == VM_MAX_CALL_DEPTH)) {
        return false;
    }

    if (VM_UNLIKELY(!vm_reserve_stack(vm, base / VM_SLOT_SIZE + proto->max_registers))) {
        return false;
    }

    vm->frames[vm->frame_count++] = (CallFrame){
        .proto = proto,
        .return_ip = return_ip,
        .base = base,
        .dest = dest,
    };

    vm->instruction_pointer = proto->chunk->instructions.data;

    return true;
}

static void vm_pop_frame(VM *vm);

static void vm_clear_pointer(VM *vm, size_t reg) {
    void *null_pointer = NULL;

    memcpy(vm_registers(vm) + reg * VM_SLOT_SIZE, &null_pointer, sizeof(null_pointer));
}

static void vm_release_frame_refs(VM *vm, const CallFrame *frame) {
    const FrameRefList *refs = &frame->proto->refs;

    for (size_t i = 0; i < refs->size; i++) {
        FrameRef ref = refs->data[i];
        void *slot = vm->stack + frame->base + ref.slot * VM_SLOT_SIZE;

        object_release(&DEFAULT_ALLOCATOR, ref.drop, slot);

        memset(slot, 0, ref.release_width);
    }
}

static void vm_unwind(VM *vm) {
    while (vm->frame_count > vm->frame_floor) {
        vm_release_frame_refs(vm, &vm->frames[vm->frame_count - 1]);
        vm_pop_frame(vm);
    }
}

static void vm_pop_frame(VM *vm) {
    CallFrame frame = vm->frames[--vm->frame_count];

    if (vm->frame_count == vm->frame_floor) {
        return;
    }

    vm->instruction_pointer = frame.return_ip;
}

float vm_addf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) + vm_read_f32_at(regs, r2);
}

float vm_subf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) - vm_read_f32_at(regs, r2);
}

float vm_mulf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) * vm_read_f32_at(regs, r2);
}

float vm_divf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) / vm_read_f32_at(regs, r2);
}

static inline int32_t vm_operand2i(const uint8_t *regs, Instruction instruction) {
    size_t r2 = VM_DECODE_R_R2(instruction);

    return VM_DECODE_R_K(instruction) ? (int32_t)r2 : vm_read_i32_at(regs, r2);
}

void vm_arithmeticf(uint8_t *regs, Instruction instruction, float (*func)(const uint8_t *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_write_f32_at(regs, rd, func(regs, r1, r2));
}

static void vm_arithmeticfk(uint8_t *regs, Instruction instruction, const Chunk *chunk,
                            float (*func)(float, float)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    Constant constant = constpool_get(chunk->const_pool, VM_DECODE_R_R2(instruction));

    vm_write_f32_at(regs, rd, func(vm_read_f32_at(regs, r1), constant.as_float));
}

static float vm_add_floats(float a, float b) { return a + b; }
static float vm_sub_floats(float a, float b) { return a - b; }
static float vm_mul_floats(float a, float b) { return a * b; }
static float vm_div_floats(float a, float b) { return a / b; }

int32_t vm_addi(int32_t a, int32_t b) { return a + b; }

int32_t vm_subi(int32_t a, int32_t b) { return a - b; }

int32_t vm_muli(int32_t a, int32_t b) { return a * b; }

int32_t vm_divi(int32_t a, int32_t b) { return a / b; }

int32_t vm_modi(int32_t a, int32_t b) { return a % b; }

static int32_t vm_ftoi(float value) {
    if (value >= 2147483648.0f) {
        return INT32_MAX;
    }

    if (value <= -2147483648.0f) {
        return INT32_MIN;
    }

    if (isnan(value)) {
        return 0;
    }

    return (int32_t)value;
}

void vm_arithmetici(uint8_t *regs, Instruction instruction, int32_t (*func)(int32_t, int32_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);

    vm_write_i32_at(regs, rd, func(vm_read_i32_at(regs, r1), vm_operand2i(regs, instruction)));
}

bool vm_less_thanf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) < vm_read_f32_at(regs, r2);
}

bool vm_greater_thanf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) > vm_read_f32_at(regs, r2);
}

bool vm_equalf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) == vm_read_f32_at(regs, r2);
}

bool vm_not_equalf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) != vm_read_f32_at(regs, r2);
}

bool vm_less_equalf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) <= vm_read_f32_at(regs, r2);
}

bool vm_greater_equalf(const uint8_t *regs, size_t r1, size_t r2) {
    return vm_read_f32_at(regs, r1) >= vm_read_f32_at(regs, r2);
}

bool vm_less_thani(int32_t a, int32_t b) { return a < b; }

bool vm_greater_thani(int32_t a, int32_t b) { return a > b; }

bool vm_equali(int32_t a, int32_t b) { return a == b; }

bool vm_not_equali(int32_t a, int32_t b) { return a != b; }

bool vm_less_equali(int32_t a, int32_t b) { return a <= b; }

bool vm_greater_equali(int32_t a, int32_t b) { return a >= b; }

void vm_conditionali(uint8_t *regs, Instruction instruction, bool (*func)(int32_t, int32_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);

    vm_write_i32_at(regs, rd, func(vm_read_i32_at(regs, r1), vm_operand2i(regs, instruction)));
}

bool vm_equals(const uint8_t *regs, size_t r1, size_t r2) {
    StrRef a;
    StrRef b;

    memcpy(&a, regs + r1 * VM_SLOT_SIZE, sizeof(a));
    memcpy(&b, regs + r2 * VM_SLOT_SIZE, sizeof(b));

    if (a.length != b.length) {
        return false;
    }

    if (a.data == b.data) {
        return true;
    }

    return memcmp(a.data, b.data, (size_t)a.length) == 0;
}

bool vm_not_equals(const uint8_t *regs, size_t r1, size_t r2) { return !vm_equals(regs, r1, r2); }

void vm_conditional(uint8_t *regs, Instruction instruction, bool (*func)(const uint8_t *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_write_i32_at(regs, rd, func(regs, r1, r2));
}

static bool vm_less_than_floats(float a, float b) { return a < b; }
static bool vm_greater_than_floats(float a, float b) { return a > b; }
static bool vm_less_equal_floats(float a, float b) { return a <= b; }
static bool vm_greater_equal_floats(float a, float b) { return a >= b; }
static bool vm_equal_floats(float a, float b) { return a == b; }
static bool vm_not_equal_floats(float a, float b) { return a != b; }

static void vm_conditionalfk(uint8_t *regs, Instruction instruction, const Chunk *chunk,
                             bool (*func)(float, float)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    Constant constant = constpool_get(chunk->const_pool, VM_DECODE_R_R2(instruction));

    vm_write_i32_at(regs, rd, func(vm_read_f32_at(regs, r1), constant.as_float));
}

static void vm_load_field(uint8_t *regs, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = regs + base * VM_SLOT_SIZE + offset;

    if (width < 4) {
        vm_write_i32_at(regs, rd, 0);
    }

    memcpy(regs + rd * VM_SLOT_SIZE, source, width);
}

static void vm_store_field(uint8_t *regs, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = regs + base * VM_SLOT_SIZE + offset;

    memcpy(dest, regs + r1 * VM_SLOT_SIZE, width);
}

static void vm_load_field_ptr(uint8_t *regs, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm_read_ptr_at(regs, base) + offset;

    if (width < 4) {
        vm_write_i32_at(regs, rd, 0);
    }

    memcpy(regs + rd * VM_SLOT_SIZE, source, width);
}

static void vm_store_field_ptr(uint8_t *regs, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm_read_ptr_at(regs, base) + offset;

    memcpy(dest, regs + r1 * VM_SLOT_SIZE, width);
}

void vm_fail(VM *vm, VmRunStatus status, const char *message) {
    if (vm->error.status != VM_RUN_OK) {
        return;
    }

    vm->error.status = status;
    snprintf(vm->error.message, sizeof(vm->error.message), "%s", message);
}

bool vm_call_extern(VM *vm, const ExternProto *proto, size_t base) {
    Args args = {.vm = vm, .function = proto->function, .base = base};

    /* A host body may run the VM again, which moves the pointer its caller is still stepping through. */
    const Instruction *resume = vm->instruction_pointer;

    ffi_invoke(proto->signature, &args);

    vm->instruction_pointer = resume;

    return vm->error.status == VM_RUN_OK;
}

static inline bool vm_check_divisor(const uint8_t *regs, VM *vm, Instruction instruction,
                                    const char *zero_message, const char *overflow_message) {
    int32_t divisor = vm_operand2i(regs, instruction);

    /* 0 and -1 are the only divisors a division can trap on, and the only two whose unsigned successor is
     * at most 1, so one comparison clears the path that is always taken. */
    if (VM_LIKELY((uint32_t)divisor + 1u > 1u)) {
        return true;
    }

    if (divisor == 0) {
        vm_fail(vm, VM_RUN_ERR_DIVIDE_BY_ZERO, zero_message);
        vm_unwind(vm);

        return false;
    }

    if (vm_read_i32_at(regs, VM_DECODE_R_R1(instruction)) == INT32_MIN) {
        vm_fail(vm, VM_RUN_ERR_DIVIDE_OVERFLOW, overflow_message);
        vm_unwind(vm);

        return false;
    }

    return true;
}

static void vm_run_loop(VM *vm) {
    const Instruction *pc = NULL;
    Chunk *chunk;
    Instruction instruction;
    OpCode op;
    uint8_t *regs = NULL;

    /* This run returns to its own floor, which no frame it pushes can change. */
    const size_t frame_floor = vm->frame_floor;

    VM_RELOAD();

    VM_LOOP() {
        VM_FETCH();

        VM_DISPATCH(op) {
            VM_CASE(OP_LOAD_CONST) {
                size_t reg = VM_DECODE_I_RD(instruction);
                size_t const_index = VM_DECODE_I_KX(instruction);
                Constant constant = constpool_get(chunk->const_pool, const_index);

                memcpy(VM_REG(reg), &constant, VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_STR) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                size_t string_index = VM_DECODE_I_KX(instruction);

                const String *text = vm->program.strings.data[string_index];

                StrRef value = {.data = text->data, .length = (int32_t)text->length};

                memcpy(regs + rd * VM_SLOT_SIZE, &value, sizeof(value));
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_TRUE) {
                size_t reg = VM_DECODE_I_RD(instruction);
                vm_write_i32_at(regs, reg, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FALSE) {
                size_t reg = VM_DECODE_I_RD(instruction);
                vm_write_i32_at(regs, reg, 0);
                VM_NEXT();
            }
            VM_CASE(OP_MOVE) {
                int rd = VM_DECODE_R_RD(instruction);
                int r1 = VM_DECODE_R_R1(instruction);

                memcpy(VM_REG(rd), VM_REG(r1), VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_MOVE_N) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                memmove(VM_REG(rd), VM_REG(r1), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_ADDFK) {
                vm_arithmeticfk(regs, instruction, chunk, vm_add_floats);
                VM_NEXT();
            }
            VM_CASE(OP_SUBFK) {
                vm_arithmeticfk(regs, instruction, chunk, vm_sub_floats);
                VM_NEXT();
            }
            VM_CASE(OP_MULFK) {
                vm_arithmeticfk(regs, instruction, chunk, vm_mul_floats);
                VM_NEXT();
            }
            VM_CASE(OP_DIVFK) {
                vm_arithmeticfk(regs, instruction, chunk, vm_div_floats);
                VM_NEXT();
            }
            VM_CASE(OP_ADDF) {
                vm_arithmeticf(regs, instruction, vm_addf);
                VM_NEXT();
            }
            VM_CASE(OP_SUBF) {
                vm_arithmeticf(regs, instruction, vm_subf);
                VM_NEXT();
            }
            VM_CASE(OP_MULF) {
                vm_arithmeticf(regs, instruction, vm_mulf);
                VM_NEXT();
            }
            VM_CASE(OP_DIVF) {
                vm_arithmeticf(regs, instruction, vm_divf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTF) {
                vm_conditional(regs, instruction, vm_less_thanf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTF) {
                vm_conditional(regs, instruction, vm_greater_thanf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQS) {
                vm_conditional(regs, instruction, vm_equals);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NES) {
                vm_conditional(regs, instruction, vm_not_equals);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQF) {
                vm_conditional(regs, instruction, vm_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEF) {
                vm_conditional(regs, instruction, vm_not_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEF) {
                vm_conditional(regs, instruction, vm_less_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEF) {
                vm_conditional(regs, instruction, vm_greater_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_less_than_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_greater_than_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_less_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_greater_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEFK) {
                vm_conditionalfk(regs, instruction, chunk, vm_not_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_ADDI) {
                vm_arithmetici(regs, instruction, vm_addi);
                VM_NEXT();
            }
            VM_CASE(OP_SUBI) {
                vm_arithmetici(regs, instruction, vm_subi);
                VM_NEXT();
            }
            VM_CASE(OP_MULI) {
                vm_arithmetici(regs, instruction, vm_muli);
                VM_NEXT();
            }
            VM_CASE(OP_DIVI) {
                if (!vm_check_divisor(regs, vm, instruction, "divided by zero",
                                      "divided the most negative int by -1")) {
                    VM_RETRY();
                }

                vm_arithmetici(regs, instruction, vm_divi);
                VM_NEXT();
            }
            VM_CASE(OP_ITOF) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                vm_write_f32_at(regs, rd, (float)vm_read_i32_at(regs, r1));
                VM_NEXT();
            }
            VM_CASE(OP_FTOI) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                vm_write_i32_at(regs, rd, vm_ftoi(vm_read_f32_at(regs, r1)));
                VM_NEXT();
            }
            VM_CASE(OP_MODI) {
                if (!vm_check_divisor(regs, vm, instruction, "took the remainder of a division by zero",
                                      "took the remainder of the most negative int and -1")) {
                    VM_RETRY();
                }

                vm_arithmetici(regs, instruction, vm_modi);
                VM_NEXT();
            }
            VM_CASE(OP_NEGI) {
                int32_t value = vm_read_i32_at(regs, VM_DECODE_R_R1(instruction));

                vm_write_i32_at(regs, VM_DECODE_R_RD(instruction), (int32_t)(0u - (uint32_t)value));
                VM_NEXT();
            }
            VM_CASE(OP_NEGF) {
                vm_write_f32_at(regs, VM_DECODE_R_RD(instruction),
                                -vm_read_f32_at(regs, VM_DECODE_R_R1(instruction)));
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTI) {
                vm_conditionali(regs, instruction, vm_less_thani);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTI) {
                vm_conditionali(regs, instruction, vm_greater_thani);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQI) {
                vm_conditionali(regs, instruction, vm_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEI) {
                vm_conditionali(regs, instruction, vm_not_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEI) {
                vm_conditionali(regs, instruction, vm_less_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEI) {
                vm_conditionali(regs, instruction, vm_greater_equali);
                VM_NEXT();
            }
            VM_CASE(OP_BOX) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                size_t type_index = VM_DECODE_I_KX(instruction);

                const HeapShape *shape = &vm->program.heap_shapes.data[type_index];

                void *object = object_alloc(&DEFAULT_ALLOCATOR, shape->size, shape->drop);

                if (VM_UNLIKELY(!object)) {
                    vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory");

                    vm_unwind(vm);

                    VM_RETRY();
                }

                memcpy(regs + rd * VM_SLOT_SIZE, &object, sizeof(object));
                VM_NEXT();
            }
            VM_CASE(OP_NULL) {
                vm_clear_pointer(vm, VM_DECODE_I_RD(instruction));
                VM_NEXT();
            }
            VM_CASE(OP_RELEASE) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                const HeapShape *shape = &vm->program.heap_shapes.data[VM_DECODE_I_KX(instruction)];

                void *slot = regs + rd * VM_SLOT_SIZE;

                object_release(&DEFAULT_ALLOCATOR, shape->drop, slot);

                memset(slot, 0, shape->release_width);
                VM_NEXT();
            }
            VM_CASE(OP_CALL) {
                unsigned int dest = VM_DECODE_I_RD(instruction);
                size_t proto_index = VM_DECODE_I_KX(instruction);

                const FuncPrototype *proto = vm->program.prototypes.data[proto_index];

                size_t base = vm->frames[vm->frame_count - 1].base + dest * VM_SLOT_SIZE;

                if (VM_UNLIKELY(!vm_push_frame(vm, proto, base, pc, dest))) {
                    vm_fail(vm, VM_RUN_ERR_CALL_DEPTH, "call depth exceeded");

                    vm_unwind(vm);

                    VM_RETRY();
                }

                VM_RETRY();
            }
            VM_CASE(OP_CALL_EXTERN) {
                unsigned int dest = VM_DECODE_I_RD(instruction);
                size_t extern_index = VM_DECODE_I_KX(instruction);

                const ExternProto *proto = &vm->program.extern_protos.data[extern_index];

                if (VM_UNLIKELY(!vm_call_extern(
                        vm, proto, vm->frames[vm->frame_count - 1].base + dest * VM_SLOT_SIZE))) {
                    vm_unwind(vm);

                    VM_RETRY();
                }

                VM_NEXT();
            }
            VM_CASE(OP_RETURN) VM_CASE(OP_RETURN_N) {
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = op == OP_RETURN ? 1 : VM_DECODE_R_R2(instruction);

                uint8_t result[VM_MAX_RETURN_SLOTS * VM_SLOT_SIZE];
                memcpy(result, VM_REG(r1), slots * VM_SLOT_SIZE);

                const CallFrame *frame = &vm->frames[vm->frame_count - 1];
                unsigned int dest = frame->dest;
                size_t frame_base = frame->base;
                vm_pop_frame(vm);

                if (vm->frame_count == vm->frame_floor) {
                    memcpy(vm->stack + frame_base, result, slots * VM_SLOT_SIZE);
                    VM_RETRY();
                }

                VM_LOAD_REGS();

                memcpy(VM_REG(dest), result, slots * VM_SLOT_SIZE);
                VM_RETRY();
            }
            VM_CASE(OP_LOAD_FIELD_1) {
                vm_load_field(regs, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_2) {
                vm_load_field(regs, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_4) {
                vm_load_field(regs, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_1) {
                vm_store_field(regs, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_2) {
                vm_store_field(regs, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_4) {
                vm_store_field(regs, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_ADDR_OF) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t offset = VM_DECODE_R_R2(instruction);

                vm_write_ptr_at(regs, rd, regs + base * VM_SLOT_SIZE + offset);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_1) {
                vm_load_field_ptr(regs, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_2) {
                vm_load_field_ptr(regs, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_4) {
                vm_load_field_ptr(regs, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_1) {
                vm_store_field_ptr(regs, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_2) {
                vm_store_field_ptr(regs, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_4) {
                vm_store_field_ptr(regs, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_ALLOC) {
                unsigned int rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                size_t alignment = VM_DECODE_R_R2(instruction);
                (void)alignment;

                int32_t count;
                memcpy(&count, VM_REG(r1), sizeof(count));

                if (VM_UNLIKELY(count < 0)) {
                    vm_fail(vm, VM_RUN_ERR_BOUNDS, "an array's length cannot be negative");

                    vm_unwind(vm);
                    VM_RETRY();
                }

                size_t bytes = count == 0 ? 1 : (size_t)count;

                void *block = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, bytes);

                if (VM_UNLIKELY(!block)) {
                    vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory allocating an array");

                    vm_unwind(vm);
                    VM_RETRY();
                }

                memset(block, 0, bytes);

                vm_write_ptr_at(regs, rd, (uint8_t *)block);
                VM_NEXT();
            }
            VM_CASE(OP_FREE) {
                unsigned int rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                int32_t size;
                memcpy(&size, VM_REG(r1), sizeof(size));

                DEFAULT_ALLOCATOR.free(DEFAULT_ALLOCATOR.ctx, vm_read_ptr_at(regs, rd), (size_t)size);
                VM_NEXT();
            }
            VM_CASE(OP_ADD_PTR_REG) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t r2 = VM_DECODE_R_R2(instruction);

                int32_t offset;
                memcpy(&offset, VM_REG(r2), sizeof(offset));

                vm_write_ptr_at(regs, rd, vm_read_ptr_at(regs, base) + offset);
                VM_NEXT();
            }
            VM_CASE(OP_BOUNDS_CHECK) {
                size_t r1 = VM_DECODE_R_R1(instruction);

                int32_t length = (int32_t)VM_DECODE_R_R2(instruction);

                int32_t index;
                memcpy(&index, VM_REG(r1), sizeof(index));

                if (VM_UNLIKELY(index < 0 || index >= length)) {
                    vm_fail(vm, VM_RUN_ERR_BOUNDS, "array index is out of range");

                    vm_unwind(vm);
                    VM_RETRY();
                }

                VM_NEXT();
            }
            VM_CASE(OP_ADD_PTR) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t offset = VM_DECODE_R_R2(instruction);

                vm_write_ptr_at(regs, rd, vm_read_ptr_at(regs, base) + offset);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_PTR_N) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                memcpy(VM_REG(rd), vm_read_ptr_at(regs, base), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_PTR_N) {
                size_t base = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                memcpy(vm_read_ptr_at(regs, base), VM_REG(r1), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_LOOP_INIT) {
                int32_t counter = vm_read_i32_at(regs, VM_DECODE_R_R1(instruction));
                int32_t bound = vm_read_i32_at(regs, VM_DECODE_R_R2(instruction));

                vm_write_i32_at(regs, VM_DECODE_R_RD(instruction), counter < bound ? bound - counter : 0);
                VM_NEXT();
            }
            VM_CASE(OP_FOR_LOOP) {
                size_t counter_reg = VM_DECODE_R_RD(instruction);
                size_t left_reg = VM_DECODE_R_R1(instruction);

                vm_write_i32_at(regs, counter_reg, vm_read_i32_at(regs, counter_reg) + 1);

                int32_t left = vm_read_i32_at(regs, left_reg) - 1;
                vm_write_i32_at(regs, left_reg, left);

                if (left > 0) {
                    pc -= VM_DECODE_R_BACK(instruction);
                    VM_JUMPED();
                }

                VM_JUMPED();
            }
            VM_CASE(OP_JMP) {
                pc += VM_DECODE_I_SIMM(instruction);
                VM_JUMPED();
            }
            VM_CASE(OP_JMP_IF_FALSE) {
                size_t reg = VM_DECODE_I_RD(instruction);

                bool cond = vm_read_i32_at(regs, reg);

                if (!cond) {
                    pc += VM_DECODE_I_SIMM(instruction);
                    VM_JUMPED();
                }

                VM_JUMPED();
            }
            VM_CASE(OP_JMP_IF_TRUE) {
                size_t reg = VM_DECODE_I_RD(instruction);

                bool cond = vm_read_i32_at(regs, reg);

                if (cond) {
                    pc += VM_DECODE_I_SIMM(instruction);
                    VM_JUMPED();
                }

                VM_JUMPED();
            }

            VM_CASE_UNREACHABLE(OP__COUNT)
        }
    }

    VM_EXIT()

    while (vm->frame_count > vm->frame_floor) {
        vm_pop_frame(vm);
    }
}

VmRunStatus interp_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest) {
    vm->error = (VmError){.status = VM_RUN_OK};

    size_t floor = vm->frame_floor;
    vm->frame_floor = vm->frame_count;

    if (!vm_push_frame(vm, proto, base, 0, dest)) {
        vm->frame_floor = floor;
        vm_fail(vm, VM_RUN_ERR_STACK_OVERFLOW, "out of stack space");
        return vm->error.status;
    }

    vm_run_loop(vm);

    vm->frame_floor = floor;

    return vm->error.status;
}

VmRunStatus interp_run_extern(VM *vm, const ExternProto *proto, size_t base) {
    vm->error = (VmError){.status = VM_RUN_OK};

    vm_call_extern(vm, proto, base);

    return vm->error.status;
}

VmRunStatus interp_run_top_level(VM *vm, const FuncPrototype *top_level) {
    vm->frame_count = 0;
    vm->frame_floor = 0;

    return interp_run_frame(vm, top_level, 0, 0);
}
