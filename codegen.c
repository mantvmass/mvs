#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "mvs.tab.h"

extern ASTNode *root;
extern int yyparse(void);

// Function to free AST
void free_ast(ASTNode *node) {
    if (node == NULL) return;
    if (node->type == NODE_BINOP) {
        free_ast(node->left);
        free_ast(node->right);
    }
    free(node);
}

LLVMValueRef codegen(LLVMModuleRef module, LLVMBuilderRef builder, ASTNode *node) {
    if (node->type == NODE_NUMBER) {
        return LLVMConstInt(LLVMInt8Type(), node->value, 1);
    } else if (node->type == NODE_BINOP) {
        LLVMValueRef left = codegen(module, builder, node->left);
        LLVMValueRef right = codegen(module, builder, node->right);
        if (left == NULL || right == NULL) return NULL;
        switch (node->op) {
            case '+': return LLVMBuildAdd(builder, left, right, "add");
            case '-': return LLVMBuildSub(builder, left, right, "sub");
            case '*': return LLVMBuildMul(builder, left, right, "mul");
            case '/': return LLVMBuildSDiv(builder, left, right, "div");
            case '%': return LLVMBuildSRem(builder, left, right, "mod");
            case '^': {
                LLVMTypeRef param_types[] = { LLVMInt8Type(), LLVMInt8Type() };
                LLVMTypeRef pow_type = LLVMFunctionType(LLVMInt8Type(), param_types, 2, 0);
                LLVMValueRef pow_func = LLVMAddFunction(module, "pow", pow_type);
                LLVMValueRef args[] = { left, right };
                return LLVMBuildCall2(builder, pow_type, pow_func, args, 2, "pow");
            }
        }
    }
    return NULL;
}

int main() {
    // Parse input
    if (yyparse() != 0) {
        fprintf(stderr, "Parsing failed\n");
        return 1;
    }

    // Check if AST was generated
    if (root == NULL) {
        fprintf(stderr, "No AST generated\n");
        return 1;
    }

    // Initialize LLVM
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    LLVMModuleRef module = LLVMModuleCreateWithName("mvs_module");
    LLVMBuilderRef builder = LLVMCreateBuilder();

    // Define i8 main() function
    LLVMTypeRef returnType = LLVMInt8Type();
    LLVMTypeRef mainType = LLVMFunctionType(returnType, NULL, 0, 0);
    LLVMValueRef mainFunc = LLVMAddFunction(module, "main", mainType);

    // Create basic block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(mainFunc, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);

    // Generate code for AST
    LLVMValueRef result = codegen(module, builder, root);
    if (result == NULL) {
        fprintf(stderr, "Codegen failed\n");
        free_ast(root);
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        return 1;
    }
    LLVMBuildRet(builder, result);

    // Write object file
    LLVMTargetRef target;
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMGetTargetFromTriple(triple, &target, NULL);
    LLVMTargetMachineRef targetMachine = LLVMCreateTargetMachine(
        target, triple, "", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
    
    char *error = NULL;
    if (LLVMTargetMachineEmitToFile(targetMachine, module, "output.o", LLVMObjectFile, &error)) {
        fprintf(stderr, "Failed to emit object file: %s\n", error);
        LLVMDisposeMessage(error);
        free_ast(root);
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        LLVMDisposeTargetMachine(targetMachine);
        LLVMDisposeMessage(triple);
        return 1;
    }

    // Cleanup LLVM resources
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMDisposeTargetMachine(targetMachine);
    LLVMDisposeMessage(triple);

    // Link object file to executable
    printf("Linking output.o to mvs_program...\n");
    if (system("gcc output.o -o mvs_program -lm") != 0) {
        fprintf(stderr, "Linking failed: gcc output.o -o mvs_program -lm\n");
        free_ast(root);
        return 1;
    }

    // Free AST
    free_ast(root);
    return 0;
}
