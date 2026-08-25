#include "ast/resolve.h"

#include "ast/flow_pass.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Type *return_type;

    // Enclosing loops, so 'break' and 'continue' can tell that they have one.
    unsigned int loop_depth;
} FuncContext;

typedef struct {
    // Dies with the compile: the scopes of blocks, which only codegen reads and
    // only while the compile is still running.
    Arena *compile_arena;

    Scope *global_scope;
    Scope *current_scope;

    ModuleScopeMap *module_scopes;

    // The modules this unit imported, and its own. A qualified reference to
    // anything outside this set is an error even when the module exists: what a
    // unit may name is what it said it would.
    const ASTImportList *imports;

    // The module this unit declares into, or NULL for the default one. Only
    // an extern records it, so that a host binds a body to the same module the
    // declaration lives in.
    String *module_name;

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

// Whether this unit may name that module: its own, or one it imported. A module
// it did not import is refused even where it exists, so that the units a unit
// depends on are the ones it says it depends on.
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

    if (!resolver_may_name(state, module_name)) {
        return NULL;
    }

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

// The printable form of a type. A pointer's name is derived from its inner
// rather than stored, so 'box box Player' formats without interning two
// intermediate names. Built in the compile arena: only diagnostics ask, and
// they are already on the failing path.
static const char *type_name(ResolverState *state, Type *type) {
    if (!type) {
        return "none";
    }

    // A borrowing string keeps the name of what it borrows, so the 'ref' has to
    // be put back or a mismatch between the two reads as 'string' and 'string'.
    if (type->name && !(type->kind == TYPE_STRING && type->is_ref)) {
        return type->name->data;
    }

    const char *inner = type->name ? type->name->data : type_name(state, type->inner);
    const char *prefix = type->is_ref ? "ref " : "box ";
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
    case BIN_OP_CONCAT:
        return "..";
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

// Replaces an arithmetic operator over two literals with the literal it
// computes. Runs bottom-up with resolution, so an inner fold makes its parent
// foldable in turn and '2 + 3 * 4' reaches codegen as one constant.
//
// Here rather than in codegen because the tree is where a rewrite belongs: every
// pass after this one sees a literal, and neither of the two codegen paths into
// a binary op has to know that folding exists.
//
// Arithmetic only. A comparison yields a bool, which has no literal form for the
// result, and a string operand names an interned object rather than a value this
// can combine. Both are left as instructions.
//
// Overflow wraps on the unsigned width, matching what the VM does with the same
// operands: a fold must not give an answer the unfolded code would not.
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
            // A float divided by zero is an infinity rather than a trap, and
            // one the VM would produce too, so it folds like any other.
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
            // The two traps the VM reports: a zero divisor, and the one pair
            // whose quotient has no representation. Left as instructions so the
            // program fails where it runs rather than where it compiles.
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

// The variable an address is ultimately taken from, so that borrowing 'v.x' pins v.
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

// The type a receiver names, looking through every level of pointer. 'box Player' and
// 'Player' share a method set, so both land on the same Type.
//
// A builtin qualifies: methods hang on the Type, and nothing about the map
// requires the type to be one a unit declared.
static Type *receiver_base_type(Type *type) {
    while (type_is_indirect(type)) {
        type = type->inner;
    }

    return type;
}

// Checks each argument against the parameter type in the same position. Shared
// by both call forms, which differ only in where their parameter list starts:
// a method's skips the receiver.
static bool type_accepts(Type *to, Type *from);
static bool borrow_into(ResolverState *state, ASTExpr **slot, Type *destination, Span span);
static void check_implicit_copy(ResolverState *state, ASTExpr *value, Type *destination, Span span);

static void check_call_args(ResolverState *state, ASTExprList *args, Type **params) {
    for (size_t i = 0; i < args->size; i++) {
        ASTExpr *arg = args->data[i];
        Type *param_type = params[i];

        if (is_error_type(arg->type) || is_error_type(param_type)) {
            continue;
        }

        // type_accepts rather than identity, so an owned 'box T' fills a 'ref T'
        // parameter and so does a 'T': lending is what a borrow parameter asks
        // for, and the caller goes on owning what it lent.
        if (!type_accepts(param_type, arg->type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, arg->span, "argument %zu is %s, but %s was declared",
                       i + 1, type_name(state, arg->type), type_name(state, param_type));
            continue;
        }

        if (!borrow_into(state, &args->data[i], param_type, arg->span)) {
            continue;
        }

        // An owning parameter takes ownership, so the argument is bound into it
        // exactly as it would be into a 'let': a non-copyable one needs a move.
        check_implicit_copy(state, args->data[i], param_type, arg->span);
    }
}

// Settles how the receiver reaches parameter zero, recording it on the node for
// codegen. Go's rule: a pointer method on an addressable value takes its
// address, and a value method through a pointer copies the inner in. Returns
// false when neither is possible.
// Rewrites 'recv.m(a)' into 'm(recv', a)', where recv' is the receiver adjusted
// to what parameter zero declared: 'ref recv' where the method takes a pointer and
// the receiver is a value, '*recv' the other way round.
//
// After this there is no method call left in the tree — only a call whose first
// argument happens to be a receiver, which is exactly what parameter zero has
// always been. Codegen therefore has one call path rather than two near-copies
// of one, and anything later that reasons about arguments sees the receiver
// among them without knowing to look for it.
//
// The adjustment is a real node rather than a flag because the tree is the only
// place it can be honestly recorded: 'ref recv' is an expression, and every pass
// that walks expressions should see it as one. Rust and Go both lower method
// calls this way, and for this reason.
//
// Runs after the call is fully checked, so the diagnostics above all report
// against what the user wrote rather than against the rewrite.
// How a receiver reaches parameter zero. A 'box T' method called on a 'T' takes
// its address; a 'T' method called through a 'box T' copies the inner in.
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
// place it can be honestly recorded: 'ref recv' is an expression, and every pass
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
    case RECEIVER_DEREF: {
        // One hop per level between the receiver and what parameter zero
        // accepts. Usually one -- a 'T' method reached through a 'box T' -- but
        // a 'ref box T' receiver is two away from the struct, and the last hop
        // is the one whose type the parameter settles.
        Type *declared = method->func.params[0];

        // The stop condition is this level matching, never type_accepts: that
        // one chains, so it is already true at the level furthest out and would
        // emit no hop at all.
        while (type_is_indirect(receiver->type) && receiver->type != declared &&
               declared->inner != receiver->type->inner) {
            Type *inner = receiver->type->inner;

            receiver = ast_deref_expr_create(span, receiver);
            receiver->type = inner;
        }

        break;
    }
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

    if (type_is_indirect(declared) && !type_is_indirect(actual)) {
        if (!is_addressable(receiver)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot call '%s' on a temporary, since it takes a pointer receiver", name->data);
            return false;
        }

        // The address is loose for the duration of the call, so the slot it
        // names must survive the whole block — exactly as for 'ref x'.
        Symbol *addressed = addressed_symbol(receiver);
        if (addressed) {
            addressed->pinned = true;
        }

        *out = RECEIVER_ADDRESS_OF;
        return true;
    }

    if (!type_is_indirect(declared) && type_is_indirect(actual)) {
        *out = RECEIVER_DEREF;
        return true;
    }

    // A pointer receiver still a level or more away from what the method takes:
    // a 'ref box T' calling a 'ref T' method reaches the 'box T' inside it,
    // which then lends. Checked before the widening below, since that one only
    // recognises a receiver already at the right level.
    if (type_is_indirect(actual) && type_is_indirect(actual->inner) &&
        type_accepts(declared, actual->inner)) {
        *out = RECEIVER_DEREF;
        return true;
    }

    // An owned 'box T' calling a 'ref T' method, which is the only pointer-to-
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

    // A 'box box Player', or some other shape no coercion bridges.
    diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot call '%s' on %s", name->data,
               type_name(state, actual));
    return false;
}

