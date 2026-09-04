#include "ast/resolve.h"

#include "function_registry.h"

#include "ast/clone.h"

#include "ast/flow_pass.h"
#include "binding.h"
#include "object.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct StructDecl {
    ASTStmt *stmt;
    Scope *scope;
    String *name;

    TypeDecl *decl;

    bool fields_demanded;

    bool poisoned;
} StructDecl;

GAB_LIST(StructDeclList, struct_decl_list, StructDecl *)

typedef struct {
    TypeRegistry *registry;
    ASTStmt *body;
    Binding **params;
    size_t param_count;
    const Type *return_type;
    Function *function;
} FlowWork;

GAB_LIST(FlowWorkList, flow_work_list, FlowWork)

typedef struct {
    const Type *return_type;

    unsigned int loop_depth;
} FuncContext;

typedef struct {
    Arena *compile_arena;

    Scope *global_scope;
    Scope *current_scope;

    ModuleScopeMap *module_scopes;

    const ASTImportList *imports;

    String *module_name;

    bool allow_primitive_impls;

    FuncContext func_context;

    StructDeclList struct_decls;
    FlowWorkList flow_work;

    StructDeclList resolving;

    /* The interface bounding each type parameter of the declaration being resolved, by index. */
    Interface *param_bounds[GAB_MAX_TYPE_PARAMS];

    /* What each bound applies to its interface's parameters. */
    const Type *param_bound_args[GAB_MAX_TYPE_PARAMS][GAB_MAX_TYPE_PARAMS];
    size_t param_bound_arg_count[GAB_MAX_TYPE_PARAMS];

    /* Set while a generic body is checked against its bounds, where no instantiation exists to flow. */
    bool checking_abstract;

    ASTUnit *unit;

    unsigned int instantiating;

    Diagnostics *diagnostics;
} ResolverState;

static Arena *resolver_owner_arena(ResolverState *state) { return state->global_scope->arena; }

static const Type *resolver_error_type(ResolverState *state) {
    return type_registry_error_type(state->current_scope->type_registry);
}

static String *resolver_intern(ResolverState *state, StringRef ref) {
    return string_from_ref(state->current_scope->strings, ref);
}

static String *resolver_intern_cstr(ResolverState *state, const char *name) {
    return string_from_cstr(state->current_scope->strings, name);
}

