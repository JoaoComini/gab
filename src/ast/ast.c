#include "ast.h"

#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = ast_stmt_list_create();
    script->module_name = (StringRef){.data = NULL, .length = 0};
    script->module_span = (Span){0};

    return script;
}

void ast_script_destroy(ASTScript *script) {
    ast_stmt_list_free(&script->statements);

    free(script);
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    ast_stmt_list_add(&script->statements, stmt);
}

typedef struct {
    Type *return_type;

    // Enclosing loops, so 'break' and 'continue' can tell that they have one.
    // It sits here rather than on the resolver because a function body starts a
    // fresh count: a loop outside a declaration is not one the body can leave.
    unsigned int loop_depth;
} FuncContext;

typedef struct {
    // Dies with the compile: the scopes of blocks, which only codegen reads and
    // only while the compile is still running.
    Arena *compile_arena;

    Scope *global_scope;
    Scope *current_scope;

    ModuleScopeMap *module_scopes;

    FuncContext func_context;

    Diagnostics *diagnostics;
} ResolverState;

// Where anything reachable after the compile has to live. A struct Type goes
// into the TypeRegistry and a Symbol into the global scope's table, and both
// outlive every compile — so both are owned by whatever owns the global scope,
// which is the rule this derivation encodes: allocate from the arena of the
// thing that will own the result, never from whichever arena was passed in.
static Arena *resolver_owner_arena(ResolverState *state) { return state->global_scope->arena; }

static Type *resolver_error_type(ResolverState *state) {
    return type_registry_error_type(state->current_scope->type_registry);
}

static String *resolver_intern(ResolverState *state, StringRef ref) {
    return string_from_ref(state->current_scope->strings, ref);
}

// Splits 'Module::Type' into its two halves. Returns false for a bare name,
// which needs no splitting.
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

// The scope a type spec names: another module's for 'Module::Type', and the
// current one otherwise. NULL when the spec names a module that does not
// exist, which the caller reports as an unknown type.
static Scope *resolver_spec_scope(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (!string_ref_split_colons(name, &module, &member)) {
        return state->current_scope;
    }

    if (!state->module_scopes) {
        return NULL;
    }

    String *module_name = string_from_ref(state->current_scope->strings, module);
    Scope **existing = module_scope_map_lookup(state->module_scopes, module_name);

    return existing ? *existing : NULL;
}

// The half of a spec name a scope holds: 'Type' out of 'Module::Type'.
static String *resolver_spec_member(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (string_ref_split_colons(name, &module, &member)) {
        return string_from_ref(state->current_scope->strings, member);
    }

    return resolver_intern(state, name);
}

// A type that is already poisoned had its error reported at the origin, so any
// further check involving it silently succeeds rather than cascading.
static bool is_error_type(Type *type) { return !type || type->kind == TYPE_ERROR; }

// The printable form of a type. A pointer's name is derived from its pointee
// rather than stored, so '**Player' formats without interning two
// intermediate names. Built in the compile arena: only diagnostics ask, and
// they are already on the failing path.
static const char *type_name(ResolverState *state, Type *type) {
    if (!type) {
        return "none";
    }

    if (type->name) {
        return type->name->data;
    }

    const char *pointee = type_name(state, type->pointee);
    const char *prefix = type->is_ref ? "ref " : "*";
    size_t length = strlen(prefix) + strlen(pointee) + 1;
    char *out = arena_alloc(state->compile_arena, length);

    snprintf(out, length, "%s%s", prefix, pointee);

    return out;
}

void resolver_enter_scope(ResolverState *state) {
    state->current_scope =
        scope_create(state->compile_arena, state->current_scope->strings, state->current_scope);
}

void resolver_exit_scope(ResolverState *state) { state->current_scope = state->current_scope->parent; }

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

// The block depth of what a pointer-valued expression points at, or 0 when it
// points at nothing known. Comparing depths is what catches a pointer being
// moved somewhere that outlives its pointee: a smaller depth is a longer life.
//
// Flow-insensitive: a variable carries one depth, overwritten at each
// assignment, with no merge where branches rejoin. That is sound for the
// straight-line and if/else code the language has today, because a later
// assignment is the only thing that can change what a pointer names. A loop
// would break it — a back-edge can carry a depth from an iteration this has
// already walked past — so whichever lands second, loops or a precise analysis,
// has to reckon with the other.
static int pointee_depth(const ASTExpr *expr) {
    if (!expr) {
        return 0;
    }

    switch (expr->kind) {
    case EXPR_ADDR_OF: {
        const Symbol *symbol = expr->unary.target->symbol;

        return symbol ? symbol->scope_depth : 0;
    }
    case EXPR_VARIABLE:
        return expr->symbol ? expr->symbol->var.pointee_depth : 0;
    case EXPR_NEW:
        // A heap object outlives every frame, so 0 is the truth here rather
        // than the "unknown" the default stands for: it can be stored
        // anywhere, and the depth comparison already says so.
        return 0;
    case EXPR_CALL: {
        // A call handing back a 'ref T' hands back a borrow of something, and
        // that something can only have come from an argument: the callee's own
        // locals die with its frame, and returning a borrow of one is already
        // refused where the callee returns it.
        //
        // Which argument is not knowable without a per-function summary, so the
        // result is treated as borrowing from the shortest-lived of them. That
        // is conservative in one direction only — it can refuse a borrow of
        // something longer-lived than the deepest argument, never accept one
        // that dangles.
        //
        // An owned '*T' return is a heap object whatever it was made from, so
        // it outlives every frame and inherits nothing.
        if (!expr->type || !expr->type->is_ref) {
            return 0;
        }

        int deepest = 0;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            int depth = pointee_depth(expr->call.args.data[i]);

            if (depth > deepest) {
                deepest = depth;
            }
        }

        return deepest;
    }
    default:
        break;
    }

    return 0;
}

