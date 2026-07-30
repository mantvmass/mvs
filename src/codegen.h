/*
 * codegen.h - code generator header, the architecture-independent layer
 *
 * This driver takes the AST and selects a backend for the target architecture.
 * Adding a new architecture later (e.g. ARM64) means adding a TargetArch value
 * and a new backend file under arch/, without touching the lexer/parser.
 */
#ifndef MVS_CODEGEN_H
#define MVS_CODEGEN_H

#include "ast.h"

/* Supported target architectures */
typedef enum {
    ARCH_X86_64_WIN,  /* x86-64 on Windows (output is NASM syntax, win64 ABI) */
    ARCH_X86_64_SYSV  /* x86-64 Linux/ELF (NASM elf64, System V AMD64 ABI) */
    /* future: ARCH_ARM64, ... */
} TargetArch;

/* Generate an assembly file from the AST, written to asm_path.
 * Returns 0 on success, nonzero on error */
int codegen_generate(Node *program, const char *asm_path, TargetArch arch);

#endif /* MVS_CODEGEN_H */
