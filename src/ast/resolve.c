#include "ast/resolve.h"

#include "ast/facts.h"

#include "function_registry.h"

#include "decl.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* What a file may name: the modules it imports, and the core, which every file reaches without
 * stating it. One per file, so a name one file reaches is not a name its siblings do. */
typedef struct {
    Module **modules;
    size_t count;
} Visible;

typedef struct StructDecl {
    ASTStmt *stmt;

    /* Where the type is bound, which is the module's however many files write it. */
    Scope *scope;

    /* Where its fields resolve from, which is the file that wrote it: a field's type may name what
     * that file imports, and fields resolve after every file has declared. */
    Scope *file_scope;

    /* Fields resolve after every file has declared, so the struct carries the file whose imports its
     * field types may name, and what that file may name. */
    const ASTFile *file;

    const Visible *visible;

    String *name;

    TypeDecl *decl;

    bool fields_demanded;

    bool poisoned;
} StructDecl;

GAB_LIST(StructDeclList, struct_decl_list, StructDecl *)

typedef struct {
    const Type *return_type;

    /* Whether the body being resolved was declared 'caller', which is what '@caller()' needs. */
    bool is_caller;

    unsigned int loop_depth;
} FuncContext;

typedef struct ResolverState {
    Arena *compile_arena;

    /* One compilation's, shared by every module in it: a scope names things, and does not own them. */
    TypeRegistry *types;
    FunctionRegistry *functions;
    StringPool *strings;
    Scope *current_scope;

    /* The file being resolved, whose imports are the ones its statements may name. */
    const ASTFile *file;

    /* The modules that file may name, which is what an unqualified name reaches past this module. */
    const Visible *visible;

    /* Where this module's declarations land, which every file of it declares into. */
    Scope *module_scope;

    /* Where a declaration being resolved belongs, which is the module while a top-level statement is
     * read: a scope binding type parameters stands between it and the file, and binds none of them. */
    Scope *declaring;

    String *module_name;

    bool declares_intrinsics;

    FuncContext func_context;

    StructDeclList struct_decls;

    /* What resolving concluded about each node, and the bodies generation will lower. */
    Facts *facts;
    PendingBodies *work;

    StructDeclList resolving;

    /* The bound on each type parameter of the declaration being resolved, by index. */
    TypeParamBound param_bounds[GAB_MAX_TYPE_PARAMS];

    Diagnostics *diagnostics;
} ResolverState;

static const Type *resolver_error_type(ResolverState *state) {
    return type_registry_error_type(state->types);
}

static String *resolver_intern(ResolverState *state, StringRef ref) {
    return string_from_ref(state->strings, ref);
}

static const KnownNames *resolver_names(ResolverState *state) { return type_registry_names(state->types); }

static bool names_the_same(ResolverState *state, StringRef ref, const String *known) {
    return resolver_intern(state, ref) == known;
}

/* 'Self' names the type an impl block is for, so nothing else may take the name. */
static bool reject_self_as_name(ResolverState *state, String *name, Span span) {
    if (name != resolver_names(state)->self) {
        return false;
    }

    diag_error(state->diagnostics, GAB_ERR_NAME, span,
               "'Self' names the type an 'impl' block is for, so it cannot be declared");

    return true;
}

static bool string_ref_split_colons(StringRef ref, StringRef *module, StringRef *member) {
    for (size_t i = 0; i + 1 < ref.length; i++) {
        if (ref.data[i] != ':' || ref.data[i + 1] != ':') {
            continue;
        }

        *module = (StringRef){.data = ref.data, .length = i};
        *member = (StringRef){.data = ref.data + i + 2, .length = ref.length - i - 2};

        return true;
    }

    return false;
}

/* What the syntax means for a symbol, which needs the body the syntax does not mention. */
static Linkage linkage_of(const ASTFuncDecl *decl) {
    if (decl->syntax & FUNC_SYN_FOREIGN) {
        return LINKAGE_C;
    }

    /* An intrinsic is expanded rather than called, so it names no symbol and links to nothing. */
    if (decl->syntax & FUNC_SYN_INTRINSIC) {
        return LINKAGE_INTERNAL;
    }

    return decl->body ? LINKAGE_INTERNAL : LINKAGE_GAB;
}

/* The syntax that survives resolution, which the rest of the compiler reads instead of the tokens. */
/* Where a declaration belongs: what a file declares is the module's, however many files write it, so
 * only what a file imports stays with the file. */
static Scope *resolver_declaring_scope(ResolverState *state) {
    return state->declaring ? state->declaring : state->current_scope;
}

static const Type *resolver_location_type(ResolverState *state);

static const Type *location_type_of(ResolverState *state, const ASTFuncDecl *decl) {
    if (!(decl->syntax & FUNC_SYN_CALLER)) {
        return NULL;
    }

    return resolver_location_type(state);
}

static unsigned modifiers_of(const ASTFuncDecl *decl) {
    return (unsigned)((decl->syntax & FUNC_SYN_INTRINSIC) ? FUNC_MOD_INTRINSIC : FUNC_MOD_NONE) |
           (unsigned)((decl->syntax & FUNC_SYN_CALLER) ? FUNC_MOD_CALLER : FUNC_MOD_NONE);
}

/* What a name qualified by a module resolves through: an import binds the module in this file, so
 * naming it is the lookup any other name is, and one this file did not import is bound nowhere. */
static Scope *resolver_expr_scope(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (!string_ref_split_colons(name, &module, &member)) {
        return state->current_scope;
    }

    String *module_name = string_from_ref(state->strings, module);

    if (module_name == state->module_name) {
        return state->module_scope;
    }

    Symbol *bound = scope_lookup(state->current_scope, module_name);

    return bound && bound->kind == SYMBOL_MODULE ? bound->module->scope : NULL;
}

/* A name a module this file may name declares, which an unqualified use reaches once this unit
 * declares none itself. The core is last, so a name the file imports is the one it means. */
static Symbol *file_lookup(const ResolverState *state, String *name) {
    if (!state->visible) {
        return NULL;
    }

    for (size_t i = 0; i < state->visible->count; i++) {
        Symbol *found = scope_lookup(state->visible->modules[i]->scope, name);

        if (found) {
            return found;
        }
    }

    return NULL;
}

static Symbol *resolver_resolve_name(ResolverState *state, Scope *scope, String *name) {
    Symbol *found = scope ? scope_lookup(scope, name) : NULL;

    /* A name qualified by a module is that module's, so what this file imports does not answer it. */
    if (found) {
        return found;
    }

    return file_lookup(state, name);
}

/* What '@caller()' answers with, which the prelude declares and every file reaches by importing it. */
static const Type *resolver_location_type(ResolverState *state) {
    String *name = string_from_cstr(state->strings, GAB_LOCATION_TYPE);

    Symbol *found = resolver_resolve_name(state, state->current_scope, name);

    return found ? symbol_type(state->types, found) : NULL;
}

/* The interface a name denotes, or null where it denotes something else or nothing. */
static InterfaceDecl *interface_of(const Symbol *symbol) {
    return symbol && symbol->kind == SYMBOL_INTERFACE ? symbol->interface : NULL;
}

static InterfaceDecl *resolver_lookup_interface(ResolverState *state, String *name) {
    InterfaceDecl *found = interface_of(scope_lookup(state->current_scope, name));

    if (found) {
        return found;
    }

    return interface_of(file_lookup(state, name));
}

static String *resolver_expr_member(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (string_ref_split_colons(name, &module, &member)) {
        return string_from_ref(state->strings, member);
    }

    return resolver_intern(state, name);
}

static bool is_error_type(const Type *type) { return !type || type_kind(type) == TYPE_ERROR; }

static const char *type_name(ResolverState *state, const Type *type) {
    if (!type) {
        return "none";
    }

    if (type_kind(type) == TYPE_ARRAY) {
        const char *element = type_name(state, type_array_element(type));
        size_t length = strlen(element) + 32;
        char *out = arena_alloc(state->compile_arena, length);

        if (type_array_length_is_known(type)) {
            snprintf(out, length, "array<%s, %d>", element, type_array_length(type));
        } else {
            snprintf(out, length, "array<%s, _>", element);
        }

        return out;
    }

    if (type_kind(type) == TYPE_SLICE) {
        const char *element = type_name(state, type_slice_element(type));
        size_t length = strlen(element) + 16;
        char *out = arena_alloc(state->compile_arena, length);

        snprintf(out, length, "slice<%s>", element);

        return out;
    }

    if (type_kind(type) == TYPE_RAW) {
        const char *element = type_name(state, type_pointee(type));
        size_t length = strlen(element) + 16;
        char *out = arena_alloc(state->compile_arena, length);

        snprintf(out, length, "raw<%s>", element);

        return out;
    }

    if (type_name_of(type)) {
        return type_name_of(type)->data;
    }

    /* A parameter interns by index and carries no name, so what a declaration called it is not here. */
    if (type_kind(type) == TYPE_PARAM) {
        return "a type parameter";
    }

    const char *inner = type_name(state, type_pointee(type));
    const char *prefix = type_kind(type) == TYPE_REF ? "&" : "*";
    size_t length = strlen(prefix) + strlen(inner) + 1;
    char *out = arena_alloc(state->compile_arena, length);

    snprintf(out, length, "%s%s", prefix, inner);

    return out;
}

static void resolver_enter_scope(ResolverState *state) {
    state->current_scope = scope_create(state->compile_arena, state->current_scope);

    /* Inside a body a declaration is a local, so it belongs where it is written. */
    state->declaring = NULL;
}

static void resolver_exit_scope(ResolverState *state) { state->current_scope = state->current_scope->parent; }

static const char *bin_op_name(BinOp op) {
    switch (op) {
    case BIN_OP_ADD:
        return "+";
    case BIN_OP_SUB:
        return "-";
    case BIN_OP_MUL:
        return "*";
    case BIN_OP_DIV:
        return "/";
    case BIN_OP_MOD:
        return "%";
    case BIN_OP_LESS:
        return "<";
    case BIN_OP_GREATER:
        return ">";
    case BIN_OP_EQUAL:
        return "==";
    case BIN_OP_NEQUAL:
        return "!=";
    case BIN_OP_LEQUAL:
        return "<=";
    case BIN_OP_GEQUAL:
        return ">=";
    case BIN_OP_AND:
        return "&&";
    case BIN_OP_OR:
        return "||";
    }

    return "?";
}

static bool is_addressable(ResolverState *state, const ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE:
        return fact_use_of(state->facts, expr) && fact_use_of(state->facts, expr)->kind == SYMBOL_VAR;
    case EXPR_FIELD:
        return is_addressable(state, expr->field.target);
    case EXPR_INDEX:

        return is_addressable(state, expr->index.target);
    case EXPR_DEREF:
        return true;
    default:
        return false;
    }
}

static const Type *receiver_base_type(const Type *type) {
    while (type_is_indirect(type)) {
        type = type_pointee(type);
    }

    return type;
}

static const Type *derefs_to(TypeRegistry *registry, const Type *type);

static Function *find_method_on_chain(TypeRegistry *registry, FunctionRegistry *functions, const Type *type,
                                      const String *name, const Type **out_base) {
    for (const Type *at = receiver_base_type(type); at; at = derefs_to(registry, at)) {
        Function *found = function_registry_owned_for(functions, at, name);

        if (found) {
            *out_base = at;
            return found;
        }
    }

    *out_base = receiver_base_type(type);
    return NULL;
}

static bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from);
static bool accepts_by_borrowing(const Type *to, const Type *from);
static bool reads_as_a_view(TypeRegistry *registry, const Type *to, const Type *from);
static bool lends_by_pointer(const Type *to, const Type *from);
static bool borrow_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span);
static void adjust_derefs(ResolverState *state, Adjustment *adjustment, const Type *from, unsigned int count);
static void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span);

/* The arguments the source wrote, which an indexing spells as the one between its brackets. A
 * receiver is not among them, being held beside the call rather than written as an argument. */
static size_t written_arg_count(const ASTExpr *expr) {
    return expr->kind == EXPR_INDEX ? 1 : expr->call.args.size;
}

static ASTExpr *written_arg(const ASTExpr *expr, size_t i) {
    return expr->kind == EXPR_INDEX ? expr->index.index : expr->call.args.data[i];
}

/* 'written' is what the source spells; a method's receiver is checked apart from them, so it numbers
 * the arguments the way they were written rather than the way they are passed. */
static void check_call_arg(ResolverState *state, ASTExpr *arg, const Type *param_type, size_t written) {
    if (is_error_type(fact_type_of(state->facts, arg)) || is_error_type(param_type)) {
        return;
    }

    if (!type_accepts(state->types, param_type, fact_adjusted_type_of(state->facts, arg))) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, arg->span, "argument %zu is %s, but %s was declared",
                   written, type_name(state, fact_adjusted_type_of(state->facts, arg)),
                   type_name(state, param_type));
        return;
    }

    if (!borrow_into(state, arg, param_type, arg->span)) {
        return;
    }

    mark_implicit_move(state, arg, param_type, arg->span);
}

static void check_call_args(ResolverState *state, ASTExprList *args, const Type **params) {
    for (size_t i = 0; i < args->size; i++) {
        check_call_arg(state, args->data[i], params[i], i + 1);
    }
}

