/*
 * main.c - จุดเริ่มต้นของคอมไพเลอร์ MVS และตัวขับ pipeline ทั้งหมด
 *
 * ขั้นตอนการทำงาน:
 *   1. อ่านไฟล์ซอร์ส .mvs
 *   2. parse เป็น AST                         (lexer + parser)
 *   3. สร้างไฟล์แอสเซมบลี .asm                (codegen)
 *   4. ประกอบเป็นไฟล์อ็อบเจกต์ .obj           (nasm -f win64)
 *   5. ลิงก์เป็นไฟล์ปฏิบัติการ .exe            (clang)
 *
 * การใช้งาน:
 *   mvs <input.mvs> [-o output.exe] [-S] [--keep] [--emit-ast]
 *     -S          สร้างเฉพาะไฟล์ .asm แล้วหยุด (ไม่เรียก nasm/clang)
 *     --keep      เก็บไฟล์กลาง (.asm, .obj) ไว้ ไม่ลบทิ้ง
 *     -o <file>   กำหนดชื่อไฟล์ผลลัพธ์
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "generic.h"
#include "codegen.h"

#define PATHBUF 1024

/* รัน "<tool> --version" อ่านบรรทัดแรกลง ver; คืน 1 ถ้าพบเครื่องมือ (และพร้อมใช้งาน)
 * ใช้ตรวจว่าผู้ใช้ติดตั้ง nasm/clang ไว้หรือยัง และรายงานเวอร์ชันที่ใช้ */
static int tool_version(const char *name, char *ver, size_t vn) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --version 2>&1", name);
    FILE *f = _popen(cmd, "r");
    if (!f) return 0;
    ver[0] = '\0';
    if (fgets(ver, (int)vn, f)) { char *nl = strchr(ver, '\n'); if (nl) *nl = '\0'; }
    int rc = _pclose(f);
    return (rc == 0 && ver[0] != '\0');
}

/* คัดลอกชื่อฐาน (ตัดนามสกุล .mvs ออก) ลง dst[PATHBUF]
 * ตัดเฉพาะจุดที่อยู่หลังตัวคั่นพาธสุดท้าย เพื่อไม่ให้โฟลเดอร์ที่มีจุด (เช่น my.proj) โดนตัด */
static int base_name(const char *path, char *dst) {
    if (strlen(path) >= PATHBUF) { fprintf(stderr, "error: input path too long\n"); return 0; }
    snprintf(dst, PATHBUF, "%s", path);
    char *slash = strrchr(dst, '/');
    char *bslash = strrchr(dst, '\\');
    char *sep = slash > bslash ? slash : bslash;
    char *dot = strrchr(dst, '.');
    if (dot && (!sep || dot > sep)) *dot = '\0';
    return 1;
}

