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

// One gab_compile: a chunk to run and a lifetime. Not a namespace — that is
// the 'module' directive's job, and it may span several of these.
struct GabModule {
    CompiledScript script;

    // Diagnostics live on the VM's compile arena, which the next compile
    // reclaims, so anything a unit must outlive a compile with is copied.
    char name[128];

    // The declared module, empty when the unit named none.
    char module[128];
};

struct GabFunc {
    const Symbol *symbol;

    // The signature this handle was built against, captured at lookup. Every
    // cached field below is derived from it, so a reload that changes the
    // signature leaves them describing a call the function no longer accepts.
    // Comparing it at call time is what turns that into an error rather than a
    // frame built to the wrong layout.
    //
    // The arity and parameter types are compared rather than the symbol's
    // generation: a reload that leaves the signature alone must keep working,
    // since calling the new body through the old handle is the whole point.
    size_t sig_param_count;
    const Type *sig_params[VM_MAX_REGISTERS];

    // Slot layout of the call block, mirroring what codegen emits for a call:
    // slot 0 is the return slot and the arguments tile from slot 1.
    unsigned int arg_slots;

    // Where each parameter starts within the call block.
    unsigned int param_slot[VM_MAX_REGISTERS];

    // Arguments are staged here rather than on the live stack: gab_arg_* runs
    // before there is a frame to write into, and an abandoned call then leaves
    // nothing behind.
    Value args[VM_MAX_REGISTERS];

    // Which parameters have ever been given a value. Staged arguments persist
    // across calls on purpose — that is what makes a per-frame call
    // allocation-free, and a host holding one argument constant should not have
    // to re-set it. So this is not "set since the last call": it is "set at
    // all", checked until every parameter has been, after which the call path
    // does no checking whatsoever.
    bool arg_set[VM_MAX_REGISTERS];
    size_t args_pending;

    // Set by a setter that could not do what it was asked. Reported by the
    // next gab_call, so a host can build a call without checking every step.
    bool arg_error;
    char arg_message[256];
};

// A module's functions are looked up through the VM's global scope, so a
// handle is only as valid as the VM. Handles are malloc'd rather than arena
// allocated because gab_module_free must actually release them.

// Builds the handle's cached call layout from its symbol's current signature.
static void gab_func_bind(GabFunc *fn);

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

    if (mod->script.module_name) {
        snprintf(mod->module, sizeof(mod->module), "%s", mod->script.module_name->data);
    }

    return mod;
}

const char *gab_module_name(const GabModule *mod) {
    if (!mod || mod->module[0] == '\0') {
        return NULL;
    }

    return mod->module;
}