/* 'Self' names the type an impl block is for, so nothing else may take the name. */
static bool reject_self_as_name(ResolverState *state, String *name, Span span) {
    if (name != resolver_intern_cstr(state, "Self")) {
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

static bool resolver_may_name(ResolverState *state, String *module) {
    if (module == state->module_name) {
        return true;
    }

    for (size_t i = 0; i < state->imports->size; i++) {
        if (string_from_ref(state->current_scope->strings, state->imports->data[i].name) == module) {
            return true;
        }
    }

    return false;
}

static Scope *resolver_expr_scope(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (!string_ref_split_colons(name, &module, &member)) {
        return state->current_scope;
    }

    if (!state->module_scopes) {
        return NULL;
    }

    String *module_name = string_from_ref(state->current_scope->strings, module);

    if (!resolver_may_name(state, module_name)) {
        return NULL;
    }

    Scope **existing = module_scope_map_lookup(state->module_scopes, module_name);

    return existing ? *existing : NULL;
}

static Resolution resolver_resolve_name(ResolverState *state, Scope *scope, String *name) {
    Resolution resolution = scope ? scope_resolve(scope, name) : (Resolution){0};

    if (resolution.kind != RESOLUTION_NONE || scope != state->current_scope || !state->module_scopes) {
        return resolution;
    }

    for (size_t i = 0; i < state->imports->size; i++) {
        String *module = string_from_ref(state->current_scope->strings, state->imports->data[i].name);

        Scope **imported = module_scope_map_lookup(state->module_scopes, module);

        if (!imported) {
            continue;
        }

        Resolution found = scope_resolve(*imported, name);

        if (found.kind != RESOLUTION_NONE) {
            return found;
        }
    }

    return resolution;
}

/* The prelude's interfaces are named without an import, as the methods it declares on a primitive are. */
static Interface *resolver_lookup_interface(ResolverState *state, String *name) {
    Interface *found = scope_interface_lookup(state->current_scope, name);

    if (found || !state->module_scopes) {
        return found;
    }

    Scope **prelude = module_scope_map_lookup(
        state->module_scopes, string_from_cstr(state->current_scope->strings, GAB_CORE_MODULE));

    if (prelude && (found = scope_interface_lookup(*prelude, name))) {
        return found;
    }

    for (size_t i = 0; i < state->imports->size; i++) {
        String *module = string_from_ref(state->current_scope->strings, state->imports->data[i].name);

        Scope **imported = module_scope_map_lookup(state->module_scopes, module);

        if (imported && (found = scope_interface_lookup(*imported, name))) {
            return found;
        }
    }

    return NULL;
}

static String *resolver_expr_member(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (string_ref_split_colons(name, &module, &member)) {
        return string_from_ref(state->current_scope->strings, member);
    }

    return resolver_intern(state, name);
}

static bool is_error_type(const Type *type) { return !type || type_kind(type) == TYPE_ERROR; }

static const char *type_name(ResolverState *state, const Type *type) {
    if (!type) {
        return "none";
    }

    if (type_name_of(type)) {
        return type_name_of(type)->data;
    }

    if (type_kind(type) == TYPE_ARRAY) {
        const char *element = type_name(state, type_array_element(type));
        size_t length = strlen(element) + 32;
        char *out = arena_alloc(state->compile_arena, length);

        snprintf(out, length, "[%s; %d]", element, type_array_length(type));

        return out;
    }

    const char *inner = type_name(state, type_pointee(type));
    const char *prefix = type_kind(type) == TYPE_REF ? "&" : "*";
    size_t length = strlen(prefix) + strlen(inner) + 1;
    char *out = arena_alloc(state->compile_arena, length);

    snprintf(out, length, "%s%s", prefix, inner);

    return out;
}

static void resolver_enter_scope(ResolverState *state) {
    state->current_scope =
        scope_create(state->compile_arena, state->current_scope->strings, state->current_scope);
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

static void fold_bin_op(ASTExpr *expr) {
    ASTExpr *left = expr->bin_op.left;
    ASTExpr *right = expr->bin_op.right;

    if (left->kind != EXPR_LITERAL || right->kind != EXPR_LITERAL) {
        return;
    }

    if (left->lit.kind != right->lit.kind) {
        return;
    }

    Literal folded = {.kind = left->lit.kind};

    if (left->lit.kind == TYPE_FLOAT) {
        float a = left->lit.as_float;
        float b = right->lit.as_float;

        switch (expr->bin_op.op) {
        case BIN_OP_ADD:
            folded.as_float = a + b;
            break;
        case BIN_OP_SUB:
            folded.as_float = a - b;
            break;
        case BIN_OP_MUL:
            folded.as_float = a * b;
            break;
        case BIN_OP_DIV:

            folded.as_float = a / b;
            break;
        default:
            return;
        }
    } else if (left->lit.kind == TYPE_INT) {
        int32_t a = left->lit.as_int;
        int32_t b = right->lit.as_int;

        switch (expr->bin_op.op) {
        case BIN_OP_ADD:
            folded.as_int = (int32_t)((uint32_t)a + (uint32_t)b);
            break;
        case BIN_OP_SUB:
            folded.as_int = (int32_t)((uint32_t)a - (uint32_t)b);
            break;
        case BIN_OP_MUL:
            folded.as_int = (int32_t)((uint32_t)a * (uint32_t)b);
            break;
        case BIN_OP_DIV:
        case BIN_OP_MOD:

            if (b == 0 || (a == INT32_MIN && b == -1)) {
                return;
            }

            folded.as_int = expr->bin_op.op == BIN_OP_DIV ? a / b : a % b;
            break;
        default:
            return;
        }
    } else {
        return;
    }

    expr->kind = EXPR_LITERAL;
    expr->lit = folded;
}

static bool is_addressable(const ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE:
        return ast_binding_of(expr) && ast_binding_of(expr)->kind == BINDING_VAR;
    case EXPR_FIELD:
        return is_addressable(expr->field.target);
    case EXPR_INDEX:

        return is_addressable(expr->index.target);
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

/* A declaration serves every instantiation of its owner, so the one for this type is made on demand. */
static Function *owned_for(TypeRegistry *registry, FunctionRegistry *functions, const Type *type,
                           const String *name) {
    Function *declaration = type_registry_find_owned(registry, type, name);

    if (!declaration || type_registry_owned_is_shared(declaration, type)) {
        return declaration;
    }

    /* A method with parameters of its own is specialized at the call, which knows all of them. */
    if (declaration->decl->type_param_count > type_arg_count(type)) {
        return declaration;
    }

    const Type *args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < type_arg_count(type); i++) {
        args[i] = type_args(type)[i].type;
    }

    return function_registry_specialize(functions, declaration, args, type_arg_count(type));
}

static Function *find_method_on_chain(TypeRegistry *registry, FunctionRegistry *functions, const Type *type,
                                      const String *name, const Type **out_base) {
    for (const Type *at = receiver_base_type(type); at; at = derefs_to(registry, at)) {
        Function *found = owned_for(registry, functions, at, name);

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
static bool lends_by_value(TypeRegistry *registry, const Type *to, const Type *from);
static bool lends_by_pointer(const Type *to, const Type *from);
static bool borrow_into(ResolverState *state, ASTExpr **slot, const Type *destination, Span span);
static void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span);

static void check_call_args(ResolverState *state, ASTExprList *args, const Type **params) {
    for (size_t i = 0; i < args->size; i++) {
        ASTExpr *arg = args->data[i];
        const Type *param_type = params[i];

        if (is_error_type(arg->type) || is_error_type(param_type)) {
            continue;
        }

        if (!type_accepts(state->current_scope->type_registry, param_type, arg->type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, arg->span, "argument %zu is %s, but %s was declared",
                       i + 1, type_name(state, arg->type), type_name(state, param_type));
            continue;
        }

        if (!borrow_into(state, &args->data[i], param_type, arg->span)) {
            continue;
        }

        mark_implicit_move(state, args->data[i], param_type, arg->span);
    }
}

typedef struct {
    size_t derefs;

    bool address_of;
} ReceiverAdjustment;

static void lower_method_call(Arena *arena, ASTExpr *expr, Function *method, ReceiverAdjustment adjustment) {
    ASTExpr *target = expr->call.target;

    ASTExpr *receiver = target->field.target;
    Span span = receiver->span;

    target->field.target = NULL;

    for (size_t i = 0; i < adjustment.derefs; i++) {
        const Type *inner = type_pointee(receiver->type);

        receiver = ast_deref_expr_create(arena, span, receiver);
        receiver->type = inner;
    }

    if (adjustment.address_of) {
        receiver = ast_addr_of_expr_create(arena, span, receiver);
        receiver->type = method->params[0];
    }

    ASTExprList args = ast_expr_list_create(arena_allocator(arena));
    ast_expr_list_add(&args, receiver);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        ast_expr_list_add(&args, expr->call.args.data[i]);
    }

    expr->call.target = NULL;
    expr->call.args = args;
    expr->callee = method;
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
            if (!is_addressable(receiver)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
                return false;
            }

            Binding *addressed = ast_root_local(receiver);
            if (addressed) {
                addressed->pinned = true;
            }

            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = true};
            return true;
        }

        if (lends_by_value(state->current_scope->type_registry, declared, at) ||
            lends_by_pointer(declared, at)) {
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

static void resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected);
static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr);

#define GAB_MAX_INSTANTIATION_DEPTH 32

static void resolve_func_body(ResolverState *state, ASTStmt *stmt);
static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

static void instantiate_body(ResolverState *state, Function *method, Span span);

/* Matches a declared parameter type against an argument's, binding each type parameter it reaches. */
static bool infer_type_args(const Type *declared, const Type *actual, const Type **args, size_t owed) {
    if (!declared || !actual || !type_has_param(declared)) {
        return true;
    }

    if (type_kind(declared) == TYPE_PARAM) {
        size_t index = type_param_index(declared);

        /* The first argument to reach a parameter fixes it; a later disagreement is an argument type error.
         */
        if (index < owed && !args[index]) {
            args[index] = actual;
        }

        return true;
    }

    /* An argument is lent or dereferenced to reach a borrowing parameter, so match what each finally names.
     */
    if (type_kind(declared) == TYPE_REF) {
        for (const Type *at = actual;; at = type_pointee(at)) {
            const Type *attempt[GAB_MAX_TYPE_PARAMS];
            memcpy(attempt, args, owed * sizeof(const Type *));

            if (infer_type_args(type_pointee(declared), at, attempt, owed)) {
                memcpy(args, attempt, owed * sizeof(const Type *));
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

    if (type_kind(declared) == TYPE_ARRAY) {
        return infer_type_args(type_array_element(declared), type_array_element(actual), args, owed);
    }

    if (type_decl(declared) != type_decl(actual) || type_arg_count(declared) != type_arg_count(actual)) {
        return false;
    }

    for (size_t i = 0; i < type_arg_count(declared); i++) {
        if (type_args(declared)[i].kind != TYPE_ARG_TYPE || type_args(actual)[i].kind != TYPE_ARG_TYPE) {
            continue;
        }

        if (!infer_type_args(type_args(declared)[i].type, type_args(actual)[i].type, args, owed)) {
            return false;
        }
    }

    return true;
}

/* 'fixed' slots are already known from a receiver; 'self_params' is 1 when parameter zero is one. */
static bool infer_call_args(ResolverState *state, ASTExpr *expr, Function *function, const Type **args,
                            size_t fixed, size_t self_params) {
    size_t owed = function->decl->type_param_count;

    if (expr->call.args.size + self_params != function->param_count) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                   function->param_count - self_params, expr->call.args.size);
        return false;
    }

    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i], NULL);

        if (is_error_type(expr->call.args.data[i]->type)) {
            return false;
        }

        infer_type_args(function->params[i + self_params], expr->call.args.data[i]->type, args, owed);
    }

    for (size_t i = fixed; i < owed; i++) {
        if (!args[i]) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "no argument names every type parameter of '%s', so each is written",
                       function->decl->name->data);
            return false;
        }
    }

    return true;
}

/* Type arguments a call names itself; only a plain call can, a method call's target having nowhere for them.
 */
static bool take_written_type_args(ResolverState *state, ASTExpr *expr, Function *generic,
                                   const TypeExpr *supplied, const Type **args) {
    size_t owed = generic->decl->type_param_count;

    if (supplied->kind != TYPE_EXPR_APPLY || supplied->apply.args.size != owed) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s)",
                   generic->decl->name->data, owed);
        return false;
    }

    for (size_t i = 0; i < owed; i++) {
        args[i] = resolve_type_expr(state, supplied->apply.args.data[i], expr->span);

        if (is_error_type(args[i])) {
            return false;
        }
    }

    return true;
}

