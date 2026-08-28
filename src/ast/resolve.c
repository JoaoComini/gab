#include "ast/resolve.h"

#include "ast/flow_pass.h"
#include "object.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*
    A struct as declared, before it has a layout.

    Complete as soon as its name is bound: the name, the scope it was bound in,
    the type interned for it, and the fields as the source wrote them. None of
    that depends on another declaration, which is what lets every struct in a
    unit be declared before any of their fields resolve -- and so what lets two
    structs name each other.

    The layout is derived from this and memoized, rather than being part of it.
    Deriving one reads other declarations, so it may not run until they are all
    bound; and it may not run twice, since a Type is laid out once.
*/
typedef struct StructDecl {
    ASTStmt *stmt;
    Scope *scope;
    String *name;

    // The interned identity, which the declaration has from the start. Const
    // once laid out, but built through here, so it is the mutable pointer.
    Type *type;

    enum {
        // Bound, with no layout asked for yet.
        STRUCT_DECLARED,

        // Being laid out. A field reaching a declaration in this state by value
        // is a containment cycle: its width is waiting on itself.
        STRUCT_LAYING_OUT,

        STRUCT_LAID_OUT,

        // Laid out as far as it goes: a field failed, so the type is poisoned
        // rather than half-built. Distinct from LAID_OUT so a second demand
        // reports nothing a second time.
        STRUCT_POISONED,
    } state;
} StructDecl;

#define struct_decl_list_item_free(item) ((void)(item))
GAB_LIST(StructDeclList, struct_decl_list, StructDecl *)

typedef struct {
    const Type *return_type;

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

    // Every struct this unit declared, in declaration order. Held so that the
    // layouts can be forced once the whole unit's names are bound -- a struct
    // must leave this unit laid out, since the AST its fields were written in
    // does not outlive the unit.
    StructDeclList struct_decls;

    Diagnostics *diagnostics;
} ResolverState;

// Where anything reachable after the compile has to live. A struct Type goes
// into the TypeRegistry and a Symbol into the global scope's table, and both
// outlive every compile — so both are owned by whatever owns the global scope,
// which is the rule this derivation encodes: allocate from the arena of the
// thing that will own the result, never from whichever arena was passed in.
static Arena *resolver_owner_arena(ResolverState *state) { return state->global_scope->arena; }

