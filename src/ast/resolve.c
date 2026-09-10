#include "ast/resolve.h"

#include "ast/check.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const KnownNames *resolver_names(ResolverState *state) { return type_registry_names(state->global->types); }

bool reject_self_as_name(ResolverState *state, String *name, Span span) {
    if (name != resolver_names(state)->self) {
        return false;
    }

    diag_error(state->global->diagnostics, GAB_ERR_NAME, span,
               "'Self' names the type an 'impl' block is for, so it cannot be declared");

    return true;
}

static Linkage linkage_of(const ASTFuncDecl *decl) {
    if (decl->syntax & FUNC_SYN_FOREIGN) {
        return LINKAGE_C;
    }

    if (decl->syntax & FUNC_SYN_INTRINSIC) {
        return LINKAGE_INTERNAL;
    }

    return decl->body ? LINKAGE_INTERNAL : LINKAGE_GAB;
}

static bool reject_generic_without_body(ResolverState *state, const ASTStmt *stmt, const String *name) {
    if (stmt->func_decl.type_param_count == 0 || linkage_of(&stmt->func_decl) == LINKAGE_INTERNAL) {
        return false;
    }

    if (stmt->func_decl.syntax & FUNC_SYN_FOREIGN) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "C declares no generic, so '%s' cannot take type parameters", name->data);
    } else {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a generic is instantiated by whoever names it, so '%s' must carry a body", name->data);
    }

    return true;
}

static Scope *resolver_declaring_scope(ResolverState *state) {
    return state->env.declaring ? state->env.declaring : state->env.scope;
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

Scope *resolver_type_expr_scope(ResolverState *state, const TypeExpr *expr) {
    return expr->qualifier ? resolver_qualifier_scope(state, expr->qualifier) : state->env.scope;
}

String *resolver_type_expr_member(ResolverState *state, const TypeExpr *expr) {
    (void)state;

    return expr->name->name;
}

Scope *resolver_qualifier_scope(ResolverState *state, const ASTIdent *qualifier) {
    if (qualifier->name == state->module_name) {
        return state->module_scope;
    }

    Symbol *bound = scope_lookup(state->env.scope, qualifier->name);

    return bound && bound->kind == SYMBOL_MODULE ? bound->module->scope : NULL;
}

static Symbol *file_lookup(const ResolverState *state, String *name) {
    if (!state->env.visible) {
        return NULL;
    }

    for (size_t i = 0; i < state->env.visible->count; i++) {
        Symbol *found = scope_lookup(state->env.visible->modules[i]->scope, name);

        if (found) {
            return found;
        }
    }

    return NULL;
}

Symbol *resolver_resolve_name(ResolverState *state, Scope *scope, String *name) {
    Symbol *found = scope ? scope_lookup(scope, name) : NULL;

    if (found) {
        return found;
    }

    return file_lookup(state, name);
}

static const Type *resolver_location_type(ResolverState *state) {
    String *name = string_from_cstr(state->global->strings, GAB_LOCATION_TYPE);

    Symbol *found = resolver_resolve_name(state, state->env.scope, name);

    return found ? symbol_type(state->global->types, found) : NULL;
}

static InterfaceDecl *interface_of(const Symbol *symbol) {
    return symbol && symbol->kind == SYMBOL_INTERFACE ? symbol->interface : NULL;
}

static InterfaceDecl *resolver_lookup_interface(ResolverState *state, String *name) {
    InterfaceDecl *found = interface_of(scope_lookup(state->env.scope, name));

    if (found) {
        return found;
    }

    return interface_of(file_lookup(state, name));
}

const char *type_name(ResolverState *state, const Type *type) {
    if (!type) {
        return "none";
    }

    if (type_kind(type) == TYPE_ARRAY) {
        const char *element = type_name(state, type_array_element(type));
        size_t length = strlen(element) + 32;
        char *out = arena_alloc(state->global->arena, length);

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
        char *out = arena_alloc(state->global->arena, length);

        snprintf(out, length, "slice<%s>", element);

        return out;
    }

    if (type_kind(type) == TYPE_RAW) {
        const char *element = type_name(state, type_pointee(type));
        size_t length = strlen(element) + 16;
        char *out = arena_alloc(state->global->arena, length);

        snprintf(out, length, "raw<%s>", element);

        return out;
    }

    if (type_name_of(type)) {
        return type_name_of(type)->data;
    }

    if (type_kind(type) == TYPE_PARAM) {
        return "a type parameter";
    }

    const char *inner = type_name(state, type_pointee(type));
    const char *prefix = type_kind(type) == TYPE_REF ? "&" : "*";
    size_t length = strlen(prefix) + strlen(inner) + 1;
    char *out = arena_alloc(state->global->arena, length);

    snprintf(out, length, "%s%s", prefix, inner);

    return out;
}

static void resolver_enter_scope(ResolverState *state) {
    state->env.scope = scope_create(state->global->arena, state->env.scope);

    state->env.declaring = NULL;
}

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

static size_t written_arg_count(const ASTExpr *expr) {
    return expr->kind == EXPR_INDEX ? 1 : expr->call.args.size;
}

static ASTExpr *written_arg(const ASTExpr *expr, size_t i) {
    return expr->kind == EXPR_INDEX ? expr->index.index : expr->call.args.data[i];
}

static void check_call_arg(ResolverState *state, ASTExpr *arg, const Type *param_type, size_t written) {
    if (is_error_type(fact_type_of(state->facts, arg)) || is_error_type(param_type)) {
        return;
    }

    if (!type_accepts(state->global->types, param_type, fact_adjusted_type_of(state->facts, arg))) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, arg->span,
                   "argument %zu is %s, but %s was declared", written,
                   type_name(state, fact_adjusted_type_of(state->facts, arg)), type_name(state, param_type));
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

typedef struct {
    size_t derefs;

    bool address_of;

    int32_t unsize_length;
} ReceiverAdjustment;

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

static void resolve_as_method_call(ResolverState *state, ASTExpr *expr, Function *method,
                                   ReceiverAdjustment adjustment) {
    ASTExpr *receiver = expr->call.target->field.target;

    record_receiver_adjustment(state, receiver, method, adjustment);

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
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
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
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                           "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
                return false;
            }

            Symbol *addressed = fact_root_local(state->facts, receiver);

            if (addressed) {
                addressed->pinned = true;
            }

            const Type *array = receiver_base_type(at);

            *out = (ReceiverAdjustment){.derefs = derefs,
                                        .unsize_length =
                                            type_array_length_is_known(array) ? type_array_length(array) : 1};
            return true;
        }

        if (reads_as_a_view(state->global->types, declared, at) || lends_by_pointer(declared, at)) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        if (!type_is_indirect(at)) {
            break;
        }

        at = type_pointee(at);
    }

    diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "cannot call '%s' on %s", name->data,
               type_name(state, actual));
    return false;
}

static const Type *resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected);
static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr);

static void resolve_func_body(ResolverState *state, ASTStmt *stmt);

static bool infer_type_args(const Type *declared, const Type *actual, TypeArg *args, size_t owed) {
    if (!declared || !actual || !type_has_param(declared)) {
        return true;
    }

    if (type_kind(declared) == TYPE_PARAM) {
        size_t index = type_param_index(declared);

        if (index < owed && !type_arg_is_set(args[index])) {
            args[index] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = actual};
        }

        return true;
    }

    if (type_kind(declared) == TYPE_REF) {
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

        if (want.constant.kind == CONST_PARAM && want.constant.param < owed &&
            !type_arg_is_set(args[want.constant.param])) {
            args[want.constant.param] = got;
        }
    }

    return true;
}

