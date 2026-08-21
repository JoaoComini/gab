#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "gab.h"
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
#include "vm/vm_dispatch.h"

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

static void environment_init(Environment *env) {
    env->arena = arena_create(ARENA_BLOCK_SIZE);
    env->compile_arena = arena_create(ARENA_BLOCK_SIZE);

    // The pool must be live before the global scope: scope_init builds the
    // TypeRegistry, which interns the builtin type names.
    string_pool_init(&env->strings, env->arena);

    scope_init(&env->global_scope, env->arena, &env->strings, NULL);
    env->module_scopes = module_scope_map_create_alloc(arena_allocator(env->arena), 8);

    env->module_imports = module_import_list_create();
}

static void environment_free(Environment *env) {
    module_import_list_free(&env->module_imports);

    // Frees the bucket arrays; the Scopes themselves are arena-owned.
    module_scope_map_destroy(env->module_scopes);

    // Frees the bucket array, which walks entries — must happen before the
    // arena holding the string payloads is destroyed.
    string_pool_free(&env->strings);

    arena_destroy(env->arena);
    arena_destroy(env->compile_arena);
}

static void program_init(Program *program) {
    program->prototypes = func_proto_list_create();
    program->heap_types = type_list_create();
    program->strings = string_list_create();
    program->top_levels = top_level_list_create();
    program->externs = extern_binding_list_create();
    program->builtin_proto_count = 0;
}

// Frees only what the program allocated for itself. The prototypes and types it
// indexes come from the environment's arena and go with it.
static void program_free(Program *program) {
    func_proto_list_free(&program->prototypes);
    type_list_free(&program->heap_types);
    string_list_free(&program->strings);
    extern_binding_list_free(&program->externs);

    // Frees each loaded unit's top-level chunk through the item_free hook.
    top_level_list_free(&program->top_levels);
}

// 's.len()'. Reads the count the receiver's header already carries, which is
// what a string argument's accessor hands back.
static void string_len(GabArgs *args) {
    int32_t length = 0;

    gab_arg_get_string(args, 0, &length);
    gab_return_int(args, length);
}

// The methods a builtin type answers, registered the way a host registers an
// extern: a Symbol in the type's method map, and a prototype carrying a C body.
//
// Registered rather than known to the compiler, so that adding one is an entry
// here instead of a case in the resolver and another in codegen. It costs a
// call where an instruction would do; nothing yet makes that worth a second
// mechanism.
static void register_builtin_method(VM *vm, Type *receiver, const char *name, GabExternFn body,
                                    Type *return_type) {
    Arena *arena = vm->env.arena;

    Symbol *symbol = arena_alloc(arena, sizeof(Symbol));
    symbol->kind = SYMBOL_FUNC;
    symbol->func.return_type = return_type;
    symbol->func.param_count = 1;
    symbol->func.params = arena_alloc(arena, sizeof(Type *));
    symbol->func.is_extern = true;
    symbol->func.name = string_from_cstr(&vm->env.strings, name);
    symbol->func.module = NULL;

    // The receiver is parameter zero, by value: a string is a header that
    // copies, and a method that only reads it wants no indirection.
    symbol->func.params[0] = receiver;

    FuncPrototype *proto = arena_alloc(arena, sizeof(FuncPrototype));
    *proto = (FuncPrototype){
        .chunk = NULL,
        .native = body,
        .extern_symbol = symbol,
        .arity = 1,
        .max_registers = (int)(1 + (receiver->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE),
        .refs = frame_ref_list_create(),
    };

    symbol->func.proto_index = vm->program.prototypes.size;
    func_proto_list_add(&vm->program.prototypes, proto);

    type_add_method(arena, receiver, string_from_cstr(&vm->env.strings, name), symbol);
}

static void register_builtin_methods(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    register_builtin_method(vm, registry->builtins.string_type, "len", string_len,
                            registry->builtins.int_type);
}

VM *vm_create() {
    VM *vm = malloc(sizeof(VM));

    environment_init(&vm->env);
    program_init(&vm->program);

    vm->func_handles = func_handle_list_create();

    vm->stack_capacity = VM_STACK_SIZE;
    vm->stack = calloc(vm->stack_capacity, VM_SLOT_SIZE);
    vm->registers = vm->stack;
    vm->frame_count = 0;
    vm->instruction_pointer = 0;
    vm->error = (VmError){.status = VM_RUN_OK};

    // After the program exists: a method's prototype goes in its list.
    register_builtin_methods(vm);

    // Everything after this point is a unit's.
    vm->program.builtin_proto_count = vm->program.prototypes.size;

    return vm;
}

void vm_free(VM *vm) {
    // The handles themselves are gab.c's to free, and gab_vm_free has done so
    // by now; this releases only the array that tracked them.
    func_handle_list_free(&vm->func_handles);

    // Program before environment: what a prototype allocated is freed here,
    // and the prototype itself lives in the environment's arena.
    program_free(&vm->program);
    environment_free(&vm->env);

    free(vm->stack);
    free(vm);
}

// The scope a module's declarations live in, created on first mention. A
// module accumulates across compiles, so a second unit naming the same module
// gets the scope the first one filled rather than a fresh one.
//
// The scope is parented to the root for builtin lookup but stays at depth 0:
// its declarations are a unit's top level, not a nested block.
Scope *environment_module_scope(Environment *env, String *name) {
    assert(name && "every unit names a module");

    Scope **existing = module_scope_map_lookup(env->module_scopes, name);
    if (existing) {
        return *existing;
    }

    Scope *scope = arena_alloc(env->arena, sizeof(Scope));
    scope_init_module(scope, env->arena, &env->strings, &env->global_scope);

    module_scope_map_insert(env->module_scopes, name, scope);

    return scope;
}
