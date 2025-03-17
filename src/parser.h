#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

// โหนดใน Abstract Syntax Tree (AST)
typedef struct ASTNode {
    TokenType type; // ประเภทของโหนด
    char value[MAX_TOKEN_LENGTH]; // ค่าของโหนด
    struct ASTNode *left; // โหนดลูกซ้าย
    struct ASTNode *right; // โหนดลูกขวา
    struct ASTNode *next; // โหนดถัดไป (สำหรับลิสต์คำสั่ง)
} ASTNode;

ASTNode *new_node(TokenType type, char *value); // สร้างโหนดใหม่
ASTNode *parse(const char *source); // แยกวิเคราะห์ซอร์สโค้ดทั้งหมด
ASTNode *parse_expression(int *pos); // แยกวิเคราะห์นิพจน์
ASTNode *parse_function(int *pos); // แยกวิเคราะห์ฟังก์ชัน (เพิ่ม)
ASTNode *parse_struct(int *pos); // แยกวิเคราะห์ Struct (เพิ่ม)
ASTNode *parse_parameters(int *pos); // แยกวิเคราะห์พารามิเตอร์ (เพิ่ม)
void free_ast(ASTNode *node); // คืนหน่วยความจำของ AST

#endif // PARSER_H