static bool infer_call_args(ResolverState *state, ASTExpr *expr, Function *function, TypeArg *args,
                            size_t fixed, size_t self_params) {
    size_t owed = function->decl->type_param_count;

    size_t written = written_arg_count(expr);

    if (written + self_params != function->signature.param_count) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                   "expected %zu argument(s), found %zu", function->signature.param_count - self_params,
                   written);
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "no argument names every type parameter of '%s', so each is written",
                       function->decl->id.name->data);
            return false;
        }
    }

    return true;
}

static bool take_written_type_args(ResolverState *state, ASTExpr *expr, Function *generic,
                                   const TypeExpr *supplied, TypeArg *args) {
    size_t owed = generic->decl->type_param_count;

    if (supplied->kind != TYPE_EXPR_APPLY || supplied->apply.args.size != owed) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s)",
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

static size_t take_receiver_type_args(const Type *receiver, TypeArg *args) {
    size_t fixed = type_arg_count(receiver);

    for (size_t i = 0; i < fixed; i++) {
        args[i] = type_args(receiver)[i];
    }

    return fixed;
}

static bool check_bounds_satisfied(ResolverState *state, ASTExpr *expr, const Function *generic,
                                   const TypeArg *args, size_t owed) {
    const TypeParamBound *bounds = generic->decl->type_param_bounds;

    if (!bounds) {
        return true;
    }

    TypeRegistry *registry = state->global->types;

    for (size_t i = 0; i < owed && i < GAB_MAX_TYPE_PARAMS; i++) {
        if (bounds[i].kind != BOUND_INTERFACE || !type_arg_is_set(args[i]) || args[i].kind != TYPE_ARG_TYPE ||
            type_kind(args[i].type) == TYPE_PARAM) {
            continue;
        }

        const InterfaceRef *bound = &bounds[i].interface;

        if (!type_registry_conforms_at_any(registry, args[i].type, bound->interface->id)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "%s does not implement '%s'",
                       type_name(state, args[i].type), bound->interface->id.name->data);
            return false;
        }

        if (!type_registry_conforms(registry, args[i].type, bound->interface->id, bound->args,
                                    bound->arg_count)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s implements '%s' at another type", type_name(state, args[i].type),
                       bound->interface->id.name->data);
            return false;
        }
    }

    return true;
}

static Function *specialize(ResolverState *state, ASTExpr *expr, Function *generic, TypeArg *args,
                            size_t fixed, size_t self_params) {
    size_t owed = generic->decl->type_param_count;

    if (owed <= fixed) {
        if (type_args_are_concrete(generic->type_args, generic->type_arg_count)) {
            pending_bodies_instantiate(state->work, generic, state->global->diagnostics);
        }

        return generic;
    }

    if (!infer_call_args(state, expr, generic, args, fixed, self_params)) {
        return NULL;
    }

    if (!check_bounds_satisfied(state, expr, generic, args, owed)) {
        return NULL;
    }

    Function *specialized = function_registry_instance(state->global->functions, generic->decl, args, owed);

    if (!type_args_are_concrete(args, owed)) {
        return specialized;
    }

    pending_bodies_instantiate(state->work, specialized, state->global->diagnostics);

    return specialized;
}

static Function *specialize_method_call(ResolverState *state, ASTExpr *expr, Function *method,
                                        const Type *receiver) {
    TypeArg args[GAB_MAX_TYPE_PARAMS] = {0};

    return specialize(state, expr, method, args, take_receiver_type_args(receiver, args), 1);
}

static const TypeExpr *written_owner_type(const ASTExpr *target) {
    if (target->kind == EXPR_QUALIFIED) {
        return target->qualified.owner_type_expr;
    }

    return target->kind == EXPR_NAME ? target->name.owner_type_expr : NULL;
}

static Function *specialize_call(ResolverState *state, ASTExpr *expr, Function *generic) {
    const TypeExpr *supplied = written_owner_type(expr->call.target);

    TypeArg args[GAB_MAX_TYPE_PARAMS] = {0};

    if (supplied && !take_written_type_args(state, expr, generic, supplied, args)) {
        return NULL;
    }

    return specialize(state, expr, generic, args, 0, 0);
}

static const Type *resolve_param_type_in(ResolverState *state, ASTField *param, bool generic);

static Function *interface_method_for(ResolverState *state, const InterfaceDecl *interface, size_t index,
                                      const Type *implementor, const TypeArg *args, size_t arg_count) {
    const FuncDecl *signature = interface->methods[index];

    TypeArg substitutions[GAB_MAX_TYPE_PARAMS];
    substitutions[0] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = implementor};

    for (size_t i = 0; i < arg_count && i + 1 < GAB_MAX_TYPE_PARAMS; i++) {
        substitutions[i + 1] = args[i];
    }

    Arena *arena = state->global->arena;

    FuncDecl *decl = arena_alloc(arena, sizeof(FuncDecl));
    *decl = *signature;
    decl->type_param_count = 0;

    Function *method = arena_alloc(arena, sizeof(Function));
    *method = (Function){
        .decl = decl,
        .signature = func_signature_instantiate(state->global->types, arena, &signature->signature,
                                                substitutions, arg_count + 1),

        .bound_self = implementor,
    };

    return method;
}

static Function *bound_method(ResolverState *state, const Type *base, String *name, Span span) {
    (void)span;

    if (!base || type_kind(base) != TYPE_PARAM) {
        return NULL;
    }

    size_t index = type_param_index(base);

    if (state->env.param_bounds[index].kind != BOUND_INTERFACE) {
        return NULL;
    }

    const InterfaceRef *bound = &state->env.param_bounds[index].interface;

    for (size_t i = 0; i < bound->interface->method_count; i++) {
        if (bound->interface->methods[i]->id.name != name) {
            continue;
        }

        return interface_method_for(state, bound->interface, i, base, bound->args, bound->arg_count);
    }

    return NULL;
}

static Function *find_method(ResolverState *state, const Type *receiver, String *name, Span span,
                             const Type **out_base) {
    const Type *base = NULL;

    Function *method =
        find_method_on_chain(state->global->types, state->global->functions, receiver, name, &base);

    if (!method) {
        method = bound_method(state, base, name, span);
    }

    if (out_base) {
        *out_base = base;
    }

    return method;
}

static const Type *resolve_index_through_interface(ResolverState *state, ASTExpr *expr) {
    Span span = expr->span;

    ASTExpr *target = expr->index.target;
    const Type *target_type = fact_type_of(state->facts, target);

    String *name = string_from_cstr(state->global->strings, "index");

    const Type *base = NULL;
    Function *method = find_method(state, target_type, name, span, &base);

    if (!method) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "%s is indexed with '[]' by implementing 'Index'", type_name(state, target_type));
        return resolver_error_type(state);
    }

    method = specialize_method_call(state, expr, method, base);

    if (!method) {
        return resolver_error_type(state);
    }

    if (method->signature.param_count != 2) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "'index' of %s takes %zu argument(s), not one", type_name(state, target_type),
                   method->signature.param_count);
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
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "'index' of %s returns %s rather than lending an element", type_name(state, target_type),
                   type_name(state, lent));
        return resolver_error_type(state);
    }

    return type_pointee(lent);
}

