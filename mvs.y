%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

extern int yylineno;
extern char *yytext;

void yyerror(const char *s) { 
    fprintf(stderr, "Error at line %d: %s (near '%s')\n", yylineno, s, yytext); 
}
int yylex();
ASTNode *create_number_node(int value, DataType type);
ASTNode *create_string_node(char *value);
ASTNode *create_binop_node(char op, ASTNode *left, ASTNode *right);
ASTNode *create_var_decl_node(char *name, DataType type, ASTNode *init, int is_global);
ASTNode *create_const_decl_node(char *name, DataType type, ASTNode *init, int is_global);
ASTNode *create_var_ref_node(char *name, DataType type);
ASTNode *create_struct_decl_node(char *name, StructField *fields);
ASTNode *create_struct_init_node(char *struct_name, ASTNode *init_values);
ASTNode *create_pointer_node(ASTNode *child, DataType type);
ASTNode *create_deref_node(ASTNode *child);
ASTNode *create_address_node(ASTNode *child);
ASTNode *create_member_access_node(ASTNode *base, char *field);
StructField *create_struct_field(char *name, DataType type);
int check_types(ASTNode *left, ASTNode *right, char op);
extern ASTNode *root;
%}

%union {
    int num;
    char *str;
    char ch;
    DataType data_type;
    ASTNode *node;
    StructField *field;
}

%token FUNC MAIN ARROW RETURN PLUS MINUS MULT DIV POW MOD ADDRESS DOT
%token LPAREN RPAREN LBRACE RBRACE SEMICOLON COLON COMMA
%token LET CONST STRUCT
%token I8 I16 I32 I64 I128 ISIZE U8 U16 U32 U64 U128 USIZE
%token BOOL VOID STR CHAR F32 F64
%token <num> NUMBER
%token <str> STRING IDENTIFIER
%token <ch> CHARACTER

%type <node> expr stmt program var_decl const_decl struct_decl stmt_list struct_init init_list init_value decl_list decl
%type <field> field_list field
%type <data_type> type pointer_type

%left PLUS MINUS
%left MULT DIV MOD
%right POW
%right ADDRESS
%left DOT

%%

program: decl_list FUNC MAIN LPAREN RPAREN ARROW type LBRACE stmt_list RBRACE { 
           ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
           node->type = NODE_PROGRAM;
           node->left = $1;
           node->right = $9;
           node->data_type = $7;
           root = node;
         }
       | FUNC MAIN LPAREN RPAREN ARROW type LBRACE stmt_list RBRACE { 
           root = $8; 
           root->data_type = $6;
         }
       ;

decl_list: /* empty */ { $$ = NULL; }
         | decl_list decl { $$ = $2; $2->left = $1; }
         ;

decl: struct_decl SEMICOLON { $$ = $1; }
    | LET IDENTIFIER COLON type '=' expr SEMICOLON { $$ = create_var_decl_node($2, $4, $6, 1); }
    | LET IDENTIFIER COLON pointer_type '=' expr SEMICOLON { $$ = create_var_decl_node($2, $4, $6, 1); }
    | LET IDENTIFIER COLON IDENTIFIER '=' struct_init SEMICOLON { $$ = create_var_decl_node($2, TYPE_STRUCT, $6, 1); }
    | CONST IDENTIFIER COLON type '=' expr SEMICOLON { $$ = create_const_decl_node($2, $4, $6, 1); }
    | CONST IDENTIFIER COLON pointer_type '=' expr SEMICOLON { $$ = create_const_decl_node($2, $4, $6, 1); }
    | CONST IDENTIFIER COLON IDENTIFIER '=' struct_init SEMICOLON { $$ = create_const_decl_node($2, TYPE_STRUCT, $6, 1); }
    ;

stmt_list: stmt                { $$ = $1; }
         | stmt_list stmt      { $$ = $2; /* Use last statement for return value */ }
         ;

stmt: var_decl SEMICOLON       { $$ = $1; }
    | const_decl SEMICOLON     { $$ = $1; }
    | struct_decl SEMICOLON    { $$ = $1; }
    | RETURN expr SEMICOLON    { $$ = $2; }
    ;