static bool unsizes_to_a_slice(const Type *to, const Type *from);

typedef struct {
    size_t derefs;

    bool address_of;

    /* The array the receiver names is handed over as a slice of it, address and length together. */
    int32_t unsize_length;
} ReceiverAdjustment;

/* How the receiver reaches the type the method declares, which lowering applies where it emits it. */
static void record_receiver_adjustment(ResolverState *state, ASTExpr *receiver, const Function *method,
                                       ReceiverAdjustment adjustment) {
    if (adjustment.unsize_length == 0 && !adjustment.address_of && adjustment.derefs == 0) {
        return;
    }

    Adjustment coercion = {.kind = adjustment.unsize_length > 0 ? ADJUST_UNSIZE
                                   : adjustment.address_of      ? ADJUST_BORROW
                                                                : ADJUST_NONE,
                           .to = method->signature.params[0],
                           .length = adjustment.unsize_length};

    adjust_derefs(state, &coercion, fact_type_of(state->facts, receiver), (unsigned int)adjustment.derefs);

    if (coercion.kind == ADJUST_NONE) {
        coercion.to = coercion.deref_types[coercion.derefs - 1];
    }

    fact_set_adjustment(state->facts, receiver, coercion);
}

/* What the call names, which lowering reads: the receiver stands as the first argument where it is
 * emitted, and the tree keeps the shape the source was written in. */
static void resolve_as_method_call(ResolverState *state, ASTExpr *expr, Function *method,
                                   ReceiverAdjustment adjustment) {
    ASTExpr *receiver = expr->call.target->field.target;

    record_receiver_adjustment(state, receiver, method, adjustment);

    /* An owning receiver is given away by the call, as an owning parameter is by an argument. */
    mark_implicit_move(state, receiver, method->signature.params[0], receiver->span);

    fact_set_callee(state->facts, expr, method);
    fact_set_call_kind(state->facts, expr, CALL_METHOD);
}

static bool reconcile_receiver(ResolverState *state, ASTExpr *expr, ASTExpr *receiver, const Type *declared,
                               const Type *actual, const String *name, ReceiverAdjustment *out) {
    const Type *at = actual;

    for (size_t derefs = 0;; derefs++) {
        if (declared == at) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        if (accepts_by_borrowing(declared, at)) {
            if (!is_addressable(state, receiver)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
                return false;
            }

            Symbol *addressed = fact_root_local(state->facts, receiver);
            if (addressed) {
                addressed->pinned = true;
            }

            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = true};
            return true;
        }

        if (unsizes_to_a_slice(declared, at)) {
            if (!is_addressable(state, receiver)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
                return false;
            }

            Symbol *addressed = fact_root_local(state->facts, receiver);

            if (addressed) {
                addressed->pinned = true;
            }

            /* A body checked before its length is fixed emits nothing, so the count it sees is unused. */
            const Type *array = receiver_base_type(at);

            *out = (ReceiverAdjustment){.derefs = derefs,
                                        .unsize_length =
                                            type_array_length_is_known(array) ? type_array_length(array) : 1};
            return true;
        }

        if (reads_as_a_view(state->types, declared, at) || lends_by_pointer(declared, at)) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        if (!type_is_indirect(at)) {
            break;
        }

        at = type_pointee(at);
    }

    diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot call '%s' on %s", name->data,
               type_name(state, actual));
    return false;
}

static const Type *resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected);
static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr);

/* A generic instantiating itself at an ever-larger type would collect without end. */
static void resolve_func_body(ResolverState *state, ASTStmt *stmt);
static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

/* Matches a declared parameter type against an argument's, binding each type parameter it reaches. */
static bool infer_type_args(const Type *declared, const Type *actual, TypeArg *args, size_t owed) {
    if (!declared || !actual || !type_has_param(declared)) {
        return true;
    }

    if (type_kind(declared) == TYPE_PARAM) {
        size_t index = type_param_index(declared);

        /* The first argument to reach a parameter fixes it; a later disagreement is an argument type error.
         */
        if (index < owed && !type_arg_is_set(args[index])) {
            args[index] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = actual};
        }

        return true;
    }

    /* An argument is lent or dereferenced to reach a borrowing parameter, so match what each finally names.
     */
    if (type_kind(declared) == TYPE_REF) {
        /* A borrow reaches what it names, so '&C' takes the pointee rather than binding C to the reference.
         */
        const Type *from = type_is_indirect(actual) ? type_pointee(actual) : actual;

        for (const Type *at = from;; at = type_pointee(at)) {
            TypeArg attempt[GAB_MAX_TYPE_PARAMS];
            memcpy(attempt, args, owed * sizeof(TypeArg));

            if (infer_type_args(type_pointee(declared), at, attempt, owed)) {
                memcpy(args, attempt, owed * sizeof(TypeArg));
                return true;
            }

            if (!type_is_indirect(at)) {
                return false;
            }
        }
    }

    if (type_kind(declared) != type_kind(actual)) {
        return false;
    }

    if (type_is_indirect(declared)) {
        return infer_type_args(type_pointee(declared), type_pointee(actual), args, owed);
    }

    if (type_decl(declared) != type_decl(actual) || type_arg_count(declared) != type_arg_count(actual)) {
        return false;
    }

    for (size_t i = 0; i < type_arg_count(declared); i++) {
        const TypeArg want = type_args(declared)[i];
        const TypeArg got = type_args(actual)[i];

        if (want.kind != got.kind) {
            return false;
        }

        if (want.kind == TYPE_ARG_TYPE) {
            if (!infer_type_args(want.type, got.type, args, owed)) {
                return false;
            }

            continue;
        }

        /* A value argument fixes its parameter the way a type one does, from what the call was given. */
        if (want.constant.kind == CONST_PARAM && want.constant.param < owed &&
            !type_arg_is_set(args[want.constant.param])) {
            args[want.constant.param] = got;
        }
    }

    return true;
}

/* 'fixed' slots are already known from a receiver; 'self_params' is 1 when parameter zero is one. */
static bool infer_call_args(ResolverState *state, ASTExpr *expr, Function *function, TypeArg *args,
                            size_t fixed, size_t self_params) {
    size_t owed = function->decl->type_param_count;

    size_t written = written_arg_count(expr);

    if (written + self_params != function->signature.param_count) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                   function->signature.param_count - self_params, written);
        return false;
    }

    for (size_t i = 0; i < written; i++) {
        const Type *argument = resolve_expr(state, written_arg(expr, i), NULL);

        if (is_error_type(argument)) {
            return false;
        }

        infer_type_args(function->signature.params[i + self_params], argument, args, owed);
    }

    for (size_t i = fixed; i < owed; i++) {
        if (!type_arg_is_set(args[i])) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "no argument names every type parameter of '%s', so each is written",
                       function->decl->id.name->data);
            return false;
        }
    }

    return true;
}

/* Type arguments a call names itself; only a plain call can, a method call's target having nowhere for them.
 */
/* The type a value parameter and an array length both have, which is the only one they can have. */
static const Type *i32_type(ResolverState *state) {
    return type_registry_get_primitive(state->types, TYPE_I32);
}

static bool take_written_type_args(ResolverState *state, ASTExpr *expr, Function *generic,
                                   const TypeExpr *supplied, TypeArg *args) {
    size_t owed = generic->decl->type_param_count;

    if (supplied->kind != TYPE_EXPR_APPLY || supplied->apply.args.size != owed) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s)",
                   generic->decl->id.name->data, owed);
        return false;
    }

    for (size_t i = 0; i < owed; i++) {
        if (supplied->apply.args.data[i]->kind == TYPE_EXPR_CONST) {
            Constant given = constant_int(i32_type(state), supplied->apply.args.data[i]->constant);

            args[i] = (TypeArg){.kind = TYPE_ARG_CONST, .constant = {.kind = CONST_VALUE, .value = given}};
            continue;
        }

        const Type *argument = resolve_type_expr(state, supplied->apply.args.data[i], expr->span);

        if (is_error_type(argument)) {
            return false;
        }

        args[i] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = argument};
    }

    return true;
}

/* The arguments a receiver's type already fixes, which are the ones its owner declared. */
static size_t take_receiver_type_args(const Type *receiver, TypeArg *args) {
    size_t fixed = type_arg_count(receiver);

    for (size_t i = 0; i < fixed; i++) {
        args[i] = type_args(receiver)[i];
    }

    return fixed;
}

/* A bound is nominal: the argument must say it implements the interface, not merely supply its methods. */
static bool check_bounds_satisfied(ResolverState *state, ASTExpr *expr, const Function *generic,
                                   const TypeArg *args, size_t owed) {
    const TypeParamBound *bounds = generic->decl->type_param_bounds;

    if (!bounds) {
        return true;
    }

    TypeRegistry *registry = state->types;

    for (size_t i = 0; i < owed && i < GAB_MAX_TYPE_PARAMS; i++) {
        /* Only a type argument carries a conformance; a value one has no interface to satisfy. */
        if (bounds[i].kind != BOUND_INTERFACE || !type_arg_is_set(args[i]) || args[i].kind != TYPE_ARG_TYPE ||
            type_kind(args[i].type) == TYPE_PARAM) {
            continue;
        }

        const InterfaceRef *bound = &bounds[i].interface;

        if (!type_registry_conforms_at_any(registry, args[i].type, bound->interface->id)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "%s does not implement '%s'",
                       type_name(state, args[i].type), bound->interface->id.name->data);
            return false;
        }

        if (!type_registry_conforms(registry, args[i].type, bound->interface->id, bound->args,
                                    bound->arg_count)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "%s implements '%s' at another type",
                       type_name(state, args[i].type), bound->interface->id.name->data);
            return false;
        }
    }

    return true;
}

static Function *specialize(ResolverState *state, ASTExpr *expr, Function *generic, TypeArg *args,
                            size_t fixed, size_t self_params) {
    size_t owed = generic->decl->type_param_count;

    /* An owner's parameters were fixed before the call, so this already is the instance. */
    if (owed <= fixed) {
        pending_bodies_instantiate(state->work, generic, state->diagnostics);

        return generic;
    }

    if (!infer_call_args(state, expr, generic, args, fixed, self_params)) {
        return NULL;
    }

    if (!check_bounds_satisfied(state, expr, generic, args, owed)) {
        return NULL;
    }

    Function *specialized = function_registry_instance(state->functions, generic->decl, args, owed);

    pending_bodies_instantiate(state->work, specialized, state->diagnostics);

    return specialized;
}

static Function *specialize_method_call(ResolverState *state, ASTExpr *expr, Function *method,
                                        const Type *receiver) {
    TypeArg args[GAB_MAX_TYPE_PARAMS] = {0};

    return specialize(state, expr, method, args, take_receiver_type_args(receiver, args), 1);
}

static Function *specialize_call(ResolverState *state, ASTExpr *expr, Function *generic) {
    const TypeExpr *supplied = expr->call.target->var.owner_type_expr;

    TypeArg args[GAB_MAX_TYPE_PARAMS] = {0};

    if (supplied && !take_written_type_args(state, expr, generic, supplied, args)) {
        return NULL;
    }

    return specialize(state, expr, generic, args, 0, 0);
}

static const Type *resolve_param_type_in(ResolverState *state, ASTField *param, bool generic);

/* The signature as the implementor sees it: 'Self' and the interface's parameters substituted away. */
static Function *interface_method_for(ResolverState *state, const InterfaceDecl *interface, size_t index,
                                      const Type *implementor, const TypeArg *args, size_t arg_count) {
    const Function *signature = interface->methods[index];

    TypeArg substitutions[GAB_MAX_TYPE_PARAMS];
    substitutions[0] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = implementor};

    for (size_t i = 0; i < arg_count && i + 1 < GAB_MAX_TYPE_PARAMS; i++) {
        substitutions[i + 1] = args[i];
    }

    Arena *arena = state->compile_arena;

    /* Substituted away, so the result is concrete however generic the signature it came from was. */
    FuncDecl *decl = arena_alloc(arena, sizeof(FuncDecl));
    *decl = *signature->decl;
    decl->type_param_count = 0;

    Function *method = arena_alloc(arena, sizeof(Function));
    *method = (Function){
        .decl = decl,
        .signature = func_signature_instantiate(state->types, arena, &signature->signature, substitutions,
                                                arg_count + 1),
        /* The parameter this stands on, so substituting it finds the implementor's own method. */
        .bound_self = implementor,
    };

    return method;
}

/* A parameter's methods are the ones its bound declares, with 'Self' as the parameter itself. */
static Function *bound_method(ResolverState *state, const Type *base, String *name, Span span) {
    (void)span;

    if (!base || type_kind(base) != TYPE_PARAM) {
        return NULL;
    }

    size_t index = type_param_index(base);

    if (state->param_bounds[index].kind != BOUND_INTERFACE) {
        return NULL;
    }

    const InterfaceRef *bound = &state->param_bounds[index].interface;

    for (size_t i = 0; i < bound->interface->method_count; i++) {
        if (bound->interface->methods[i]->decl->id.name != name) {
            continue;
        }

        return interface_method_for(state, bound->interface, i, base, bound->args, bound->arg_count);
    }

    return NULL;
}

