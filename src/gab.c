#include "gab.h"

#include "binding.h"
#include "compile.h"
#include "diagnostics.h"
#include "object.h"
#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GabVM;

struct GabFunc {
    const Function *function;

    TypeRegistry *registry;

    size_t sig_param_count;
    const Type **sig_params;

    unsigned int arg_slots;

    unsigned int *param_slot;

    void *params_block;

    size_t return_size;
};

struct GabCall {
    GabFunc *fn;

    uint8_t *args;

    bool *arg_set;
    size_t args_pending;

    void *block;
    size_t capacity;
};

static void gab_func_bind(GabFunc *fn);

static unsigned int gab_signature_slots(TypeRegistry *registry, const Function *function);

static bool gab_func_alloc_params(GabFunc *fn, size_t param_count) {
    size_t sig_params_at = 0;
    size_t bytes = param_count * sizeof(const Type *);

    size_t param_slot_at = bytes;
    bytes += param_count * sizeof(unsigned int);

    if (bytes == 0) {
        free(fn->params_block);

        fn->params_block = NULL;
        fn->sig_params = NULL;
        fn->param_slot = NULL;

        return true;
    }

    void *block = calloc(1, bytes);
    if (!block) {
        return false;
    }

    free(fn->params_block);

    fn->params_block = block;
    fn->sig_params = (const Type **)((char *)block + sig_params_at);
    fn->param_slot = (unsigned int *)((char *)block + param_slot_at);

    return true;
}

static void gab_error_clear(GabError *err) {
    if (!err) {
        return;
    }

    err->message[0] = '\0';
    err->line = 0;
    err->column = 0;
}

static void gab_error_set(GabError *err, int line, int column, const char *message) {
    if (!err) {
        return;
    }

    snprintf(err->message, sizeof(err->message), "%s", message);
    err->line = line;
    err->column = column;
}

static void gab_error_from_diagnostics(GabError *err, const Diagnostics *diagnostics) {
    if (!err) {
        return;
    }

    for (size_t i = 0; i < diagnostics_count(diagnostics); i++) {
        const Diagnostic *diag = diagnostics_get(diagnostics, i);

        gab_error_set(err, diag->span.line, diag->span.column, diag->message);
        return;
    }

    gab_error_set(err, 0, 0, "compilation failed");
}

GabVM *gab_vm_new(void) { return (GabVM *)vm_create(); }

void gab_vm_free(GabVM *handle) {
    if (!handle) {
        return;
    }

    VM *vm = (VM *)handle;

    for (size_t i = 0; i < vm->func_handles.size; i++) {
        GabFunc *fn = vm->func_handles.data[i];

        free(fn->params_block);
        free(fn);
    }

    vm->func_handles.size = 0;

    vm_free(vm);
}

int32_t gab_arg_get_int(GabArgs *args, int index) { return args_int(args, index); }

float gab_arg_get_float(GabArgs *args, int index) { return args_float(args, index); }

bool gab_arg_get_bool(GabArgs *args, int index) { return args_bool(args, index); }

void gab_arg_get_struct(GabArgs *args, int index, void *out, size_t size) {
    args_struct(args, index, out, size);
}

const char *gab_arg_get_string(GabArgs *args, int index, int32_t *out_length) {
    StrRef value = args_string(args, index);

    if (out_length) {
        *out_length = value.length;
    }

    return value.data;
}

void *gab_arg_get_pointer(GabArgs *args, int index) { return args_pointer(args, index); }

void gab_drop(GabArgs *args, int index) { args_drop(args, index); }

void gab_drop_pointer(void *pointer) {
    if (!pointer) {
        return;
    }

    object_free(&DEFAULT_ALLOCATOR, pointer);
}

void gab_return_int(GabArgs *args, int32_t value) { args_return_int(args, value); }

void gab_return_float(GabArgs *args, float value) { args_return_float(args, value); }

