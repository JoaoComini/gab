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

// One gab_compile: a top-level chunk to run, and nothing else. Deliberately not a
// namespace — that is the 'module' directive's job, and a host names a module
// directly when it looks something up. Several units may declare one module,
// so a unit could not stand in for one anyway.

struct GabScript {
    CompiledScript script;

    // Diagnostics live on the VM's compile arena, which the next compile
    // reclaims, so anything a unit must outlive a compile with is copied.
    char name[128];
};

// Slack in a handle's arrays, so a reload that adds a parameter or widens a
// struct rebinds in place instead of making the host look the function up
// again. Small: the point is to absorb an edit, not to preallocate for 255.
#define GAB_FUNC_SPARE_PARAMS 4
#define GAB_FUNC_SPARE_SLOTS 8

// A handle is sized for the signature it was built against rather than for the
// largest one the VM could express. VM_MAX_REGISTERS is 255 because that is
// what an 8-bit operand addresses — a bound on a frame's slots, not on a
// function's parameters — so sizing these arrays by it made every handle 4632
// bytes to hold a signature that is almost always three or four entries.
//
// The per-parameter arrays and the argument staging buffer therefore trail the
// struct in one allocation, laid out by gab_func_alloc.
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
    const Type **sig_params;

    // Slot layout of the call block, mirroring what codegen emits for a call:
    // slot 0 is the return slot and the arguments tile from slot 1.
    unsigned int arg_slots;

    // Where each parameter starts within the call block.
    unsigned int *param_slot;

    // Arguments are staged here rather than on the live stack: gab_arg_* runs
    // before there is a frame to write into, and an abandoned call then leaves
    // nothing behind. Indexed by slot, and slot 0 is the return slot, so this
    // holds arg_slots + 1 of them.
    Value *args;

    // Which parameters have ever been given a value. Staged arguments persist
    // across calls on purpose — that is what makes a per-frame call
    // allocation-free, and a host holding one argument constant should not have
    // to re-set it. So this is not "set since the last call": it is "set at
    // all", checked until every parameter has been, after which the call path
    // does no checking whatsoever.
    bool *arg_set;
    size_t args_pending;

    // What the trailing arrays were sized for. A reload may widen the
    // signature past them, which is when the handle has to be reallocated
    // rather than rebound in place.
    size_t param_capacity;
    unsigned int slot_capacity;

};

// A function is looked up through the VM.s scopes, so a handle is only as valid
// as the VM. Handles are malloc.d rather than arena allocated so that
// gab_func_free can actually release one.

// Builds the handle's cached call layout from its symbol's current signature.
static void gab_func_bind(GabFunc *fn);

// The slots a call block needs for this signature: one per parameter's worth of
// slots, plus slot 0 for the return value.
static unsigned int gab_signature_slots(const Symbol *symbol);

// One allocation holding the handle and its four variable-length arrays. They
// are laid out widest-aligned first — pointers, then 4-byte, then bytes — so
// each lands on its natural alignment without padding arithmetic.
static GabFunc *gab_func_alloc(size_t param_count, unsigned int slot_count) {
    size_t bytes = sizeof(GabFunc);

    size_t sig_params_at = bytes;
    bytes += param_count * sizeof(const Type *);

    size_t args_at = bytes;
    bytes += (size_t)slot_count * sizeof(Value);

    size_t param_slot_at = bytes;
    bytes += param_count * sizeof(unsigned int);

    size_t arg_set_at = bytes;
    bytes += param_count * sizeof(bool);

    GabFunc *fn = calloc(1, bytes);
    if (!fn) {
        return NULL;
    }

    char *base = (char *)fn;

    // Zero-length arrays are legal here only because nothing indexes them when
    // param_count is 0; the pointers are never dereferenced.
    fn->sig_params = (const Type **)(base + sig_params_at);
    fn->args = (Value *)(base + args_at);
    fn->param_slot = (unsigned int *)(base + param_slot_at);
    fn->arg_set = (bool *)(base + arg_set_at);

    fn->param_capacity = param_count;
    fn->slot_capacity = slot_count;

    return fn;
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

GabScript *gab_compile(GabVM *handle, const char *name, const char *src, GabError *err) {
    gab_error_clear(err);

    if (!handle || !src) {
        gab_error_set(err, 0, 0, "gab_compile requires a VM and a source string");
        return NULL;
    }

    VM *vm = (VM *)handle;

    GabScript *script = calloc(1, sizeof(GabScript));
    if (!script) {
        gab_error_set(err, 0, 0, "out of memory");
        return NULL;
    }

    snprintf(script->name, sizeof(script->name), "%s", name ? name : "<script>");

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, script->name);

    bool ok = vm_compile(vm, src, &script->script, &diagnostics);

    if (!ok) {
        // Nothing is printed: a host reports through its own console, and the
        // message is copied out before the sink goes.
        gab_error_from_diagnostics(err, &diagnostics);

        diagnostics_free(&diagnostics);
        free(script);

        return NULL;
    }

    diagnostics_free(&diagnostics);

    return script;
}

GabStatus gab_run(GabVM *handle, GabScript *script, GabError *err) {
    gab_error_clear(err);

    if (!handle || !script) {
        gab_error_set(err, 0, 0, "gab_run requires a VM and a script");
        return GAB_ERR_RUNTIME;
    }

    VM *vm = (VM *)handle;

    if (vm_run(vm, &script->script) != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        return GAB_ERR_RUNTIME;
    }

    return GAB_OK;
}

