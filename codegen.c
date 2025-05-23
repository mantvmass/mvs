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

typedef struct Symbol {
    char *name;
    DataType type;
    LLVMValueRef value;
    struct Symbol *next;
} Symbol;

typedef struct StructInfo {
    char *name;
    LLVMTypeRef type;
    StructField *fields;
    struct StructInfo *next;
} StructInfo;

Symbol *symbol_table = NULL;
StructInfo *struct_table = NULL;

void add_symbol(char *name, DataType type, LLVMValueRef value) {
    Symbol *sym = (Symbol *)malloc(sizeof(Symbol));
    sym->name = strdup(name);
    sym->type = type;
    sym->value = value;
    sym->next = symbol_table;
    symbol_table = sym;
}

LLVMValueRef find_symbol(char *name) {
    for (Symbol *sym = symbol_table; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return sym->value;
    }
    return NULL;
}

void add_struct(char *name, LLVMTypeRef type, StructField *fields) {
    StructInfo *info = (StructInfo *)malloc(sizeof(StructInfo));
    info->name = strdup(name);
    info->type = type;
    info->fields = fields;
    info->next = struct_table;
    struct_table = info;
}

StructInfo *find_struct(char *name) {
    for (StructInfo *info = struct_table; info; info = info->next) {
        if (strcmp(info->name, name) == 0) return info;
    }
    return NULL;
}

void free_symbols() {
    Symbol *sym = symbol_table;
    while (sym) {
        Symbol *next = sym->next;
        free(sym->name);
        free(sym);
        sym = next;
    }
    symbol_table = NULL;
}

void free_structs() {
    StructInfo *info = struct_table;
    while (info) {
        StructInfo *next = info->next;
        free(info->name);
        StructField *field = info->fields;
        while (field) {
            StructField *field_next = field->next;
            free(field->name);
            free(field);
            field = field_next;
        }
        free(info);
        info = next;
    }
    struct_table = NULL;
}

void free_ast(ASTNode *node) {
    if (node == NULL) return;
    if (node->type == NODE_BINOP || node->type == NODE_VAR_DECL || node->type == NODE_PROGRAM || node->type == NODE_CONST_DECL || node->type == NODE_MEMBER_ACCESS) {
        free_ast(node->left);
        free_ast(node->right);
    } else if (node->type == NODE_STRUCT_DECL) {
        StructField *field = node->fields;
        while (field) {
            StructField *next = field->next;
            free(field->name);
            free(field);
            field = next;
        }
    } else if (node->type == NODE_POINTER || node->type == NODE_DEREF || node->type == NODE_ADDRESS) {
        free_ast(node->left);
    }
    if (node->type == NODE_VAR_DECL || node->type == NODE_VAR_REF || node->type == NODE_STRUCT_DECL || node->type == NODE_CONST_DECL) {
        free(node->var_name);
    } else if (node->type == NODE_STRING) {
        free(node->string_value);
    } else if (node->type == NODE_MEMBER_ACCESS) {
        free(node->field_name);
    }
    free(node);
}

LLVMTypeRef get_llvm_type(DataType type) {
    switch (type) {
        case TYPE_I8: return LLVMInt8Type();
        case TYPE_I16: return LLVMInt16Type();
        case TYPE_I32: return LLVMInt32Type();
        case TYPE_I64: return LLVMInt64Type();
        case TYPE_I128: return LLVMIntType(128);
        case TYPE_ISIZE: return LLVMInt64Type();
        case TYPE_U8: return LLVMInt8Type();
        case TYPE_U16: return LLVMInt16Type();
        case TYPE_U32: return LLVMInt32Type();
        case TYPE_U64: return LLVMInt64Type();
        case TYPE_U128: return LLVMIntType(128);
        case TYPE_USIZE: return LLVMInt64Type();
        case TYPE_BOOL: return LLVMInt1Type();
        case TYPE_VOID: return LLVMVoidType();
        case TYPE_CHAR: return LLVMInt8Type();
        case TYPE_F32: return LLVMFloatType();
        case TYPE_F64: return LLVMDoubleType();
        case TYPE_STR: return LLVMPointerType(LLVMInt8Type(), 0);
        case TYPE_STRUCT: return LLVMStructType(NULL, 0, 0); /* Resolved dynamically */
        default: return NULL;
    }
}