/* คัดลอกส่วนโฟลเดอร์ของ path ลง dst[PATHBUF] (รองรับทั้ง / และ \) ถ้าไม่มีคืน "." */
static void dir_name(const char *path, char *dst) {
    snprintf(dst, PATHBUF, "%s", path);
    char *slash = strrchr(dst, '/');
    char *bslash = strrchr(dst, '\\');
    char *cut = slash > bslash ? slash : bslash;
    if (cut) *cut = '\0';
    else strcpy(dst, ".");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "MVS compiler\n"
            "usage: %s <input.mvs> [options]\n"
            "  -o <file>     set the output file name\n"
            "  -S            emit assembly (.asm) only, then stop\n"
            "  -c            emit an object file (.obj) only (for linking with C)\n"
            "  --nostd       freestanding mode: no std/C runtime/OS (emits .obj) - for OS dev\n"
            "  --keep        keep intermediate files (.asm, .obj)\n", argv[0]);
        return 1;
    }

    /* อ่านอาร์กิวเมนต์บรรทัดคำสั่ง */
    const char *input = NULL;
    const char *output = NULL;
    int only_asm = 0;  /* -S */
    int emit_obj = 0;  /* -c / --emit-obj : หยุดที่ไฟล์ .obj */
    int nostd = 0;     /* --nostd : freestanding (ไม่พึ่ง std/CRT) */
    int keep = 0;      /* --keep */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) only_asm = 1;
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--emit-obj") == 0) emit_obj = 1;
        else if (strcmp(argv[i], "--nostd") == 0) { nostd = 1; emit_obj = 1; } /* freestanding -> ผลิต .obj */
        else if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (argv[i][0] != '-') input = argv[i];
    }
    if (!input) { fprintf(stderr, "error: no input file\n"); return 1; }

    /* คำนวณชื่อไฟล์ผลลัพธ์ต่าง ๆ จากชื่อฐาน */
    char base[PATHBUF];
    if (!base_name(input, base)) return 1;
    char asm_path[PATHBUF + 8], obj_path[PATHBUF + 8], exe_path[PATHBUF + 8];
    snprintf(asm_path, sizeof(asm_path), "%s.asm", base);
    snprintf(obj_path, sizeof(obj_path), "%s.obj", base);
    if (output) snprintf(exe_path, sizeof(exe_path), "%s", output);
    else        snprintf(exe_path, sizeof(exe_path), "%s.exe", base);

    /* หาโฟลเดอร์ standard library: ใช้ตัวแปรแวดล้อม MVS_STD ก่อน ไม่งั้นอิงตำแหน่งของ mvs.exe */
    char stddir[PATHBUF + 8];
    const char *env_std = getenv("MVS_STD");
    if (env_std) {
        snprintf(stddir, sizeof(stddir), "%s", env_std);
    } else {
        char exedir[1024];
        dir_name(argv[0], exedir);
        snprintf(stddir, sizeof(stddir), "%s/std", exedir);
    }

    /* 1+2. โหลดไฟล์ entry + resolve import ทั้งหมด แล้ว parse เป็น AST รวม */
    int had_error = 0;
    Node *program = module_load(input, stddir, nostd, &had_error);
    if (had_error) { fprintf(stderr, "compilation failed (parse/import errors)\n"); return 1; }

    /* monomorphization: แปลง generic function เป็น instance เฉพาะชนิดที่ถูกเรียกจริง
     * ตามด้วย resolve overload (รวม instance ของ generic ที่อาจเรียกฟังก์ชัน overload) */
    if (check_duplicates(program) > 0) {   /* ชื่อซ้ำ (struct/trait/func) -> error ทันที */
        fprintf(stderr, "compilation failed (duplicate definitions)\n");
        return 1;
    }
    if (monomorphize(program) > 0) {   /* ตรวจ trait bound ระหว่าง instantiate */
        fprintf(stderr, "compilation failed (trait bound errors)\n");
        return 1;
    }
    resolve_overloads(program);

    /* ตรวจชนิดเวลาคอมไพล์ (จับชนิดมั่ว เช่น 50 + "50") ก่อนลงมือ gen โค้ด */
    if (typecheck(program) > 0) {
        fprintf(stderr, "compilation failed (type errors)\n");
        return 1;
    }

    /* 3. codegen → .asm */
    if (codegen_generate(program, asm_path, ARCH_X86_64_WIN) != 0) {
        fprintf(stderr, "compilation failed (codegen errors)\n");
        return 1;
    }
    printf("[mvs] generated %s\n", asm_path);
    if (only_asm) return 0; /* -S: หยุดที่ไฟล์แอสเซมบลี */

    /* 4. ประกอบด้วย nasm เป็นไฟล์อ็อบเจกต์ — ตรวจก่อนว่ามี nasm ติดตั้งไหม */
    char cmd[PATHBUF * 3], ver[256];
    if (!tool_version("nasm", ver, sizeof(ver))) {
        fprintf(stderr, "error: 'nasm' not found. MVS needs the NASM assembler.\n"
                        "       Install it from https://www.nasm.us and make sure it is on your PATH.\n");
        return 1;
    }
    printf("[mvs] using %s\n", ver);
    snprintf(cmd, sizeof(cmd), "nasm -f win64 \"%s\" -o \"%s\"", asm_path, obj_path);
    printf("[mvs] %s\n", cmd);
    if (system(cmd) != 0) { fprintf(stderr, "error: nasm failed\n"); return 1; }

    /* โหมด -c / --nostd: หยุดที่ไฟล์ .obj (ไว้ลิงก์เองกับ C หรือฝังลงเคอร์เนล) */
    if (emit_obj || nostd) {
        printf("[mvs] produced object file %s%s\n", obj_path, nostd ? " (freestanding, no std/CRT)" : "");
        if (!keep) remove(asm_path);
        return 0;
    }

    /* 5. ลิงก์ด้วย clang (ทำหน้าที่เป็น linker driver พร้อมผูก C runtime)
     *    - legacy_stdio_definitions: ให้สัญลักษณ์ printf/scanf/... จริง (UCRT ทำเป็น inline)
     *    - ws2_32: Winsock สำหรับโมดูล net (ผูกไว้เสมอ ไม่กระทบโปรแกรมที่ไม่ใช้) */
    if (!tool_version("clang", ver, sizeof(ver))) {
        fprintf(stderr, "error: 'clang' not found. MVS uses clang as the linker.\n"
                        "       Install LLVM/clang (https://llvm.org) and make sure it is on your PATH.\n");
        return 1;
    }
    printf("[mvs] using %s\n", ver);
    snprintf(cmd, sizeof(cmd), "clang \"%s\" -o \"%s\" -llegacy_stdio_definitions -lws2_32", obj_path, exe_path);
    printf("[mvs] %s\n", cmd);
    if (system(cmd) != 0) { fprintf(stderr, "error: link failed\n"); return 1; }

    printf("[mvs] built %s\n", exe_path);

    /* ลบไฟล์กลางถ้าไม่ขอเก็บไว้ */
    if (!keep) { remove(asm_path); remove(obj_path); }
    return 0;
}
