#include "gab.h"

#include "diagnostics.h"
#include "object.h"
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

// A handle is the signature and call layout of one function, shared by every
// caller and never written to outside a reload. What a caller stages for a
// call is per-caller and lives in a GabCall instead.
//
// Sized for the signature it was built against rather than for the largest one
// the VM could express: VM_MAX_FRAME_SLOTS is 255 because that is what an 8-bit
// operand addresses — a bound on a frame's slots, not on a function's
// parameters — so sizing these arrays by it made every handle 4632 bytes to
// hold a signature that is almost always three or four entries. The
// per-parameter arrays therefore trail the struct in one allocation, laid out
// by gab_func_alloc.
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

    // The one allocation sig_params and param_slot point into, kept so a reload
    // that widens the signature can replace it without moving the handle.
    void *params_block;

    // The return type's size, resolved at bind time: fixed by the signature, so
    // the call path does not chase symbol->func->return_type to find it. The
    // prototype cannot be cached the same way — the VM's list of them grows on
    // every compile, so a pointer into it would dangle after a reload.
    size_t return_size;

    // Bumped every time this handle rebinds to a new signature. A GabCall
    // captures it and compares, which is how a caller finds out its staged
    // arguments describe a signature that no longer exists — one integer,
    // rather than walking the parameter types on every setter.
    uint32_t generation;
};

// One caller's arguments for one function. Separate from the handle because the
// handle is shared: two systems calling the same function through one handle
// would otherwise overwrite each other's half-staged arguments, and the call
// path cannot notice — once every parameter has been set it does no checking at
// all.
//
// The staging buffer and the set flags trail the struct in one allocation, as
// they did in the handle, sized for the signature at init.
struct GabCall {
    GabFunc *fn;

    // The signature's generation when this was initialised. A reload that
    // changes the signature bumps the handle's, and the mismatch is what makes
    // the staged arguments an error rather than bytes for the wrong frame.
    uint32_t generation;

    // Arguments are staged here rather than on the live stack: gab_arg_* runs
    // before there is a frame to write into, and an abandoned call then leaves
    // nothing behind. Indexed by slot, and slot 0 is the return slot, so this
    // holds arg_slots + 1 of them.
    uint8_t *args;

    // Which parameters have ever been given a value. Staged arguments persist
    // across calls on purpose — that is what makes a per-frame call
    // allocation-free, and a host holding one argument constant should not have
    // to re-set it. So this is not "set since the last call": it is "set at
    // all", checked until every parameter has been, after which the call path
    // does no checking whatsoever.
    bool *arg_set;
    size_t args_pending;

    // The one allocation args and arg_set point into, and its size. Held
    // separately from the struct so a reload that widens the signature can
    // grow it without moving the call the host is holding.
    void *block;
    size_t capacity;
};

// A handle is only as valid as the VM that produced it, which is why the VM
// owns every one it hands out and frees them all with itself. They are malloc'd
// rather than arena allocated so that a reload widening a signature can grow one
// in place.

// Builds the handle's cached call layout from its symbol's current signature.
static void gab_func_bind(GabFunc *fn);

// The slots a call block needs for this signature: one per parameter's worth of
// slots, plus slot 0 for the return value.
static unsigned int gab_signature_slots(const Symbol *symbol);

// How many slots a value of this type occupies.
static unsigned int gab_type_slots(const Type *type);

