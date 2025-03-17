#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Token tokens[MAX_TOKENS];
extern int token_count;

ASTNode *new_node(TokenType type, char *value) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (!node) {
        printf("❌ [ERROR] ไม่สามารถจองหน่วยความจำสำหรับโหนด AST ได้\n");
        exit(1);
    }
    node->type = type;
    strcpy(node->value, value);
    node->left = node->right = node->next = NULL;
    return node;
}

ASTNode *parse_parameters(int *pos) {
    if (tokens[*pos].type != TOKEN_LPAREN) {
        printf("❌ [PARSER] คาดหวัง '(' ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    (*pos)++; // ข้าม (

    ASTNode *params = NULL, *current = NULL;
    while (tokens[*pos].type != TOKEN_RPAREN && tokens[*pos].type != TOKEN_EOF) {
        if (tokens[*pos].type == TOKEN_IDENTIFIER) {
            ASTNode *param = new_node(TOKEN_IDENTIFIER, tokens[*pos].value);
            (*pos)++; // ข้ามชื่อพารามิเตอร์
            if (tokens[*pos].type == TOKEN_COLON) {
                (*pos)++; // ข้าม :
                if (tokens[*pos].type == TOKEN_TYPE || tokens[*pos].type == TOKEN_POINTER) {
                    param->left = new_node(tokens[*pos].type, tokens[*pos].value);
                    (*pos)++;
                } else {
                    printf("❌ [PARSER] คาดหวังชนิดข้อมูลหลัง ':' ที่ตำแหน่ง %d\n", *pos);
                    return NULL;
                }
            }
            if (!params) params = param;
            else current->next = param;
            current = param;
            if (tokens[*pos].type == TOKEN_COMMA) (*pos)++; // ข้าม ,
        } else break;
    }
    if (tokens[*pos].type != TOKEN_RPAREN) {
        printf("❌ [PARSER] คาดหวัง ')' ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    (*pos)++; // ข้าม )
    return params;
}

ASTNode *parse_function(int *pos) {
    if (tokens[*pos].type != TOKEN_FUNC) return NULL;
    (*pos)++; // ข้าม func

    if (tokens[*pos].type != TOKEN_IDENTIFIER) {
        printf("❌ [PARSER] คาดหวังชื่อฟังก์ชันหลัง 'func' ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    ASTNode *func = new_node(TOKEN_FUNC, tokens[*pos].value);
    printf("✅ [PARSER] แยกฟังก์ชัน: %s\n", func->value);
    (*pos)++; // ข้ามชื่อฟังก์ชัน

    func->left = parse_parameters(pos); // แยกพารามิเตอร์
    if (!func->left && tokens[*pos-1].type != TOKEN_RPAREN) {
        printf("❌ [PARSER] การแยกพารามิเตอร์ล้มเหลวที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }

    if (tokens[*pos].type == TOKEN_ARROW) {
        (*pos)++; // ข้าม ->
        if (tokens[*pos].type != TOKEN_TYPE && tokens[*pos].type != TOKEN_POINTER) {
            printf("❌ [PARSER] คาดหวังชนิดข้อมูลหลัง '->' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        func->right = new_node(tokens[*pos].type, tokens[*pos].value);
        (*pos)++; // ข้ามชนิดข้อมูลที่คืนค่า
    }

    if (tokens[*pos].type != TOKEN_LBRACE) {
        printf("❌ [PARSER] คาดหวัง '{' หลังฟังก์ชันที่ตำแหน่ง %d (พบ %s)\n", *pos, tokens[*pos].value);
        return NULL;
    }
    (*pos)++; // ข้าม {
    ASTNode *body = NULL, *current = NULL;
    while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
        ASTNode *stmt = parse_expression(pos);
        if (stmt) {
            if (!body) body = stmt;
            else current->next = stmt;
            current = stmt;
        } else break;
    }
    if (tokens[*pos].type != TOKEN_RBRACE) {
        printf("❌ [PARSER] คาดหวัง '}' ที่ท้ายฟังก์ชันที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    (*pos)++; // ข้าม }
    func->next = body;
    return func;
}

ASTNode *parse_struct(int *pos) {
    if (tokens[*pos].type != TOKEN_STRUCT) return NULL;
    (*pos)++; // ข้าม struct

    if (tokens[*pos].type != TOKEN_IDENTIFIER) {
        printf("❌ [PARSER] คาดหวังชื่อ Struct หลัง 'struct' ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    ASTNode *struct_node = new_node(TOKEN_STRUCT, tokens[*pos].value);
    printf("✅ [PARSER] แยก Struct: %s\n", struct_node->value);
    (*pos)++; // ข้ามชื่อ Struct

    if (tokens[*pos].type != TOKEN_LBRACE) {
        printf("❌ [PARSER] คาดหวัง '{' หลัง Struct ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    (*pos)++; // ข้าม {
    ASTNode *fields = NULL, *current = NULL;
    while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
        if (tokens[*pos].type == TOKEN_IDENTIFIER) {
            ASTNode *field = new_node(TOKEN_IDENTIFIER, tokens[*pos].value);
            (*pos)++; // ข้ามชื่อฟิลด์
            if (tokens[*pos].type == TOKEN_COLON) {
                (*pos)++; // ข้าม :
                if (tokens[*pos].type == TOKEN_TYPE) {
                    field->left = new_node(tokens[*pos].type, tokens[*pos].value);
                    (*pos)++; // ข้ามชนิดข้อมูล
                } else {
                    printf("❌ [PARSER] คาดหวังชนิดข้อมูลหลัง ':' ใน Struct ที่ตำแหน่ง %d\n", *pos);
                    return NULL;
                }
            }
            if (!fields) fields = field;
            else current->next = field;
            current = field;
            if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        } else break;
    }
    if (tokens[*pos].type != TOKEN_RBRACE) {
        printf("❌ [PARSER] คาดหวัง '}' ที่ท้าย Struct ที่ตำแหน่ง %d\n", *pos);
        return NULL;
    }
    (*pos)++; // ข้าม }
    struct_node->left = fields;
    return struct_node;
}

ASTNode *parse_expression(int *pos) {
    printf("🔍 [PARSER] แยกโทเค็นที่ตำแหน่ง %d: %s\n", *pos, tokens[*pos].value);

    if (tokens[*pos].type == TOKEN_EOF) return NULL;

    if (tokens[*pos].type == TOKEN_FUNC) return parse_function(pos);
    if (tokens[*pos].type == TOKEN_STRUCT) return parse_struct(pos);

    if (tokens[*pos].type == TOKEN_VAR) {
        (*pos)++; // ข้าม var
        if (tokens[*pos].type != TOKEN_IDENTIFIER) {
            printf("❌ [PARSER] คาดหวังชื่อตัวแปรหลัง 'var' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        ASTNode *var = new_node(TOKEN_VAR, tokens[*pos].value);
        (*pos)++; // ข้ามชื่อตัวแปร
        if (tokens[*pos].type == TOKEN_COLON) {
            (*pos)++; // ข้าม :
            if (tokens[*pos].type == TOKEN_TYPE || tokens[*pos].type == TOKEN_POINTER) {
                var->left = new_node(tokens[*pos].type, tokens[*pos].value);
                (*pos)++; // ข้ามชนิดข้อมูล
            } else {
                printf("❌ [PARSER] คาดหวังชนิดข้อมูลหลัง ':' ที่ตำแหน่ง %d\n", *pos);
                return NULL;
            }
        }
        if (tokens[*pos].type == TOKEN_ASSIGN) {
            (*pos)++; // ข้าม =
            var->right = parse_expression(pos);
            if (!var->right) {
                printf("❌ [PARSER] คาดหวังนิพจน์หลัง '=' ที่ตำแหน่ง %d\n", *pos);
                return NULL;
            }
        }
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return var;
    }

    if (tokens[*pos].type == TOKEN_PRINT || tokens[*pos].type == TOKEN_PRINTLN || 
        tokens[*pos].type == TOKEN_FORMAT) {
        ASTNode *call = new_node(tokens[*pos].type, tokens[*pos].value);
        (*pos)++; // ข้ามชื่อฟังก์ชัน
        if (tokens[*pos].type != TOKEN_LPAREN) {
            printf("❌ [PARSER] คาดหวัง '(' หลัง %s ที่ตำแหน่ง %d\n", call->value, *pos);
            return NULL;
        }
        (*pos)++; // ข้าม (
        ASTNode *args = NULL, *current = NULL;
        while (tokens[*pos].type != TOKEN_RPAREN && tokens[*pos].type != TOKEN_EOF) {
            ASTNode *arg = parse_expression(pos);
            if (arg) {
                if (!args) args = arg;
                else current->next = arg;
                current = arg;
            }
            if (tokens[*pos].type == TOKEN_COMMA) (*pos)++; // ข้าม ,
        }
        if (tokens[*pos].type != TOKEN_RPAREN) {
            printf("❌ [PARSER] คาดหวัง ')' หลังอาร์กิวเมนต์ของ %s ที่ตำแหน่ง %d\n", call->value, *pos);
            return NULL;
        }
        (*pos)++; // ข้าม )
        call->left = args;
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return call;
    }

    if (tokens[*pos].type == TOKEN_PROMPT) {
        ASTNode *call = new_node(TOKEN_PROMPT, "prompt");
        (*pos)++; // ข้าม prompt
        if (tokens[*pos].type == TOKEN_LPAREN) (*pos)++; // ข้าม (
        if (tokens[*pos].type == TOKEN_RPAREN) (*pos)++; // ข้าม )
        else {
            printf("❌ [PARSER] คาดหวัง ')' หลัง prompt() ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return call;
    }

    if (tokens[*pos].type == TOKEN_IF) {
        ASTNode *if_node = new_node(TOKEN_IF, "if");
        (*pos)++; // ข้าม if
        if (tokens[*pos].type != TOKEN_LPAREN) {
            printf("❌ [PARSER] คาดหวัง '(' หลัง 'if' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม (
        if_node->left = parse_expression(pos);
        if (!if_node->left) {
            printf("❌ [PARSER] คาดหวังเงื่อนไขใน 'if' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        if (tokens[*pos].type != TOKEN_RPAREN) {
            printf("❌ [PARSER] คาดหวัง ')' หลังเงื่อนไข 'if' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม )
        if (tokens[*pos].type != TOKEN_LBRACE) {
            printf("❌ [PARSER] คาดหวัง '{' หลัง 'if' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม {
        ASTNode *body = NULL, *current = NULL;
        while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
            ASTNode *stmt = parse_expression(pos);
            if (stmt) {
                if (!body) body = stmt;
                else current->next = stmt;
                current = stmt;
            } else break;
        }
        if (tokens[*pos].type != TOKEN_RBRACE) {
            printf("❌ [PARSER] คาดหวัง '}' หลัง 'if' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม }
        if_node->right = body;

        if (tokens[*pos].type == TOKEN_ELSE) {
            (*pos)++; // ข้าม else
            ASTNode *else_node = new_node(TOKEN_ELSE, "else");
            if (tokens[*pos].type == TOKEN_IF) {
                else_node->left = parse_expression(pos); // else if
            } else if (tokens[*pos].type == TOKEN_LBRACE) {
                (*pos)++; // ข้าม {
                ASTNode *else_body = NULL, *current = NULL;
                while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
                    ASTNode *stmt = parse_expression(pos);
                    if (stmt) {
                        if (!else_body) else_body = stmt;
                        else current->next = stmt;
                        current = stmt;
                    } else break;
                }
                if (tokens[*pos].type != TOKEN_RBRACE) {
                    printf("❌ [PARSER] คาดหวัง '}' หลัง 'else' ที่ตำแหน่ง %d\n", *pos);
                    return NULL;
                }
                (*pos)++; // ข้าม }
                else_node->left = else_body;
            }
            if_node->next = else_node;
        }
        return if_node;
    }

    if (tokens[*pos].type == TOKEN_FOR) {
        ASTNode *for_node = new_node(TOKEN_FOR, "for");
        (*pos)++; // ข้าม for
        if (tokens[*pos].type != TOKEN_LPAREN) {
            printf("❌ [PARSER] คาดหวัง '(' หลัง 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม (
        for_node->left = parse_expression(pos); // init
        if (tokens[*pos].type != TOKEN_SEMICOLON) {
            printf("❌ [PARSER] คาดหวัง ';' หลังการกำหนดค่าเริ่มต้นใน 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม ;
        for_node->right = parse_expression(pos); // condition
        if (tokens[*pos].type != TOKEN_SEMICOLON) {
            printf("❌ [PARSER] คาดหวัง ';' หลังเงื่อนไขใน 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม ;
        ASTNode *step = parse_expression(pos); // step
        for_node->right->next = step;
        if (tokens[*pos].type != TOKEN_RPAREN) {
            printf("❌ [PARSER] คาดหวัง ')' หลัง 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม )
        if (tokens[*pos].type != TOKEN_LBRACE) {
            printf("❌ [PARSER] คาดหวัง '{' หลัง 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม {
        ASTNode *body = NULL, *current = NULL;
        while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
            ASTNode *stmt = parse_expression(pos);
            if (stmt) {
                if (!body) body = stmt;
                else current->next = stmt;
                current = stmt;
            } else break;
        }
        if (tokens[*pos].type != TOKEN_RBRACE) {
            printf("❌ [PARSER] คาดหวัง '}' หลัง 'for' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม }
        for_node->next = body;
        return for_node;
    }

    if (tokens[*pos].type == TOKEN_WHILE) {
        ASTNode *while_node = new_node(TOKEN_WHILE, "while");
        (*pos)++; // ข้าม while
        if (tokens[*pos].type != TOKEN_LPAREN) {
            printf("❌ [PARSER] คาดหวัง '(' หลัง 'while' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม (
        while_node->left = parse_expression(pos);
        if (!while_node->left) {
            printf("❌ [PARSER] คาดหวังเงื่อนไขใน 'while' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        if (tokens[*pos].type != TOKEN_RPAREN) {
            printf("❌ [PARSER] คาดหวัง ')' หลังเงื่อนไข 'while' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม )
        if (tokens[*pos].type != TOKEN_LBRACE) {
            printf("❌ [PARSER] คาดหวัง '{' หลัง 'while' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม {
        ASTNode *body = NULL, *current = NULL;
        while (tokens[*pos].type != TOKEN_RBRACE && tokens[*pos].type != TOKEN_EOF) {
            ASTNode *stmt = parse_expression(pos);
            if (stmt) {
                if (!body) body = stmt;
                else current->next = stmt;
                current = stmt;
            } else break;
        }
        if (tokens[*pos].type != TOKEN_RBRACE) {
            printf("❌ [PARSER] คาดหวัง '}' หลัง 'while' ที่ตำแหน่ง %d\n", *pos);
            return NULL;
        }
        (*pos)++; // ข้าม }
        while_node->right = body;
        return while_node;
    }

    if (tokens[*pos].type == TOKEN_BREAK || tokens[*pos].type == TOKEN_CONTINUE) {
        ASTNode *node = new_node(tokens[*pos].type, tokens[*pos].value);
        (*pos)++;
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return node;
    }

    if (tokens[*pos].type == TOKEN_RETURN) {
        ASTNode *ret = new_node(TOKEN_RETURN, "return");
        (*pos)++; // ข้าม return
        ret->left = parse_expression(pos);
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return ret;
    }

    if (tokens[*pos].type == TOKEN_NUMBER || tokens[*pos].type == TOKEN_STRING || 
        tokens[*pos].type == TOKEN_BOOL || tokens[*pos].type == TOKEN_NULL || 
        tokens[*pos].type == TOKEN_IDENTIFIER) {
        ASTNode *node = new_node(tokens[*pos].type, tokens[*pos].value);
        (*pos)++;
        if (tokens[*pos].type == TOKEN_PLUS || tokens[*pos].type == TOKEN_MINUS || 
            tokens[*pos].type == TOKEN_MULTIPLY || tokens[*pos].type == TOKEN_DIVIDE || 
            tokens[*pos].type == TOKEN_POW || tokens[*pos].type == TOKEN_EQ || 
            tokens[*pos].type == TOKEN_NEQ || tokens[*pos].type == TOKEN_GT || 
            tokens[*pos].type == TOKEN_LT || tokens[*pos].type == TOKEN_GTE || 
            tokens[*pos].type == TOKEN_LTE) {
            ASTNode *op = new_node(tokens[*pos].type, tokens[*pos].value);
            (*pos)++;
            op->left = node;
            op->right = parse_expression(pos);
            if (!op->right) {
                printf("❌ [PARSER] คาดหวังนิพจน์หลังตัวดำเนินการ %s ที่ตำแหน่ง %d\n", op->value, *pos);
                return NULL;
            }
            return op;
        }
        if (tokens[*pos].type == TOKEN_LBRACKET) {
            (*pos)++; // ข้าม [
            node->left = parse_expression(pos);
            if (tokens[*pos].type != TOKEN_RBRACKET) {
                printf("❌ [PARSER] คาดหวัง ']' หลังการเข้าถึง Array ที่ตำแหน่ง %d\n", *pos);
                return NULL;
            }
            (*pos)++; // ข้าม ]
        }
        if (tokens[*pos].type == TOKEN_DOT) {
            (*pos)++; // ข้าม .
            if (tokens[*pos].type != TOKEN_IDENTIFIER) {
                printf("❌ [PARSER] คาดหวังชื่อฟิลด์หลัง '.' ที่ตำแหน่ง %d\n", *pos);
                return NULL;
            }
            node->right = new_node(TOKEN_IDENTIFIER, tokens[*pos].value);
            (*pos)++;
        }
        if (tokens[*pos].type == TOKEN_SEMICOLON) (*pos)++; // ข้าม ;
        return node;
    }

    printf("❌ [PARSER] ไม่รู้จักโทเค็น %s ที่ตำแหน่ง %d\n", tokens[*pos].value, *pos);
    return NULL;
}

ASTNode *parse(const char *source) {
    lex(source);
    int pos = 0;
    ASTNode *root = NULL, *current = NULL;
    while (pos < token_count && tokens[pos].type != TOKEN_EOF) {
        printf("🔍 [PARSER] เริ่มแยกที่ตำแหน่ง %d: %s\n", pos, tokens[pos].value);
        ASTNode *stmt = parse_expression(&pos);
        if (stmt) {
            if (!root) root = stmt;
            else current->next = stmt;
            current = stmt;
        } else {
            printf("❌ [PARSER] การแยกวิเคราะห์ล้มเหลวที่ตำแหน่ง %d\n", pos);
            free_ast(root);
            return NULL;
        }
    }
    printf("✅ [PARSER] การแยกวิเคราะห์เสร็จสมบูรณ์\n");
    return root;
}

void free_ast(ASTNode *node) {
    if (!node) return;
    free_ast(node->left);
    free_ast(node->right);
    free_ast(node->next);
    free(node);
}