#ifndef AST_H
#define AST_H

typedef enum { NODE_NUMBER, NODE_BINOP } NodeType;

typedef struct ASTNode {
    NodeType type;
    int value; // For NUMBER
    char op;   // For BINOP
    struct ASTNode *left;
    struct ASTNode *right;
} ASTNode;

#endif
