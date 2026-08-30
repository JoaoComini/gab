#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "builtin/builtin.h"
#include "lexer.h"
#include "object.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "type/type.h"
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

#define VM_STACK_SIZE (VM_MAX_CALL_DEPTH * VM_MAX_REGISTERS)

#define VM_STACK_ALIGNMENT 8

_Static_assert(_Alignof(max_align_t) >= VM_STACK_ALIGNMENT,
               "malloc alignment is insufficient for the stack; use aligned allocation");

static void environment_init(Environment *env) {
    env->arena = arena_create(ARENA_BLOCK_SIZE);
    env->compile_arena = arena_create(ARENA_BLOCK_SIZE);

    string_pool_init(&env->strings, env->arena);

    scope_init(&env->global_scope, env->arena, &env->strings, NULL);
    env->module_scopes = module_scope_map_create_alloc(arena_allocator(env->arena), 8);

    env->module_imports = module_import_list_create();
}

static void environment_free(Environment *env) {
    module_import_list_free(&env->module_imports);

    module_scope_map_destroy(env->module_scopes);

    string_pool_free(&env->strings);

    arena_destroy(env->arena);
    arena_destroy(env->compile_arena);
}

static void program_init(Program *program) {
    program->prototypes = func_proto_list_create();
    program->heap_shapes = heap_shape_list_create();
    program->shape_types = type_list_create();
    program->strings = string_list_create();
    program->top_levels = top_level_list_create();
    program->extern_bindings = extern_binding_list_create();
    program->extern_protos = extern_proto_list_create();
}

static void program_free(Program *program) {
    func_proto_list_free(&program->prototypes);
    heap_shape_list_free(&program->heap_shapes);
    type_list_free(&program->shape_types);
    string_list_free(&program->strings);
    extern_binding_list_free(&program->extern_bindings);
    extern_proto_list_free(&program->extern_protos);

    top_level_list_free(&program->top_levels);
}

VM *vm_create() {
    VM *vm = malloc(sizeof(VM));

    environment_init(&vm->env);
    program_init(&vm->program);

    vm->func_handles = func_handle_list_create();

    vm->stack_capacity = VM_STACK_SIZE;
    vm->stack = calloc(vm->stack_capacity, VM_SLOT_SIZE);
    vm->frame_count = 0;
    vm->instruction_pointer = NULL;
    vm->error = (VmError){.status = VM_RUN_OK};

    builtin_register_all(vm);

    return vm;
}

void vm_free(VM *vm) {
    func_handle_list_free(&vm->func_handles);

    program_free(&vm->program);
    environment_free(&vm->env);

    free(vm->stack);
    free(vm);
}

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