LLVMValueRef codegen(LLVMModuleRef module, LLVMBuilderRef builder, ASTNode *node) {
    if (node == NULL) return NULL;

    switch (node->type) {
        case NODE_PROGRAM: {
            /* Process global declarations */
            ASTNode *decl = node->left;
            while (decl) {
                codegen(module, builder, decl);
                decl = decl->left;
            }
            /* Process main function body */
            return codegen(module, builder, node->right);
        }
        case NODE_NUMBER: {
            if (node->data_type == TYPE_CHAR) {
                return LLVMConstInt(LLVMInt8Type(), node->value, 0);
            }
            return LLVMConstInt(get_llvm_type(node->data_type), node->value, node->data_type >= TYPE_I8 && node->data_type <= TYPE_ISIZE);
        }
        case NODE_STRING: {
            /* Create global string constant */
            char *str = node->string_value;
            int len = strlen(str) - 1; /* Exclude quotes */
            char *content = strndup(str + 1, len - 1); /* Strip quotes */
            LLVMValueRef global_str = LLVMAddGlobal(module, LLVMArrayType(LLVMInt8Type(), len), ".str");
            LLVMSetInitializer(global_str, LLVMConstString(content, len, 0));
            LLVMSetGlobalConstant(global_str, 1);
            LLVMSetLinkage(global_str, LLVMPrivateLinkage);
            free(content);
            LLVMValueRef indices[] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), 0, 0) };
            return LLVMBuildGEP2(builder, LLVMArrayType(LLVMInt8Type(), len), global_str, indices, 2, "str_ptr");
        }
        case NODE_VAR_DECL: {
            LLVMTypeRef llvm_type = node->data_type == TYPE_STRUCT ? 
                                    LLVMGetTypeByName(module, node->struct_name) : 
                                    get_llvm_type(node->data_type);
            if (!llvm_type) {
                fprintf(stderr, "Unknown type for variable %s\n", node->var_name);
                return NULL;
            }
            LLVMValueRef var;
            if (node->is_global) {
                var = LLVMAddGlobal(module, llvm_type, node->var_name);
                LLVMSetInitializer(var, LLVMConstNull(llvm_type));
                LLVMSetLinkage(var, LLVMExternalLinkage);
            } else {
                var = LLVMBuildAlloca(builder, llvm_type, node->var_name);
            }
            add_symbol(node->var_name, node->data_type, var);
            if (node->left) {
                LLVMValueRef init = codegen(module, builder, node->left);
                if (init) {
                    if (node->data_type == TYPE_STRUCT) {
                        ASTNode *init_value = node->left;
                        int index = 0;
                        while (init_value) {
                            LLVMValueRef field_val = codegen(module, builder, init_value);
                            if (field_val) {
                                LLVMValueRef indices[] = { LLVMConstInt(LLVMInt32Type(), index, 0) };
                                LLVMValueRef field_ptr = LLVMBuildGEP2(builder, llvm_type, var, indices, 1, "field_ptr");
                                LLVMBuildStore(builder, field_val, field_ptr);
                            }
                            init_value = init_value->left;
                            index++;
                        }
                    } else {
                        LLVMBuildStore(builder, init, var);
                    }
                }
            }
            return var;
        }
        case NODE_CONST_DECL: {
            LLVMTypeRef llvm_type = node->data_type == TYPE_STRUCT ? 
                                    LLVMGetTypeByName(module, node->struct_name) : 
                                    get_llvm_type(node->data_type);
            if (!llvm_type) {
                fprintf(stderr, "Unknown type for constant %s\n", node->var_name);
                return NULL;
            }
            if (!node->left) {
                fprintf(stderr, "Constant %s must have an initializer\n", node->var_name);
                return NULL;
            }
            LLVMValueRef init = codegen(module, builder, node->left);
            if (!init) return NULL;
            LLVMValueRef var;
            if (node->is_global) {
                var = LLVMAddGlobal(module, llvm_type, node->var_name);
                LLVMSetInitializer(var, init);
                LLVMSetGlobalConstant(var, 1);
                LLVMSetLinkage(var, LLVMExternalLinkage);
            } else {
                var = LLVMBuildAlloca(builder, llvm_type, node->var_name);
                LLVMBuildStore(builder, init, var);
            }
            add_symbol(node->var_name, node->data_type, var);
            return var;
        }
        case NODE_VAR_REF: {
            LLVMValueRef var = find_symbol(node->var_name);
            if (!var) {
                fprintf(stderr, "Undefined variable: %s\n", node->var_name);
                return NULL;
            }
            return LLVMBuildLoad2(builder, get_llvm_type(node->data_type), var, node->var_name);
        }
        case NODE_MEMBER_ACCESS: {
            ASTNode *base = node->left;
            char *field_name = node->field_name;
            LLVMValueRef base_val = codegen(module, builder, base);
            if (!base_val) {
                fprintf(stderr, "Invalid base for member access\n");
                return NULL;
            }
            if (base->data_type != TYPE_STRUCT) {
                fprintf(stderr, "Member access requires struct type\n");
                return NULL;
            }
            StructInfo *struct_info = find_struct(base->struct_name);
            if (!struct_info) {
                fprintf(stderr, "Struct %s not found\n", base->struct_name);
                return NULL;
            }
            StructField *field = struct_info->fields;
            int index = 0;
            while (field) {
                if (strcmp(field->name, field_name) == 0) {
                    LLVMValueRef indices[] = { LLVMConstInt(LLVMInt32Type(), 0, 0), LLVMConstInt(LLVMInt32Type(), index, 0) };
                    LLVMValueRef field_ptr = LLVMBuildGEP2(builder, struct_info->type, base_val, indices, 2, "field_ptr");
                    return LLVMBuildLoad2(builder, get_llvm_type(field->type), field_ptr, field_name);
                }
                field = field->next;
                index++;
            }
            fprintf(stderr, "Field %s not found in struct %s\n", field_name, base->struct_name);
            return NULL;
        }
        case NODE_BINOP: {
            LLVMValueRef left = codegen(module, builder, node->left);
            LLVMValueRef right = codegen(module, builder, node->right);
            if (left == NULL || right == NULL) return NULL;

            if (node->op == '/' && node->right->type == NODE_NUMBER && node->right->value == 0) {
                fprintf(stderr, "Division by zero\n");
                return NULL;
            }

            switch (node->op) {
                case '+': return node->data_type == TYPE_F32 || node->data_type == TYPE_F64 ?
                                 LLVMBuildFAdd(builder, left, right, "fadd") :
                                 LLVMBuildAdd(builder, left, right, "add");
                case '-': return node->data_type == TYPE_F32 || node->data_type == TYPE_F64 ?
                                 LLVMBuildFSub(builder, left, right, "fsub") :
                                 LLVMBuildSub(builder, left, right, "sub");
                case '*': return node->data_type == TYPE_F32 || node->data_type == TYPE_F64 ?
                                 LLVMBuildFMul(builder, left, right, "fmul") :
                                 LLVMBuildMul(builder, left, right, "mul");
                case '/': return node->data_type == TYPE_F32 || node->data_type == TYPE_F64 ?
                                 LLVMBuildFDiv(builder, left, right, "fdiv") :
                                 LLVMBuildSDiv(builder, left, right, "div");
                case '%': return LLVMBuildSRem(builder, left, right, "mod");
                case '^': {
                    LLVMBasicBlockRef entry = LLVMGetInsertBlock(builder);
                    LLVMBasicBlockRef loop_header = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_header");
                    LLVMBasicBlockRef loop_body = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_body");
                    LLVMBasicBlockRef loop_end = LLVMAppendBasicBlock(LLVMGetBasicBlockParent(entry), "pow_end");

                    LLVMValueRef base = left;
                    LLVMValueRef exp = right;
                    LLVMValueRef result_ptr = LLVMBuildAlloca(builder, get_llvm_type(node->data_type), "result");
                    LLVMValueRef counter_ptr = LLVMBuildAlloca(builder, get_llvm_type(node->data_type), "counter");
                    LLVMValueRef one = LLVMConstInt(get_llvm_type(node->data_type), 1, 1);
                    LLVMValueRef zero = LLVMConstInt(get_llvm_type(node->data_type), 0, 1);

                    LLVMBuildStore(builder, one, result_ptr);
                    LLVMBuildStore(builder, zero, counter_ptr);
                    LLVMBuildBr(builder, loop_header);

                    LLVMPositionBuilderAtEnd(builder, loop_header);
                    LLVMValueRef counter_val = LLVMBuildLoad2(builder, get_llvm_type(node->data_type), counter_ptr, "counter_val");
                    LLVMValueRef cond = LLVMBuildICmp(builder, LLVMIntSLT, counter_val, exp, "cmp");
                    LLVMBuildCondBr(builder, cond, loop_body, loop_end);

                    LLVMPositionBuilderAtEnd(builder, loop_body);
                    LLVMValueRef current_result = LLVMBuildLoad2(builder, get_llvm_type(node->data_type), result_ptr, "current_result");
                    LLVMValueRef new_result = node->data_type == TYPE_F32 || node->data_type == TYPE_F64 ?
                                             LLVMBuildFMul(builder, current_result, base, "new_result") :
                                             LLVMBuildMul(builder, current_result, base, "new_result");
                    LLVMBuildStore(builder, new_result, result_ptr);

                    LLVMValueRef current_counter = LLVMBuildLoad2(builder, get_llvm_type(node->data_type), counter_ptr, "current_counter");
                    LLVMValueRef next_counter = LLVMBuildAdd(builder, current_counter, one, "next_counter");
                    LLVMBuildStore(builder, next_counter, counter_ptr);

                    LLVMBuildBr(builder, loop_header);

                    LLVMPositionBuilderAtEnd(builder, loop_end);
                    return LLVMBuildLoad2(builder, get_llvm_type(node->data_type), result_ptr, "final_result");
                }
            }
        }
        case NODE_DEREF: {
            LLVMValueRef ptr = codegen(module, builder, node->left);
            if (!ptr) return NULL;
            return LLVMBuildLoad2(builder, get_llvm_type(node->data_type), ptr, "deref");
        }
        case NODE_ADDRESS: {
            LLVMValueRef var = codegen(module, builder, node->left);
            if (!var) return NULL;
            return var;
        }
        case NODE_STRUCT_DECL: {
            LLVMTypeRef struct_type = LLVMStructCreateNamed(LLVMGetGlobalContext(), node->struct_name);
            StructField *field = node->fields;
            int field_count = 0;
            while (field) { field_count++; field = field->next; }
            LLVMTypeRef *field_types = (LLVMTypeRef *)malloc(field_count * sizeof(LLVMTypeRef));
            field = node->fields;
            for (int i = 0; i < field_count; i++) {
                field_types[i] = field->type == TYPE_STRUCT ? 
                                 LLVMGetTypeByName(module, field->name) : 
                                 get_llvm_type(field->type);
                field = field->next;
            }
            LLVMStructSetBody(struct_type, field_types, field_count, 0);
            add_struct(node->struct_name, struct_type, node->fields);
            free(field_types);
            return NULL; /* Struct declaration doesn't return a value */
        }
        case NODE_STRUCT_FIELD: {
            fprintf(stderr, "NODE_STRUCT_FIELD should not appear in codegen\n");
            return NULL;
        }
        case NODE_POINTER: {
            LLVMValueRef child = codegen(module, builder, node->left);
            if (!child) return NULL;
            return child; /* Pointer type is resolved in codegen */
        }
    }
    return NULL;
}

