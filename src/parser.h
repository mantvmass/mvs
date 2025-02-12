#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

typedef struct ASTNode {
    TokenType type;
    char value[MAX_TOKEN_LENGTH];
    struct ASTNode *left;
    struct ASTNode *right;
} ASTNode;

ASTNode *new_node(TokenType type, char *value);
ASTNode *parse_expression(int *pos);
ASTNode *parse_function(int *pos);
ASTNode *parse_struct_definition(int *pos);
ASTNode *parse_variable_declaration(int *pos);
ASTNode *parse_array_definition(int *pos);
void print_ast(ASTNode *node);

#endif // PARSER_H
