#ifndef GAB_AST_CHECK_H
#define GAB_AST_CHECK_H

/* What the passes resolving a module share. Not an interface anything outside resolution reads:
 * 'resolve.h' states that, and this states how the passes behind it reach each other. */

#include "ast/ast.h"
#include "ast/facts.h"
#include "ast/pending.h"
#include "diagnostics.h"
#include "function_registry.h"
#include "scope.h"
#include "type/type_registry.h"

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

/* Where a lookup happens, and what it may reach. One value, so resolving something out of place
 * saves and restores it whole rather than field by field. */
typedef struct {
    Scope *scope;

    /* Where a declaration being resolved belongs, which is the module while a top-level statement is
     * read: a scope binding type parameters stands between it and the file, and binds none of them. */
    Scope *declaring;

    /* The file being resolved, whose imports are the ones its statements may name. */
    const ASTFile *file;

    /* The modules that file may name, which is what an unqualified name reaches past this module. */
    const Visible *visible;

    FuncContext func;

    /* The bound on each type parameter of the declaration being resolved, by index. An owner's
     * parameters are prepended to a member's, so one list numbers both and the declaration keeps it. */
    const TypeParamBound *param_bounds;
} Env;

/* What every module resolved in one compilation shares, and what none of them may change. */
typedef struct {
    Arena *arena;

    TypeRegistry *types;
    FunctionRegistry *functions;
    StringPool *strings;

    Diagnostics *diagnostics;
} Global;

/* Resolving one module: what it shares with the compilation, what it is concluding, and where it is. */
typedef struct ResolverState {
    const Global *global;

    Env env;

    /* Where this module's declarations land, which every file of it declares into. */
    Scope *module_scope;

    String *module_name;

    bool declares_intrinsics;

    /* What resolving concluded about each node, and the bodies generation will lower. */
    Facts *facts;
    PendingBodies *work;

    StructDeclList struct_decls;
    StructDeclList resolving;
} ResolverState;

static inline const Type *resolver_error_type(const ResolverState *state) {
    return type_registry_error_type(state->global->types);
}

static inline String *resolver_intern(const ResolverState *state, StringRef ref) {
    return string_from_ref(state->global->strings, ref);
}

static inline bool is_error_type(const Type *type) { return !type || type_kind(type) == TYPE_ERROR; }

/* The type a value parameter and an array length both have, which is the only one they can have. */
static inline const Type *i32_type(const ResolverState *state) {
    return type_registry_get_primitive(state->global->types, TYPE_I32);
}

/* How a value the source wrote is named where a diagnostic reports it. */
const char *type_name(ResolverState *state, const Type *type);

/* Whether a place expression names something with an address, rather than a value in flight. */
bool is_addressable(ResolverState *state, const ASTExpr *expr);

/* The local a chain of projections ends at, with its own pointers stripped. */
const Type *receiver_base_type(const Type *type);

/* Whether a value of one type may stand where the other is wanted, borrowing and unsizing included. */
bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from);
bool accepts_by_borrowing(const Type *to, const Type *from);
bool reads_as_a_view(TypeRegistry *registry, const Type *to, const Type *from);
bool lends_by_pointer(const Type *to, const Type *from);
bool unsizes_to_a_slice(const Type *to, const Type *from);
const Type *derefs_to(TypeRegistry *registry, const Type *type);

/* Records the coercion a value needs to reach its destination, and reports where it cannot. */
bool borrow_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span);
void adjust_derefs(ResolverState *state, Adjustment *adjustment, const Type *from, unsigned int count);

/* Marks a value as given away where its destination owns what it holds. */
void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span);

/* The type a written one denotes, which is the error type where it denotes none. */
const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

/* The element of a run, rejected where it has no size or would contain itself. */
const Type *resolve_element_type(ResolverState *state, TypeExpr *expr, Span span, const char *held_as);

/* Reports a type with no width where one is needed; true where it did. */
bool reject_unsized(ResolverState *state, const Type *type, Span span, const char *held_as);

/* Which kind of parameter a bound declares, which its syntax alone says. */
BoundKind bound_kind_of(const TypeRegistry *registry, StringPool *strings, const TypeExpr *bound);

bool bind_type_param(TypeRegistry *registry, Scope *params, String *name, size_t index, BoundKind kind);

/* What a name denotes here, and where a name qualified by a module resolves through. Supplied by
 * the pass that declares, since what a file may name is what it read. */
Scope *resolver_expr_scope(ResolverState *state, StringRef name);
String *resolver_expr_member(ResolverState *state, StringRef name);
Symbol *resolver_resolve_name(ResolverState *state, Scope *scope, String *name);
const KnownNames *resolver_names(ResolverState *state);
bool names_the_same(ResolverState *state, StringRef ref, const String *known);
bool reject_self_as_name(ResolverState *state, String *name, Span span);

/* Whether an element would close a containment cycle, and how that cycle is reported. */
StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type);
void report_containment_cycle(ResolverState *state, StructDecl *closes_on, Span span);

#endif