/* A method is the one the type owns, or the one its bound declares when the type is a parameter. */
static Function *find_method(ResolverState *state, const Type *receiver, String *name, Span span,
                             const Type **out_base) {
    const Type *base = NULL;

    Function *method = find_method_on_chain(state->types, state->functions, receiver, name, &base);

    if (!method) {
        method = bound_method(state, base, name, span);
    }

    if (out_base) {
        *out_base = base;
    }

    return method;
}

/* 'xs[i]' on an implementor of 'Index' is 'xs.index(i)' read through, which is concluded here rather
 * than written into the tree: the call the element's 'Index' names is what lowering emits. */
static const Type *resolve_index_through_interface(ResolverState *state, ASTExpr *expr) {
    Span span = expr->span;

    ASTExpr *target = expr->index.target;
    const Type *target_type = fact_type_of(state->facts, target);

    String *name = string_from_cstr(state->strings, "index");

    const Type *base = NULL;
    Function *method = find_method(state, target_type, name, span, &base);

    /* Reaching the method is an implementation detail, so a missing one is reported as the interface. */
    if (!method) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "%s is indexed with '[]' by implementing 'Index'",
                   type_name(state, target_type));
        return resolver_error_type(state);
    }

    method = specialize_method_call(state, expr, method, base);

    if (!method) {
        return resolver_error_type(state);
    }

    if (method->signature.param_count != 2) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'index' of %s takes %zu argument(s), not one",
                   type_name(state, target_type), method->signature.param_count);
        return resolver_error_type(state);
    }

    ReceiverAdjustment adjustment;

    if (!reconcile_receiver(state, expr, target, method->signature.params[0], target_type, name,
                            &adjustment)) {
        return resolver_error_type(state);
    }

    record_receiver_adjustment(state, target, method, adjustment);

    mark_implicit_move(state, target, method->signature.params[0], target->span);

    check_call_arg(state, expr->index.index, method->signature.params[1], 1);

    fact_set_callee(state->facts, expr, method);
    fact_set_call_kind(state->facts, expr, CALL_INDEX);

    const Type *lent = method->signature.return_type;

    if (type_kind(lent) != TYPE_REF) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "'index' of %s returns %s rather than lending an element", type_name(state, target_type),
                   type_name(state, lent));
        return resolver_error_type(state);
    }

    /* The element is read through what 'index' lends, so indexing answers with what it points at. */
    return type_pointee(lent);
}

/* A declaration is an intrinsic only where this names one of these, so the two cannot drift. */
static const IntrinsicLowering *intrinsic_for(ResolverState *state, const String *owner, const String *name) {
    return type_registry_intrinsic(state->types, owner, name);
}

static void resolve_method_call(ResolverState *state, ASTExpr *expr) {
    ASTExpr *receiver = expr->call.target->field.target;
    StringRef name = expr->call.target->field.name;

    const Type *receiver_type = resolve_expr(state, receiver, NULL);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i], NULL);
    }

    if (is_error_type(receiver_type)) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    String *method_name = resolver_intern(state, name);

    const Type *base = NULL;
    Function *method = find_method(state, receiver_type, method_name, expr->span, &base);

    if (!method) {
        /* A parameter's methods are its bound's, so one with no bound has none to name. */
        if (base && type_kind(base) == TYPE_PARAM &&
            state->param_bounds[type_param_index(base)].kind != BOUND_INTERFACE) {
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span,
                       "a type parameter has the methods its bound declares, and this one has no bound");
            fact_set_type(state->facts, expr, resolver_error_type(state));
            return;
        }

        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    /* The ending runs where the value ends, so calling it here would run it twice on that value. */
    if (method == function_registry_destructor(state->functions, base)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'destroy' runs where the value ends, so nothing calls it by hand");
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    method = specialize_method_call(state, expr, method, base);

    if (!method) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    const Type *declared_receiver = method->signature.param_count > 0 ? method->signature.params[0] : base;

    if (method->signature.param_count == 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'%s' takes nothing, so it is called as '%s::%s()' rather than on a value",
                   method_name->data, type_name(state, base), method_name->data);

        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    size_t declared_params = method->signature.param_count - 1;

    ReceiverAdjustment adjustment;

    if (!reconcile_receiver(state, expr, receiver, declared_receiver, receiver_type, method_name,
                            &adjustment)) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    if (expr->call.args.size != declared_params) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                   declared_params, expr->call.args.size);
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    resolve_as_method_call(state, expr, method, adjustment);

    /* The receiver stands as parameter zero, so the written arguments answer for the rest. */
    check_call_args(state, &expr->call.args, method->signature.params + 1);

    /* An intrinsic type-checks as the call it is written as, and lowering expands it. */
    if (method->decl->modifiers & FUNC_MOD_INTRINSIC) {
        fact_set_type(state->facts, expr,
                      type_registry_substitute(state->types, method->signature.return_type, type_args(base),
                                               type_arg_count(base)));
        return;
    }

    fact_set_type(state->facts, expr, method->signature.return_type);
}

static bool reads_as_a_view(TypeRegistry *registry, const Type *to, const Type *from) {
    const Type *view = type_registry_deref_of(registry, from);

    return view && type_kind(to) == TYPE_REF && view == type_pointee(to);
}

static const Type *derefs_to(TypeRegistry *registry, const Type *type) {
    return type_registry_deref_of(registry, type);
}

static bool lends_by_pointer(const Type *to, const Type *from) {
    return type_kind(to) == TYPE_REF && type_is_indirect(from) && type_pointee(to) == type_pointee(from);
}

static bool accepts_by_borrowing(const Type *to, const Type *from) {
    return to != from && type_kind(to) == TYPE_REF && type_pointee(to) == from;
}

/* An array reaches a '&slice<T>' by handing over where it starts and how many it holds. */
static bool unsizes_to_a_slice(const Type *to, const Type *from) {
    if (type_kind(to) != TYPE_REF || type_kind(type_pointee(to)) != TYPE_SLICE) {
        return false;
    }

    while (type_is_indirect(from)) {
        from = type_pointee(from);
    }

    return type_kind(from) == TYPE_ARRAY && type_array_element(from) == type_slice_element(type_pointee(to));
}

static bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from) {
    if (to == from) {
        return true;
    }

    if (unsizes_to_a_slice(to, from)) {
        return true;
    }

    if (accepts_by_borrowing(to, from)) {
        return true;
    }

    for (const Type *at = from;; at = type_pointee(at)) {
        if (reads_as_a_view(registry, to, at) || lends_by_pointer(to, at)) {
            return true;
        }

        if (!type_is_indirect(at)) {
            return false;
        }
    }
}

/* Records the dereferences a coercion applies, naming the type each one reaches. */
static void adjust_derefs(ResolverState *state, Adjustment *adjustment, const Type *from,
                          unsigned int count) {
    adjustment->derefs = count;
    adjustment->deref_types = count ? arena_alloc(state->compile_arena, count * sizeof(const Type *)) : NULL;

    for (unsigned int i = 0; i < count; i++) {
        from = type_pointee(from);
        adjustment->deref_types[i] = from;
    }
}