static const Type *resolver_error_type(ResolverState *state) {
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

// The half of a spec name a scope holds: 'Type' out of 'Module::Type'.
static String *resolver_expr_member(ResolverState *state, StringRef name) {
    StringRef module, member;

    if (string_ref_split_colons(name, &module, &member)) {
        return string_from_ref(state->current_scope->strings, member);
    }

    return resolver_intern(state, name);
}

// A type that is already poisoned had its error reported at the origin, so any
// further check involving it silently succeeds rather than cascading.
static bool is_error_type(const Type *type) { return !type || type->kind == TYPE_ERROR; }

// The printable form of a type. A pointer's name is derived from its inner
// rather than stored, so 'box box Player' formats without interning two
// intermediate names. Built in the compile arena: only diagnostics ask, and
// they are already on the failing path.
static const char *type_name(ResolverState *state, const Type *type) {
    if (!type) {
        return "none";
    }

    if (type->name) {
        return type->name->data;
    }

    const char *inner = type->name ? type->name->data : type_name(state, type_pointee(type));
    const char *prefix = type->kind == TYPE_REF ? "ref " : "box ";
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
    case EXPR_INDEX:
        return addressed_symbol(expr->index.target);
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
    case EXPR_INDEX:
        // An element lives in the block the header names, so it has an address
        // whenever the array does.
        return is_addressable(expr->index.target);
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
static const Type *receiver_base_type(const Type *type) {
    while (type_is_indirect(type)) {
        type = type_pointee(type);
    }

    return type;
}

static const Type *derefs_to(TypeRegistry *registry, const Type *type);

// The method a receiver of this type answers to, found by walking from the type
// itself down through what it derefs to.
//
// An owner reaches its borrowed view this way: a 'String' finds a 'str's
// methods because it is laid out as one, which is the relation Rust spells
// Deref and applies at exactly this point.
static Symbol *find_method_on_chain(TypeRegistry *registry, const Type *type, const String *name,
                                    const Type **out_base) {
    for (const Type *at = receiver_base_type(type); at; at = derefs_to(registry, at)) {
        Symbol *found = type_registry_find_method(registry, at, name);

        if (found) {
            *out_base = at;
            return found;
        }
    }

    *out_base = receiver_base_type(type);
    return NULL;
}

// Checks each argument against the parameter type in the same position. Shared
// by both call forms, which differ only in where their parameter list starts:
// a method's skips the receiver.
static bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from);
static bool accepts_by_borrowing(const Type *to, const Type *from);
static bool lends_by_value(TypeRegistry *registry, const Type *to, const Type *from);
static bool lends_by_pointer(const Type *to, const Type *from);
static bool borrow_into(ResolverState *state, ASTExpr **slot, const Type *destination, Span span);
static void check_implicit_copy(ResolverState *state, ASTExpr *value, const Type *destination, Span span);

static void check_call_args(ResolverState *state, ASTExprList *args, const Type **params) {
    for (size_t i = 0; i < args->size; i++) {
        ASTExpr *arg = args->data[i];
        const Type *param_type = params[i];

        if (is_error_type(arg->type) || is_error_type(param_type)) {
            continue;
        }

        // type_accepts rather than identity, so an owned 'box T' fills a 'ref T'
        // parameter and so does a 'T': lending is what a borrow parameter asks
        // for, and the caller goes on owning what it lent.
        if (!type_accepts(state->current_scope->type_registry, param_type, arg->type)) {
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
// How a receiver reaches parameter zero: how many levels to reach through
// first, and whether its address is taken once there.
//
// The two are one answer rather than two, because the search that finds a
// method is what settles both -- and the walk that lowers the call must make
// exactly the hops that search stopped at. Recording the count is what keeps
// the two from having to agree by rederiving it.
typedef struct {
    // Levels of indirection to reach through before the receiver is at the type
    // the method was found on. Zero for a receiver already there.
    size_t derefs;

    // Whether the method wants a pointer to what those hops reached, which a
    // value receiver at that level has to be given by taking its address.
    bool address_of;
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

    // Exactly the hops the search stopped at, in the order it made them: reach
    // through each level, then take the address if the method asked for one.
    for (size_t i = 0; i < adjustment.derefs; i++) {
        const Type *inner = type_pointee(receiver->type);

        receiver = ast_deref_expr_create(span, receiver);
        receiver->type = inner;
    }

    if (adjustment.address_of) {
        receiver = ast_addr_of_expr_create(span, receiver);
        receiver->type = method->func.params[0];
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
static bool reconcile_receiver(ResolverState *state, ASTExpr *expr, ASTExpr *receiver, const Type *declared,
                               const Type *actual, const String *name, ReceiverAdjustment *out) {
    // Every level the receiver can be reached through, nearest first: the
    // receiver itself, what it points at, what that points at. At each rung the
    // method is tried by value and then by address, and the first rung that
    // answers wins -- which is what keeps the nearest reading of an ambiguous
    // receiver the one taken.
    //
    // Rust's method call resolves this way, and for the same reason: the shapes
    // a receiver may take are a chain rather than a set of pairs, and walking it
    // is what a pile of pairwise cases was approximating.
    const Type *at = actual;

    for (size_t derefs = 0;; derefs++) {
        // Already what the method takes, at this level.
        if (declared == at) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        // Taking the receiver's address, which is what a 'ref T' method called
        // on a T asks for: what it names is the slot the receiver sits in.
        //
        // Asked before lending because the two run in opposite directions and
        // one pair answers to both. A T reaching a 'ref T' is this -- the
        // address of its own storage -- while lending reaches through a
        // receiver to something it already holds. Which of the two a pair is
        // decides what the tree gets, so it is read off the relation rather
        // than off which test was written first.
        if (accepts_by_borrowing(declared, at)) {
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

            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = true};
            return true;
        }

        // Lending: the receiver already holds what the method wants, so it is
        // handed over as it stands. A 'box T' gives the 'ref T' it points at,
        // and an owning string gives the address and count it already is.
        //
        // One level at a time, never type_accepts: that one walks the chain
        // itself and would answer from the outermost level, reporting no hops
        // where the receiver still has levels to go.
        if (lends_by_value(state->current_scope->type_registry, declared, at) ||
            lends_by_pointer(declared, at)) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        // A method declared on the declaration this level instantiates: the
        // bare 'Array' every '[T; N]' hangs its set on. What such a method
        // reads is the header, which every array has the same shape of, so the
        // element it was written over does not enter into it.
        if (declared == at->decl) {
            *out = (ReceiverAdjustment){.derefs = derefs, .address_of = false};
            return true;
        }

        if (!type_is_indirect(at)) {
            break;
        }

        at = type_pointee(at);
    }

    // A 'box box Player', or some other shape no rung of the chain bridges.
    diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot call '%s' on %s", name->data,
               type_name(state, actual));
    return false;
}

// 'expected' is the type the value is going to land in, or NULL where the
// destination is not known before the value is. Only an array literal reads it:
// '[1, 2, 3]' has no type of its own, so what it must be is whatever it is
// being stored into.
static void resolve_expr(ResolverState *state, ASTExpr *expr, const Type *expected);
static Symbol *resolve_qualified_func(ResolverState *state, ASTExpr *expr);

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

    resolve_expr(state, receiver, NULL);

    // No destination type for the arguments: which method this is depends on
    // the receiver's type, so the parameter list is not known until after the
    // arguments would have to be resolved.
    for (size_t i = 0; i < expr->call.args.size; i++) {
        resolve_expr(state, expr->call.args.data[i], NULL);
    }

    const Type *receiver_type = receiver->type;

    if (is_error_type(receiver_type)) {
        expr->type = resolver_error_type(state);
        return;
    }

    // 'box Player' and 'Player' share one method set, so the lookup and every
    // message below name the type itself rather than what was written -- and a
    // borrowed view finds what it borrows by lending, which the walk follows.
    String *method_name = resolver_intern(state, name);

    const Type *base = NULL;
    Symbol *method =
        find_method_on_chain(state->current_scope->type_registry, receiver_type, method_name, &base);

    if (!method) {
        // type_name rather than the name field: a pointer type has none, and a
        // receiver that is one reaches here.
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "%s has no method '%s'",
                   type_name(state, base), method_name->data);
        expr->type = resolver_error_type(state);
        return;
    }

    // The sugar borrows, and never moves: a function consuming parameter zero
    // is reached where the transfer can be written, so that no call site gives
    // up ownership without saying so.
    if (method->func.param_count > 0 && method->func.params[0]->kind == TYPE_BOX) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'%s' consumes what it takes, so it is called as '%s::%s(move ...)' rather than on a "
                   "value",
                   method_name->data, type_name(state, base), method_name->data);

        expr->type = resolver_error_type(state);
        return;
    }

    // The sugar fills parameter zero, so a function that declares none has
    // nothing for the value to become and is reached on the type instead.
    if (method->func.param_count == 0) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                   "'%s' takes nothing, so it is called as '%s::%s()' rather than on a value",
                   method_name->data, type_name(state, base), method_name->data);

        expr->type = resolver_error_type(state);
        return;
    }

    // Parameter zero is the receiver, so the declared parameters — the ones the
    // caller actually writes — are everything after it.
    const Type *declared_receiver = method->func.params[0];
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

    // An array's length is part of its type, so 'xs.len()' is known here and
    // becomes the literal it answers. Folded rather than called because there
    // is nothing at run time to read: the elements carry no count beside them.
    //
    // Ahead of the lowering below, which would otherwise build a call this
    // discards.
    if (base->kind == TYPE_ARRAY &&
        method_name == string_from_cstr(state->current_scope->type_registry->strings, "len")) {
        ast_expr_free(expr->call.target);
        ast_expr_list_free(&expr->call.args);

        expr->kind = EXPR_LITERAL;
        expr->lit = (Literal){.kind = TYPE_INT, .as_int = type_array_length(base)};
        expr->type = method->func.return_type;
        return;
    }

    // Lowered before the arguments are checked, so that the value reaching
    // parameter zero is checked as the argument it has become. It is one: the
    // sugar writes it rather than the caller, but what it costs -- a copy of a
    // type that owns -- is the same either way.
    lower_method_call(expr, method, adjustment);

    check_call_args(state, &expr->call.args, method->func.params);

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
// A value handing over a reference to what it stands for: an owning string
// lends a 'ref str', because what it derefs to is the run of characters such a
// reference names.
//
// Stated in terms of the deref relation rather than of strings, so a second
// owner -- a growable buffer lending a slice -- lends by declaring what it
// stands for and nothing here changes.
//
// The reverse is refused, as every borrow-to-owner is: what a reference names
// belongs to someone else, and an owning slot would free what it never
// allocated.
static bool lends_by_value(TypeRegistry *registry, const Type *to, const Type *from) {
    const Type *view = type_registry_deref_of(registry, from);

    // A type that stands for nothing lends nothing. Checked rather than left to
    // the comparison, which two absent answers would otherwise satisfy.
    return view && to->kind == TYPE_REF && view == type_pointee(to);
}

