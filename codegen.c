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
    // Simple power calculation for i8
    LLVMValueRef base = left;
    LLVMValueRef exp = right;

    // Create basic blocks
    LLVMBasicBlockRef entry = LLVMGetInsertBlock(builder);
    LLVMBasicBlockRef loop_header = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_header");
    LLVMBasicBlockRef loop_body = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_body");
    LLVMBasicBlockRef loop_end = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_end");

    // Initialize variables
    LLVMValueRef result_ptr = LLVMBuildAlloca(builder, LLVMInt8Type(), "result");
    LLVMValueRef counter_ptr = LLVMBuildAlloca(builder, LLVMInt8Type(), "counter");
    LLVMValueRef one = LLVMConstInt(LLVMInt8Type(), 1, 1);
    LLVMValueRef zero = LLVMConstInt(LLVMInt8Type(), 0, 1);
    
    LLVMBuildStore(builder, one, result_ptr);     // result = 1
    LLVMBuildStore(builder, zero, counter_ptr);   // counter = 0
    LLVMBuildBr(builder, loop_header);

    // Loop header - check condition
    LLVMPositionBuilderAtEnd(builder, loop_header);
    LLVMValueRef counter_val = LLVMBuildLoad2(builder, LLVMInt8Type(), counter_ptr, "counter_val");
    LLVMValueRef cond = LLVMBuildICmp(builder, LLVMIntSLT, counter_val, exp, "cmp");
    LLVMBuildCondBr(builder, cond, loop_body, loop_end);

    // Loop body - multiply and increment
    LLVMPositionBuilderAtEnd(builder, loop_body);
    LLVMValueRef current_result = LLVMBuildLoad2(builder, LLVMInt8Type(), result_ptr, "current_result");
    LLVMValueRef new_result = LLVMBuildMul(builder, current_result, base, "new_result");
    LLVMBuildStore(builder, new_result, result_ptr);
    
    LLVMValueRef current_counter = LLVMBuildLoad2(builder, LLVMInt8Type(), counter_ptr, "current_counter");
    LLVMValueRef next_counter = LLVMBuildAdd(builder, current_counter, one, "next_counter");
    LLVMBuildStore(builder, next_counter, counter_ptr);
    
    LLVMBuildBr(builder, loop_header);

    // Loop end - return result
    LLVMPositionBuilderAtEnd(builder, loop_end);
    return LLVMBuildLoad2(builder, LLVMInt8Type(), result_ptr, "final_result");
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
    if (system("gcc output.o -o mvs_program") != 0) {
        fprintf(stderr, "Linking failed: gcc output.o -o mvs_program\n");
        free_ast(root);
        return 1;
    }

    // Free AST
    free_ast(root);
    return 0;
}
