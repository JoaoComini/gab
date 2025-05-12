#ifndef RULE_AST_H
#define RULE_AST_H

#include "lexer.h"

typedef enum {
    NODE_NUMBER,
    NODE_BIN_OP,
    NODE_VARIABLE,
} NodeType;

typedef struct ASTNode {
    NodeType type;
    union {
        double number; // Numeric nodes
        char *name;    // Variable nodes
        struct {       // Binary operator nodes
            struct ASTNode *left;
            struct ASTNode *right;
            TokenType op;
        };
    };
} ASTNode;

ASTNode *new_ast_number_node(double value);
ASTNode *new_ast_bin_op_node(ASTNode *left, TokenType op, ASTNode *right);
ASTNode *new_ast_variable_node(char *name);

void ast_free(ASTNode *node);

#endif
