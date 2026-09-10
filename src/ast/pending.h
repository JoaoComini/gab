#ifndef GAB_AST_PENDING_H
#define GAB_AST_PENDING_H

#include "ast/ast.h"
#include "decl.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "type/type_registry.h"
#include "util/list.h"

typedef struct {
    ASTStmt *body;

    const ASTFieldList *param_fields;

    Function *function;
} PendingBody;

GAB_LIST(PendingBodyList, pending_body_list, PendingBody)

GAB_LIST(InstantiationList, instantiation_list, Function *)

typedef struct {
    PendingBodyList bodies;
    InstantiationList instances;

    bool instantiation_overflowed;
} PendingBodies;

bool type_args_are_concrete(const TypeArg *args, size_t count);

PendingBodies pending_bodies_create(Arena *arena);

void pending_bodies_instantiate(PendingBodies *work, Function *method, Diagnostics *diagnostics);

#endif
