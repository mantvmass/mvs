#include <stdio.h>
#include <stdlib.h>
#include "codegen.h"

#define MAX_SOURCE_SIZE 10000

// อ่านซอร์สโค้ดจากไฟล์
char *read_source_code(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("❌ [ERROR] ไม่สามารถเปิดไฟล์: %s\n", filename);
        return NULL;
    }
    char *buffer = (char *)malloc(MAX_SOURCE_SIZE);
    size_t length = fread(buffer, 1, MAX_SOURCE_SIZE - 1, file);
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("❌ [ERROR] วิธีใช้: %s <source_file.mvs>\n", argv[0]);
        return 1;
    }

    char *source = read_source_code(argv[1]);
    if (!source) return 1;

    printf("🚀 [COMPILER] เริ่มคอมไพล์ MVS...\n");
    ASTNode *ast = parse(source);
    free(source);

    if (!ast) {
        printf("❌ [ERROR] การแยกวิเคราะห์ล้มเหลว\n");
        return 1;
    }

    generate_assembly(ast);
    free_ast(ast);

    printf("✅ [COMPILER] คอมไพล์เสร็จสมบูรณ์\n");
    return 0;
}