/*
 * parser.h - Header for the syntax analyzer (Parser)
 *
 * The parser reads tokens from the lexer and assembles them into an AST following
 * the MVS grammar. It uses recursive descent (one function per grammar rule).
 */
#ifndef MVS_PARSER_H
#define MVS_PARSER_H

#include "lexer.h"
#include "ast.h"

/* Parser state */
typedef struct {
    Lexer *lx;          /* the lexer in use */
    Token  cur;         /* current token under consideration */
    int    had_error;   /* flag: whether a syntax error was found */
    int    nerrors;     /* number of syntax errors reported so far */
    int    panic;       /* 1 = inside a bad region; suppress errors until synchronize() */
    int    fatal;       /* 1 = too many errors, stop parsing */
    int    depth;       /* current recursion depth (guards against stack overflow from deeply nested input) */
} Parser;

/* Parse a whole source file into an AST (ND_PROGRAM node). Returns NULL on a fatal error */
Node *parse_program(const char *src, const char *filename, int *had_error);

#endif /* MVS_PARSER_H */
