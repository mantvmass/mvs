/*
 * lexer.h - Header for the lexical analyzer (Tokenizer)
 *
 * The lexer reads the source code (a string) and converts it into Tokens one at a time.
 * The parser calls lexer_next() repeatedly to get the next token until it reaches TK_EOF.
 */
#ifndef MVS_LEXER_H
#define MVS_LEXER_H

#include "token.h"

/* Lexer state while running */
typedef struct {
    const char *src;       /* pointer to the entire source code */
    size_t      pos;       /* current position in the source (character index) */
    int         line;      /* current line (starts at 1) */
    int         col;       /* current column (starts at 1) */
    const char *filename;  /* file name, used for error reporting */
    int         had_error; /* flag: whether an error was found during tokenizing */
} Lexer;

/* Initialize the lexer with source code and a file name */
void  lexer_init(Lexer *lx, const char *src, const char *filename);

/* Get the next token. The caller is responsible for free(token.lexeme) when done */
Token lexer_next(Lexer *lx);

#endif /* MVS_LEXER_H */