var_decl: LET IDENTIFIER COLON type                { $$ = create_var_decl_node($2, $4, NULL, 0); }
        | LET IDENTIFIER COLON type '=' expr        { $$ = create_var_decl_node($2, $4, $6, 0); }
        | LET IDENTIFIER COLON pointer_type '=' expr { $$ = create_var_decl_node($2, $4, $6, 0); }
        | LET IDENTIFIER COLON IDENTIFIER '=' struct_init { $$ = create_var_decl_node($2, TYPE_STRUCT, $6, 0); }
        ;

const_decl: CONST IDENTIFIER COLON type '=' expr        { $$ = create_const_decl_node($2, $4, $6, 0); }
          | CONST IDENTIFIER COLON pointer_type '=' expr { $$ = create_const_decl_node($2, $4, $6, 0); }
          | CONST IDENTIFIER COLON IDENTIFIER '=' struct_init { $$ = create_const_decl_node($2, TYPE_STRUCT, $6, 0); }
          ;

struct_decl: STRUCT IDENTIFIER LBRACE field_list RBRACE { $$ = create_struct_decl_node($2, $4); }
           ;

field_list: field                  { $$ = $1; }
          | field_list COMMA field { $$ = $1; $1->next = $3; }
          ;

field: IDENTIFIER COLON type { $$ = create_struct_field($1, $3); }
     | IDENTIFIER COLON IDENTIFIER { $$ = create_struct_field($1, TYPE_STRUCT); } /* Nested struct */
     ;

type: I8      { $$ = TYPE_I8; }
    | I16     { $$ = TYPE_I16; }
    | I32     { $$ = TYPE_I32; }
    | I64     { $$ = TYPE_I64; }
    | I128    { $$ = TYPE_I128; }
    | ISIZE   { $$ = TYPE_ISIZE; }
    | U8      { $$ = TYPE_U8; }
    | U16     { $$ = TYPE_U16; }
    | U32     { $$ = TYPE_U32; }
    | U64     { $$ = TYPE_U64; }
    | U128    { $$ = TYPE_U128; }
    | USIZE   { $$ = TYPE_USIZE; }
    | BOOL    { $$ = TYPE_BOOL; }
    | VOID    { $$ = TYPE_VOID; }
    | STR     { $$ = TYPE_STR; }
    | CHAR    { $$ = TYPE_CHAR; }
    | F32     { $$ = TYPE_F32; }
    | F64     { $$ = TYPE_F64; }
    ;

pointer_type: MULT type { $$ = $2; /* Pointer to the base type */ }
            ;

struct_init: IDENTIFIER LBRACE init_list RBRACE { $$ = create_struct_init_node($1, $3); }
           ;

init_list: init_value                    { $$ = $1; }
         | init_list COMMA init_value    { $$ = $1; $1->left = $3; /* Chain init values */ }
         ;

init_value: IDENTIFIER COLON expr { $$ = $3; /* Store expression for field */ }
          ;

expr: NUMBER                { $$ = create_number_node($1, TYPE_I32); }
    | STRING                { $$ = create_string_node($1); }
    | CHARACTER             { $$ = create_number_node($1, TYPE_CHAR); }
    | IDENTIFIER            { $$ = create_var_ref_node($1, TYPE_I32); /* Type to be resolved */ }
    | IDENTIFIER DOT IDENTIFIER { $$ = create_member_access_node(create_var_ref_node($1, TYPE_STRUCT), $3); }
    | expr DOT IDENTIFIER   { $$ = create_member_access_node($1, $3); } /* Nested access */
    | expr PLUS expr        { if (check_types($1, $3, '+')) $$ = create_binop_node('+', $1, $3); else yyerror("Type mismatch in +"); }
    | expr MINUS expr       { if (check_types($1, $3, '-')) $$ = create_binop_node('-', $1, $3); else yyerror("Type mismatch in -"); }
    | expr MULT expr        { if (check_types($1, $3, '*')) $$ = create_binop_node('*', $1, $3); else yyerror("Type mismatch in *"); }
    | expr DIV expr         { if (check_types($1, $3, '/')) $$ = create_binop_node('/', $1, $3); else yyerror("Type mismatch in /"); }
    | expr POW expr         { if (check_types($1, $3, '^')) $$ = create_binop_node('^', $1, $3); else yyerror("Type mismatch in ^"); }
    | expr MOD expr         { if (check_types($1, $3, '%')) $$ = create_binop_node('%', $1, $3); else yyerror("Type mismatch in %"); }
    | MULT expr %prec ADDRESS { $$ = create_deref_node($2); }
    | ADDRESS expr           { $$ = create_address_node($2); }
    | LPAREN expr RPAREN    { $$ = $2; }
    ;

