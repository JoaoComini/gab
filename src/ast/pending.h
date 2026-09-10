#ifndef GAB_AST_PENDING_H
#define GAB_AST_PENDING_H

#include "ast/ast.h"
#include "decl.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "type/type_registry.h"
#include "util/list.h"

/* A body resolution gathered, with what it needs to lower and to answer for its moves and borrows. */
typedef struct {
    ASTStmt *body;

    /* Where each parameter's name is, which is what its binding is keyed on. */
    const ASTFieldList *param_fields;

    Function *function;
} PendingBody;

GAB_LIST(PendingBodyList, pending_body_list, PendingBody)

/* The instances whose bodies are their declarations', substituted. Which declaration that is comes from
 * an instance's own id, never from the record a call reached it through: that record may already be
 * specialized on an owner's arguments, and so is not the template the body was lowered under. */
GAB_LIST(InstantiationList, instantiation_list, Function *)

/* What resolution leaves for generation: the bodies to lower and the instances their calls named. */
typedef struct {
    PendingBodyList bodies;
    InstantiationList instances;

    bool instantiation_overflowed;
} PendingBodies;

PendingBodies pending_bodies_create(Arena *arena);

/* Records that an instance is wanted; its body is the declaration's, substituted after lowering. */
void pending_bodies_instantiate(PendingBodies *work, Function *method, Diagnostics *diagnostics);

#endif
