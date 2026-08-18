#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "type.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE 2048

// The stack never moves, so it is sized for the worst case up front: every
// frame to the call-depth limit addressing every register it can name. That is
// a few hundred kilobytes, and it is what makes '&local' sound — an address
// into a buffer realloc could move would dangle, and untagged slots give the
// VM no way to find live pointers and rebase them.
#define VM_STACK_SIZE (VM_MAX_CALL_DEPTH * VM_MAX_REGISTERS)

// The base must hold an 8-byte value at its natural alignment. malloc already
// guarantees at least alignof(max_align_t), which covers this on every platform
// with an 8-byte scalar type; the assertion fails the build anywhere it would not.
#define VM_STACK_ALIGNMENT 8

_Static_assert(_Alignof(max_align_t) >= VM_STACK_ALIGNMENT,
               "malloc alignment is insufficient for the stack; use aligned allocation");

VM *vm_create() {
    VM *vm = malloc(sizeof(VM));
    vm->instruction_pointer = 0;

    vm->global_funcs = func_proto_list_create();

    vm->arena = arena_create(ARENA_BLOCK_SIZE);
    vm->compile_arena = arena_create(ARENA_BLOCK_SIZE);

    // The pool must be live before the global scope: scope_init builds the
    // TypeRegistry, which interns the builtin type names.
    string_pool_init(&vm->strings, vm->arena);

    scope_init(&vm->global_scope, vm->arena, &vm->strings, NULL);

    vm->stack_capacity = VM_STACK_SIZE;
    vm->stack = calloc(vm->stack_capacity, sizeof(Value));
    vm->registers = vm->stack;
    vm->frame_count = 0;
    vm->error = (VmError){.status = VM_RUN_OK, .message = NULL};

    return vm;
}

// Registers sit at base + r * sizeof(Value), so the stack must hold every
// register the frame can address before it starts executing. 'needed' counts
// slots. The buffer is never resized, so this only reports whether the frame
// fits: a pointer into the stack must stay valid for as long as its pointee
// does, which a moving buffer cannot promise.
static bool vm_reserve_stack(const VM *vm, size_t needed) { return needed <= vm->stack_capacity; }

static bool vm_push_frame(VM *vm, const FuncPrototype *proto, size_t base, size_t return_ip,
                          unsigned int dest) {
    if (vm->frame_count == VM_MAX_CALL_DEPTH) {
        return false;
    }

    // base is a byte offset; the reservation is in slots.
    if (!vm_reserve_stack(vm, base / sizeof(Value) + proto->max_registers)) {
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

static void vm_pop_frame(VM *vm) {
    CallFrame frame = vm->frames[--vm->frame_count];

    if (vm->frame_count == 0) {
        vm->registers = vm->stack;
        return;
    }

    vm->registers = vm->stack + vm->frames[vm->frame_count - 1].base;
    vm->instruction_pointer = frame.return_ip;
}

void func_proto_free(FuncPrototype proto) {
    if (proto.chunk) {
        chunk_free(proto.chunk);
    }
}

void vm_free(VM *vm) {
    func_proto_list_free(&vm->global_funcs);

    // Frees the bucket array, which walks entries — must happen before the
    // arena holding the string payloads is destroyed.
    string_pool_free(&vm->strings);

    free(vm->stack);

    arena_destroy(vm->arena);
    arena_destroy(vm->compile_arena);

    free(vm);
}

float vm_addf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float + vm_reg(vm, r2)->as_float; }

float vm_subf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float - vm_reg(vm, r2)->as_float; }

float vm_mulf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float * vm_reg(vm, r2)->as_float; }

float vm_divf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float / vm_reg(vm, r2)->as_float; }

