#include "gab.h"

#include "diagnostics.h"
#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GabVM is the existing VM under another name: the handle is a cast, not a
// wrapper, so there is no second object and no second lifetime to track. The
// tag is only here to give the header something opaque to point at.
struct GabVM;

struct GabModule {
    CompiledScript script;

    // Diagnostics live on the VM's compile arena, which the next compile
    // reclaims, so anything a module must outlive a compile with is copied.
    char name[128];
};

struct GabFunc {
    const Symbol *symbol;

    // Slot layout of the call block, mirroring what codegen emits for a call:
    // slot 0 is the return slot and the arguments tile from slot 1.
    unsigned int arg_slots;

    // Where each parameter starts within the call block.
    unsigned int param_slot[VM_MAX_REGISTERS];

    // Arguments are staged here rather than on the live stack: gab_arg_* runs
    // before there is a frame to write into, and an abandoned call then leaves
    // nothing behind.
    Value args[VM_MAX_REGISTERS];

    // Set by a setter that could not do what it was asked. Reported by the
    // next gab_call, so a host can build a call without checking every step.
    bool arg_error;
    char arg_message[256];
};

// A module's functions are looked up through the VM's global scope, so a
// handle is only as valid as the VM. Handles are malloc'd rather than arena
// allocated because gab_module_free must actually release them.

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

// Copies the first error out of a diagnostics sink. The message is copied
// because the sink's arena is reclaimed by the next compile.
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

void gab_vm_free(GabVM *vm) {
    if (!vm) {
        return;
    }

    vm_free((VM *)vm);
}

GabModule *gab_compile(GabVM *handle, const char *name, const char *src, GabError *err) {
    gab_error_clear(err);

    if (!handle || !src) {
        gab_error_set(err, 0, 0, "gab_compile requires a VM and a source string");
        return NULL;
    }

    VM *vm = (VM *)handle;

    GabModule *mod = calloc(1, sizeof(GabModule));
    if (!mod) {
        gab_error_set(err, 0, 0, "out of memory");
        return NULL;
    }

    snprintf(mod->name, sizeof(mod->name), "%s", name ? name : "<module>");

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, mod->name);

    bool ok = vm_compile(vm, src, &mod->script, &diagnostics);

    if (!ok) {
        // Nothing is printed: a host reports through its own console, and the
        // message is copied out before the sink goes.
        gab_error_from_diagnostics(err, &diagnostics);

        diagnostics_free(&diagnostics);
        free(mod);

        return NULL;
    }

    diagnostics_free(&diagnostics);

    return mod;
}

GabStatus gab_module_run(GabVM *handle, GabModule *mod, GabError *err) {
    gab_error_clear(err);

    if (!handle || !mod) {
        gab_error_set(err, 0, 0, "gab_module_run requires a VM and a module");
        return GAB_ERR_RUNTIME;
    }

    vm_run((VM *)handle, &mod->script);

    return GAB_OK;
}

void gab_module_free(GabVM *handle, GabModule *mod) {
    (void)handle;

    if (!mod) {
        return;
    }

    vm_compiled_script_free(&mod->script);
    free(mod);
}

// --- Types -----------------------------------------------------------------

const GabType *gab_find_type(GabVM *handle, GabModule *mod, const char *name) {
    (void)mod;

    if (!handle || !name) {
        return NULL;
    }

    VM *vm = (VM *)handle;

    // Interning is a lookup, not an insert-if-missing, only because the name
    // of a type that exists is already in the pool.
    String *interned = string_from_cstr(&vm->strings, name);
    if (!interned) {
        return NULL;
    }

    return (const GabType *)type_registry_get(vm->global_scope.type_registry, interned);
}

size_t gab_type_size(const GabType *type) { return type ? ((const Type *)type)->size : 0; }

size_t gab_type_align(const GabType *type) { return type ? ((const Type *)type)->alignment : 0; }

bool gab_field_offset(const GabType *handle, const char *field, size_t *out_offset) {
    if (!handle || !field) {
        return false;
    }

    const Type *type = (const Type *)handle;

    // Field names are interned, but this compares text so that a host can ask
    // about a name the pool has never seen without inserting it.
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *candidate = &type->fields[i];

        if (candidate->name && strcmp(candidate->name->data, field) == 0) {
            if (out_offset) {
                *out_offset = candidate->offset;
            }

            return true;
        }
    }

    return false;
}

// --- Calling ---------------------------------------------------------------

// Slots a value of this type occupies, matching codegen's tiling exactly: the
// two must agree or an argument lands in the wrong register.
static unsigned int gab_type_slots(const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type->size + sizeof(Value) - 1) / sizeof(Value));
}

