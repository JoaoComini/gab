#include "ast.h"

#include "ast/flow.h"
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
    script->imports = ast_import_list_create();

    return script;
}

void ast_script_destroy(ASTScript *script) {
    ast_stmt_list_free(&script->statements);
    ast_import_list_free(&script->imports);

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

    // What the innermost loop's 'break' edges carry, owned by the STMT_FOR
    // being walked. A 'break' leaves the loop without passing the condition or
    // the back-edge, so its state joins the post-loop state directly; merging
    // only the body's fall-through would lose it and read a slot moved before
    // a 'break' as live afterwards.
    Flow *breaks;
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

    // Per-slot state at the point the walk has reached. Forked and merged at
    // every join, so what it says about a slot is what holds on every path
    // that arrives there.
    Flow flow;

    // Set while the target of an assignment is visited. A plain 'x = v'
    // writes x rather than reading it, so a dead x is revived by the write
    // instead of being an error. 'x.f = v' and '*x = v' do read x, and clear
    // this before visiting what they reach through.
    bool assigning;

    // Set while the outermost field of an assignment target is visited. 'h.b'
    // in 'h.b = v' is stored into rather than read, so it is what makes the
    // field readable; a nested 'h.b' in 'h.b.n = v' clears this and is read.
    bool assigning_field;

    // Set while a loop body is walked a second time to check it against the
    // state the back-edge carries. Declarations reuse the symbol the first
    // walk cached rather than declaring again, so the second walk checks
    // without disturbing the scope the first one built.
    bool rechecking;

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

// The block depth of what a pointer-valued expression points at, or 0 when it
// points at nothing known. Comparing depths is what catches a pointer being
// moved somewhere that outlives its pointee: a smaller depth is a longer life.
//
// A variable's depth comes from the flow state, so what this answers is what
// holds on every path reaching the expression rather than whatever the most
// recent assignment happened to store.
static int pointee_depth(ResolverState *state, const ASTExpr *expr) {
    if (!expr) {
        return 0;
    }

    switch (expr->kind) {
    case EXPR_ADDR_OF: {
        const Symbol *symbol = expr->unary.target->symbol;

        return symbol ? symbol->scope_depth : 0;
    }
    case EXPR_VARIABLE:
        return expr->symbol ? flow_get(&state->flow, expr->symbol).pointee_depth : 0;
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
            int depth = pointee_depth(state, expr->call.args.data[i]);

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

// Whether this expression is a struct local's owning pointer field -- 'h.b'
// where h is a struct variable and b owns. Those are the fields codegen nulls
// at the declaration, so those are the ones whose written-ness has to be
// tracked. A field reached through a pointer is excluded: what it belongs to
// was not declared here, so nothing local says whether it was written.
//
// Fills 'out_index' with the field's index in its struct, which is the bit the
// written-field set uses.
static bool owning_field_of_local(const ASTExpr *expr, Symbol **out_symbol, unsigned int *out_index) {
    if (expr->kind != EXPR_FIELD || expr->field.target->kind != EXPR_VARIABLE) {
        return false;
    }

    Symbol *symbol = expr->field.target->symbol;
    const Type *struct_type = expr->field.target->type;

    if (!symbol || symbol->kind != SYMBOL_VAR || !struct_type || struct_type->kind != TYPE_STRUCT) {
        return false;
    }

    const TypeField *field = expr->field.field;

    if (!field || !type_is_pointer(field->type) || field->type->is_ref) {
        return false;
    }

    size_t index = (size_t)(field - struct_type->fields);

    if (index >= FLOW_MAX_FIELDS) {
        return false;
    }

    *out_symbol = symbol;
    *out_index = (unsigned int)index;

    return true;
}

// The written-field set a declaration's initializer hands its new variable.
// Taking a whole struct -- 'let g = move h', or a copy of one -- makes g's
// fields exactly as written as h's were, so the set travels with the value.
//
// Everything else answers all-written. A declaration with no initializer holds
// the nulls codegen wrote and needs none of its fields marked; every other
// initializer is a value this frame did not build field by field -- a call
// result, a 'new' -- and nothing local says one of its fields is unwritten.
static uint64_t initialized_fields(ResolverState *state, ASTExpr *initializer) {
    if (!initializer) {
        return 0;
    }

    ASTExpr *source = initializer->kind == EXPR_MOVE ? initializer->unary.target : initializer;

    if (source->kind == EXPR_VARIABLE && source->symbol && source->symbol->kind == SYMBOL_VAR) {
        return flow_get(&state->flow, source->symbol).written_fields;
    }

    return UINT64_MAX;
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

// The type a receiver names, looking through one level of pointer. '*Player' and
// 'Player' share a method set, so both land on the same Type.
//
// A builtin qualifies: methods hang on the Type, and nothing about the map
// requires the type to be one a script declared.
static Type *receiver_base_type(Type *type) {
    if (type_is_pointer(type)) {
        type = type->pointee;
    }

    return type;
}

// Checks each argument against the parameter type in the same position. Shared
// by both call forms, which differ only in where their parameter list starts:
// a method's skips the receiver.
static bool type_accepts(Type *to, Type *from);
static void check_implicit_copy(ResolverState *state, ASTExpr *value, Type *destination, Span span);

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
            continue;
        }

        // An owning parameter takes ownership, so the argument is bound into it
        // exactly as it would be into a 'let': a non-copyable one needs a move.
        check_implicit_copy(state, arg, param_type, arg->span);
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

bool is_integer_type(Type *t) { return t->kind == TYPE_INT; }

bool is_boolean_type(Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

bool is_string_type(Type *t) { return t->kind == TYPE_STRING; }

// Ordering is left out: '<' on text asks which comes first, and no order is
// defined for it. Equality asks only whether two strings spell the same thing.
bool is_comparable_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t) || is_string_type(t); }

Type *ast_script_resolve_type(ResolverState *state, TypeSpec *spec, Span span);

// Whether a binary operator accepts operands of this type, reporting why not
// when it does not. Both operands are already known to share the type.
//
// Shared with compound assignment, which applies the same operator to its
// target and its value: 'a %= b' is accepted exactly where 'a % b' is, and
// keeping one copy of the rules is what makes that true rather than intended.
static bool bin_op_accepts(ResolverState *state, BinOp op, Type *type, Span span) {
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

    ast_script_expr_visit(state, operand);

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

        if (!bin_op_accepts(state, expr->bin_op.op, left_type, expr->span)) {
            expr->type = resolver_error_type(state);
            break;
        }

        expr->type = bin_op_yields_bool(expr->bin_op.op)
                         ? type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL)
                         : left_type;
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

        // A slot moved out of no longer names what it held, so reading it
        // would be a second use of something already given away.
        if (entry->kind == SYMBOL_VAR && !state->assigning) {
            FlowInit init = flow_get(&state->flow, entry).init;

            if (init == FLOW_MOVED) {
                char *name = string_ref_to_cstr(expr->var.name);
                diag_error(state->diagnostics, GAB_ERR_LIFETIME, expr->span,
                           "'%s' was moved out of and no longer holds a value", name);
                free(name);
            } else if (init == FLOW_UNINIT && type_is_pointer(entry->var.type)) {
                // Only a pointer. An unwritten slot holds whatever the frame
                // last left there: read as an int that is a wrong answer, read
                // as a pointer it is an address nothing chose. A struct's own
                // slots exist from its declaration, so building one field by
                // field is how a struct is made rather than a use of nothing.
                char *name = string_ref_to_cstr(expr->var.name);
                diag_error(state->diagnostics, GAB_ERR_LIFETIME, expr->span,
                           "'%s' is read before it is given a value", name);
                free(name);
            }
        }
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
        // Only this field is the one being stored into; what it reaches
        // through is read as usual.
        bool stored_into = state->assigning_field;
        state->assigning_field = false;

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

        // Reaching through an owning field that nothing has written to
        // dereferences the null the declaration put there. Writing the field
        // is a store into it, not a read of it, so 'assigning' excludes the
        // 'h.b = ...' that makes it readable.
        Symbol *field_owner;
        unsigned int field_index;

        if (!stored_into && owning_field_of_local(expr, &field_owner, &field_index)) {
            FlowSlot slot = flow_get(&state->flow, field_owner);

            if (!(slot.written_fields & ((uint64_t)1 << field_index))) {
                diag_error(state->diagnostics, GAB_ERR_LIFETIME, expr->span,
                           "'%s' is read before it is given a value", field_name->data);
            }
        }
        break;
    }
    case EXPR_MOVE: {
        ast_script_expr_visit(state, expr->unary.target);

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

        Symbol *source = expr->unary.target->symbol;

        // Moving out of anything but a named slot has nothing to kill: a
        // temporary already owns what it produced, and no later use can name
        // it.
        if (!source || source->kind != SYMBOL_VAR) {
            break;
        }

        // The operand is read before it dies, so a slot that is already dead
        // is caught by the ordinary use check above rather than here.
        FlowSlot slot = flow_get(&state->flow, source);

        // Only 'init' changes: the slot is dead, but what its fields hold is
        // what the destination now receives, and is still true of the slot if
        // an assignment later revives it.
        slot.init = FLOW_MOVED;
        flow_set(&state->flow, source, slot);
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
    case EXPR_NOT: {
        ast_script_expr_visit(state, expr->unary.target);

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

    diag_error(state->diagnostics, GAB_ERR_LIFETIME, span,
               "%s owns what it holds, so binding it needs 'move' to transfer ownership, or 'clone' to "
               "duplicate it",
               type_name(state, value->type));
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

    int depth = pointee_depth(state, value);

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
// Resolves one parameter's declared type.
//
// A parameter spells ownership the way a local and a field do: bare '*T' owns
// what it is given and frees it when the call ends, 'ref T' borrows and frees
// nothing. Which one a signature declares is what a call site reads to know
// whether it must move, so the two may not be written interchangeably.
static Type *resolve_param_type(ResolverState *state, ASTField *param) {
    return ast_script_resolve_type(state, param->type_spec, param->span);
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

    // A script declares methods on its own structs only. A builtin carries the
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
    // spelling -- 'p.consume()' gives no place to mark the transfer -- so '*T'
    // here would be an ownership nothing could grant.
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

    // Go's rule: a method belongs with the module that declares its receiver,
    // so the type has to be one this scope holds rather than one it inherits.
    if (!scope_type_lookup_local(state->current_scope, base->name)) {
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

    // A body is outside any loop the declaration sits in, so it has no 'break'
    // edge to contribute to until one of its own loops opens.
    state->func_context.breaks = NULL;

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

        Symbol *var = state->rechecking ? stmt->var_decl.symbol
                                        : scope_decl_var(state->current_scope,
                                                         resolver_intern(state, stmt->var_decl.name), type);

        if (!var) {
            char *name = string_ref_to_cstr(stmt->var_decl.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                       name);
            free(name);
            break;
        }

        check_implicit_copy(state, stmt->var_decl.initializer, type, stmt->span);

        // A declaration is always at the current depth, so it can never outlive
        // its initializer; what it records is the depth, for later assignments
        // and returns to check against.
        flow_set(&state->flow, var,
                 (FlowSlot){.init = stmt->var_decl.initializer ? FLOW_INIT : FLOW_UNINIT,
                            .pointee_depth = pointee_depth(state, stmt->var_decl.initializer),
                            .written_fields = initialized_fields(state, stmt->var_decl.initializer)});

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
        // Only a bare name is purely written. Reaching through a field or a
        // dereference reads the slot first, so those are visited as uses.
        //
        // An owning field is the exception in one direction: 'h.b = v' stores
        // into the field rather than through it, so it is the write that makes
        // the field readable. 'h.b.n = v' still reaches through h.b and reads
        // it, which is why this looks at the target itself and not the chain.
        state->assigning = stmt->assign.target->kind == EXPR_VARIABLE;
        state->assigning_field = stmt->assign.target->kind == EXPR_FIELD;
        ast_script_expr_visit(state, stmt->assign.target);
        state->assigning = false;
        state->assigning_field = false;

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
            // An owning field takes ownership of what is stored in it, exactly
            // as a 'let' does, so a non-copyable value needs a move.
            check_implicit_copy(state, stmt->assign.value, target_type, stmt->span);
            check_pointer_lifetime(state, stmt->assign.value, 0, stmt->span, "stored here");

            // The field now holds what was stored, so reaching through it is
            // no longer a null dereference.
            Symbol *field_owner;
            unsigned int field_index;

            if (owning_field_of_local(stmt->assign.target, &field_owner, &field_index)) {
                FlowSlot slot = flow_get(&state->flow, field_owner);
                slot.written_fields |= (uint64_t)1 << field_index;
                flow_set(&state->flow, field_owner, slot);
            }
            break;
        }

        Symbol *target = stmt->assign.target->symbol;

        if (target && target->kind == SYMBOL_VAR) {
            check_implicit_copy(state, stmt->assign.value, target_type, stmt->span);
            check_pointer_lifetime(state, stmt->assign.value, target->scope_depth, stmt->span,
                                   "assigned here");

            // The variable now points at whatever was just stored in it.
            flow_set(
                &state->flow, target,
                (FlowSlot){.init = FLOW_INIT, .pointee_depth = pointee_depth(state, stmt->assign.value)});
        }
        break;
    }
    case STMT_COMPOUND_ASSIGN: {
        ast_script_expr_visit(state, stmt->compound_assign.target);
        ast_script_expr_visit(state, stmt->compound_assign.value);

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

        // Nothing here tracks pointee depth or ownership the way plain
        // assignment does: the operators this node carries are arithmetic, so
        // the target is an int or a float and never names an object.
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

        // Each arm walks from the state at the branch point, and what holds
        // after the 'if' is what holds on both arms. An arm that cannot fall
        // through -- one ending in 'return' -- contributes nothing to the
        // merge, so the other arm's answer survives it.
        Flow before;
        flow_init(&before, state->compile_arena);
        flow_copy(&before, &state->flow);

        ast_script_stmt_visit(state, stmt->ifstmt.then_block);

        Flow after_then;
        flow_init(&after_then, state->compile_arena);
        flow_copy(&after_then, &state->flow);

        flow_copy(&state->flow, &before);
        ast_script_stmt_visit(state, stmt->ifstmt.else_block);

        flow_merge(&state->flow, &after_then);
        break;
    }
    case STMT_FOR: {
        Scope *outer_scope = state->current_scope;

        // The initializer's scope encloses the condition, the post clause, and
        // the body, so 'for let i = 0; i < n; i = i + 1' scopes i to the loop.
        if (state->rechecking) {
            state->current_scope = stmt->forstmt.scope;
        } else {
            resolver_enter_scope(state);
            stmt->forstmt.scope = state->current_scope;
        }

        ast_script_stmt_visit(state, stmt->forstmt.init);

        if (stmt->forstmt.condition) {
            ast_script_expr_visit(state, stmt->forstmt.condition);

            Type *condition_type = stmt->forstmt.condition->type;

            if (condition_type && !is_error_type(condition_type) && !is_boolean_type(condition_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->forstmt.condition->span,
                           "'for' requires a boolean condition, found %s", type_name(state, condition_type));
            }
        }

        // The body may run any number of times, so what holds at its head is
        // what holds on the way in and on the way round alike.
        //
        // The first walk resolves the body -- declaring its locals -- and
        // discovers what the back-edge carries. Merging that into the state
        // from before the body gives the state at the head of a second
        // iteration, and the body is then walked again to check against it.
        // Without the second walk a borrow taken late in one iteration would
        // never be checked against the code that reads it early in the next.
        //
        // The second walk declares nothing: 'rechecking' makes a declaration
        // reuse the symbol the first walk cached. Only that walk reports, so
        // the first walk's diagnostics are rolled back and any real error is
        // reported once, against the merged state that subsumes it.
        Flow before_body;
        flow_init(&before_body, state->compile_arena);
        flow_copy(&before_body, &state->flow);

        size_t reported = diagnostics_count(state->diagnostics);

        // Starts unreachable: no 'break' has been taken, so a loop without one
        // contributes nothing to the state after it.
        Flow breaks;
        flow_init(&breaks, state->compile_arena);
        breaks.unreachable = true;

        Flow *outer_breaks = state->func_context.breaks;
        state->func_context.breaks = &breaks;

        state->func_context.loop_depth++;
        ast_script_stmt_visit(state, stmt->forstmt.body);
        ast_script_stmt_visit(state, stmt->forstmt.post);

        diagnostics_truncate(state->diagnostics, reported);

        flow_merge(&state->flow, &before_body);

        // The second walk sees the same 'break's against the merged state that
        // subsumes the first walk's, so its edges replace rather than join
        // them: the state after the loop is built from the walk that reports.
        flow_init(&breaks, state->compile_arena);
        breaks.unreachable = true;

        state->rechecking = true;
        ast_script_stmt_visit(state, stmt->forstmt.body);

        // Visited after the body, matching when it runs, though it is the
        // initializer's scope either way.
        ast_script_stmt_visit(state, stmt->forstmt.post);
        state->rechecking = false;
        state->func_context.loop_depth--;

        // The body may run zero times, so the state before it is also a way to
        // arrive after the loop -- as is every 'break' that left it.
        flow_merge(&state->flow, &before_body);
        flow_merge(&state->flow, &breaks);

        state->func_context.breaks = outer_breaks;

        state->current_scope = outer_scope;
        break;
    }
    case STMT_JUMP: {
        if (state->func_context.loop_depth == 0) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "'%s' is only valid inside a loop",
                       stmt->jump.is_break ? "break" : "continue");
        }

        // Where this jump goes decides which join it feeds. A 'break' arrives
        // after the loop, so it joins the accumulated break state; a
        // 'continue' arrives at the back-edge, which the body's own walk
        // already merges. Either way nothing falls through to the next
        // statement.
        if (stmt->jump.is_break && state->func_context.breaks) {
            flow_merge(state->func_context.breaks, &state->flow);
        }

        state->flow.unreachable = true;
        break;
    }
    case STMT_BLOCK: {
        Scope *outer = state->current_scope;

        // A recheck walks the block the first walk built, so its locals are
        // found where they were declared.
        if (state->rechecking) {
            state->current_scope = stmt->block.scope;
        } else {
            resolver_enter_scope(state);
            stmt->block.scope = state->current_scope;
        }

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            ast_script_stmt_visit(state, stmt->block.list.data[i]);
        }

        state->current_scope = outer;
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

        // Nothing after a return runs, so what this state says about a slot
        // must not reach a merge as though it were one way of arriving there.
        state->flow.unreachable = true;
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
        .imports = &script->imports,
        .module_name =
            script->module_name.data ? string_from_ref(global_scope->strings, script->module_name) : NULL,
        .func_context =
            {
                .return_type = NULL,
            },
        .diagnostics = diagnostics,
    };

    flow_init(&state.flow, compile_arena);

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