void gab_script_free(GabVM *handle, GabScript *script) {
    (void)handle;

    if (!script) {
        return;
    }

    vm_compiled_script_free(&script->script);
    free(script);
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

const GabType *gab_find_type(GabVM *handle, const char *module, const char *name) {
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
    // same walk gab_lookup does for symbols.
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

GabFunc *gab_lookup(GabVM *handle, const char *module, const char *name, GabError *err) {
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

    // Sized for this signature with a little room, so the common reload — a
    // parameter added or a struct widened — is absorbed in place rather than
    // forcing the host to look the function up again.
    size_t param_capacity = symbol->func.param_count + GAB_FUNC_SPARE_PARAMS;
    unsigned int slot_capacity = gab_signature_slots(symbol) + GAB_FUNC_SPARE_SLOTS;

    if (param_capacity > VM_MAX_REGISTERS) {
        param_capacity = VM_MAX_REGISTERS;
    }

    if (slot_capacity > VM_MAX_REGISTERS) {
        slot_capacity = VM_MAX_REGISTERS;
    }

    GabFunc *fn = gab_func_alloc(param_capacity, slot_capacity);
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

// Whether the symbol's current signature still fits the arrays this handle was
// allocated with. A reload that only changes parameter types, or narrows the
// signature, fits; one that adds parameters or widens a struct may not.
//
// The handle cannot grow: the host owns the pointer, so reallocating would
// leave it holding a dangling one. A signature that outgrew the handle is
// therefore the one case a host must re-look-up to follow.
static bool gab_func_fits(const GabFunc *fn, const Symbol *symbol) {
    return symbol->func.param_count <= fn->param_capacity &&
           gab_signature_slots(symbol) <= fn->slot_capacity;
}

static unsigned int gab_signature_slots(const Symbol *symbol) {
    unsigned int slots = 1;

    for (size_t i = 0; i < symbol->func.param_count; i++) {
        slots += gab_type_slots(symbol->func.params[i]);
    }

    return slots;
}

// Builds the cached call layout from the symbol's current signature. Shared by
// lookup and by rebinding after a reload, so the two cannot drift.
//
// The caller guarantees the trailing arrays are wide enough: at lookup they are
// sized for this signature, and on a reload gab_func_rebind reallocates first if
// the new signature outgrew them.
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
    //
    // Cleared to capacity, not to the new count: a signature that shrank leaves
    // entries above it that a later widening would otherwise resurrect.
    memset(fn->arg_set, 0, fn->param_capacity * sizeof(bool));
    memset(fn->args, 0, (size_t)fn->slot_capacity * sizeof(Value));
    fn->args_pending = symbol->func.param_count;
}

static Value *gab_arg_slot(GabFunc *fn, int index, TypeKind expected) {
    if (!fn) {
        return NULL;
    }

    // Rebound before the slot is read, so a host that reloads and then stages
    // its arguments writes them into the new layout and never sees an error.
    // Whatever it had staged before the reload is cleared by the rebind.
    //
    // A signature too wide for this handle's arrays cannot be rebound; the
    // setter fails and gab_call reports it, which is the host's cue to look the
    // function up again.
    if (gab_func_is_stale(fn)) {
        if (!gab_func_fits(fn, fn->symbol)) {
            return NULL;
        }

        gab_func_bind(fn);
    }

    if (index < 0 || (size_t)index >= fn->sig_param_count) {
        return NULL;
    }

    const Type *param = fn->sig_params[index];

    if (!param || param->kind != expected) {
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

bool gab_arg_int(GabVM *vm, GabFunc *fn, int index, int32_t value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_INT);
    if (!slot) {
        return false;
    }

    slot->as_int = value;

    return true;
}

bool gab_arg_float(GabVM *vm, GabFunc *fn, int index, float value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_FLOAT);
    if (!slot) {
        return false;
    }

    slot->as_float = value;

    return true;
}

bool gab_arg_bool(GabVM *vm, GabFunc *fn, int index, bool value) {
    (void)vm;

    Value *slot = gab_arg_slot(fn, index, TYPE_BOOL);
    if (!slot) {
        return false;
    }

    slot->as_int = value ? 1 : 0;

    return true;
}

bool gab_arg_struct(GabVM *vm, GabFunc *fn, int index, const void *data, size_t size) {
    (void)vm;

    // The size is the one thing a host can get wrong that the type system
    // cannot catch, so it is checked rather than trusted — and before the slot
    // is claimed, so a rejected struct leaves the parameter unset rather than
    // counting as supplied.
    if (fn && index >= 0 && (size_t)index < fn->sig_param_count) {
        const Type *param = fn->sig_params[index];

        if (param && param->kind == TYPE_STRUCT && (!data || size != param->size)) {
            return false;
        }
    }

    Value *slot = gab_arg_slot(fn, index, TYPE_STRUCT);
    if (!slot) {
        return false;
    }

    memcpy(slot, data, size);

    return true;
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
        // Too wide to rebind in place: the host owns this pointer, so the
        // arrays cannot grow under it and a fresh lookup is the only way
        // forward.
        if (!gab_func_fits(fn, fn->symbol)) {
            gab_error_set(err, 0, 0,
                          "this function was recompiled with a wider signature than this handle can hold; "
                          "look it up again");

            return GAB_ERR_STALE;
        }

        gab_func_bind(fn);

        gab_error_set(err, 0, 0,
                      "this function was recompiled with a different signature; its arguments were cleared "
                      "and must be set again");

        return GAB_ERR_STALE;
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
