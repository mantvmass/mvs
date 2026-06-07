/*
 * lexer.h - ส่วนหัวของตัวตัดคำ (Lexical Analyzer / Tokenizer)
 *
 * lexer ทำหน้าที่อ่านซอร์สโค้ด (สตริง) แล้วแปลงเป็นชุดของ Token ทีละตัว
 * parser จะเรียก lexer_next() เพื่อขอ token ตัวถัดไปเรื่อย ๆ จนกว่าจะถึง TK_EOF
 */
#ifndef MVS_LEXER_H
#define MVS_LEXER_H

#include "token.h"

/* สถานะของ lexer ขณะทำงาน */
typedef struct {
    const char *src;       /* ตัวชี้ไปยังซอร์สโค้ดทั้งหมด */
    size_t      pos;       /* ตำแหน่งปัจจุบันในซอร์ส (ดัชนีตัวอักษร) */
    int         line;      /* บรรทัดปัจจุบัน (เริ่มที่ 1) */
    int         col;       /* คอลัมน์ปัจจุบัน (เริ่มที่ 1) */
    const char *filename;  /* ชื่อไฟล์ ใช้รายงาน error */
    int         had_error; /* ธงบอกว่าพบข้อผิดพลาดระหว่างตัดคำหรือไม่ */
} Lexer;

/* เริ่มต้น lexer ด้วยซอร์สโค้ดและชื่อไฟล์ */
void  lexer_init(Lexer *lx, const char *src, const char *filename);

/* ดึง token ตัวถัดไป ผู้เรียกมีหน้าที่ free(token.lexeme) เองเมื่อใช้เสร็จ */
Token lexer_next(Lexer *lx);

#endif /* MVS_LEXER_H */