// The variable an address is ultimately taken from, so that '&v.x' pins v.
static Symbol *addressed_symbol(ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE:
        return expr->symbol;
    case EXPR_FIELD:
        return addressed_symbol(expr->field.target);
    default:
        break;
    }

    return NULL;
}

// Something with a home in memory whose address can be named: a variable, a
// field of one, or whatever a pointer already points at. A literal or a call
// result is a temporary and has no address to take.
static bool is_addressable(const ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_VARIABLE:
        return expr->symbol && expr->symbol->kind == SYMBOL_VAR;
    case EXPR_FIELD:
        return is_addressable(expr->field.target);
    case EXPR_DEREF:
        return true;
    default:
        return false;
    }
}

// The struct a receiver type names, looking through one level of pointer, or
// NULL if it does not name one. '*Player' and 'Player' share a method set, so
// both land on the same Type.
static Type *receiver_base_type(Type *type) {
    if (type_is_pointer(type)) {
        type = type->pointee;
    }

    return (type && type->kind == TYPE_STRUCT) ? type : NULL;
}

// Checks each argument against the parameter type in the same position. Shared
// by both call forms, which differ only in where their parameter list starts:
// a method's skips the receiver.
static bool type_accepts(Type *to, Type *from);

static void check_call_args(ResolverState *state, ASTExprList *args, Type **params) {
    for (size_t i = 0; i < args->size; i++) {
        ASTExpr *arg = args->data[i];
        Type *param_type = params[i];

        if (is_error_type(arg->type) || is_error_type(param_type)) {
            continue;
        }

        // type_accepts rather than identity, so an owned '*T' fills a 'ref T'
        // parameter: lending is what a borrow parameter asks for, and the
        // caller goes on owning what it lent.
        if (!type_accepts(param_type, arg->type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, arg->span, "argument %zu is %s, but %s was declared",
                       i + 1, type_name(state, arg->type), type_name(state, param_type));
        }
    }
}

// Settles how the receiver reaches parameter zero, recording it on the node for
// codegen. Go's rule: a pointer method on an addressable value takes its
// address, and a value method through a pointer copies the pointee in. Returns
// false when neither is possible.
// Rewrites 'recv.m(a)' into 'm(recv', a)', where recv' is the receiver adjusted
// to what parameter zero declared: '&recv' where the method takes a pointer and
// the receiver is a value, '*recv' the other way round.
//
// After this there is no method call left in the tree — only a call whose first
// argument happens to be a receiver, which is exactly what parameter zero has
// always been. Codegen therefore has one call path rather than two near-copies
// of one, and anything later that reasons about arguments sees the receiver
// among them without knowing to look for it.
//
// The adjustment is a real node rather than a flag because the tree is the only
// place it can be honestly recorded: '&recv' is an expression, and every pass
// that walks expressions should see it as one. Rust and Go both lower method
// calls this way, and for this reason.
//
// Runs after the call is fully checked, so the diagnostics above all report
// against what the user wrote rather than against the rewrite.
// How a receiver reaches parameter zero. A '*T' method called on a 'T' takes
// its address; a 'T' method called through a '*T' copies the pointee in.
typedef enum {
    RECEIVER_AS_IS,
    RECEIVER_ADDRESS_OF,
    RECEIVER_DEREF,
} ReceiverAdjustment;

// Rewrites 'recv.m(a)' — parsed as a call over the field expression 'recv.m' —
// into 'm(recv', a)', where recv' is the receiver adjusted to what parameter
// zero declared.
//
// After this there is no method call left in the tree, and no node kind for one
// either: only a call whose first argument happens to be a receiver, which is
// exactly what parameter zero has always been. Codegen therefore has one call
// path rather than two near-copies of one, and anything later that reasons
// about arguments sees the receiver among them without knowing to look.
//
// The adjustment is a real node rather than a flag because the tree is the only
// place it can be honestly recorded: '&recv' is an expression, and every pass
// that walks expressions should see it as one. Rust and Go both lower method
// calls this way, and for this reason.
//
// Runs after the call is fully checked, so every diagnostic above reports
// against what the user wrote rather than against the rewrite.
static void lower_method_call(ASTExpr *expr, Symbol *method, ReceiverAdjustment adjustment) {
    ASTExpr *target = expr->call.target;

    // Lifted out of the field node before it is freed: the field held the
    // receiver, and the call is about to hold it directly.
    ASTExpr *receiver = target->field.target;
    Span span = receiver->span;

    target->field.target = NULL;
    ast_expr_free(target);

    switch (adjustment) {
    case RECEIVER_ADDRESS_OF:
        receiver = ast_addr_of_expr_create(span, receiver);
        receiver->type = method->func.params[0];
        break;
    case RECEIVER_DEREF:
        receiver = ast_deref_expr_create(span, receiver);
        receiver->type = method->func.params[0];
        break;
    case RECEIVER_AS_IS:
        break;
    }

    // A fresh list, since the receiver has to lead and the list only appends.
    ASTExprList args = ast_expr_list_create();
    ast_expr_list_add(&args, receiver);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        ast_expr_list_add(&args, expr->call.args.data[i]);
    }

    // The old list's storage only, never its elements: every one of them is now
    // held by the new list, and the receiver by the adjustment node above it.
    ast_expr_list_free(&expr->call.args);

    expr->call.target = NULL;
    expr->call.args = args;
    expr->symbol = method;
}

