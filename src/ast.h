#ifndef RULE_AST_H
#define RULE_AST_H

typedef enum {
    NODE_NUMBER,
    NODE_BIN_OP,
    NODE_VARIABLE,
} ASTNodeType;

typedef enum {
    BIN_OP_ADD,
    BIN_OP_SUB,
    BIN_OP_MUL,
    BIN_OP_DIV,
} ASTBinOp;

typedef struct ASTNode {
    ASTNodeType type;
    union {
        double number; // Numeric nodes
        char *name;    // Variable nodes
        struct {       // Binary operator nodes
            struct ASTNode *left;
            struct ASTNode *right;
            ASTBinOp op;
        };
    };
} ASTNode;

ASTNode *new_ast_number_node(double value);
ASTNode *new_ast_bin_op_node(ASTNode *left, ASTBinOp op, ASTNode *right);
ASTNode *new_ast_variable_node(char *name);

void ast_free(ASTNode *node);

#endif
