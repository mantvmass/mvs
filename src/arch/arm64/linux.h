/*
 * arch/arm64/linux.h - header of the AArch64 Linux (AAPCS64) backend
 * The driver in codegen.c calls this function when ARCH_ARM64_LINUX is selected.
 */
#ifndef MVS_ARCH_ARM64_LINUX_H
#define MVS_ARCH_ARM64_LINUX_H

#include <stdio.h>
#include "../../ast.h"

/* Generate AArch64 GNU-assembler output from the AST; returns 0 on success. */
int arm64_linux_generate(Node *program, FILE *out);

#endif /* MVS_ARCH_ARM64_LINUX_H */