// How the receiver has to be adjusted to reach parameter zero, reported through
// 'out'. Returns false when no adjustment bridges the two, having said why.
static bool reconcile_receiver(ResolverState *state, ASTExpr *expr, ASTExpr *receiver, Type *declared,
                               Type *actual, const String *name, ReceiverAdjustment *out) {
    if (declared == actual) {
        *out = RECEIVER_AS_IS;
        return true;
    }

    if (type_is_pointer(declared) && !type_is_pointer(actual)) {
        if (!is_addressable(receiver)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
            return false;
        }

        // The address is loose for the duration of the call, so the slot it
        // names must survive the whole block — exactly as for '&x'.
        Symbol *addressed = addressed_symbol(receiver);
        if (addressed) {
            addressed->pinned = true;
        }

        *out = RECEIVER_ADDRESS_OF;
        return true;
    }

    if (!type_is_pointer(declared) && type_is_pointer(actual)) {
        *out = RECEIVER_DEREF;
        return true;
    }

    // An owned '*T' calling a 'ref T' method, which is the only pointer-to-
    // pointer case left: a receiver is declared 'ref T' or it is not a pointer
    // at all, so 'declared' is never an owning pointer here.
    //
    // Lending is what the method asked for, and the caller goes on owning what
    // it lent. The same widening type_accepts allows for any argument — which
    // is what a receiver is — and 'declared == actual' above does not catch it
    // only because the two are distinct interned types.
    if (type_accepts(declared, actual)) {
        *out = RECEIVER_AS_IS;
        return true;
    }

    // A '**Player', or some other shape no coercion bridges.
    diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot call '%s' on %s", name->data,
               type_name(state, actual));
    return false;
}

void ast_script_expr_visit(ResolverState *state, ASTExpr *expr);

// A call whose target is a field expression: 'recv.m(args)'. Resolves the
// method against the receiver's type, checks the call, and lowers the whole
// thing into an ordinary call with the receiver as argument zero.
//
// The target is never visited as a field expression — 'm' names a method, and
// the struct has no field by it — so the receiver inside it is what gets
// walked.
static void resolve_method_call(ResolverState *state, ASTExpr *expr) {
    ASTExpr *receiver = expr->call.target->field.target;
    StringRef name = expr->call.target->field.name;

    ast_script_expr_visit(state, receiver);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        ast_script_expr_visit(state, expr->call.args.data[i]);
    }

    Type *receiver_type = receiver->type;

    if (is_error_type(receiver_type)) {
        expr->type = resolver_error_type(state);
        return;
    }

    // '*Player' and 'Player' share one method set, so the lookup and every
    // message below name the struct rather than what was written.
    Type *base = receiver_base_type(receiver_type);

    if (!base) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "%s is not a struct, so it has no methods",
                   type_name(state, receiver_type));
        expr->type = resolver_error_type(state);
        return;
    }

    String *method_name = resolver_intern(state, name);
    Symbol *method = type_find_method(base, method_name);

    if (!method) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no method '%s'", base->name->data,
                   method_name->data);
        expr->type = resolver_error_type(state);
        return;
    }

    // Parameter zero is the receiver, so the declared parameters — the ones the
    // caller actually writes — are everything after it.
    Type *declared_receiver = method->func.params[0];
    size_t declared_params = method->func.param_count - 1;

    ReceiverAdjustment adjustment;

    if (!reconcile_receiver(state, expr, receiver, declared_receiver, receiver_type, method_name,
                            &adjustment)) {
        expr->type = resolver_error_type(state);
        return;
    }

    // The count the user sees excludes the receiver, which they never wrote.
    if (expr->call.args.size != declared_params) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                   declared_params, expr->call.args.size);
        expr->type = resolver_error_type(state);
        return;
    }

    check_call_args(state, &expr->call.args, method->func.params + 1);

    lower_method_call(expr, method, adjustment);

    expr->type = method->func.return_type;
}

// Whether a value of 'from' may be stored where 'to' is expected. Identity for
// everything except the one conversion the language allows: an owned '*T' may
// be stored where a 'ref T' is expected, since giving something up to be named
// costs nothing and is how a borrowing field is ever populated.
//
// The reverse is deliberately absent: promoting 'ref T' to '*T' would hand out
// ownership nobody granted, and the object already has an owner that will free
// it.
static bool type_accepts(Type *to, Type *from) {
    if (to == from) {
        return true;
    }

    return type_is_pointer(to) && type_is_pointer(from) && to->is_ref && !from->is_ref &&
           to->pointee == from->pointee;
}

bool is_numeric_type(Type *t) { return t->kind == TYPE_INT || t->kind == TYPE_FLOAT; }

