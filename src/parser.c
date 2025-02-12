#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void skip_comments(int *pos) {
    while (tokens[*pos].type == TOKEN_COMMENT) {
        printf("📝 [PARSER] Skipping comment: %s\n", tokens[*pos].value);
        (*pos)++;
    }
}

ASTNode *new_node(TokenType type, char *value) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = type;
    strcpy(node->value, value);
    node->left = node->right = NULL;
    return node;
}

ASTNode *parse_array_definition(int *pos) {
    if (tokens[*pos].type == TOKEN_IDENTIFIER && strcmp(tokens[*pos].value, "array") == 0) {
        printf("✅ [PARSER] Parsing array definition...\n");
        (*pos)++;

        if (tokens[*pos].type == TOKEN_OPERATOR && strcmp(tokens[*pos].value, "<") == 0) {
            (*pos)++;

            if (tokens[*pos].type == TOKEN_TYPE) {
                printf("   🔹 [PARSER] Array Type: %s\n", tokens[*pos].value);
                ASTNode *array_node = new_node(TOKEN_TYPE, tokens[*pos].value);
                (*pos)++;

                if (tokens[*pos].type == TOKEN_OPERATOR && strcmp(tokens[*pos].value, ">") == 0) {
                    (*pos)++;
                    return array_node;
                } else {
                    printf("❌ [ERROR] Expected '>' after array type\n");
                    return NULL;
                }
            } else {
                printf("❌ [ERROR] Expected type inside array<>\n");
                return NULL;
            }
        } else {
            printf("❌ [ERROR] Expected '<' after 'array'\n");
            return NULL;
        }
    }
    return NULL;
}

ASTNode *parse_struct_definition(int *pos) {
    if (tokens[*pos].type == TOKEN_STRUCT) {
        printf("✅ [PARSER] Defining struct: %s\n", tokens[*pos + 1].value);
        (*pos)++;

        if (tokens[*pos].type == TOKEN_IDENTIFIER) {
            ASTNode *struct_node = new_node(TOKEN_STRUCT, tokens[*pos].value);
            (*pos)++;

            if (tokens[*pos].type == TOKEN_LBRACE) {
                (*pos)++;
                printf("✅ [PARSER] Parsing struct fields...\n");

                while (tokens[*pos].type == TOKEN_IDENTIFIER) {
                    printf("   🔹 [PARSER] Struct Field: %s\n", tokens[*pos].value);
                    char field_name[MAX_TOKEN_LENGTH];
                    strcpy(field_name, tokens[*pos].value);
                    (*pos)++;

                    if (tokens[*pos].type == TOKEN_COLON) {
                        (*pos)++;
                        ASTNode *type_node = NULL;

                        if (tokens[*pos].type == TOKEN_TYPE) {
                            printf("      🔹 [PARSER] Field Type: %s\n", tokens[*pos].value);
                            type_node = new_node(TOKEN_TYPE, tokens[*pos].value);
                            (*pos)++;
                        }

                        if (tokens[*pos].type == TOKEN_COMMA) {  // ✅ รองรับ `,` แทน `;`
                            (*pos)++;
                        }
                    }
                }

                if (tokens[*pos].type == TOKEN_RBRACE) {
                    (*pos)++;
                    printf("✅ [PARSER] Struct definition complete\n");
                    return struct_node;
                } else {
                    printf("❌ [ERROR] Expected '}' at the end of struct definition\n");
                    return NULL;
                }
            } else {
                printf("❌ [ERROR] Expected '{' after struct name\n");
                return NULL;
            }
        }
    }
    return NULL;
}

