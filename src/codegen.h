/*
 * codegen.h - ส่วนหัวของตัวสร้างโค้ด (Code Generator) — ชั้นที่ไม่ขึ้นกับสถาปัตยกรรม
 *
 * ตัวขับ (driver) นี้รับ AST แล้วเลือก backend ตามสถาปัตยกรรมเป้าหมาย
 * การเพิ่มสถาปัตยกรรมใหม่ในอนาคต (เช่น ARM64) ทำได้โดยเพิ่มค่าใน TargetArch
 * และเพิ่มไฟล์ backend ใหม่ใน arch/ โดยไม่ต้องแก้ส่วน lexer/parser
 */
#ifndef MVS_CODEGEN_H
#define MVS_CODEGEN_H

#include "ast.h"

/* สถาปัตยกรรมเป้าหมายที่รองรับ */
typedef enum {
    ARCH_X86_64_WIN   /* x86-64 บน Windows (เอาต์พุตเป็น NASM syntax, win64 ABI) */
    /* อนาคต: ARCH_ARM64, ARCH_X86_64_LINUX, ... */
} TargetArch;

/* สร้างไฟล์ assembly จาก AST เขียนลงไฟล์ asm_path
 * คืน 0 เมื่อสำเร็จ, ค่าอื่นเมื่อผิดพลาด */
int codegen_generate(Node *program, const char *asm_path, TargetArch arch);

#endif /* MVS_CODEGEN_H */