/* The arguments a receiver's type already fixes, which are the ones its owner declared. */
static size_t take_receiver_type_args(const Type *receiver, const Type **args) {
    size_t fixed = type_arg_count(receiver);

    for (size_t i = 0; i < fixed; i++) {
        args[i] = type_args(receiver)[i].type;
    }

    return fixed;
}

static Function *specialize(ResolverState *state, ASTExpr *expr, Function *generic, const Type **args,
                            size_t fixed, size_t self_params) {
    size_t owed = generic->decl->type_param_count;

    if (owed <= fixed) {
        instantiate_body(state, generic, expr->span);

        return generic;
    }

    if (!infer_call_args(state, expr, generic, args, fixed, self_params)) {
        return NULL;
    }

    Function *specialized =
        function_registry_specialize(state->current_scope->functions, generic, args, owed);

    instantiate_body(state, specialized, expr->span);

    return specialized;
}

static Function *specialize_method_call(ResolverState *state, ASTExpr *expr, Function *method,
                                        const Type *receiver) {
    const Type *args[GAB_MAX_TYPE_PARAMS] = {0};

    return specialize(state, expr, method, args, take_receiver_type_args(receiver, args), 1);
}

static Function *specialize_call(ResolverState *state, ASTExpr *expr, Function *generic) {
    const TypeExpr *supplied = expr->call.target->var.owner_type_expr;

    const Type *args[GAB_MAX_TYPE_PARAMS] = {0};

    if (supplied && !take_written_type_args(state, expr, generic, supplied, args)) {
        return NULL;
    }

    return specialize(state, expr, generic, args, 0, 0);
}

static void instantiate_body(ResolverState *state, Function *method, Span span) {
    if (method->decl->body_kind != BODY_GAB || method->instance || !method->decl->body) {
        return;
    }

    if (method->decl->type_param_count == 0) {
        return;
    }

    if (state->instantiating >= GAB_MAX_INSTANTIATION_DEPTH) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' instantiates itself without end",
                   method->decl->name->data);
        return;
    }

    const ASTStmt *declaration = method->decl->body;

    ASTStmt *clone = ast_clone_stmt(state->compile_arena, declaration);

    ast_stmt_list_add(&state->unit->instances, clone);
    method->instance = clone;
    clone->func_decl.function = method;

    Scope *enclosing = state->current_scope;
    Scope *params = scope_create(resolver_owner_arena(state), enclosing->strings, enclosing);

    for (size_t i = 0; i < method->type_arg_count && i < clone->func_decl.type_param_count; i++) {
        scope_bind_argument(params, resolver_intern(state, clone->func_decl.type_params[i]),
                            method->type_args[i]);
    }

    state->current_scope = params;
    state->instantiating++;

    clone->func_decl.resolved_return_type =
        resolve_type_expr(state, clone->func_decl.return_type, clone->span);

    resolve_func_body(state, clone);

    state->instantiating--;
    state->current_scope = enclosing;
}

static Scope *signature_scope(ResolverState *state, const Type *implementor, const Interface *interface,
                              const Type *const *args, size_t arg_count);
static const Type *resolve_param_type_in(ResolverState *state, ASTField *param, bool generic);

/* A parameter's methods are the ones its bound declares, resolved with 'Self' as the parameter itself. */
static Function *bound_method(ResolverState *state, const Type *base, String *name, Span span) {
    if (!base || type_kind(base) != TYPE_PARAM) {
        return NULL;
    }

    Interface *interface = state->param_bounds[type_param_index(base)];

    if (!interface) {
        return NULL;
    }

    for (size_t i = 0; i < interface->method_count; i++) {
        ASTStmt *signature = interface->methods[i];

        if (resolver_intern(state, signature->func_decl.name) != name) {
            continue;
        }

        size_t index = type_param_index(base);

        Scope *enclosing = state->current_scope;
        state->current_scope = signature_scope(state, base, interface, state->param_bound_args[index],
                                               state->param_bound_arg_count[index]);

        FuncDecl *decl = arena_alloc(resolver_owner_arena(state), sizeof(FuncDecl));
        *decl = (FuncDecl){.name = name, .body_kind = BODY_GAB};

        Function *func = arena_alloc(resolver_owner_arena(state), sizeof(Function));
        *func = (Function){
            .decl = decl,
            .return_type = resolve_type_expr(state, signature->func_decl.return_type, span),
            .func_index = FUNCTION_NO_BODY,
        };

        size_t count = signature->func_decl.params.size;

        if (count > 0) {
            func->params = arena_alloc(resolver_owner_arena(state), count * sizeof(const Type *));
            func->param_count = count;

            for (size_t p = 0; p < count; p++) {
                func->params[p] = resolve_param_type_in(state, signature->func_decl.params.data[p], true);
            }
        }

        state->current_scope = enclosing;

        return func;
    }

    return NULL;
}

static void resolve_method_call(ResolverState *state, ASTExpr *expr) {
    ASTExpr *receiver = expr->call.target->field.target;
    StringRef name = expr->call.target->field.name;

    resolve_expr(state, receiver, NULL);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i], NULL);
    }

    const Type *receiver_type = receiver->type;

    if (is_error_type(receiver_type)) {
        expr->type = resolver_error_type(state);
        return;
    }

    String *method_name = resolver_intern(state, name);

    if (type_kind(receiver_base_type(receiver_type)) == TYPE_ARRAY &&
        method_name == string_from_cstr(state->current_scope->strings, "len")) {
        const Type *array = receiver_base_type(receiver_type);

        if (expr->call.args.size != 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected 0 argument(s), found %zu",
                       expr->call.args.size);
            expr->type = resolver_error_type(state);
            return;
        }

        expr->kind = EXPR_LITERAL;
        expr->lit = (Literal){.kind = TYPE_INT, .as_int = type_array_length(array)};
        expr->type = type_registry_get_primitive(state->current_scope->type_registry, TYPE_INT);
        return;
    }

    const Type *base = NULL;
    Function *method =
        find_method_on_chain(state->current_scope->type_registry, state->current_scope->functions,
                             receiver_type, method_name, &base);

    if (!method) {
        method = bound_method(state, base, method_name, expr->span);
    }

    if (!method) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
        expr->type = resolver_error_type(state);
        return;
    }

    method = specialize_method_call(state, expr, method, base);

    if (!method) {
        expr->type = resolver_error_type(state);
        return;
    }

    const Type *declared_receiver = method->param_count > 0 ? method->params[0] : base;

    if (method->param_count == 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'%s' takes nothing, so it is called as '%s::%s()' rather than on a value",
                   method_name->data, type_name(state, base), method_name->data);

        expr->type = resolver_error_type(state);
        return;
    }

    size_t declared_params = method->param_count - 1;

    ReceiverAdjustment adjustment;

    if (!reconcile_receiver(state, expr, receiver, declared_receiver, receiver_type, method_name,
                            &adjustment)) {
        expr->type = resolver_error_type(state);
        return;
    }

    if (expr->call.args.size != declared_params) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                   declared_params, expr->call.args.size);
        expr->type = resolver_error_type(state);
        return;
    }

    lower_method_call(state->compile_arena, expr, method, adjustment);

    check_call_args(state, &expr->call.args, method->params);

    expr->type = method->return_type;
}

static bool lends_by_value(TypeRegistry *registry, const Type *to, const Type *from) {
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

static bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from) {
    if (to == from) {
        return true;
    }

    if (accepts_by_borrowing(to, from)) {
        return true;
    }

    for (const Type *at = from;; at = type_pointee(at)) {
        if (lends_by_value(registry, to, at) || lends_by_pointer(to, at)) {
            return true;
        }

        if (!type_is_indirect(at)) {
            return false;
        }
    }
}