static bool unsize_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span) {
    const Type *array = fact_type_of(state->facts, expr);
    unsigned int derefs = 0;

    while (type_is_indirect(array)) {
        array = type_pointee(array);
        derefs++;
    }

    if (!is_addressable(state, expr)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    Symbol *addressed = fact_root_local(state->facts, expr);

    if (addressed) {
        addressed->pinned = true;
    }

    Adjustment adjustment = {.kind = ADJUST_UNSIZE,
                             .to = destination,
                             .length = type_array_length_is_known(array) ? type_array_length(array) : 1};

    adjust_derefs(state, &adjustment, fact_type_of(state->facts, expr), derefs);
    fact_set_adjustment(state->facts, expr, adjustment);

    return true;
}

static bool borrow_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span) {
    const Type *from = fact_type_of(state->facts, expr);

    if (unsizes_to_a_slice(destination, from)) {
        return unsize_into(state, expr, destination, span);
    }

    const Type *at = from;
    unsigned int derefs = 0;

    while (type_is_indirect(at) && type_pointee(destination) != type_pointee(at) &&
           type_pointee(destination) != at) {
        at = type_pointee(at);
        derefs++;
    }

    if (!accepts_by_borrowing(destination, at)) {
        /* The dereferences alone reach what the destination takes, so they stand as the coercion. */
        if (derefs > 0) {
            Adjustment adjustment = {.kind = ADJUST_NONE, .to = at};

            adjust_derefs(state, &adjustment, from, derefs);
            fact_set_adjustment(state->facts, expr, adjustment);
        }

        return true;
    }

    if (!is_addressable(state, expr)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    Symbol *addressed = fact_root_local(state->facts, expr);

    if (addressed) {
        addressed->pinned = true;
    }

    Adjustment adjustment = {.kind = ADJUST_BORROW, .to = destination};

    adjust_derefs(state, &adjustment, from, derefs);
    fact_set_adjustment(state->facts, expr, adjustment);

    return true;
}

/* Listing every kind rather than the ones that answer true, so a kind added later must be placed here. */
bool is_integer_type(const Type *t) {
    switch (type_kind(t)) {
    case TYPE_I32:
    case TYPE_U8:
    case TYPE_USIZE:
        return true;

    case TYPE_F32:
    case TYPE_BOOL:
    case TYPE_STR:
    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
    case TYPE_PARAM:
    case TYPE_UNKNOWN:
    case TYPE_ERROR:
        return false;
    }

    return false;
}

bool is_numeric_type(const Type *t) { return is_integer_type(t) || type_kind(t) == TYPE_F32; }

bool is_boolean_type(const Type *t) { return type_kind(t) == TYPE_BOOL; }

bool is_ordered_type(const Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

static bool is_string_type(TypeRegistry *registry, const Type *t) {
    return type_registry_deref_of(registry, t) == type_registry_get_primitive(registry, TYPE_STR) ||
           type_is_str_ref(t);
}

static bool is_comparable_type(TypeRegistry *registry, const Type *t) {
    return is_numeric_type(t) || is_boolean_type(t) || is_string_type(registry, t);
}

static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

static StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type);
static void report_containment_cycle(ResolverState *state, StructDecl *closes_on, Span span);

static bool reject_unsized(ResolverState *state, const Type *type, Span span, const char *held_as) {
    if (!type || type_is_sized(type)) {
        return false;
    }

    diag_error(state->diagnostics, GAB_ERR_TYPE, span,
               "nothing holds a '%s', so it cannot be %s; write '&%s'", type_name(state, type), held_as,
               type_name(state, type));
    return true;
}

static bool bin_op_accepts(ResolverState *state, BinOp op, const Type *type, Span span) {
    const char *op_name = bin_op_name(op);

    switch (op) {
    case BIN_OP_ADD:
    case BIN_OP_SUB:
    case BIN_OP_MUL:
    case BIN_OP_DIV:
        if (!is_numeric_type(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires a numeric type, found %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;

    case BIN_OP_MOD:
        if (!is_integer_type(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires an integer type, found %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_EQUAL:
    case BIN_OP_NEQUAL:
        if (!is_comparable_type(state->types, type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' is not supported for %s", op_name,
                       type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_LESS:
    case BIN_OP_GREATER:
    case BIN_OP_LEQUAL:
    case BIN_OP_GEQUAL:
        if (!is_ordered_type(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires an ordered type, found %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_AND:
    case BIN_OP_OR:
        if (!is_boolean_type(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires a boolean type, found %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;
    }

    return true;
}

static bool bin_op_yields_bool(BinOp op) {
    switch (op) {
    case BIN_OP_EQUAL:
    case BIN_OP_NEQUAL:
    case BIN_OP_LESS:
    case BIN_OP_GREATER:
    case BIN_OP_LEQUAL:
    case BIN_OP_GEQUAL:
    case BIN_OP_AND:
    case BIN_OP_OR:
        return true;
    default:
        return false;
    }
}

static bool resolve_cast(ResolverState *state, ASTExpr *expr) {
    Symbol *symbol = scope_lookup(state->current_scope, resolver_intern(state, expr->call.target->var.name));

    const Type *target = symbol_type(state->types, symbol);

    /* 'raw<i32>(p)' names its target by application, where 'i32(x)' names one that takes no argument;
     * a name that resolves to no type at all is a call rather than a conversion, generic or not. */
    if (expr->call.target->var.owner_type_expr) {
        /* The runs and the arrays name no binding of their own, so what they resolve to is asked for. */
        bool names_a_type = (symbol && (symbol->kind == SYMBOL_TYPE || symbol->kind == SYMBOL_TYPE_DECL)) ||
                            names_the_same(state, expr->call.target->var.name, resolver_names(state)->raw);

        if (!names_a_type) {
            return false;
        }

        target = resolve_type_expr(state, expr->call.target->var.owner_type_expr, expr->span);
    }

    if (!target || is_error_type(target)) {
        return false;
    }

    ASTExprList args = expr->call.args;
    ASTExpr *operand = args.size == 1 ? args.data[0] : NULL;

    fact_set_call_kind(state->facts, expr, CALL_CONVERSION);

    if (!operand) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "a conversion to %s takes one operand",
                   type_name(state, target));
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return true;
    }

    const Type *from = resolve_expr(state, operand, NULL);

    if (is_error_type(from)) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return true;
    }

    /* A run reads as a run of another element: the address is the same, and what it points at is not
     * checked, which is what makes 'raw' the type that says so. */
    bool reads_as_a_run = type_kind(target) == TYPE_RAW && type_kind(from) == TYPE_RAW;

    if (!reads_as_a_run && (!is_numeric_type(target) || !is_numeric_type(from))) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot convert %s to %s",
                   type_name(state, from), type_name(state, target));
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return true;
    }

    fact_set_type(state->facts, expr, target);
    return true;
}

static const Type *resolve_expr_kind(ResolverState *state, ASTExpr *expr, const Type *expected) {
    switch (expr->kind) {
    case EXPR_BIN_OP: {
        const Type *left_type = resolve_expr(state, expr->bin_op.left, expected);

        /* The left side types the right, so a literal beside a count is that count's width. */
        const Type *right_type = resolve_expr(state, expr->bin_op.right, left_type);

        if (is_error_type(left_type) || is_error_type(right_type)) {
            return resolver_error_type(state);
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        TypeRegistry *registry = state->types;

        bool both_strings = is_string_type(registry, left_type) && is_string_type(registry, right_type);

        if (left_type != right_type && !both_strings) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot apply '%s' to %s and %s",
                       op_name, type_name(state, left_type), type_name(state, right_type));
            return resolver_error_type(state);
        }

        if (!bin_op_accepts(state, expr->bin_op.op, left_type, expr->span)) {
            return resolver_error_type(state);
        }

        if (both_strings) {
            const Type *characters =
                type_registry_ref_to(state->types, type_registry_get_primitive(state->types, TYPE_STR));

            borrow_into(state, expr->bin_op.left, characters, expr->span);
            borrow_into(state, expr->bin_op.right, characters, expr->span);
        }

        return bin_op_yields_bool(expr->bin_op.op) ? type_registry_get_primitive(state->types, TYPE_BOOL)
                                                   : left_type;
    }
    case EXPR_BUILTIN: {
        if (names_the_same(state, expr->builtin.name, resolver_names(state)->caller)) {
            if (!state->func_context.is_caller) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'@caller()' answers where a call was written, so only a 'caller' function "
                           "asks it");

                return resolver_error_type(state);
            }

            const Type *location = resolver_location_type(state);

            if (!location) {
                diag_error(state->diagnostics, GAB_ERR_NAME, expr->span,
                           "the core declares no '%s', which '@caller()' answers with", GAB_LOCATION_TYPE);

                return resolver_error_type(state);
            }

            return location;
        }

        if (names_the_same(state, expr->builtin.name, resolver_names(state)->size_of)) {
            if (!expr->builtin.type_expr || expr->builtin.type_expr->apply.args.size != 1) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'@size_of<T>()' measures one type, as '@size_of<i32>()'");

                return resolver_error_type(state);
            }

            const Type *measured =
                resolve_type_expr(state, expr->builtin.type_expr->apply.args.data[0], expr->span);

            if (is_error_type(measured)) {
                return resolver_error_type(state);
            }

            const Type *counted = type_registry_get_primitive(state->types, TYPE_USIZE);

            /* The size is known here, so it is recorded beside the node rather than measured again. */
            fact_set_constant(state->facts, expr,
                              constant_int(counted, (int32_t)type_registry_size_of(state->types, measured)));

            return counted;
        }

        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "the compiler supplies no '@%.*s'",
                   (int)expr->builtin.name.length, expr->builtin.name.data);

        return resolver_error_type(state);
    }
    case EXPR_VARIABLE: {
        String *sought = resolver_intern(state, expr->var.name);

        Symbol *entry = scope_lookup(state->current_scope, sought);

        if (!entry) {
            entry = file_lookup(state, sought);
        }

        if (entry) {
            if (entry->kind == SYMBOL_FUNC) {
                fact_set_callee(state->facts, expr, entry->func);
                return NULL;
            }

            fact_set_use(state->facts, expr, entry);
            return entry->var.type;
        }

        fact_set_callee(state->facts, expr, resolve_qualified_func(state, expr));

        if (!fact_callee_of(state->facts, expr)) {
            char *name = string_ref_to_cstr(expr->var.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "undeclared variable '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        return NULL;
    }
    case EXPR_CALL: {
        if (!expr->call.target && fact_callee_of(state->facts, expr)) {
            return fact_callee_of(state->facts, expr)->signature.return_type;
        }

        if (expr->call.target && expr->call.target->kind == EXPR_FIELD) {
            resolve_method_call(state, expr);
            return fact_type_of(state->facts, expr);
        }

        if (expr->call.target && expr->call.target->kind == EXPR_VARIABLE && resolve_cast(state, expr)) {
            return fact_type_of(state->facts, expr);
        }

        /* A builtin names no function, so what it answers with is what the call is. */
        if (expr->call.target && expr->call.target->kind == EXPR_BUILTIN) {
            resolve_expr(state, expr->call.target, NULL);

            if (expr->call.args.size) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'@%.*s()' takes no arguments",
                           (int)expr->call.target->builtin.name.length, expr->call.target->builtin.name.data);
            }

            /* A constant one answers with the constant itself, which the call is worth just as much. */
            Constant answered;

            if (fact_constant_of(state->facts, expr->call.target, &answered)) {
                fact_set_constant(state->facts, expr, answered);
            }

            return fact_type_of(state->facts, expr->call.target);
        }

        resolve_expr(state, expr->call.target, NULL);

        Function *callee = fact_callee_of(state->facts, expr->call.target);

        if (callee && callee->decl->type_param_count > 0) {
            callee = specialize_call(state, expr, callee);

            if (!callee) {
                return resolver_error_type(state);
            }

            fact_set_callee(state->facts, expr->call.target, callee);
        }

        bool params_known = callee && expr->call.args.size == callee->signature.param_count;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            resolve_expr(state, expr->call.args.data[i], params_known ? callee->signature.params[i] : NULL);
        }

        if (!callee) {
            if (fact_type_of(state->facts, expr->call.target) != resolver_error_type(state)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "this expression is not callable");
            }

            return resolver_error_type(state);
        }

        if (expr->call.args.size != callee->signature.param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                       callee->signature.param_count, expr->call.args.size);
            return resolver_error_type(state);
        }

        check_call_args(state, &expr->call.args, callee->signature.params);

        fact_set_callee(state->facts, expr, callee);
        return callee->signature.return_type;
    }
    case EXPR_INDEX: {
        const Type *target_type = resolve_expr(state, expr->index.target, NULL);
        const Type *index_type = resolve_expr(state, expr->index.index, NULL);

        if (is_error_type(target_type) || is_error_type(index_type)) {
            return resolver_error_type(state);
        }

        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
        }

        /* An array indexes inline, against the length its type carries; anything else supplies 'Index'. */
        if (type_kind(target_type) != TYPE_ARRAY) {
            return resolve_index_through_interface(state, expr);
        }

        if (index_type != type_registry_get_primitive(state->types, TYPE_I32)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "an index must be an i32, not %s",
                       type_name(state, index_type));
            return resolver_error_type(state);
        }

        return type_array_element(target_type);
    }
    case EXPR_FIELD: {
        const Type *target_type = resolve_expr(state, expr->field.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
        }

        if (type_kind(target_type) != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s is not a struct, so it has no fields",
                       type_name(state, fact_type_of(state->facts, expr->field.target)));
            return resolver_error_type(state);
        }

        String *field_name = resolver_intern(state, expr->field.name);
        const TypeField *field = type_registry_find_field(state->types, target_type, field_name);

        if (!field) {
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no field '%s'",
                       type_name(state, target_type), field_name->data);
            return resolver_error_type(state);
        }

        fact_set_field(state->facts, expr,
                       (size_t)(field - type_registry_fields_of(state->types, target_type)->fields));

        return field->type;
    }
    case EXPR_ADDR_OF: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!is_addressable(state, expr->unary.target)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of a temporary");
            return resolver_error_type(state);
        }

        if (type_kind(target_type) == TYPE_BOX) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of an owning pointer; return ownership instead of "
                       "repointing it through a borrow");
            return resolver_error_type(state);
        }

        Symbol *addressed = fact_root_local(state->facts, expr->unary.target);
        if (addressed) {
            addressed->pinned = true;
        }

        return type_registry_ref_to(state->types, target_type);
    }
    case EXPR_DEREF: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!type_is_indirect(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot dereference %s",
                       type_name(state, target_type));
            return resolver_error_type(state);
        }

        return type_pointee(target_type);
    }
    case EXPR_NEG: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!is_numeric_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "unary '-' requires a numeric type, found %s", type_name(state, target_type));
            return resolver_error_type(state);
        }

        return target_type;
    }
    case EXPR_NOT: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!is_boolean_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "unary '!' requires bool, found %s",
                       type_name(state, target_type));
            return resolver_error_type(state);
        }

        return target_type;
    }
    case EXPR_BOX: {
        const Type *type = resolve_expr(state, expr->box_expr.value, NULL);

        if (is_error_type(type)) {
            return resolver_error_type(state);
        }

        if (type_kind(type) == TYPE_REF) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; a heap slot cannot hold a borrow", type_name(state, type));
            return resolver_error_type(state);
        }

        return type_registry_box_to(state->types, type);
    }
    case EXPR_STRUCT_LIT: {
        const Type *type = resolve_type_expr(state, expr->struct_lit.type_expr, expr->span);

        if (is_error_type(type)) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            return resolver_error_type(state);
        }

        TypeRegistry *registry = state->types;

        if (type_kind(type) != TYPE_STRUCT) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "%s is not a struct",
                       type_name(state, type));
            return resolver_error_type(state);
        }

        const TypeFields *fields = type_registry_fields_of(registry, type);

        bool ok = true;
        bool *seen = fields->count ? arena_alloc(state->compile_arena, fields->count * sizeof *seen) : NULL;

        for (size_t f = 0; f < fields->count; f++) {
            seen[f] = false;
        }

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            ASTFieldInit *init = &expr->struct_lit.fields.data[i];
            String *field_name = resolver_intern(state, init->name);

            size_t index = fields->count;
            for (size_t f = 0; f < fields->count; f++) {
                if (fields->fields[f].name == field_name) {
                    index = f;
                    break;
                }
            }

            if (index == fields->count) {
                resolve_expr(state, init->value, NULL);
                diag_error(state->diagnostics, GAB_ERR_TYPE, init->span, "%s has no field '%.*s'",
                           type_name(state, type), (int)init->name.length, init->name.data);
                ok = false;
                continue;
            }

            if (seen[index]) {
                resolve_expr(state, init->value, fields->fields[index].type);
                diag_error(state->diagnostics, GAB_ERR_TYPE, init->span, "field '%.*s' is given twice",
                           (int)init->name.length, init->name.data);
                ok = false;
                continue;
            }

            seen[index] = true;
            fact_set_initialized_field(state->facts, init->value, index);

            const Type *field_type = fields->fields[index].type;

            resolve_expr(state, init->value, field_type);

            ASTExpr *value = init->value;

            if (is_error_type(fact_type_of(state->facts, value))) {
                ok = false;
                continue;
            }

            if (!type_accepts(registry, field_type, fact_type_of(state->facts, value))) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, value->span,
                           "field '%.*s' is %s, but %s was given", (int)init->name.length, init->name.data,
                           type_name(state, field_type), type_name(state, fact_type_of(state->facts, value)));
                ok = false;
                continue;
            }

            if (!borrow_into(state, init->value, field_type, value->span)) {
                ok = false;
                continue;
            }

            mark_implicit_move(state, init->value, field_type, value->span);
        }

        for (size_t f = 0; f < fields->count; f++) {
            if (!seen[f]) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "field '%s' is missing",
                           fields->fields[f].name->data);
                ok = false;
            }
        }

        return ok ? type : resolver_error_type(state);
    }
    case EXPR_ARRAY_LIT: {
        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            resolve_expr(state, expr->array_lit.elements.data[i], NULL);
        }

        if (!expected || type_kind(expected) != TYPE_ARRAY) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "an array's elements need the array's type to be written, as "
                       "'let xs: [i32; 3] = [1, 2, 3];'");
            return resolver_error_type(state);
        }

        int32_t length = type_array_length(expected);
        const Type *element = type_array_element(expected);

        if ((int32_t)expr->array_lit.elements.size != length) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %d element(s), found %zu",
                       length, expr->array_lit.elements.size);
            return resolver_error_type(state);
        }

        bool ok = true;

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            ASTExpr *value = expr->array_lit.elements.data[i];

            if (is_error_type(fact_type_of(state->facts, value))) {
                ok = false;
                continue;
            }

            if (!type_accepts(state->types, element, fact_type_of(state->facts, value))) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, value->span,
                           "element %zu is %s, but the array holds %s", i + 1,
                           type_name(state, fact_type_of(state->facts, value)), type_name(state, element));
                ok = false;
                continue;
            }

            if (!borrow_into(state, expr->array_lit.elements.data[i], element, value->span)) {
                ok = false;
                continue;
            }

            mark_implicit_move(state, expr->array_lit.elements.data[i], element, value->span);
        }

        return ok ? expected : resolver_error_type(state);
    }
    case EXPR_LITERAL: {
        TypeRegistry *registry = state->types;

        TypeKind kind = literal_type_kind(expr->lit.kind);

        /* A whole number takes the integer type its context asks for, rather than a width of its own. */
        if (expr->lit.kind == LITERAL_INT && expected && is_integer_type(expected)) {
            return expected;
        }

        /* Text is read through a reference, since the unit holds it and the value names where. */
        return kind == TYPE_STR ? type_registry_ref_to(registry, type_registry_get_primitive(registry, kind))
                                : type_registry_get_primitive(registry, kind);
    }
    default:
        return NULL;
    }
}

