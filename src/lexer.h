#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

// กำหนดขนาดสูงสุดของโทเค็นและจำนวนโทเค็น
#define MAX_TOKENS 4096
#define MAX_TOKEN_LENGTH 128

// ประเภทของโทเค็นที่คอมไพเลอร์จะรู้จัก
typedef enum {
    TOKEN_NUMBER, TOKEN_STRING, TOKEN_BOOL, TOKEN_NULL, TOKEN_IDENTIFIER, // ชนิดข้อมูลพื้นฐาน
    TOKEN_VAR, TOKEN_FUNC, TOKEN_STRUCT, TOKEN_RETURN, // คำสงวนทั่วไป
    TOKEN_IF, TOKEN_ELSE, TOKEN_FOR, TOKEN_WHILE, TOKEN_BREAK, TOKEN_CONTINUE, // โครงสร้างควบคุม
    TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACKET, TOKEN_RBRACKET, // วงเล็บ
    TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_COMMA, TOKEN_DOT, TOKEN_ASSIGN, TOKEN_ARROW, // อักขระพิเศษ
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_MULTIPLY, TOKEN_DIVIDE, TOKEN_POW, // ตัวดำเนินการคณิตศาสตร์
    TOKEN_EQ, TOKEN_NEQ, TOKEN_GT, TOKEN_LT, TOKEN_GTE, TOKEN_LTE, // ตัวดำเนินการเปรียบเทียบ
    TOKEN_TYPE, TOKEN_OPTION_SOME, TOKEN_OPTION_NONE, TOKEN_POINTER, TOKEN_ADDRESS, // ชนิดข้อมูลเพิ่มเติม
    TOKEN_PRINT, TOKEN_PRINTLN, TOKEN_FORMAT, TOKEN_PROMPT, // ฟังก์ชันในตัว
    TOKEN_EOF, TOKEN_COMMENT // โทเค็นพิเศษ
} TokenType;

// โครงสร้างข้อมูลสำหรับเก็บโทเค็น
typedef struct {
    TokenType type; // ประเภทของโทเค็น
    char value[MAX_TOKEN_LENGTH]; // ค่าของโทเค็น
} Token;

extern Token tokens[MAX_TOKENS];
extern int token_count;

void lex(const char *source); // ฟังก์ชันแยกโทเค็น
void add_token(TokenType type, const char *value); // ฟังก์ชันเพิ่มโทเค็น

#endif // LEXER_H