GabFunc *gab_lookup(GabVM *handle, GabModule *mod, const char *name, GabError *err) {
    (void)mod;

    gab_error_clear(err);

    if (!handle || !name) {
        gab_error_set(err, 0, 0, "gab_lookup requires a VM and a name");
        return NULL;
    }

    VM *vm = (VM *)handle;

    String *interned = string_from_cstr(&vm->strings, name);
    Symbol *symbol = interned ? scope_symbol_lookup(&vm->global_scope, interned) : NULL;

    if (!symbol) {
        char message[256];
        snprintf(message, sizeof(message), "'%s' is not declared", name);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    if (symbol->kind != SYMBOL_FUNC) {
        char message[256];
        snprintf(message, sizeof(message), "'%s' is not a function", name);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    GabFunc *fn = calloc(1, sizeof(GabFunc));
    if (!fn) {
        gab_error_set(err, 0, 0, "out of memory");
        return NULL;
    }

    fn->symbol = symbol;
    // Slot 0 of the call block is the return slot, so parameters start at 1 —
    // the same layout codegen_call_expr emits.
    unsigned int offset = 1;
    for (size_t i = 0; i < symbol->func.param_count; i++) {
        fn->param_slot[i] = offset;
        offset += gab_type_slots(symbol->func.params[i]);
    }

    fn->arg_slots = offset - 1;

    return fn;
}

void gab_func_free(GabFunc *fn) { free(fn); }

int gab_func_arity(const GabFunc *fn) { return fn ? (int)fn->symbol->func.param_count : 0; }

// Records the first thing a setter could not do. Reporting is deferred to
// gab_call so a host can stage a whole call without checking each step.
static void gab_arg_fail(GabFunc *fn, const char *fmt, int index, const char *detail) {
    if (fn->arg_error) {
        return;
    }

    fn->arg_error = true;
    snprintf(fn->arg_message, sizeof(fn->arg_message), fmt, index, detail);
}

// Validates an argument index and that the parameter has the expected kind,
// returning where to write it or NULL if it may not be written.
static Value *gab_arg_slot(GabFunc *fn, int index, TypeKind expected, const char *expected_name) {
    if (!fn) {
        return NULL;
    }

    if (index < 0 || (size_t)index >= fn->symbol->func.param_count) {
        gab_arg_fail(fn, "argument %d is out of range for this function%s", index, "");
        return NULL;
    }

    const Type *param = fn->symbol->func.params[index];

    if (!param || param->kind != expected) {
        gab_arg_fail(fn, "argument %d is not declared %s", index, expected_name);
        return NULL;
    }

    return &fn->args[fn->param_slot[index]];
}

void gab_arg_int(GabVM *vm, GabFunc *fn, int index, int32_t value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_INT, "int");
    if (!slot) {
        return;
    }

    slot->as_int = value;
}

void gab_arg_float(GabVM *vm, GabFunc *fn, int index, float value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_FLOAT, "float");
    if (!slot) {
        return;
    }

    slot->as_float = value;
}

void gab_arg_bool(GabVM *vm, GabFunc *fn, int index, bool value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_BOOL, "bool");
    if (!slot) {
        return;
    }

    slot->as_int = value ? 1 : 0;
}

void gab_arg_struct(GabVM *vm, GabFunc *fn, int index, const void *data, size_t size) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_STRUCT, "a struct");
    if (!slot) {
        return;
    }

    // The size is the one thing a host can get wrong that the type system
    // cannot catch, so it is checked rather than trusted.
    const Type *param = fn->symbol->func.params[index];

    if (!data || size != param->size) {
        gab_arg_fail(fn, "argument %d was given %s bytes", index, "the wrong number of");
        return;
    }

    memcpy(slot, data, size);
}

GabStatus gab_call(GabVM *handle, GabFunc *fn, void *ret, GabError *err) {
    gab_error_clear(err);

    if (!handle || !fn) {
        gab_error_set(err, 0, 0, "gab_call requires a VM and a function");
        return GAB_ERR_ARG;
    }

    // A setter that failed leaves the frame unbuilt: the call never happens.
    if (fn->arg_error) {
        gab_error_set(err, 0, 0, fn->arg_message);

        fn->arg_error = false;
        fn->arg_message[0] = '\0';

        return GAB_ERR_ARG;
    }

    VM *vm = (VM *)handle;

    size_t proto_index = fn->symbol->offset;

    if (proto_index >= vm->global_funcs.size) {
        gab_error_set(err, 0, 0, "this function has no compiled body");
        return GAB_ERR_RUNTIME;
    }

    const FuncPrototype *proto = &vm->global_funcs.data[proto_index];

    if (!proto->chunk) {
        gab_error_set(err, 0, 0, "this function has no compiled body");
        return GAB_ERR_RUNTIME;
    }

    // The call block goes above anything a module run left in frame zero, so a
    // host call never overwrites top-level state it might want to read after.
    size_t base_slot = vm->stack_capacity / 2;
    size_t base = base_slot * sizeof(Value);

    // Frame zero is not on the stack during a host call; the callee's frame is
    // the only one, and it returns into its own slot 0.
    vm->frame_count = 0;
    vm->registers = vm->stack + base;

    // Arguments start at slot 1 of the block, matching where the callee's
    // frame — based here — expects its parameters.
    memcpy(vm->stack + base + sizeof(Value), &fn->args[1], fn->arg_slots * sizeof(Value));

    if (!vm_run_frame(vm, proto, base, 0)) {
        gab_error_set(err, 0, 0, "out of stack space");
        return GAB_ERR_RUNTIME;
    }

    if (ret) {
        // The return value lands in the frame's slot 0, which is the base of
        // the block, and is as wide as the declared return type.
        memcpy(ret, vm->stack + base, gab_type_size((const GabType *)fn->symbol->func.return_type));
    }

    return GAB_OK;
}
