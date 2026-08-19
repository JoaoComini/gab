#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "refcounted.h"
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

static void vm_pop_frame(VM *vm);

// Writes NULL over a pointer slot, so a slot that has already been released
// reads as empty rather than as an address that was freed.
static void vm_clear_pointer(VM *vm, size_t reg) {
    void *null_pointer = NULL;

    memcpy(vm->registers + reg * sizeof(Value), &null_pointer, sizeof(null_pointer));
}

// Drops every reference a frame still holds. Only ever called while unwinding
// from a failure: a run that ends normally has already executed the releases
// codegen emitted at each scope's close, which is both cheaper and more precise
// than this — it releases at the brace rather than at the frame's end.
//
// A slot listed on the prototype either holds a live reference or holds NULL,
// because releasing one clears it. That is what makes walking the list safe
// despite sibling blocks reusing slots.
static void vm_release_frame_refs(VM *vm, const CallFrame *frame) {
    const FrameRefList *refs = &frame->proto->refs;

    for (size_t i = 0; i < refs->size; i++) {
        FrameRef ref = refs->data[i];

        void *object;
        memcpy(&object, vm->stack + frame->base + ref.slot * sizeof(Value), sizeof(object));

        if (!object) {
            continue;
        }

        memcpy(vm->stack + frame->base + ref.slot * sizeof(Value), &(void *){NULL}, sizeof(object));

        if (ref.weak) {
            gab_release_weak(DEFAULT_ALLOCATOR, object);
            continue;
        }

        gab_release(DEFAULT_ALLOCATOR, object);
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

float vm_addf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float + vm_reg(vm, r2)->as_float; }

float vm_subf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float - vm_reg(vm, r2)->as_float; }

float vm_mulf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float * vm_reg(vm, r2)->as_float; }

float vm_divf(VM *vm, size_t r1, size_t r2) { return vm_reg(vm, r1)->as_float / vm_reg(vm, r2)->as_float; }

// The second operand, which the k bit makes either a register to read or a
// small immediate encoded in the instruction itself.
//
// Immediates are integers: a float literal has no compact encoding in eight
// bits, so codegen never marks one, and the float path reads a register as it
// always did.
static inline int32_t vm_operand2i(const VM *vm, Instruction instruction) {
    size_t r2 = VM_DECODE_R_R2(instruction);

    return VM_DECODE_R_K(instruction) ? (int32_t)r2 : vm_reg(vm, r2)->as_int;
}