/* Records what the expression was found to be, so an arm states its answer by returning it. */
static const Type *resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected) {
    if (!expr) {
        return NULL;
    }

    const Type *type = resolve_expr_kind(state, expr, expected);

    /* A name denoting a function concludes no type, and recording none leaves it that way. */
    if (type) {
        fact_set_type(state->facts, expr, type);
    }

    return type;
}

static void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span) {
    if (!value || is_error_type(fact_type_of(state->facts, value))) {
        return;
    }

    if (type_registry_copies(state->types, fact_type_of(state->facts, value))) {
        return;
    }

    if (destination && !type_registry_owns(state->types, destination)) {
        return;
    }

    if (value->kind == EXPR_FIELD) {
        diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
                   "a field cannot be given up on its own; bind the whole struct instead");
        return;
    }

    if (value->kind == EXPR_INDEX) {
        diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
                   "an element cannot be given up on its own; bind the whole array instead");
        return;
    }

    if (value->kind != EXPR_VARIABLE || !fact_use_of(state->facts, value) ||
        fact_use_of(state->facts, value)->kind != SYMBOL_VAR) {
        return;
    }

    fact_set_moves(state->facts, value, true);
}

/* An element must be sized and non-recursive wherever a run of it is laid out. */
static const Type *resolve_element_type(ResolverState *state, TypeExpr *expr, Span span,
                                        const char *held_as) {
    const Type *element = resolve_type_expr(state, expr, span);

    if (is_error_type(element)) {
        return NULL;
    }

    if (type_has_param(element)) {
        return element;
    }

    if (reject_unsized(state, element, span, held_as)) {
        return NULL;
    }

    StructDecl *cycle = element_completes_a_cycle(state, element);

    if (cycle) {
        report_containment_cycle(state, cycle, span);
        return NULL;
    }

    if (type_registry_size_of(state->types, element) == 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "%s must have a size", held_as);
        return NULL;
    }

    return element;
}

/* Which kind of parameter a bound declares, which its syntax alone says: nothing here is resolved, so
 * this answers before the parameters are in scope and their bounds can be. 'N: i32' declares a value
 * parameter, as Rust spells 'const N: usize'; any other bound names an interface. */
static BoundKind bound_kind_of(const TypeRegistry *registry, StringPool *strings, const TypeExpr *bound) {
    if (!bound) {
        return BOUND_NONE;
    }

    return bound->kind == TYPE_EXPR_NAME &&
                   string_from_ref(strings, bound->name) == type_registry_names(registry)->i32
               ? BOUND_VALUE
               : BOUND_INTERFACE;
}

static bool bind_type_param(TypeRegistry *registry, Scope *params, String *name, size_t index,
                            BoundKind kind) {
    if (kind == BOUND_VALUE) {
        return scope_bind_const(params, name, (TypeConst){.kind = CONST_PARAM, .param = index});
    }

    return scope_bind_type_param(params, name, type_registry_param(registry, index));
}

/* A length is written as a literal, or named as the value parameter a generic declaration takes. */
static bool resolve_array_length(ResolverState *state, TypeExpr *expr, Span span, TypeArg *out) {
    if (expr->kind == TYPE_EXPR_CONST) {
        Constant length = constant_int(i32_type(state), expr->constant);

        *out = (TypeArg){.kind = TYPE_ARG_CONST, .constant = {.kind = CONST_VALUE, .value = length}};
        return true;
    }

    if (expr->kind == TYPE_EXPR_NAME) {
        Scope *scope = resolver_expr_scope(state, expr->name);
        Symbol *symbol = resolver_resolve_name(state, scope, resolver_expr_member(state, expr->name));

        if (symbol && symbol->kind == SYMBOL_CONST) {
            *out = (TypeArg){.kind = TYPE_ARG_CONST, .constant = symbol->constant};
            return true;
        }
    }

    diag_error(state->diagnostics, GAB_ERR_TYPE, span,
               "an array's length is a literal or a value parameter, as 'array<i32, 3>'");
    return false;
}

static const Type *resolve_array_type(ResolverState *state, TypeExpr *expr, Span span) {
    TypeRegistry *registry = state->types;

    if (expr->apply.args.size != 2) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "'array' takes an element and a length, as 'array<i32, 3>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "an array's element");

    TypeArg length;

    if (!element || !resolve_array_length(state, expr->apply.args.data[1], span, &length)) {
        return resolver_error_type(state);
    }

    if (length.constant.kind == CONST_PARAM) {
        return type_registry_array_with(registry, element, length);
    }

    if (length.constant.value.as_int <= 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "an array's length must be positive, not %d",
                   length.constant.value.as_int);
        return resolver_error_type(state);
    }

    return type_registry_array_with(registry, element, length);
}

static const Type *resolve_slice_type(ResolverState *state, TypeExpr *expr, Span span) {
    if (expr->apply.args.size != 1 || expr->apply.args.data[0]->kind == TYPE_EXPR_CONST) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'slice' takes one element type, as 'slice<int>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "a slice's element");

    if (!element) {
        return resolver_error_type(state);
    }

    return type_registry_slice_of(state->types, element);
}

/* A raw run names where elements start and nothing more: no length, and nothing it owns. */
static const Type *resolve_raw_type(ResolverState *state, TypeExpr *expr, Span span) {
    if (expr->apply.args.size != 1 || expr->apply.args.data[0]->kind == TYPE_EXPR_CONST) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'raw' takes one element type, as 'raw<i32>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "a raw run's element");

    if (!element) {
        return resolver_error_type(state);
    }

    return type_registry_raw_of(state->types, element);
}

static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span) {
    if (!expr) {
        return NULL;
    }

    TypeRegistry *registry = state->types;

    switch (expr->kind) {
    case TYPE_EXPR_BOX:
    case TYPE_EXPR_REF: {
        const Type *inner = resolve_type_expr(state, expr->indirect.inner, span);

        if (is_error_type(inner)) {
            return resolver_error_type(state);
        }

        return expr->kind == TYPE_EXPR_REF ? type_registry_ref_to(registry, inner)
                                           : type_registry_box_to(registry, inner);
    }

    case TYPE_EXPR_APPLY: {
        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->array)) {
            return resolve_array_type(state, expr, span);
        }

        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->slice)) {
            return resolve_slice_type(state, expr, span);
        }

        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->raw)) {
            return resolve_raw_type(state, expr, span);
        }

        Scope *base_scope = resolver_expr_scope(state, expr->apply.base->name);

        String *base_name = base_scope ? resolver_expr_member(state, expr->apply.base->name) : NULL;

        Symbol *base_symbol = base_name ? resolver_resolve_name(state, base_scope, base_name) : NULL;

        const TypeDecl *base_decl =
            base_symbol && base_symbol->kind == SYMBOL_TYPE_DECL ? base_symbol->type_decl : NULL;
        const Type *base = symbol_type(registry, base_symbol);

        if (!base_symbol) {
            char *name = string_ref_to_cstr(expr->apply.base->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        if (base_decl) {
            if (expr->apply.args.size != base_decl->param_count) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not %zu",
                           base_decl->id.name->data, base_decl->param_count, expr->apply.args.size);
                return resolver_error_type(state);
            }

            const Type *args[GAB_MAX_TYPE_PARAMS];

            for (size_t i = 0; i < expr->apply.args.size; i++) {
                args[i] = resolve_type_expr(state, expr->apply.args.data[i], span);

                if (is_error_type(args[i])) {
                    return resolver_error_type(state);
                }

                if (type_has_param(args[i])) {
                    continue;
                }

                if (reject_unsized(state, args[i], span, "a type argument")) {
                    return resolver_error_type(state);
                }

                StructDecl *cycle = element_completes_a_cycle(state, args[i]);

                if (cycle) {
                    report_containment_cycle(state, cycle, span);
                    return resolver_error_type(state);
                }
            }

            return type_registry_apply(registry, base_decl, args, expr->apply.args.size);
        }

        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "%s does not take a type argument",
                   type_name(state, base));

        return resolver_error_type(state);
    }

    case TYPE_EXPR_CONST:
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "a length is an argument to 'array', not a type");
        return resolver_error_type(state);

    case TYPE_EXPR_NAME:
        break;
    }

    Scope *scope = resolver_expr_scope(state, expr->name);

    Symbol *symbol = resolver_resolve_name(state, scope, resolver_expr_member(state, expr->name));

    const Type *type = symbol_type(registry, symbol);

    if (type) {
        return type;
    }

    if (symbol && symbol->kind == SYMBOL_TYPE_DECL) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not 0",
                   symbol->type_decl->id.name->data, symbol->type_decl->param_count);

        return resolver_error_type(state);
    }

    if (resolver_intern(state, expr->name) == resolver_names(state)->self) {
        diag_error(state->diagnostics, GAB_ERR_NAME, span,
                   "'Self' names the type an 'impl' block is for, and there is none here");

        return resolver_error_type(state);
    }

    char *name_text = string_ref_to_cstr(expr->name);
    diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name_text);
    free(name_text);

    return resolver_error_type(state);
}

static void resolve_stmt(ResolverState *state, ASTStmt *stmt);

static StructDecl *decl_held_by_value(ResolverState *state, const Type *type) {
    while (type && type_kind(type) == TYPE_ARRAY) {
        type = type_array_element(type);
    }

    for (size_t i = 0; i < state->struct_decls.size; i++) {
        if (type && state->struct_decls.data[i]->decl == type_decl(type)) {
            return state->struct_decls.data[i];
        }
    }

    return NULL;
}

static StructDecl *declare_struct(ResolverState *state, ASTStmt *stmt) {
    stmt->struct_decl.declared = true;

    String *struct_name = resolver_intern(state, stmt->struct_decl.name);

    if (stmt->struct_decl.intrinsic) {
        if (!state->declares_intrinsics) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "an intrinsic struct is given its meaning by the compiler, so only its core "
                       "library declares one");
            return NULL;
        }

        if (struct_name != resolver_names(state)->unique) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "the compiler gives no intrinsic struct '%s' its meaning", struct_name->data);
            return NULL;
        }
    }

    if (reject_self_as_name(state, struct_name, stmt->span)) {
        return NULL;
    }

    if (scope_type_lookup_declaring(state->current_scope, struct_name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return NULL;
    }

    size_t param_count = stmt->struct_decl.param_count;

    TypeDecl *declared = arena_alloc(state->compile_arena, sizeof(TypeDecl));

    *declared = (TypeDecl){
        .id = {.module = state->module_name, .name = struct_name},
        .param_count = param_count,
    };

    scope_bind_type_decl(resolver_declaring_scope(state), struct_name, declared);

    StructDecl *decl = arena_alloc(state->compile_arena, sizeof(StructDecl));

    *decl = (StructDecl){
        .stmt = stmt,
        .scope = resolver_declaring_scope(state),
        .file_scope = state->current_scope,
        .file = state->file,
        .visible = state->visible,
        .name = struct_name,
        .decl = declared,
        .fields_demanded = false,
        .poisoned = false,
    };

    struct_decl_list_add(&state->struct_decls, decl);

    return decl;
}

static void resolve_struct_fields(ResolverState *state, StructDecl *decl);
static void layout_struct(ResolverState *state, StructDecl *decl);

static size_t resolving_index_of(ResolverState *state, const StructDecl *decl) {
    size_t i = 0;

    while (i < state->resolving.size && state->resolving.data[i] != decl) {
        i++;
    }

    return i;
}

static StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type) {
    StructDecl *decl = decl_held_by_value(state, type);

    if (!decl) {
        return NULL;
    }

    if (resolving_index_of(state, decl) < state->resolving.size) {
        return decl;
    }

    resolve_struct_fields(state, decl);

    return NULL;
}

static void report_containment_cycle(ResolverState *state, StructDecl *closes_on, Span span) {
    char path[256];
    size_t written = 0;

    for (size_t i = resolving_index_of(state, closes_on); i < state->resolving.size; i++) {
        int n = snprintf(path + written, sizeof(path) - written, "'%s' contains ",
                         state->resolving.data[i]->name->data);

        if (n < 0 || (size_t)n >= sizeof(path) - written) {
            written += (size_t)snprintf(path + written, sizeof(path) - written, "... ");
            break;
        }

        written += (size_t)n;
    }

    diag_error(state->diagnostics, GAB_ERR_TYPE, span, "struct '%s' cannot contain itself: %.*s'%s'",
               closes_on->name->data, (int)written, path, closes_on->name->data);
}

static bool field_type_failed(ResolverState *state, const Type *type) {
    StructDecl *decl = decl_held_by_value(state, type);

    return decl && decl->poisoned;
}

