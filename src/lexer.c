/*
 * lexer.c - Tokenizer of the MVS language, fully hand-written, no flex
 *
 * Approach: walk through the source code character by character and group characters into tokens.
 * - Skip whitespace and comments
 * - Read numbers / strings / characters / names (identifier or keyword)
 * - Read operators and punctuation (supports two-character operators such as ==, ->, ++)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "lexer.h"

/* Table mapping keywords to token kinds, looked up after an identifier is read */
typedef struct { const char *word; TokenType type; } Keyword;
static const Keyword KEYWORDS[] = {
    {"let", TK_LET}, {"const", TK_CONST}, {"func", TK_FUNC}, {"return", TK_RETURN},
    {"struct", TK_STRUCT}, {"if", TK_IF}, {"elseif", TK_ELSEIF}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"do", TK_DO}, {"for", TK_FOR}, {"switch", TK_SWITCH},
    {"case", TK_CASE}, {"default", TK_DEFAULT}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"import", TK_IMPORT}, {"from", TK_FROM},
    {"extern", TK_EXTERN}, {"export", TK_EXPORT}, {"impl", TK_IMPL}, {"trait", TK_TRAIT},
    {"dyn", TK_DYN}, {"where", TK_WHERE},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"as", TK_AS},
    /* Data types */
    {"i8", TK_TYPE_I8}, {"i16", TK_TYPE_I16}, {"i32", TK_TYPE_I32}, {"i64", TK_TYPE_I64},
    {"i128", TK_TYPE_I128}, {"isize", TK_TYPE_ISIZE},
    {"u8", TK_TYPE_U8}, {"u16", TK_TYPE_U16}, {"u32", TK_TYPE_U32}, {"u64", TK_TYPE_U64},
    {"u128", TK_TYPE_U128}, {"usize", TK_TYPE_USIZE},
    {"bool", TK_TYPE_BOOL}, {"void", TK_TYPE_VOID}, {"str", TK_TYPE_STR},
    {"char", TK_TYPE_CHAR}, {"f32", TK_TYPE_F32}, {"f64", TK_TYPE_F64},
    {NULL, TK_EOF}
};

/* Initialize lexer state */
void lexer_init(Lexer *lx, const char *src, const char *filename) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
    lx->filename = filename;
    lx->had_error = 0;
}

/* Look at the current character without moving the position */
static char peek(Lexer *lx) { return lx->src[lx->pos]; }

/* Look at the next character (1-character lookahead) */
static char peek_next(Lexer *lx) {
    if (lx->src[lx->pos] == '\0') return '\0';
    return lx->src[lx->pos + 1];
}

/* Consume the current character and move the position, updating line/column */
static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else { lx->col++; }
    return c;
}

/* Build a token, copying the lexeme from the source range [start, end) */
static Token make_token(Lexer *lx, TokenType type, size_t start, size_t end, int line, int col) {
    Token t;
    t.type = type;
    size_t len = end - start;
    t.lexeme = (char *)malloc(len + 1);
    memcpy(t.lexeme, lx->src + start, len);
    t.lexeme[len] = '\0';
    t.line = line;
    t.col = col;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}

/* Build a simple token (operator/punctuation) with a fixed lexeme */
static Token simple_token(TokenType type, const char *lex, int line, int col) {
    Token t;
    t.type = type;
    t.lexeme = strdup(lex);
    t.line = line;
    t.col = col;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}

/* Skip whitespace and both single-line and multi-line comments */
static void skip_trivia(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '/' && peek_next(lx) == '/') {
            /* Single-line comment: skip until end of line */
            while (peek(lx) != '\n' && peek(lx) != '\0') advance(lx);
        } else if (c == '/' && peek_next(lx) == '*') {
            /* Multi-line comment: skip until *\/ is found */
            advance(lx); advance(lx);
            while (!(peek(lx) == '*' && peek_next(lx) == '/') && peek(lx) != '\0') advance(lx);
            if (peek(lx) != '\0') { advance(lx); advance(lx); } /* consume *\/ */
        } else {
            break;
        }
    }
}

/* Read an identifier or keyword: starts with a letter/_ followed by letters/digits/_ */
static Token read_identifier(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    /* Limit identifier length to keep label/signature buffers (built from names) from overflowing
     * (see func_label_of etc.) */
    if (lx->pos - start > 100) {
        lx->had_error = 1;
        fprintf(stderr, "%s:%d: error: identifier too long (max 100 characters)\n", lx->filename, line);
    }
    Token t = make_token(lx, TK_IDENT, start, lx->pos, line, col);
    /* Check whether it matches a keyword; if so, change the token kind */
    for (int i = 0; KEYWORDS[i].word != NULL; i++) {
        if (strcmp(t.lexeme, KEYWORDS[i].word) == 0) {
            t.type = KEYWORDS[i].type;
            break;
        }
    }
    return t;
}

