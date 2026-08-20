#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "lexer.h"
#include "object.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "type.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
    vm->heap_types = type_list_create();
    vm->func_handles = func_handle_list_create();
    vm->scripts = loaded_script_list_create();

    vm->arena = arena_create(ARENA_BLOCK_SIZE);
    vm->compile_arena = arena_create(ARENA_BLOCK_SIZE);

    // The pool must be live before the global scope: scope_init builds the
    // TypeRegistry, which interns the builtin type names.
    string_pool_init(&vm->strings, vm->arena);

    scope_init(&vm->global_scope, vm->arena, &vm->strings, NULL);
    vm->module_scopes = module_scope_map_create_alloc(arena_allocator(vm->arena), 8);

    // The root scope declares the builtins at generation 0, so the first
    // compile is generation 1 and never mistakes a builtin for its own work.
    vm->compile_generation = 0;

    vm->stack_capacity = VM_STACK_SIZE;
    vm->stack = calloc(vm->stack_capacity, VM_SLOT_SIZE);
    vm->registers = vm->stack;
    vm->frame_count = 0;
    vm->error = (VmError){.status = VM_RUN_OK, .message = NULL};

    return vm;
}

// Registers sit at base + r * VM_SLOT_SIZE, so the stack must hold every
// register the frame can address before it starts executing. 'needed' counts
// slots. The buffer is never resized, so this only reports whether the frame
// fits: a pointer into the stack must stay valid for as long as its pointee
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

        void *object;
        memcpy(&object, vm->stack + frame->base + ref.slot * VM_SLOT_SIZE, sizeof(object));

        if (!object) {
            continue;
        }

        memcpy(vm->stack + frame->base + ref.slot * VM_SLOT_SIZE, &(void *){NULL}, sizeof(object));

        gab_object_free(DEFAULT_ALLOCATOR, object);
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

void func_proto_free(FuncPrototype proto) {
    if (proto.chunk) {
        chunk_free(proto.chunk);
    }

    frame_ref_list_free(&proto.refs);
}

