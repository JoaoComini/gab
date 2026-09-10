#ifndef GAB_AST_CHECK_H
#define GAB_AST_CHECK_H

#include "ast/ast.h"
#include "ast/facts.h"
#include "ast/pending.h"
#include "diagnostics.h"
#include "function_registry.h"
#include "scope.h"
#include "type/type_registry.h"
#include "util/hash_map.h"

typedef struct {
    Module **modules;
    size_t count;
} Visible;

#define declared_hash(key) ((size_t)(key) >> 4)
#define declared_key_equals(key, other) ((key) == (other))

GAB_HASH_MAP(DeclaredSet, declared, const ASTStmt *, bool)

typedef struct StructDecl {
    ASTStmt *stmt;

    Scope *scope;

    Scope *file_scope;

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

    bool is_caller;

    unsigned int loop_depth;
} FuncContext;

typedef struct {
    Scope *scope;

    Scope *declaring;

    const ASTFile *file;

    const Visible *visible;

    FuncContext func;

    const TypeParamBound *param_bounds;
} Env;

typedef struct {
    Arena *arena;

    TypeRegistry *types;
    FunctionRegistry *functions;
    StringPool *strings;

    Diagnostics *diagnostics;
} Global;

typedef struct ResolverState {
    const Global *global;

    Env env;

    Scope *module_scope;

    String *module_name;

    bool declares_intrinsics;

    Facts *facts;
    PendingBodies *work;

    StructDeclList struct_decls;
    StructDeclList resolving;

    DeclaredSet declared;
} ResolverState;

static inline bool resolver_mark_declared(ResolverState *state, const ASTStmt *stmt) {
    if (declared_lookup(&state->declared, stmt)) {
        return true;
    }

    declared_insert(&state->declared, stmt, true);

    return false;
}

static inline const Type *resolver_error_type(const ResolverState *state) {
    return type_registry_error_type(state->global->types);
}

static inline bool is_error_type(const Type *type) { return !type || type_kind(type) == TYPE_ERROR; }

static inline const Type *i32_type(const ResolverState *state) {
    return type_registry_get_primitive(state->global->types, TYPE_I32);
}

const char *type_name(ResolverState *state, const Type *type);

bool is_addressable(ResolverState *state, const ASTExpr *expr);

const Type *receiver_base_type(const Type *type);

bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from);
bool accepts_by_borrowing(const Type *to, const Type *from);
bool reads_as_a_view(TypeRegistry *registry, const Type *to, const Type *from);
bool lends_by_pointer(const Type *to, const Type *from);
bool unsizes_to_a_slice(const Type *to, const Type *from);
const Type *derefs_to(TypeRegistry *registry, const Type *type);

bool borrow_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span);
void adjust_derefs(ResolverState *state, Adjustment *adjustment, const Type *from, unsigned int count);

void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span);

const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span);

const Type *resolve_element_type(ResolverState *state, TypeExpr *expr, Span span, const char *held_as);

bool reject_unsized(ResolverState *state, const Type *type, Span span, const char *held_as);

BoundKind bound_kind_of(const TypeRegistry *registry, StringPool *strings, const TypeExpr *bound);

bool bind_type_param(TypeRegistry *registry, Scope *params, String *name, size_t index, BoundKind kind);

Scope *resolver_qualifier_scope(ResolverState *state, const ASTIdent *qualifier);

Scope *resolver_type_expr_scope(ResolverState *state, const TypeExpr *expr);
String *resolver_type_expr_member(ResolverState *state, const TypeExpr *expr);
Symbol *resolver_resolve_name(ResolverState *state, Scope *scope, String *name);
const KnownNames *resolver_names(ResolverState *state);
bool reject_self_as_name(ResolverState *state, String *name, Span span);

StructDecl *element_completes_a_cycle(ResolverState *state, const Type *type);
void report_containment_cycle(ResolverState *state, StructDecl *closes_on, Span span);

#endif