void vm_arithmeticf(VM *vm, Instruction instruction, float (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_reg(vm, rd)->as_float = func(vm, r1, r2);
}

int32_t vm_addi(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int + vm_reg(vm, r2)->as_int; }

int32_t vm_subi(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int - vm_reg(vm, r2)->as_int; }

int32_t vm_muli(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int * vm_reg(vm, r2)->as_int; }

int32_t vm_divi(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int / vm_reg(vm, r2)->as_int; }

void vm_arithmetici(VM *vm, Instruction instruction, int32_t (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_reg(vm, rd)->as_int = func(vm, r1, r2);
}

bool vm_less_thanf(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_float < vm_reg(vm, r2)->as_float;
}

bool vm_greater_thanf(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_float > vm_reg(vm, r2)->as_float;
}

bool vm_equalf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float == vm_reg(vm, r2)->as_float; }

bool vm_not_equalf(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_float != vm_reg(vm, r2)->as_float;
}

bool vm_less_equalf(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_float <= vm_reg(vm, r2)->as_float;
}

bool vm_greater_equalf(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_float >= vm_reg(vm, r2)->as_float;
}

bool vm_less_thani(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int < vm_reg(vm, r2)->as_int; }

bool vm_greater_thani(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_int > vm_reg(vm, r2)->as_int;
}

bool vm_equali(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int == vm_reg(vm, r2)->as_int; }

bool vm_not_equali(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int != vm_reg(vm, r2)->as_int; }

bool vm_less_equali(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_int <= vm_reg(vm, r2)->as_int; }

bool vm_greater_equali(VM *vm, size_t r1, size_t r2) {
    return vm_reg(vm, r1)->as_int >= vm_reg(vm, r2)->as_int;
}

void vm_conditional(VM *vm, Instruction instruction, bool (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_reg(vm, rd)->as_int = func(vm, r1, r2);
}

static void vm_load_field(VM *vm, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm->registers + base * sizeof(Value) + offset;

    // The destination is a whole slot, so a narrow field is widened rather
    // than left beside stale bytes.
    vm_reg(vm, rd)->as_int = 0;
    memcpy(vm_reg(vm, rd), source, width);
}

static void vm_store_field(VM *vm, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm->registers + base * sizeof(Value) + offset;

    // Only the field's own bytes are written; anything sharing the slot keeps
    // its value.
    memcpy(dest, vm_reg(vm, r1), width);
}

// The address a 2-slot pointer register holds. The slot pair is placed at an
// even index and the stack base is 8-byte aligned, so this is a natural read.
static uint8_t *vm_read_pointer(const VM *vm, size_t reg) {
    uint8_t *address;
    memcpy(&address, vm->registers + reg * sizeof(Value), sizeof(address));

    return address;
}

static void vm_write_pointer(VM *vm, size_t reg, uint8_t *address) {
    memcpy(vm->registers + reg * sizeof(Value), &address, sizeof(address));
}

static void vm_load_field_ptr(VM *vm, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm_read_pointer(vm, base) + offset;

    vm_reg(vm, rd)->as_int = 0;
    memcpy(vm_reg(vm, rd), source, width);
}

static void vm_store_field_ptr(VM *vm, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm_read_pointer(vm, base) + offset;

    memcpy(dest, vm_reg(vm, r1), width);
}

bool vm_compile(VM *vm, const char *source, CompiledScript *out, Diagnostics *diagnostics) {
    // Reclaimed at the start of a compile rather than the end of one, so
    // everything a compile produced — diagnostics included — stays readable
    // until the next compile begins.
    arena_reset(vm->compile_arena);

    Lexer lexer = lexer_create(source, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTScript *script = ast_script_create();

    // Each stage is a precondition for the next: a failure must stop the
    // pipeline rather than let a malformed AST reach codegen.
    Chunk *chunk = NULL;
    unsigned int max_registers = 0;

    if (parser_parse(&parser, script) &&
        ast_script_resolve(vm->compile_arena, script, &vm->global_scope, diagnostics)) {
        chunk = codegen_generate(script, &vm->global_funcs, diagnostics, &max_registers);
    }

    // Nothing reads the AST once codegen has run, so the compile owns it end to
    // end and only the chunk outlives this call.
    ast_script_destroy(script);

    if (!chunk) {
        return false;
    }

    out->chunk = chunk;
    out->max_registers = max_registers;

    return true;
}

void vm_compiled_script_free(CompiledScript *script) {
    if (!script->chunk) {
        return;
    }

    chunk_free(script->chunk);
    script->chunk = NULL;
}

// Records why a run stopped. The first failure wins: a later one is a
// consequence of unwinding, not an independent problem.
static void vm_fail(VM *vm, VmRunStatus status, const char *message) {
    if (vm->error.status != VM_RUN_OK) {
        return;
    }

    vm->error.status = status;
    vm->error.message = message;
}

// Runs until every frame the caller pushed has unwound. Both entry points
// share it: vm_run pushes frame zero, and a host call pushes one frame for the
// function it is invoking, so there is exactly one interpreter either way.
static void vm_run_loop(VM *vm) {
    while (vm->frame_count > 0 &&
           vm->instruction_pointer < vm->frames[vm->frame_count - 1].proto->chunk->instructions.size) {
        CallFrame *frame = &vm->frames[vm->frame_count - 1];
        Chunk *chunk = frame->proto->chunk;

        Instruction instruction = instruction_list_get(&chunk->instructions, vm->instruction_pointer);

        OpCode op = VM_DECODE_OPCODE(instruction);
        switch (op) {
        case OP_LOAD_CONST: {
            size_t reg = VM_DECODE_I_RD(instruction);
            size_t const_index = VM_DECODE_I_KX(instruction);
            (*vm_reg(vm, reg)) = constpool_get(chunk->const_pool, const_index);
            break;
        }
        case OP_LOAD_TRUE: {
            size_t reg = VM_DECODE_I_RD(instruction);
            vm_reg(vm, reg)->as_int = 1;
            break;
        }
        case OP_LOAD_FALSE: {
            size_t reg = VM_DECODE_I_RD(instruction);
            vm_reg(vm, reg)->as_int = 0;
            break;
        }
        case OP_MOVE: {
            int rd = VM_DECODE_R_RD(instruction);
            int r1 = VM_DECODE_R_R1(instruction);

            (*vm_reg(vm, rd)) = (*vm_reg(vm, r1));
            break;
        }
        case OP_ADDF: {
            vm_arithmeticf(vm, instruction, vm_addf);
            break;
        }
        case OP_SUBF: {
            vm_arithmeticf(vm, instruction, vm_subf);
            break;
        }
        case OP_MULF: {
            vm_arithmeticf(vm, instruction, vm_mulf);
            break;
        }
        case OP_DIVF: {
            vm_arithmeticf(vm, instruction, vm_divf);
            break;
        }
        case OP_CMP_LTF: {
            vm_conditional(vm, instruction, vm_less_thanf);
            break;
        }
        case OP_CMP_GTF: {
            vm_conditional(vm, instruction, vm_greater_thanf);
            break;
        }
        case OP_CMP_EQF: {
            vm_conditional(vm, instruction, vm_equalf);
            break;
        }
        case OP_CMP_NEF: {
            vm_conditional(vm, instruction, vm_not_equalf);
            break;
        }
        case OP_CMP_LEF: {
            vm_conditional(vm, instruction, vm_less_equalf);
            break;
        }
        case OP_CMP_GEF: {
            vm_conditional(vm, instruction, vm_less_equalf);
            break;
        }
        case OP_ADDI: {
            vm_arithmetici(vm, instruction, vm_addi);
            break;
        }
        case OP_SUBI: {
            vm_arithmetici(vm, instruction, vm_subi);
            break;
        }
        case OP_MULI: {
            vm_arithmetici(vm, instruction, vm_muli);
            break;
        }
        case OP_DIVI: {
            vm_arithmetici(vm, instruction, vm_divi);
            break;
        }
        case OP_CMP_LTI: {
            vm_conditional(vm, instruction, vm_less_thani);
            break;
        }
        case OP_CMP_GTI: {
            vm_conditional(vm, instruction, vm_greater_thani);
            break;
        }
        case OP_CMP_EQI: {
            vm_conditional(vm, instruction, vm_equali);
            break;
        }
        case OP_CMP_NEI: {
            vm_conditional(vm, instruction, vm_not_equali);
            break;
        }
        case OP_CMP_LEI: {
            vm_conditional(vm, instruction, vm_less_equali);
            break;
        }
        case OP_CMP_GEI: {
            vm_conditional(vm, instruction, vm_less_equali);
            break;
        }
        case OP_CALL: {
            unsigned int dest = VM_DECODE_R_RD(instruction);
            size_t proto_index = VM_DECODE_R_R1(instruction);

            const FuncPrototype *proto = &vm->global_funcs.data[proto_index];

            // The callee's r0 is its return slot and its parameters are
            // r1..arity, so basing it at dest lines its parameters up with the
            // arguments the caller already placed above dest.
            size_t base = frame->base + dest * sizeof(Value);

            if (!vm_push_frame(vm, proto, base, vm->instruction_pointer + 1, dest)) {
                // Unwinding here is what makes the failure safe; the reason is
                // left on the VM because the loop has no caller to return to.
                vm_fail(vm, VM_RUN_ERR_CALL_DEPTH, "call depth exceeded");

                while (vm->frame_count > 0) {
                    vm_pop_frame(vm);
                }

                break;
            }

            continue;
        }
        case OP_RETURN:
        case OP_RETURN_N: {
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t slots = op == OP_RETURN ? 1 : VM_DECODE_R_R2(instruction);

            // The result is copied down to the frame's r0 before unwinding.
            // Source and destination never overlap: the callee builds its
            // result in temporaries above its parameters.
            Value result[VM_MAX_RETURN_SLOTS];
            memcpy(result, vm_reg(vm, r1), slots * sizeof(Value));

            unsigned int dest = frame->dest;
            size_t frame_base = frame->base;
            vm_pop_frame(vm);

            if (vm->frame_count == 0) {
                // The last frame returning ends this run, and its result stays
                // at its own r0 so the caller can read it. That is stack slot 0
                // for frame zero, and the call block's base for a host call —
                // which is why it is written relative to the frame, not the
                // stack.
                memcpy(vm->stack + frame_base, result, slots * sizeof(Value));
                continue;
            }

            memcpy(vm_reg(vm, dest), result, slots * sizeof(Value));
            continue;
        }
        case OP_LOAD_FIELD_1: {
            vm_load_field(vm, instruction, 1);
            break;
        }
        case OP_LOAD_FIELD_2: {
            vm_load_field(vm, instruction, 2);
            break;
        }
        case OP_LOAD_FIELD_4: {
            vm_load_field(vm, instruction, 4);
            break;
        }
        case OP_STORE_FIELD_1: {
            vm_store_field(vm, instruction, 1);
            break;
        }
        case OP_STORE_FIELD_2: {
            vm_store_field(vm, instruction, 2);
            break;
        }
        case OP_STORE_FIELD_4: {
            vm_store_field(vm, instruction, 4);
            break;
        }
        case OP_ADDR_OF: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t base = VM_DECODE_R_R1(instruction);
            size_t offset = VM_DECODE_R_R2(instruction);

            // Addresses are absolute, not frame-relative: the pointee may
            // outlive the frame the address was taken in, and a caller reading
            // through the pointer has a different base. The byte offset reaches
            // a field within the slots, so '&v.y' names the field, not v.
            vm_write_pointer(vm, rd, vm->registers + base * sizeof(Value) + offset);
            break;
        }
        case OP_LOAD_FIELD_PTR_1: {
            vm_load_field_ptr(vm, instruction, 1);
            break;
        }
        case OP_LOAD_FIELD_PTR_2: {
            vm_load_field_ptr(vm, instruction, 2);
            break;
        }
        case OP_LOAD_FIELD_PTR_4: {
            vm_load_field_ptr(vm, instruction, 4);
            break;
        }
        case OP_STORE_FIELD_PTR_1: {
            vm_store_field_ptr(vm, instruction, 1);
            break;
        }
        case OP_STORE_FIELD_PTR_2: {
            vm_store_field_ptr(vm, instruction, 2);
            break;
        }
        case OP_STORE_FIELD_PTR_4: {
            vm_store_field_ptr(vm, instruction, 4);
            break;
        }
        case OP_ADD_PTR: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t base = VM_DECODE_R_R1(instruction);
            size_t offset = VM_DECODE_R_R2(instruction);

            vm_write_pointer(vm, rd, vm_read_pointer(vm, base) + offset);
            break;
        }
        case OP_LOAD_PTR_N: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t base = VM_DECODE_R_R1(instruction);
            size_t slots = VM_DECODE_R_R2(instruction);

            memcpy(vm_reg(vm, rd), vm_read_pointer(vm, base), slots * sizeof(Value));
            break;
        }
        case OP_STORE_PTR_N: {
            size_t base = VM_DECODE_R_RD(instruction);
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t slots = VM_DECODE_R_R2(instruction);

            memcpy(vm_read_pointer(vm, base), vm_reg(vm, r1), slots * sizeof(Value));
            break;
        }
        case OP_JMP: {
            vm->instruction_pointer += VM_DECODE_I_IMM(instruction);
            break;
        }
        case OP_JMP_IF_FALSE: {
            size_t reg = VM_DECODE_I_RD(instruction);

            bool cond = vm_reg(vm, reg)->as_int;
            if (!cond) {
                size_t offset = VM_DECODE_I_IMM(instruction);
                vm->instruction_pointer += offset;
            }

            break;
        }
        case OP_JMP_IF_TRUE: {
            size_t reg = VM_DECODE_I_RD(instruction);

            bool cond = vm_reg(vm, reg)->as_int;
            if (cond) {
                size_t offset = VM_DECODE_I_IMM(instruction);
                vm->instruction_pointer += offset;
            }

            break;
        }
        }

        vm->instruction_pointer += 1;
    }

    // Top-level code has no trailing return, so the loop usually ends by
    // running off the end of the chunk rather than through OP_RETURN.
    while (vm->frame_count > 0) {
        vm_pop_frame(vm);
    }
}