static bool borrow_into(ResolverState *state, ASTExpr **slot, const Type *destination, Span span) {
    if (type_is_str_ref(destination) &&
        type_registry_deref_of(state->current_scope->type_registry, (*slot)->type) &&
        !is_addressable(*slot)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a string that nothing holds, since its characters are freed where the "
                   "expression ends");
        return false;
    }

    while (type_is_indirect((*slot)->type) && type_pointee(destination) != type_pointee((*slot)->type) &&
           type_pointee(destination) != (*slot)->type) {
        const Type *inner = type_pointee((*slot)->type);

        ASTExpr *hop = ast_deref_expr_create(state->compile_arena, span, *slot);
        hop->type = inner;
        *slot = hop;
    }

    if (lends_by_value(state->current_scope->type_registry, destination, (*slot)->type)) {
        const Deref *deref = type_registry_deref(state->current_scope->type_registry, (*slot)->type);

        ASTExpr *lend = ast_lend_expr_create(state->compile_arena, span, *slot);
        lend->type = destination;

        lend->lend.parts = deref->parts;
        lend->lend.part_count = deref->part_count;

        *slot = lend;

        return true;
    }

    if (!accepts_by_borrowing(destination, (*slot)->type)) {
        return true;
    }

    if (!is_addressable(*slot)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    Binding *addressed = ast_root_local(*slot);
    if (addressed) {
        addressed->pinned = true;
    }

    ASTExpr *borrow = ast_addr_of_expr_create(state->compile_arena, span, *slot);
    borrow->type = destination;
    *slot = borrow;

    return true;
}

bool is_numeric_type(const Type *t) { return type_kind(t) == TYPE_INT || type_kind(t) == TYPE_FLOAT; }

bool is_integer_type(const Type *t) { return type_kind(t) == TYPE_INT; }

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
        if (!is_comparable_type(state->current_scope->type_registry, type)) {
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
    Resolution resolution =
        scope_resolve(state->current_scope, resolver_intern(state, expr->call.target->var.name));

    const Type *target = resolution_type(state->current_scope->type_registry, resolution);

    if (!target) {
        return false;
    }

    ASTExprList args = expr->call.args;
    ASTExpr *operand = args.size == 1 ? args.data[0] : NULL;

    expr->kind = EXPR_CAST;
    expr->cast.operand = operand;
    ast_bind(expr, NULL);

    if (!operand) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "a conversion to %s takes one operand",
                   type_name(state, target));
        expr->type = resolver_error_type(state);
        return true;
    }

    resolve_expr(state, operand, NULL);

    const Type *from = operand->type;

    if (is_error_type(from)) {
        expr->type = resolver_error_type(state);
        return true;
    }

    if (!is_numeric_type(target) || !is_numeric_type(from)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot convert %s to %s",
                   type_name(state, from), type_name(state, target));
        expr->type = resolver_error_type(state);
        return true;
    }

    expr->type = target;
    return true;
}

