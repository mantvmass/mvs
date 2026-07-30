/*
 * arch/x86_64/sysv.h - header of the x86-64 Linux/ELF (SysV ABI) backend
 * The driver in codegen.c calls this function when ARCH_X86_64_SYSV is selected.
 */
#ifndef MVS_ARCH_X86_64_SYSV_H
#define MVS_ARCH_X86_64_SYSV_H

#include <stdio.h>
#include "../../ast.h"

/* Generate SysV/ELF assembly from the AST and write it to out; returns 0 on success. */
int x86_64_sysv_generate(Node *program, FILE *out);

#endif /* MVS_ARCH_X86_64_SYSV_H */
