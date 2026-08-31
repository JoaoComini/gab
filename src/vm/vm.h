#ifndef GAB_VM_H
#define GAB_VM_H

#include "arena.h"
#include "diagnostics.h"
#include "scope.h"
#include "slot.h"
#include "string/string_pool.h"
#include "util/list.h"
#include "vm/chunk.h"
#include "vm/link.h"
#include "vm/opcode.h"

#include <stdint.h>
#include <string.h>

#define VM_MAX_CALL_DEPTH 256

typedef struct {
    const FuncPrototype *proto;

    const Instruction *return_ip;

    size_t base;
    unsigned int dest;
} CallFrame;

GAB_LIST(FuncHandleList, func_handle_list, void *)

typedef struct {
    String *from;
    String *to;
} ModuleImport;

GAB_LIST(ModuleImportList, module_import_list, ModuleImport)

GAB_LIST(StagingArenaList, staging_arena_list, Arena *)

typedef enum {
    VM_RUN_OK,
    VM_RUN_ERR_CALL_DEPTH,
    VM_RUN_ERR_STACK_OVERFLOW,
    VM_RUN_ERR_OUT_OF_MEMORY,
    VM_RUN_ERR_DIVIDE_BY_ZERO,
    VM_RUN_ERR_DIVIDE_OVERFLOW,

    VM_RUN_ERR_BOUNDS,

    VM_RUN_ERR_EXTERN,
} VmRunStatus;

typedef struct {
    VmRunStatus status;
    char message[256];
} VmError;

typedef struct {
    Arena *arena;

    Arena *compile_arena;

    StringPool strings;

    Scope global_scope;

    ModuleScopeMap *module_scopes;

    ModuleImportList module_imports;

    /* Arenas of compiles that committed; a rejected compile destroys its own instead of adding it here. */
    StagingArenaList staging_arenas;
} Environment;

typedef struct VM {
    Environment env;
    Program program;

    FuncHandleList func_handles;

    uint8_t *stack;
    size_t stack_capacity;

    CallFrame frames[VM_MAX_CALL_DEPTH];
    size_t frame_count;

    const Instruction *instruction_pointer;

    VmError error;
} VM;

struct GabArgs {
    VM *vm;

    const struct Function *function;

    size_t base;
};

static inline uint8_t *vm_slot_at(const VM *vm, size_t i) { return vm->stack + i * VM_SLOT_SIZE; }

static inline int32_t vm_read_i32_at(const uint8_t *regs, size_t r) {
    int32_t value;
    memcpy(&value, regs + r * VM_SLOT_SIZE, sizeof(value));

    return value;
}

static inline float vm_read_f32_at(const uint8_t *regs, size_t r) {
    float value;
    memcpy(&value, regs + r * VM_SLOT_SIZE, sizeof(value));

    return value;
}

static inline void vm_write_i32_at(uint8_t *regs, size_t r, int32_t value) {
    memcpy(regs + r * VM_SLOT_SIZE, &value, sizeof(value));
}

static inline void vm_write_f32_at(uint8_t *regs, size_t r, float value) {
    memcpy(regs + r * VM_SLOT_SIZE, &value, sizeof(value));
}

static inline uint8_t *vm_read_ptr_at(const uint8_t *regs, size_t r) {
    uint8_t *address;
    memcpy(&address, regs + r * VM_SLOT_SIZE, sizeof(address));

    return address;
}

static inline void vm_write_ptr_at(uint8_t *regs, size_t r, uint8_t *address) {
    memcpy(regs + r * VM_SLOT_SIZE, &address, sizeof(address));
}

static inline uint8_t *vm_registers(const VM *vm) {
    return vm->stack + (vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].base : 0);
}

VM *vm_create();
void vm_free(VM *vm);

Scope *environment_module_scope(Environment *env, String *name);

#endif