// What a value derefs to, or NULL for a type that derefs to nothing. Registered
// on the registry rather than read off the type: what a value stands for is not
// in its shape, so a 'String' and a 'Vec<byte>' laid out alike are told apart by
// what each was declared to deref to.
//
// The direction an owner reaches its borrowed view, never the reverse: methods
// that only read characters belong to the characters, and an owner reaches them
// by being one. What belongs to the owner -- duplicating the allocation -- is
// not reachable from a borrow, which is the whole point of the direction.
static const Type *derefs_to(TypeRegistry *registry, const Type *type) {
    return type_registry_deref_of(registry, type);
}

// A pointer handing over a reference to what it points at, reaching through
// exactly one level.
//
// The single step type_accepts chains. Kept apart from that walk so a caller
// that must know which level answered can ask one level at a time -- which the
// walk cannot tell it, since it reports only that some level did.
static bool lends_by_pointer(const Type *to, const Type *from) {
    return to->kind == TYPE_REF && type_is_indirect(from) && type_pointee(to) == type_pointee(from);
}

static bool accepts_by_borrowing(const Type *to, const Type *from) {
    return to != from && to->kind == TYPE_REF && type_pointee(to) == from;
}

static bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from) {
    if (to == from) {
        return true;
    }

    // The two relations a destination may accept a value by, which run in
    // opposite directions: borrowing names the slot 'from' itself sits in, while
    // lending reaches through it to what it already holds. A pair is one or the
    // other, never both -- a pointer equal to what 'to' names cannot also name
    // it -- so asking for either is the whole question here.
    if (accepts_by_borrowing(to, from)) {
        return true;
    }

    // Lending, at every level the value can be reached through: a 'box box T'
    // reaches a 'ref T' by way of the 'box T' it holds, and a 'ref String'
    // reaches a 'ref str' by way of the header it names. The first level that
    // answers wins, which is what keeps the nearer destination winning when
    // both are declarable.
    //
    // The same chain a method receiver walks, so what an argument accepts and
    // what a receiver reaches are one rule rather than two that drift.
    //
    // A borrow is walked like an owning level: lending confers no ownership, so
    // reaching through one produces another borrow rather than giving anything
    // away. What must hold is that the inner outlives the borrow, which is a
    // lifetime question and belongs to the flow pass rather than here.
    for (const Type *at = from;; at = type_pointee(at)) {
        if (lends_by_value(registry, to, at) || lends_by_pointer(to, at)) {
            return true;
        }

        if (!type_is_indirect(at)) {
            return false;
        }
    }
}