static void resolve_expr(ResolverState *state, ASTExpr *expr);

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

    resolve_expr(state, receiver);

    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i]);
    }

    Type *receiver_type = receiver->type;

    if (is_error_type(receiver_type)) {
        expr->type = resolver_error_type(state);
        return;
    }

    // 'box Player' and 'Player' share one method set, so the lookup and every
    // message below name the type itself rather than what was written.
    Type *base = receiver_base_type(receiver_type);

    String *method_name = resolver_intern(state, name);
    Symbol *method = type_find_method(base, method_name);

    if (!method) {
        // type_name rather than the name field: a pointer type has none, and a
        // receiver that is one reaches here.
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
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

// Whether a value of 'from' may be stored where 'to' is expected. Identity plus
// the two ways a borrow is produced, since a borrow is never spelled: an owned
// 'box T' may be stored where a 'ref T' is expected, and so may a 'T' itself,
// whose address is taken to make one.
//
// The reverse is deliberately absent: promoting 'ref T' to 'box T' would hand out
// ownership nobody granted, and the object already has an owner that will free
// it.
static bool type_accepts(Type *to, Type *from) {
    if (to == from) {
        return true;
    }

    // An owning string lends to a borrow of one. The reverse is refused: the
    // characters a borrow names belong to someone else, and an owning slot
    // would free what it never allocated.
    if (to->kind == TYPE_STRING && from && from->kind == TYPE_STRING) {
        return to->is_ref;
    }

    if (!type_is_indirect(to) || !to->is_ref) {
        return false;
    }

    // Borrowing: the destination names what 'from' itself sits in. A 'box T'
    // reaching a 'ref box T' takes this arm -- the address of the slot, not the
    // pointer it holds.
    if (to->inner == from) {
        return true;
    }

    // Lending what is already a pointer. Chained through every level, so a
    // 'box box T' reaches a 'ref T' by way of the 'box T' it holds, and stops at
    // the first level that matches -- which is what keeps the nearer destination
    // winning when both are declarable.
    //
    // A borrow is walked like an owning level: lending confers no ownership, so
    // reaching through one produces another borrow rather than giving anything
    // away. What must hold is that the inner outlives the borrow, which is a
    // lifetime question and belongs to the flow pass rather than here.
    //
    // Disjoint from the arm above at every step, since a pointer equal to
    // 'to->inner' cannot also have 'to->inner' as its own inner.
    while (type_is_indirect(from)) {
        if (to->inner == from->inner) {
            return true;
        }

        from = from->inner;
    }

    return false;
}

// Whether 'to' accepts 'from' only by taking its address, which is the case
// needing a node in the tree rather than a widening at the check.
static bool accepts_by_borrowing(Type *to, Type *from) {
    return to != from && type_is_indirect(to) && to->is_ref && to->inner == from;
}

// Materialises the borrow a 'ref T' destination asks for. Borrowing is implicit,
// so nothing in the source says to take an address -- but every later pass reads
// the tree, so the address-of has to be in it. The same reasoning as the receiver
// adjustment, and the same shape: a real node, typed as the destination.
//
// Returns false for a temporary, which has no address to take.
static bool borrow_into(ResolverState *state, ASTExpr **slot, Type *destination, Span span) {
    // Only something with a home in memory can be lent. A string that owns and
    // has no home is a temporary -- a concatenation -- and its characters are
    // freed where the expression ends, so the borrow would name freed memory.
    if (destination->kind == TYPE_STRING && destination->is_ref && (*slot)->type &&
        (*slot)->type->kind == TYPE_STRING && !(*slot)->type->is_ref && !is_addressable(*slot)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a string that nothing holds, since its characters are freed where the "
                   "expression ends");
        return false;
    }

    // Lending from further out than one level: 'box box T' filling a 'ref T'
    // lends the 'box T' inside it, so the levels above that have to be followed
    // here. type_accepts allows the pair; this is what makes the tree agree with
    // it, and without the hops the callee would receive the outer pointer and
    // read a pointer where the object should be.
    while (type_is_indirect((*slot)->type) && destination->inner != (*slot)->type->inner &&
           destination->inner != (*slot)->type) {
        Type *inner = (*slot)->type->inner;

        ASTExpr *hop = ast_deref_expr_create(span, *slot);
        hop->type = inner;
        *slot = hop;
    }

    if (!accepts_by_borrowing(destination, (*slot)->type)) {
        return true;
    }

    if (!is_addressable(*slot)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    // The slot must survive the whole block now that its address is loose, so
    // codegen may not reclaim it at the end of the statement.
    Symbol *addressed = addressed_symbol(*slot);
    if (addressed) {
        addressed->pinned = true;
    }

    ASTExpr *borrow = ast_addr_of_expr_create(span, *slot);
    borrow->type = destination;
    *slot = borrow;

    return true;
}

bool is_numeric_type(Type *t) { return t->kind == TYPE_INT || t->kind == TYPE_FLOAT; }

bool is_integer_type(Type *t) { return t->kind == TYPE_INT; }

bool is_boolean_type(Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

bool is_string_type(Type *t) { return t->kind == TYPE_STRING; }

// Ordering is left out: '<' on text asks which comes first, and no order is
// defined for it. Equality asks only whether two strings spell the same thing.
bool is_comparable_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t) || is_string_type(t); }