/* Read a number: integer or float (contains a dot) */
static Token read_number(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    int is_float = 0;

    /* hex (0x1F) and binary (0b1010) literals */
    if (peek(lx) == '0' && (peek_next(lx) == 'x' || peek_next(lx) == 'X' ||
                            peek_next(lx) == 'b' || peek_next(lx) == 'B')) {
        int base = (peek_next(lx) == 'x' || peek_next(lx) == 'X') ? 16 : 2;
        advance(lx); advance(lx);   /* consume "0x" / "0b" */
        size_t dstart = lx->pos;
        if (base == 16) while (isxdigit((unsigned char)peek(lx))) advance(lx);
        else            while (peek(lx) == '0' || peek(lx) == '1') advance(lx);
        Token t = make_token(lx, TK_INT, start, lx->pos, line, col);
        if (lx->pos == dstart) {
            lx->had_error = 1;
            fprintf(stderr, "%s:%d:%d: error: %s literal needs at least one digit after '%s'\n",
                    lx->filename, line, col, base == 16 ? "hex" : "binary", base == 16 ? "0x" : "0b");
            return t;
        }
        errno = 0;
        t.int_val = (long long)strtoull(t.lexeme + 2, NULL, base);
        if (errno == ERANGE) {
            lx->had_error = 1;
            fprintf(stderr, "%s:%d:%d: error: integer literal '%s' does not fit in 64 bits\n",
                    lx->filename, line, col, t.lexeme);
            fprintf(stderr, "help: build 128-bit values with arithmetic, e.g. (1 as i128) << 100\n");
        }
        return t;
    }

    while (isdigit((unsigned char)peek(lx))) advance(lx);
    /* Detect a decimal point; it must be followed by a digit so it does not clash with
     * member access (a.b) */
    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        is_float = 1;
        advance(lx); /* consume the dot */
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }
    Token t = make_token(lx, is_float ? TK_FLOAT : TK_INT, start, lx->pos, line, col);
    if (is_float) t.float_val = atof(t.lexeme);
    else {
        /* Use strtoull to support unsigned integers across the full 64-bit range (e.g. u64 > 2^63).
         * Stored as a bit-pattern in int_val (signed); the bits stay correct when moved into rax.
         * A literal past 2^64-1 would silently saturate, so it is a hard error instead. */
        errno = 0;
        t.int_val = (long long)strtoull(t.lexeme, NULL, 10);
        if (errno == ERANGE) {
            lx->had_error = 1;
            fprintf(stderr, "%s:%d:%d: error: integer literal '%s' does not fit in 64 bits\n",
                    lx->filename, line, col, t.lexeme);
            fprintf(stderr, "help: build 128-bit values with arithmetic, e.g. (1 as i128) << 100\n");
        }
    }
    return t;
}

/* Read a string in "..." quotes; supports basic escapes (\n \t \" \\ \0) */
static Token read_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* consume opening " */
    /* Store the unescaped value into a new buffer */
    char *buf = (char *)malloc(strlen(lx->src + lx->pos) + 1);
    size_t bi = 0;
    while (peek(lx) != '"' && peek(lx) != '\0') {
        char c = advance(lx);
        if (c == '\\') { /* handle escape sequence */
            if (peek(lx) == '\0') break; /* '\' at end of file: avoid reading past the buffer */
            char e = advance(lx);
            switch (e) {
                case 'n': buf[bi++] = '\n'; break;
                case 't': buf[bi++] = '\t'; break;
                case 'r': buf[bi++] = '\r'; break;
                case '0': buf[bi++] = '\0'; break;
                case '"': buf[bi++] = '"';  break;
                case '\\': buf[bi++] = '\\'; break;
                default:  buf[bi++] = e;    break;
            }
        } else {
            buf[bi++] = c;
        }
    }
    if (peek(lx) == '"') advance(lx); /* consume closing " */
    else { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated string\n", lx->filename, line); }
    buf[bi] = '\0';
    Token t;
    t.type = TK_STRING;
    t.lexeme = buf; /* lexeme holds the unescaped value (without the quote marks) */
    t.line = line; t.col = col;
    t.int_val = (long long)bi; /* store the real length in int_val in case of embedded \0 */
    t.float_val = 0.0;
    return t;
}