ASTNode *parse_variable_declaration(int *pos) {
    if (tokens[*pos].type == TOKEN_VAR) {
        (*pos)++;

        if (tokens[*pos].type == TOKEN_IDENTIFIER) {
            ASTNode *var_node = new_node(TOKEN_VAR, tokens[*pos].value);
            (*pos)++;

            if (tokens[*pos].type == TOKEN_COLON) {
                (*pos)++;

                ASTNode *type_node = NULL;
                if (tokens[*pos].type == TOKEN_TYPE) {
                    printf("✅ [PARSER] Declaring variable %s of type %s\n", var_node->value, tokens[*pos].value);
                    type_node = new_node(TOKEN_TYPE, tokens[*pos].value);
                    (*pos)++;
                } else if (tokens[*pos].type == TOKEN_IDENTIFIER && strcmp(tokens[*pos].value, "array") == 0) {
                    type_node = parse_array_definition(pos);
                    if (!type_node) return NULL;
                } else {
                    printf("❌ [ERROR] Unexpected type for variable: %s\n", tokens[*pos].value);
                    return NULL;
                }

                if (tokens[*pos].type == TOKEN_ASSIGN) {
                    (*pos)++;
                    printf("✅ [PARSER] Assigning value: %s\n", tokens[*pos].value);
                    (*pos)++;
                }

                if (tokens[*pos].type == TOKEN_SEMICOLON) {
                    (*pos)++;
                    return var_node;
                } else {
                    printf("❌ [ERROR] Expected ';' after variable declaration\n");
                    return NULL;
                }
            }
        }
    }
    return NULL;
}

ASTNode *parse_function(int *pos) {
    if (tokens[*pos].type == TOKEN_FUNC) {
        printf("🔍 [PARSER] Parsing function...\n");
        (*pos)++;

        if (tokens[*pos].type == TOKEN_IDENTIFIER) {
            printf("✅ [PARSER] Function name: %s\n", tokens[*pos].value);
            ASTNode *func_node = new_node(TOKEN_FUNC, tokens[*pos].value);
            (*pos)++;

            if (tokens[*pos].type == TOKEN_LPAREN) {
                (*pos)++;
                if (tokens[*pos].type == TOKEN_RPAREN) {
                    printf("✅ [PARSER] Function parameters empty\n");
                    (*pos)++;
                } else {
                    printf("❌ [ERROR] Expected ')' after function parameters\n");
                    return NULL;
                }
            } else {
                printf("❌ [ERROR] Expected '(' after function name\n");
                return NULL;
            }

            if (tokens[*pos].type == TOKEN_ARROW) {
                (*pos)++;
                if (tokens[*pos].type == TOKEN_TYPE) {
                    printf("✅ [PARSER] Function return type: %s\n", tokens[*pos].value);
                    (*pos)++;
                } else {
                    printf("❌ [ERROR] Expected return type after '->'\n");
                    return NULL;
                }
            } else {
                printf("❌ [ERROR] Expected '->' after function parameters\n");
                return NULL;
            }

            if (tokens[*pos].type == TOKEN_LBRACE) {
                (*pos)++;
                printf("✅ [PARSER] Function body found '{'\n");

                ASTNode *current = func_node;
                while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
                    skip_comments(pos);

                    if (tokens[*pos].type == TOKEN_STRUCT) {
                        ASTNode *struct_def = parse_struct_definition(pos);
                        if (struct_def) {
                            current->right = struct_def;
                            current = struct_def;
                        } else {
                            return NULL;
                        }
                    } else {
                        printf("❌ [ERROR] Unexpected token: %s\n", tokens[*pos].value);
                        return NULL;
                    }
                }

                if (tokens[*pos].type == TOKEN_RBRACE) {
                    (*pos)++;
                    printf("✅ [PARSER] Function parsing completed\n");
                    return func_node;
                } else {
                    printf("❌ [ERROR] Expected '}' at the end of function body\n");
                    return NULL;
                }
            } else {
                printf("❌ [ERROR] Expected '{' after function signature\n");
                return NULL;
            }
        } else {
            printf("❌ [ERROR] Expected function name after 'func'\n");
            return NULL;
        }
    }
    return NULL;
}

ASTNode *parse_expression(int *pos) {
    printf("🔍 [PARSER] Parsing token: %s\n", tokens[*pos].value);

    if (tokens[*pos].type == TOKEN_FUNC) {
        return parse_function(pos);
    }

    if (tokens[*pos].type == TOKEN_STRUCT) {
        return parse_struct_definition(pos);
    }

    if (tokens[*pos].type == TOKEN_VAR) {
        return parse_variable_declaration(pos);
    }

    return NULL;
}