void gab_return_bool(GabArgs *args, bool value) { args_return_bool(args, value); }

void gab_return_struct(GabArgs *args, const void *data, size_t size) { args_return_struct(args, data, size); }

void gab_return_pointer(GabArgs *args, void *pointer) { args_return_pointer(args, pointer); }

void gab_error(GabArgs *args, const char *message) {
    if (!args) {
        return;
    }

    vm_fail(args->vm, VM_RUN_ERR_EXTERN, message ? message : "the extern function failed");
}

static bool extern_bind(GabVM *handle, const char *module, const char *type, const char *name, GabExternFn fn,
                        void *symbol, GabError *err) {
    gab_error_clear(err);

    if (!handle || !name || (!fn && !symbol)) {
        gab_error_set(err, 0, 0, "binding an extern requires a VM, a name, and a function");
        return false;
    }

    VM *vm = (VM *)handle;

    String *interned_owner = (type && type[0] != '\0') ? string_from_cstr(&vm->env.strings, type) : NULL;

    String *interned_name = string_from_cstr(&vm->env.strings, name);
    String *interned_module =
        (module && module[0] != '\0') ? string_from_cstr(&vm->env.strings, module) : NULL;

    if (!interned_name || (module && module[0] != '\0' && !interned_module)) {
        gab_error_set(err, 0, 0, "out of memory");
        return false;
    }

    for (size_t i = 0; i < vm->program.extern_bindings.size; i++) {
        const ExternBinding *binding = &vm->program.extern_bindings.data[i];

        if (binding->name == interned_name && binding->module == interned_module &&
            binding->owner == interned_owner) {
            gab_error_set(err, 0, 0, "an extern of this name is already bound in this module");
            return false;
        }
    }

    extern_binding_list_add(&vm->program.extern_bindings, (ExternBinding){.module = interned_module,
                                                                          .owner = interned_owner,
                                                                          .name = interned_name,
                                                                          .fn = fn,
                                                                          .symbol = symbol});

    return true;
}

bool gab_extern(GabVM *handle, const char *module, const char *type, const char *name, GabExternFn fn,
                GabError *err) {
    return extern_bind(handle, module, type, name, fn, NULL, err);
}

bool gab_extern_c(GabVM *handle, const char *module, const char *type, const char *name, void *symbol,
                  GabError *err) {
    return extern_bind(handle, module, type, name, NULL, symbol, err);
}

void gab_ctx_fail(GabCtx *ctx, const char *message) { gab_error((GabArgs *)ctx, message); }

size_t gab_ctx_type_count(GabCtx *ctx) { return ctx ? ((GabArgs *)ctx)->function->type_arg_count : 0; }

GabTypeKind gab_ctx_type_kind(GabCtx *ctx, size_t index) {
    const Function *function = ((GabArgs *)ctx)->function;

    assert(index < function->type_arg_count && "a C body read a type argument its declaration does not have");

    return (GabTypeKind)type_kind(function->type_args[index]);
}

size_t gab_ctx_type_size(GabCtx *ctx, size_t index) {
    GabArgs *args = (GabArgs *)ctx;
    const Function *function = args->function;

    assert(index < function->type_arg_count && "a C body read a type argument its declaration does not have");

    return type_registry_size_of(args->vm->env.global_scope.type_registry, function->type_args[index]);
}

void *gab_box(GabCtx *ctx) { return args_box_return((GabArgs *)ctx); }

bool gab_ctx_return_string(GabCtx *ctx, const char *data, int32_t length) {
    return args_return_string_copy((GabArgs *)ctx, data, length);
}