VmRunStatus vm_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest) {
    // A run reports only its own outcome, so whatever the last one left behind
    // is cleared before this one starts.
    vm->error = (VmError){.status = VM_RUN_OK, .message = NULL};

    if (!vm_push_frame(vm, proto, base, 0, dest)) {
        vm_fail(vm, VM_RUN_ERR_STACK_OVERFLOW, "out of stack space");
        return vm->error.status;
    }

    vm_run_loop(vm);

    return vm->error.status;
}

VmRunStatus vm_run(VM *vm, const CompiledScript *script) {
    // The top level runs as frame zero, so the interpreter has a single path
    // and OP_RETURN means the same thing everywhere.
    FuncPrototype top_level = {
        .chunk = script->chunk,
        .arity = 0,
        .max_registers = (int)script->max_registers,
    };

    vm->frame_count = 0;

    return vm_run_frame(vm, &top_level, 0, 0);
}

void vm_execute(VM *vm, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<script>");

    CompiledScript script;

    if (!vm_compile(vm, source, &script, &diagnostics)) {
        diagnostics_print(&diagnostics, stderr);
        diagnostics_free(&diagnostics);
        return;
    }

    diagnostics_free(&diagnostics);

    // The convenience path is the one caller that still reports for itself; a
    // host uses gab_module_run and gets the status instead.
    if (vm_run(vm, &script) != VM_RUN_OK) {
        fprintf(stderr, "<script>: %s\n", vm->error.message);
    }

    vm_compiled_script_free(&script);
}