void vm_arithmeticf(VM *vm, Instruction instruction, float (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm_reg(vm, rd)->as_float = func(vm, r1, r2);
}

// The integer operations take values rather than register indices, so the same
// body serves a register operand and an immediate one.
int32_t vm_addi(int32_t a, int32_t b) { return a + b; }

int32_t vm_subi(int32_t a, int32_t b) { return a - b; }

int32_t vm_muli(int32_t a, int32_t b) { return a * b; }

int32_t vm_divi(int32_t a, int32_t b) { return a / b; }

void vm_arithmetici(VM *vm, Instruction instruction, int32_t (*func)(int32_t, int32_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);

    vm_reg(vm, rd)->as_int = func(vm_reg(vm, r1)->as_int, vm_operand2i(vm, instruction));
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

    vm_reg(vm, rd)->as_int = func(vm_reg(vm, r1)->as_int, vm_operand2i(vm, instruction));
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

// The scope a module's declarations live in, created on first mention. A
// module accumulates across compiles, so a second unit naming the same module
// gets the scope the first one filled rather than a fresh one.
//
// The scope is parented to the root for builtin lookup but stays at depth 0:
// its declarations are a unit's top level, not a nested block.
// Naming another module's type must not bring that module into being, so this
// only ever looks: an unknown name is a miss the resolver reports as an
// unknown type. The ModuleScopeFn the resolver calls, hence the void * VM.
Scope *vm_module_scope_lookup(void *ctx, String *name) {
    VM *vm = (VM *)ctx;

    if (!name) {
        return &vm->global_scope;
    }

    Scope **existing = module_scope_map_lookup(vm->module_scopes, name);

    return existing ? *existing : NULL;
}

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

        if (ast_script_resolve(vm->compile_arena, script, scope, vm_module_scope_lookup, vm, diagnostics)) {
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
            vm_conditionali(vm, instruction, vm_less_thani);
            break;
        }
        case OP_CMP_GTI: {
            vm_conditionali(vm, instruction, vm_greater_thani);
            break;
        }
        case OP_CMP_EQI: {
            vm_conditionali(vm, instruction, vm_equali);
            break;
        }
        case OP_CMP_NEI: {
            vm_conditionali(vm, instruction, vm_not_equali);
            break;
        }
        case OP_CMP_LEI: {
            vm_conditionali(vm, instruction, vm_less_equali);
            break;
        }
        case OP_CMP_GEI: {
            vm_conditionali(vm, instruction, vm_less_equali);
            break;
        }
        case OP_NEW: {
            unsigned int rd = VM_DECODE_I_RD(instruction);
            size_t type_index = VM_DECODE_I_KX(instruction);

            const Type *type = vm->heap_types.data[type_index];

            // DEFAULT_ALLOCATOR until GabConfig's realloc_fn lands in step 8;
            // this is the one place a heap object is created, so that is the
            // only line that has to change.
            void *object = gab_refcounted_alloc(DEFAULT_ALLOCATOR, type);

            if (!object) {
                vm_fail(vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory");

                vm_unwind(vm);

                break;
            }

            // A pointer spans two slots at an even index, which codegen has
            // already arranged for rd.
            memcpy(vm->registers + rd * sizeof(Value), &object, sizeof(object));
            break;
        }
        case OP_RELEASE: {
            unsigned int rd = VM_DECODE_R_RD(instruction);

            void *object;
            memcpy(&object, vm->registers + rd * sizeof(Value), sizeof(object));

            // Cleared as well as released, so the slot holds NULL rather than a
            // pointer to something freed. An abnormal unwind walks every slot
            // the frame may own a reference in, and this is what makes a slot
            // that was already released safe to visit again.
            vm_clear_pointer(vm, rd);

            gab_release(DEFAULT_ALLOCATOR, object);
            break;
        }
        case OP_RETAIN: {
            unsigned int rd = VM_DECODE_R_RD(instruction);

            void *object;
            memcpy(&object, vm->registers + rd * sizeof(Value), sizeof(object));

            gab_retain(object);
            break;
        }
        case OP_LOAD_NULL_PTR: {
            unsigned int rd = VM_DECODE_R_RD(instruction);
            void *null_pointer = NULL;

            memcpy(vm->registers + rd * sizeof(Value), &null_pointer, sizeof(null_pointer));
            break;
        }
        case OP_CHECK_ALIVE: {
            unsigned int rd = VM_DECODE_R_RD(instruction);

            void *object;
            memcpy(&object, vm->registers + rd * sizeof(Value), sizeof(object));

            if (gab_is_alive(object)) {
                break;
            }

            // The object a weak reference named is gone. Reading through it
            // would find a zeroed payload, which is a plausible-looking answer
            // rather than an obviously wrong one — so the run fails here
            // instead, where the mistake is.
            vm_fail(vm, VM_RUN_ERR_DANGLING_WEAK, "dereferenced a weak pointer whose object has been freed");

            vm_unwind(vm);

            break;
        }
        case OP_RETAIN_WEAK: {
            unsigned int rd = VM_DECODE_R_RD(instruction);

            void *object;
            memcpy(&object, vm->registers + rd * sizeof(Value), sizeof(object));

            gab_retain_weak(object);
            break;
        }
        case OP_RELEASE_WEAK: {
            unsigned int rd = VM_DECODE_R_RD(instruction);

            void *object;
            memcpy(&object, vm->registers + rd * sizeof(Value), sizeof(object));

            vm_clear_pointer(vm, rd);

            gab_release_weak(DEFAULT_ALLOCATOR, object);
            break;
        }
        case OP_CALL: {
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
            size_t base = frame->base + dest * sizeof(Value);

            if (!vm_push_frame(vm, proto, base, vm->instruction_pointer + 1, dest)) {
                // Unwinding here is what makes the failure safe; the reason is
                // left on the VM because the loop has no caller to return to.
                vm_fail(vm, VM_RUN_ERR_CALL_DEPTH, "call depth exceeded");

                vm_unwind(vm);

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
