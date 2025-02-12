#include "lexer.h"
#include <locale.h>

Token tokens[MAX_TOKENS];
int token_count = 0;

void add_token(TokenType type, const char *value) {
    tokens[token_count].type = type;
    strncpy(tokens[token_count].value, value, MAX_TOKEN_LENGTH - 1);
    tokens[token_count].value[MAX_TOKEN_LENGTH - 1] = '\0';
    token_count++;
}

void lex(const char *source) {
    setlocale(LC_ALL, "en_US.UTF-8");  // รองรับ UTF-8
    printf("🔍 [LEXER] Tokenizing Source Code...\n");
    token_count = 0;

    while (*source) {
        if (isspace(*source)) {
            source++;
            continue;
        }

        // ✅ Skip Comments (`//`)
        if (*source == '/' && *(source + 1) == '/') {
            while (*source != '\n' && *source != '\0') {
                source++;
            }
            continue;
        }

        // ✅ Parse Numbers (int, float)
        if (isdigit(*source)) {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0, is_float = 0;

            while (isdigit(*source) || *source == '.') {
                if (*source == '.') {
                    if (is_float) break;
                    is_float = 1;
                }
                buffer[i++] = *source++;
            }
            buffer[i] = '\0';
            add_token(is_float ? TOKEN_FLOAT : TOKEN_NUMBER, buffer);
            printf("[LEXER] %s: %s\n", is_float ? "FLOAT" : "NUMBER", buffer);
            continue;
        }

        // ✅ Parse Identifiers (var, struct, func, type names)
        if (isalpha(*source) || *source == '_') {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0;
            while (isalnum(*source) || *source == '_') {
                buffer[i++] = *source++;
            }
            buffer[i] = '\0';

            if (strcmp(buffer, "var") == 0) add_token(TOKEN_VAR, buffer);
            else if (strcmp(buffer, "struct") == 0) add_token(TOKEN_STRUCT, buffer);
            else if (strcmp(buffer, "func") == 0) add_token(TOKEN_FUNC, buffer);
            else if (strcmp(buffer, "return") == 0) add_token(TOKEN_RETURN, buffer);
            else if (strcmp(buffer, "int") == 0 || strcmp(buffer, "float") == 0 ||
                     strcmp(buffer, "string") == 0 || strcmp(buffer, "bool") == 0 ||
                     strcmp(buffer, "Array") == 0) {
                add_token(TOKEN_TYPE, buffer);
            } else {
                add_token(TOKEN_IDENTIFIER, buffer);
            }
            printf("[LEXER] IDENTIFIER: %s\n", buffer);
            continue;
        }

        // ✅ Special Characters
        if (*source == '(') { add_token(TOKEN_LPAREN, "("); }
        else if (*source == ')') { add_token(TOKEN_RPAREN, ")"); }
        else if (*source == '{') { add_token(TOKEN_LBRACE, "{"); }
        else if (*source == '}') { add_token(TOKEN_RBRACE, "}"); }
        else if (*source == '[') { add_token(TOKEN_LBRACKET, "["); }
        else if (*source == ']') { add_token(TOKEN_RBRACKET, "]"); }
        else if (*source == ':') { add_token(TOKEN_COLON, ":"); }
        else if (*source == '=') { add_token(TOKEN_ASSIGN, "="); }
        else if (*source == ',') { add_token(TOKEN_COMMA, ","); }
        else if (*source == ';') { add_token(TOKEN_SEMICOLON, ";"); }
        else if (*source == '-' && *(source + 1) == '>') {
            add_token(TOKEN_ARROW, "->");
            source++;
        }
        else if (*source == '"') {
            char buffer[MAX_TOKEN_LENGTH];
            int i = 0;
            source++;  // Skip `"`

            while (*source && *source != '"') {
                buffer[i++] = *source++;
            }
            buffer[i] = '\0';
            source++;  // Skip closing `"`

            add_token(TOKEN_STRING, buffer);
            printf("[LEXER] STRING: \"%s\"\n", buffer);
        }
        else {
            char unknown[2] = {*source, '\0'};
            add_token(TOKEN_OPERATOR, unknown);
        }

        source++;
    }
    
    add_token(TOKEN_EOF, "EOF");
}
