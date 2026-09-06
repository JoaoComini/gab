#ifndef GAB_AST_PENDING_H
#define GAB_AST_PENDING_H

#include "memory/arena.h"
#include "ast/ast.h"
#include "binding.h"
#include "diagnostics.h"
#include "type/type_registry.h"
#include "util/list.h"

/* A body resolution gathered, with what it needs to lower and to answer for its moves and borrows. */
typedef struct {
    TypeRegistry *registry;
    ASTStmt *body;
    const ASTFieldList *param_fields;
    Function *function;
} PendingBody;

GAB_LIST(PendingBodyList, pending_body_list, PendingBody)

/* An instance and the declaration whose lowered body it substitutes. */
typedef struct {
    Function *generic;
    Function *instance;
} Instantiation;

GAB_LIST(InstantiationList, instantiation_list, Instantiation)

/* What resolution leaves for generation: the bodies to lower and the instances their calls named. */
typedef struct {
    PendingBodyList bodies;
    InstantiationList instances;

    bool instantiation_overflowed;
} PendingBodies;

PendingBodies pending_bodies_create(Arena *arena);

/* Records that an instance is wanted; its body is the declaration's, substituted after lowering. */
void pending_bodies_instantiate(PendingBodies *work, Function *generic, Function *method,
                                Diagnostics *diagnostics);

#endif
