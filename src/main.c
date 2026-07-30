/*
 * main.c - MVS compiler entry point and driver of the whole pipeline
 *
 * Steps:
 *   1. Read the .mvs source file
 *   2. Parse into an AST                        (lexer + parser)
 *   3. Generate the .asm assembly file          (codegen)
 *   4. Assemble into a .obj object file         (nasm -f win64)
 *   5. Link into a .exe executable              (clang)
 *
 * Usage:
 *   mvs <input.mvs> [-o output.exe] [-S] [--keep] [--emit-ast]
 *     -S          emit only the .asm file, then stop (no nasm/clang)
 *     --keep      keep intermediate files (.asm, .obj) instead of deleting them
 *     -o <file>   set the output file name
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "generic.h"
#include "codegen.h"
#include "diag.h"

#define PATHBUF 1024

/* popen is spelled _popen on Windows; everything else is portable */
#ifdef _WIN32
#define POPEN  _popen
#define PCLOSE _pclose
#else
#define POPEN  popen
#define PCLOSE pclose
#endif

/* Run "<tool> --version", read the first line into ver; returns 1 if the tool exists (and works).
 * Used to check that the user has nasm/clang installed and to report the version in use */
static int tool_version(const char *name, char *ver, size_t vn) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --version 2>&1", name);
    FILE *f = POPEN(cmd, "r");
    if (!f) return 0;
    ver[0] = '\0';
    if (fgets(ver, (int)vn, f)) { char *nl = strchr(ver, '\n'); if (nl) *nl = '\0'; }
    int rc = PCLOSE(f);
    return (rc == 0 && ver[0] != '\0');
}

/* Copy the base name (with the .mvs extension stripped) into dst[PATHBUF].
 * Only strip a dot after the last path separator, so folders with dots (e.g. my.proj) are untouched */
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