bool is_boolean_type(Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

bool is_comparable_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

Type *ast_script_resolve_type(ResolverState *state, TypeSpec *spec, Span span);

void ast_script_expr_visit(ResolverState *state, ASTExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP: {
        ast_script_expr_visit(state, expr->bin_op.left);
        ast_script_expr_visit(state, expr->bin_op.right);

        Type *left_type = expr->bin_op.left->type;
        Type *right_type = expr->bin_op.right->type;

        if (is_error_type(left_type) || is_error_type(right_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        if (left_type != right_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot apply '%s' to %s and %s",
                       op_name, type_name(state, left_type), type_name(state, right_type));
            expr->type = resolver_error_type(state);
            break;
        }

        switch (expr->bin_op.op) {
        case BIN_OP_ADD:
        case BIN_OP_SUB:
        case BIN_OP_MUL:
        case BIN_OP_DIV:
            if (!is_numeric_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires a numeric type, found %s", op_name, type_name(state, left_type));
                expr->type = resolver_error_type(state);
                return;
            }

            expr->type = left_type;
            return;
        case BIN_OP_EQUAL:
        case BIN_OP_NEQUAL:
            if (!is_comparable_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' is not supported for %s",
                           op_name, type_name(state, left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        case BIN_OP_LESS:
        case BIN_OP_GREATER:
        case BIN_OP_LEQUAL:
        case BIN_OP_GEQUAL:
            if (!is_ordered_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires an ordered type, found %s", op_name, type_name(state, left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        case BIN_OP_AND:
        case BIN_OP_OR:
            if (!is_boolean_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires a boolean type, found %s", op_name, type_name(state, left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        }

        expr->type = type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL);
        break;
    }
    case EXPR_VARIABLE: {
        Symbol *entry = scope_symbol_lookup(state->current_scope, resolver_intern(state, expr->var.name));

        if (!entry) {
            char *name = string_ref_to_cstr(expr->var.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "undeclared variable '%s'", name);
            free(name);

            expr->type = resolver_error_type(state);
            break;
        }

        expr->symbol = entry;
        expr->type = entry->var.type;
        break;
    }
    case EXPR_CALL: {
        // 'recv.m(args)' parsed as a call over the field expression 'recv.m'.
        // The target is not a field and must not be visited as one — 'm' is a
        // method name, and a struct has no field by it — so the method path
        // takes over before anything walks it.
        if (expr->call.target && expr->call.target->kind == EXPR_FIELD) {
            resolve_method_call(state, expr);
            break;
        }

        ast_script_expr_visit(state, expr->call.target);

        for (size_t i = 0; i < expr->call.args.size; i++) {
            ast_script_expr_visit(state, expr->call.args.data[i]);
        }

        Symbol *callee = expr->call.target->symbol;

        if (!callee) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (callee->kind != SYMBOL_FUNC) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "this expression is not callable");
            expr->type = resolver_error_type(state);
            break;
        }

        if (expr->call.args.size != callee->func.param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                       callee->func.param_count, expr->call.args.size);
            expr->type = resolver_error_type(state);
            break;
        }

        check_call_args(state, &expr->call.args, callee->func.params);

        expr->symbol = callee;
        expr->type = callee->func.return_type;
        break;
    }
    case EXPR_FIELD: {
        ast_script_expr_visit(state, expr->field.target);

        Type *target_type = expr->field.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // 'p.health' where p is a '*Player' reaches through the pointer, as in
        // Go and C's '->'. One level only: a '**Player' has to be written out.
        if (type_is_pointer(target_type)) {
            target_type = target_type->pointee;
        }

        if (target_type->kind != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s is not a struct, so it has no fields", type_name(state, expr->field.target->type));
            expr->type = resolver_error_type(state);
            break;
        }

        String *field_name = resolver_intern(state, expr->field.name);
        const TypeField *field = type_find_field(target_type, field_name);

        if (!field) {
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no field '%s'",
                       type_name(state, target_type), field_name->data);
            expr->type = resolver_error_type(state);
            break;
        }

        expr->field.field = field;
        expr->type = field->type;

        // Field access addresses the target's slots, so it inherits the
        // target's symbol and stays assignable through the chain.
        expr->symbol = expr->field.target->symbol;
        break;
    }
    case EXPR_ADDR_OF: {
        ast_script_expr_visit(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

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

        // '&o' where 'o' owns would be a borrow of an owning pointer, and
        // 'ref *T' cannot be written: 'ref' does not combine with '*'. Producing
        // a value nothing can name is worse than refusing it here, where the
        // mistake is.
        //
        // That type is what an out-parameter would need — a borrow of the
        // caller's variable rather than of the object, so the callee could
        // repoint it. Which is more than a spelling: assigning through one would
        // free the caller's old object from inside the callee, an owning slot
        // changing owner mid-call. Returning ownership says the same thing with
        // the transfer visible at the call site.
        if (type_is_pointer(target_type) && !target_type->is_ref) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of an owning pointer; return ownership instead of "
                       "repointing it through a borrow");
            expr->type = resolver_error_type(state);
            break;
        }

        // The slot must survive the whole block now that its address is loose,
        // so codegen may not reclaim it at the end of the statement.
        Symbol *addressed = addressed_symbol(expr->unary.target);
        if (addressed) {
            addressed->pinned = true;
        }

        // '&x' is a borrow, and 'ref T' is what a borrow is spelled. The slot it
        // names is owned by whoever declared it — a stack local owns itself, and
        // freeing through this address would free something 'new' never made.
        //
        // Typing it '*T' would make one type mean two things: an address of
        // something, and ownership of a heap object. Nothing could then tell
        // them apart from the type alone.
        expr->type = type_registry_pointer_to_kind(state->current_scope->type_registry, target_type, true);
        break;
    }
    case EXPR_DEREF: {
        ast_script_expr_visit(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (!type_is_pointer(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot dereference %s",
                       type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->type = target_type->pointee;

        // The address itself lives in the target's slots, so a deref stays
        // assignable through whatever the target was.
        expr->symbol = expr->unary.target->symbol;
        break;
    }
    case EXPR_NEG: {
        ast_script_expr_visit(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // Bool is ordered and comparable but has no negation, so this asks
        // for numeric rather than reusing either of those.
        if (!is_numeric_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "unary '-' requires a numeric type, found %s", type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        // The result is a fresh value, so it deliberately inherits no symbol:
        // '-x' is a temporary and must not be assignable or addressable.
        expr->type = target_type;
        break;
    }
    case EXPR_NEW: {
        Type *type = ast_script_resolve_type(state, expr->new_expr.type_spec, expr->span);

        if (is_error_type(type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // Only a struct has a layout to allocate. 'new int' would be a boxed
        // scalar, which is a different feature and not this one.
        if (type->kind != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; 'new' takes a struct", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->new_expr.type = type;
        expr->type = type_registry_pointer_to(state->current_scope->type_registry, type);
        break;
    }
    case EXPR_LITERAL: {
        expr->type = type_registry_get_builtin(state->current_scope->type_registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

// Rejects a pointer being stored somewhere that outlives what it points at.
// 'target_depth' is the block depth of the destination; a pointee declared
// deeper than that is gone by the time the destination can still be read.
//
// The rule is block-scoped rather than function-scoped because register reuse
// reclaims slots at the closing brace, so a pointer into an inner block dangles
// into a reused slot as soon as that block ends.
static void check_pointer_lifetime(ResolverState *state, ASTExpr *value, int target_depth, Span span,
                                   const char *what) {
    if (!value || !type_is_pointer(value->type)) {
        return;
    }

    int depth = pointee_depth(value);

    // 0 means the pointee is unknown, which is not evidence of a problem.
    if (depth == 0 || depth <= target_depth) {
        return;
    }

    diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
               "this pointer outlives what it points at, so it cannot be %s", what);
}

// Returns NULL when there is no spec to resolve (an omitted type), and the
// poison type when the spec names something that does not exist.
Type *ast_script_resolve_type(ResolverState *state, TypeSpec *spec, Span span) {
    if (!spec) {
        return NULL;
    }

    Scope *scope = resolver_spec_scope(state, spec->name);

    // Walks outward from that scope, so a module's own 'Config' shadows a
    // root-namespace one and 'int' resolves with no import. A 'Module::Type'
    // spec resolved to that module's scope above, and its bare member name is
    // what that scope holds.
    Type *type = scope ? scope_type_lookup(scope, resolver_spec_member(state, spec->name)) : NULL;

    if (!type) {
        char *name = string_ref_to_cstr(spec->name);
        diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
        free(name);

        return resolver_error_type(state);
    }

    // Interned in the one shared registry whichever scope named the pointee,
    // so '*Config' is one type however many modules mention it.
    //
    // Every level is a borrow or none is, since the parser rejects the two
    // spellings mixed: 'ref ref T' is a borrow of a borrow, and '**T' an owning
    // pointer to an owning pointer.
    for (unsigned int i = 0; i < spec->pointer_depth; i++) {
        type = type_registry_pointer_to_kind(state->current_scope->type_registry, type, spec->is_ref);
    }

    return type;
}

void ast_script_stmt_visit(ResolverState *state, ASTStmt *stmt);

// Declares the struct's Type: its name, its fields, and its layout. Split from
// the body walk so the pre-pass can run it over a whole script's top level
// before any signature mentions a type.
static void declare_struct(ResolverState *state, ASTStmt *stmt) {
    stmt->struct_decl.declared = true;

    // Declared under its bare name into the scope it appears in, so two
    // modules may each declare a 'Config' without either name carrying the
    // module in it.
    String *struct_name = resolver_intern(state, stmt->struct_decl.name);

    // Local: shadowing an outer type is allowed, declaring the same name
    // twice in one scope is not.
    if (scope_declares_type_now(state->current_scope, struct_name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return;
    }

    Type *type = type_struct_create(resolver_owner_arena(state), struct_name, stmt->struct_decl.fields.size);

    // Registered under its name *before* its fields resolve, so that a field
    // pointing at the struct being declared finds it. A scene graph is exactly
    // this shape — 'struct Node { parent: ref Node, child: *Node }' — and
    // without this it fails with "unknown type", which the containment check
    // below was already written expecting not to happen.
    //
    // Safe because only a pointer to self can appear: a struct containing
    // itself by value is rejected below, before its size is ever needed, and
    // the layout is computed only once every field has resolved.
    scope_decl_type(state->current_scope, struct_name, type);

    bool poisoned = false;

    for (size_t i = 0; i < stmt->struct_decl.fields.size; i++) {
        ASTField *field = stmt->struct_decl.fields.data[i];
        String *field_name = resolver_intern(state, field->name);

        if (type_find_field(type, field_name)) {
            diag_error(state->diagnostics, GAB_ERR_NAME, field->span, "duplicate field '%s' in struct '%s'",
                       field_name->data, struct_name->data);
            poisoned = true;
            continue;
        }

        // The struct is registered only after its fields resolve, so a
        // self-reference would otherwise surface as "unknown type". A
        // pointer to self is not containment, so only depth 0 is rejected.
        if (field->type_spec->pointer_depth == 0 &&
            string_ref_equals_ref(field->type_spec->name, stmt->struct_decl.name)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, field->span, "struct '%s' cannot contain itself",
                       struct_name->data);
            poisoned = true;
            continue;
        }

        Type *field_type = ast_script_resolve_type(state, field->type_spec, field->span);

        if (is_error_type(field_type)) {
            poisoned = true;
            continue;
        }

        type_add_field(type, field_name, field_type);
    }

    // Registered before its fields resolved, so a struct that failed has to be
    // taken back out: with no layout computed, anything naming it would read a
    // size of zero.
    if (poisoned) {
        scope_withdraw_type(state->current_scope, struct_name);
        return;
    }

    type_layout_compute(type);

    stmt->struct_decl.type = type;
}

// Declares the function's name, return type, and parameter types — everything a
// caller needs — without touching the body. Split from the body walk so the
// pre-pass can declare a whole script's top level before resolving any of it,
// Resolves one parameter's declared type, refusing an owning '*T'.
//
// A parameter never owns what it is given. The caller keeps owning it across
// the call, no callee frees one — codegen_own_slot is never reached for a
// parameter slot — and none may be stored where something else would own it,
// since an owning field takes only 'new' or a call's result. So '*T' here would
// spell an ownership that cannot happen, and would let one signature be written
// two ways that behave identically.
//
// It is also how a borrow was laundered into an owned return, which the
// sanitizer caught as a use-after-free: 'func f(b: *Box): *Box { return b; }'
// handed the caller a second owner of what it already owned. With no owning
// parameter there is nothing to launder.
//
// '*T' keeps its meaning everywhere a slot can outlive the statement and free
// what it holds: 'let', a struct field, 'new', and a return type.
static Type *resolve_param_type(ResolverState *state, ASTField *param) {
    Type *type = ast_script_resolve_type(state, param->type_spec, param->span);

    if (!is_error_type(type) && type_is_pointer(type) && !type->is_ref) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, param->span,
                   "a parameter borrows rather than owning, so write 'ref %s' instead of '*%s'",
                   type_name(state, type->pointee), type_name(state, type->pointee));

        return resolver_error_type(state);
    }

    return type;
}

// which is what lets a function call one declared below it.
// Declares a method into its receiver type's method map, rather than into any
// scope: a method has no free-standing name, so 'Player.update' and
// 'Enemy.update' coexist and neither is reachable as a bare 'update'.
//
// The Symbol is an ordinary SYMBOL_FUNC whose parameter zero is the receiver.
// That is what makes the call free at runtime: codegen puts the receiver in the
// first argument slot and emits the OP_CALL it already would.
static void declare_method(ResolverState *state, ASTStmt *stmt) {
    ASTField *receiver = stmt->func_decl.receiver;

    Type *receiver_type = ast_script_resolve_type(state, receiver->type_spec, receiver->span);

    if (is_error_type(receiver_type)) {
        return;
    }

    Type *base = receiver_base_type(receiver_type);

    if (!base) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, receiver->span,
                   "a method's receiver must be a struct or a pointer to one, found %s",
                   type_name(state, receiver_type));
        return;
    }

    // A method never owns its receiver. It is handed one for the duration of
    // the call and frees nothing — no callee frees a parameter — so declaring
    // one '*T' would spell an ownership the method cannot have, and would let
    // the same method be written two ways that behave identically.
    //
    // 'ref T' is the form that says what is true. A receiver by value stays
    // available as 'T', which copies; the two axes are separate, and only this
    // one is about ownership.
    if (type_is_pointer(receiver_type) && !receiver_type->is_ref) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, receiver->span,
                   "a method borrows its receiver rather than owning it, so write 'ref %s' instead of '*%s'",
                   base->name->data, base->name->data);
        return;
    }

    // Go's rule, and it keeps ownership of the method map unambiguous: a type
    // and its methods are made by one compile and replaced together on reload.
    if (!scope_declares_type_now(state->current_scope, base->name)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, receiver->span,
                   "cannot declare a method on '%s', which this module does not declare", base->name->data);
        return;
    }

    Type *return_type = ast_script_resolve_type(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = return_type;

    String *method_name = resolver_intern(state, stmt->func_decl.name);

    // Not scope_decl_func: this name lives on the type, not in a scope. The
    // Symbol is built directly for the same reason.
    Symbol *method = arena_alloc(resolver_owner_arena(state), sizeof(Symbol));
    *method = (Symbol){
        .kind = SYMBOL_FUNC,
        .scope_depth = state->current_scope->depth,
        .generation = state->current_scope->generation,
        .pinned = false,
        .func =
            {
                .return_type = return_type,
                .params = NULL,
                .param_count = 0,
                .proto_index = SYMBOL_FUNC_NO_PROTO,
            },
    };

    // The receiver is parameter zero, so the declared parameters shift up one.
    size_t param_count = stmt->func_decl.params.size + 1;

    method->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(Type *));
    method->func.param_count = param_count;
    method->func.params[0] = receiver_type;

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        method->func.params[i + 1] = resolve_param_type(state, param);
    }

    if (!type_add_method(resolver_owner_arena(state), base, method_name, method)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a method '%s'",
                   base->name->data, method_name->data);
        return;
    }

    stmt->func_decl.symbol = method;
}

static void declare_func(ResolverState *state, ASTStmt *stmt) {
    stmt->func_decl.declared = true;

    if (stmt->func_decl.receiver) {
        declare_method(state, stmt);
        return;
    }

    StringRef func_name = stmt->func_decl.name;
    Type *func_return_type = ast_script_resolve_type(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = func_return_type;

    Symbol *func = scope_decl_func(state->current_scope, resolver_intern(state, func_name), func_return_type);

    if (!func) {
        char *name = string_ref_to_cstr(func_name);
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                   name);
        free(name);
    }

    stmt->func_decl.symbol = func;

    size_t param_count = stmt->func_decl.params.size;

    if (func && param_count > 0) {
        func->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(Type *));
        func->func.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            func->func.params[i] = resolve_param_type(state, param);
        }
    }
}

