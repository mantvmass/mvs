/*
 * arch/x86_64/win.h - header of the x86-64 Windows backend
 * The driver in codegen.c calls this function when the ARCH_X86_64_WIN architecture is selected.
 */
#ifndef MVS_ARCH_X86_64_WIN_H
#define MVS_ARCH_X86_64_WIN_H

#include <stdio.h>
#include "../../ast.h"

/* Generate assembly from the AST and write it to out; returns 0 on success. */
int x86_64_win_generate(Node *program, FILE *out);

#endif /* MVS_ARCH_X86_64_WIN_H */
