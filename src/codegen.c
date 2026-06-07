/*
 * codegen.c - ตัวขับการสร้างโค้ด เลือก backend ตามสถาปัตยกรรมเป้าหมาย
 *
 * ชั้นนี้บางมากโดยตั้งใจ: หน้าที่เดียวคือเปิดไฟล์เอาต์พุตและส่งต่อ AST
 * ไปยัง backend ที่ถูกต้อง ทำให้ส่วนอื่นของคอมไพเลอร์ไม่ผูกกับสถาปัตยกรรมใด ๆ
 */
#include <stdio.h>
#include "codegen.h"
#include "arch/x86_64/win.h"

int codegen_generate(Node *program, const char *asm_path, TargetArch arch) {
    /* เปิดไฟล์สำหรับเขียนแอสเซมบลี */
    FILE *out = fopen(asm_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot open output file '%s'\n", asm_path);
        return 1;
    }

    int rc;
    switch (arch) {
        case ARCH_X86_64_WIN:
            rc = x86_64_win_generate(program, out);
            break;
        default:
            fprintf(stderr, "error: unsupported target architecture\n");
            rc = 1;
            break;
    }

    fclose(out);
    return rc;
}