int main() {
    if (yyparse() != 0) {
        fprintf(stderr, "Parsing failed\n");
        return 1;
    }

    if (root == NULL) {
        fprintf(stderr, "No AST generated\n");
        return 1;
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    LLVMModuleRef module = LLVMModuleCreateWithName("mvs_module");
    LLVMBuilderRef builder = LLVMCreateBuilder();

    LLVMTypeRef returnType = get_llvm_type(root->data_type);
    LLVMTypeRef mainType = LLVMFunctionType(returnType, NULL, 0, 0);
    LLVMValueRef mainFunc = LLVMAddFunction(module, "main", mainType);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(mainFunc, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);

    LLVMValueRef result = codegen(module, builder, root);
    if (result == NULL && root->type != NODE_STRUCT_DECL && root->type != NODE_PROGRAM) {
        fprintf(stderr, "Codegen failed\n");
        free_ast(root);
        free_symbols();
        free_structs();
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        return 1;
    }
    if (result) LLVMBuildRet(builder, result);

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
        free_symbols();
        free_structs();
        LLVMDisposeBuilder(builder);
        LLVMDisposeModule(module);
        LLVMDisposeTargetMachine(targetMachine);
        LLVMDisposeMessage(triple);
        return 1;
    }

    printf("Linking output.o to mvs_program...\n");
    if (system("gcc output.o -o mvs_program") != 0) {
        fprintf(stderr, "Linking failed: gcc output.o -o mvs_program\n");
        free_ast(root);
        free_symbols();
        free_structs();
        return 1;
    }

    free_ast(root);
    free_symbols();
    free_structs();
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMDisposeTargetMachine(targetMachine);
    LLVMDisposeMessage(triple);
    return 0;
}
