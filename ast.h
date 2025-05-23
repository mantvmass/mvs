#ifndef AST_H
#define AST_H

typedef enum {
    NODE_NUMBER, NODE_BINOP, NODE_VAR_DECL, NODE_VAR_REF,
    NODE_STRUCT_DECL, NODE_STRUCT_FIELD, NODE_POINTER, NODE_DEREF, NODE_ADDRESS,
    NODE_PROGRAM, NODE_CONST_DECL, NODE_MEMBER_ACCESS, NODE_STRING
} NodeType;

typedef enum {
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64, TYPE_I128, TYPE_ISIZE,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64, TYPE_U128, TYPE_USIZE,
    TYPE_BOOL, TYPE_VOID, TYPE_STR, TYPE_CHAR, TYPE_F32, TYPE_F64, TYPE_STRUCT
} DataType;

typedef struct StructField {
    char *name;
    DataType type;
    struct StructField *next;
} StructField;

typedef struct ASTNode {
    NodeType type;
    union {
        int value;           // For NODE_NUMBER
        char op;             // For NODE_BINOP
        char *var_name;      // For NODE_VAR_DECL, NODE_VAR_REF, NODE_CONST_DECL
        char *struct_name;   // For NODE_STRUCT_DECL, struct initialization
        char *field_name;    // For NODE_MEMBER_ACCESS
        char *string_value;  // For NODE_STRING
    };
    DataType data_type;      // Type of the node
    struct ASTNode *left;    // Left child (for binary operations, pointers, init values, member access)
    struct ASTNode *right;   // Right child (for binary operations)
    StructField *fields;     // For struct fields
    int is_global;           // Flag for global variables
} ASTNode;

#endif