// Materialises the borrow a 'ref T' destination asks for. Borrowing is implicit,
// so nothing in the source says to take an address -- but every later pass reads
// the tree, so the address-of has to be in it. The same reasoning as the receiver
// adjustment, and the same shape: a real node, typed as the destination.
//
// Returns false for a temporary, which has no address to take.
static bool borrow_into(ResolverState *state, ASTExpr **slot, const Type *destination, Span span) {
    // Only something with a home in memory can be lent. A string that owns and
    // has no home is a temporary -- an owned copy -- and its characters are
    // freed where the expression ends, so the borrow would name freed memory.
    if (type_is_str_ref(destination) &&
        type_registry_deref_of(state->current_scope->type_registry, (*slot)->type) &&
        !is_addressable(*slot)) {
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
    while (type_is_indirect((*slot)->type) && type_pointee(destination) != type_pointee((*slot)->type) &&
           type_pointee(destination) != (*slot)->type) {
        const Type *inner = type_pointee((*slot)->type);

        ASTExpr *hop = ast_deref_expr_create(span, *slot);
        hop->type = inner;
        *slot = hop;
    }

    // A value lending what it holds: the reference is built out of the header's
    // fields rather than being the header read at a narrower width, so the two
    // need not be the same bytes.
    if (lends_by_value(state->current_scope->type_registry, destination, (*slot)->type)) {
        const Deref *deref = type_registry_deref(state->current_scope->type_registry, (*slot)->type);

        ASTExpr *lend = ast_lend_expr_create(span, *slot);
        lend->type = destination;

        // Settled here rather than left for codegen: which bytes name the view
        // is what the library said when it declared the type, and resolving is
        // where a declaration is read.
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

bool is_numeric_type(const Type *t) { return t->kind == TYPE_INT || t->kind == TYPE_FLOAT; }

bool is_integer_type(const Type *t) { return t->kind == TYPE_INT; }

bool is_boolean_type(const Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(const Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

// Characters, however they are named: the owning header that derefs to them, or
// a reference to someone else's. Comparison reads the same two words from
// either, so what may be compared is the pair rather than one of them.
static bool is_string_type(TypeRegistry *registry, const Type *t) {
    return type_registry_deref_of(registry, t) == registry->primitives.str_type || type_is_str_ref(t);
}

// Ordering is left out: '<' on text asks which comes first, and no order is
// defined for it. Equality asks only whether two strings spell the same thing.
static bool is_comparable_type(TypeRegistry *registry, const Type *t) {
    return is_numeric_type(t) || is_boolean_type(t) || is_string_type(registry, t);
}

static const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

// Computes the layout this type owes if it is held by value, and reports back
// the declaration a cycle closes on when that layout is waiting on itself.
//
// A declaration is bound before its fields resolve, so a type may name a struct
// whose width is not yet known -- and every by-value use of one needs that
// width now rather than whenever its own declaration is reached. The
// layout-level questions are exactly the by-value ones: how wide a field is,
// how far an array strides. An indirection asks none of them, which is why it
// is not one of these and why a ring through a 'box' stays finite.
static StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type);

// Rejects a type nothing can hold. How far an unsized value runs is not in its
// type, so a slot, a field or a parameter has no width to reserve for one: a
// reference carries that count and is what every use of it goes through.
//
// Asked where a value is held rather than where the name resolves, since 'ref
// str' resolves the same name and is exactly what is wanted.
static bool reject_unsized(ResolverState *state, const Type *type, Span span, const char *held_as) {
    if (!type || type_is_sized(type)) {
        return false;
    }

    diag_error(state->diagnostics, GAB_ERR_TYPE, span,
               "nothing holds a '%s', so it cannot be %s; write 'ref %s'", type_name(state, type), held_as,
               type_name(state, type));
    return true;
}

// Whether a binary operator accepts operands of this type, reporting why not
// when it does not. Both operands are already known to share the type.
//
// Shared with compound assignment, which applies the same operator to its
// target and its value: 'a %= b' is accepted exactly where 'a % b' is, and
// keeping one copy of the rules is what makes that true rather than intended.
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
    const Type *target =
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

    resolve_expr(state, operand, NULL);

    const Type *from = operand->type;

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

        // Two strings are one operand type however each of them owns: what
        // '==' reads is the characters, and ownership decides who frees the
        // result rather than whether the operator applies.
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

        // An owning string is compared through a reference to its characters,
        // never as the slots it occupies: a header carries a capacity beside
        // the count, and reading it as a reference would compare that capacity
        // as though it were the length.
        if (both_strings) {
            const Type *characters =
                type_registry_ref_to(state->current_scope->type_registry,
                                     state->current_scope->type_registry->primitives.str_type);

            borrow_into(state, &expr->bin_op.left, characters, expr->span);
            borrow_into(state, &expr->bin_op.right, characters, expr->span);
        }

        expr->type = bin_op_yields_bool(expr->bin_op.op)
                         ? type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL)
                         : left_type;

        fold_bin_op(expr);

        break;
    }
    case EXPR_VARIABLE: {
        Symbol *entry = scope_symbol_lookup(state->current_scope, resolver_intern(state, expr->var.name));

        // 'Type::name' where the first half names no module: a function the
        // type owns. Tried after the scope lookup rather than before, so a
        // module keeps the meaning it already had where both could answer.
        if (!entry) {
            entry = resolve_qualified_func(state, expr);
        }

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

        resolve_expr(state, expr->call.target, NULL);

        Symbol *callee = expr->call.target->symbol;

        // The parameters are what the arguments land in, so they are looked up
        // before the arguments are resolved: an argument with no type of its own
        // has nothing else to take one from. Only when the callee is a function
        // whose arity matches, since a parameter list is what is being indexed.
        bool params_known =
            callee && callee->kind == SYMBOL_FUNC && expr->call.args.size == callee->func.param_count;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            resolve_expr(state, expr->call.args.data[i], params_known ? callee->func.params[i] : NULL);
        }

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
    case EXPR_INDEX: {
        resolve_expr(state, expr->index.target, NULL);
        resolve_expr(state, expr->index.index, NULL);

        const Type *target_type = expr->index.target->type;

        if (is_error_type(target_type) || is_error_type(expr->index.index->type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // Reaches through every pointer level, as a field access does: an
        // '[int; 3]' and a 'ref [int; 3]' are indexed the same way.
        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
        }

        if (target_type->kind != TYPE_ARRAY) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot index %s",
                       type_name(state, expr->index.target->type));
            expr->type = resolver_error_type(state);
            break;
        }

        if (expr->index.index->type != state->current_scope->type_registry->primitives.int_type) {
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

        // 'p.health' where p is a 'box Player' reaches through the pointer, as in
        // Go and C's '->'. Every level, since the search is what bounds it: a
        // 'ref box Player' is two hops from the struct, and stopping short would
        // only report a pointer as having no fields.
        while (type_is_indirect(target_type)) {
            target_type = type_pointee(target_type);
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

        expr->field.owner = target_type;
        expr->field.index = (size_t)(field - type_fields(target_type));

        expr->type = field->type;

        // Field access addresses the target's slots, so it inherits the
        // target's symbol and stays assignable through the chain.
        expr->symbol = expr->field.target->symbol;

        break;
    }
    case EXPR_MOVE: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

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
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

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
        if (target_type->kind == TYPE_BOX) {
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

        // The address itself lives in the target's slots, so a deref stays
        // assignable through whatever the target was.
        expr->symbol = expr->unary.target->symbol;
        break;
    }
    case EXPR_NEG: {
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

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
        resolve_expr(state, expr->unary.target, NULL);

        const Type *target_type = expr->unary.target->type;

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
        const Type *type = resolve_type_expr(state, expr->new_expr.type_expr, expr->span);

        if (is_error_type(type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        // A struct has a layout to allocate, and so does a pointer: 'new box T'
        // is a heap slot holding an owning pointer, which is what makes a
        // 'box box T' fillable rather than merely declarable. Anything laid out
        // from fields has a layout to fill, which is a struct or a string --
        // zeroed, the latter is the empty string. A scalar has none: 'new int'
        // would be a boxed scalar, a different feature.
        if (type_field_count(type) == 0 && !type_is_indirect(type)) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "cannot allocate %s; 'new' takes a struct or a pointer", type_name(state, type));
            expr->type = resolver_error_type(state);
            break;
        }

        // A borrow has no owner to name, so a heap slot holding one would
        // outlive whatever it borrows with nothing tracking that.
        if (type->kind == TYPE_REF) {
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

        if (!expected || expected->kind != TYPE_ARRAY) {
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

            // type_accepts rather than identity, for the reason an argument
            // uses it: an element is bound into the array exactly as a value is
            // bound into any slot that owns it.
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

            check_implicit_copy(state, expr->array_lit.elements.data[i], element, value->span);
        }

        expr->type = ok ? expected : resolver_error_type(state);
        break;
    }
    case EXPR_LITERAL: {
        TypeRegistry *registry = state->current_scope->type_registry;

        // A string literal names characters the unit's arena holds, which
        // outlive every value that reads them and are freed with the unit. So
        // it borrows: nothing in a frame allocated them, and nothing there may
        // free them.
        // A literal names characters the unit's arena holds, which outlive every
        // value that reads them. What names them is a reference: no slot holds
        // the characters themselves, so the count rides with the address.
        expr->type = expr->lit.kind == TYPE_STR
                         ? type_registry_ref_to(registry, registry->primitives.str_type)
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
static void check_implicit_copy(ResolverState *state, ASTExpr *value, const Type *destination, Span span) {
    if (!value || value->kind == EXPR_MOVE || is_error_type(value->type)) {
        return;
    }

    if (type_is_copyable(value->type)) {
        return;
    }

    // Binding into a slot that owns nothing borrows rather than copies: the
    // destination never frees what it names, so the owner stays the only one.
    // True of a 'ref T' and of a 'str', which owns nothing through its fields.
    if (destination && !type_is_owned(destination)) {
        return;
    }

    if (!value->symbol || value->symbol->kind != SYMBOL_VAR) {
        return;
    }

    // Only advise the clone where there is one to call. A type that declares
    // none is told so instead, since sending the programmer to a method that
    // does not exist costs them the round trip to find that out.
    const Type *base = receiver_base_type(value->type);

    if (base && type_registry_find_method(state->current_scope->type_registry, base,
                                          resolver_intern(state, string_ref_create("clone")))) {
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

        // Interned in the one shared registry whichever scope named the inner,
        // so 'box Config' is one type however many modules mention it.
        return expr->kind == TYPE_EXPR_REF ? type_registry_ref_to(registry, inner)
                                           : type_registry_box_to(registry, inner);
    }

    case TYPE_EXPR_APPLY: {
        // Looked up rather than resolved: 'Array' names no type on its own, so
        // resolving the base as an expression of its own would report that
        // before this ever saw it.
        Scope *base_scope = resolver_expr_scope(state, expr->apply.base->name);

        const Type *base =
            base_scope ? scope_type_lookup(base_scope, resolver_expr_member(state, expr->apply.base->name))
                       : NULL;

        if (!base) {
            char *name = string_ref_to_cstr(expr->apply.base->name);
            diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        // A generic declaration is instantiated by what it was applied to.
        // Nothing here is 'Vec': what the constructor takes and how its
        // instantiations are laid out are read off the declaration, so a second
        // generic name resolves through this same arm.
        if (base->generic) {
            if (expr->apply.args.size != base->generic->param_count) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not %zu",
                           base->name->data, base->generic->param_count, expr->apply.args.size);
                return resolver_error_type(state);
            }

            const Type *args[GAB_MAX_TYPE_PARAMS];

            for (size_t i = 0; i < expr->apply.args.size; i++) {
                args[i] = resolve_type_expr(state, expr->apply.args.data[i], span);

                if (is_error_type(args[i])) {
                    return resolver_error_type(state);
                }

                if (reject_unsized(state, args[i], span, "a type argument")) {
                    return resolver_error_type(state);
                }

                // The argument's width is what an instantiation is laid out by
                // and what a walk over its elements strides by, so it is owed
                // here -- and a struct still being laid out is one waiting on
                // its own width.
                StructDecl *cycle = element_completes_a_cycle(state, args[i]);

                if (cycle) {
                    diag_error(state->diagnostics, GAB_ERR_TYPE, span, "struct '%s' cannot contain itself",
                               cycle->name->data);
                    return resolver_error_type(state);
                }

                // A zero-width argument would put every element of a block at
                // one address, leaving nothing for a length to count.
                if (type_registry_size_of(registry, args[i]) == 0) {
                    diag_error(state->diagnostics, GAB_ERR_TYPE, span, "a type argument must have a size");
                    return resolver_error_type(state);
                }
            }

            return type_registry_instantiate(registry, base, args, expr->apply.args.size);
        }

        if (base != registry->primitives.array_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "%s does not take an element type",
                       type_name(state, base));
            return resolver_error_type(state);
        }

        // One element type is all 'Array' takes. The list holds however many
        // were written, so a count that does not match is this constructor's
        // own complaint rather than something the shape prevented.
        if (expr->apply.args.size != 1) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'Array' takes one element type");
            return resolver_error_type(state);
        }

        const Type *element = resolve_type_expr(state, expr->apply.args.data[0], span);

        if (is_error_type(element)) {
            return resolver_error_type(state);
        }

        if (reject_unsized(state, element, span, "an array's element")) {
            return resolver_error_type(state);
        }

        // The element's width is what every index strides by, so it is owed
        // here rather than wherever the element's own declaration is reached --
        // and a run of a struct still being laid out is that struct waiting on
        // its own width, which is a containment cycle however many elements
        // long the run is.
        StructDecl *cycle = element_completes_a_cycle(state, element);

        if (cycle) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "struct '%s' cannot contain itself",
                       cycle->name->data);
            return resolver_error_type(state);
        }

        // A zero-width element would make every index the same address, so
        // there would be nothing for a length to count.
        size_t element_size = type_registry_size_of(registry, element);

        if (element_size == 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "an array's element must have a size");
            return resolver_error_type(state);
        }

        int32_t length = expr->apply.length;

        // An array of nothing has no element to index and no width to lay out.
        if (length <= 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span, "an array's length must be positive, not %d",
                       length);
            return resolver_error_type(state);
        }

        // The elements live in the frame, so the whole run has to be reachable
        // by the operands that address it: a slot index names where the array
        // starts, and a byte offset reaches the element within it.
        size_t bytes = element_size * (size_t)length;

        if (bytes > GAB_MAX_TYPE_BYTES) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                       "'Array' of %d needs %zu bytes, over the %d a frame addresses", length, bytes,
                       GAB_MAX_TYPE_BYTES);
            return resolver_error_type(state);
        }

        return type_registry_array_of(registry, element, length);
    }

    case TYPE_EXPR_NAME:
        break;
    }

    Scope *scope = resolver_expr_scope(state, expr->name);

    // Walks outward from that scope, so a module's own 'Config' shadows a
    // root-namespace one and 'int' resolves with no import. A 'Module::Type'
    // expression resolved to that module's scope above, and its bare member
    // name is what that scope holds.
    const Type *type = scope ? scope_type_lookup(scope, resolver_expr_member(state, expr->name)) : NULL;

    if (!type) {
        char *name = string_ref_to_cstr(expr->name);
        diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
        free(name);

        return resolver_error_type(state);
    }

    // 'Array' alone names no type: every array is '[T; N]'.
    if (type == registry->primitives.array_type) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span,
                   "'Array' needs an element type and a length, as '[int; 3]'");
        return resolver_error_type(state);
    }

    // Nor does a generic declaration: what it takes is what makes it a type.
    if (type->generic) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, span, "'%s' needs a type argument, as '%s<int>'",
                   type->name->data, type->name->data);
        return resolver_error_type(state);
    }

    return type;
}

