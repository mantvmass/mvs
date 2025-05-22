%{
   #include <stdio.h>
   #include <stdlib.h>
   #include <math.h>
   #include "ast.h"
   
   void yyerror(const char *s) { fprintf(stderr, "Error: %s\n", s); }
   int yylex();
   
   ASTNode *create_number_node(int value);
   ASTNode *create_binop_node(char op, ASTNode *left, ASTNode *right);
   extern ASTNode *root;
   %}
   
   %union {
       int num;
       ASTNode *node;
   }
   
   %token FUNC MAIN ARROW I8 RETURN PLUS MINUS MULT DIV POW MOD
   %token LPAREN RPAREN LBRACE RBRACE SEMICOLON
   %token <num> NUMBER
   
   %type <node> expr stmt program
   
   %left PLUS MINUS
   %left MULT DIV MOD
   %right POW
   
   %%
   
   program: FUNC MAIN LPAREN RPAREN ARROW I8 LBRACE stmt RBRACE { root = $8; }
          ;
   
   stmt: RETURN expr SEMICOLON { $$ = $2; }
       ;
   
   expr: NUMBER                { $$ = create_number_node($1); }
       | expr PLUS expr        { $$ = create_binop_node('+', $1, $3); }
       | expr MINUS expr       { $$ = create_binop_node('-', $1, $3); }
       | expr MULT expr        { $$ = create_binop_node('*', $1, $3); }
       | expr DIV expr         { $$ = create_binop_node('/', $1, $3); }
       | expr POW expr         { $$ = create_binop_node('^', $1, $3); }
       | expr MOD expr         { $$ = create_binop_node('%', $1, $3); }
       | LPAREN expr RPAREN    { $$ = $2; }
       ;
   
   %%
   
   ASTNode *root = NULL;
   
   ASTNode *create_number_node(int value) {
       ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
       node->type = NODE_NUMBER;
       node->value = value;
       node->left = NULL;
       node->right = NULL;
       return node;
   }
   
   ASTNode *create_binop_node(char op, ASTNode *left, ASTNode *right) {
       ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
       node->type = NODE_BINOP;
       node->op = op;
       node->left = left;
       node->right = right;
       return node;
   }