GabStatus gab_module_run(GabVM *handle, GabModule *mod, GabError *err) {
    gab_error_clear(err);

    if (!handle || !mod) {
        gab_error_set(err, 0, 0, "gab_module_run requires a VM and a module");
        return GAB_ERR_RUNTIME;
    }

    VM *vm = (VM *)handle;

    if (vm_run(vm, &mod->script) != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        return GAB_ERR_RUNTIME;
    }

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

// The scope a module's names live in. NULL or "" is the root namespace; an
// unknown module name is a miss, reported as NULL rather than falling back to
// the root, so a typo'd module does not silently resolve to a root symbol.
//
// Types and functions both come through here: the namespace is the scope, and
// the only difference downstream is which of the scope's two tables is read.
static Scope *gab_namespace(VM *vm, const char *module) {
    if (!module || module[0] == '\0') {
        return &vm->global_scope;
    }

    String *interned = string_from_cstr(&vm->strings, module);
    Scope **found = interned ? module_scope_map_lookup(vm->module_scopes, interned) : NULL;

    return found ? *found : NULL;
}

const GabType *gab_find_type(GabVM *handle, GabModule *mod, const char *name) {
    return gab_find_type_in(handle, mod ? gab_module_name(mod) : NULL, name);
}

const GabType *gab_find_type_in(GabVM *handle, const char *module, const char *name) {
    if (!handle || !name) {
        return NULL;
    }

    VM *vm = (VM *)handle;

    Scope *scope = gab_namespace(vm, module);
    if (!scope) {
        return NULL;
    }

    // Interning is a lookup, not an insert-if-missing, only because the name
    // of a type that exists is already in the pool.
    String *interned = string_from_cstr(&vm->strings, name);
    if (!interned) {
        return NULL;
    }

    // Walks outward on a miss, so a module sees root-namespace types and
    // builtins — the same visibility a script inside that module has, and the
    // same walk gab_lookup_in does for symbols.
    return (const GabType *)scope_type_lookup(scope, interned);
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
    // The unit says which module to look in. A unit is not itself a namespace —
    // several may share one — so this resolves to the module the unit belongs
    // to, which is what a host asking "the on_update of this script" means.
    return gab_lookup_in(handle, mod ? gab_module_name(mod) : NULL, name, err);
}

GabFunc *gab_lookup_in(GabVM *handle, const char *module, const char *name, GabError *err) {
    gab_error_clear(err);

    if (!handle || !name) {
        gab_error_set(err, 0, 0, "gab_lookup requires a VM and a name");
        return NULL;
    }

    VM *vm = (VM *)handle;

    // A module scope parents to the root, so a bare lookup in a module also
    // finds root declarations — the same visibility a script inside that
    // module has.
    Scope *scope = gab_namespace(vm, module);

    if (!scope) {
        char message[256];
        snprintf(message, sizeof(message), "no module named '%s'", module);
        gab_error_set(err, 0, 0, message);

        return NULL;
    }

    String *interned = string_from_cstr(&vm->strings, name);
    Symbol *symbol = interned ? scope_symbol_lookup(scope, interned) : NULL;

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

    // The handle's per-parameter arrays are all VM_MAX_REGISTERS wide, and a
    // frame cannot address more slots than that anyway. Refused here so the
    // fill loops below cannot run past their arrays.
    if (symbol->func.param_count > VM_MAX_REGISTERS) {
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

    fn->symbol = symbol;

    gab_func_bind(fn);

    return fn;
}

void gab_func_free(GabFunc *fn) { free(fn); }

// The signature this handle was built for, not the symbol's current one: a
// handle describes the call it can make, and after a signature-changing reload
// that is the captured one until the host looks the function up again.
int gab_func_arity(const GabFunc *fn) { return fn ? (int)fn->sig_param_count : 0; }

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
// Whether the function has been recompiled with a different signature since
// this handle was built. Everything the handle caches — the slot layout, the
// staged arguments, the pending count — describes the old signature, so a call
// made through it would build a frame the new body does not expect.
static bool gab_func_is_stale(const GabFunc *fn) {
    if (fn->symbol->func.param_count != fn->sig_param_count) {
        return true;
    }

    for (size_t i = 0; i < fn->sig_param_count; i++) {
        if (fn->symbol->func.params[i] != fn->sig_params[i]) {
            return true;
        }
    }

    return false;
}

// Builds the cached call layout from the symbol's current signature. Shared by
// lookup and by rebinding after a reload, so the two cannot drift.
static void gab_func_bind(GabFunc *fn) {
    const Symbol *symbol = fn->symbol;

    fn->sig_param_count = symbol->func.param_count;

    // Slot 0 of the call block is the return slot, so parameters start at 1 —
    // the same layout codegen_call_expr emits.
    unsigned int offset = 1;

    for (size_t i = 0; i < symbol->func.param_count; i++) {
        fn->sig_params[i] = symbol->func.params[i];
        fn->param_slot[i] = offset;
        offset += gab_type_slots(symbol->func.params[i]);
    }

    fn->arg_slots = offset - 1;

    // The staged arguments described the old signature and mean nothing under
    // the new one: a parameter may have changed type, or been added and never
    // set at all. They are cleared rather than carried over, so the host is
    // made to supply them again instead of a stale value reaching the new body.
    memset(fn->arg_set, 0, sizeof(fn->arg_set));
    memset(fn->args, 0, sizeof(fn->args));
    fn->args_pending = symbol->func.param_count;
}

static Value *gab_arg_slot(GabFunc *fn, int index, TypeKind expected, const char *expected_name) {
    if (!fn) {
        return NULL;
    }

    // Rebound before the slot is read, so a host that reloads and then stages
    // its arguments writes them into the new layout and never sees an error.
    // Whatever it had staged before the reload is cleared by the rebind.
    if (gab_func_is_stale(fn)) {
        gab_func_bind(fn);
    }

    if (index < 0 || (size_t)index >= fn->sig_param_count) {
        gab_arg_fail(fn, "argument %d is out of range for this function%s", index, "");
        return NULL;
    }

    const Type *param = fn->sig_params[index];

    if (!param || param->kind != expected) {
        gab_arg_fail(fn, "argument %d is not declared %s", index, expected_name);
        return NULL;
    }

    // Only a setter that got this far counts: a rejected one leaves the
    // parameter as unset as it was.
    if (!fn->arg_set[index]) {
        fn->arg_set[index] = true;
        fn->args_pending--;
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

    // The size is the one thing a host can get wrong that the type system
    // cannot catch, so it is checked rather than trusted — and before the slot
    // is claimed, so a rejected struct leaves the parameter unset rather than
    // counting as supplied.
    if (fn && index >= 0 && (size_t)index < fn->sig_param_count) {
        const Type *param = fn->sig_params[index];

        if (param && param->kind == TYPE_STRUCT && (!data || size != param->size)) {
            gab_arg_fail(fn, "argument %d was given %s bytes", index, "the wrong number of");
            return;
        }
    }

    Value *slot = gab_arg_slot(fn, index, TYPE_STRUCT, "a struct");
    if (!slot) {
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

    // Before anything reads the cached layout. The handle rebinds itself to the
    // new signature, so the host does not have to look the function up again —
    // but the arguments it had staged described the old one and are gone, so
    // this call does not happen and the next one needs them set afresh.
    if (gab_func_is_stale(fn)) {
        gab_func_bind(fn);

        fn->arg_error = false;

        gab_error_set(err, 0, 0,
                      "this function was recompiled with a different signature; its arguments were cleared "
                      "and must be set again");

        return GAB_ERR_STALE;
    }

    // A setter that failed leaves the frame unbuilt: the call never happens.
    if (fn->arg_error) {
        gab_error_set(err, 0, 0, fn->arg_message);

        fn->arg_error = false;
        fn->arg_message[0] = '\0';

        return GAB_ERR_ARG;
    }

    // An argument never set would otherwise pass whatever the buffer held —
    // zero on the first call, the previous frame's value on every one after.
    // Once every parameter has been supplied this is skipped for good, so the
    // per-frame path stays free.
    if (fn->args_pending > 0) {
        for (size_t i = 0; i < fn->sig_param_count; i++) {
            if (fn->arg_set[i]) {
                continue;
            }

            char message[256];
            snprintf(message, sizeof(message), "argument %zu was never set", i);
            gab_error_set(err, 0, 0, message);

            return GAB_ERR_ARG;
        }
    }

    VM *vm = (VM *)handle;

    size_t proto_index = fn->symbol->func.proto_index;

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

    if (vm_run_frame(vm, proto, base, 0) != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        return GAB_ERR_RUNTIME;
    }

    if (ret) {
        // The return value lands in the frame's slot 0, which is the base of
        // the block, and is as wide as the declared return type.
        memcpy(ret, vm->stack + base, gab_type_size((const GabType *)fn->symbol->func.return_type));
    }

    return GAB_OK;
}
