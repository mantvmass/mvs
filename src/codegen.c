#include <stdio.h>
#include <string.h>
#include "codegen.h"

// ฟังก์ชันช่วยในการสร้างโค้ด C
void generate_expression(FILE *file, ASTNode *node);

// ฟังก์ชันสร้างโค้ดสำหรับ Struct
void generate_struct(FILE *file, ASTNode *node) {
    fprintf(file, "typedef struct {\n");
    ASTNode *field = node->left;
    while (field) {
        if (field->left->type == TOKEN_TYPE) {
            if (strcmp(field->left->value, "Number") == 0) fprintf(file, "    double %s;\n", field->value);
            else if (strcmp(field->left->value, "String") == 0) fprintf(file, "    char *%s;\n", field->value);
            else if (strcmp(field->left->value, "Bool") == 0) fprintf(file, "    int %s;\n", field->value);
        }
        field = field->next;
    }
    fprintf(file, "} %s;\n\n", node->value);
}

// ฟังก์ชันสร้างโค้ดสำหรับฟังก์ชัน
void generate_function(FILE *file, ASTNode *node) {
    if (node->right) {
        if (strcmp(node->right->value, "Number") == 0) fprintf(file, "double ");
        else if (strcmp(node->right->value, "String") == 0) fprintf(file, "char *");
        else if (strcmp(node->right->value, "Void") == 0) fprintf(file, "void ");
    } else fprintf(file, "void ");
    fprintf(file, "%s(", node->value);

    ASTNode *param = node->left;
    int first = 1;
    while (param) {
        if (!first) fprintf(file, ", ");
        if (param->left->type == TOKEN_POINTER) fprintf(file, "double *");
        else if (strcmp(param->left->value, "Number") == 0) fprintf(file, "double ");
        else if (strcmp(param->left->value, "String") == 0) fprintf(file, "char *");
        fprintf(file, "%s", param->value);
        first = 0;
        param = param->next;
    }
    fprintf(file, ") {\n");

    ASTNode *stmt = node->next;
    while (stmt) {
        generate_expression(file, stmt);
        stmt = stmt->next;
    }
    fprintf(file, "}\n\n");
}

