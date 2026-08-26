#include "vm/interp.h"

#include "object.h"
#include "type.h"
#include "vm/chunk.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include "vm/vm_dispatch.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Registers sit at base + r * VM_SLOT_SIZE, so the stack must hold every
// register the frame can address before it starts executing. 'needed' counts
// slots. The buffer is never resized, so this only reports whether the frame
// fits: a pointer into the stack must stay valid for as long as its inner
// does, which a moving buffer cannot promise.
static bool vm_reserve_stack(const VM *vm, size_t needed) { return needed <= vm->stack_capacity; }

static bool vm_push_frame(VM *vm, const FuncPrototype *proto, size_t base, ptrdiff_t return_ip,
                          unsigned int dest) {
    if (vm->frame_count == VM_MAX_CALL_DEPTH) {
        return false;
    }

    // base is a byte offset; the reservation is in slots.
    if (!vm_reserve_stack(vm, base / VM_SLOT_SIZE + proto->max_registers)) {
        return false;
    }

    vm->frames[vm->frame_count++] = (CallFrame){
        .proto = proto,
        .return_ip = return_ip,
        .base = base,
        .dest = dest,
    };

    vm->registers = vm->stack + base;
    vm->instruction_pointer = 0;

    return true;
}

static void vm_pop_frame(VM *vm);

// Writes NULL over a pointer slot, so a slot that has already been released
// reads as empty rather than as an address that was freed.
static void vm_clear_pointer(VM *vm, size_t reg) {
    void *null_pointer = NULL;

    memcpy(vm->registers + reg * VM_SLOT_SIZE, &null_pointer, sizeof(null_pointer));
}

// Frees every object a frame still owns. Only ever called while unwinding from
// a failure: a run that ends normally has already executed the frees codegen
// emitted at each scope's close, which is both cheaper and more precise than
// this — it frees at the brace rather than at the frame's end.
//
// A slot listed on the prototype either owns a live object or holds NULL,
// because freeing one clears it. That is what makes walking the list safe
// despite sibling blocks reusing slots. A 'ref T' slot is never listed.
static void vm_release_frame_refs(VM *vm, const CallFrame *frame) {
    const FrameRefList *refs = &frame->proto->refs;

    for (size_t i = 0; i < refs->size; i++) {
        FrameRef ref = refs->data[i];
        void *slot = vm->stack + frame->base + ref.slot * VM_SLOT_SIZE;

        object_release(DEFAULT_ALLOCATOR, ref.type, slot);

        // Cleared so a slot freed here is safe to visit again: the pointer a
        // release read is gone, and an array's length goes with it so a second
        // visit walks nothing.
        memset(slot, 0, type_release_width(ref.type));
    }
}

// Unwinds every frame after a failure, dropping what each still holds. The
// ordinary releases are jumped past by the failure, so without this a run that
// fails leaks everything that was live when it did.
static void vm_unwind(VM *vm) {
    while (vm->frame_count > 0) {
        vm_release_frame_refs(vm, &vm->frames[vm->frame_count - 1]);
        vm_pop_frame(vm);
    }
}

static void vm_pop_frame(VM *vm) {
    CallFrame frame = vm->frames[--vm->frame_count];

    if (vm->frame_count == 0) {
        vm->registers = vm->stack;
        return;
    }

    vm->registers = vm->stack + vm->frames[vm->frame_count - 1].base;
    vm->instruction_pointer = frame.return_ip;
}

float vm_addf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) + vm_read_f32(vm, r2); }

float vm_subf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) - vm_read_f32(vm, r2); }

float vm_mulf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) * vm_read_f32(vm, r2); }

float vm_divf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) / vm_read_f32(vm, r2); }

// The second operand, which the k bit makes either a register to read or a
// small immediate encoded in the instruction itself.
//
// Immediates are integers: a float literal has no compact encoding in eight
// bits, so codegen never marks one, and the float path reads a register as it
// always did.
static inline int32_t vm_operand2i(const VM *vm, Instruction instruction) {
    size_t r2 = VM_DECODE_R_R2(instruction);

    return VM_DECODE_R_K(instruction) ? (int32_t)r2 : vm_read_i32(vm, r2);
}

