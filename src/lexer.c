#include "lexer.h"
#include <locale.h>

Token tokens[MAX_TOKENS];
int token_count = 0;

void add_token(TokenType type, const char *value) {
    if (token_count >= MAX_TOKENS) {
        printf("❌ [LEXER] จำนวนโทเค็นเกินขีดจำกัด (%d)\n", MAX_TOKENS);
        exit(1);
    }
    tokens[token_count].type = type;
    strncpy(tokens[token_count].value, value, MAX_TOKEN_LENGTH - 1);
    tokens[token_count].value[MAX_TOKEN_LENGTH - 1] = '\0';
    token_count++;
    printf("[LEXER] %s: %s\n", type == TOKEN_IDENTIFIER ? "IDENTIFIER" : 
                            type == TOKEN_STRING ? "STRING" : 
                            type == TOKEN_NUMBER ? "NUMBER" : "SYMBOL", value);
}

void lex(const char *source) {
    setlocale(LC_ALL, "en_US.UTF-8");
    printf("🔍 [LEXER] แยกโทเค็นจากซอร์สโค้ด...\n");
    token_count = 0;

    while (*source) {
        if (isspace(*source)) { source++; continue; }

        if (*source == '/' && *(source + 1) == '/') {
            add_token(TOKEN_COMMENT, "//");
            while (*source != '\n' && *source != '\0') source++;
            continue;
        }

        if (isdigit(*source)) {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0;
            while (isdigit(*source) || *source == '.') buffer[i++] = *source++;
            buffer[i] = '\0';
            add_token(TOKEN_NUMBER, buffer);
            continue;
        }

        if (*source == '"') {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0;
            source++;
            while (*source && *source != '"') buffer[i++] = *source++;
            buffer[i] = '\0';
            source++;
            add_token(TOKEN_STRING, buffer);
            continue;
        }

        if (isalpha(*source) || *source == '_') {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0;
            while (isalnum(*source) || *source == '_') buffer[i++] = *source++;
            buffer[i] = '\0';

            if (strcmp(buffer, "var") == 0) add_token(TOKEN_VAR, buffer);
            else if (strcmp(buffer, "func") == 0) add_token(TOKEN_FUNC, buffer);
            else if (strcmp(buffer, "struct") == 0) add_token(TOKEN_STRUCT, buffer);
            else if (strcmp(buffer, "return") == 0) add_token(TOKEN_RETURN, buffer);
            else if (strcmp(buffer, "if") == 0) add_token(TOKEN_IF, buffer);
            else if (strcmp(buffer, "else") == 0) add_token(TOKEN_ELSE, buffer);
            else if (strcmp(buffer, "for") == 0) add_token(TOKEN_FOR, buffer);
            else if (strcmp(buffer, "while") == 0) add_token(TOKEN_WHILE, buffer);
            else if (strcmp(buffer, "break") == 0) add_token(TOKEN_BREAK, buffer);
            else if (strcmp(buffer, "continue") == 0) add_token(TOKEN_CONTINUE, buffer);
            else if (strcmp(buffer, "true") == 0 || strcmp(buffer, "false") == 0) add_token(TOKEN_BOOL, buffer);
            else if (strcmp(buffer, "null") == 0) add_token(TOKEN_NULL, buffer);
            else if (strcmp(buffer, "Some") == 0) add_token(TOKEN_OPTION_SOME, buffer);
            else if (strcmp(buffer, "None") == 0) add_token(TOKEN_OPTION_NONE, buffer);
            else if (strcmp(buffer, "Number") == 0 || strcmp(buffer, "String") == 0 || 
                     strcmp(buffer, "Bool") == 0 || strcmp(buffer, "Array") == 0 || 
                     strcmp(buffer, "Struct") == 0 || strcmp(buffer, "Option") == 0 || 
                     strcmp(buffer, "Void") == 0) add_token(TOKEN_TYPE, buffer);
            else if (strcmp(buffer, "print") == 0) add_token(TOKEN_PRINT, buffer);
            else if (strcmp(buffer, "println") == 0) add_token(TOKEN_PRINTLN, buffer);
            else if (strcmp(buffer, "format") == 0) add_token(TOKEN_FORMAT, buffer);
            else if (strcmp(buffer, "prompt") == 0) add_token(TOKEN_PROMPT, buffer);
            else add_token(TOKEN_IDENTIFIER, buffer);
            continue;
        }

        if (*source == '+') { add_token(TOKEN_PLUS, "+"); source++; }
        else if (*source == '-') { 
            if (*(source + 1) == '>') { add_token(TOKEN_ARROW, "->"); source += 2; } 
            else { add_token(TOKEN_MINUS, "-"); source++; }
        }
        else if (*source == '*') { 
            if (*(source + 1) == '*') { add_token(TOKEN_POW, "**"); source += 2; } 
            else if (isalpha(*(source + 1))) { add_token(TOKEN_POINTER, "*"); source++; } 
            else { add_token(TOKEN_MULTIPLY, "*"); source++; }
        }
        else if (*source == '/') { add_token(TOKEN_DIVIDE, "/"); source++; }
        else if (*source == '=') { 
            if (*(source + 1) == '=') { add_token(TOKEN_EQ, "=="); source += 2; } 
            else { add_token(TOKEN_ASSIGN, "="); source++; }
        }
        else if (*source == '!') { 
            if (*(source + 1) == '=') { add_token(TOKEN_NEQ, "!="); source += 2; } 
        }
        else if (*source == '>') { 
            if (*(source + 1) == '=') { add_token(TOKEN_GTE, ">="); source += 2; } 
            else { add_token(TOKEN_GT, ">"); source++; }
        }
        else if (*source == '<') { 
            if (*(source + 1) == '=') { add_token(TOKEN_LTE, "<="); source += 2; } 
            else { add_token(TOKEN_LT, "<"); source++; }
        }
        else if (*source == '{') { add_token(TOKEN_LBRACE, "{"); source++; }
        else if (*source == '}') { add_token(TOKEN_RBRACE, "}"); source++; }
        else if (*source == '(') { add_token(TOKEN_LPAREN, "("); source++; }
        else if (*source == ')') { add_token(TOKEN_RPAREN, ")"); source++; }
        else if (*source == '[') { add_token(TOKEN_LBRACKET, "["); source++; }
        else if (*source == ']') { add_token(TOKEN_RBRACKET, "]"); source++; }
        else if (*source == ';') { add_token(TOKEN_SEMICOLON, ";"); source++; }
        else if (*source == ':') { add_token(TOKEN_COLON, ":"); source++; }
        else if (*source == ',') { add_token(TOKEN_COMMA, ","); source++; }
        else if (*source == '.') { add_token(TOKEN_DOT, "."); source++; }
        else if (*source == '&') { add_token(TOKEN_ADDRESS, "&"); source++; }
        else source++;
    }
    add_token(TOKEN_EOF, "EOF");
    printf("✅ [LEXER] การแยกโทเค็นเสร็จสมบูรณ์\n");
}