// Walks the body in a scope holding the parameters. The signature is already
// resolved, so this re-resolves each parameter's TypeSpec only to bind its
// name; the types a caller sees were settled by declare_func.
static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    resolver_enter_scope(state);

    // The receiver is an ordinary local in the body's scope, so 'p.health'
    // resolves through the existing field path — including the auto-deref that
    // a '*Player' receiver needs.
    ASTField *receiver = stmt->func_decl.receiver;

    if (receiver) {
        Type *receiver_type = ast_script_resolve_type(state, receiver->type_spec, receiver->span);

        receiver->symbol =
            scope_decl_var(state->current_scope, resolver_intern(state, receiver->name), receiver_type);
    }

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);
        Type *param_type = ast_script_resolve_type(state, param->type_spec, param->span);

        Symbol *symbol = scope_decl_var(state->current_scope, param_name, param_type);

        if (!symbol) {
            char *name = string_ref_to_cstr(param->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, param->span, "duplicate parameter '%s'", name);
            free(name);
            continue;
        }

        param->symbol = symbol;
    }

    FuncContext previous_context = state->func_context;

    state->func_context.return_type = stmt->func_decl.resolved_return_type;

    ast_script_stmt_visit(state, stmt->func_decl.body);

    state->func_context = previous_context;

    resolver_exit_scope(state);
}