static void resolve_stmt(ResolverState *state, ASTStmt *stmt);

// The declaration whose width holding this type by value depends on, or NULL
// when it depends on none of this unit's.
//
// An array is a run of its element, so it is held by value exactly as the
// element is and the walk continues through it. An indirection is where the
// walk stops: what a 'box T' or a 'ref T' costs does not depend on T's width,
// which is what keeps a ring through one finite and is how a linked structure
// is written.
//
// A linear scan because the list is one unit's structs and the question is
// asked once per field of each.
static StructDecl *decl_held_by_value(ResolverState *state, const Type *type) {
    while (type && type->kind == TYPE_ARRAY) {
        type = type_array_element(type);
    }

    for (size_t i = 0; i < state->struct_decls.size; i++) {
        if (state->struct_decls.data[i]->type == type) {
            return state->struct_decls.data[i];
        }
    }

    return NULL;
}

// Binds the struct's name to a type with no layout. Everything a declaration is
// -- the name, the scope, the fields as written -- is settled here, and nothing
// it needs comes from another declaration, so a whole unit's structs can be
// bound before any field resolves.
static StructDecl *declare_struct(ResolverState *state, ASTStmt *stmt) {
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
        return NULL;
    }

    Type *type = type_registry_declare_struct(state->current_scope->type_registry, struct_name,
                                              stmt->struct_decl.fields.size);

    scope_decl_type(state->current_scope, struct_name, type);

    StructDecl *decl = arena_alloc(resolver_owner_arena(state), sizeof(StructDecl));

    *decl = (StructDecl){
        .stmt = stmt,
        .scope = state->current_scope,
        .name = struct_name,
        .type = type,
        .state = STRUCT_DECLARED,
    };

    struct_decl_list_add(&state->struct_decls, decl);

    return decl;
}

