#include "ast.h"

ASTNode *new_ast_node() { return malloc(sizeof(ASTNode)); }

ASTNode *new_ast_number_node(double value) {
    ASTNode *node = new_ast_node();
    node->type = NODE_NUMBER;
    node->number = value;
    return node;
}

ASTNode *new_ast_bin_op_node(ASTNode *left, TokenType op, ASTNode *right) {
    ASTNode *node = new_ast_node();
    node->type = NODE_BIN_OP;
    node->left = left;
    node->right = right;
    node->op = op;
    return node;
}

ASTNode *new_ast_variable_node(char *name) {
    ASTNode *node = new_ast_node();
    node->type = NODE_VARIABLE;
    node->name = name;
    return node;
}

void ast_free(ASTNode *node) {
    if (!node)
        return;

    switch (node->type) {
    case NODE_BIN_OP:
        ast_free(node->left);
        ast_free(node->right);
        break;
    case NODE_VARIABLE:
        free(node->name);
        break;
    case NODE_NUMBER:
        break;
    }

    free(node);
}