/* Copy the directory part of path into dst[PATHBUF] (handles both / and \); "." if none */
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
            "  --target <t>  target: win64 (default), elf64 (x86-64 Linux), arm64 (AArch64 Linux)\n"
            "  --keep        keep intermediate files (.asm, .obj)\n", argv[0]);
        return 1;
    }

    /* parse command line arguments */
    const char *input = NULL;
    const char *output = NULL;
    int only_asm = 0;  /* -S */
    int emit_obj = 0;  /* -c / --emit-obj : stop at the .obj file */
    int nostd = 0;     /* --nostd : freestanding (no std/CRT dependency) */
    int keep = 0;      /* --keep */
    TargetArch arch = ARCH_X86_64_WIN;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) only_asm = 1;
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--emit-obj") == 0) emit_obj = 1;
        else if (strcmp(argv[i], "--nostd") == 0) { nostd = 1; emit_obj = 1; } /* freestanding -> produce .obj */
        else if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "win64") == 0) arch = ARCH_X86_64_WIN;
            else if (strcmp(t, "elf64") == 0) arch = ARCH_X86_64_SYSV;
            else if (strcmp(t, "arm64") == 0) arch = ARCH_ARM64_LINUX;
            else { fprintf(stderr, "error: unknown target '%s' (supported: win64, elf64, arm64)\n", t); return 1; }
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (argv[i][0] != '-') input = argv[i];
    }
    if (!input) { fprintf(stderr, "error: no input file\n"); return 1; }
    /* elf64/arm64 objects cannot be linked into a Windows exe here: stop at the .o
     * (link on a Linux system, e.g. `gcc file.o -o file`) */
    if (arch == ARCH_X86_64_SYSV || arch == ARCH_ARM64_LINUX) emit_obj = 1;

    /* derive the various output file names from the base name */
    char base[PATHBUF];
    if (!base_name(input, base)) return 1;
    char asm_path[PATHBUF + 8], obj_path[PATHBUF + 8], exe_path[PATHBUF + 8];
    snprintf(asm_path, sizeof(asm_path), arch == ARCH_ARM64_LINUX ? "%s.s" : "%s.asm", base);
    snprintf(obj_path, sizeof(obj_path), arch == ARCH_X86_64_WIN ? "%s.obj" : "%s.o", base);
    if (output) snprintf(exe_path, sizeof(exe_path), "%s", output);
    else        snprintf(exe_path, sizeof(exe_path), "%s.exe", base);

    /* locate the standard library folder: prefer the MVS_STD env var, else next to mvs.exe */
    char stddir[PATHBUF + 8];
    const char *env_std = getenv("MVS_STD");
    if (env_std) {
        snprintf(stddir, sizeof(stddir), "%s", env_std);
    } else {
        char exedir[1024];
        dir_name(argv[0], exedir);
        snprintf(stddir, sizeof(stddir), "%s/std", exedir);
    }

    /* Target identity for @compile(target_os/target_arch) filtering in the loader */
    const char *target_os = (arch == ARCH_X86_64_WIN) ? "windows" : "linux";
    const char *target_arch = (arch == ARCH_ARM64_LINUX) ? "aarch64" : "x86_64";

    /* 1+2. load the entry file + resolve all imports, then parse into one merged AST */
    diag_set_primary(input);   /* warnings are only emitted for the entry file's own code */
    int had_error = 0;
    Node *program = module_load(input, stddir, nostd, target_os, target_arch, &had_error);
    if (had_error) { fprintf(stderr, "compilation failed (parse/import errors)\n"); return 1; }

    /* monomorphization: turn generic functions into instances for the types actually called,
     * followed by overload resolution (including generic instances that may call overloads) */
    if (check_duplicates(program) > 0) {   /* duplicate names (struct/trait/func) -> error right away */
        fprintf(stderr, "compilation failed (duplicate definitions)\n");
        return 1;
    }
    fill_default_args(program);        /* append declared default values to calls that omit them */
    if (monomorphize(program) > 0) {   /* checks trait bounds during instantiation */
        fprintf(stderr, "compilation failed (trait bound errors)\n");
        return 1;
    }
    resolve_overloads(program);

    /* compile-time type check (catches type mismatches such as 50 + "50") before codegen */
    if (typecheck(program) > 0) {
        fprintf(stderr, "compilation failed (type errors)\n");
        return 1;
    }

    /* 3. codegen -> .asm */
    if (codegen_generate(program, asm_path, arch) != 0) {
        fprintf(stderr, "compilation failed (codegen errors)\n");
        return 1;
    }
    printf("[mvs] generated %s\n", asm_path);
    if (only_asm) return 0; /* -S: stop at the assembly file */

    /* 4. assemble into an object file (nasm for x86-64; a GNU-as-compatible driver for arm64) */
    char cmd[PATHBUF * 3], ver[256];
    if (arch == ARCH_ARM64_LINUX) {
        const char *as_tool = NULL;
        if (tool_version("aarch64-linux-gnu-gcc", ver, sizeof(ver))) as_tool = "aarch64-linux-gnu-gcc -c";
        else if (tool_version("clang", ver, sizeof(ver))) as_tool = "clang --target=aarch64-linux-gnu -c";
        else {
            fprintf(stderr, "error: no AArch64 assembler found.\n"
                            "       Install aarch64-linux-gnu-gcc (Linux) or clang (any platform).\n");
            return 1;
        }
        printf("[mvs] using %s\n", ver);
        snprintf(cmd, sizeof(cmd), "%s \"%s\" -o \"%s\"", as_tool, asm_path, obj_path);
        printf("[mvs] %s\n", cmd);
        if (system(cmd) != 0) { fprintf(stderr, "error: assembler failed\n"); return 1; }
    } else {
        if (!tool_version("nasm", ver, sizeof(ver))) {
            fprintf(stderr, "error: 'nasm' not found. MVS needs the NASM assembler.\n"
                            "       Install it from https://www.nasm.us and make sure it is on your PATH.\n");
            return 1;
        }
        printf("[mvs] using %s\n", ver);
        snprintf(cmd, sizeof(cmd), "nasm -f %s \"%s\" -o \"%s\"",
                 arch == ARCH_X86_64_SYSV ? "elf64" : "win64", asm_path, obj_path);
        printf("[mvs] %s\n", cmd);
        if (system(cmd) != 0) { fprintf(stderr, "error: nasm failed\n"); return 1; }
    }

    /* -c / --nostd / elf64 mode: stop at the object file */
    if (emit_obj || nostd) {
        printf("[mvs] produced object file %s%s\n", obj_path,
               nostd ? " (freestanding, no std/CRT)" :
               arch == ARCH_X86_64_SYSV ? " (ELF64; link on Linux, e.g. gcc file.o -o file)" :
               arch == ARCH_ARM64_LINUX ? " (AArch64; link with aarch64-linux-gnu-gcc, run with qemu-aarch64)" : "");
        if (!keep) remove(asm_path);
        return 0;
    }

    /* 5. link with clang (acting as the linker driver, bringing in the C runtime)
     *    - legacy_stdio_definitions: provides real printf/scanf/... symbols (UCRT inlines them)
     *    - ws2_32: Winsock for the net module (always linked; harmless for programs not using it) */
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

    /* delete intermediate files unless asked to keep them */
    if (!keep) { remove(asm_path); remove(obj_path); }
    return 0;
}