static void layout_struct(ResolverState *state, StructDecl *decl);

// The declaration a by-value use of this type closes a containment cycle on, or
// NULL when the use is finite. Forces the layout of whatever declaration it reaches, so
// that a field naming a struct declared further down has a width by the time
// this returns -- and a declaration reached while it is still being laid out is
// the cycle, since its width is waiting on itself. A field and an array's
// element ask this identically: both are held by value.
//
// That declaration is what the diagnostic names rather than the one being laid
// out: it is the one the ring closes on.
static StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type) {
    StructDecl *decl = decl_held_by_value(state, type);

    if (!decl) {
        return NULL;
    }

    if (decl->state == STRUCT_LAYING_OUT) {
        return decl;
    }

    layout_struct(state, decl);

    return NULL;
}

// Whether the field's type is a declaration whose own layout failed. Asked
// after the cycle walk, which is what forced that layout.
static bool field_type_failed(ResolverState *state, const Type *type) {
    StructDecl *decl = decl_held_by_value(state, type);

    return decl && decl->state == STRUCT_POISONED;
}

// Derives the struct's layout from its declaration, once: resolves the fields,
// rejects what no slot may hold, and computes the offsets and the width.
//
// Memoized rather than run where the declaration was bound, because a field may
// name a struct declared further down the file: forcing that field's layout
// from here is what orders the computation by dependency rather than by the
// order the declarations were written in.
static void layout_struct(ResolverState *state, StructDecl *decl) {
    if (decl->state != STRUCT_DECLARED) {
        return;
    }

    decl->state = STRUCT_LAYING_OUT;

    ASTStmt *stmt = decl->stmt;
    Type *type = decl->type;

    // The fields resolve in the scope the struct was declared in, which is not
    // where the layout was demanded from: a struct declared in one module is
    // laid out when another names it.
    Scope *enclosing = state->current_scope;
    state->current_scope = decl->scope;

    bool poisoned = false;

    for (size_t i = 0; i < stmt->struct_decl.fields.size; i++) {
        ASTField *field = stmt->struct_decl.fields.data[i];
        String *field_name = resolver_intern(state, field->name);

        if (type_find_field(type, field_name)) {
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

        // Asked before the field is added, since a struct waiting on its own
        // width has no width to add.
        StructDecl *cycle = element_completes_a_cycle(state, field_type);

        if (cycle) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, field->span, "struct '%s' cannot contain itself",
                       cycle->name->data);
            poisoned = true;
            continue;
        }

        // A field whose own layout failed carries the failure up rather than
        // being laid out at the width it does not have. Silent, because the
        // field's declaration already reported why: this struct is unusable as
        // a consequence, not as a second fault.
        if (field_type_failed(state, field_type)) {
            poisoned = true;
            continue;
        }

        if (reject_unsized(state, field_type, field->span, "a field")) {
            poisoned = true;
            continue;
        }

        type_add_field(type, field_name, field_type);
    }

    state->current_scope = enclosing;

    // A struct with a bad field has no layout worth computing: its name stays
    // bound so that the rest of the unit resolves against something, and the
    // fields that did resolve are dropped rather than laid out into a width
    // that would be wrong.
    if (poisoned) {
        decl->state = STRUCT_POISONED;
        scope_withdraw_type(decl->scope, decl->name);
        return;
    }

    type_registry_layout_of(state->current_scope->type_registry, type);
    type_registry_drop_of(state->current_scope->type_registry, type);

    decl->state = STRUCT_LAID_OUT;
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
static const Type *resolve_param_type(ResolverState *state, ASTField *param) {
    const Type *type = resolve_type_expr(state, param->type_expr, param->span);

    if (reject_unsized(state, type, param->span, "a parameter")) {
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
// Declares a function into a type's own set rather than into a scope: it has no
// free-standing name, so 'Player::update' and 'Enemy::update' coexist and
// neither is reachable as a bare 'update'.
//
// Every parameter is an ordinary parameter, parameter zero included. Whether a
// value reaches this is not a property of the declaration but of the call:
// 'p.damage(30)' is sugar for 'Player::damage(p, 30)', and what makes the sugar
// apply is parameter zero's type rather than anything written here.
static void declare_owned(ResolverState *state, ASTStmt *stmt) {
    const Type *owner = resolve_type_expr(state, stmt->func_decl.owner, stmt->span);

    if (is_error_type(owner)) {
        return;
    }

    // A unit declares functions on its own structs only. A builtin carries the
    // ones the VM registered, and a second set declared over them would have no
    // module to belong to and no way to be told apart from the first.
    if (owner->kind != TYPE_STRUCT) {
        diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                   "a function belongs to a struct this module declares, not to %s", type_name(state, owner));
        return;
    }

    if (!owner->name || !scope_type_lookup_local(state->current_scope, owner->name)) {
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

    // Not scope_decl_func: this name lives on the type, not in a scope. The
    // Symbol is built directly for the same reason.
    Symbol *func = arena_alloc(resolver_owner_arena(state), sizeof(Symbol));
    *func = (Symbol){
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

    size_t param_count = stmt->func_decl.params.size;

    if (param_count > 0) {
        func->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->func.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            func->func.params[i] = resolve_param_type(state, stmt->func_decl.params.data[i]);
        }
    }

    // 'clone' is the remedy the implicit-copy diagnostic names, so its shape is
    // fixed: it duplicates what it is given and yields another of the same type.
    // Checked here rather than at a call so a type declaring the wrong thing
    // hears about it where it wrote it.
    if (name == resolver_intern(state, string_ref_create("clone"))) {
        if (return_type != owner) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'clone' duplicates what it takes, so it must return %s, not %s", owner->name->data,
                       type_name(state, return_type));
            return;
        }

        if (param_count != 1) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "'clone' duplicates what it takes and takes nothing else");
            return;
        }
    }

    if (!type_registry_add_method(state->current_scope->type_registry, owner, name, func)) {
        diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' already has a function '%s'",
                   owner->name->data, name->data);
        return;
    }

    stmt->func_decl.symbol = func;
}

