/*
 * parser.h - ส่วนหัวของตัววิเคราะห์ไวยากรณ์ (Parser)
 *
 * parser อ่าน token จาก lexer แล้วประกอบเป็น AST ตามไวยากรณ์ของภาษา MVS
 * ใช้เทคนิค recursive descent (ฟังก์ชันหนึ่งตัวต่อกฎไวยากรณ์หนึ่งกฎ)
 */
#ifndef MVS_PARSER_H
#define MVS_PARSER_H

#include "lexer.h"
#include "ast.h"

/* สถานะของ parser */
typedef struct {
    Lexer *lx;          /* ตัวตัดคำที่กำลังใช้ */
    Token  cur;         /* token ปัจจุบันที่กำลังพิจารณา */
    int    had_error;   /* ธงบอกว่าพบ syntax error หรือไม่ */
    int    depth;       /* ความลึก recursion ปัจจุบัน (กัน stack overflow จาก input ซ้อนลึก) */
} Parser;

/* แปลงซอร์สโค้ดทั้งไฟล์เป็น AST (โหนด ND_PROGRAM) คืน NULL ถ้ามี error ร้ายแรง */
Node *parse_program(const char *src, const char *filename, int *had_error);

#endif /* MVS_PARSER_H */
