#include <stdio.h>
#include "parser.h"

void generate_assembly(ASTNode *ast) {
    FILE *file = fopen("output.asm", "w");
    if (!file) {
        printf("❌ [ERROR] Failed to open output.asm\n");
        return;
    }

    printf("🔍 [CODEGEN] Generating Assembly...\n");

    fprintf(file, "section .text\n");
    fprintf(file, "global _start\n");
    fprintf(file, "_start:\n");

    if (ast->type == TOKEN_FUNC && strcmp(ast->value, "main") == 0) {
        fprintf(file, "    mov rax, 60\n");
        fprintf(file, "    xor rdi, rdi\n");
        fprintf(file, "    syscall\n");
        printf("✅ [CODEGEN] Function main() compiled successfully!\n");
    }

    fclose(file);
}