void vm_arithmeticf(VM *vm, Instruction instruction, float (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_write_f32(vm, rd, func(vm, r1, r2));
}

// The right operand read from the constant pool rather than a register, for
// the OP_*FK family. Separate from vm_arithmeticf because those take their
// operands as register indices, and this one has a value in hand.
static void vm_arithmeticfk(VM *vm, Instruction instruction, const Chunk *chunk,
                            float (*func)(float, float)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    Constant constant = constpool_get(chunk->const_pool, VM_DECODE_R_R2(instruction));

    vm_write_f32(vm, rd, func(vm_read_f32(vm, r1), constant.as_float));
}

static float vm_add_floats(float a, float b) { return a + b; }
static float vm_sub_floats(float a, float b) { return a - b; }
static float vm_mul_floats(float a, float b) { return a * b; }
static float vm_div_floats(float a, float b) { return a / b; }

// The integer operations take values rather than register indices, so the same
// body serves a register operand and an immediate one.
int32_t vm_addi(int32_t a, int32_t b) { return a + b; }

int32_t vm_subi(int32_t a, int32_t b) { return a - b; }

int32_t vm_muli(int32_t a, int32_t b) { return a * b; }

int32_t vm_divi(int32_t a, int32_t b) { return a / b; }

int32_t vm_modi(int32_t a, int32_t b) { return a % b; }

// Truncates a float to an int, clamping whatever does not fit to the nearest
// end of the range.
//
// A float outside the int range has no truncation to give, and the plain cast
// is undefined there -- a license the optimizer may act on, not merely a value
// the hardware picks. Clamping gives every operand a defined answer, and one
// that reads as what it is: a value pinned at the limit, rather than a wrapped
// number small enough to pass for real data.
//
// The bounds are the two powers of two, which a float holds exactly: -2^31 is
// INT32_MIN itself, and 2^31 is the first float above the range. Writing the
// upper one as (float)INT32_MAX would name the same float -- 2^31 is what
// INT32_MAX rounds to -- but says something untrue about which values are in
// range, so the power of two is written directly.
static int32_t vm_ftoi(float value) {
    if (value >= 2147483648.0f) {
        return INT32_MAX;
    }

    if (value <= -2147483648.0f) {
        return INT32_MIN;
    }

    // NaN reaches here, having failed both comparisons: it is not outside the
    // range in either direction, so neither limit is the nearer one. Zero is
    // the answer by convention rather than by derivation.
    if (isnan(value)) {
        return 0;
    }

    return (int32_t)value;
}

void vm_arithmetici(VM *vm, Instruction instruction, int32_t (*func)(int32_t, int32_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);

    vm_write_i32(vm, rd, func(vm_read_i32(vm, r1), vm_operand2i(vm, instruction)));
}

bool vm_less_thanf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) < vm_read_f32(vm, r2); }

bool vm_greater_thanf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) > vm_read_f32(vm, r2); }

bool vm_equalf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) == vm_read_f32(vm, r2); }

bool vm_not_equalf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) != vm_read_f32(vm, r2); }

bool vm_less_equalf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) <= vm_read_f32(vm, r2); }

bool vm_greater_equalf(VM *vm, size_t r1, size_t r2) { return vm_read_f32(vm, r1) >= vm_read_f32(vm, r2); }

// As the integer arithmetic, these take values so an immediate second operand
// costs nothing extra.
bool vm_less_thani(int32_t a, int32_t b) { return a < b; }

bool vm_greater_thani(int32_t a, int32_t b) { return a > b; }

bool vm_equali(int32_t a, int32_t b) { return a == b; }

bool vm_not_equali(int32_t a, int32_t b) { return a != b; }

bool vm_less_equali(int32_t a, int32_t b) { return a <= b; }

bool vm_greater_equali(int32_t a, int32_t b) { return a >= b; }

void vm_conditionali(VM *vm, Instruction instruction, bool (*func)(int32_t, int32_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);

    vm_write_i32(vm, rd, func(vm_read_i32(vm, r1), vm_operand2i(vm, instruction)));
}