static Type *resolve_type_spec(ResolverState *state, TypeSpec *spec, Span span);

// Whether a binary operator accepts operands of this type, reporting why not
// when it does not. Both operands are already known to share the type.
//
// Shared with compound assignment, which applies the same operator to its
// target and its value: 'a %= b' is accepted exactly where 'a % b' is, and
// keeping one copy of the rules is what makes that true rather than intended.
static bool bin_op_accepts(ResolverState *state, BinOp op, Type *type, Span span) {
    const char *op_name = bin_op_name(op);

    switch (op) {
    // Joining is the one operator a string answers beyond equality. Which types
    // are joinable is the whole rule: an array becomes joinable by saying so
    // here, and nothing else about '..' has to change.
    case BIN_OP_CONCAT:
        if (is_string_type(type)) {
            return true;
        }

        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires a joinable type, found %s", op_name,
                   type_name(state, type));
        return false;

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

    // Alone among the arithmetic operators, '%' takes ints and not floats: a
    // float remainder is a libc call rather than an instruction, and is a
    // different feature than this one.
    case BIN_OP_MOD:
        if (!is_integer_type(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' requires an integer type, found %s",
                       op_name, type_name(state, type));
            return false;
        }

        return true;
    case BIN_OP_EQUAL:
    case BIN_OP_NEQUAL:
        if (!is_comparable_type(type)) {
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

// Whether a binary operator yields the type of its operands or a bool. The
// comparisons answer a question about their operands; everything else computes
// another value of the same type.
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

// 'int(x)' and 'float(x)': a call whose target names a type is a conversion.
// Returns false when the target names no type, leaving the node alone for the
// call path to resolve.
//
// The node is rewritten in place from EXPR_CALL to EXPR_CAST. Both live in the
// same union, so the operand is lifted out and the argument list freed before
// anything is written back over them.
static bool resolve_cast(ResolverState *state, ASTExpr *expr) {
    Type *target =
        scope_type_lookup(state->current_scope, resolver_intern(state, expr->call.target->var.name));

    if (!target) {
        return false;
    }

    // Everything below reports against a conversion rather than a call, so the
    // node becomes one here even where the conversion turns out to be illegal.
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
    expr->symbol = NULL;

    if (!operand) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "a conversion to %s takes one operand",
                   type_name(state, target));
        expr->type = resolver_error_type(state);
        return true;
    }

    resolve_expr(state, operand);

    Type *from = operand->type;

    if (is_error_type(from)) {
        expr->type = resolver_error_type(state);
        return true;
    }

    // Only the two numeric types convert. Bool is deliberately absent: a
    // 'bool(1)' would be an int used as a truth value, which nothing else in
    // the language permits.
    if (!is_numeric_type(target) || !is_numeric_type(from)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot convert %s to %s",
                   type_name(state, from), type_name(state, target));
        expr->type = resolver_error_type(state);
        return true;
    }

    // The result is a fresh value, so it deliberately inherits no symbol:
    // 'int(x)' is a temporary and must not be assignable or addressable.
    expr->type = target;
    return true;
}