static const IntrinsicLowering *intrinsic_for(ResolverState *state, const String *owner, const String *name) {
    return type_registry_intrinsic(state->global->types, owner, name);
}

static void resolve_method_call(ResolverState *state, ASTExpr *expr) {
    ASTExpr *receiver = expr->call.target->field.target;
    String *method_name = expr->call.target->field.name->name;

    const Type *receiver_type = resolve_expr(state, receiver, NULL);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i], NULL);
    }

    if (is_error_type(receiver_type)) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    const Type *base = NULL;
    Function *method = find_method(state, receiver_type, method_name, expr->span, &base);

    if (!method) {
        if (base && type_kind(base) == TYPE_PARAM &&
            state->env.param_bounds[type_param_index(base)].kind != BOUND_INTERFACE) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span,
                       "a type parameter has the methods its bound declares, and this one has no bound");
            fact_set_type(state->facts, expr, resolver_error_type(state));
            return;
        }

        diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    if (method == function_registry_destructor(state->global->functions, base)) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
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
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
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
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                   "expected %zu argument(s), found %zu", declared_params, expr->call.args.size);
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return;
    }

    resolve_as_method_call(state, expr, method, adjustment);

    check_call_args(state, &expr->call.args, method->signature.params + 1);

    if (method->decl->modifiers & FUNC_MOD_INTRINSIC) {
        fact_set_type(state->facts, expr,
                      type_registry_substitute(state->global->types, method->signature.return_type,
                                               type_args(base), type_arg_count(base)));
        return;
    }

    fact_set_type(state->facts, expr, method->signature.return_type);
}

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

static bool bin_op_accepts(ResolverState *state, BinOp op, const Type *type, Span span) {
    const char *op_name = bin_op_name(op);

    switch (op) {
    case BIN_OP_ADD:
    case BIN_OP_SUB:
    case BIN_OP_MUL:
    case BIN_OP_DIV:
        if (!is_numeric_type(type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                       "'%s' requires a numeric type, found %s", op_name, type_name(state, type));
            return false;
        }

        return true;

    case BIN_OP_MOD:
        if (!is_integer_type(type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                       "'%s' requires an integer type, found %s", op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_EQUAL:
    case BIN_OP_NEQUAL:
        if (!is_comparable_type(state->global->types, type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, span, "'%s' is not supported for %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_LESS:
    case BIN_OP_GREATER:
    case BIN_OP_LEQUAL:
    case BIN_OP_GEQUAL:
        if (!is_ordered_type(type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                       "'%s' requires an ordered type, found %s", op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_AND:
    case BIN_OP_OR:
        if (!is_boolean_type(type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                       "'%s' requires a boolean type, found %s", op_name, type_name(state, type));
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
    Symbol *symbol = scope_lookup(state->env.scope, expr->call.target->name.name->name);

    const Type *target = symbol_type(state->global->types, symbol);

    if (expr->call.target->name.owner_type_expr) {
        bool names_a_type = (symbol && (symbol->kind == SYMBOL_TYPE || symbol->kind == SYMBOL_TYPE_DECL)) ||
                            expr->call.target->name.name->name == resolver_names(state)->raw;

        if (!names_a_type) {
            return false;
        }

        target = resolve_type_expr(state, expr->call.target->name.owner_type_expr, expr->span);
    }

    if (!target || is_error_type(target)) {
        return false;
    }

    ASTExprList args = expr->call.args;
    ASTExpr *operand = args.size == 1 ? args.data[0] : NULL;

    fact_set_call_kind(state->facts, expr, CALL_CONVERSION);

    if (!operand) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                   "a conversion to %s takes one operand", type_name(state, target));
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return true;
    }

    const Type *from = resolve_expr(state, operand, NULL);

    if (is_error_type(from)) {
        fact_set_type(state->facts, expr, resolver_error_type(state));
        return true;
    }

    bool reads_as_a_run = type_kind(target) == TYPE_RAW && type_kind(from) == TYPE_RAW;

    if (!reads_as_a_run && (!is_numeric_type(target) || !is_numeric_type(from))) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "cannot convert %s to %s",
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

        const Type *right_type = resolve_expr(state, expr->bin_op.right, left_type);

        if (is_error_type(left_type) || is_error_type(right_type)) {
            return resolver_error_type(state);
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        TypeRegistry *registry = state->global->types;

        bool both_strings = is_string_type(registry, left_type) && is_string_type(registry, right_type);

        if (left_type != right_type && !both_strings) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "cannot apply '%s' to %s and %s",
                       op_name, type_name(state, left_type), type_name(state, right_type));
            return resolver_error_type(state);
        }

        if (!bin_op_accepts(state, expr->bin_op.op, left_type, expr->span)) {
            return resolver_error_type(state);
        }

        if (both_strings) {
            const Type *characters = type_registry_ref_to(
                state->global->types, type_registry_get_primitive(state->global->types, TYPE_STR));

            borrow_into(state, expr->bin_op.left, characters, expr->span);
            borrow_into(state, expr->bin_op.right, characters, expr->span);
        }

        return bin_op_yields_bool(expr->bin_op.op)
                   ? type_registry_get_primitive(state->global->types, TYPE_BOOL)
                   : left_type;
    }
    case EXPR_BUILTIN: {
        if (expr->builtin.name->name == resolver_names(state)->caller) {
            if (!state->env.func.is_caller) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'@caller()' answers where a call was written, so only a 'caller' function "
                           "asks it");

                return resolver_error_type(state);
            }

            const Type *location = resolver_location_type(state);

            if (!location) {
                diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span,
                           "the core declares no '%s', which '@caller()' answers with", GAB_LOCATION_TYPE);

                return resolver_error_type(state);
            }

            return location;
        }

        if (expr->builtin.name->name == resolver_names(state)->size_of) {
            if (!expr->builtin.type_expr || expr->builtin.type_expr->apply.args.size != 1) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'@size_of<T>()' measures one type, as '@size_of<i32>()'");

                return resolver_error_type(state);
            }

            const Type *measured =
                resolve_type_expr(state, expr->builtin.type_expr->apply.args.data[0], expr->span);

            if (is_error_type(measured)) {
                return resolver_error_type(state);
            }

            const Type *counted = type_registry_get_primitive(state->global->types, TYPE_USIZE);

            fact_set_constant(
                state->facts, expr,
                constant_int(counted, (int32_t)type_registry_size_of(state->global->types, measured)));

            return counted;
        }

        diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span, "the compiler supplies no '@%s'",
                   expr->builtin.name->name->data);

        return resolver_error_type(state);
    }
    case EXPR_NAME: {
        String *sought = expr->name.name->name;

        Symbol *entry = scope_lookup(state->env.scope, sought);

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

        diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->name.name->span, "undeclared name '%s'",
                   sought->data);

        return resolver_error_type(state);
    }
    case EXPR_QUALIFIED: {
        fact_set_callee(state->facts, expr, resolve_qualified_func(state, expr));

        if (!fact_callee_of(state->facts, expr)) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span, "undeclared name '%s::%s'",
                       expr->qualified.qualifier->name->data, expr->qualified.name->name->data);

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

        if (expr->call.target && expr->call.target->kind == EXPR_NAME && resolve_cast(state, expr)) {
            return fact_type_of(state->facts, expr);
        }

        if (expr->call.target && expr->call.target->kind == EXPR_BUILTIN) {
            resolve_expr(state, expr->call.target, NULL);

            if (expr->call.args.size) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "'@%s()' takes no arguments",
                           expr->call.target->builtin.name->name->data);
            }

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
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                           "this expression is not callable");
            }

            return resolver_error_type(state);
        }

        if (expr->call.args.size != callee->signature.param_count) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "expected %zu argument(s), found %zu", callee->signature.param_count,
                       expr->call.args.size);
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

        if (type_kind(target_type) != TYPE_ARRAY) {
            return resolve_index_through_interface(state, expr);
        }

        if (index_type != type_registry_get_primitive(state->global->types, TYPE_I32)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "an index must be an i32, not %s", type_name(state, index_type));
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s is not a struct, so it has no fields",
                       type_name(state, fact_type_of(state->facts, expr->field.target)));
            return resolver_error_type(state);
        }

        String *field_name = expr->field.name->name;
        const TypeField *field = type_registry_find_field(state->global->types, target_type, field_name);

        if (!field) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->field.name->span,
                       "'%s' has no field '%s'", type_name(state, target_type), field_name->data);
            return resolver_error_type(state);
        }

        fact_set_field(state->facts, expr,
                       (size_t)(field - type_registry_fields_of(state->global->types, target_type)->fields));

        return field->type;
    }
    case EXPR_ADDR_OF: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!is_addressable(state, expr->unary.target)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of a temporary");
            return resolver_error_type(state);
        }

        if (type_kind(target_type) == TYPE_BOX) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of an owning pointer; return ownership instead of "
                       "repointing it through a borrow");
            return resolver_error_type(state);
        }

        Symbol *addressed = fact_root_local(state->facts, expr->unary.target);
        if (addressed) {
            addressed->pinned = true;
        }

        return type_registry_ref_to(state->global->types, target_type);
    }
    case EXPR_DEREF: {
        const Type *target_type = resolve_expr(state, expr->unary.target, NULL);

        if (is_error_type(target_type)) {
            return resolver_error_type(state);
        }

        if (!type_is_indirect(target_type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "cannot dereference %s",
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "unary '!' requires bool, found %s", type_name(state, target_type));
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; a heap slot cannot hold a borrow", type_name(state, type));
            return resolver_error_type(state);
        }

        return type_registry_box_to(state->global->types, type);
    }
    case EXPR_STRUCT_LIT: {
        const Type *type = resolve_type_expr(state, expr->struct_lit.type_expr, expr->span);

        if (is_error_type(type)) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            return resolver_error_type(state);
        }

        TypeRegistry *registry = state->global->types;

        if (type_kind(type) != TYPE_STRUCT) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "%s is not a struct",
                       type_name(state, type));
            return resolver_error_type(state);
        }

        const TypeFields *fields = type_registry_fields_of(registry, type);

        bool ok = true;
        bool *seen = fields->count ? arena_alloc(state->global->arena, fields->count * sizeof *seen) : NULL;

        for (size_t f = 0; f < fields->count; f++) {
            seen[f] = false;
        }

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            ASTFieldInit *init = &expr->struct_lit.fields.data[i];
            String *field_name = init->name->name;

            size_t index = fields->count;
            for (size_t f = 0; f < fields->count; f++) {
                if (fields->fields[f].name == field_name) {
                    index = f;
                    break;
                }
            }

            if (index == fields->count) {
                resolve_expr(state, init->value, NULL);
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, init->name->span, "%s has no field '%s'",
                           type_name(state, type), field_name->data);
                ok = false;
                continue;
            }

            if (seen[index]) {
                resolve_expr(state, init->value, fields->fields[index].type);
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, init->name->span,
                           "field '%s' is given twice", field_name->data);
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
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, value->span,
                           "field '%s' is %s, but %s was given", field_name->data,
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
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span, "field '%s' is missing",
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "an array's elements need the array's type to be written, as "
                       "'let xs: [i32; 3] = [1, 2, 3];'");
            return resolver_error_type(state);
        }

        int32_t length = type_array_length(expected);
        const Type *element = type_array_element(expected);

        if ((int32_t)expr->array_lit.elements.size != length) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "expected %d element(s), found %zu", length, expr->array_lit.elements.size);
            return resolver_error_type(state);
        }

        bool ok = true;

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            ASTExpr *value = expr->array_lit.elements.data[i];

            if (is_error_type(fact_type_of(state->facts, value))) {
                ok = false;
                continue;
            }

            if (!type_accepts(state->global->types, element, fact_type_of(state->facts, value))) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, value->span,
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
        TypeRegistry *registry = state->global->types;

        TypeKind kind = literal_type_kind(expr->lit.kind);

        if (expr->lit.kind == LITERAL_INT && expected && is_integer_type(expected)) {
            return expected;
        }

        return kind == TYPE_STR ? type_registry_ref_to(registry, type_registry_get_primitive(registry, kind))
                                : type_registry_get_primitive(registry, kind);
    }
    default:
        return NULL;
    }
}

