#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define MAX_TOKENS 1024
#define MAX_TOKEN_LENGTH 64

typedef enum {
    TOKEN_INT, TOKEN_FLOAT, TOKEN_STRING, TOKEN_BOOL,
    TOKEN_IDENTIFIER, TOKEN_FUNC, TOKEN_STRUCT, TOKEN_IMPL,
    TOKEN_IF, TOKEN_ELSE, TOKEN_FOR, TOKEN_WHILE, TOKEN_MATCH,
    TOKEN_RETURN, TOKEN_VAR, TOKEN_OPERATOR, TOKEN_NUMBER,
    TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_COMMA, TOKEN_ASSIGN, TOKEN_EOF,
    TOKEN_ARROW, TOKEN_TYPE, TOKEN_COMMENT, TOKEN_LBRACKET, TOKEN_RBRACKET
} TokenType;

typedef struct {
    TokenType type;
    char value[MAX_TOKEN_LENGTH];
} Token;

extern Token tokens[MAX_TOKENS]; // ใช้ extern เพื่อป้องกัน duplicate definition
extern int token_count; // ใช้ extern เช่นกัน

void lex(const char *source);
void add_token(TokenType type, const char *value);

#endif // LEXER_H