bool gab_load(GabVM *handle, const char *name, const char *src, GabError *err) {
    gab_error_clear(err);

    if (!handle || !src) {
        gab_error_set(err, 0, 0, "gab_load requires a VM and a source string");
        return false;
    }

    VM *vm = (VM *)handle;

    char unit_name[128];
    snprintf(unit_name, sizeof(unit_name), "%s", name ? name : "<script>");

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, unit_name);

    FuncPrototype compiled = {0};
    bool ok = compile_unit(vm, src, &compiled, &diagnostics);

    if (!ok) {
        gab_error_from_diagnostics(err, &diagnostics);
        diagnostics_free(&diagnostics);

        return false;
    }

    diagnostics_free(&diagnostics);

    if (interp_run_top_level(vm, &compiled) != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        func_proto_free(&compiled);

        return false;
    }

    top_level_list_add(&vm->program.top_levels, compiled);

    return true;
}

static Scope *gab_namespace(VM *vm, const char *module) {
    if (!module || module[0] == '\0') {
        return NULL;
    }

    String *interned = string_from_cstr(&vm->env.strings, module);
    Scope **found = interned ? module_scope_map_lookup(vm->env.module_scopes, interned) : NULL;

    return found ? *found : NULL;
}

const GabType *gab_find_type(GabVM *handle, const char *module, const char *name) {
    if (!handle || !name) {
        return NULL;
    }

    VM *vm = (VM *)handle;

    Scope *scope = gab_namespace(vm, module);
    if (!scope) {
        return NULL;
    }

    String *interned = string_from_cstr(&vm->env.strings, name);
    if (!interned) {
        return NULL;
    }

    return (const GabType *)scope_type_lookup(scope, interned);
}

static TypeRegistry *gab_registry(GabVM *handle) {
    VM *vm = (VM *)handle;

    return vm ? vm->env.global_scope.type_registry : NULL;
}

size_t gab_type_size(GabVM *vm, const GabType *type) {
    return type && vm ? type_registry_size_of(gab_registry(vm), (const Type *)type) : 0;
}

size_t gab_type_align(GabVM *vm, const GabType *type) {
    return type && vm ? type_registry_align_of(gab_registry(vm), (const Type *)type) : 0;
}

bool gab_field_offset(GabVM *handle, const GabType *type_handle, const char *field, size_t *out_offset) {
    if (!handle || !type_handle || !field) {
        return false;
    }

    const Type *type = (const Type *)type_handle;
    const TypeLayout *layout = type_registry_layout_of(gab_registry(handle), type);

    const TypeFields *fields = type_registry_fields_of(gab_registry(handle), type);

    for (size_t i = 0; i < fields->count; i++) {
        const TypeField *candidate = &fields->fields[i];

        if (candidate->name && strcmp(candidate->name->data, field) == 0) {
            if (out_offset) {
                *out_offset = layout->offsets[i];
            }

            return true;
        }
    }

    return false;
}