static void resolve_struct_fields(ResolverState *state, StructDecl *decl) {
    if (decl->fields_demanded) {
        return;
    }

    decl->fields_demanded = true;

    struct_decl_list_add(&state->resolving, decl);

    ASTStmt *stmt = decl->stmt;
    TypeDecl *declared = decl->decl;

    Scope *enclosing = state->current_scope;
    const ASTFile *naming = state->file;
    const Visible *naming_visible = state->visible;

    state->file = decl->file;
    state->visible = decl->visible;

    Scope *params = scope_create(state->compile_arena, decl->file_scope);

    for (size_t i = 0; i < stmt->struct_decl.param_count; i++) {
        String *param_name = resolver_intern(state, stmt->struct_decl.params[i]);

        if (reject_self_as_name(state, param_name, stmt->span)) {
            continue;
        }

        if (!scope_bind_type(params, param_name, type_registry_param(state->types, i))) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "duplicate type parameter '%s' on '%s'",
                       param_name->data, decl->name->data);
        }
    }

    state->current_scope = params;

    size_t field_count = stmt->struct_decl.fields.size;
    TypeField *fields =
        field_count ? arena_alloc(state->compile_arena, field_count * sizeof(TypeField)) : NULL;

    bool poisoned = false;
    size_t resolved = 0;

    for (size_t i = 0; i < field_count; i++) {
        ASTField *field = stmt->struct_decl.fields.data[i];
        String *field_name = resolver_intern(state, field->name);

        bool duplicate = false;

        for (size_t seen = 0; seen < resolved; seen++) {
            if (fields[seen].name == field_name) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            diag_error(state->diagnostics, GAB_ERR_NAME, field->span, "duplicate field '%s' in struct '%s'",
                       field_name->data, decl->name->data);
            poisoned = true;
            continue;
        }

        const Type *field_type = resolve_type_expr(state, field->type_expr, field->span);

        if (is_error_type(field_type)) {
            poisoned = true;
            continue;
        }

        if (!type_has_param(field_type)) {
            StructDecl *cycle = element_completes_a_cycle(state, field_type);

            if (cycle) {
                report_containment_cycle(state, cycle, field->span);
                poisoned = true;
                continue;
            }

            if (field_type_failed(state, field_type)) {
                poisoned = true;
                continue;
            }

            if (reject_unsized(state, field_type, field->span, "a field")) {
                poisoned = true;
                continue;
            }
        }

        fields[resolved++] = (TypeField){.name = field_name, .type = field_type};
    }

    state->current_scope = enclosing;
    state->file = naming;
    state->visible = naming_visible;

    state->resolving.size--;

    if (poisoned) {
        decl->poisoned = true;
        scope_withdraw(decl->scope, decl->name);
        return;
    }

    declared->fields = fields;
    declared->field_count = resolved;

    layout_struct(state, decl);
}

static void layout_struct(ResolverState *state, StructDecl *decl) {
    if (decl->decl->param_count > 0) {
        return;
    }

    TypeRegistry *registry = state->types;

    type_registry_complete(registry, type_registry_apply(registry, decl->decl, NULL, 0));
}

/* A type parameter has no width until it is substituted, so a generic signature is checked per instantiation.
 */
static const Type *resolve_param_type_in(ResolverState *state, ASTField *param, bool generic) {
    const Type *type = resolve_type_expr(state, param->type_expr, param->span);

    if (generic && type_has_param(type)) {
        return type;
    }

    if (reject_unsized(state, type, param->span, "a parameter")) {
        return resolver_error_type(state);
    }

    return type;
}

static bool func_decl_is_generic(const ASTStmt *stmt) {
    if (stmt->func_decl.type_param_count > 0) {
        return true;
    }

    return stmt->func_decl.owner && stmt->func_decl.owner->kind == TYPE_EXPR_APPLY;
}

/* Enters a scope naming the owner's type arguments and 'Self'; the caller restores the one it saved. */
static void enter_owner_scope(ResolverState *state, TypeExpr *owner, TypeExpr *const *bounds) {
    if (!owner) {
        return;
    }

    Scope *enclosing = state->current_scope;
    Scope *params = scope_create(state->compile_arena, enclosing);

    if (owner->kind == TYPE_EXPR_APPLY) {
        for (size_t i = 0; i < owner->apply.args.size; i++) {
            const TypeExpr *arg = owner->apply.args.data[i];

            if (arg->kind != TYPE_EXPR_NAME) {
                continue;
            }

            bind_type_param(state->types, params, resolver_intern(state, arg->name), i,
                            bound_kind_of(state->types, state->strings, bounds ? bounds[i] : NULL));
        }
    }

    /* Entered before 'Self' resolves, so on a generic owner it names the type applied to them. */
    state->current_scope = params;

    const Type *self = resolve_type_expr(state, owner, (Span){0});

    if (!is_error_type(self)) {
        scope_bind_type_param(params, resolver_names(state)->self, self);
    }
}

/* Continues the owner's numbering, which enter_owner_scope bound at 0..n-1. */
static void bind_own_type_params(ResolverState *state, ASTStmt *stmt, size_t owner_count) {
    for (size_t i = owner_count; i < stmt->func_decl.type_param_count; i++) {
        String *name = resolver_intern(state, stmt->func_decl.type_params[i]);

        if (reject_self_as_name(state, name, stmt->span)) {
            continue;
        }

        if (!bind_type_param(
                state->types, state->current_scope, name, i,
                bound_kind_of(state->types, state->strings, stmt->func_decl.type_param_bounds[i]))) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "duplicate type parameter '%s' on '%s'",
                       name->data, name->data);
        }
    }
}

static size_t owner_type_param_count(const TypeExpr *owner) {
    return owner && owner->kind == TYPE_EXPR_APPLY ? owner->apply.args.size : 0;
}

static void enter_param_bounds(ResolverState *state, ASTStmt *stmt);
static void record_param_bounds(ResolverState *state, FuncDecl *decl);
static void check_abstract_body(ResolverState *state, ASTStmt *stmt);

static void declare_owned_in_scope(ResolverState *state, Scope *declaring, ASTStmt *stmt) {
    bind_own_type_params(state, stmt, owner_type_param_count(stmt->func_decl.owner));

    const Type *owner = resolve_type_expr(state, stmt->func_decl.owner, stmt->span);

    if (is_error_type(owner)) {
        return;
    }

    bool is_host = stmt->func_decl.body == NULL;

    bool owner_is_primitive = type_is_primitive(owner);

    if (owner_is_primitive && !is_host && !state->declares_intrinsics) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function on %s is declared by its core library, which is where its body belongs",
                   type_name(state, owner));
        return;
    }

    /* Only the core library declares one, and only where the compiler has a lowering to match it. */
    if ((stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) && !state->declares_intrinsics) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "an intrinsic is lowered by the compiler, so only its core library declares one");
        return;
    }

    if (stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) {
        const IntrinsicLowering *intrinsic =
            intrinsic_for(state, type_name_of(owner), resolver_intern(state, stmt->func_decl.name));

        if (!intrinsic) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "the compiler lowers no intrinsic '%s' on %s",
                       resolver_intern(state, stmt->func_decl.name)->data, type_name(state, owner));
            return;
        }
    }

    if (owner_is_primitive && !state->declares_intrinsics) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function on %s is declared by the runtime's core library", type_name(state, owner));
        return;
    }

    if (!owner_is_primitive) {
        if (type_kind(owner) != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a function belongs to a struct this module declares, not to %s",
                       type_name(state, owner));
            return;
        }

        /* A scope keys its bindings on a mutable name, though a lookup only ever hashes one. */
        Symbol *bound = type_name_of(owner)
                            ? scope_type_lookup_declaring(declaring, (String *)type_name_of(owner))
                            : NULL;

        if (!bound || bound->kind != SYMBOL_TYPE_DECL || bound->type_decl != type_decl(owner)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot declare a function on '%s', which this module does not declare",
                       type_name(state, owner));
            return;
        }
    }

    const Type *return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    if (!type_has_param(return_type) && reject_unsized(state, return_type, stmt->span, "returned")) {
        return_type = resolver_error_type(state);
    }

    fact_set_return_type(state->facts, stmt, return_type);

    String *name = resolver_intern(state, stmt->func_decl.name);

    FuncDecl *decl = arena_alloc(state->compile_arena, sizeof(FuncDecl));
    const String *decl_module = (stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) ? NULL : state->module_name;
    const String *decl_owner = (stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) ? NULL : type_name_of(owner);

    *decl = (FuncDecl){
        .id = {.module = decl_module, .owner = decl_owner, .name = name},
        .linkage = linkage_of(&stmt->func_decl),
        .modifiers = modifiers_of(&stmt->func_decl),
        .location_type = location_type_of(state, &stmt->func_decl),
        .signature = {.return_type = return_type},
        .type_param_count = stmt->func_decl.type_param_count,
    };

    size_t param_count = stmt->func_decl.params.size;

    if (param_count > 0) {
        decl->signature.params = arena_alloc(state->compile_arena, param_count * sizeof(const Type *));
        decl->signature.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            decl->signature.params[i] = resolve_param_type_in(state, stmt->func_decl.params.data[i],
                                                              stmt->func_decl.type_param_count > 0);
        }
    }

    Function *func = arena_alloc(state->compile_arena, sizeof(Function));
    *func = (Function){
        .decl = decl,
        .signature = decl->signature,
    };

    if (!function_registry_declare_owned(state->functions, owner, func)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a function '%s'",
                   type_name_of(owner)->data, name->data);
        return;
    }

    stmt->func_decl.function = func;

    if (stmt->func_decl.type_param_count > 0) {
        enter_param_bounds(state, stmt);
        record_param_bounds(state, decl);

        if (stmt->func_decl.body) {
            check_abstract_body(state, stmt);
        }
    }
}

static void enter_impl_scope(ResolverState *state, ASTStmt *stmt) {
    enter_owner_scope(state, stmt->impl.type, stmt->impl.param_bounds);
}

static void declare_interface(ResolverState *state, ASTStmt *stmt) {
    String *name = resolver_intern(state, stmt->interface_decl.name);

    if (reject_self_as_name(state, name, stmt->span)) {
        return;
    }

    if (scope_type_lookup_declaring(state->current_scope, name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared", name->data);
        return;
    }

    size_t count = stmt->interface_decl.members.size;
    size_t param_count = stmt->interface_decl.param_count;

    Arena *arena = state->compile_arena;

    /* 'Self' is parameter 0 and the interface's own follow it, so one substitution serves both. */
    Scope *enclosing = state->current_scope;
    Scope *params = scope_create(arena, enclosing);

    TypeRegistry *registry = state->types;

    scope_bind_type(params, resolver_names(state)->self, type_registry_param(registry, 0));

    for (size_t i = 0; i < param_count; i++) {
        scope_bind_type(params, resolver_intern(state, stmt->interface_decl.params[i]),
                        type_registry_param(registry, i + 1));
    }

    state->current_scope = params;

    Function **methods = count > 0 ? arena_alloc(arena, count * sizeof(Function *)) : NULL;

    for (size_t i = 0; i < count; i++) {
        ASTStmt *signature = stmt->interface_decl.members.data[i];

        FuncDecl *decl = arena_alloc(arena, sizeof(FuncDecl));
        *decl = (FuncDecl){
            .id = {.name = resolver_intern(state, signature->func_decl.name)},
            .linkage = LINKAGE_INTERNAL,
            .type_param_count = param_count + 1,
        };

        Function *method = arena_alloc(arena, sizeof(Function));
        *method = (Function){
            .decl = decl,
            .signature = {.return_type =
                              resolve_type_expr(state, signature->func_decl.return_type, signature->span)},
        };

        size_t signature_params = signature->func_decl.params.size;

        if (signature_params > 0) {
            const Type **types = arena_alloc(arena, signature_params * sizeof(const Type *));

            for (size_t p = 0; p < signature_params; p++) {
                types[p] = resolve_param_type_in(state, signature->func_decl.params.data[p], true);
            }

            method->signature.params = types;
            method->signature.param_count = signature_params;
        }

        methods[i] = method;
    }

    state->current_scope = enclosing;

    InterfaceDecl *interface = arena_alloc(arena, sizeof(InterfaceDecl));

    *interface = (InterfaceDecl){
        .id = {.module = state->module_name, .name = name},
        .methods = methods,
        .method_count = count,
        .param_count = param_count,
    };

    scope_bind_interface(resolver_declaring_scope(state), name, interface);
}

/* An interface's method is supplied by the block implementing it, so an inherent one does not answer for it.
 */
static bool block_declares(ResolverState *state, const ASTStmt *stmt, const String *name) {
    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        const ASTStmt *member = stmt->impl.members.data[i];

        if (member && member->kind == STMT_FUNC_DECL &&
            resolver_intern(state, member->func_decl.name) == name) {
            return true;
        }
    }

    return false;
}