static const Type *resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected) {
    if (!expr) {
        return NULL;
    }

    const Type *type = resolve_expr_kind(state, expr, expected);

    if (type) {
        fact_set_type(state->facts, expr, type);
    }

    return type;
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
    resolver_mark_declared(state, stmt);

    String *struct_name = stmt->struct_decl.name->name;

    if (stmt->struct_decl.intrinsic) {
        if (!state->declares_intrinsics) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "an intrinsic struct is given its meaning by the compiler, so only its core "
                       "library declares one");
            return NULL;
        }

        if (struct_name != resolver_names(state)->unique) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "the compiler gives no intrinsic struct '%s' its meaning", struct_name->data);
            return NULL;
        }
    }

    if (reject_self_as_name(state, struct_name, stmt->span)) {
        return NULL;
    }

    if (scope_type_lookup_declaring(state->env.scope, struct_name)) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return NULL;
    }

    size_t param_count = stmt->struct_decl.param_count;

    TypeDecl *declared = arena_alloc(state->global->arena, sizeof(TypeDecl));

    *declared = (TypeDecl){
        .id = {.module = state->module_name, .name = struct_name},
        .param_count = param_count,
    };

    scope_bind_type_decl(resolver_declaring_scope(state), struct_name, declared);

    StructDecl *decl = arena_alloc(state->global->arena, sizeof(StructDecl));

    *decl = (StructDecl){
        .stmt = stmt,
        .scope = resolver_declaring_scope(state),
        .file_scope = state->env.scope,
        .file = state->env.file,
        .visible = state->env.visible,
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

StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type) {
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

void report_containment_cycle(ResolverState *state, StructDecl *closes_on, Span span) {
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

    diag_error(state->global->diagnostics, GAB_ERR_TYPE, span, "struct '%s' cannot contain itself: %.*s'%s'",
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

    Env saved = state->env;

    state->env.file = decl->file;
    state->env.visible = decl->visible;

    Scope *params = scope_create(state->global->arena, decl->file_scope);

    for (size_t i = 0; i < stmt->struct_decl.param_count; i++) {
        String *param_name = stmt->struct_decl.params[i]->name;

        if (reject_self_as_name(state, param_name, stmt->struct_decl.params[i]->span)) {
            continue;
        }

        if (!scope_bind_type(params, param_name, type_registry_param(state->global->types, i))) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span,
                       "duplicate type parameter '%s' on '%s'", param_name->data, decl->name->data);
        }
    }

    state->env.scope = params;

    size_t field_count = stmt->struct_decl.fields.size;
    TypeField *fields =
        field_count ? arena_alloc(state->global->arena, field_count * sizeof(TypeField)) : NULL;

    bool poisoned = false;
    size_t resolved = 0;

    for (size_t i = 0; i < field_count; i++) {
        ASTField *field = stmt->struct_decl.fields.data[i];
        String *field_name = field->name->name;

        bool duplicate = false;

        for (size_t seen = 0; seen < resolved; seen++) {
            if (fields[seen].name == field_name) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, field->name->span,
                       "duplicate field '%s' in struct '%s'", field_name->data, decl->name->data);
            poisoned = true;
            continue;
        }

        const Type *field_type = resolve_type_expr(state, field->type_expr, field->name->span);

        if (is_error_type(field_type)) {
            poisoned = true;
            continue;
        }

        if (!type_has_param(field_type)) {
            StructDecl *cycle = element_completes_a_cycle(state, field_type);

            if (cycle) {
                report_containment_cycle(state, cycle, field->name->span);
                poisoned = true;
                continue;
            }

            if (field_type_failed(state, field_type)) {
                poisoned = true;
                continue;
            }

            if (reject_unsized(state, field_type, field->name->span, "a field")) {
                poisoned = true;
                continue;
            }
        }

        fields[resolved++] = (TypeField){.name = field_name, .type = field_type};
    }

    state->env = saved;

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

    TypeRegistry *registry = state->global->types;

    type_registry_complete(registry, type_registry_apply(registry, decl->decl, NULL, 0));
}