// The handle's per-parameter arrays, in one allocation separate from the
// struct. Separate because the host holds the handle's address: a reload that
// widens a signature has to grow these, and the struct itself must not move
// under the pointer gab_lookup handed out.
//
// Laid out widest-aligned first — pointers, then 4-byte — so each lands on its
// natural alignment without padding arithmetic. Sized exactly for the
// signature, since growing is possible rather than something slack must
// pre-empt.
static bool gab_func_alloc_params(GabFunc *fn, size_t param_count) {
    size_t sig_params_at = 0;
    size_t bytes = param_count * sizeof(const Type *);

    size_t param_slot_at = bytes;
    bytes += param_count * sizeof(unsigned int);

    // A zero-arity function has nothing to index, and calloc(1, 0) may return
    // NULL without that being a failure.
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

void gab_vm_free(GabVM *handle) {
    if (!handle) {
        return;
    }

    VM *vm = (VM *)handle;

    // Every handle this VM handed out. Freed here rather than in vm_free
    // because a GabFunc is this file's type: the VM tracked the pointers
    // without ever knowing what they were.
    for (size_t i = 0; i < vm->func_handles.size; i++) {
        GabFunc *fn = vm->func_handles.data[i];

        free(fn->params_block);
        free(fn);
    }

    vm->func_handles.size = 0;

    vm_free(vm);
}

// Where this name's unit is kept, or NULL if it has not been loaded. Linear
// because a host loads a handful of units and looks one up only when loading,
// never per frame.
static LoadedScript *gab_find_script(VM *vm, const char *name) {
    for (size_t i = 0; i < vm->scripts.size; i++) {
        if (strcmp(vm->scripts.data[i].name, name) == 0) {
            return &vm->scripts.data[i];
        }
    }

    return NULL;
}

// --- Extern functions ------------------------------------------------------

// Where a parameter's slots begin, counting from the frame's slot 0 — which
// holds the return value, exactly as it does for a script callee. A multi-slot
// parameter occupies consecutive slots, so each index is found by walking the
// widths ahead of it rather than by indexing a table.
//
// Returns NULL when the index names no parameter, which is what every accessor
// checks before touching a slot.
static uint8_t *gab_arg_address(GabArgs *args, int index, const Type **out_type) {
    if (!args || index < 0) {
        return NULL;
    }

    const Symbol *symbol = args->symbol;

    if ((size_t)index >= symbol->func.param_count) {
        return NULL;
    }

    unsigned int slot = 1;

    for (int i = 0; i < index; i++) {
        slot += gab_type_slots(symbol->func.params[i]);
    }

    if (out_type) {
        *out_type = symbol->func.params[index];
    }

    return args->vm->stack + args->base + (size_t)slot * VM_SLOT_SIZE;
}

int32_t gab_arg_get_int(GabArgs *args, int index) {
    const uint8_t *at = gab_arg_address(args, index, NULL);

    if (!at) {
        return 0;
    }

    int32_t value;
    memcpy(&value, at, sizeof(value));

    return value;
}

float gab_arg_get_float(GabArgs *args, int index) {
    const uint8_t *at = gab_arg_address(args, index, NULL);

    if (!at) {
        return 0.0f;
    }

    float value;
    memcpy(&value, at, sizeof(value));

    return value;
}

bool gab_arg_get_bool(GabArgs *args, int index) {
    const uint8_t *at = gab_arg_address(args, index, NULL);

    if (!at) {
        return false;
    }

    int32_t value;
    memcpy(&value, at, sizeof(value));

    return value != 0;
}

bool gab_arg_get_struct(GabArgs *args, int index, void *out, size_t size) {
    const Type *type = NULL;
    const uint8_t *at = gab_arg_address(args, index, &type);

    if (!at || !out || !type || type->size != size) {
        return false;
    }

    memcpy(out, at, size);

    return true;
}

void *gab_arg_get_pointer(GabArgs *args, int index) {
    const Type *type = NULL;
    const uint8_t *at = gab_arg_address(args, index, &type);

    if (!at || !type || type->kind != TYPE_POINTER) {
        return NULL;
    }

    void *pointer;
    memcpy(&pointer, at, sizeof(pointer));

    return pointer;
}

// The frame's slot 0, which is where a callee leaves its result.
static uint8_t *gab_return_address(GabArgs *args) { return args->vm->stack + args->base; }

void gab_return_int(GabArgs *args, int32_t value) {
    if (args) {
        memcpy(gab_return_address(args), &value, sizeof(value));
    }
}

void gab_return_float(GabArgs *args, float value) {
    if (args) {
        memcpy(gab_return_address(args), &value, sizeof(value));
    }
}

void gab_return_bool(GabArgs *args, bool value) {
    if (args) {
        int32_t widened = value ? 1 : 0;

        memcpy(gab_return_address(args), &widened, sizeof(widened));
    }
}

bool gab_return_struct(GabArgs *args, const void *data, size_t size) {
    if (!args || !data) {
        return false;
    }

    const Type *return_type = args->symbol->func.return_type;

    if (!return_type || return_type->size != size) {
        return false;
    }

    memcpy(gab_return_address(args), data, size);

    return true;
}

void gab_return_pointer(GabArgs *args, void *pointer) {
    if (args) {
        memcpy(gab_return_address(args), &pointer, sizeof(pointer));
    }
}

void gab_error(GabArgs *args, const char *message) {
    if (!args) {
        return;
    }

    snprintf(args->vm->extern_message, sizeof(args->vm->extern_message), "%s",
             message ? message : "the extern function failed");

    args->failed = true;
}

bool gab_extern(GabVM *handle, const char *module, const char *name, GabExternFn fn, GabError *err) {
    gab_error_clear(err);

    if (!handle || !name || !fn) {
        gab_error_set(err, 0, 0, "gab_extern requires a VM, a name, and a function");
        return false;
    }

    VM *vm = (VM *)handle;

    // Interned rather than copied: the binding is matched against a declared
    // name, which is interned in the same pool, so identity is the comparison.
    String *interned_name = string_from_cstr(&vm->strings, name);
    String *interned_module = (module && module[0] != '\0') ? string_from_cstr(&vm->strings, module) : NULL;

    if (!interned_name || (module && module[0] != '\0' && !interned_module)) {
        gab_error_set(err, 0, 0, "out of memory");
        return false;
    }

    for (size_t i = 0; i < vm->externs.size; i++) {
        const ExternBinding *binding = &vm->externs.data[i];

        if (binding->name == interned_name && binding->module == interned_module) {
            gab_error_set(err, 0, 0, "an extern of this name is already bound in this module");
            return false;
        }
    }

    extern_binding_list_add(&vm->externs,
                            (ExternBinding){.module = interned_module, .name = interned_name, .fn = fn});

    return true;
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
    diagnostics_init(&diagnostics, vm->compile_arena, unit_name);

    // Compiled into a local rather than over the previous unit: a failed
    // reload must leave what was loaded before intact and running.
    CompiledScript compiled = {0};
    bool ok = vm_compile(vm, src, &compiled, &diagnostics);

    if (!ok) {
        // Nothing is printed: a host reports through its own console, and the
        // message is copied out before the sink goes.
        gab_error_from_diagnostics(err, &diagnostics);
        diagnostics_free(&diagnostics);

        return false;
    }

    diagnostics_free(&diagnostics);

    // The top level declares the unit's functions and types and initialises
    // whatever it sets up, so a load that did not run it would leave the unit
    // half present.
    if (vm_run(vm, &compiled) != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        vm_compiled_script_free(&compiled);

        return false;
    }

    // Only now that it has compiled and run does it replace the previous unit
    // of this name. The old chunk is dead: its declarations have already been
    // superseded by generation, and nothing holds a pointer into it.
    LoadedScript *existing = gab_find_script(vm, unit_name);

    if (existing) {
        vm_compiled_script_free(&existing->script);
        existing->script = compiled;

        return true;
    }

    LoadedScript loaded = {.script = compiled};
    snprintf(loaded.name, sizeof(loaded.name), "%s", unit_name);

    loaded_script_list_add(&vm->scripts, loaded);

    return true;
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

// --- Heap objects ----------------------------------------------------------

// The VM is taken but unused: an allocator is global today, and a host that
// installs its own later would reach it through the VM rather than through a
// changed signature.
void *gab_new(GabVM *handle, const GabType *type) {
    (void)handle;

    if (!type) {
        return NULL;
    }

    return gab_object_alloc(DEFAULT_ALLOCATOR, (const Type *)type);
}

void gab_free(GabVM *handle, void *object) {
    (void)handle;

    gab_object_free(DEFAULT_ALLOCATOR, object);
}

// --- Calling ---------------------------------------------------------------

// Slots a value of this type occupies, matching codegen's tiling exactly: the
// two must agree or an argument lands in the wrong register.
static unsigned int gab_type_slots(const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
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

    // A parameter list cannot outgrow the slots a frame addresses. Refused here
    // so the fill loops below cannot run past their arrays.
    if (symbol->func.param_count > VM_MAX_FRAME_SLOTS) {
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

    if (!gab_func_alloc_params(fn, symbol->func.param_count)) {
        free(fn);
        gab_error_set(err, 0, 0, "out of memory");

        return NULL;
    }

    fn->symbol = symbol;

    gab_func_bind(fn);

    // The VM owns it from here, for good: a handle cannot be released early.
    // A host looks a function up once and calls it for the life of the VM, so
    // the handles a program accumulates are bounded by the functions it calls.
    func_handle_list_add(&vm->func_handles, fn);

    return fn;
}

// The signature this handle currently describes. A reload rebinds the handle,
// so this follows the new signature; a call staged against the old one is what
// reports GAB_ERR_STALE.
int gab_func_arity(const GabFunc *fn) { return fn ? (int)fn->sig_param_count : 0; }

// Whether the function has been recompiled with a different signature since
// this handle was bound. The handle's slot layout describes the old signature,
// so a call made through it would build a frame the new body does not expect.
//
// The arity and parameter types are compared rather than the symbol's
// generation: a reload that leaves the signature alone must keep working, since
// calling the new body through the old handle is the whole point.
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
// The caller guarantees the trailing arrays hold the new parameter count.
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
    fn->return_size = symbol->func.return_type ? symbol->func.return_type->size : 0;
}

// Rebinds a stale handle to its symbol's current signature, growing its arrays
// first if the new signature has more parameters than the old.
//
// The handle itself never moves, because the host holds its address. Only the
// trailing arrays are reallocated, so every GabFunc a host looked up stays
// valid across any reload — a caller's GabCall notices through the generation
// and restages, which is what the old design could not do without making the
// host look the function up again.
static bool gab_func_rebind(GabFunc *fn) {
    if (fn->symbol->func.param_count > fn->sig_param_count) {
        if (!gab_func_alloc_params(fn, fn->symbol->func.param_count)) {
            return false;
        }
    }

    gab_func_bind(fn);
    fn->generation++;

    return true;
}

// Sizes a call's staging buffer for its function's current signature and clears
// whatever was in it. Shared by init and by restaging after a reload, so the two
// cannot drift.
static bool gab_call_stage(GabCall *call, GabError *err) {
    const GabFunc *fn = call->fn;

    // Slot 0 is the return slot, so the buffer holds one more than the
    // arguments do.
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

    // Laid out afresh either way: a signature that changed shape moves the set
    // flags even when the total still fits what was already allocated.
    memset(call->block, 0, call->capacity);

    call->args = (uint8_t *)call->block;
    call->arg_set = (bool *)((char *)call->block + slots * VM_SLOT_SIZE);

    call->generation = fn->generation;
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

// What a host does after GAB_ERR_STALE: the handle has already rebound itself,
// so the function is the same and only this caller's buffer is behind. Restaging
// through the same GabCall keeps whatever the host stored it in — a member of an
// entity system, a slot in an array — pointing at a live call, which is why this
// exists rather than making the host free and initialise a new one.
//
// Every argument is cleared, because they described the old signature: a
// parameter may have changed type, or been added and never set at all.
//
// Returns false only if memory ran out, and leaves the call as it was: still
// stale, still refusing, still the host's to free.
bool gab_call_restage(GabCall *call, GabError *err) {
    gab_error_clear(err);

    if (!call) {
        gab_error_set(err, 0, 0, "gab_call_restage requires a call");
        return false;
    }

    return gab_call_stage(call, err);
}

void gab_call_free(GabCall *call) {
    if (!call) {
        return;
    }

    free(call->block);
    free(call);
}

// Validates an argument index and that the parameter has the expected kind,
// returning where to write it or NULL if it may not be written.
static uint8_t *gab_arg_slot(GabCall *call, int index, TypeKind expected) {
    if (!call) {
        return NULL;
    }

    const GabFunc *fn = call->fn;

    // Staged against a signature that no longer exists. The buffer is sized for
    // the old one, so writing into it would overflow as readily as it would
    // mislead; the host initialises the call again to stage afresh.
    if (call->generation != fn->generation) {
        return NULL;
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
    if (!call->arg_set[index]) {
        call->arg_set[index] = true;
        call->args_pending--;
    }

    return call->args + (size_t)fn->param_slot[index] * VM_SLOT_SIZE;
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
    // The size is the one thing a host can get wrong that the type system
    // cannot catch, so it is checked rather than trusted — and before the slot
    // is claimed, so a rejected struct leaves the parameter unset rather than
    // counting as supplied.
    if (call && index >= 0 && (size_t)index < call->fn->sig_param_count) {
        const Type *param = call->fn->sig_params[index];

        if (param && param->kind == TYPE_STRUCT && (!data || size != param->size)) {
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

bool gab_arg_pointer(GabCall *call, int index, void *pointer, const GabType *pointee) {
    // The pointee is checked before the slot is claimed, so a rejected pointer
    // leaves the parameter unset rather than counting as supplied. Pointer types
    // are interned, so this is a pointer compare rather than a structural one.
    if (call && index >= 0 && (size_t)index < call->fn->sig_param_count) {
        const Type *param = call->fn->sig_params[index];

        if (param && param->kind == TYPE_POINTER && param->pointee != (const Type *)pointee) {
            return false;
        }
    }

    uint8_t *slot = gab_arg_slot(call, index, TYPE_POINTER);
    if (!slot) {
        return false;
    }

    // A pointer tiles over two slots, which is the only reason this moves more
    // than the others do.
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

    // Before anything reads the cached layout. The handle rebinds to the new
    // signature — every other caller's next call reaches the new body without
    // looking anything up — but this call's staged arguments described the old
    // one, so it does not happen and the host initialises the call again.
    if (gab_func_is_stale(fn)) {
        if (!gab_func_rebind(fn)) {
            gab_error_set(err, 0, 0, "out of memory");
            return GAB_ERR_RUNTIME;
        }

        gab_error_set(err, 0, 0,
                      "this function was recompiled with a different signature; initialise the call again "
                      "to stage its arguments afresh");

        return GAB_ERR_STALE;
    }

    // Staged before some other caller rebound the handle. Same cause, but the
    // rebind already happened, so there is nothing to do but report it.
    if (call->generation != fn->generation) {
        gab_error_set(err, 0, 0,
                      "this function was recompiled with a different signature; initialise the call again "
                      "to stage its arguments afresh");

        return GAB_ERR_STALE;
    }

    // An argument never set would otherwise pass whatever the buffer held —
    // zero on the first call, the previous frame's value on every one after.
    // Once every parameter has been supplied this is skipped for good, so the
    // per-frame path stays free.
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

    size_t proto_index = fn->symbol->func.proto_index;

    if (proto_index >= vm->global_funcs.size) {
        gab_error_set(err, 0, 0, "this function has no compiled body");
        return GAB_ERR_RUNTIME;
    }

    const FuncPrototype *proto = &vm->global_funcs.data[proto_index];

    // An extern is bound rather than compiled, so a missing body means a
    // missing chunk for one and a missing binding for the other.
    if (!proto->chunk && !proto->native) {
        gab_error_set(err, 0, 0, "this function has no compiled body");
        return GAB_ERR_RUNTIME;
    }

    // The call block goes above anything a module run left in frame zero, so a
    // host call never overwrites top-level state it might want to read after.
    size_t base_slot = vm->stack_capacity / 2;
    size_t base = base_slot * VM_SLOT_SIZE;

    // Frame zero is not on the stack during a host call; the callee's frame is
    // the only one, and it returns into its own slot 0.
    vm->frame_count = 0;
    vm->registers = vm->stack + base;

    // Arguments start at slot 1 of the block, matching where the callee's
    // frame — based here — expects its parameters.
    memcpy(vm->stack + base + VM_SLOT_SIZE, call->args + VM_SLOT_SIZE, fn->arg_slots * VM_SLOT_SIZE);

    VmRunStatus status = proto->native ? vm_run_extern(vm, proto, base) : vm_run_frame(vm, proto, base, 0);

    if (status != VM_RUN_OK) {
        gab_error_set(err, 0, 0, vm->error.message);
        return GAB_ERR_RUNTIME;
    }

    if (ret) {
        // The return value lands in the frame's slot 0, which is the base of
        // the block, and is as wide as the declared return type.
        memcpy(ret, vm->stack + base, fn->return_size);
    }

    return GAB_OK;
}
