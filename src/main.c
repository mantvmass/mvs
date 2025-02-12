#include <stdio.h>
#include <stdlib.h>
#include "codegen.h"

#define MAX_SOURCE_SIZE 10000  // กำหนดขนาดไฟล์สูงสุด

char *read_source_code(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("❌ [ERROR] Unable to open source file: %s\n", filename);
        return NULL;
    }

    char *buffer = (char *)malloc(MAX_SOURCE_SIZE);
    if (!buffer) {
        printf("❌ [ERROR] Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    size_t length = fread(buffer, 1, MAX_SOURCE_SIZE - 1, file);
    buffer[length] = '\0';  // ใส่ Null Terminator
    fclose(file);

    return buffer;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("❌ [ERROR] Usage: %s <source_file.mvs>\n", argv[0]);
        return 1;
    }

    char *source_code = read_source_code(argv[1]);
    if (!source_code) {
        return 1;
    }

    printf("🚀 [COMPILER] Starting MVS Compiler...\n");

    lex(source_code);
    free(source_code);  // ✅ ป้องกัน Memory Leak

    int pos = 0;
    ASTNode *ast = parse_expression(&pos);

    if (ast == NULL) {
        printf("❌ [ERROR] Failed to generate AST\n");
        return 1;
    }

    generate_assembly(ast);

    printf("✅ [COMPILER] Compilation finished. Assembly output written to output.asm\n");

    return 0;
}