static const Type *resolve_param_type_in(ResolverState *state, ASTField *param, bool generic) {
    const Type *type = resolve_type_expr(state, param->type_expr, param->name->span);

    if (generic && type_has_param(type)) {
        return type;
    }

    if (reject_unsized(state, type, param->name->span, "a parameter")) {
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

static void enter_owner_scope(ResolverState *state, TypeExpr *owner, TypeExpr *const *bounds) {
    if (!owner) {
        return;
    }

    Scope *params = scope_create(state->global->arena, state->env.scope);

    if (owner->kind == TYPE_EXPR_APPLY) {
        for (size_t i = 0; i < owner->apply.args.size; i++) {
            const TypeExpr *arg = owner->apply.args.data[i];

            if (arg->kind != TYPE_EXPR_NAME) {
                continue;
            }

            bind_type_param(
                state->global->types, params, arg->name->name, i,
                bound_kind_of(state->global->types, state->global->strings, bounds ? bounds[i] : NULL));
        }
    }

    state->env.scope = params;

    const Type *self = resolve_type_expr(state, owner, (Span){0});

    if (!is_error_type(self)) {
        scope_bind_type_param(params, resolver_names(state)->self, self);
    }
}

static void bind_own_type_params(ResolverState *state, ASTStmt *stmt, size_t owner_count) {
    for (size_t i = owner_count; i < stmt->func_decl.type_param_count; i++) {
        String *name = stmt->func_decl.type_params[i]->name;

        if (reject_self_as_name(state, name, stmt->func_decl.type_params[i]->span)) {
            continue;
        }

        if (!bind_type_param(state->global->types, state->env.scope, name, i,
                             bound_kind_of(state->global->types, state->global->strings,
                                           stmt->func_decl.type_param_bounds[i]))) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span,
                       "duplicate type parameter '%s' on '%s'", name->data, name->data);
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
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function on %s is declared by its core library, which is where its body belongs",
                   type_name(state, owner));
        return;
    }

    if ((stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) && !state->declares_intrinsics) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "an intrinsic is lowered by the compiler, so only its core library declares one");
        return;
    }

    if (stmt->func_decl.syntax & FUNC_SYN_INTRINSIC) {
        const IntrinsicLowering *intrinsic =
            intrinsic_for(state, type_name_of(owner), stmt->func_decl.name->name);

        if (!intrinsic) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "the compiler lowers no intrinsic '%s' on %s", stmt->func_decl.name->name->data,
                       type_name(state, owner));
            return;
        }
    }

    if (owner_is_primitive && !state->declares_intrinsics) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function on %s is declared by the runtime's core library", type_name(state, owner));
        return;
    }

    if (!owner_is_primitive) {
        if (type_kind(owner) != TYPE_STRUCT) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a function belongs to a struct this module declares, not to %s",
                       type_name(state, owner));
            return;
        }

        Symbol *bound = type_name_of(owner)
                            ? scope_type_lookup_declaring(declaring, (String *)type_name_of(owner))
                            : NULL;

        if (!bound || bound->kind != SYMBOL_TYPE_DECL || bound->type_decl != type_decl(owner)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot declare a function on '%s', which this module does not declare",
                       type_name(state, owner));
            return;
        }
    }

    const Type *return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    if (!type_has_param(return_type) && reject_unsized(state, return_type, stmt->span, "returned")) {
        return_type = resolver_error_type(state);
    }

    String *name = stmt->func_decl.name->name;

    if (reject_generic_without_body(state, stmt, name)) {
        return;
    }

    FuncDecl *decl = arena_alloc(state->global->arena, sizeof(FuncDecl));
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
        decl->signature.params = arena_alloc(state->global->arena, param_count * sizeof(const Type *));
        decl->signature.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            decl->signature.params[i] = resolve_param_type_in(state, stmt->func_decl.params.data[i],
                                                              stmt->func_decl.type_param_count > 0);
        }
    }

    Function *func = arena_alloc(state->global->arena, sizeof(Function));
    *func = (Function){
        .decl = decl,
        .signature = decl->signature,
    };

    fact_set_function(state->facts, stmt, func);

    if (!function_registry_declare_owned(state->global->functions, owner, func)) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a function '%s'",
                   type_name_of(owner)->data, name->data);
        return;
    }

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
    String *name = stmt->interface_decl.name->name;

    if (reject_self_as_name(state, name, stmt->interface_decl.name->span)) {
        return;
    }

    if (scope_type_lookup_declaring(state->env.scope, name)) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared",
                   name->data);
        return;
    }

    size_t count = stmt->interface_decl.members.size;
    size_t param_count = stmt->interface_decl.param_count;

    Arena *arena = state->global->arena;

    Env saved = state->env;
    Scope *params = scope_create(arena, state->env.scope);

    TypeRegistry *registry = state->global->types;

    scope_bind_type(params, resolver_names(state)->self, type_registry_param(registry, 0));

    for (size_t i = 0; i < param_count; i++) {
        scope_bind_type(params, stmt->interface_decl.params[i]->name, type_registry_param(registry, i + 1));
    }

    state->env.scope = params;

    FuncDecl **methods = count > 0 ? arena_alloc(arena, count * sizeof(FuncDecl *)) : NULL;

    for (size_t i = 0; i < count; i++) {
        ASTStmt *signature = stmt->interface_decl.members.data[i];

        FuncDecl *decl = arena_alloc(arena, sizeof(FuncDecl));
        *decl = (FuncDecl){
            .id = {.name = signature->func_decl.name->name},
            .linkage = LINKAGE_INTERNAL,
            .type_param_count = param_count + 1,
            .signature = {.return_type =
                              resolve_type_expr(state, signature->func_decl.return_type, signature->span)},
        };

        size_t signature_params = signature->func_decl.params.size;

        if (signature_params > 0) {
            const Type **types = arena_alloc(arena, signature_params * sizeof(const Type *));

            for (size_t p = 0; p < signature_params; p++) {
                types[p] = resolve_param_type_in(state, signature->func_decl.params.data[p], true);
            }

            decl->signature.params = types;
            decl->signature.param_count = signature_params;
        }

        methods[i] = decl;
    }

    state->env = saved;

    InterfaceDecl *interface = arena_alloc(arena, sizeof(InterfaceDecl));

    *interface = (InterfaceDecl){
        .id = {.module = state->module_name, .name = name},
        .methods = (const FuncDecl *const *)methods,
        .method_count = count,
        .param_count = param_count,
    };

    scope_bind_interface(resolver_declaring_scope(state), name, interface);
}