static void resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP: {
        resolve_expr(state, expr->bin_op.left, NULL);
        resolve_expr(state, expr->bin_op.right, NULL);

        const Type *left_type = expr->bin_op.left->type;
        const Type *right_type = expr->bin_op.right->type;

        if (is_error_type(left_type) || is_error_type(right_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        TypeRegistry *registry = state->current_scope->type_registry;

        bool both_strings = is_string_type(registry, left_type) && is_string_type(registry, right_type);

        if (left_type != right_type && !both_strings) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot apply '%s' to %s and %s",
                       op_name, type_name(state, left_type), type_name(state, right_type));
            expr->type = resolver_error_type(state);
            break;
        }

        if (!bin_op_accepts(state, expr->bin_op.op, left_type, expr->span)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (both_strings) {
            const Type *characters = type_registry_ref_to(
                state->current_scope->type_registry,
                type_registry_get_primitive(state->current_scope->type_registry, TYPE_STR));

            borrow_into(state, &expr->bin_op.left, characters, expr->span);
            borrow_into(state, &expr->bin_op.right, characters, expr->span);
        }

        expr->type = bin_op_yields_bool(expr->bin_op.op)
                         ? type_registry_get_primitive(state->current_scope->type_registry, TYPE_BOOL)
                         : left_type;

        fold_bin_op(expr);

        break;
    }
    case EXPR_VARIABLE: {
        Binding *entry = scope_binding_lookup(state->current_scope, resolver_intern(state, expr->var.name));

        if (entry) {
            if (entry->kind == BINDING_FUNC) {
                expr->callee = entry->func;
                break;
            }

            ast_bind(expr, entry);
            expr->type = entry->var.type;
            break;
        }

        expr->callee = resolve_qualified_func(state, expr);

        if (!expr->callee) {
            char *name = string_ref_to_cstr(expr->var.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "undeclared variable '%s'", name);
            free(name);

            expr->type = resolver_error_type(state);
            break;
        }

        break;
    }
    case EXPR_CALL: {
        if (!expr->call.target && expr->callee) {
            expr->type = expr->callee->return_type;
            break;
        }

        if (expr->call.target && expr->call.target->kind == EXPR_FIELD) {
            resolve_method_call(state, expr);
            break;
        }

        if (expr->call.target && expr->call.target->kind == EXPR_VARIABLE && resolve_cast(state, expr)) {
            break;
        }

        resolve_expr(state, expr->call.target, NULL);

        Function *callee = expr->call.target->callee;

        if (callee && callee->decl->type_param_count > 0) {
            callee = specialize_call(state, expr, callee);

            if (!callee) {
                expr->type = resolver_error_type(state);
                break;
            }

            expr->call.target->callee = callee;
        }

        bool params_known = callee && expr->call.args.size == callee->param_count;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            resolve_expr(state, expr->call.args.data[i], params_known ? callee->params[i] : NULL);
        }

        if (!callee) {
            if (expr->call.target->type != resolver_error_type(state)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "this expression is not callable");
            }

            expr->type = resolver_error_type(state);
            break;
        }

        if (expr->call.args.size != callee->param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                       callee->param_count, expr->call.args.size);
            expr->type = resolver_error_type(state);
            break;
        }

        check_call_args(state, &expr->call.args, callee->params);

        expr->callee = callee;
        expr->type = callee->return_type;
        break;
    }
    case EXPR_INDEX: {
        resolve_expr(state, expr->index.target, NULL);
        resolve_expr(state, expr->index.index, NULL);

        const Type *target_type = expr->index.target->type;

        if (is_error_type(target_type) || is_error_type(expr->index.index->type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
        }

        if (type_kind(target_type) != TYPE_ARRAY) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot index %s",
                       type_name(state, expr->index.target->type));
            expr->type = resolver_error_type(state);
            break;
        }

        if (expr->index.index->type !=
            type_registry_get_primitive(state->current_scope->type_registry, TYPE_INT)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "an index must be an int, not %s",
                       type_name(state, expr->index.index->type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->index.array_type = target_type;
        expr->type = type_array_element(target_type);
        break;
    }
    case EXPR_FIELD: {
        resolve_expr(state, expr->field.target, NULL);

        const Type *target_type = expr->field.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
        }

        if (type_kind(target_type) != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s is not a struct, so it has no fields", type_name(state, expr->field.target->type));
            expr->type = resolver_error_type(state);
            break;
        }

        String *field_name = resolver_intern(state, expr->field.name);
        const TypeField *field =
            type_registry_find_field(state->current_scope->type_registry, target_type, field_name);

        if (!field) {
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no field '%s'",
                       type_name(state, target_type), field_name->data);
            expr->type = resolver_error_type(state);
            break;
        }

        expr->field.owner = target_type;
        expr->field.index =
            (size_t)(field -
                     type_registry_fields_of(state->current_scope->type_registry, target_type)->fields);

        expr->type = field->type;

        break;
    }
    case EXPR_ADDR_OF: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (!is_addressable(expr->unary.target)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of a temporary");
            expr->type = resolver_error_type(state);
            break;
        }

        if (type_kind(target_type) == TYPE_BOX) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of an owning pointer; return ownership instead of "
                       "repointing it through a borrow");
            expr->type = resolver_error_type(state);
            break;
        }

        Binding *addressed = ast_root_local(expr->unary.target);
        if (addressed) {
            addressed->pinned = true;
        }

        expr->type = type_registry_ref_to(state->current_scope->type_registry, target_type);
        break;
    }
    case EXPR_DEREF: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (!type_is_indirect(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot dereference %s",
                       type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->type = type_pointee(target_type);

        break;
    }
    case EXPR_NEG: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (!is_numeric_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "unary '-' requires a numeric type, found %s", type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->type = target_type;
        break;
    }
    case EXPR_NOT: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (!is_boolean_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "unary '!' requires bool, found %s",
                       type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->type = target_type;
        break;
    }
    case EXPR_BOX: {
        resolve_expr(state, expr->box_expr.value, NULL);

        const Type *type = expr->box_expr.value->type;

        if (is_error_type(type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (type_kind(type) == TYPE_REF) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; a heap slot cannot hold a borrow", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->box_expr.type = type;
        expr->type = type_registry_box_to(state->current_scope->type_registry, type);
        break;
    }
    case EXPR_STRUCT_LIT: {
        const Type *type = resolve_type_expr(state, expr->struct_lit.type_expr, expr->span);

        if (is_error_type(type)) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            expr->type = resolver_error_type(state);
            break;
        }

        TypeRegistry *registry = state->current_scope->type_registry;

        if (type_kind(type) != TYPE_STRUCT) {
            for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
                resolve_expr(state, expr->struct_lit.fields.data[i].value, NULL);
            }

            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "%s is not a struct",
                       type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
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
            init->index = index;

            const Type *field_type = fields->fields[index].type;

            resolve_expr(state, init->value, field_type);

            ASTExpr *value = init->value;

            if (is_error_type(value->type)) {
                ok = false;
                continue;
            }

            if (!type_accepts(registry, field_type, value->type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, value->span,
                           "field '%.*s' is %s, but %s was given", (int)init->name.length, init->name.data,
                           type_name(state, field_type), type_name(state, value->type));
                ok = false;
                continue;
            }

            if (!borrow_into(state, &init->value, field_type, value->span)) {
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

        expr->type = ok ? type : resolver_error_type(state);
        break;
    }
    case EXPR_ARRAY_LIT: {
        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            resolve_expr(state, expr->array_lit.elements.data[i], NULL);
        }

        if (!expected || type_kind(expected) != TYPE_ARRAY) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "an array's elements need the array's type to be written, as "
                       "'let xs: [int; 3] = [1, 2, 3];'");
            expr->type = resolver_error_type(state);
            break;
        }

        int32_t length = type_array_length(expected);
        const Type *element = type_array_element(expected);

        if ((int32_t)expr->array_lit.elements.size != length) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %d element(s), found %zu",
                       length, expr->array_lit.elements.size);
            expr->type = resolver_error_type(state);
            break;
        }

        bool ok = true;

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            ASTExpr *value = expr->array_lit.elements.data[i];

            if (is_error_type(value->type)) {
                ok = false;
                continue;
            }

            if (!type_accepts(state->current_scope->type_registry, element, value->type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, value->span,
                           "element %zu is %s, but the array holds %s", i + 1, type_name(state, value->type),
                           type_name(state, element));
                ok = false;
                continue;
            }

            if (!borrow_into(state, &expr->array_lit.elements.data[i], element, value->span)) {
                ok = false;
                continue;
            }

            mark_implicit_move(state, expr->array_lit.elements.data[i], element, value->span);
        }

        expr->type = ok ? expected : resolver_error_type(state);
        break;
    }
    case EXPR_LITERAL: {
        TypeRegistry *registry = state->current_scope->type_registry;

        expr->type = expr->lit.kind == TYPE_STR
                         ? type_registry_ref_to(registry, type_registry_get_primitive(registry, TYPE_STR))
                         : type_registry_get_primitive(registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

static void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span) {
    if (!value || is_error_type(value->type)) {
        return;
    }

    if (type_registry_copies(state->current_scope->type_registry, value->type)) {
        return;
    }

    if (destination && !type_registry_owns(state->current_scope->type_registry, destination)) {
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

    if (value->kind != EXPR_VARIABLE || !ast_binding_of(value) ||
        ast_binding_of(value)->kind != BINDING_VAR) {
        return;
    }

    value->moves = true;
}

static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span) {
    if (!expr) {
        return NULL;
    }

    TypeRegistry *registry = state->current_scope->type_registry;

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
        Scope *base_scope = resolver_expr_scope(state, expr->apply.base->name);

        String *base_name = base_scope ? resolver_expr_member(state, expr->apply.base->name) : NULL;

        Resolution base_resolution =
            base_name ? resolver_resolve_name(state, base_scope, base_name) : (Resolution){0};

        const TypeDecl *base_decl =
            base_resolution.kind == RESOLUTION_TYPE_DECL ? base_resolution.decl : NULL;
        const Type *base = resolution_type(registry, base_resolution);

        if (base_resolution.kind == RESOLUTION_NONE) {
            char *name = string_ref_to_cstr(expr->apply.base->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        if (base_decl) {
            if (expr->apply.args.size != base_decl->param_count) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not %zu",
                           base_decl->name->data, base_decl->param_count, expr->apply.args.size);
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

                if (type_registry_size_of(registry, args[i]) == 0) {
                    diag_error(state->diagnostics, GAB_ERR_TYPE, span, "a type argument must have a size");
                    return resolver_error_type(state);
                }
            }

            return type_registry_apply(registry, base_decl, args, expr->apply.args.size);
        }

        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "%s does not take a type argument",
                   type_name(state, base));

        return resolver_error_type(state);
    }

    case TYPE_EXPR_ARRAY: {
        const Type *element = resolve_type_expr(state, expr->array.element, span);

        if (is_error_type(element)) {
            return resolver_error_type(state);
        }

        if (type_has_param(element)) {
            return type_registry_array_of(registry, element, expr->array.length);
        }

        if (reject_unsized(state, element, span, "an array's element")) {
            return resolver_error_type(state);
        }

        StructDecl *cycle = element_completes_a_cycle(state, element);

        if (cycle) {
            report_containment_cycle(state, cycle, span);
            return resolver_error_type(state);
        }

        size_t element_size = type_registry_size_of(registry, element);

        if (element_size == 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "an array's element must have a size");
            return resolver_error_type(state);
        }

        int32_t length = expr->array.length;

        if (length <= 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "an array's length must be positive, not %d",
                       length);
            return resolver_error_type(state);
        }

        size_t bytes = element_size * (size_t)length;

        if (bytes > GAB_MAX_TYPE_BYTES) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                       "an array of %d needs %zu bytes, over the %d a frame addresses", length, bytes,
                       GAB_MAX_TYPE_BYTES);
            return resolver_error_type(state);
        }

        return type_registry_array_of(registry, element, length);
    }

    case TYPE_EXPR_NAME:
        break;
    }

    Scope *scope = resolver_expr_scope(state, expr->name);

    Resolution resolution = resolver_resolve_name(state, scope, resolver_expr_member(state, expr->name));

    const Type *type = resolution_type(registry, resolution);

    if (type) {
        return type;
    }

    if (resolution.kind == RESOLUTION_TYPE_DECL) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not 0",
                   resolution.decl->name->data, resolution.decl->param_count);

        return resolver_error_type(state);
    }

    if (resolver_intern(state, expr->name) == resolver_intern_cstr(state, "Self")) {
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

    if (reject_self_as_name(state, struct_name, stmt->span)) {
        return NULL;
    }

    if (scope_declares_type(state->current_scope, struct_name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return NULL;
    }

    size_t param_count = stmt->struct_decl.param_count;

    TypeDecl *declared = arena_alloc(resolver_owner_arena(state), sizeof(TypeDecl));

    *declared = (TypeDecl){
        .name = struct_name,
        .param_count = param_count,
    };

    scope_bind_decl(state->current_scope, struct_name, declared);

    StructDecl *decl = arena_alloc(resolver_owner_arena(state), sizeof(StructDecl));

    *decl = (StructDecl){
        .stmt = stmt,
        .scope = state->current_scope,
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
    Scope *params = scope_create(resolver_owner_arena(state), decl->scope->strings, decl->scope);

    for (size_t i = 0; i < stmt->struct_decl.param_count; i++) {
        String *param_name = resolver_intern(state, stmt->struct_decl.params[i]);

        if (reject_self_as_name(state, param_name, stmt->span)) {
            continue;
        }

        if (!scope_bind_type(params, param_name, type_registry_param(decl->scope->type_registry, i))) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "duplicate type parameter '%s' on '%s'",
                       param_name->data, decl->name->data);
        }
    }

    state->current_scope = params;

    size_t field_count = stmt->struct_decl.fields.size;
    TypeField *fields =
        field_count ? arena_alloc(resolver_owner_arena(state), field_count * sizeof(TypeField)) : NULL;

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

    state->resolving.size--;

    if (poisoned) {
        decl->poisoned = true;
        scope_withdraw_type(decl->scope, decl->name);
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

    TypeRegistry *registry = state->current_scope->type_registry;

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
static void enter_owner_scope(ResolverState *state, TypeExpr *owner) {
    if (!owner) {
        return;
    }

    Scope *enclosing = state->current_scope;
    Scope *params = scope_create(resolver_owner_arena(state), enclosing->strings, enclosing);

    if (owner->kind == TYPE_EXPR_APPLY) {
        for (size_t i = 0; i < owner->apply.args.size; i++) {
            const TypeExpr *arg = owner->apply.args.data[i];

            if (arg->kind != TYPE_EXPR_NAME) {
                continue;
            }

            scope_bind_type(params, resolver_intern(state, arg->name),
                            type_registry_param(enclosing->type_registry, i));
        }
    }

    /* Entered before 'Self' resolves, so on a generic owner it names the type applied to them. */
    state->current_scope = params;

    const Type *self = resolve_type_expr(state, owner, (Span){0});

    if (!is_error_type(self)) {
        scope_bind_argument(params, resolver_intern_cstr(state, "Self"), self);
    }
}

/* Continues the owner's numbering, which enter_owner_scope bound at 0..n-1. */
static void bind_own_type_params(ResolverState *state, ASTStmt *stmt, size_t owner_count) {
    for (size_t i = owner_count; i < stmt->func_decl.type_param_count; i++) {
        String *name = resolver_intern(state, stmt->func_decl.type_params[i]);

        if (reject_self_as_name(state, name, stmt->span)) {
            continue;
        }

        if (!scope_bind_type(state->current_scope, name,
                             type_registry_param(state->current_scope->type_registry, i))) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "duplicate type parameter '%s' on '%s'",
                       name->data, name->data);
        }
    }
}