static void check_conformance(ResolverState *state, ASTStmt *stmt, const Type *implementor) {
    String *interface_name = resolver_intern(state, stmt->impl.interface_name);

    InterfaceDecl *interface = resolver_lookup_interface(state, interface_name);

    if (!interface) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->impl.interface_span, "unknown interface '%s'",
                   interface_name->data);
        return;
    }

    size_t arg_count = stmt->impl.interface_args.size;

    if (arg_count != interface->param_count) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                   "'%s' takes %zu type argument(s), but %zu were given", interface_name->data,
                   interface->param_count, arg_count);
        return;
    }

    TypeArg args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < arg_count; i++) {
        const Type *argument =
            resolve_type_expr(state, stmt->impl.interface_args.data[i], stmt->impl.interface_span);

        if (is_error_type(argument)) {
            return;
        }

        args[i] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = argument};
    }

    if (!type_registry_declare_conformance(state->types, implementor, interface->id, args, arg_count)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                   "'%s' already implements '%s'", type_name(state, implementor), interface_name->data);
        return;
    }

    for (size_t i = 0; i < interface->method_count; i++) {
        const String *name = interface->methods[i]->decl->id.name;

        Function *supplied = block_declares(state, stmt, name)
                                 ? function_registry_find_owned(state->functions, implementor, name)
                                 : NULL;

        if (!supplied) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' implements '%s', which declares '%s', but supplies no '%s'",
                       type_name(state, implementor), interface_name->data, name->data, name->data);
            continue;
        }

        const Function *required = interface_method_for(state, interface, i, implementor, args, arg_count);

        const Type *expected_return = required->signature.return_type;

        if (expected_return != supplied->signature.return_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' of '%s' returns %s, but '%s' declares it returns %s", name->data,
                       type_name(state, implementor), type_name(state, supplied->signature.return_type),
                       interface_name->data, type_name(state, expected_return));
            continue;
        }

        size_t expected_count = required->signature.param_count;

        if (expected_count != supplied->signature.param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' of '%s' takes %zu parameters, but '%s' declares %zu", name->data,
                       type_name(state, implementor), supplied->signature.param_count, interface_name->data,
                       expected_count);
            continue;
        }

        for (size_t p = 0; p < expected_count; p++) {
            const Type *expected = required->signature.params[p];

            if (expected != supplied->signature.params[p]) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                           "parameter %zu of '%s' is %s, but '%s' declares it %s", p + 1, name->data,
                           type_name(state, supplied->signature.params[p]), interface_name->data,
                           type_name(state, expected));
                break;
            }
        }
    }
}

static void declare_impl(ResolverState *state, ASTStmt *stmt) {
    Scope *enclosing = state->current_scope;

    enter_impl_scope(state, stmt);

    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        ASTStmt *member = stmt->impl.members.data[i];

        if (member && member->kind == STMT_FUNC_DECL) {
            declare_owned_in_scope(state, enclosing, member);

            member->func_decl.declared = true;
        }
    }

    if (stmt->impl.interface_name.length > 0) {
        const Type *implementor = resolve_type_expr(state, stmt->impl.type, stmt->span);

        if (!is_error_type(implementor)) {
            check_conformance(state, stmt, implementor);
        }
    }

    state->current_scope = enclosing;
}

static void resolve_impl(ResolverState *state, ASTStmt *stmt) {
    Scope *enclosing = state->current_scope;

    enter_impl_scope(state, stmt);

    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        resolve_stmt(state, stmt->impl.members.data[i]);
    }

    state->current_scope = enclosing;
}

static void declare_owned(ResolverState *state, ASTStmt *stmt) {
    Scope *enclosing = state->current_scope;

    enter_owner_scope(state, stmt->func_decl.owner, stmt->func_decl.type_param_bounds);

    declare_owned_in_scope(state, enclosing, stmt);

    state->current_scope = enclosing;
}

static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr) {
    StringRef owner_ref, member_ref;

    if (!string_ref_split_colons(expr->var.name, &owner_ref, &member_ref)) {
        return NULL;
    }

    Scope *module_scope = expr->var.owner_type_expr ? NULL : resolver_expr_scope(state, expr->var.name);

    if (module_scope) {
        Symbol *entry = scope_lookup(module_scope, resolver_intern(state, member_ref));

        if (entry && entry->kind == SYMBOL_FUNC) {
            return entry->func;
        }
    }

    const Type *owner;

    if (expr->var.owner_type_expr) {
        owner = resolve_type_expr(state, expr->var.owner_type_expr, expr->span);

        if (is_error_type(owner)) {
            return NULL;
        }
    } else {
        Symbol *symbol =
            resolver_resolve_name(state, state->current_scope, resolver_intern(state, owner_ref));

        if (symbol && symbol->kind == SYMBOL_TYPE_DECL && symbol->type_decl->param_count > 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s), not 0",
                       symbol->type_decl->id.name->data, symbol->type_decl->param_count);
            return NULL;
        }

        owner = symbol_type(state->types, symbol);
    }

    if (!owner) {
        return NULL;
    }

    String *member = resolver_intern(state, member_ref);
    Function *found = function_registry_owned_for(state->functions, owner, member);

    if (!found) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no function '%s'",
                   type_name_of(owner)->data, member->data);

        return NULL;
    }

    /* The ending runs where the value ends, so calling it here would run it twice on that value. The
     * function is still named, so what follows reports nothing further about a name it did resolve. */
    if (found == function_registry_destructor(state->functions, owner)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'destroy' runs where the value ends, so nothing calls it by hand");
    }

    return found;
}

/* Checked once with its parameters abstract, which is the body every instantiation substitutes. */
static void check_abstract_body(ResolverState *state, ASTStmt *stmt) { resolve_func_body(state, stmt); }

/* Each bound names an interface, which the body is checked against before any instantiation. */
static void enter_param_bounds(ResolverState *state, ASTStmt *stmt) {
    for (size_t i = 0; i < GAB_MAX_TYPE_PARAMS; i++) {
        state->param_bounds[i] = (TypeParamBound){.kind = BOUND_NONE};
    }

    for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
        const TypeExpr *bound = stmt->func_decl.type_param_bounds[i];

        BoundKind kind = bound_kind_of(state->types, state->strings, bound);

        if (kind == BOUND_NONE) {
            continue;
        }

        /* A value parameter's bound names its type rather than an interface, so it declares no methods. */
        if (kind == BOUND_VALUE) {
            state->param_bounds[i] = (TypeParamBound){
                .kind = BOUND_VALUE,
                .value = resolve_type_expr(state, (TypeExpr *)bound, stmt->span),
            };
            continue;
        }

        const TypeExpr *named = bound->kind == TYPE_EXPR_APPLY ? bound->apply.base : bound;

        String *name = resolver_intern(state, named->name);
        InterfaceDecl *interface = resolver_lookup_interface(state, name);

        if (!interface) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span,
                       "'%s' bounds a type parameter, so it names an interface", name->data);
            continue;
        }

        size_t arg_count = bound->kind == TYPE_EXPR_APPLY ? bound->apply.args.size : 0;

        if (arg_count != interface->param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'%s' takes %zu type argument(s), but %zu were given", name->data,
                       interface->param_count, arg_count);
            continue;
        }

        state->param_bounds[i] = (TypeParamBound){
            .kind = BOUND_INTERFACE, .interface = {.interface = interface, .arg_count = arg_count}};

        for (size_t a = 0; a < arg_count; a++) {
            state->param_bounds[i].interface.args[a] =
                (TypeArg){.kind = TYPE_ARG_TYPE,
                          .type = resolve_type_expr(state, bound->apply.args.data[a], stmt->span)};
        }
    }
}

/* Kept on the declaration so a call site can judge its arguments without the statement. */
static void record_param_bounds(ResolverState *state, FuncDecl *decl) {
    if (!decl) {
        return;
    }

    Arena *arena = state->compile_arena;

    TypeParamBound *bounds = arena_alloc(arena, GAB_MAX_TYPE_PARAMS * sizeof(TypeParamBound));

    for (size_t i = 0; i < GAB_MAX_TYPE_PARAMS; i++) {
        bounds[i] = state->param_bounds[i];
    }

    decl->type_param_bounds = bounds;
}

static void declare_func(ResolverState *state, ASTStmt *stmt) {
    stmt->func_decl.declared = true;

    if (stmt->func_decl.owner) {
        declare_owned(state, stmt);
        return;
    }

    StringRef func_name = stmt->func_decl.name;

    Scope *enclosing = state->current_scope;

    if (stmt->func_decl.type_param_count > 0) {
        Scope *params = scope_create(state->compile_arena, enclosing);

        for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
            String *param_name = resolver_intern(state, stmt->func_decl.type_params[i]);

            if (reject_self_as_name(state, param_name, stmt->span)) {
                continue;
            }

            if (!bind_type_param(
                    state->types, params, param_name, i,
                    bound_kind_of(state->types, state->strings, stmt->func_decl.type_param_bounds[i]))) {
                diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span,
                           "duplicate type parameter '%s' on '%s'", param_name->data, param_name->data);
            }
        }

        state->current_scope = params;

        enter_param_bounds(state, stmt);
    }

    const Type *func_return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    fact_set_return_type(state->facts, stmt, func_return_type);

    String *declared_name = resolver_intern(state, func_name);

    if (reject_self_as_name(state, declared_name, stmt->span)) {
        state->current_scope = enclosing;
        return;
    }

    Symbol *declared = scope_decl_func_against(resolver_declaring_scope(state), state->current_scope,
                                               declared_name, func_return_type);

    if (!declared) {
        char *name = string_ref_to_cstr(func_name);
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                   name);
        free(name);
    }

    Function *func = declared ? declared->func : NULL;

    stmt->func_decl.function = func;

    FuncDecl *decl = func ? (FuncDecl *)func->decl : NULL;

    if (decl) {
        if (stmt->func_decl.type_param_count > 0) {
            record_param_bounds(state, decl);
        }

        decl->linkage = linkage_of(&stmt->func_decl);
        decl->modifiers = modifiers_of(&stmt->func_decl);
        decl->location_type = location_type_of(state, &stmt->func_decl);

        decl->id.module = state->module_name;

        /* A C body links to the name as spelled, which is the name its id carries; the module still
         * qualifies the id, since two modules may each declare the same foreign function. */
        if (decl->linkage == LINKAGE_C) {
            decl->id.name = resolver_intern(state, func_name);
        }
    }

    size_t param_count = stmt->func_decl.params.size;

    if (decl && param_count > 0) {
        decl->signature.params = arena_alloc(state->compile_arena, param_count * sizeof(const Type *));
        decl->signature.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            decl->signature.params[i] =
                resolve_param_type_in(state, param, stmt->func_decl.type_param_count > 0);
        }

        func->signature = decl->signature;
    }

    if (decl && stmt->func_decl.type_param_count > 0) {
        decl->type_param_count = stmt->func_decl.type_param_count;

        if (stmt->func_decl.body) {
            check_abstract_body(state, stmt);
        }
    }

    state->current_scope = enclosing;
}

static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    size_t errors_before = diagnostics_count(state->diagnostics);

    const Function *signature = stmt->func_decl.function;

    resolver_enter_scope(state);

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);

        /* The signature resolved this already, so resolving it again would report its errors twice. */
        const Type *param_type = signature && i < signature->signature.param_count
                                     ? signature->signature.params[i]
                                     : resolve_type_expr(state, param->type_expr, param->span);

        if (reject_self_as_name(state, param_name, param->span)) {
            continue;
        }

        Symbol *binding = scope_decl_var(state->current_scope, param_name, param_type);

        if (!binding) {
            char *name = string_ref_to_cstr(param->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, param->span, "duplicate parameter '%s'", name);
            free(name);
            continue;
        }

        param->binding = binding;
    }

    FuncContext previous_context = state->func_context;

    state->func_context.return_type = fact_return_type_of(state->facts, stmt);
    state->func_context.is_caller = (stmt->func_decl.syntax & FUNC_SYN_CALLER) != 0;

    resolve_stmt(state, stmt->func_decl.body);

    state->func_context = previous_context;

    if (diagnostics_count(state->diagnostics) == errors_before) {
        size_t param_count = stmt->func_decl.params.size;
        Symbol **params = arena_alloc(state->compile_arena, (param_count + 1) * sizeof(Symbol *));
        size_t count = 0;

        for (size_t i = 0; i < param_count; i++) {
            params[count++] = stmt->func_decl.params.data[i]->binding;
        }

        PendingBody body = {.registry = state->types,
                            .body = stmt->func_decl.body,
                            .param_fields = &stmt->func_decl.params,
                            .function = stmt->func_decl.function};

        pending_body_list_add(&state->work->bodies, body);
    }

    resolver_exit_scope(state);
}