GabFunc *gab_lookup(GabVM *handle, const char *module, const char *name, GabError *err) {
    gab_error_clear(err);

    if (!handle || !name) {
        gab_error_set(err, 0, 0, "gab_lookup requires a VM and a name");
        return NULL;
    }

    VM *vm = (VM *)handle;

    Scope *scope = gab_namespace(vm, module);

    if (!scope) {
        char message[256];
        snprintf(message, sizeof(message), "no module named '%s'", module);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    String *interned = string_from_cstr(&vm->env.strings, name);
    Binding *binding = interned ? scope_binding_lookup(scope, interned) : NULL;

    if (!binding) {
        char message[256];
        snprintf(message, sizeof(message), "'%s' is not declared", name);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    if (binding->kind != BINDING_FUNC) {
        char message[256];
        snprintf(message, sizeof(message), "'%s' is not a function", name);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    const Function *function = binding->func;

    if (function->param_count > VM_MAX_FRAME_SLOTS) {
        char message[256];
        snprintf(message, sizeof(message), "'%s' has more parameters than a frame can hold", name);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    GabFunc *fn = calloc(1, sizeof(GabFunc));
    if (!fn) {
        gab_error_set(err, 0, 0, "out of memory");
        return NULL;
    }

    if (!gab_func_alloc_params(fn, function->param_count)) {
        free(fn);
        gab_error_set(err, 0, 0, "out of memory");

        return NULL;
    }

    fn->function = binding->func;
    fn->registry = gab_registry(handle);

    gab_func_bind(fn);

    func_handle_list_add(&vm->func_handles, fn);

    return fn;
}

int gab_func_arity(const GabFunc *fn) { return fn ? (int)fn->sig_param_count : 0; }

static unsigned int gab_signature_slots(TypeRegistry *registry, const Function *function) {
    unsigned int slots = 1;

    for (size_t i = 0; i < function->param_count; i++) {
        slots += args_type_slots(registry, function->params[i]);
    }

    return slots;
}

static void gab_func_bind(GabFunc *fn) {
    const Function *function = fn->function;

    fn->sig_param_count = function->param_count;

    unsigned int offset = 1;

    for (size_t i = 0; i < function->param_count; i++) {
        fn->sig_params[i] = function->params[i];
        fn->param_slot[i] = offset;
        offset += args_type_slots(fn->registry, function->params[i]);
    }

    fn->arg_slots = offset - 1;
    fn->return_size = type_registry_size_of(fn->registry, function->return_type);
}

static bool gab_call_stage(GabCall *call, GabError *err) {
    const GabFunc *fn = call->fn;

    size_t slots = (size_t)fn->arg_slots + 1;
    size_t bytes = slots * VM_SLOT_SIZE + fn->sig_param_count * sizeof(bool);

    if (bytes > call->capacity) {
        void *block = calloc(1, bytes);
        if (!block) {
            gab_error_set(err, 0, 0, "out of memory");
            return false;
        }

        free(call->block);

        call->block = block;
        call->capacity = bytes;
    }

    memset(call->block, 0, call->capacity);

    call->args = (uint8_t *)call->block;
    call->arg_set = (bool *)((char *)call->block + slots * VM_SLOT_SIZE);

    call->args_pending = fn->sig_param_count;

    return true;
}

GabCall *gab_call_init(GabFunc *fn, GabError *err) {
    gab_error_clear(err);

    if (!fn) {
        gab_error_set(err, 0, 0, "gab_call_init requires a function");
        return NULL;
    }

    GabCall *call = calloc(1, sizeof(GabCall));
    if (!call) {
        gab_error_set(err, 0, 0, "out of memory");
        return NULL;
    }

    call->fn = fn;

    if (!gab_call_stage(call, err)) {
        free(call);
        return NULL;
    }

    return call;
}

void gab_call_free(GabCall *call) {
    if (!call) {
        return;
    }

    free(call->block);
    free(call);
}

static uint8_t *gab_arg_slot_where(GabCall *call, int index, bool (*accepts)(const Type *, TypeKind),
                                   TypeKind expected) {
    if (!call) {
        return NULL;
    }

    const GabFunc *fn = call->fn;

    if (index < 0 || (size_t)index >= fn->sig_param_count) {
        return NULL;
    }

    const Type *param = fn->sig_params[index];

    if (!param || !accepts(param, expected)) {
        return NULL;
    }

    if (!call->arg_set[index]) {
        call->arg_set[index] = true;
        call->args_pending--;
    }

    return call->args + (size_t)fn->param_slot[index] * VM_SLOT_SIZE;
}

static bool accepts_indirect(const Type *type, TypeKind expected) {
    (void)expected;

    return type_is_indirect(type);
}

static bool accepts_kind(const Type *type, TypeKind expected) { return type_kind(type) == expected; }

static uint8_t *gab_arg_slot(GabCall *call, int index, TypeKind expected) {
    return gab_arg_slot_where(call, index, accepts_kind, expected);
}

bool gab_arg_int(GabCall *call, int index, int32_t value) {
    uint8_t *slot = gab_arg_slot(call, index, TYPE_INT);
    if (!slot) {
        return false;
    }

    memcpy(slot, &value, sizeof(value));

    return true;
}

bool gab_arg_float(GabCall *call, int index, float value) {
    uint8_t *slot = gab_arg_slot(call, index, TYPE_FLOAT);
    if (!slot) {
        return false;
    }

    memcpy(slot, &value, sizeof(value));

    return true;
}

bool gab_arg_bool(GabCall *call, int index, bool value) {
    uint8_t *slot = gab_arg_slot(call, index, TYPE_BOOL);
    if (!slot) {
        return false;
    }

    int32_t as_word = value ? 1 : 0;

    memcpy(slot, &as_word, sizeof(as_word));

    return true;
}

bool gab_arg_struct(GabCall *call, int index, const void *data, size_t size) {
    if (call && index >= 0 && (size_t)index < call->fn->sig_param_count) {
        const Type *param = call->fn->sig_params[index];

        if (param && type_kind(param) == TYPE_STRUCT &&
            (!data || size != type_registry_size_of(call->fn->registry, param))) {
            return false;
        }
    }

    uint8_t *slot = gab_arg_slot(call, index, TYPE_STRUCT);
    if (!slot) {
        return false;
    }

    memcpy(slot, data, size);

    return true;
}

bool gab_arg_pointer(GabCall *call, int index, void *pointer, const GabType *inner) {
    if (call && index >= 0 && (size_t)index < call->fn->sig_param_count) {
        const Type *param = call->fn->sig_params[index];

        if (param && type_is_indirect(param) && type_pointee(param) != (const Type *)inner) {
            return false;
        }
    }

    uint8_t *slot = gab_arg_slot_where(call, index, accepts_indirect, TYPE_UNKNOWN);
    if (!slot) {
        return false;
    }

    memcpy(slot, &pointer, sizeof(pointer));

    return true;
}

GabStatus gab_call(GabVM *handle, GabCall *call, void *ret, GabError *err) {
    gab_error_clear(err);

    if (!handle || !call) {
        gab_error_set(err, 0, 0, "gab_call requires a VM and a call");
        return GAB_ERR_ARG;
    }

    VM *vm = (VM *)handle;
    GabFunc *fn = call->fn;

    if (call->args_pending > 0) {
        for (size_t i = 0; i < fn->sig_param_count; i++) {
            if (call->arg_set[i]) {
                continue;
            }

            char message[256];
            snprintf(message, sizeof(message), "argument %zu was never set", i);
            gab_error_set(err, 0, 0, message);

            return GAB_ERR_ARG;
        }
    }

    bool runs_native = function_runs_native(fn->function);
    size_t func_index = fn->function->func_index;

    assert(func_index != FUNCTION_NO_BODY && "a loaded function has a body");
    assert(func_index < (runs_native ? vm->program.extern_protos.size : vm->program.prototypes.size) &&
           "an installed index names a body in its own table");

    /* A host body may call back in, so this call nests above the frames already running. */
    VmRunStatus saved_status = vm->error.status;

    size_t base = vm_live_stack_end(vm);

    if (!vm_reserve_stack(vm, base / VM_SLOT_SIZE + fn->arg_slots + 1)) {
        gab_error_set(err, 0, 0, "out of stack space");
        return GAB_ERR_RUNTIME;
    }

    memcpy(vm->stack + base + VM_SLOT_SIZE, call->args + VM_SLOT_SIZE, fn->arg_slots * VM_SLOT_SIZE);

    VmRunStatus status = runs_native
                             ? interp_run_extern(vm, &vm->program.extern_protos.data[func_index], base)
                             : interp_run_frame(vm, vm->program.prototypes.data[func_index], base, 0);

    /* The nested run reports its failure through 'err', so the caller it returns into keeps running. */
    if (status != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        vm->error.status = saved_status;

        return GAB_ERR_RUNTIME;
    }

    vm->error.status = saved_status;

    if (ret) {
        memcpy(ret, vm->stack + base, fn->return_size);
    }

    return GAB_OK;
}