static size_t owner_type_param_count(const TypeExpr *owner) {
    return owner && owner->kind == TYPE_EXPR_APPLY ? owner->apply.args.size : 0;
}

static void declare_owned_in_scope(ResolverState *state, Scope *declaring, ASTStmt *stmt) {
    bind_own_type_params(state, stmt, owner_type_param_count(stmt->func_decl.owner));

    const Type *owner = resolve_type_expr(state, stmt->func_decl.owner, stmt->span);

    if (is_error_type(owner)) {
        return;
    }

    bool is_host = stmt->func_decl.body == NULL;

    bool owner_is_primitive = type_is_primitive(owner);

    if (owner_is_primitive && !is_host) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function on %s is defined by the host, so it must be 'extern'",
                   type_name(state, owner));
        return;
    }

    if (owner_is_primitive && !state->allow_primitive_impls) {
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

        TypeBinding *bound =
            type_name_of(owner) ? scope_binding_lookup_local(declaring, type_name_of(owner)) : NULL;

        if (!bound || bound->decl != type_decl(owner)) {
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

    stmt->func_decl.resolved_return_type = return_type;

    String *name = resolver_intern(state, stmt->func_decl.name);

    FuncDecl *decl = arena_alloc(resolver_owner_arena(state), sizeof(FuncDecl));
    *decl = (FuncDecl){
        .name = name,
        .module = is_host ? state->module_name : NULL,
        .owner = is_host ? type_name_of(owner) : NULL,
        .body_kind = is_host ? BODY_HOST : BODY_GAB,
        .body = stmt,
        .type_param_count = stmt->func_decl.type_param_count,
    };

    Function *func = arena_alloc(resolver_owner_arena(state), sizeof(Function));
    *func = (Function){
        .decl = decl,
        .return_type = return_type,
        .params = NULL,
        .param_count = 0,
        .func_index = FUNCTION_NO_BODY,
    };

    size_t param_count = stmt->func_decl.params.size;

    if (param_count > 0) {
        func->params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            func->params[i] = resolve_param_type_in(state, stmt->func_decl.params.data[i],
                                                    stmt->func_decl.type_param_count > 0);
        }
    }

    if (!type_registry_declare_owned(state->current_scope->type_registry, owner, func)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a function '%s'",
                   type_name_of(owner)->data, name->data);
        return;
    }

    stmt->func_decl.function = func;
}

static void enter_impl_scope(ResolverState *state, ASTStmt *stmt) {
    enter_owner_scope(state, stmt->impl.type);
}

