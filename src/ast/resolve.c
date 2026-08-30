#include "ast/resolve.h"

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

    TypeDef *def;

    bool fields_demanded;

    bool poisoned;
} StructDecl;

#define struct_decl_list_item_free(item) ((void)(item))
GAB_LIST(StructDeclList, struct_decl_list, StructDecl *)

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

    FuncContext func_context;

    StructDeclList struct_decls;

    StructDeclList resolving;

    Diagnostics *diagnostics;
} ResolverState;

static Arena *resolver_owner_arena(ResolverState *state) { return state->global_scope->arena; }

static const Type *resolver_error_type(ResolverState *state) {
    return type_registry_error_type(state->current_scope->type_registry);
}

static String *resolver_intern(ResolverState *state, StringRef ref) {
    return string_from_ref(state->current_scope->strings, ref);
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
    const char *prefix = type_kind(type) == TYPE_REF ? "ref " : "box ";
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

    ast_expr_free(left);
    ast_expr_free(right);

    expr->kind = EXPR_LITERAL;
    expr->lit = folded;
}

static bool is_addressable(const ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE:
        return expr->binding && expr->binding->kind == BINDING_VAR;
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

static Function *find_method_on_chain(TypeRegistry *registry, const Type *type, const String *name,
                                      const Type **out_base) {
    for (const Type *at = receiver_base_type(type); at; at = derefs_to(registry, at)) {
        Function *found = type_registry_find_method(registry, at, name);

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

static void lower_method_call(ASTExpr *expr, Function *method, ReceiverAdjustment adjustment) {
    ASTExpr *target = expr->call.target;

    ASTExpr *receiver = target->field.target;
    Span span = receiver->span;

    target->field.target = NULL;
    ast_expr_free(target);

    for (size_t i = 0; i < adjustment.derefs; i++) {
        const Type *inner = type_pointee(receiver->type);

        receiver = ast_deref_expr_create(span, receiver);
        receiver->type = inner;
    }

    if (adjustment.address_of) {
        receiver = ast_addr_of_expr_create(span, receiver);
        receiver->type = method->params[0];
    }

    ASTExprList args = ast_expr_list_create();
    ast_expr_list_add(&args, receiver);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        ast_expr_list_add(&args, expr->call.args.data[i]);
    }

    ast_expr_list_free(&expr->call.args);

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

        ast_expr_free(expr->call.target);
        ast_expr_list_free(&expr->call.args);

        expr->kind = EXPR_LITERAL;
        expr->lit = (Literal){.kind = TYPE_INT, .as_int = type_array_length(array)};
        expr->type = type_registry_get_primitive(state->current_scope->type_registry, TYPE_INT);
        return;
    }

    const Type *base = NULL;
    Function *method =
        find_method_on_chain(state->current_scope->type_registry, receiver_type, method_name, &base);

    if (!method) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
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

    lower_method_call(expr, method, adjustment);

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

        ASTExpr *hop = ast_deref_expr_create(span, *slot);
        hop->type = inner;
        *slot = hop;
    }

    if (lends_by_value(state->current_scope->type_registry, destination, (*slot)->type)) {
        const Deref *deref = type_registry_deref(state->current_scope->type_registry, (*slot)->type);

        ASTExpr *lend = ast_lend_expr_create(span, *slot);
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

    ASTExpr *borrow = ast_addr_of_expr_create(span, *slot);
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
               "nothing holds a '%s', so it cannot be %s; write 'ref %s'", type_name(state, type), held_as,
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
    ASTExpr *callee = expr->call.target;
    ASTExpr *operand = args.size == 1 ? args.data[0] : NULL;

    for (size_t i = 0; i < args.size; i++) {
        if (args.data[i] != operand) {
            ast_expr_free(args.data[i]);
        }
    }

    ast_expr_list_free(&args);
    ast_expr_free(callee);

    expr->kind = EXPR_CAST;
    expr->cast.operand = operand;
    expr->binding = NULL;

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
                expr->callee = &entry->func;
                break;
            }

            expr->binding = entry;
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
        if (expr->call.target && expr->call.target->kind == EXPR_FIELD) {
            resolve_method_call(state, expr);
            break;
        }

        if (expr->call.target && expr->call.target->kind == EXPR_VARIABLE && resolve_cast(state, expr)) {
            break;
        }

        resolve_expr(state, expr->call.target, NULL);

        Function *callee = expr->call.target->callee;

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
    case EXPR_NEW: {
        const Type *type = resolve_type_expr(state, expr->new_expr.type_expr, expr->span);

        if (is_error_type(type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        const TypeFields *type_fields_of = type_registry_fields_of(state->current_scope->type_registry, type);

        if (type_fields_of->count == 0 && !type_is_indirect(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; 'new' takes a struct or a pointer", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        if (type_kind(type) == TYPE_REF) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; a heap slot cannot hold a borrow", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->new_expr.type = type;
        expr->type = type_registry_box_to(state->current_scope->type_registry, type);
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

    if (value->kind != EXPR_VARIABLE || !value->binding || value->binding->kind != BINDING_VAR) {
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

        Resolution base_resolution = base_name ? scope_resolve(base_scope, base_name) : (Resolution){0};

        const TypeDef *base_def = base_resolution.kind == RESOLUTION_TYPE_DECL ? base_resolution.def : NULL;
        const Type *base = resolution_type(registry, base_resolution);

        if (base_resolution.kind == RESOLUTION_NONE) {
            char *name = string_ref_to_cstr(expr->apply.base->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        if (base_def) {
            if (expr->apply.args.size != base_def->param_count) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not %zu",
                           base_def->name->data, base_def->param_count, expr->apply.args.size);
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

            return type_registry_apply(registry, base_def, args, expr->apply.args.size);
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

    Resolution resolution =
        scope ? scope_resolve(scope, resolver_expr_member(state, expr->name)) : (Resolution){0};

    const Type *type = resolution_type(registry, resolution);

    if (type) {
        return type;
    }

    if (resolution.kind == RESOLUTION_TYPE_DECL) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not 0",
                   resolution.def->name->data, resolution.def->param_count);

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
        if (type_decl(type) && state->struct_decls.data[i]->def == type_decl(type)) {
            return state->struct_decls.data[i];
        }
    }

    return NULL;
}

static StructDecl *declare_struct(ResolverState *state, ASTStmt *stmt) {
    stmt->struct_decl.declared = true;

    String *struct_name = resolver_intern(state, stmt->struct_decl.name);

    if (scope_declares_type(state->current_scope, struct_name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return NULL;
    }

    size_t param_count = stmt->struct_decl.param_count;

    TypeDef *def = arena_alloc(resolver_owner_arena(state), sizeof(TypeDef));

    *def = (TypeDef){
        .name = struct_name,
        .param_count = param_count,
    };

    scope_bind_decl(state->current_scope, struct_name, def);

    StructDecl *decl = arena_alloc(resolver_owner_arena(state), sizeof(StructDecl));

    *decl = (StructDecl){
        .stmt = stmt,
        .scope = state->current_scope,
        .name = struct_name,
        .def = def,
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
    TypeDef *def = decl->def;

    Scope *enclosing = state->current_scope;
    Scope *params = scope_create(resolver_owner_arena(state), decl->scope->strings, decl->scope);

    for (size_t i = 0; i < stmt->struct_decl.param_count; i++) {
        String *param_name = resolver_intern(state, stmt->struct_decl.params[i]);

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

    def->fields = fields;
    def->field_count = resolved;

    layout_struct(state, decl);
}

static void layout_struct(ResolverState *state, StructDecl *decl) {
    if (decl->def->param_count > 0) {
        return;
    }

    TypeRegistry *registry = state->current_scope->type_registry;

    type_registry_complete(registry, type_registry_apply(registry, decl->def, NULL, 0));
}

static const Type *resolve_param_type(ResolverState *state, ASTField *param) {
    const Type *type = resolve_type_expr(state, param->type_expr, param->span);

    if (reject_unsized(state, type, param->span, "a parameter")) {
        return resolver_error_type(state);
    }

    return type;
}

static void declare_owned(ResolverState *state, ASTStmt *stmt) {
    const Type *owner = resolve_type_expr(state, stmt->func_decl.owner, stmt->span);

    if (is_error_type(owner)) {
        return;
    }

    if (type_kind(owner) != TYPE_STRUCT) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function belongs to a struct this module declares, not to %s", type_name(state, owner));
        return;
    }

    TypeBinding *bound =
        type_name_of(owner) ? scope_binding_lookup_local(state->current_scope, type_name_of(owner)) : NULL;

    if (!bound || bound->def != type_decl(owner)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "cannot declare a function on '%s', which this module does not declare",
                   type_name(state, owner));
        return;
    }

    const Type *return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    if (reject_unsized(state, return_type, stmt->span, "returned")) {
        return_type = resolver_error_type(state);
    }

    stmt->func_decl.resolved_return_type = return_type;

    String *name = resolver_intern(state, stmt->func_decl.name);

    Binding *func = arena_alloc(resolver_owner_arena(state), sizeof(Binding));
    *func = (Binding){
        .kind = BINDING_FUNC,
        .scope_depth = state->current_scope->depth,
        .pinned = false,
        .func =
            {
                .return_type = return_type,
                .params = NULL,
                .param_count = 0,
                .func_index = FUNCTION_NO_BODY,
            },
    };

    size_t param_count = stmt->func_decl.params.size;

    if (param_count > 0) {
        func->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->func.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            func->func.params[i] = resolve_param_type(state, stmt->func_decl.params.data[i]);
        }
    }

    const MethodDecl method = {
        .name = name,
        .receiver = param_count > 0 ? func->func.params[0] : owner,
        .result = return_type,
        .params = func->func.params,
        .param_count = param_count,
        .function = &func->func,
    };

    if (!type_registry_declare_method(state->current_scope->type_registry, owner, &method)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a function '%s'",
                   type_name_of(owner)->data, name->data);
        return;
    }

    stmt->func_decl.binding = func;
}

static Function *resolve_qualified_func(ResolverState *state, ASTExpr *expr) {
    StringRef owner_ref, member_ref;

    if (!string_ref_split_colons(expr->var.name, &owner_ref, &member_ref)) {
        return NULL;
    }

    Resolution resolution = scope_resolve(state->current_scope, resolver_intern(state, owner_ref));

    if (resolution.kind == RESOLUTION_TYPE_DECL && resolution.def->param_count > 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' takes %zu type argument(s), not 0",
                   resolution.def->name->data, resolution.def->param_count);
        return NULL;
    }

    const Type *owner = resolution_type(state->current_scope->type_registry, resolution);

    if (!owner) {
        return NULL;
    }

    String *member = resolver_intern(state, member_ref);
    Function *found = type_registry_find_method(state->current_scope->type_registry, owner, member);

    if (!found) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no function '%s'",
                   type_name_of(owner)->data, member->data);

        return NULL;
    }

    return found;
}

static void declare_func(ResolverState *state, ASTStmt *stmt) {
    stmt->func_decl.declared = true;

    if (stmt->func_decl.owner) {
        declare_owned(state, stmt);
        return;
    }

    StringRef func_name = stmt->func_decl.name;
    const Type *func_return_type = resolve_type_expr(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = func_return_type;

    Binding *func =
        scope_decl_func(state->current_scope, resolver_intern(state, func_name), func_return_type);

    if (!func) {
        char *name = string_ref_to_cstr(func_name);
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                   name);
        free(name);
    }

    stmt->func_decl.binding = func;

    if (func) {
        func->func.is_extern = stmt->func_decl.body == NULL;

        if (func->func.is_extern) {
            func->func.name = resolver_intern(state, func_name);
            func->func.module = state->module_name;
        }
    }

    size_t param_count = stmt->func_decl.params.size;

    if (func && param_count > 0) {
        func->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->func.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            func->func.params[i] = resolve_param_type(state, param);
        }
    }
}

static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    size_t errors_before = diagnostics_count(state->diagnostics);

    resolver_enter_scope(state);

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);
        const Type *param_type = resolve_type_expr(state, param->type_expr, param->span);

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

    if (diagnostics_count(state->diagnostics) == errors_before) {
        size_t param_count = stmt->func_decl.params.size;
        Binding **params = arena_alloc(state->compile_arena, (param_count + 1) * sizeof(Binding *));
        size_t count = 0;

        for (size_t i = 0; i < param_count; i++) {
            params[count++] = stmt->func_decl.params.data[i]->binding;
        }

        flow_pass_run(state->compile_arena, state->current_scope->type_registry, stmt->func_decl.body, params,
                      count, stmt->func_decl.resolved_return_type, state->diagnostics);
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
            scope_decl_var(state->current_scope, resolver_intern(state, stmt->var_decl.name), type);

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

        if (stmt->func_decl.body) {
            resolve_func_body(state, stmt);
        }
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

        Binding *target = stmt->assign.target->binding;

        if (target && target->kind == BINDING_VAR) {
            if (stmt->assign.value->kind == EXPR_VARIABLE && stmt->assign.value->binding == target &&
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
                  Diagnostics *diagnostics) {
    ResolverState state = {
        .compile_arena = compile_arena,
        .global_scope = global_scope,
        .current_scope = global_scope,
        .module_scopes = module_scopes,
        .imports = &unit->imports,
        .module_name =
            unit->module_name.data ? string_from_ref(global_scope->strings, unit->module_name) : NULL,
        .func_context =
            {
                .return_type = NULL,
            },
        .struct_decls = struct_decl_list_create(),
        .resolving = struct_decl_list_create(),
        .diagnostics = diagnostics,
    };

    size_t errors_before = diagnostics_count(diagnostics);

    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

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
    }

    for (size_t i = 0; i < unit->statements.size; i++) {
        resolve_stmt(&state, unit->statements.data[i]);
    }

    struct_decl_list_free(&state.struct_decls);
    struct_decl_list_free(&state.resolving);

    return diagnostics_count(diagnostics) == errors_before;
}