%%

ASTNode *root = NULL;

ASTNode *create_number_node(int value, DataType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_NUMBER;
    node->value = value;
    node->data_type = type;
    node->left = NULL;
    node->right = NULL;
    return node;
}

ASTNode *create_string_node(char *value) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_STRING;
    node->string_value = value;
    node->data_type = TYPE_STR;
    node->left = NULL;
    node->right = NULL;
    return node;
}

ASTNode *create_binop_node(char op, ASTNode *left, ASTNode *right) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_BINOP;
    node->op = op;
    node->data_type = left->data_type; /* Inherit type from left operand */
    node->left = left;
    node->right = right;
    return node;
}

ASTNode *create_var_decl_node(char *name, DataType type, ASTNode *init, int is_global) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_VAR_DECL;
    node->var_name = name;
    node->data_type = type;
    node->left = init;
    node->right = NULL;
    node->is_global = is_global;
    return node;
}

ASTNode *create_const_decl_node(char *name, DataType type, ASTNode *init, int is_global) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_CONST_DECL;
    node->var_name = name;
    node->data_type = type;
    node->left = init;
    node->right = NULL;
    node->is_global = is_global;
    return node;
}

ASTNode *create_var_ref_node(char *name, DataType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_VAR_REF;
    node->var_name = name;
    node->data_type = type;
    node->left = NULL;
    node->right = NULL;
    return node;
}

ASTNode *create_struct_decl_node(char *name, StructField *fields) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_STRUCT_DECL;
    node->struct_name = name;
    node->data_type = TYPE_STRUCT;
    node->fields = fields;
    node->left = NULL;
    node->right = NULL;
    return node;
}

ASTNode *create_struct_init_node(char *struct_name, ASTNode *init_values) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_VAR_DECL; /* Treat as variable initialization */
    node->struct_name = struct_name;
    node->data_type = TYPE_STRUCT;
    node->left = init_values;
    node->right = NULL;
    return node;
}

ASTNode *create_member_access_node(ASTNode *base, char *field) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_MEMBER_ACCESS;
    node->field_name = field;
    node->data_type = TYPE_I32; /* To be resolved in codegen */
    node->left = base;
    node->right = NULL;
    return node;
}

StructField *create_struct_field(char *name, DataType type) {
    StructField *field = (StructField *)malloc(sizeof(StructField));
    field->name = name;
    field->type = type;
    field->next = NULL;
    return field;
}

ASTNode *create_pointer_node(ASTNode *child, DataType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_POINTER;
    node->data_type = type;
    node->left = child;
    node->right = NULL;
    return node;
}

ASTNode *create_deref_node(ASTNode *child) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_DEREF;
    node->data_type = child->data_type; /* Type of the pointed-to value */
    node->left = child;
    node->right = NULL;
    return node;
}

ASTNode *create_address_node(ASTNode *child) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->type = NODE_ADDRESS;
    node->data_type = child->data_type; /* Type of the pointed-to value */
    node->left = child;
    node->right = NULL;
    return node;
}

int check_types(ASTNode *left, ASTNode *right, char op) {
    if (left->data_type != right->data_type) return 0;
    if (op == '/' && right->type == NODE_NUMBER && right->value == 0) {
        yyerror("Division by zero");
        return 0;
    }
    if (left->data_type == TYPE_STR || left->data_type == TYPE_VOID || left->data_type == TYPE_STRUCT) {
        return 0; /* No arithmetic on str, void, struct */
    }
    return 1;
}