static bool block_declares(const ASTStmt *stmt, const String *name) {
    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        const ASTStmt *member = stmt->impl.members.data[i];

        if (member && member->kind == STMT_FUNC_DECL && member->func_decl.name->name == name) {
            return true;
        }
    }

    return false;
}

static void check_conformance(ResolverState *state, ASTStmt *stmt, const Type *implementor) {
    String *interface_name = stmt->impl.interface_name->name;

    InterfaceDecl *interface = resolver_lookup_interface(state, interface_name);

    if (!interface) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->impl.interface_name->span,
                   "unknown interface '%s'", interface_name->data);
        return;
    }

    size_t arg_count = stmt->impl.interface_args.size;

    if (arg_count != interface->param_count) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                   "'%s' takes %zu type argument(s), but %zu were given", interface_name->data,
                   interface->param_count, arg_count);
        return;
    }

    TypeArg args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < arg_count; i++) {
        const Type *argument =
            resolve_type_expr(state, stmt->impl.interface_args.data[i], stmt->impl.interface_name->span);

        if (is_error_type(argument)) {
            return;
        }

        args[i] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = argument};
    }

    if (!type_registry_declare_conformance(state->global->types, implementor, interface->id, args,
                                           arg_count)) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                   "'%s' already implements '%s'", type_name(state, implementor), interface_name->data);
        return;
    }

    for (size_t i = 0; i < interface->method_count; i++) {
        const String *name = interface->methods[i]->id.name;

        Function *supplied = block_declares(stmt, name)
                                 ? function_registry_find_owned(state->global->functions, implementor, name)
                                 : NULL;

        if (!supplied) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                       "'%s' implements '%s', which declares '%s', but supplies no '%s'",
                       type_name(state, implementor), interface_name->data, name->data, name->data);
            continue;
        }

        const Function *required = interface_method_for(state, interface, i, implementor, args, arg_count);

        const Type *expected_return = required->signature.return_type;

        if (expected_return != supplied->signature.return_type) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                       "'%s' of '%s' returns %s, but '%s' declares it returns %s", name->data,
                       type_name(state, implementor), type_name(state, supplied->signature.return_type),
                       interface_name->data, type_name(state, expected_return));
            continue;
        }

        size_t expected_count = required->signature.param_count;

        if (expected_count != supplied->signature.param_count) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                       "'%s' of '%s' takes %zu parameters, but '%s' declares %zu", name->data,
                       type_name(state, implementor), supplied->signature.param_count, interface_name->data,
                       expected_count);
            continue;
        }

        for (size_t p = 0; p < expected_count; p++) {
            const Type *expected = required->signature.params[p];

            if (expected != supplied->signature.params[p]) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_name->span,
                           "parameter %zu of '%s' is %s, but '%s' declares it %s", p + 1, name->data,
                           type_name(state, supplied->signature.params[p]), interface_name->data,
                           type_name(state, expected));
                break;
            }
        }
    }
}

static void declare_impl(ResolverState *state, ASTStmt *stmt) {
    Env saved = state->env;

    enter_impl_scope(state, stmt);

    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        ASTStmt *member = stmt->impl.members.data[i];

        if (member && member->kind == STMT_FUNC_DECL) {
            declare_owned_in_scope(state, saved.scope, member);

            resolver_mark_declared(state, member);
        }
    }

    if (stmt->impl.interface_name) {
        const Type *implementor = resolve_type_expr(state, stmt->impl.type, stmt->span);

        if (!is_error_type(implementor)) {
            check_conformance(state, stmt, implementor);
        }
    }

    state->env = saved;
}

static void resolve_impl(ResolverState *state, ASTStmt *stmt) {
    Env saved = state->env;

    enter_impl_scope(state, stmt);

    for (size_t i = 0; i < stmt->impl.members.size; i++) {
        resolve_stmt(state, stmt->impl.members.data[i]);
    }

    state->env = saved;
}

static void declare_owned(ResolverState *state, ASTStmt *stmt) {
    Env saved = state->env;

    enter_owner_scope(state, stmt->func_decl.owner, stmt->func_decl.type_param_bounds);

    declare_owned_in_scope(state, saved.scope, stmt);

    state->env = saved;
}

static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr) {
    if (expr->kind != EXPR_QUALIFIED) {
        return NULL;
    }

    String *member = expr->qualified.name->name;

    Scope *module_scope =
        expr->qualified.owner_type_expr ? NULL : resolver_qualifier_scope(state, expr->qualified.qualifier);

    if (module_scope) {
        Symbol *entry = scope_lookup(module_scope, member);

        if (entry && entry->kind == SYMBOL_FUNC) {
            return entry->func;
        }
    }

    const Type *owner;

    if (expr->qualified.owner_type_expr) {
        owner = resolve_type_expr(state, expr->qualified.owner_type_expr, expr->span);

        if (is_error_type(owner)) {
            return NULL;
        }
    } else {
        Symbol *symbol = resolver_resolve_name(state, state->env.scope, expr->qualified.qualifier->name);

        if (symbol && symbol->kind == SYMBOL_TYPE_DECL && symbol->type_decl->param_count > 0) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                       "'%s' takes %zu type argument(s), not 0", symbol->type_decl->id.name->data,
                       symbol->type_decl->param_count);
            return NULL;
        }

        owner = symbol_type(state->global->types, symbol);
    }

    if (!owner) {
        return NULL;
    }

    Function *found = function_registry_owned_for(state->global->functions, owner, member);

    if (!found) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no function '%s'",
                   type_name_of(owner)->data, member->data);

        return NULL;
    }

    if (found == function_registry_destructor(state->global->functions, owner)) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'destroy' runs where the value ends, so nothing calls it by hand");
    }

    return found;
}

static void check_abstract_body(ResolverState *state, ASTStmt *stmt) { resolve_func_body(state, stmt); }