static void resolve_expr(ResolverState *state, ASTExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP: {
        resolve_expr(state, expr->bin_op.left);
        resolve_expr(state, expr->bin_op.right);

        Type *left_type = expr->bin_op.left->type;
        Type *right_type = expr->bin_op.right->type;

        if (is_error_type(left_type) || is_error_type(right_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        // Two strings are one operand type however each of them owns: what '..'
        // and '==' read is the characters, and ownership decides who frees the
        // result rather than whether the operator applies.
        bool both_strings = is_string_type(left_type) && is_string_type(right_type);

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

        if (both_strings && expr->bin_op.op == BIN_OP_CONCAT) {
            // '..' allocates the characters it yields, so the result owns
            // them however its operands were written. Two literals are not
            // joined here: the result would still have to be copied into a
            // slot that owns, which is what OP_CONCAT already does.
            expr->type = state->current_scope->type_registry->builtins.string_type;
            break;
        }

        expr->type = bin_op_yields_bool(expr->bin_op.op)
                         ? type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL)
                         : left_type;

        fold_bin_op(expr);

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

        // 'int(x)' is a conversion, not a call. Types and symbols live in
        // separate namespaces, so a bare name in call position that names a
        // type cannot also name a function, and checking here costs nothing.
        // Left as an EXPR_CALL it would report "undeclared variable 'int'",
        // since the symbol table is the only place the call path looks.
        if (expr->call.target && expr->call.target->kind == EXPR_VARIABLE && resolve_cast(state, expr)) {
            break;
        }

        resolve_expr(state, expr->call.target);

        for (size_t i = 0; i < expr->call.args.size; i++) {
            resolve_expr(state, expr->call.args.data[i]);
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
        resolve_expr(state, expr->field.target);

        Type *target_type = expr->field.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // 'p.health' where p is a 'box Player' reaches through the pointer, as in
        // Go and C's '->'. Every level, since the search is what bounds it: a
        // 'ref box Player' is two hops from the struct, and stopping short would
        // only report a pointer as having no fields.
        while (type_is_indirect(target_type)) {
            target_type = target_type->inner;
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
    case EXPR_MOVE: {
        resolve_expr(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // A move produces exactly what the operand held; only what happens to
        // the operand's slot differs.
        expr->type = target_type;

        // A struct moves whole or not at all. Moving one field would leave the
        // rest behind, and a half-moved struct is not something the language
        // says anything about: what its other fields mean, whether it may be
        // passed on, and what its scope end frees are all unanswered.
        if (expr->unary.target->kind == EXPR_FIELD) {
            diag_error(state->diagnostics, GAB_ERR_LIFETIME, expr->span,
                       "a field cannot be moved out of on its own; move the whole struct instead");
            break;
        }

        break;
    }
    case EXPR_ADDR_OF: {
        resolve_expr(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // No syntax makes one of these: an address-of is synthesized where a
        // 'ref T' destination asks for a borrow, and both places that build one
        // type it themselves rather than resolving it again. The checks below
        // therefore guard a path nothing currently walks, and are kept because
        // they state what the node requires rather than what reaches it.
        if (!is_addressable(expr->unary.target)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot take the address of a temporary");
            expr->type = resolver_error_type(state);
            break;
        }

        // A borrow of an owning pointer is what an out-parameter would need — a
        // borrow of the caller's variable rather than of the object, so the
        // callee could repoint it. Which is more than a spelling: assigning
        // through one would free the caller's old object from inside the callee,
        // an owning slot changing owner mid-call. Returning ownership says the
        // same thing with the transfer visible at the call site.
        if (type_is_indirect(target_type) && !target_type->is_ref) {
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

        // An address is a borrow: the slot it names is owned by whoever declared
        // it — a stack local owns itself, and freeing through this address would
        // free something 'new' never made.
        //
        // Typing it 'box T' would make one type mean two things: an address of
        // something, and ownership of a heap object. Nothing could then tell
        // them apart from the type alone.
        expr->type = type_registry_indirect_to_kind(state->current_scope->type_registry, target_type, true);
        break;
    }
    case EXPR_DEREF: {
        resolve_expr(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

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

        expr->type = target_type->inner;

        // The address itself lives in the target's slots, so a deref stays
        // assignable through whatever the target was.
        expr->symbol = expr->unary.target->symbol;
        break;
    }
    case EXPR_NEG: {
        resolve_expr(state, expr->unary.target);

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
    case EXPR_NOT: {
        resolve_expr(state, expr->unary.target);

        Type *target_type = expr->unary.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // Nothing else converts to bool, so an int is not a truth value here
        // the way it is in C.
        if (!is_boolean_type(target_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "unary '!' requires bool, found %s",
                       type_name(state, target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        // As with '-x', the result is a fresh value and deliberately inherits
        // no symbol: '!b' is a temporary and must not be assignable or
        // addressable.
        expr->type = target_type;
        break;
    }
    case EXPR_NEW: {
        Type *type = resolve_type_spec(state, expr->new_expr.type_spec, expr->span);

        if (is_error_type(type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // A struct has a layout to allocate, and so does a pointer: 'new box T'
        // is a heap slot holding an owning pointer, which is what makes a
        // 'box box T' fillable rather than merely declarable. A string is a
        // header with a layout of its own, and zeroed it is the empty string.
        // A scalar is none of these -- 'new int' would be a boxed scalar, a
        // different feature.
        if (type->kind != TYPE_STRUCT && type->kind != TYPE_STRING && !type_is_indirect(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; 'new' takes a struct or a pointer", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        // A borrow has no owner to name, so a heap slot holding one would
        // outlive whatever it borrows with nothing tracking that.
        if (type_is_indirect(type) && type->is_ref) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; a heap slot cannot hold a borrow", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        expr->new_expr.type = type;
        expr->type = type_registry_indirect_to(state->current_scope->type_registry, type);
        break;
    }
    case EXPR_LITERAL: {
        TypeRegistry *registry = state->current_scope->type_registry;

        // A string literal names characters the unit's arena holds, which
        // outlive every value that reads them and are freed with the unit. So
        // it borrows: nothing in a frame allocated them, and nothing there may
        // free them.
        expr->type = expr->lit.kind == TYPE_STRING ? registry->builtins.ref_string_type
                                                   : type_registry_get_builtin(registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

// Rejects binding a non-copyable value without saying where ownership goes.
// Copying is the default and is implicit, so the error is not that a copy is
// impossible but that this one was not asked for: the message names both ways
// to ask, since which is wanted is the programmer's to say.
//
// Only a value read out of a named slot can be implicitly copied. A temporary
// -- 'new Box', a call's owned return -- is already nobody else's, so binding
// it transfers what it made rather than duplicating what someone holds.
static void check_implicit_copy(ResolverState *state, ASTExpr *value, Type *destination, Span span) {
    if (!value || value->kind == EXPR_MOVE || is_error_type(value->type)) {
        return;
    }

    if (type_is_copyable(value->type)) {
        return;
    }

    // Binding into a 'ref' slot borrows rather than copies: the destination
    // never frees what it names, so the owner stays the only one.
    if (destination && destination->is_ref) {
        return;
    }

    if (!value->symbol || value->symbol->kind != SYMBOL_VAR) {
        return;
    }

    // Only advise the clone where there is one to call. A type that declares
    // none is told so instead, since sending the programmer to a method that
    // does not exist costs them the round trip to find that out.
    const Type *base = receiver_base_type(value->type);

    if (base && type_find_method(base, resolver_intern(state, string_ref_create("clone")))) {
        diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
                   "%s owns what it holds, so binding it needs 'move' to transfer ownership, or "
                   "'clone()' to duplicate it",
                   type_name(state, value->type));
        return;
    }

    diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
               "%s owns what it holds, so binding it needs 'move' to transfer ownership; it declares no "
               "'clone' to duplicate it",
               type_name(state, value->type));
}

// Returns NULL when there is no spec to resolve (an omitted type), and the
// poison type when the spec names something that does not exist.
static Type *resolve_type_spec(ResolverState *state, TypeSpec *spec, Span span) {
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

    // Interned in the one shared registry whichever scope named the inner,
    // so 'box Config' is one type however many modules mention it.
    //
    // Built from the name outward, which is the order the mask records: bit i is
    // the level wrapped on the i'th time round, so 'ref box T' owns first and
    // borrows that.
    // A string carries its own borrow bit rather than being wrapped, because
    // its characters hang off the value itself: 'ref string' is the same two
    // slots as 'string', borrowed. Wrapping would put an indirection in front
    // of a header that is already one.
    if (type->kind == TYPE_STRING && spec->indirect_depth == 1 && (spec->ref_levels & 1)) {
        return state->current_scope->type_registry->builtins.ref_string_type;
    }

    for (unsigned int i = 0; i < spec->indirect_depth; i++) {
        bool is_ref = (spec->ref_levels >> i) & 1;
        type = type_registry_indirect_to_kind(state->current_scope->type_registry, type, is_ref);
    }

    return type;
}

static void resolve_stmt(ResolverState *state, ASTStmt *stmt);

// Declares the struct's Type: its name, its fields, and its layout. Split from
// the body walk so the pre-pass can run it over a whole unit's top level
// before any signature mentions a type.
static void declare_struct(ResolverState *state, ASTStmt *stmt) {
    stmt->struct_decl.declared = true;

    // Declared under its bare name into the scope it appears in, so two
    // modules may each declare a 'Config' without either name carrying the
    // module in it.
    String *struct_name = resolver_intern(state, stmt->struct_decl.name);

    // Shadowing an outer type is allowed; redeclaring one this module already
    // has is not, whether this unit declared it or an earlier one did.
    if (scope_type_lookup_declaring(state->current_scope, struct_name)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                   struct_name->data);
        return;
    }

    Type *type = type_struct_create(resolver_owner_arena(state), struct_name, stmt->struct_decl.fields.size);

    // Registered under its name *before* its fields resolve, so that a field
    // pointing at the struct being declared finds it. A scene graph is exactly
    // this shape — 'struct Node { parent: ref Node, child: box Node }' — and
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
        if (field->type_spec->indirect_depth == 0 &&
            string_ref_equals_ref(field->type_spec->name, stmt->struct_decl.name)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, field->span, "struct '%s' cannot contain itself",
                       struct_name->data);
            poisoned = true;
            continue;
        }

        Type *field_type = resolve_type_spec(state, field->type_spec, field->span);

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
// pre-pass can declare a whole unit's top level before resolving any of it,
// Resolves one parameter's declared type.
//
// A parameter spells ownership the way a local and a field do: bare 'box T' owns
// what it is given and frees it when the call ends, 'ref T' borrows and frees
// nothing. Which one a signature declares is what a call site reads to know
// whether it must move, so the two may not be written interchangeably.
static Type *resolve_param_type(ResolverState *state, ASTField *param) {
    return resolve_type_spec(state, param->type_spec, param->span);
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

    Type *receiver_type = resolve_type_spec(state, receiver->type_spec, receiver->span);

    if (is_error_type(receiver_type)) {
        return;
    }
    Type *base = receiver_base_type(receiver_type);

    // A unit declares methods on its own structs only. A builtin carries the
    // ones the VM registered, and a second set declared over them would have no
    // module to belong to and no way to be told apart from the first.
    if (!base || base->kind != TYPE_STRUCT) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, receiver->span,
                   "a method's receiver must be a struct or a pointer to one, found %s",
                   type_name(state, receiver_type));
        return;
    }

    // A method never owns its receiver. A parameter may, but only because a
    // call site can say 'move' where it hands one over; a receiver has no such
    // spelling -- 'p.consume()' gives no place to mark the transfer -- so
    // 'box T' here would be an ownership nothing could grant.
    //
    // 'ref T' is the form that says what is true. A receiver by value stays
    // available as 'T', which copies; the two axes are separate, and only this
    // one is about ownership.
    if (type_is_indirect(receiver_type) && !receiver_type->is_ref) {
        diag_error(
            state->diagnostics, GAB_ERR_TYPE, receiver->span,
            "a method borrows its receiver rather than owning it, so write 'ref %s' instead of 'box %s'",
            base->name->data, base->name->data);
        return;
    }

    // Go's rule: a method belongs with the module that declares its receiver,
    // so the type has to be one this scope holds rather than one it inherits.
    if (!scope_type_lookup_local(state->current_scope, base->name)) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, receiver->span,
                   "cannot declare a method on '%s', which this module does not declare", base->name->data);
        return;
    }

    Type *return_type = resolve_type_spec(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = return_type;

    String *method_name = resolver_intern(state, stmt->func_decl.name);

    // Not scope_decl_func: this name lives on the type, not in a scope. The
    // Symbol is built directly for the same reason.
    Symbol *method = arena_alloc(resolver_owner_arena(state), sizeof(Symbol));
    *method = (Symbol){
        .kind = SYMBOL_FUNC,
        .scope_depth = state->current_scope->depth,
        .pinned = false,
        .func =
            {
                .return_type = return_type,
                .params = NULL,
                .param_count = 0,
                .func_index = SYMBOL_FUNC_NO_BODY,
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

    // 'clone' is the remedy the implicit-copy diagnostic names, so its shape is
    // fixed: it duplicates its receiver and yields another of the same type.
    // Checked here rather than at a call so a type declaring the wrong thing
    // hears about it where it wrote it.
    if (method_name == resolver_intern(state, string_ref_create("clone"))) {
        if (return_type != base) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'clone' duplicates its receiver, so it must return %s, not %s", base->name->data,
                       type_name(state, return_type));
            return;
        }

        if (stmt->func_decl.params.size > 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'clone' duplicates its receiver and takes nothing else");
            return;
        }
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
    Type *func_return_type = resolve_type_spec(state, stmt->func_decl.return_type, stmt->span);

    stmt->func_decl.resolved_return_type = func_return_type;

    Symbol *func = scope_decl_func(state->current_scope, resolver_intern(state, func_name), func_return_type);

    if (!func) {
        char *name = string_ref_to_cstr(func_name);
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                   name);
        free(name);
    }

    stmt->func_decl.symbol = func;

    if (func) {
        func->func.is_extern = stmt->func_decl.body == NULL;

        if (func->func.is_extern) {
            func->func.name = resolver_intern(state, func_name);
            func->func.module = state->module_name;
        }
    }

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
    size_t errors_before = diagnostics_count(state->diagnostics);

    resolver_enter_scope(state);

    // The receiver is an ordinary local in the body's scope, so 'p.health'
    // resolves through the existing field path — including the auto-deref that
    // a 'box Player' receiver needs.
    ASTField *receiver = stmt->func_decl.receiver;

    if (receiver) {
        Type *receiver_type = resolve_type_spec(state, receiver->type_spec, receiver->span);

        receiver->symbol =
            scope_decl_var(state->current_scope, resolver_intern(state, receiver->name), receiver_type);
    }

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);
        Type *param_type = resolve_type_spec(state, param->type_spec, param->span);

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

    resolve_stmt(state, stmt->func_decl.body);

    state->func_context = previous_context;

    // Flow analysis runs over the resolved body rather than during it: the
    // symbols it reads are the ones just bound, and it iterates the body as
    // many times as convergence takes, which resolution could not survive.
    //
    // Only where the body resolved cleanly. A poisoned tree has nodes with no
    // symbol and no type, and what the flow rules would say about those is
    // noise on top of the error that already explains it.
    if (diagnostics_count(state->diagnostics) == errors_before) {
        size_t param_count = stmt->func_decl.params.size;
        Symbol **params = arena_alloc(state->compile_arena, (param_count + 1) * sizeof(Symbol *));
        size_t count = 0;

        if (receiver) {
            params[count++] = receiver->symbol;
        }

        for (size_t i = 0; i < param_count; i++) {
            params[count++] = stmt->func_decl.params.data[i]->symbol;
        }

        flow_pass_run(state->compile_arena, stmt->func_decl.body, params, count,
                      stmt->func_decl.resolved_return_type, state->diagnostics);
    }

    resolver_exit_scope(state);
}

static void resolve_stmt(ResolverState *state, ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR: {
        resolve_expr(state, stmt->expr.value);
        break;
    }
    case STMT_VAR_DECL: {
        resolve_expr(state, stmt->var_decl.initializer);

        Type *type;
        if (stmt->var_decl.type_spec) {
            Type *decl_type = resolve_type_spec(state, stmt->var_decl.type_spec, stmt->span);

            if (stmt->var_decl.initializer) {
                Type *init_type = stmt->var_decl.initializer->type;

                if (!is_error_type(decl_type) && !is_error_type(init_type) &&
                    !type_accepts(decl_type, init_type)) {
                    // A borrow reaching an owning string is the one mismatch
                    // with a remedy to name: the characters belong to the
                    // arena, and 'clone()' is what copies them into a string
                    // this slot may free.
                    if (decl_type->kind == TYPE_STRING && init_type->kind == TYPE_STRING) {
                        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                                   "a %s borrows characters it does not own, so a 'string' cannot take it; "
                                   "write 'ref string', or '.clone()' to copy them",
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

        // Nothing frees a top-level slot: the unit's chunk ends in a return
        // that closes no block, so a value that owns would live until the VM
        // dies and be leaked rather than released. A borrow is fine -- it frees
        // nothing wherever it lives.
        if (state->current_scope == state->global_scope && type_is_owned(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "a top-level variable may not own, since no scope closes to free it; %s belongs in a "
                       "function body",
                       type_name(state, type));
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

        check_implicit_copy(state, stmt->var_decl.initializer, type, stmt->span);

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

        // An extern declares a signature and nothing else: there is no body to
        // walk, and its parameters name no slots any code here will read.
        if (stmt->func_decl.body) {
            resolve_func_body(state, stmt);
        }
        break;
    }
    case STMT_STRUCT_DECL: {
        if (!stmt->struct_decl.declared) {
            declare_struct(state, stmt);
        }
        break;
    }
    case STMT_ASSIGN: {
        resolve_expr(state, stmt->assign.target);
        resolve_expr(state, stmt->assign.value);

        Type *target_type = stmt->assign.target->type;
        Type *value_type = stmt->assign.value->type;

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !type_accepts(target_type, value_type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot assign a value of type %s to a target of type %s",
                       type_name(state, value_type), type_name(state, target_type));
            break;
        }

        if (!is_error_type(target_type) && !is_error_type(value_type) &&
            !borrow_into(state, &stmt->assign.value, target_type, stmt->span)) {
            break;
        }

        // Storing through a pointer reaches something whose lifetime this frame
        // does not bound — a heap object outlives every frame, and a 'box T'
        // parameter may point at a caller's. Depth 0 is that bound: "outlives
        // everything", so only a pointer to something equally long-lived may be
        // stored there. Without this, 'ref local' escapes into a heap object and
        // dangles the moment the frame returns, ownership notwithstanding.
        if (stmt->assign.target->kind == EXPR_FIELD || stmt->assign.target->kind == EXPR_DEREF) {
            // An owning field takes ownership of what is stored in it, exactly
            // as a 'let' does, so a non-copyable value needs a move.
            check_implicit_copy(state, stmt->assign.value, target_type, stmt->span);
            break;
        }

        Symbol *target = stmt->assign.target->symbol;

        if (target && target->kind == SYMBOL_VAR) {
            check_implicit_copy(state, stmt->assign.value, target_type, stmt->span);
        }
        break;
    }
    case STMT_COMPOUND_ASSIGN: {
        resolve_expr(state, stmt->compound_assign.target);
        resolve_expr(state, stmt->compound_assign.value);

        Type *target_type = stmt->compound_assign.target->type;
        Type *value_type = stmt->compound_assign.value->type;

        if (is_error_type(target_type) || is_error_type(value_type)) {
            break;
        }

        const char *op_name = bin_op_name(stmt->compound_assign.op);

        if (target_type != value_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "cannot apply '%s=' to %s and %s",
                       op_name, type_name(state, target_type), type_name(state, value_type));
            break;
        }

        // The same rule the bare operator answers to, so 'a %= b' is accepted
        // exactly where 'a % b' is.
        if (!bin_op_accepts(state, stmt->compound_assign.op, target_type, stmt->span)) {
            break;
        }

        // A comparison yields a bool, which is not what the target holds. No
        // token spells one of these, so this guards the enum rather than a
        // program anyone can write.
        assert(!bin_op_yields_bool(stmt->compound_assign.op) &&
               "a compound assignment must yield its target's type");

        // Nothing here tracks inner depth or ownership the way plain
        // assignment does: the operators this node carries are arithmetic, so
        // the target is an int or a float and never names an object.
        break;
    }
    case STMT_IF: {
        resolve_expr(state, stmt->ifstmt.condition);

        // A branch has to have something to branch on, and bool is the only
        // thing the jump opcodes read. An already-poisoned condition has
        // reported its own error, so it is let through rather than complained
        // about twice.
        Type *condition_type = stmt->ifstmt.condition->type;

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

        // The initializer's scope encloses the condition, the post clause, and
        // the body, so 'for let i = 0; i < n; i = i + 1' scopes i to the loop.
        resolver_enter_scope(state);
        stmt->forstmt.scope = state->current_scope;

        resolve_stmt(state, stmt->forstmt.init);

        if (stmt->forstmt.condition) {
            resolve_expr(state, stmt->forstmt.condition);

            Type *condition_type = stmt->forstmt.condition->type;

            if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->forstmt.condition->span,
                           "'for' requires a boolean condition, found %s", type_name(state, condition_type));
            }
        }

        // One walk, because this only resolves: what the back-edge carries is
        // the flow pass's question, and it iterates the graph rather than the
        // tree.
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
        resolve_expr(state, stmt->ret.result);

        Type *expected = state->func_context.return_type;
        Type *actual = stmt->ret.result ? stmt->ret.result->type : NULL;

        // A NULL type here means "no value", which is a distinct case from a
        // poisoned one: it must still be checked against the declared type.
        bool poisoned = (expected && expected->kind == TYPE_ERROR) || (actual && actual->kind == TYPE_ERROR);

        // type_accepts once both are present, so a function declaring 'ref T'
        // may return an owned 'box T' — it lends what it was given rather than
        // handing ownership out. A NULL on either side is "no value", which
        // only identity settles.
        bool accepted = actual && expected ? type_accepts(expected, actual) : actual == expected;

        if (!poisoned && !accepted) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "returns %s, but %s was declared",
                       type_name(state, actual), type_name(state, expected));
            break;
        }

        // The borrow has to reach the tree here too, or the lifetime pass sees a
        // value being returned and never learns an address escaped the frame.
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
    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

        if (stmt && stmt->kind == STMT_STRUCT_DECL) {
            declare_struct(&state, stmt);
        }
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

    return diagnostics_count(diagnostics) == errors_before;
}