void ast_script_stmt_visit(ResolverState *state, ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR: {
        ast_script_expr_visit(state, stmt->expr.value);
        break;
    }
    case STMT_VAR_DECL: {
        ast_script_expr_visit(state, stmt->var_decl.initializer);

        Type *type;
        if (stmt->var_decl.type_spec) {
            Type *decl_type = ast_script_resolve_type(state, stmt->var_decl.type_spec, stmt->span);

            if (stmt->var_decl.initializer) {
                Type *init_type = stmt->var_decl.initializer->type;

                if (!is_error_type(decl_type) && !is_error_type(init_type) &&
                    !type_accepts(decl_type, init_type)) {
                    diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                               "cannot initialize a variable of type %s with a value of type %s",
                               type_name(state, decl_type), type_name(state, init_type));
                    decl_type = resolver_error_type(state);
                }
            }

            type = decl_type;
        } else if (stmt->var_decl.initializer) {
            type = stmt->var_decl.initializer->type;
        } else {
            type = resolver_error_type(state);
        }

        Symbol *var = scope_decl_var(state->current_scope, resolver_intern(state, stmt->var_decl.name), type);

        if (!var) {
            char *name = string_ref_to_cstr(stmt->var_decl.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                       name);
            free(name);
            break;
        }

        // A declaration is always at the current depth, so it can never outlive
        // its initializer; what it records is the depth, for later assignments
        // and returns to check against.
        var->var.pointee_depth = pointee_depth(stmt->var_decl.initializer);

        stmt->var_decl.symbol = var;
        break;
    }
    case STMT_FUNC_DECL: {
        // The signature is already declared: at the top level by the pre-pass,
        // and here for a nested function, whose declaration nothing above it
        // could have seen.
        if (!stmt->func_decl.declared) {
            declare_func(state, stmt);
        }

        resolve_func_body(state, stmt);
        break;
    }
    case STMT_STRUCT_DECL: {
        if (!stmt->struct_decl.declared) {
            declare_struct(state, stmt);
        }
        break;
    }
    case STMT_ASSIGN: {
        ast_script_expr_visit(state, stmt->assign.target);
        ast_script_expr_visit(state, stmt->assign.value);

        Type *target_type = stmt->assign.target->type;
        Type *value_type = stmt->assign.value->type;

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !type_accepts(target_type, value_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot assign a value of type %s to a target of type %s",
                       type_name(state, value_type), type_name(state, target_type));
            break;
        }

        // Storing through a pointer reaches something whose lifetime this frame
        // does not bound — a heap object outlives every frame, and a '*T'
        // parameter may point at a caller's. Depth 0 is that bound: "outlives
        // everything", so only a pointer to something equally long-lived may be
        // stored there. Without this, '&local' escapes into a heap object and
        // dangles the moment the frame returns, ownership notwithstanding.
        if (stmt->assign.target->kind == EXPR_FIELD || stmt->assign.target->kind == EXPR_DEREF) {
            check_pointer_lifetime(state, stmt->assign.value, 0, stmt->span, "stored here");
            break;
        }

        Symbol *target = stmt->assign.target->symbol;

        if (target && target->kind == SYMBOL_VAR) {
            check_pointer_lifetime(state, stmt->assign.value, target->scope_depth, stmt->span,
                                   "assigned here");

            // The variable now points at whatever was just stored in it.
            target->var.pointee_depth = pointee_depth(stmt->assign.value);
        }
        break;
    }
    case STMT_IF: {
        ast_script_expr_visit(state, stmt->ifstmt.condition);

        // A branch has to have something to branch on, and bool is the only
        // thing the jump opcodes read. An already-poisoned condition has
        // reported its own error, so it is let through rather than complained
        // about twice.
        Type *condition_type = stmt->ifstmt.condition->type;

        if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->ifstmt.condition->span,
                       "'if' requires a boolean condition, found %s", type_name(state, condition_type));
        }

        ast_script_stmt_visit(state, stmt->ifstmt.then_block);
        ast_script_stmt_visit(state, stmt->ifstmt.else_block);
        break;
    }
    case STMT_FOR: {
        // The initializer's scope encloses the condition, the post clause, and
        // the body, so 'for let i = 0; i < n; i = i + 1' scopes i to the loop.
        resolver_enter_scope(state);

        ast_script_stmt_visit(state, stmt->forstmt.init);

        if (stmt->forstmt.condition) {
            ast_script_expr_visit(state, stmt->forstmt.condition);

            Type *condition_type = stmt->forstmt.condition->type;

            if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->forstmt.condition->span,
                           "'for' requires a boolean condition, found %s", type_name(state, condition_type));
            }
        }

        state->func_context.loop_depth++;
        ast_script_stmt_visit(state, stmt->forstmt.body);
        state->func_context.loop_depth--;

        // Visited after the body, matching when it runs, though it is the
        // initializer's scope either way.
        ast_script_stmt_visit(state, stmt->forstmt.post);

        resolver_exit_scope(state);
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
        resolver_enter_scope(state);

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            ast_script_stmt_visit(state, stmt->block.list.data[i]);
        }

        resolver_exit_scope(state);
        break;
    }
    case STMT_RETURN: {
        ast_script_expr_visit(state, stmt->ret.result);

        Type *expected = state->func_context.return_type;
        Type *actual = stmt->ret.result ? stmt->ret.result->type : NULL;

        // A NULL type here means "no value", which is a distinct case from a
        // poisoned one: it must still be checked against the declared type.
        bool poisoned = (expected && expected->kind == TYPE_ERROR) || (actual && actual->kind == TYPE_ERROR);

        // type_accepts once both are present, so a function declaring 'ref T'
        // may return an owned '*T' — it lends what it was given rather than
        // handing ownership out. A NULL on either side is "no value", which
        // only identity settles.
        bool accepted = actual && expected ? type_accepts(expected, actual) : actual == expected;

        if (!poisoned && !accepted) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "returns %s, but %s was declared",
                       type_name(state, actual), type_name(state, expected));
            break;
        }

        // A returned pointer outlives the whole frame, so nothing declared
        // inside the function may be pointed at. Depth 0 is the global scope.
        check_pointer_lifetime(state, stmt->ret.result, 0, stmt->span, "returned");
        break;
    }
    }
}