static void enter_param_bounds(ResolverState *state, ASTStmt *stmt) {
    TypeParamBound *bounds = arena_alloc(state->global->arena, GAB_MAX_TYPE_PARAMS * sizeof(TypeParamBound));

    for (size_t i = 0; i < GAB_MAX_TYPE_PARAMS; i++) {
        bounds[i] = (TypeParamBound){.kind = BOUND_NONE};
    }

    state->env.param_bounds = bounds;

    for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
        const TypeExpr *bound = stmt->func_decl.type_param_bounds[i];

        BoundKind kind = bound_kind_of(state->global->types, state->global->strings, bound);

        if (kind == BOUND_NONE) {
            continue;
        }

        if (kind == BOUND_VALUE) {
            bounds[i] = (TypeParamBound){
                .kind = BOUND_VALUE,
                .value = resolve_type_expr(state, (TypeExpr *)bound, stmt->span),
            };
            continue;
        }

        const TypeExpr *named = bound->kind == TYPE_EXPR_APPLY ? bound->apply.base : bound;

        String *name = named->name->name;
        InterfaceDecl *interface = resolver_lookup_interface(state, name);

        if (!interface) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span,
                       "'%s' bounds a type parameter, so it names an interface", name->data);
            continue;
        }

        size_t arg_count = bound->kind == TYPE_EXPR_APPLY ? bound->apply.args.size : 0;

        if (arg_count != interface->param_count) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'%s' takes %zu type argument(s), but %zu were given", name->data,
                       interface->param_count, arg_count);
            continue;
        }

        bounds[i] = (TypeParamBound){.kind = BOUND_INTERFACE,
                                     .interface = {.interface = interface, .arg_count = arg_count}};

        for (size_t a = 0; a < arg_count; a++) {
            bounds[i].interface.args[a] =
                (TypeArg){.kind = TYPE_ARG_TYPE,
                          .type = resolve_type_expr(state, bound->apply.args.data[a], stmt->span)};
        }
    }
}

static void record_param_bounds(ResolverState *state, FuncDecl *decl) {
    if (!decl) {
        return;
    }

    decl->type_param_bounds = state->env.param_bounds;
}

static void declare_func(ResolverState *state, ASTStmt *stmt) {
    resolver_mark_declared(state, stmt);

    if (stmt->func_decl.owner) {
        declare_owned(state, stmt);
        return;
    }

    ASTIdent *func_name = stmt->func_decl.name;

    Env saved = state->env;

    if (stmt->func_decl.type_param_count > 0) {
        Scope *params = scope_create(state->global->arena, state->env.scope);

        for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
            String *param_name = stmt->func_decl.type_params[i]->name;

            if (reject_self_as_name(state, param_name, stmt->func_decl.type_params[i]->span)) {
                continue;
            }

            if (!bind_type_param(state->global->types, params, param_name, i,
                                 bound_kind_of(state->global->types, state->global->strings,
                                               stmt->func_decl.type_param_bounds[i]))) {
                diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->span,
                           "duplicate type parameter '%s' on '%s'", param_name->data, param_name->data);
            }
        }

        state->env.scope = params;

        enter_param_bounds(state, stmt);
    }

    const Type *func_return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    String *declared_name = func_name->name;

    if (reject_self_as_name(state, declared_name, func_name->span)) {
        state->env = saved;
        return;
    }

    if (reject_generic_without_body(state, stmt, declared_name)) {
        state->env = saved;
        return;
    }

    FuncDecl *decl = arena_alloc(state->global->arena, sizeof(FuncDecl));

    *decl = (FuncDecl){
        .id = {.module = state->module_name, .name = declared_name},
        .linkage = linkage_of(&stmt->func_decl),
        .modifiers = modifiers_of(&stmt->func_decl),
        .location_type = location_type_of(state, &stmt->func_decl),
        .signature = {.return_type = func_return_type},
    };

    size_t param_count = stmt->func_decl.params.size;

    if (param_count > 0) {
        decl->signature.params = arena_alloc(state->global->arena, param_count * sizeof(const Type *));
        decl->signature.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            decl->signature.params[i] =
                resolve_param_type_in(state, param, stmt->func_decl.type_param_count > 0);
        }
    }

    if (stmt->func_decl.type_param_count > 0) {
        decl->type_param_count = stmt->func_decl.type_param_count;

        record_param_bounds(state, decl);
    }

    Function *func = arena_alloc(state->global->arena, sizeof(Function));

    *func = (Function){.decl = decl, .signature = decl->signature};

    fact_set_function(state->facts, stmt, func);

    if (!scope_bind_func_against(resolver_declaring_scope(state), state->env.scope, declared_name, func)) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, func_name->span,
                   "'%s' is already declared in this scope", declared_name->data);
    }

    if (stmt->func_decl.type_param_count > 0 && stmt->func_decl.body) {
        check_abstract_body(state, stmt);
    }

    state->env = saved;
}

static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    size_t errors_before = diagnostics_count(state->global->diagnostics);

    Function *signature = fact_function_of(state->facts, stmt);

    Env saved = state->env;

    resolver_enter_scope(state);

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = param->name->name;

        const Type *param_type = signature->signature.params[i];

        if (reject_self_as_name(state, param_name, param->name->span)) {
            continue;
        }

        Symbol *binding = scope_decl_var(state->env.scope, param_name, param_type);

        if (!binding) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, param->name->span,
                       "duplicate parameter '%s'", param_name->data);
            continue;
        }

        fact_set_def(state->facts, param->name, binding);
    }

    state->env.func.return_type = signature->signature.return_type;
    state->env.func.is_caller = (stmt->func_decl.syntax & FUNC_SYN_CALLER) != 0;

    resolve_stmt(state, stmt->func_decl.body);

    if (diagnostics_count(state->global->diagnostics) == errors_before) {
        PendingBody body = {
            .body = stmt->func_decl.body, .param_fields = &stmt->func_decl.params, .function = signature};

        pending_body_list_add(&state->work->bodies, body);
    }

    state->env = saved;
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
        if (state->env.scope->kind != SCOPE_LOCAL) {
            diag_error(
                state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                "a variable is declared in a function body, and a module declares no state of its own");
            break;
        }

        const Type *declared =
            stmt->var_decl.type_expr ? resolve_type_expr(state, stmt->var_decl.type_expr, stmt->span) : NULL;

        if (reject_unsized(state, declared, stmt->span, "a variable")) {
            declared = resolver_error_type(state);
        }

        resolve_expr(state, stmt->var_decl.initializer, declared);

        if (!stmt->var_decl.initializer && declared && !is_error_type(declared) &&
            type_kind(declared) == TYPE_STRUCT) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
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
                    !type_accepts(state->global->types, decl_type, init_type)) {
                    if (type_registry_deref_of(state->global->types, decl_type) &&
                        type_is_str_ref(init_type)) {
                        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                                   "text is borrowed characters, so %s cannot take it",
                                   type_name(state, decl_type));
                    } else {
                        diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                                   "cannot initialize a variable of type %s with a value of type %s",
                                   type_name(state, decl_type), type_name(state, init_type));
                    }

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

        String *var_name = stmt->var_decl.name->name;

        Symbol *var = reject_self_as_name(state, var_name, stmt->var_decl.name->span)
                          ? NULL
                          : scope_decl_var(resolver_declaring_scope(state), var_name, type);

        if (!var) {
            diag_error(state->global->diagnostics, GAB_ERR_NAME, stmt->var_decl.name->span,
                       "'%s' is already declared in this scope", var_name->data);
            break;
        }

        mark_implicit_move(state, stmt->var_decl.initializer, type, stmt->span);

        fact_set_def(state->facts, stmt->var_decl.name, var);
        break;
    }
    case STMT_FUNC_DECL: {
        if (!resolver_mark_declared(state, stmt)) {
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
        if (!resolver_mark_declared(state, stmt)) {
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
            !type_accepts(state->global->types, target_type, value_type)) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
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
            if (fact_use_of(state->facts, stmt->assign.value) == target &&
                !type_registry_copies(state->global->types, target_type)) {
                diag_error(state->global->diagnostics, GAB_ERR_LIFETIME, stmt->span,
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot apply '%s=' to %s and %s", op_name, type_name(state, target_type),
                       type_name(state, value_type));
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
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->ifstmt.condition->span,
                       "'if' requires a boolean condition, found %s", type_name(state, condition_type));
        }

        resolve_stmt(state, stmt->ifstmt.then_block);
        resolve_stmt(state, stmt->ifstmt.else_block);
        break;
    }
    case STMT_FOR: {
        Env saved = state->env;

        resolver_enter_scope(state);

        resolve_stmt(state, stmt->forstmt.init);

        if (stmt->forstmt.condition) {
            const Type *condition_type = resolve_expr(state, stmt->forstmt.condition, NULL);

            if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->forstmt.condition->span,
                           "'for' requires a boolean condition, found %s", type_name(state, condition_type));
            }
        }

        state->env.func.loop_depth++;
        resolve_stmt(state, stmt->forstmt.body);
        resolve_stmt(state, stmt->forstmt.post);
        state->env.func.loop_depth--;

        state->env = saved;
        break;
    }
    case STMT_JUMP: {
        if (state->env.func.loop_depth == 0) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'%s' is only valid inside a loop", stmt->jump.is_break ? "break" : "continue");
        }

        break;
    }
    case STMT_BLOCK: {
        Env saved = state->env;

        resolver_enter_scope(state);

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            resolve_stmt(state, stmt->block.list.data[i]);
        }

        state->env = saved;
        break;
    }
    case STMT_RETURN: {
        resolve_expr(state, stmt->ret.result, state->env.func.return_type);

        const Type *expected = state->env.func.return_type;
        const Type *actual = stmt->ret.result ? fact_type_of(state->facts, stmt->ret.result) : NULL;

        bool poisoned =
            (expected && type_kind(expected) == TYPE_ERROR) || (actual && type_kind(actual) == TYPE_ERROR);

        bool accepted =
            actual && expected ? type_accepts(state->global->types, expected, actual) : actual == expected;

        if (!poisoned && !accepted) {
            diag_error(state->global->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "returns %s, but %s was declared", type_name(state, actual),
                       type_name(state, expected));
            break;
        }

        if (!poisoned && accepted && actual) {
            mark_implicit_move(state, stmt->ret.result, expected, stmt->span);

            borrow_into(state, stmt->ret.result, expected, stmt->span);
        }

        break;
    }
    }
}