void vm_free(VM *vm) {
    func_proto_list_free(&vm->global_funcs);
    type_list_free(&vm->heap_types);

    // The handles themselves are gab.c's to free, and gab_vm_free has done so
    // by now; this releases only the array that tracked them.
    func_handle_list_free(&vm->func_handles);

    // Frees each loaded unit's top-level chunk through the item_free hook.
    loaded_script_list_free(&vm->scripts);

    // Frees the bucket arrays; the Scopes themselves are arena-owned.
    module_scope_map_destroy(vm->module_scopes);

    // Frees the bucket array, which walks entries — must happen before the
    // arena holding the string payloads is destroyed.
    string_pool_free(&vm->strings);

    free(vm->stack);

    arena_destroy(vm->arena);
    arena_destroy(vm->compile_arena);

    free(vm);
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

void vm_conditional(VM *vm, Instruction instruction, bool (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_write_i32(vm, rd, func(vm, r1, r2));
}

static void vm_load_field(VM *vm, Instruction instruction, size_t width) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t base = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    const uint8_t *source = vm->registers + base * VM_SLOT_SIZE + offset;

    // The destination is a whole slot, so a narrow field is widened rather
    // than left beside stale bytes.
    vm_write_i32(vm, rd, 0);
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

    vm_write_i32(vm, rd, 0);
    memcpy(vm_reg_at(vm, rd), source, width);
}

static void vm_store_field_ptr(VM *vm, Instruction instruction, size_t width) {
    size_t base = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t offset = VM_DECODE_R_R2(instruction);

    uint8_t *dest = vm_read_ptr(vm, base) + offset;

    memcpy(dest, vm_reg_at(vm, r1), width);
}

// The scope a module's declarations live in, created on first mention. A
// module accumulates across compiles, so a second unit naming the same module
// gets the scope the first one filled rather than a fresh one.
//
// The scope is parented to the root for builtin lookup but stays at depth 0:
// its declarations are a unit's top level, not a nested block.
Scope *vm_module_scope(VM *vm, String *name) {
    if (!name) {
        return &vm->global_scope;
    }

    Scope **existing = module_scope_map_lookup(vm->module_scopes, name);
    if (existing) {
        return *existing;
    }

    Scope *scope = arena_alloc(vm->arena, sizeof(Scope));
    scope_init_module(scope, vm->arena, &vm->strings, &vm->global_scope);

    module_scope_map_insert(vm->module_scopes, name, scope);

    return scope;
}

bool vm_compile(VM *vm, const char *source, CompiledScript *out, Diagnostics *diagnostics) {
    // Reclaimed at the start of a compile rather than the end of one, so
    // everything a compile produced — diagnostics included — stays readable
    // until the next compile begins.
    arena_reset(vm->compile_arena);

    // A new generation, so every name this compile declares is stamped as its
    // own. Anything it meets bearing an older stamp was declared by a previous
    // compile and is replaced rather than rejected.
    vm->compile_generation++;

    Lexer lexer = lexer_create(source, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTScript *script = ast_script_create();

    // Each stage is a precondition for the next: a failure must stop the
    // pipeline rather than let a malformed AST reach codegen.
    Chunk *chunk = NULL;
    CodegenFrameInfo frame_info = {.max_registers = 0, .refs = frame_ref_list_create()};
    String *module_name = NULL;

    if (parser_parse(&parser, script)) {
        // A unit that named no module declares into the root namespace, which
        // is what keeps a single-script host from needing a directive at all.
        if (script->module_name.data) {
            module_name = string_from_ref(&vm->strings, script->module_name);
        }

        Scope *scope = module_name ? vm_module_scope(vm, module_name) : &vm->global_scope;

        // Every name this compile declares is stamped with this generation.
        scope_set_generation(scope, vm->compile_generation);

        if (ast_script_resolve(vm->compile_arena, script, scope, vm->module_scopes, diagnostics)) {
            chunk = codegen_generate(
                script, (CodegenOutput){.funcs = &vm->global_funcs, .heap_types = &vm->heap_types},
                diagnostics, &frame_info);
        }
    }

    // Nothing reads the AST once codegen has run, so the compile owns it end to
    // end and only the chunk outlives this call.
    ast_script_destroy(script);

    if (!chunk) {
        return false;
    }

    out->chunk = chunk;
    out->max_registers = frame_info.max_registers;
    out->refs = frame_info.refs;
    out->module_name = module_name;

    return true;
}

void vm_compiled_script_free(CompiledScript *script) {
    if (!script->chunk) {
        return;
    }

    chunk_free(script->chunk);
    frame_ref_list_free(&script->refs);
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

// Dispatch. Two spellings of the same interpreter: a jump through a table of
// label addresses where the compiler has the extension for it, and a switch
// everywhere else.
//
// The table costs one indirect jump per instruction where the switch costs a
// bounds check and then the same jump. What actually makes it faster is the
// branch predictor: the switch has a single indirect jump that every opcode
// shares, so one history entry has to predict the whole instruction stream,
// while a jump at the end of each case is predicted on what tends to follow
// that opcode -- and bytecode is full of pairs that follow each other.
//
// The two must stay in step. Every opcode needs a label here and a case there,
// which _Static_assert on OP__COUNT and -Wswitch respectively enforce.
#if defined(VM_FORCE_SWITCH)
#define VM_COMPUTED_GOTO 0
#elif defined(__GNUC__) || defined(__clang__)
#define VM_COMPUTED_GOTO 1
#else
#define VM_COMPUTED_GOTO 0
#endif

// Reloads what the running frame's bytecode is, for the handlers that change
// which frame that is: a call, a return, or an unwind. The guard is the one
// VM_DISPATCH makes anyway, hoisted here so the common path never repeats it.
//
// Independent of how the interpreter dispatches, so both spellings share it.
#define VM_RELOAD()                                                                                          \
    do {                                                                                                     \
        if (vm->frame_count > 0) {                                                                           \
            frame = &vm->frames[vm->frame_count - 1];                                                        \
            chunk = frame->proto->chunk;                                                                     \
            code = chunk->instructions.data;                                                                 \
            code_size = chunk->instructions.size;                                                            \
        }                                                                                                    \
    } while (0)

// The two forms below differ only in how they reach the next handler. VM_NEXT
// advances to the following instruction; VM_RETRY resumes at wherever a handler
// has already placed the instruction pointer -- a call, a return, or a failure
// that unwound -- reloading the frame first.

#if VM_COMPUTED_GOTO

// Reads the next instruction and jumps straight to its handler. The bounds are
// the loop condition the switch form spells out, and failing them lands on the
// exit label rather than falling out of a loop.
#define VM_DISPATCH()                                                                                        \
    do {                                                                                                     \
        if (vm->frame_count == 0 || vm->instruction_pointer < 0 ||                                           \
            vm->instruction_pointer >= (ptrdiff_t)code_size) {                                               \
            goto vm_done;                                                                                    \
        }                                                                                                    \
                                                                                                             \
        instruction = code[vm->instruction_pointer];                                                         \
        op = VM_DECODE_OPCODE(instruction);                                                                  \
                                                                                                             \
        goto *vm_dispatch_table[op];                                                                         \
    } while (0)

#define VM_CASE(name) name##_label
#define VM_NEXT()                                                                                            \
    do {                                                                                                     \
        vm->instruction_pointer += 1;                                                                        \
        VM_DISPATCH();                                                                                       \
    } while (0)

#define VM_RETRY()                                                                                           \
    do {                                                                                                     \
        VM_RELOAD();                                                                                         \
        VM_DISPATCH();                                                                                       \
    } while (0)

#else

#define VM_CASE(name) case name

#define VM_NEXT()                                                                                            \
    do {                                                                                                     \
        goto vm_next;                                                                                        \
    } while (0)
#define VM_RETRY()                                                                                           \
    do {                                                                                                     \
        goto vm_retry;                                                                                       \
    } while (0)

#endif

// Runs until every frame the caller pushed has unwound. Both entry points
// share it: vm_run pushes frame zero, and a host call pushes one frame for the
// function it is invoking, so there is exactly one interpreter either way.
static void vm_run_loop(VM *vm) {
#if VM_COMPUTED_GOTO
    // One entry per opcode, in enum order: the index is the opcode itself.
    static void *const vm_dispatch_table[] = {
        [OP_LOAD_CONST] = &&OP_LOAD_CONST_label,
        [OP_LOAD_TRUE] = &&OP_LOAD_TRUE_label,
        [OP_LOAD_FALSE] = &&OP_LOAD_FALSE_label,
        [OP_MOVE] = &&OP_MOVE_label,
        [OP_MOVE_N] = &&OP_MOVE_N_label,
        [OP_ADDI] = &&OP_ADDI_label,
        [OP_SUBI] = &&OP_SUBI_label,
        [OP_MULI] = &&OP_MULI_label,
        [OP_DIVI] = &&OP_DIVI_label,
        [OP_MODI] = &&OP_MODI_label,
        [OP_ITOF] = &&OP_ITOF_label,
        [OP_FTOI] = &&OP_FTOI_label,
        [OP_CMP_LTI] = &&OP_CMP_LTI_label,
        [OP_CMP_GTI] = &&OP_CMP_GTI_label,
        [OP_CMP_EQI] = &&OP_CMP_EQI_label,
        [OP_CMP_NEI] = &&OP_CMP_NEI_label,
        [OP_CMP_LEI] = &&OP_CMP_LEI_label,
        [OP_CMP_GEI] = &&OP_CMP_GEI_label,
        [OP_ADDF] = &&OP_ADDF_label,
        [OP_SUBF] = &&OP_SUBF_label,
        [OP_MULF] = &&OP_MULF_label,
        [OP_DIVF] = &&OP_DIVF_label,
        [OP_ADDFK] = &&OP_ADDFK_label,
        [OP_SUBFK] = &&OP_SUBFK_label,
        [OP_MULFK] = &&OP_MULFK_label,
        [OP_DIVFK] = &&OP_DIVFK_label,
        [OP_CMP_LTF] = &&OP_CMP_LTF_label,
        [OP_CMP_GTF] = &&OP_CMP_GTF_label,
        [OP_CMP_EQF] = &&OP_CMP_EQF_label,
        [OP_CMP_NEF] = &&OP_CMP_NEF_label,
        [OP_CMP_LEF] = &&OP_CMP_LEF_label,
        [OP_CMP_GEF] = &&OP_CMP_GEF_label,
        [OP_JMP] = &&OP_JMP_label,
        [OP_JMP_IF_FALSE] = &&OP_JMP_IF_FALSE_label,
        [OP_JMP_IF_TRUE] = &&OP_JMP_IF_TRUE_label,
        [OP_CALL] = &&OP_CALL_label,
        [OP_NEW] = &&OP_NEW_label,
        [OP_RELEASE] = &&OP_RELEASE_label,
        [OP_RETURN] = &&OP_RETURN_label,
        [OP_RETURN_N] = &&OP_RETURN_N_label,
        [OP_LOAD_FIELD_1] = &&OP_LOAD_FIELD_1_label,
        [OP_LOAD_FIELD_2] = &&OP_LOAD_FIELD_2_label,
        [OP_LOAD_FIELD_4] = &&OP_LOAD_FIELD_4_label,
        [OP_STORE_FIELD_1] = &&OP_STORE_FIELD_1_label,
        [OP_STORE_FIELD_2] = &&OP_STORE_FIELD_2_label,
        [OP_STORE_FIELD_4] = &&OP_STORE_FIELD_4_label,
        [OP_ADDR_OF] = &&OP_ADDR_OF_label,
        [OP_LOAD_FIELD_PTR_1] = &&OP_LOAD_FIELD_PTR_1_label,
        [OP_LOAD_FIELD_PTR_2] = &&OP_LOAD_FIELD_PTR_2_label,
        [OP_LOAD_FIELD_PTR_4] = &&OP_LOAD_FIELD_PTR_4_label,
        [OP_STORE_FIELD_PTR_1] = &&OP_STORE_FIELD_PTR_1_label,
        [OP_STORE_FIELD_PTR_2] = &&OP_STORE_FIELD_PTR_2_label,
        [OP_STORE_FIELD_PTR_4] = &&OP_STORE_FIELD_PTR_4_label,
        [OP_ADD_PTR] = &&OP_ADD_PTR_label,
        [OP_LOAD_PTR_N] = &&OP_LOAD_PTR_N_label,
        [OP_STORE_PTR_N] = &&OP_STORE_PTR_N_label,
        [OP_FOR_LOOP] = &&OP_FOR_LOOP_label,
    };

    _Static_assert(sizeof(vm_dispatch_table) / sizeof(vm_dispatch_table[0]) == OP__COUNT,
                   "the dispatch table must have an entry for every opcode");
#endif

    CallFrame *frame;
    Chunk *chunk;
    Instruction instruction;
    OpCode op;
    const Instruction *code = NULL;
    size_t code_size = 0;

#if VM_COMPUTED_GOTO
    VM_RELOAD();
    VM_DISPATCH();
#else
vm_retry:
    VM_RELOAD();

    // The same bounds VM_DISPATCH spells: the pointer is signed, so a jump
    // that went too far back is negative here rather than a huge index that
    // would read as a normal end of function.
    while (vm->frame_count > 0 && vm->instruction_pointer >= 0 &&
           vm->instruction_pointer < (ptrdiff_t)code_size) {
        instruction = code[vm->instruction_pointer];
        op = VM_DECODE_OPCODE(instruction);

        switch (op) {
#endif
    VM_CASE(OP_LOAD_CONST) : {
        size_t reg = VM_DECODE_I_RD(instruction);
        size_t const_index = VM_DECODE_I_KX(instruction);
        Constant constant = constpool_get(chunk->const_pool, const_index);

        memcpy(vm_reg_at(vm, reg), &constant, VM_SLOT_SIZE);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_TRUE) : {
        size_t reg = VM_DECODE_I_RD(instruction);
        vm_write_i32(vm, reg, 1);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FALSE) : {
        size_t reg = VM_DECODE_I_RD(instruction);
        vm_write_i32(vm, reg, 0);
        VM_NEXT();
    }
    VM_CASE(OP_MOVE) : {
        int rd = VM_DECODE_R_RD(instruction);
        int r1 = VM_DECODE_R_R1(instruction);

        memcpy(vm_reg_at(vm, rd), vm_reg_at(vm, r1), VM_SLOT_SIZE);
        VM_NEXT();
    }
    VM_CASE(OP_MOVE_N) : {
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
    VM_CASE(OP_ADDFK) : {
        vm_arithmeticfk(vm, instruction, chunk, vm_add_floats);
        VM_NEXT();
    }
    VM_CASE(OP_SUBFK) : {
        vm_arithmeticfk(vm, instruction, chunk, vm_sub_floats);
        VM_NEXT();
    }
    VM_CASE(OP_MULFK) : {
        vm_arithmeticfk(vm, instruction, chunk, vm_mul_floats);
        VM_NEXT();
    }
    VM_CASE(OP_DIVFK) : {
        vm_arithmeticfk(vm, instruction, chunk, vm_div_floats);
        VM_NEXT();
    }
    VM_CASE(OP_ADDF) : {
        vm_arithmeticf(vm, instruction, vm_addf);
        VM_NEXT();
    }
    VM_CASE(OP_SUBF) : {
        vm_arithmeticf(vm, instruction, vm_subf);
        VM_NEXT();
    }
    VM_CASE(OP_MULF) : {
        vm_arithmeticf(vm, instruction, vm_mulf);
        VM_NEXT();
    }
    VM_CASE(OP_DIVF) : {
        vm_arithmeticf(vm, instruction, vm_divf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_LTF) : {
        vm_conditional(vm, instruction, vm_less_thanf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_GTF) : {
        vm_conditional(vm, instruction, vm_greater_thanf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_EQF) : {
        vm_conditional(vm, instruction, vm_equalf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_NEF) : {
        vm_conditional(vm, instruction, vm_not_equalf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_LEF) : {
        vm_conditional(vm, instruction, vm_less_equalf);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_GEF) : {
        vm_conditional(vm, instruction, vm_greater_equalf);
        VM_NEXT();
    }
    VM_CASE(OP_ADDI) : {
        vm_arithmetici(vm, instruction, vm_addi);
        VM_NEXT();
    }
    VM_CASE(OP_SUBI) : {
        vm_arithmetici(vm, instruction, vm_subi);
        VM_NEXT();
    }
    VM_CASE(OP_MULI) : {
        vm_arithmetici(vm, instruction, vm_muli);
        VM_NEXT();
    }
    VM_CASE(OP_DIVI) : {
        if (!vm_check_divisor(vm, instruction, "divided by zero", "divided the most negative int by -1")) {
            VM_NEXT();
        }

        vm_arithmetici(vm, instruction, vm_divi);
        VM_NEXT();
    }
    VM_CASE(OP_ITOF) : {
        size_t rd = VM_DECODE_R_RD(instruction);
        size_t r1 = VM_DECODE_R_R1(instruction);

        vm_write_f32(vm, rd, (float)vm_read_i32(vm, r1));
        VM_NEXT();
    }
    VM_CASE(OP_FTOI) : {
        size_t rd = VM_DECODE_R_RD(instruction);
        size_t r1 = VM_DECODE_R_R1(instruction);

        vm_write_i32(vm, rd, vm_ftoi(vm_read_f32(vm, r1)));
        VM_NEXT();
    }
    VM_CASE(OP_MODI) : {
        if (!vm_check_divisor(vm, instruction, "took the remainder of a division by zero",
                              "took the remainder of the most negative int and -1")) {
            VM_NEXT();
        }

        vm_arithmetici(vm, instruction, vm_modi);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_LTI) : {
        vm_conditionali(vm, instruction, vm_less_thani);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_GTI) : {
        vm_conditionali(vm, instruction, vm_greater_thani);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_EQI) : {
        vm_conditionali(vm, instruction, vm_equali);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_NEI) : {
        vm_conditionali(vm, instruction, vm_not_equali);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_LEI) : {
        vm_conditionali(vm, instruction, vm_less_equali);
        VM_NEXT();
    }
    VM_CASE(OP_CMP_GEI) : {
        vm_conditionali(vm, instruction, vm_greater_equali);
        VM_NEXT();
    }
    VM_CASE(OP_NEW) : {
        unsigned int rd = VM_DECODE_I_RD(instruction);
        size_t type_index = VM_DECODE_I_KX(instruction);

        const Type *type = vm->heap_types.data[type_index];

        // The one place a heap object is created, so a host-supplied
        // allocator would replace this single call.
        void *object = gab_object_alloc(DEFAULT_ALLOCATOR, type);

        if (!object) {
            vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory");

            vm_unwind(vm);

            VM_NEXT();
        }

        // A pointer spans two slots at an even index, which codegen has
        // already arranged for rd.
        memcpy(vm->registers + rd * VM_SLOT_SIZE, &object, sizeof(object));
        VM_NEXT();
    }
    VM_CASE(OP_RELEASE) : {
        unsigned int rd = VM_DECODE_R_RD(instruction);

        void *object;
        memcpy(&object, vm->registers + rd * VM_SLOT_SIZE, sizeof(object));

        // Cleared as well as released, so the slot holds NULL rather than a
        // pointer to something freed. An abnormal unwind walks every slot
        // the frame may own a reference in, and this is what makes a slot
        // that was already released safe to visit again.
        vm_clear_pointer(vm, rd);

        gab_object_free(DEFAULT_ALLOCATOR, object);
        VM_NEXT();
    }
    VM_CASE(OP_CALL) : {
        // I-type: a prototype index is not a register, and an 8-bit field
        // capped one VM at 255 functions across every module it loaded.
        // The frame is sized from the prototype and the arguments are
        // already in place above dest, so no third operand is needed.
        unsigned int dest = VM_DECODE_I_RD(instruction);
        size_t proto_index = VM_DECODE_I_KX(instruction);

        const FuncPrototype *proto = &vm->global_funcs.data[proto_index];

        // The callee's r0 is its return slot and its parameters are
        // r1..arity, so basing it at dest lines its parameters up with the
        // arguments the caller already placed above dest.
        size_t base = frame->base + dest * VM_SLOT_SIZE;

        if (!vm_push_frame(vm, proto, base, vm->instruction_pointer + 1, dest)) {
            // Unwinding here is what makes the failure safe; the reason is
            // left on the VM because the loop has no caller to return to.
            vm_fail(vm, VM_RUN_ERR_CALL_DEPTH, "call depth exceeded");

            vm_unwind(vm);

            VM_NEXT();
        }

        VM_RETRY();
    }
    VM_CASE(OP_RETURN) : VM_CASE(OP_RETURN_N) : {
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
    VM_CASE(OP_LOAD_FIELD_1) : {
        vm_load_field(vm, instruction, 1);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FIELD_2) : {
        vm_load_field(vm, instruction, 2);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FIELD_4) : {
        vm_load_field(vm, instruction, 4);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_1) : {
        vm_store_field(vm, instruction, 1);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_2) : {
        vm_store_field(vm, instruction, 2);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_4) : {
        vm_store_field(vm, instruction, 4);
        VM_NEXT();
    }
    VM_CASE(OP_ADDR_OF) : {
        size_t rd = VM_DECODE_R_RD(instruction);
        size_t base = VM_DECODE_R_R1(instruction);
        size_t offset = VM_DECODE_R_R2(instruction);

        // Addresses are absolute, not frame-relative: the pointee may
        // outlive the frame the address was taken in, and a caller reading
        // through the pointer has a different base. The byte offset reaches
        // a field within the slots, so '&v.y' names the field, not v.
        vm_write_ptr(vm, rd, vm->registers + base * VM_SLOT_SIZE + offset);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FIELD_PTR_1) : {
        vm_load_field_ptr(vm, instruction, 1);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FIELD_PTR_2) : {
        vm_load_field_ptr(vm, instruction, 2);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_FIELD_PTR_4) : {
        vm_load_field_ptr(vm, instruction, 4);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_PTR_1) : {
        vm_store_field_ptr(vm, instruction, 1);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_PTR_2) : {
        vm_store_field_ptr(vm, instruction, 2);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_FIELD_PTR_4) : {
        vm_store_field_ptr(vm, instruction, 4);
        VM_NEXT();
    }
    VM_CASE(OP_ADD_PTR) : {
        size_t rd = VM_DECODE_R_RD(instruction);
        size_t base = VM_DECODE_R_R1(instruction);
        size_t offset = VM_DECODE_R_R2(instruction);

        vm_write_ptr(vm, rd, vm_read_ptr(vm, base) + offset);
        VM_NEXT();
    }
    VM_CASE(OP_LOAD_PTR_N) : {
        size_t rd = VM_DECODE_R_RD(instruction);
        size_t base = VM_DECODE_R_R1(instruction);
        size_t slots = VM_DECODE_R_R2(instruction);

        memcpy(vm_reg_at(vm, rd), vm_read_ptr(vm, base), slots * VM_SLOT_SIZE);
        VM_NEXT();
    }
    VM_CASE(OP_STORE_PTR_N) : {
        size_t base = VM_DECODE_R_RD(instruction);
        size_t r1 = VM_DECODE_R_R1(instruction);
        size_t slots = VM_DECODE_R_R2(instruction);

        memcpy(vm_read_ptr(vm, base), vm_reg_at(vm, r1), slots * VM_SLOT_SIZE);
        VM_NEXT();
    }
    VM_CASE(OP_FOR_LOOP) : {
        int32_t next = vm_read_i32(vm, VM_DECODE_R_RD(instruction)) + 1;

        vm_write_i32(vm, VM_DECODE_R_RD(instruction), next);

        if (next < vm_read_i32(vm, VM_DECODE_R_R1(instruction))) {
            vm->instruction_pointer += VM_DECODE_R_SIMM(instruction);
        }

        VM_NEXT();
    }
    VM_CASE(OP_JMP) : {
        vm->instruction_pointer += VM_DECODE_I_SIMM(instruction);
        VM_NEXT();
    }
    VM_CASE(OP_JMP_IF_FALSE) : {
        size_t reg = VM_DECODE_I_RD(instruction);

        bool cond = vm_read_i32(vm, reg);
        if (!cond) {
            vm->instruction_pointer += VM_DECODE_I_SIMM(instruction);
        }

        VM_NEXT();
    }
    VM_CASE(OP_JMP_IF_TRUE) : {
        size_t reg = VM_DECODE_I_RD(instruction);

        bool cond = vm_read_i32(vm, reg);
        if (cond) {
            vm->instruction_pointer += VM_DECODE_I_SIMM(instruction);
        }

        VM_NEXT();
    }

#if !VM_COMPUTED_GOTO
// Not an instruction, so nothing encodes it. Listed because -Wswitch
// counts it, and reaching it would mean a decoded opcode outside the
// enum -- which the 7-bit field cannot produce for 53 opcodes.
case OP__COUNT:
    goto vm_done;
}

vm_next : vm->instruction_pointer += 1;
}

goto vm_done;
#endif

vm_done:;

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
        .refs = script->refs,
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