bool ast_script_resolve(Arena *compile_arena, ASTScript *script, Scope *global_scope,
                        ModuleScopeMap *module_scopes, Diagnostics *diagnostics) {
    ResolverState state = {
        .compile_arena = compile_arena,
        .global_scope = global_scope,
        .current_scope = global_scope,
        .module_scopes = module_scopes,
        .func_context =
            {
                .return_type = NULL,
            },
        .diagnostics = diagnostics,
    };

    size_t errors_before = diagnostics_count(diagnostics);

    // Declarations first, bodies second. A signature may name a type declared
    // further down the file, and a body may call a function declared further
    // down, so neither can be resolved in the order it was written. Go and
    // Rust both hoist for the same reason.
    //
    // Types before functions: a signature names types, no type names a
    // function. This pass resolves signatures only — never a body — so nothing
    // about scoping or block depth depends on it.
    for (size_t i = 0; i < script->statements.size; i++) {
        ASTStmt *stmt = script->statements.data[i];

        if (stmt && stmt->kind == STMT_STRUCT_DECL) {
            declare_struct(&state, stmt);
        }
    }

    for (size_t i = 0; i < script->statements.size; i++) {
        ASTStmt *stmt = script->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            declare_func(&state, stmt);
        }
    }

    for (size_t i = 0; i < script->statements.size; i++) {
        ast_script_stmt_visit(&state, script->statements.data[i]);
    }

    return diagnostics_count(diagnostics) == errors_before;
}