// ฟังก์ชันสร้างโค้ดสำหรับนิพจน์
void generate_expression(FILE *file, ASTNode *node) {
    if (!node) return;

    if (node->type == TOKEN_VAR) {
        fprintf(file, "    ");
        if (node->left) {
            if (strcmp(node->left->value, "Number") == 0) fprintf(file, "double ");
            else if (strcmp(node->left->value, "String") == 0) fprintf(file, "char *");
            else if (strcmp(node->left->value, "Bool") == 0) fprintf(file, "int ");
            else if (strcmp(node->left->value, "Array") == 0) fprintf(file, "double *");
        } else fprintf(file, "double "); // Default to Number
        fprintf(file, "%s", node->value);
        if (node->right) {
            fprintf(file, " = ");
            generate_expression(file, node->right);
        }
        fprintf(file, ";\n");
    } else if (node->type == TOKEN_PRINT || node->type == TOKEN_PRINTLN) {
        fprintf(file, "    printf(");
        ASTNode *arg = node->left;
        if (arg->type == TOKEN_STRING) fprintf(file, "\"%s\"", arg->value);
        arg = arg->next;
        while (arg) {
            fprintf(file, ", ");
            generate_expression(file, arg);
            arg = arg->next;
        }
        fprintf(file, ");\n");
        if (node->type == TOKEN_PRINTLN) fprintf(file, "    printf(\"\\n\");\n");
    } else if (node->type == TOKEN_FORMAT) {
        fprintf(file, "format(");
        ASTNode *arg = node->left;
        if (arg->type == TOKEN_STRING) fprintf(file, "\"%s\"", arg->value);
        arg = arg->next;
        while (arg) {
            fprintf(file, ", ");
            generate_expression(file, arg);
            arg = arg->next;
        }
        fprintf(file, ")");
    } else if (node->type == TOKEN_PROMPT) {
        fprintf(file, "prompt()");
    } else if (node->type == TOKEN_IF) {
        fprintf(file, "    if (");
        generate_expression(file, node->left);
        fprintf(file, ") {\n");
        ASTNode *stmt = node->right;
        while (stmt) {
            generate_expression(file, stmt);
            stmt = stmt->next;
        }
        fprintf(file, "    }\n");
        if (node->next && node->next->type == TOKEN_ELSE) {
            fprintf(file, "    else ");
            if (node->next->left->type == TOKEN_IF) {
                generate_expression(file, node->next->left);
            } else {
                fprintf(file, "{\n");
                stmt = node->next->left;
                while (stmt) {
                    generate_expression(file, stmt);
                    stmt = stmt->next;
                }
                fprintf(file, "    }\n");
            }
        }
    } else if (node->type == TOKEN_FOR) {
        fprintf(file, "    for (");
        generate_expression(file, node->left);
        fprintf(file, "; ");
        generate_expression(file, node->right);
        fprintf(file, "; ");
        generate_expression(file, node->right->next);
        fprintf(file, ") {\n");
        ASTNode *stmt = node->next;
        while (stmt) {
            generate_expression(file, stmt);
            stmt = stmt->next;
        }
        fprintf(file, "    }\n");
    } else if (node->type == TOKEN_WHILE) {
        fprintf(file, "    while (");
        generate_expression(file, node->left);
        fprintf(file, ") {\n");
        ASTNode *stmt = node->right;
        while (stmt) {
            generate_expression(file, stmt);
            stmt = stmt->next;
        }
        fprintf(file, "    }\n");
    } else if (node->type == TOKEN_BREAK) {
        fprintf(file, "    break;\n");
    } else if (node->type == TOKEN_CONTINUE) {
        fprintf(file, "    continue;\n");
    } else if (node->type == TOKEN_RETURN) {
        fprintf(file, "    return ");
        generate_expression(file, node->left);
        fprintf(file, ";\n");
    } else if (node->type == TOKEN_NUMBER || node->type == TOKEN_STRING || 
               node->type == TOKEN_BOOL || node->type == TOKEN_NULL || 
               node->type == TOKEN_IDENTIFIER) {
        if (node->type == TOKEN_STRING) fprintf(file, "\"%s\"", node->value);
        else if (node->left && node->left->type == TOKEN_NUMBER) fprintf(file, "%s[%s]", node->value, node->left->value); // Array access
        else if (node->right) fprintf(file, "%s.%s", node->value, node->right->value); // Struct access
        else fprintf(file, "%s", node->value);
        if (node->next && node->next->type == TOKEN_ASSIGN) {
            fprintf(file, " = ");
            generate_expression(file, node->next->right);
        }
    } else if (node->type == TOKEN_PLUS || node->type == TOKEN_MINUS || 
               node->type == TOKEN_MULTIPLY || node->type == TOKEN_DIVIDE || 
               node->type == TOKEN_POW || node->type == TOKEN_EQ || 
               node->type == TOKEN_NEQ || node->type == TOKEN_GT || 
               node->type == TOKEN_LT || node->type == TOKEN_GTE || 
               node->type == TOKEN_LTE) {
        generate_expression(file, node->left);
        fprintf(file, " %s ", node->value);
        generate_expression(file, node->right);
    }
}

// สร้างโค้ด C จาก AST
void generate_assembly(ASTNode *ast) {
    FILE *file = fopen("output.c", "w");
    if (!file) {
        printf("❌ [ERROR] ไม่สามารถสร้างไฟล์ output.c ได้\n");
        return;
    }

    // เขียนส่วนหัว
    fprintf(file, "#include <stdio.h>\n");
    fprintf(file, "#include <stdlib.h>\n");
    fprintf(file, "#include <string.h>\n");
    fprintf(file, "#include <math.h>\n\n");
    fprintf(file, "char *format(const char *fmt, ...) {\n");
    fprintf(file, "    char buffer[256];\n");
    fprintf(file, "    va_list args;\n");
    fprintf(file, "    va_start(args, fmt);\n");
    fprintf(file, "    vsnprintf(buffer, sizeof(buffer), fmt, args);\n");
    fprintf(file, "    va_end(args);\n");
    fprintf(file, "    return strdup(buffer);\n");
    fprintf(file, "}\n\n");
    fprintf(file, "char *prompt() {\n");
    fprintf(file, "    char buffer[256];\n");
    fprintf(file, "    fgets(buffer, sizeof(buffer), stdin);\n");
    fprintf(file, "    buffer[strcspn(buffer, \"\\n\")] = 0;\n");
    fprintf(file, "    return strdup(buffer);\n");
    fprintf(file, "}\n\n");

    // สร้างโค้ดจาก AST
    ASTNode *current = ast;
    while (current) {
        if (current->type == TOKEN_STRUCT) generate_struct(file, current);
        else if (current->type == TOKEN_FUNC) generate_function(file, current);
        else generate_expression(file, current);
        current = current->next;
    }

    fclose(file);
    system("gcc output.c -o output -lm");
    printf("✅ [CODEGEN] สร้างและคอมไพล์โค้ด C เสร็จสมบูรณ์\n");
}