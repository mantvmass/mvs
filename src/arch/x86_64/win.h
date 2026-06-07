/*
 * arch/x86_64/win.h - ส่วนหัวของ backend x86-64 Windows
 * ตัวขับใน codegen.c เรียกฟังก์ชันนี้เมื่อเลือกสถาปัตยกรรม ARCH_X86_64_WIN
 */
#ifndef MVS_ARCH_X86_64_WIN_H
#define MVS_ARCH_X86_64_WIN_H

#include <stdio.h>
#include "../../ast.h"

/* สร้างแอสเซมบลีจาก AST แล้วเขียนลง out คืน 0 เมื่อสำเร็จ */
int x86_64_win_generate(Node *program, FILE *out);

#endif /* MVS_ARCH_X86_64_WIN_H */