// Whether two string headers name the same characters. Length first, since it
// settles most pairs without reading any of them, and identical addresses
// second: interning makes equal literals one address, but a string built at
// runtime is never interned, so identity is a fast path and never the answer.
bool vm_equals(VM *vm, size_t r1, size_t r2) {
    GabStringValue a;
    GabStringValue b;

    memcpy(&a, vm->registers + r1 * VM_SLOT_SIZE, sizeof(a));
    memcpy(&b, vm->registers + r2 * VM_SLOT_SIZE, sizeof(b));

    if (a.length != b.length) {
        return false;
    }

    if (a.data == b.data) {
        return true;
    }

    return memcmp(a.data, b.data, (size_t)a.length) == 0;
}

bool vm_not_equals(VM *vm, size_t r1, size_t r2) { return !vm_equals(vm, r1, r2); }

void vm_conditional(VM *vm, Instruction instruction, bool (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_write_i32(vm, rd, func(vm, r1, r2));
}

// The float comparisons whose right operand is a pool constant. As
// vm_arithmeticfk is to vm_arithmeticf: the operands are values here rather than
// register indices, since one of them is already in hand.
static bool vm_less_than_floats(float a, float b) { return a < b; }
static bool vm_greater_than_floats(float a, float b) { return a > b; }
static bool vm_less_equal_floats(float a, float b) { return a <= b; }
static bool vm_greater_equal_floats(float a, float b) { return a >= b; }
static bool vm_equal_floats(float a, float b) { return a == b; }
static bool vm_not_equal_floats(float a, float b) { return a != b; }

static void vm_conditionalfk(VM *vm, Instruction instruction, const Chunk *chunk,
                             bool (*func)(float, float)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    Constant constant = constpool_get(chunk->const_pool, VM_DECODE_R_R2(instruction));

    vm_write_i32(vm, rd, func(vm_read_f32(vm, r1), constant.as_float));
}

static void vm_load_field(VM *vm, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm->registers + base * VM_SLOT_SIZE + offset;

    // The destination is a whole slot, so a narrow field is widened rather than
    // left beside stale bytes. A 4-byte one fills the low word exactly, and the
    // zeroing would only be overwritten -- 'width' is a literal at every call
    // site, so this costs nothing to decide.
    if (width < 4) {
        vm_write_i32(vm, rd, 0);
    }

    memcpy(vm_reg_at(vm, rd), source, width);
}

static void vm_store_field(VM *vm, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm->registers + base * VM_SLOT_SIZE + offset;

    // Only the field's own bytes are written; anything sharing the slot keeps
    // its value.
    memcpy(dest, vm_reg_at(vm, r1), width);
}

// The address a 2-slot pointer register holds. The slot pair is placed at an
// even index and the stack base is 8-byte aligned, so this is a natural read.
static void vm_load_field_ptr(VM *vm, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm_read_ptr(vm, base) + offset;

    // A 4-byte field fills the destination's low word exactly, so the zeroing a
    // narrower one needs would be written and then overwritten in full. Four is
    // the common width -- an int, a float, every pointer field's payload -- so
    // the branch buys a store on most executions.
    if (width < 4) {
        vm_write_i32(vm, rd, 0);
    }

    memcpy(vm_reg_at(vm, rd), source, width);
}

static void vm_store_field_ptr(VM *vm, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm_read_ptr(vm, base) + offset;

    memcpy(dest, vm_reg_at(vm, r1), width);
}

// Records why a run stopped. The first failure wins: a later one is a
// consequence of unwinding, not an independent problem.
void vm_fail(VM *vm, VmRunStatus status, const char *message) {
    if (vm->error.status != VM_RUN_OK) {
        return;
    }

    vm->error.status = status;
    snprintf(vm->error.message, sizeof(vm->error.message), "%s", message);
}

// Runs a C body against the frame at 'base'. Returns false when the run must
// unwind, which is the body having reported a failure.
//
// The body says so through gab_error, which sets the status and the message on
// its way past -- so what the run stopped for is already recorded, and there is
// nothing for this to carry across the boundary.
//
// No frame is pushed. The body reads and writes slots directly, so there is
// nothing for a frame to track — and a C function that cannot be interpreted
// has no instruction pointer to return to.
bool vm_call_extern(VM *vm, const ExternProto *proto, size_t base) {
    Args args = {.vm = vm, .symbol = proto->symbol, .base = base};

    proto->body(&args);

    return vm->error.status == VM_RUN_OK;
}

// Whether an int division or remainder may go ahead, failing the run when it
// may not. Two operand pairs are undefined in C and take the whole host
// process down with SIGFPE, so they are checked rather than performed: a zero
// divisor, and INT32_MIN over -1, whose true quotient is one past the top of
// the range. Both opcodes are the same hardware instruction, so both are
// undefined for '%' too, even though INT32_MIN % -1 is mathematically 0.
//
// The float opcodes need no such guard: IEEE division by zero yields an
// infinity, which is a value the VM can carry.
static bool vm_check_divisor(VM *vm, Instruction instruction, const char *zero_message,
                             const char *overflow_message) {
    int32_t divisor = vm_operand2i(vm, instruction);
    int32_t dividend = vm_read_i32(vm, VM_DECODE_R_R1(instruction));

    if (divisor == 0) {
        vm_fail(vm, VM_RUN_ERR_DIVIDE_BY_ZERO, zero_message);
        vm_unwind(vm);

        return false;
    }

    if (dividend == INT32_MIN && divisor == -1) {
        vm_fail(vm, VM_RUN_ERR_DIVIDE_OVERFLOW, overflow_message);
        vm_unwind(vm);

        return false;
    }

    return true;
}

// Runs until every frame the caller pushed has unwound. Both entry points
// share it: interp_run_top_level pushes frame zero, and a host call pushes one frame for the
// function it is invoking, so there is exactly one interpreter either way.
static void vm_run_loop(VM *vm) {
    CallFrame *frame;
    ptrdiff_t ip = 0;
    Chunk *chunk;
    Instruction instruction;
    OpCode op;
    const Instruction *code = NULL;
    size_t code_size = 0;

    VM_RELOAD();

    VM_LOOP() {
        VM_FETCH();

        VM_DISPATCH(op) {
            VM_CASE(OP_LOAD_CONST) {
                size_t reg = VM_DECODE_I_RD(instruction);
                size_t const_index = VM_DECODE_I_KX(instruction);
                Constant constant = constpool_get(chunk->const_pool, const_index);

                memcpy(vm_reg_at(vm, reg), &constant, VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_STR) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                size_t string_index = VM_DECODE_I_KX(instruction);

                const String *text = vm->program.strings.data[string_index];

                // Borrowed, not copied: the characters are interned and outlive
                // every frame, so the header names them where they already are.
                GabStringValue value = {.data = text->data, .length = (int32_t)text->length};

                memcpy(vm->registers + rd * VM_SLOT_SIZE, &value, sizeof(value));
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_TRUE) {
                size_t reg = VM_DECODE_I_RD(instruction);
                vm_write_i32(vm, reg, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FALSE) {
                size_t reg = VM_DECODE_I_RD(instruction);
                vm_write_i32(vm, reg, 0);
                VM_NEXT();
            }
            VM_CASE(OP_MOVE) {
                int rd = VM_DECODE_R_RD(instruction);
                int r1 = VM_DECODE_R_R1(instruction);

                memcpy(vm_reg_at(vm, rd), vm_reg_at(vm, r1), VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_MOVE_N) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                // memmove, not memcpy: a struct assigned from one of its own
                // fields, or an argument marshalled into the slots just above its
                // source, gives overlapping ranges. OP_LOAD_PTR_N can use memcpy
                // because its source is a heap payload and cannot overlap a frame.
                memmove(vm_reg_at(vm, rd), vm_reg_at(vm, r1), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_ADDFK) {
                vm_arithmeticfk(vm, instruction, chunk, vm_add_floats);
                VM_NEXT();
            }
            VM_CASE(OP_SUBFK) {
                vm_arithmeticfk(vm, instruction, chunk, vm_sub_floats);
                VM_NEXT();
            }
            VM_CASE(OP_MULFK) {
                vm_arithmeticfk(vm, instruction, chunk, vm_mul_floats);
                VM_NEXT();
            }
            VM_CASE(OP_DIVFK) {
                vm_arithmeticfk(vm, instruction, chunk, vm_div_floats);
                VM_NEXT();
            }
            VM_CASE(OP_ADDF) {
                vm_arithmeticf(vm, instruction, vm_addf);
                VM_NEXT();
            }
            VM_CASE(OP_SUBF) {
                vm_arithmeticf(vm, instruction, vm_subf);
                VM_NEXT();
            }
            VM_CASE(OP_MULF) {
                vm_arithmeticf(vm, instruction, vm_mulf);
                VM_NEXT();
            }
            VM_CASE(OP_DIVF) {
                vm_arithmeticf(vm, instruction, vm_divf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTF) {
                vm_conditional(vm, instruction, vm_less_thanf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTF) {
                vm_conditional(vm, instruction, vm_greater_thanf);
                VM_NEXT();
            }
            VM_CASE(OP_CONCAT) {
                unsigned int rd = VM_DECODE_R_RD(instruction);
                unsigned int r1 = VM_DECODE_R_R1(instruction);
                unsigned int r2 = VM_DECODE_R_R2(instruction);

                GabStringValue left;
                GabStringValue right;

                memcpy(&left, vm->registers + r1 * VM_SLOT_SIZE, sizeof(left));
                memcpy(&right, vm->registers + r2 * VM_SLOT_SIZE, sizeof(right));

                size_t total = (size_t)left.length + (size_t)right.length;

                // Typed as characters rather than as a string: what is being
                // allocated is the bytes themselves, and they own nothing
                // further. The string header naming them is the value below.
                char *characters = object_alloc_sized(
                    DEFAULT_ALLOCATOR, vm->env.global_scope.type_registry->builtins.buffer_type,
                    total == 0 ? 1 : total);

                if (!characters) {
                    vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory concatenating strings");
                    VM_NEXT();
                }

                memcpy(characters, left.data, (size_t)left.length);
                memcpy(characters + left.length, right.data, (size_t)right.length);

                GabStringValue result = {.data = characters, .length = (int32_t)total};

                memcpy(vm->registers + rd * VM_SLOT_SIZE, &result, sizeof(result));
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQS) {
                vm_conditional(vm, instruction, vm_equals);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NES) {
                vm_conditional(vm, instruction, vm_not_equals);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQF) {
                vm_conditional(vm, instruction, vm_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEF) {
                vm_conditional(vm, instruction, vm_not_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEF) {
                vm_conditional(vm, instruction, vm_less_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEF) {
                vm_conditional(vm, instruction, vm_greater_equalf);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_less_than_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_greater_than_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_less_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_greater_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEFK) {
                vm_conditionalfk(vm, instruction, chunk, vm_not_equal_floats);
                VM_NEXT();
            }
            VM_CASE(OP_ADDI) {
                vm_arithmetici(vm, instruction, vm_addi);
                VM_NEXT();
            }
            VM_CASE(OP_SUBI) {
                vm_arithmetici(vm, instruction, vm_subi);
                VM_NEXT();
            }
            VM_CASE(OP_MULI) {
                vm_arithmetici(vm, instruction, vm_muli);
                VM_NEXT();
            }
            VM_CASE(OP_DIVI) {
                if (!vm_check_divisor(vm, instruction, "divided by zero",
                                      "divided the most negative int by -1")) {
                    // The guard unwound, so a different frame is running now.
                    VM_RETRY();
                }

                vm_arithmetici(vm, instruction, vm_divi);
                VM_NEXT();
            }
            VM_CASE(OP_ITOF) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                vm_write_f32(vm, rd, (float)vm_read_i32(vm, r1));
                VM_NEXT();
            }
            VM_CASE(OP_FTOI) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                vm_write_i32(vm, rd, vm_ftoi(vm_read_f32(vm, r1)));
                VM_NEXT();
            }
            VM_CASE(OP_MODI) {
                if (!vm_check_divisor(vm, instruction, "took the remainder of a division by zero",
                                      "took the remainder of the most negative int and -1")) {
                    // The guard unwound, so a different frame is running now.
                    VM_RETRY();
                }

                vm_arithmetici(vm, instruction, vm_modi);
                VM_NEXT();
            }
            VM_CASE(OP_NEGI) {
                // On the unsigned width, so INT32_MIN wraps rather than
                // overflowing -- defined, and what the folder computes for the
                // same operand.
                int32_t value = vm_read_i32(vm, VM_DECODE_R_R1(instruction));

                vm_write_i32(vm, VM_DECODE_R_RD(instruction), (int32_t)(0u - (uint32_t)value));
                VM_NEXT();
            }
            VM_CASE(OP_NEGF) {
                vm_write_f32(vm, VM_DECODE_R_RD(instruction), -vm_read_f32(vm, VM_DECODE_R_R1(instruction)));
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LTI) {
                vm_conditionali(vm, instruction, vm_less_thani);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GTI) {
                vm_conditionali(vm, instruction, vm_greater_thani);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_EQI) {
                vm_conditionali(vm, instruction, vm_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_NEI) {
                vm_conditionali(vm, instruction, vm_not_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_LEI) {
                vm_conditionali(vm, instruction, vm_less_equali);
                VM_NEXT();
            }
            VM_CASE(OP_CMP_GEI) {
                vm_conditionali(vm, instruction, vm_greater_equali);
                VM_NEXT();
            }
            VM_CASE(OP_NEW) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                size_t type_index = VM_DECODE_I_KX(instruction);

                const Type *type = vm->program.heap_types.data[type_index];

                // The one place a heap object is created, so a host-supplied
                // allocator would replace this single call.
                void *object = object_alloc(DEFAULT_ALLOCATOR, type);

                if (!object) {
                    vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory");

                    vm_unwind(vm);

                    VM_RETRY();
                }

                // A pointer spans two slots at an even index, which codegen has
                // already arranged for rd.
                memcpy(vm->registers + rd * VM_SLOT_SIZE, &object, sizeof(object));
                VM_NEXT();
            }
            VM_CASE(OP_NULL) {
                vm_clear_pointer(vm, VM_DECODE_I_RD(instruction));
                VM_NEXT();
            }
            VM_CASE(OP_RELEASE) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                const Type *type = vm->program.heap_types.data[VM_DECODE_I_KX(instruction)];

                void *slot = vm->registers + rd * VM_SLOT_SIZE;

                object_release(DEFAULT_ALLOCATOR, type, slot);

                // Cleared as well as released, so the slot holds nothing that
                // reads as live. An abnormal unwind walks every slot the frame
                // may own a reference in, and this is what makes a slot that
                // was already released safe to visit again.
                memset(slot, 0, type_release_width(type));
                VM_NEXT();
            }
            VM_CASE(OP_CALL) {
                // I-type: a prototype index is not a register, and an 8-bit field
                // capped one VM at 255 functions across every module it loaded.
                // The frame is sized from the prototype and the arguments are
                // already in place above dest, so no third operand is needed.
                unsigned int dest = VM_DECODE_I_RD(instruction);
                size_t proto_index = VM_DECODE_I_KX(instruction);

                const FuncPrototype *proto = vm->program.prototypes.data[proto_index];

                // The callee's r0 is its return slot and its parameters are
                // r1..arity, so basing it at dest lines its parameters up with the
                // arguments the caller already placed above dest.
                size_t base = frame->base + dest * VM_SLOT_SIZE;

                if (!vm_push_frame(vm, proto, base, ip + 1, dest)) {
                    // Unwinding here is what makes the failure safe; the reason is
                    // left on the VM because the loop has no caller to return to.
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

                if (!vm_call_extern(vm, proto, frame->base + dest * VM_SLOT_SIZE)) {
                    vm_unwind(vm);

                    // The unwind left a different frame running, so where to
                    // carry on is not one past this instruction.
                    VM_RETRY();
                }

                VM_NEXT();
            }
            VM_CASE(OP_RETURN) VM_CASE(OP_RETURN_N) {
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = op == OP_RETURN ? 1 : VM_DECODE_R_R2(instruction);

                // The result is copied down to the frame's r0 before unwinding.
                // Source and destination never overlap: the callee builds its
                // result in temporaries above its parameters.
                uint8_t result[VM_MAX_RETURN_SLOTS * VM_SLOT_SIZE];
                memcpy(result, vm_reg_at(vm, r1), slots * VM_SLOT_SIZE);

                unsigned int dest = frame->dest;
                size_t frame_base = frame->base;
                vm_pop_frame(vm);

                if (vm->frame_count == 0) {
                    // The last frame returning ends this run, and its result stays
                    // at its own r0 so the caller can read it. That is stack slot 0
                    // for frame zero, and the call block's base for a host call —
                    // which is why it is written relative to the frame, not the
                    // stack.
                    memcpy(vm->stack + frame_base, result, slots * VM_SLOT_SIZE);
                    VM_RETRY();
                }

                memcpy(vm_reg_at(vm, dest), result, slots * VM_SLOT_SIZE);
                VM_RETRY();
            }
            VM_CASE(OP_LOAD_FIELD_1) {
                vm_load_field(vm, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_2) {
                vm_load_field(vm, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_4) {
                vm_load_field(vm, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_1) {
                vm_store_field(vm, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_2) {
                vm_store_field(vm, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_4) {
                vm_store_field(vm, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_ADDR_OF) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t offset = VM_DECODE_R_R2(instruction);

                // Addresses are absolute, not frame-relative: the inner may
                // outlive the frame the address was taken in, and a caller reading
                // through the pointer has a different base. The byte offset reaches
                // a field within the slots, so 'ref v.y' names the field, not v.
                vm_write_ptr(vm, rd, vm->registers + base * VM_SLOT_SIZE + offset);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_1) {
                vm_load_field_ptr(vm, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_2) {
                vm_load_field_ptr(vm, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_FIELD_PTR_4) {
                vm_load_field_ptr(vm, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_1) {
                vm_store_field_ptr(vm, instruction, 1);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_2) {
                vm_store_field_ptr(vm, instruction, 2);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_FIELD_PTR_4) {
                vm_store_field_ptr(vm, instruction, 4);
                VM_NEXT();
            }
            VM_CASE(OP_ARRAY_NEW) {
                unsigned int rd = VM_DECODE_I_RD(instruction);
                size_t type_index = VM_DECODE_I_KX(instruction);
                const Type *type = vm->program.heap_types.data[type_index];

                GabArrayValue array;
                memcpy(&array, vm->registers + rd * VM_SLOT_SIZE, sizeof(array));

                int32_t count = array.length;

                if (count < 0) {
                    vm_fail(vm, VM_RUN_ERR_BOUNDS, "an array's length cannot be negative");

                    vm_unwind(vm);
                    VM_RETRY();
                }

                // Computed in size_t from an int32_t count, so the product
                // cannot wrap: the widest it reaches is INT32_MAX times an
                // element's width, which a 64-bit size_t holds. The allocation
                // below is what refuses a length no memory could satisfy.
                size_t bytes = (size_t)count * type->element->size;

                // Typed as the element rather than as the array: what the block
                // holds is elements, and the array's own drop is what walks
                // them.
                //
                // A zero-length array still allocates, so its header holds a
                // real address: an empty block and a freed one would otherwise
                // be the same pointer.
                void *block = object_alloc_sized(DEFAULT_ALLOCATOR, type->element, bytes == 0 ? 1 : bytes);

                if (!block) {
                    vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory allocating an array");

                    vm_unwind(vm);
                    VM_RETRY();
                }

                array.data = block;
                memcpy(vm->registers + rd * VM_SLOT_SIZE, &array, sizeof(array));

                VM_NEXT();
            }
            VM_CASE(OP_ADD_PTR_REG) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t r2 = VM_DECODE_R_R2(instruction);

                int32_t offset;
                memcpy(&offset, vm_reg_at(vm, r2), sizeof(offset));

                vm_write_ptr(vm, rd, vm_read_ptr(vm, base) + offset);
                VM_NEXT();
            }
            VM_CASE(OP_BOUNDS_CHECK) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);

                GabArrayValue array;
                memcpy(&array, vm_reg_at(vm, rd), sizeof(array));

                int32_t index;
                memcpy(&index, vm_reg_at(vm, r1), sizeof(index));

                if (index < 0 || index >= array.length) {
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

                vm_write_ptr(vm, rd, vm_read_ptr(vm, base) + offset);
                VM_NEXT();
            }
            VM_CASE(OP_LOAD_PTR_N) {
                size_t rd = VM_DECODE_R_RD(instruction);
                size_t base = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                memcpy(vm_reg_at(vm, rd), vm_read_ptr(vm, base), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_STORE_PTR_N) {
                size_t base = VM_DECODE_R_RD(instruction);
                size_t r1 = VM_DECODE_R_R1(instruction);
                size_t slots = VM_DECODE_R_R2(instruction);

                memcpy(vm_read_ptr(vm, base), vm_reg_at(vm, r1), slots * VM_SLOT_SIZE);
                VM_NEXT();
            }
            VM_CASE(OP_FOR_LOOP) {
                int32_t next = vm_read_i32(vm, VM_DECODE_R_RD(instruction)) + 1;

                vm_write_i32(vm, VM_DECODE_R_RD(instruction), next);

                if (next < vm_read_i32(vm, VM_DECODE_R_R1(instruction))) {
                    ip += VM_DECODE_R_SIMM(instruction);
                }

                ip += 1;
                VM_JUMPED();
            }
            VM_CASE(OP_JMP) {
                ip += VM_DECODE_I_SIMM(instruction);
                ip += 1;
                VM_JUMPED();
            }
            VM_CASE(OP_JMP_IF_FALSE) {
                size_t reg = VM_DECODE_I_RD(instruction);

                bool cond = vm_read_i32(vm, reg);
                if (!cond) {
                    ip += VM_DECODE_I_SIMM(instruction);
                }

                ip += 1;
                VM_JUMPED();
            }
            VM_CASE(OP_JMP_IF_TRUE) {
                size_t reg = VM_DECODE_I_RD(instruction);

                bool cond = vm_read_i32(vm, reg);
                if (cond) {
                    ip += VM_DECODE_I_SIMM(instruction);
                }

                ip += 1;
                VM_JUMPED();
            }

            // Not an instruction, so nothing encodes it, and the 7-bit opcode
            // field cannot produce it for 53 opcodes. Listed because -Wswitch
            // counts every enum member.
            VM_CASE_UNREACHABLE(OP__COUNT)
        }
    }

    VM_EXIT()

    // Top-level code has no trailing return, so the loop usually ends by
    // running off the end of the chunk rather than through OP_RETURN.
    while (vm->frame_count > 0) {
        vm_pop_frame(vm);
    }
}

VmRunStatus interp_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest) {
    // A run reports only its own outcome, so whatever the last one left behind
    // is cleared before this one starts.
    vm->error = (VmError){.status = VM_RUN_OK};

    if (!vm_push_frame(vm, proto, base, 0, dest)) {
        vm_fail(vm, VM_RUN_ERR_STACK_OVERFLOW, "out of stack space");
        return vm->error.status;
    }

    vm_run_loop(vm);

    return vm->error.status;
}

VmRunStatus interp_run_extern(VM *vm, const ExternProto *proto, size_t base) {
    vm->error = (VmError){.status = VM_RUN_OK};

    // Nothing to unwind on failure: no frame was pushed, and an extern owns
    // none of the caller's slots.
    vm_call_extern(vm, proto, base);

    return vm->error.status;
}

VmRunStatus interp_run_top_level(VM *vm, const FuncPrototype *top_level) {
    // The top level runs as frame zero, so the interpreter has a single path
    // and OP_RETURN means the same thing everywhere.
    vm->frame_count = 0;

    return interp_run_frame(vm, top_level, 0, 0);
}