typedef struct {
    ASTModule *module;

    Scope **scopes;
    Visible *visible;
} Files;

static void resolver_enter_file(ResolverState *state, const Files *files, size_t f) {
    state->env.file = files->module->files.data[f];
    state->env.visible = &files->visible[f];
    state->env.scope = files->scopes[f];
    state->env.declaring = state->module_scope;
}

static Files collect_files(const Resolver *resolver, ResolverState *state, ASTModule *module) {
    Arena *arena = state->global->arena;

    Files files = {
        .module = module,
        .scopes = arena_alloc(arena, module->files.size * sizeof(Scope *)),
        .visible = arena_alloc(arena, module->files.size * sizeof(Visible)),
    };

    for (size_t f = 0; f < module->files.size; f++) {
        files.scopes[f] = arena_alloc(arena, sizeof(Scope));
        scope_init_kind(files.scopes[f], arena, state->module_scope, SCOPE_FILE);

        const ASTImportList *imports = &module->files.data[f]->imports;

        files.visible[f] = (Visible){
            .modules = arena_alloc(arena, (imports->size + 1) * sizeof(Module *)),
        };

        for (size_t i = 0; i < imports->size; i++) {
            String *name = imports->data[i].name->name;

            Module **imported = resolver->modules ? module_map_lookup(resolver->modules, name) : NULL;

            if (!imported) {
                continue;
            }

            if (!scope_bind_module(files.scopes[f], name, *imported)) {
                diag_error(state->global->diagnostics, GAB_ERR_NAME, imports->data[i].name->span,
                           "'%s' is already declared in this file", name->data);
            }

            files.visible[f].modules[files.visible[f].count++] = *imported;
        }

        if (resolver->core) {
            files.visible[f].modules[files.visible[f].count++] = resolver->core;
        }
    }

    return files;
}

static void declare_types(ResolverState *state, const Files *files) {
    for (size_t f = 0; f < files->module->files.size; f++) {
        resolver_enter_file(state, files, f);

        const ASTStmtList *statements = &state->env.file->statements;

        for (size_t i = 0; i < statements->size; i++) {
            ASTStmt *stmt = statements->data[i];

            if (!stmt) {
                continue;
            }

            if (stmt->kind == STMT_INTERFACE_DECL) {
                declare_interface(state, stmt);
            }

            if (stmt->kind == STMT_STRUCT_DECL) {
                declare_struct(state, stmt);
            }
        }
    }
}

static void resolve_type_bodies(ResolverState *state) {
    for (size_t i = 0; i < state->struct_decls.size; i++) {
        resolve_struct_fields(state, state->struct_decls.data[i]);
    }
}

static void declare_functions(ResolverState *state, const Files *files) {
    for (size_t f = 0; f < files->module->files.size; f++) {
        resolver_enter_file(state, files, f);

        const ASTStmtList *statements = &state->env.file->statements;

        for (size_t i = 0; i < statements->size; i++) {
            ASTStmt *stmt = statements->data[i];

            if (!stmt) {
                continue;
            }

            if (stmt->kind == STMT_FUNC_DECL) {
                declare_func(state, stmt);
            }

            if (stmt->kind == STMT_IMPL) {
                declare_impl(state, stmt);
            }
        }
    }
}

static void resolve_bodies(ResolverState *state, const Files *files) {
    for (size_t f = 0; f < files->module->files.size; f++) {
        resolver_enter_file(state, files, f);

        const ASTStmtList *statements = &state->env.file->statements;

        for (size_t i = 0; i < statements->size; i++) {
            resolve_stmt(state, statements->data[i]);
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
        .name = module->name ? module->name->name : NULL,
        .scope = module_scope,
    };
    resolved->work = pending_bodies_create(compile_arena);
    resolved->registry = resolver->types;
    resolved->functions = resolver->functions;

    facts_init(&resolved->facts, compile_arena);

    Global *global = arena_alloc(compile_arena, sizeof(Global));

    *global = (Global){
        .arena = compile_arena,
        .types = resolver->types,
        .functions = resolver->functions,
        .strings = resolver->strings,
        .diagnostics = diagnostics,
    };

    ResolverState state = {
        .global = global,
        .env =
            {
                .scope = module_scope,
                .file = module->files.size ? module->files.data[0] : ast_file_create(compile_arena),
            },
        .module_scope = module_scope,
        .module_name = module->name ? module->name->name : NULL,
        .declares_intrinsics = privileges.intrinsics,
        .facts = &resolved->facts,
        .work = &resolved->work,
        .struct_decls = struct_decl_list_create(arena_allocator(compile_arena)),
        .resolving = struct_decl_list_create(arena_allocator(compile_arena)),
    };

    declared_init_alloc(&state.declared, arena_allocator(compile_arena), 64);

    size_t errors_before = diagnostics_count(diagnostics);

    Files files = collect_files(resolver, &state, module);

    declare_types(&state, &files);
    resolve_type_bodies(&state);
    declare_functions(&state, &files);
    resolve_bodies(&state, &files);

    struct_decl_list_free(&state.struct_decls);
    struct_decl_list_free(&state.resolving);

    *out = resolved;

    return diagnostics_count(diagnostics) == errors_before;
}