// 'Type::name' resolved against the type's own function set. NULL when the name
// is not qualified or when the first half names no type -- a module-qualified
// name is looked up in a scope before this is reached.
static Symbol *resolve_qualified_func(ResolverState *state, ASTExpr *expr) {
    StringRef owner_ref, member_ref;

    if (!string_ref_split_colons(expr->var.name, &owner_ref, &member_ref)) {
        return NULL;
    }

    const Type *owner = scope_type_lookup(state->current_scope, resolver_intern(state, owner_ref));

    if (!owner) {
        return NULL;
    }

    String *member = resolver_intern(state, member_ref);
    Symbol *found = type_registry_find_method(state->current_scope->type_registry, owner, member);

    if (!found) {
        diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no function '%s'",
                   owner->name->data, member->data);

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
        func->func.params = arena_alloc(resolver_owner_arena(state), param_count * sizeof(const Type *));
        func->func.param_count = param_count;

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            func->func.params[i] = resolve_param_type(state, param);
        }
    }
}

// Walks the body in a scope holding the parameters. The signature is already
// resolved, so this re-resolves each parameter's TypeExpr only to bind its
// name; the types a caller sees were settled by declare_func.
static void resolve_func_body(ResolverState *state, ASTStmt *stmt) {
    size_t errors_before = diagnostics_count(state->diagnostics);

    resolver_enter_scope(state);

    for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
        ASTField *param = stmt->func_decl.params.data[i];

        String *param_name = resolver_intern(state, param->name);
        const Type *param_type = resolve_type_expr(state, param->type_expr, param->span);

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
        resolve_expr(state, stmt->expr.value, NULL);
        break;
    }
    case STMT_VAR_DECL: {
        // Resolved before the initializer, so a value with no type of its own
        // has the annotation to take one from.
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
                    // A borrow reaching an owning string is the one mismatch
                    // with a remedy to name: the characters belong to the
                    // arena, and 'to_owned()' is what copies them into a string
                    // this slot may free.
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
        // A struct nested in a body has nothing above it that could have
        // declared it, so its name is bound and its layout forced together.
        if (!stmt->struct_decl.declared) {
            StructDecl *decl = declare_struct(state, stmt);

            if (decl) {
                layout_struct(state, decl);
            }
        }
        break;
    }
    case STMT_ASSIGN: {
        resolve_expr(state, stmt->assign.target, NULL);

        // The target is resolved first, so what it holds is what the value has
        // to be -- which a value with no type of its own needs told.
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
        resolve_expr(state, stmt->ifstmt.condition, NULL);

        // A branch has to have something to branch on, and bool is the only
        // thing the jump opcodes read. An already-poisoned condition has
        // reported its own error, so it is let through rather than complained
        // about twice.
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

        // The initializer's scope encloses the condition, the post clause, and
        // the body, so 'for let i = 0; i < n; i = i + 1' scopes i to the loop.
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
        resolve_expr(state, stmt->ret.result, state->func_context.return_type);

        const Type *expected = state->func_context.return_type;
        const Type *actual = stmt->ret.result ? stmt->ret.result->type : NULL;

        // A NULL type here means "no value", which is a distinct case from a
        // poisoned one: it must still be checked against the declared type.
        bool poisoned = (expected && expected->kind == TYPE_ERROR) || (actual && actual->kind == TYPE_ERROR);

        // type_accepts once both are present, so a function declaring 'ref T'
        // may return an owned 'box T' — it lends what it was given rather than
        // handing ownership out. A NULL on either side is "no value", which
        // only identity settles.
        bool accepted = actual && expected
                            ? type_accepts(state->current_scope->type_registry, expected, actual)
                            : actual == expected;

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
        .struct_decls = struct_decl_list_create(),
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

    // Layouts second, once every name in the unit is bound. A field may name a
    // struct declared further down, and one may name the other through a 'box'
    // in both directions -- neither resolves while the layouts are computed in
    // the order the declarations were written.
    //
    // Forced here rather than left to whoever first needs a width, because the
    // fields are read off this unit's AST, which does not outlive it: a struct
    // leaves this unit laid out or poisoned, never merely declared.
    for (size_t i = 0; i < state.struct_decls.size; i++) {
        layout_struct(&state, state.struct_decls.data[i]);
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

    return diagnostics_count(diagnostics) == errors_before;
}