static void resolve_stmt(ResolverState *state, ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR: {
        resolve_expr(state, stmt->expr.value, NULL);
        break;
    }
    case STMT_VAR_DECL: {
        const Type *declared =
            stmt->var_decl.type_expr ? resolve_type_expr(state, stmt->var_decl.type_expr, stmt->span) : NULL;

        if (reject_unsized(state, declared, stmt->span, "a variable")) {
            declared = resolver_error_type(state);
        }

        resolve_expr(state, stmt->var_decl.initializer, declared);

        if (!stmt->var_decl.initializer && declared && !is_error_type(declared) &&
            type_kind(declared) == TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a %s must be given its fields where it is declared, as '%s { ... }'",
                       type_name(state, declared), type_name(state, declared));
            declared = resolver_error_type(state);
        }

        const Type *type;
        if (stmt->var_decl.type_expr) {
            const Type *decl_type = declared;

            if (stmt->var_decl.initializer) {
                const Type *init_type = fact_type_of(state->facts, stmt->var_decl.initializer);

                if (!is_error_type(decl_type) && !is_error_type(init_type) &&
                    !type_accepts(state->types, decl_type, init_type)) {
                    if (type_registry_deref_of(state->types, decl_type) && type_is_str_ref(init_type)) {
                        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                                   "a %s borrows characters it does not own, so a 'String' cannot take it; "
                                   "write 'str', or '.to_owned()' to copy them",
                                   type_name(state, init_type));
                        decl_type = resolver_error_type(state);
                        break;
                    }

                    diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                               "cannot initialize a variable of type %s with a value of type %s",
                               type_name(state, decl_type), type_name(state, init_type));
                    decl_type = resolver_error_type(state);
                } else if (!borrow_into(state, stmt->var_decl.initializer, decl_type,
                                        stmt->var_decl.initializer->span)) {
                    decl_type = resolver_error_type(state);
                }
            }

            type = decl_type;
        } else if (stmt->var_decl.initializer) {
            type = fact_type_of(state->facts, stmt->var_decl.initializer);
        } else {
            type = resolver_error_type(state);
        }

        if (state->current_scope->kind != SCOPE_LOCAL && type_registry_owns(state->types, type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a top-level variable may not own, since no scope closes to free it; %s belongs in a "
                       "function body",
                       type_name(state, type));
            type = resolver_error_type(state);
        }

        Symbol *var = reject_self_as_name(state, resolver_intern(state, stmt->var_decl.name), stmt->span)
                          ? NULL
                          : scope_decl_var(resolver_declaring_scope(state),
                                           resolver_intern(state, stmt->var_decl.name), type);

        if (!var) {
            char *name = string_ref_to_cstr(stmt->var_decl.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                       name);
            free(name);
            break;
        }

        mark_implicit_move(state, stmt->var_decl.initializer, type, stmt->span);

        stmt->var_decl.binding = var;
        break;
    }
    case STMT_FUNC_DECL: {
        if (!stmt->func_decl.declared) {
            declare_func(state, stmt);
        }

        if (stmt->func_decl.body && func_decl_is_generic(stmt)) {
            break;
        }

        if (stmt->func_decl.body) {
            resolve_func_body(state, stmt);
        }
        break;
    }
    case STMT_INTERFACE_DECL:
        break;
    case STMT_IMPL: {
        resolve_impl(state, stmt);
        break;
    }
    case STMT_STRUCT_DECL: {
        if (!stmt->struct_decl.declared) {
            StructDecl *decl = declare_struct(state, stmt);

            if (decl) {
                resolve_struct_fields(state, decl);
            }
        }
        break;
    }
    case STMT_ASSIGN: {
        const Type *target_type = resolve_expr(state, stmt->assign.target, NULL);

        const Type *value_type = resolve_expr(state, stmt->assign.value, target_type);

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !type_accepts(state->types, target_type, value_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot assign a value of type %s to a target of type %s",
                       type_name(state, value_type), type_name(state, target_type));
            break;
        }

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !borrow_into(state, stmt->assign.value, target_type, stmt->span)) {
            break;
        }

        if (stmt->assign.target->kind == EXPR_FIELD || stmt->assign.target->kind == EXPR_DEREF) {
            mark_implicit_move(state, stmt->assign.value, target_type, stmt->span);
            break;
        }

        Symbol *target = fact_use_of(state->facts, stmt->assign.target);

        if (target && target->kind == SYMBOL_VAR) {
            if (stmt->assign.value->kind == EXPR_VARIABLE &&
                fact_use_of(state->facts, stmt->assign.value) == target &&
                !type_registry_copies(state->types, target_type)) {
                diag_error(state->diagnostics, GAB_ERR_LIFETIME, stmt->span,
                           "'%s' owns what it holds, so it cannot be assigned to itself",
                           type_name_of(target_type) ? type_name_of(target_type)->data : "a value");
                break;
            }

            mark_implicit_move(state, stmt->assign.value, target_type, stmt->span);
        }
        break;
    }
    case STMT_COMPOUND_ASSIGN: {
        const Type *target_type = resolve_expr(state, stmt->compound_assign.target, NULL);
        const Type *value_type = resolve_expr(state, stmt->compound_assign.value, NULL);

        if (is_error_type(target_type) || is_error_type(value_type)) {
            break;
        }

        const char *op_name = bin_op_name(stmt->compound_assign.op);

        if (target_type != value_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "cannot apply '%s=' to %s and %s",
                       op_name, type_name(state, target_type), type_name(state, value_type));
            break;
        }

        if (!bin_op_accepts(state, stmt->compound_assign.op, target_type, stmt->span)) {
            break;
        }

        assert(!bin_op_yields_bool(stmt->compound_assign.op) &&
               "a compound assignment must yield its target's type");

        break;
    }
    case STMT_IF: {
        const Type *condition_type = resolve_expr(state, stmt->ifstmt.condition, NULL);

        if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->ifstmt.condition->span,
                       "'if' requires a boolean condition, found %s", type_name(state, condition_type));
        }

        resolve_stmt(state, stmt->ifstmt.then_block);
        resolve_stmt(state, stmt->ifstmt.else_block);
        break;
    }
    case STMT_FOR: {
        Scope *outer_scope = state->current_scope;

        resolver_enter_scope(state);
        stmt->forstmt.scope = state->current_scope;

        resolve_stmt(state, stmt->forstmt.init);

        if (stmt->forstmt.condition) {
            const Type *condition_type = resolve_expr(state, stmt->forstmt.condition, NULL);

            if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->forstmt.condition->span,
                           "'for' requires a boolean condition, found %s", type_name(state, condition_type));
            }
        }

        state->func_context.loop_depth++;
        resolve_stmt(state, stmt->forstmt.body);
        resolve_stmt(state, stmt->forstmt.post);
        state->func_context.loop_depth--;

        state->current_scope = outer_scope;
        break;
    }
    case STMT_JUMP: {
        if (state->func_context.loop_depth == 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "'%s' is only valid inside a loop",
                       stmt->jump.is_break ? "break" : "continue");
        }

        break;
    }
    case STMT_BLOCK: {
        Scope *outer = state->current_scope;

        resolver_enter_scope(state);
        stmt->block.scope = state->current_scope;

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            resolve_stmt(state, stmt->block.list.data[i]);
        }

        state->current_scope = outer;
        break;
    }
    case STMT_RETURN: {
        resolve_expr(state, stmt->ret.result, state->func_context.return_type);

        const Type *expected = state->func_context.return_type;
        const Type *actual = stmt->ret.result ? fact_type_of(state->facts, stmt->ret.result) : NULL;

        bool poisoned =
            (expected && type_kind(expected) == TYPE_ERROR) || (actual && type_kind(actual) == TYPE_ERROR);

        bool accepted =
            actual && expected ? type_accepts(state->types, expected, actual) : actual == expected;

        if (!poisoned && !accepted) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "returns %s, but %s was declared",
                       type_name(state, actual), type_name(state, expected));
            break;
        }

        if (!poisoned && accepted && actual) {
            /* Returning gives the value away, so what it names is moved out rather than dropped here. */
            mark_implicit_move(state, stmt->ret.result, expected, stmt->span);

            borrow_into(state, stmt->ret.result, expected, stmt->span);
        }

        break;
    }
    }
}

bool resolve_module(const Resolver *resolver, ASTModule *module, Scope *into, ModulePrivileges privileges,
                    ResolvedModule **out) {
    Arena *compile_arena = resolver->arena;
    Diagnostics *diagnostics = resolver->diagnostics;

    Scope *module_scope = into;

    ResolvedModule *resolved = arena_alloc(compile_arena, sizeof(ResolvedModule));

    resolved->module = module;

    resolved->declared = arena_alloc(compile_arena, sizeof(Module));

    *resolved->declared = (Module){
        .name = module->name.data ? string_from_ref(resolver->strings, module->name) : NULL,
        .scope = module_scope,
    };
    resolved->work = pending_bodies_create(compile_arena);
    resolved->registry = resolver->types;
    resolved->functions = resolver->functions;

    facts_init(&resolved->facts, compile_arena);

    ResolverState state = {
        .compile_arena = compile_arena,
        .types = resolver->types,
        .functions = resolver->functions,
        .strings = resolver->strings,
        .current_scope = module_scope,
        .module_scope = module_scope,
        .file = module->files.size ? module->files.data[0] : ast_file_create(compile_arena),
        .module_name = module->name.data ? string_from_ref(resolver->strings, module->name) : NULL,
        .declares_intrinsics = privileges.intrinsics,
        .func_context =
            {
                .return_type = NULL,
            },
        .struct_decls = struct_decl_list_create(arena_allocator(compile_arena)),
        .facts = &resolved->facts,
        .work = &resolved->work,
        .resolving = struct_decl_list_create(arena_allocator(compile_arena)),
        .diagnostics = diagnostics,
    };

    size_t errors_before = diagnostics_count(diagnostics);

    /* A scope for each file, held for every pass: what a file imports is bound in it, so a name one
     * file reaches is not a name its siblings do. */
    Scope **file_scopes = arena_alloc(compile_arena, module->files.size * sizeof(Scope *));

    Visible *visible = arena_alloc(compile_arena, module->files.size * sizeof(Visible));

    for (size_t f = 0; f < module->files.size; f++) {
        file_scopes[f] = arena_alloc(compile_arena, sizeof(Scope));
        scope_init_kind(file_scopes[f], compile_arena, module_scope, SCOPE_FILE);

        /* An import binds the module in this file, so what it declares is reached by an ordinary
         * lookup rather than by asking which modules this compilation happens to have read. */
        const ASTImportList *imports = &module->files.data[f]->imports;

        visible[f] = (Visible){
            .modules = arena_alloc(compile_arena, (imports->size + 1) * sizeof(Module *)),
        };

        for (size_t i = 0; i < imports->size; i++) {
            String *name = string_from_ref(resolver->strings, imports->data[i].name);

            Module **imported = resolver->modules ? module_map_lookup(resolver->modules, name) : NULL;

            if (!imported) {
                continue;
            }

            if (!scope_bind_module(file_scopes[f], name, *imported)) {
                diag_error(diagnostics, GAB_ERR_NAME, imports->data[i].span,
                           "'%s' is already declared in this file", name->data);
            }

            visible[f].modules[visible[f].count++] = *imported;
        }

        /* Last, so a name the file imports is the one it means where the core declares it too. */
        if (resolver->core) {
            visible[f].modules[visible[f].count++] = resolver->core;
        }
    }

    /* Every file declares before any file resolves, so a declaration is visible across the module
     * however the files were ordered. */
    for (size_t f = 0; f < module->files.size; f++) {
        state.file = module->files.data[f];
        state.visible = &visible[f];
        state.current_scope = file_scopes[f];

        for (size_t i = 0; i < state.file->statements.size; i++) {
            ASTStmt *stmt = state.file->statements.data[i];

            state.declaring = module_scope;

            if (stmt && stmt->kind == STMT_INTERFACE_DECL) {
                declare_interface(&state, stmt);
            }

            if (stmt && stmt->kind == STMT_STRUCT_DECL) {
                declare_struct(&state, stmt);
            }
        }
    }

    for (size_t i = 0; i < state.struct_decls.size; i++) {
        resolve_struct_fields(&state, state.struct_decls.data[i]);
    }

    for (size_t f = 0; f < module->files.size; f++) {
        state.file = module->files.data[f];
        state.visible = &visible[f];
        state.current_scope = file_scopes[f];

        for (size_t i = 0; i < state.file->statements.size; i++) {
            ASTStmt *stmt = state.file->statements.data[i];

            state.declaring = module_scope;

            if (stmt && stmt->kind == STMT_FUNC_DECL) {
                declare_func(&state, stmt);
            }

            if (stmt && stmt->kind == STMT_IMPL) {
                declare_impl(&state, stmt);
            }
        }
    }

    for (size_t f = 0; f < module->files.size; f++) {
        state.file = module->files.data[f];
        state.visible = &visible[f];
        state.current_scope = file_scopes[f];

        for (size_t i = 0; i < state.file->statements.size; i++) {
            state.declaring = module_scope;

            resolve_stmt(&state, state.file->statements.data[i]);
        }
    }

    /* What a script runs is a body like any other, gathered from the statements the unit holds so it
     * lowers and emits the same way. A declaration is not something it runs, so it stays behind. */
    ASTStmtList top_level = ast_stmt_list_create(arena_allocator(state.compile_arena));

    for (size_t f = 0; f < module->files.size; f++) {
        const ASTFile *file = module->files.data[f];

        for (size_t i = 0; i < file->statements.size; i++) {
            ASTStmt *stmt = file->statements.data[i];

            if (!stmt || stmt->kind == STMT_FUNC_DECL || stmt->kind == STMT_STRUCT_DECL ||
                stmt->kind == STMT_INTERFACE_DECL || stmt->kind == STMT_IMPL) {
                continue;
            }

            ast_stmt_list_add(&top_level, stmt);
        }
    }

    if (top_level.size > 0 && diagnostics_count(diagnostics) == 0) {
        ASTStmt *body = ast_block_stmt_create(state.compile_arena, (Span){0}, top_level);

        pending_body_list_add(
            &state.work->bodies,
            (PendingBody){.registry = state.types, .body = body, .param_fields = NULL, .function = NULL});
    }

    struct_decl_list_free(&state.struct_decls);
    struct_decl_list_free(&state.resolving);

    *out = resolved;

    return diagnostics_count(diagnostics) == errors_before;
}