/* Read a single character in '...' quotes */
static Token read_char(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* consume opening ' */
    long long val = 0;
    if (peek(lx) == '\0') { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated char\n", lx->filename, line); return simple_token(TK_CHAR, "char", line, col); }
    char c = advance(lx);
    if (c == '\\') { /* escape such as '\n' */
        if (peek(lx) == '\0') { lx->had_error = 1; return simple_token(TK_CHAR, "char", line, col); } /* '\' at end of file */
        char e = advance(lx);
        switch (e) {
            case 'n': val = '\n'; break;
            case 't': val = '\t'; break;
            case 'r': val = '\r'; break;
            case '0': val = '\0'; break;
            case '\\': val = '\\'; break;
            case '\'': val = '\''; break;
            default:  val = e;    break;
        }
    } else {
        val = (unsigned char)c;
    }
    if (peek(lx) == '\'') advance(lx); /* consume closing ' */
    else { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated char\n", lx->filename, line); }
    Token t = simple_token(TK_CHAR, "char", line, col);
    t.int_val = val;
    return t;
}

/* Get the next token - the lexer's main function */
Token lexer_next(Lexer *lx) {
    skip_trivia(lx);

    int line = lx->line, col = lx->col;
    char c = peek(lx);

    /* End of file */
    if (c == '\0') return simple_token(TK_EOF, "<eof>", line, col);

    /* identifier / keyword */
    if (isalpha((unsigned char)c) || c == '_') return read_identifier(lx);
    /* number */
    if (isdigit((unsigned char)c)) return read_number(lx);
    /* string */
    if (c == '"') return read_string(lx);
    /* character */
    if (c == '\'') return read_char(lx);

    /* Operators and punctuation: always try two-character forms first */
    char n = peek_next(lx);
    switch (c) {
        case '+':
            if (n == '+') { advance(lx); advance(lx); return simple_token(TK_PLUSPLUS, "++", line, col); }
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_PLUS_ASSIGN, "+=", line, col); }
            advance(lx); return simple_token(TK_PLUS, "+", line, col);
        case '-':
            if (n == '-') { advance(lx); advance(lx); return simple_token(TK_MINUSMINUS, "--", line, col); }
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_MINUS_ASSIGN, "-=", line, col); }
            if (n == '>') { advance(lx); advance(lx); return simple_token(TK_ARROW, "->", line, col); }
            advance(lx); return simple_token(TK_MINUS, "-", line, col);
        case '*':
            if (n == '*') { advance(lx); advance(lx); return simple_token(TK_STARSTAR, "**", line, col); } /* exponentiation */
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_STAR_ASSIGN, "*=", line, col); }
            advance(lx); return simple_token(TK_STAR, "*", line, col);
        case '/':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_SLASH_ASSIGN, "/=", line, col); }
            advance(lx); return simple_token(TK_SLASH, "/", line, col);
        case '%': advance(lx); return simple_token(TK_PERCENT, "%", line, col);
        case '^': advance(lx); return simple_token(TK_CARET, "^", line, col);
        case '=':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_EQ, "==", line, col); }
            advance(lx); return simple_token(TK_ASSIGN, "=", line, col);
        case '!':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_NEQ, "!=", line, col); }
            advance(lx); return simple_token(TK_NOT, "!", line, col);
        case '<':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_LE, "<=", line, col); }
            if (n == '<') { advance(lx); advance(lx); return simple_token(TK_SHL, "<<", line, col); }
            advance(lx); return simple_token(TK_LT, "<", line, col);
        case '>':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_GE, ">=", line, col); }
            if (n == '>') { advance(lx); advance(lx); return simple_token(TK_SHR, ">>", line, col); }
            advance(lx); return simple_token(TK_GT, ">", line, col);
        case '&':
            if (n == '&') { advance(lx); advance(lx); return simple_token(TK_AND, "&&", line, col); }
            advance(lx); return simple_token(TK_AMP, "&", line, col);
        case '~': advance(lx); return simple_token(TK_TILDE, "~", line, col);
        case '|':
            if (n == '|') { advance(lx); advance(lx); return simple_token(TK_OR, "||", line, col); }
            advance(lx); return simple_token(TK_PIPE, "|", line, col); /* bitwise OR */
        case '(': advance(lx); return simple_token(TK_LPAREN, "(", line, col);
        case ')': advance(lx); return simple_token(TK_RPAREN, ")", line, col);
        case '{': advance(lx); return simple_token(TK_LBRACE, "{", line, col);
        case '}': advance(lx); return simple_token(TK_RBRACE, "}", line, col);
        case '[': advance(lx); return simple_token(TK_LBRACKET, "[", line, col);
        case ']': advance(lx); return simple_token(TK_RBRACKET, "]", line, col);
        case ';': advance(lx); return simple_token(TK_SEMICOLON, ";", line, col);
        case ':':
            advance(lx);
            if (peek(lx) == ':') { advance(lx); return simple_token(TK_COLONCOLON, "::", line, col); } /* :: associated function call */
            return simple_token(TK_COLON, ":", line, col);
        case ',': advance(lx); return simple_token(TK_COMMA, ",", line, col);
        case '@': advance(lx); return simple_token(TK_AT, "@", line, col);
        case '.':
            advance(lx);
            if (peek(lx) == '.' && peek_next(lx) == '.') {   /* '...' marks a variadic parameter */
                advance(lx); advance(lx);
                return simple_token(TK_ELLIPSIS, "...", line, col);
            }
            return simple_token(TK_DOT, ".", line, col);
    }

    /* Unknown character */
    lx->had_error = 1;
    fprintf(stderr, "%s:%d:%d: error: unexpected character '%c'\n", lx->filename, line, col, c);
    advance(lx);
    return simple_token(TK_ERROR, "?", line, col);
}