static void declare_interface(ResolverState *state, ASTStmt *stmt) {
    String *name = resolver_intern(state, stmt->interface_decl.name);

    if (reject_self_as_name(state, name, stmt->span)) {
        return;
    }

    if (scope_declares_type(state->current_scope, name) ||
        scope_interface_lookup(state->current_scope, name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared", name->data);
        return;
    }

    size_t count = stmt->interface_decl.members.size;

    ASTStmt **methods =
        count > 0 ? arena_alloc(resolver_owner_arena(state), count * sizeof(ASTStmt *)) : NULL;

    /* Cloned into the arena the interface lives in: the AST as parsed dies with the unit's load. */
    for (size_t i = 0; i < count; i++) {
        Arena *arena = resolver_owner_arena(state);
        const ASTStmt *signature = stmt->interface_decl.members.data[i];

        ASTStmt *copy = ast_clone_stmt(arena, signature);

        copy->func_decl.return_type = ast_clone_type_expr(arena, signature->func_decl.return_type);

        copy->func_decl.params = ast_field_list_create(arena_allocator(arena));

        for (size_t p = 0; p < signature->func_decl.params.size; p++) {
            const ASTField *param = signature->func_decl.params.data[p];

            ast_field_list_add(&copy->func_decl.params,
                               ast_field_create(arena, param->span, param->name,
                                                ast_clone_type_expr(arena, param->type_expr)));
        }

        methods[i] = copy;
    }

    size_t param_count = stmt->interface_decl.param_count;

    String **params =
        param_count > 0 ? arena_alloc(resolver_owner_arena(state), param_count * sizeof(String *)) : NULL;

    for (size_t i = 0; i < param_count; i++) {
        params[i] = resolver_intern(state, stmt->interface_decl.params[i]);
    }

    Interface *interface = arena_alloc(resolver_owner_arena(state), sizeof(Interface));

    *interface = (Interface){
        .name = name,
        .methods = methods,
        .method_count = count,
        .params = params,
        .param_count = param_count,
    };

    scope_bind_interface(state->current_scope, name, interface);
}

/* The signature is resolved against the implementor, so 'Self' in it names that type, and the
 * interface's parameters name what was applied to them. */
static Scope *signature_scope(ResolverState *state, const Type *implementor, const Interface *interface,
                              const Type *const *args, size_t arg_count) {
    Scope *scope =
        scope_create(resolver_owner_arena(state), state->current_scope->strings, state->current_scope);

    scope_bind_argument(scope, resolver_intern_cstr(state, "Self"), implementor);

    for (size_t i = 0; interface && i < interface->param_count && i < arg_count; i++) {
        scope_bind_argument(scope, interface->params[i], args[i]);
    }

    return scope;
}

static Scope *self_scope(ResolverState *state, const Type *implementor) {
    return signature_scope(state, implementor, NULL, NULL, 0);
}

static void check_conformance(ResolverState *state, ASTStmt *stmt, const Type *implementor) {
    String *interface_name = resolver_intern(state, stmt->impl.interface_name);

    Interface *interface = resolver_lookup_interface(state, interface_name);

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

    const Type *args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < arg_count; i++) {
        args[i] = resolve_type_expr(state, stmt->impl.interface_args.data[i], stmt->impl.interface_span);

        if (is_error_type(args[i])) {
            return;
        }
    }

    if (!type_registry_declare_conformance(state->current_scope->type_registry, implementor,
                                           interface_name)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                   "'%s' already implements '%s'", type_name(state, implementor), interface_name->data);
        return;
    }

    Scope *enclosing = state->current_scope;
    state->current_scope = signature_scope(state, implementor, interface, args, arg_count);

    for (size_t i = 0; i < interface->method_count; i++) {
        ASTStmt *signature = interface->methods[i];

        String *name = resolver_intern(state, signature->func_decl.name);

        Function *supplied = type_registry_find_owned(enclosing->type_registry, implementor, name);

        if (!supplied) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' implements '%s', which declares '%s', but supplies no '%s'",
                       type_name(state, implementor), interface_name->data, name->data, name->data);
            continue;
        }

        const Type *expected_return = resolve_type_expr(state, signature->func_decl.return_type, stmt->span);

        if (expected_return != supplied->return_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' of '%s' returns %s, but '%s' declares it returns %s", name->data,
                       type_name(state, implementor), type_name(state, supplied->return_type),
                       interface_name->data, type_name(state, expected_return));
            continue;
        }

        size_t expected_count = signature->func_decl.params.size;

        if (expected_count != supplied->param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                       "'%s' of '%s' takes %zu parameters, but '%s' declares %zu", name->data,
                       type_name(state, implementor), supplied->param_count, interface_name->data,
                       expected_count);
            continue;
        }

        for (size_t p = 0; p < expected_count; p++) {
            const Type *expected = resolve_param_type_in(state, signature->func_decl.params.data[p], false);

            if (expected != supplied->params[p]) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->impl.interface_span,
                           "parameter %zu of '%s' is %s, but '%s' declares it %s", p + 1, name->data,
                           type_name(state, supplied->params[p]), interface_name->data,
                           type_name(state, expected));
                break;
            }
        }
    }

    state->current_scope = enclosing;
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

    enter_owner_scope(state, stmt->func_decl.owner);

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
        Binding *entry = scope_binding_lookup(module_scope, resolver_intern(state, member_ref));

        if (entry && entry->kind == BINDING_FUNC) {
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
        Resolution resolution =
            resolver_resolve_name(state, state->current_scope, resolver_intern(state, owner_ref));

        if (resolution.kind == RESOLUTION_TYPE_DECL && resolution.decl->param_count > 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s), not 0",
                       resolution.decl->name->data, resolution.decl->param_count);
            return NULL;
        }

        owner = resolution_type(state->current_scope->type_registry, resolution);
    }

    if (!owner) {
        return NULL;
    }

    String *member = resolver_intern(state, member_ref);
    Function *found =
        owned_for(state->current_scope->type_registry, state->current_scope->functions, owner, member);

    if (!found) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no function '%s'",
                   type_name_of(owner)->data, member->data);

        return NULL;
    }

    return found;
}

/* Checked once with its parameters abstract, so an error is reported whether or not it is instantiated. */
static void check_abstract_body(ResolverState *state, ASTStmt *stmt) {
    ASTStmt *clone = ast_clone_stmt(state->compile_arena, stmt);

    clone->func_decl.resolved_return_type = stmt->func_decl.resolved_return_type;

    bool was_checking = state->checking_abstract;
    state->checking_abstract = true;

    resolve_func_body(state, clone);

    state->checking_abstract = was_checking;
}

/* Each bound names an interface, which the body is checked against before any instantiation. */
static void enter_param_bounds(ResolverState *state, ASTStmt *stmt) {
    for (size_t i = 0; i < GAB_MAX_TYPE_PARAMS; i++) {
        state->param_bounds[i] = NULL;
        state->param_bound_arg_count[i] = 0;
    }

    for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
        const TypeExpr *bound = stmt->func_decl.type_param_bounds[i];

        if (!bound) {
            continue;
        }

        const TypeExpr *named = bound->kind == TYPE_EXPR_APPLY ? bound->apply.base : bound;

        String *name = resolver_intern(state, named->name);
        Interface *interface = resolver_lookup_interface(state, name);

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

        for (size_t a = 0; a < arg_count; a++) {
            state->param_bound_args[i][a] = resolve_type_expr(state, bound->apply.args.data[a], stmt->span);
        }

        state->param_bound_arg_count[i] = arg_count;
        state->param_bounds[i] = interface;
    }
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
        Scope *params = scope_create(resolver_owner_arena(state), enclosing->strings, enclosing);

        for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
            String *param_name = resolver_intern(state, stmt->func_decl.type_params[i]);

            if (reject_self_as_name(state, param_name, stmt->span)) {
                continue;
            }

            if (!scope_bind_type(params, param_name, type_registry_param(enclosing->type_registry, i))) {
                diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span,
                           "duplicate type parameter '%s' on '%s'", param_name->data, param_name->data);
            }
        }

        state->current_scope = params;

        enter_param_bounds(state, stmt);
    }

    const Type *func_return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = func_return_type;

    String *declared_name = resolver_intern(state, func_name);

    if (reject_self_as_name(state, declared_name, stmt->span)) {
        state->current_scope = enclosing;
        return;
    }

    Binding *declared = scope_decl_func(enclosing, declared_name, func_return_type);

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
        decl->body_kind = stmt->func_decl.body == NULL ? BODY_HOST : BODY_GAB;

        if (decl->body_kind == BODY_HOST) {
            decl->name = resolver_intern(state, func_name);
            decl->module = state->module_name;
        }
    }

    size_t param_count = stmt->func_decl.params.size;

    if (func && param_count > 0) {
        func->params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            func->params[i] = resolve_param_type_in(state, param, stmt->func_decl.type_param_count > 0);
        }
    }

    if (decl && stmt->func_decl.type_param_count > 0) {
        decl->type_param_count = stmt->func_decl.type_param_count;
        decl->name = resolver_intern(state, func_name);

        /* A generic declaration keeps its own statement, which each instantiation clones and resolves. */
        decl->body = stmt;

        if (stmt->func_decl.body) {
            check_abstract_body(state, stmt);
        }
    }

    state->current_scope = enclosing;
}

