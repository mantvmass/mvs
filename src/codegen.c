/*
 * codegen.c - code generation driver; picks the backend for the target architecture
 *
 * This layer is intentionally very thin: its only job is to open the output file
 * and hand the AST to the right backend, keeping the rest of the compiler
 * unbound from any particular architecture.
 */
#include <stdio.h>
#include "codegen.h"
#include "arch/x86_64/win.h"
#include "arch/x86_64/sysv.h"

int codegen_generate(Node *program, const char *asm_path, TargetArch arch) {
    /* open the file for writing assembly */
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
        case ARCH_X86_64_SYSV:
            rc = x86_64_sysv_generate(program, out);
            break;
        default:
            fprintf(stderr, "error: unsupported target architecture\n");
            rc = 1;
            break;
    }

    fclose(out);
    return rc;
}