static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    size_t errors_before = diagnostics_count(state->diagnostics);

    resolver_enter_scope(state);

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);
        const Type *param_type = resolve_type_expr(state, param->type_expr, param->span);

        if (reject_self_as_name(state, param_name, param->span)) {
            continue;
        }

        Binding *binding = scope_decl_var(state->current_scope, param_name, param_type);

        if (!binding) {
            char *name = string_ref_to_cstr(param->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, param->span, "duplicate parameter '%s'", name);
            free(name);
            continue;
        }

        param->binding = binding;
    }

    FuncContext previous_context = state->func_context;

    state->func_context.return_type = stmt->func_decl.resolved_return_type;

    resolve_stmt(state, stmt->func_decl.body);

    state->func_context = previous_context;

    if (!state->checking_abstract && diagnostics_count(state->diagnostics) == errors_before) {
        size_t param_count = stmt->func_decl.params.size;
        Binding **params = arena_alloc(state->compile_arena, (param_count + 1) * sizeof(Binding *));
        size_t count = 0;

        for (size_t i = 0; i < param_count; i++) {
            params[count++] = stmt->func_decl.params.data[i]->binding;
        }

        FlowWork work = {.registry = state->current_scope->type_registry,
                         .body = stmt->func_decl.body,
                         .params = params,
                         .param_count = count,
                         .return_type = stmt->func_decl.resolved_return_type,
                         .function = stmt->func_decl.function};

        flow_work_list_add(&state->flow_work, work);
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
                const Type *init_type = stmt->var_decl.initializer->type;

                if (!is_error_type(decl_type) && !is_error_type(init_type) &&
                    !type_accepts(state->current_scope->type_registry, decl_type, init_type)) {
                    if (type_registry_deref_of(state->current_scope->type_registry, decl_type) &&
                        type_is_str_ref(init_type)) {
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
                } else if (!borrow_into(state, &stmt->var_decl.initializer, decl_type,
                                        stmt->var_decl.initializer->span)) {
                    decl_type = resolver_error_type(state);
                }
            }

            type = decl_type;
        } else if (stmt->var_decl.initializer) {
            type = stmt->var_decl.initializer->type;
        } else {
            type = resolver_error_type(state);
        }

        if (state->current_scope == state->global_scope &&
            type_registry_owns(state->current_scope->type_registry, type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a top-level variable may not own, since no scope closes to free it; %s belongs in a "
                       "function body",
                       type_name(state, type));
            type = resolver_error_type(state);
        }

        Binding *var =
            reject_self_as_name(state, resolver_intern(state, stmt->var_decl.name), stmt->span)
                ? NULL
                : scope_decl_var(state->current_scope, resolver_intern(state, stmt->var_decl.name), type);

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
        resolve_expr(state, stmt->assign.target, NULL);

        resolve_expr(state, stmt->assign.value, stmt->assign.target->type);

        const Type *target_type = stmt->assign.target->type;
        const Type *value_type = stmt->assign.value->type;

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !type_accepts(state->current_scope->type_registry, target_type, value_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot assign a value of type %s to a target of type %s",
                       type_name(state, value_type), type_name(state, target_type));
            break;
        }

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !borrow_into(state, &stmt->assign.value, target_type, stmt->span)) {
            break;
        }

        if (stmt->assign.target->kind == EXPR_FIELD || stmt->assign.target->kind == EXPR_DEREF) {
            mark_implicit_move(state, stmt->assign.value, target_type, stmt->span);
            break;
        }

        Binding *target = ast_binding_of(stmt->assign.target);

        if (target && target->kind == BINDING_VAR) {
            if (stmt->assign.value->kind == EXPR_VARIABLE && ast_binding_of(stmt->assign.value) == target &&
                !type_registry_copies(state->current_scope->type_registry, target_type)) {
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
        resolve_expr(state, stmt->compound_assign.target, NULL);
        resolve_expr(state, stmt->compound_assign.value, NULL);

        const Type *target_type = stmt->compound_assign.target->type;
        const Type *value_type = stmt->compound_assign.value->type;

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
        resolve_expr(state, stmt->ifstmt.condition, NULL);

        const Type *condition_type = stmt->ifstmt.condition->type;

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
            resolve_expr(state, stmt->forstmt.condition, NULL);

            const Type *condition_type = stmt->forstmt.condition->type;

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
        const Type *actual = stmt->ret.result ? stmt->ret.result->type : NULL;

        bool poisoned =
            (expected && type_kind(expected) == TYPE_ERROR) || (actual && type_kind(actual) == TYPE_ERROR);

        bool accepted = actual && expected
                            ? type_accepts(state->current_scope->type_registry, expected, actual)
                            : actual == expected;

        if (!poisoned && !accepted) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "returns %s, but %s was declared",
                       type_name(state, actual), type_name(state, expected));
            break;
        }

        if (!poisoned && accepted && actual) {
            borrow_into(state, &stmt->ret.result, expected, stmt->span);
        }

        break;
    }
    }
}

bool resolve_unit(Arena *compile_arena, ASTUnit *unit, Scope *global_scope, ModuleScopeMap *module_scopes,
                  bool allow_primitive_impls, Diagnostics *diagnostics) {
    ResolverState state = {
        .compile_arena = compile_arena,
        .global_scope = global_scope,
        .current_scope = global_scope,
        .module_scopes = module_scopes,
        .imports = &unit->imports,
        .module_name =
            unit->module_name.data ? string_from_ref(global_scope->strings, unit->module_name) : NULL,
        .allow_primitive_impls = allow_primitive_impls,
        .func_context =
            {
                .return_type = NULL,
            },
        .struct_decls = struct_decl_list_create(arena_allocator(compile_arena)),
        .flow_work = flow_work_list_create(arena_allocator(compile_arena)),
        .resolving = struct_decl_list_create(arena_allocator(compile_arena)),
        .unit = unit,
        .diagnostics = diagnostics,
    };

    size_t errors_before = diagnostics_count(diagnostics);

    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

        if (stmt && stmt->kind == STMT_INTERFACE_DECL) {
            declare_interface(&state, stmt);
        }

        if (stmt && stmt->kind == STMT_STRUCT_DECL) {
            declare_struct(&state, stmt);
        }
    }

    for (size_t i = 0; i < state.struct_decls.size; i++) {
        resolve_struct_fields(&state, state.struct_decls.data[i]);
    }

    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            declare_func(&state, stmt);
        }

        if (stmt && stmt->kind == STMT_IMPL) {
            declare_impl(&state, stmt);
        }
    }

    for (size_t i = 0; i < unit->statements.size; i++) {
        resolve_stmt(&state, unit->statements.data[i]);
    }

    for (size_t i = 0; i < state.flow_work.size; i++) {
        FlowWork *work = &state.flow_work.data[i];

        flow_pass_run(state.compile_arena, work->registry, work->body, work->params, work->param_count,
                      work->return_type, diagnostics, work->function, false);
    }

    for (size_t i = 0; i < state.flow_work.size; i++) {
        FlowWork *work = &state.flow_work.data[i];

        flow_pass_run(state.compile_arena, work->registry, work->body, work->params, work->param_count,
                      work->return_type, diagnostics, work->function, true);
    }

    flow_work_list_free(&state.flow_work);
    struct_decl_list_free(&state.struct_decls);
    struct_decl_list_free(&state.resolving);

    return diagnostics_count(diagnostics) == errors_before;
}
