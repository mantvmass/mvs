%{
#include "ast.h"
#include "mvs.tab.h"
%}

%option yylineno

%%

"func"          { return FUNC; }
"main"          { return MAIN; }
"->"            { return ARROW; }
"i8"            { yylval.data_type = TYPE_I8; return I8; }
"i16"           { yylval.data_type = TYPE_I16; return I16; }
"i32"           { yylval.data_type = TYPE_I32; return I32; }
"i64"           { yylval.data_type = TYPE_I64; return I64; }
"i128"          { yylval.data_type = TYPE_I128; return I128; }
"isize"         { yylval.data_type = TYPE_ISIZE; return ISIZE; }
"u8"            { yylval.data_type = TYPE_U8; return U8; }
"u16"           { yylval.data_type = TYPE_U16; return U16; }
"u32"           { yylval.data_type = TYPE_U32; return U32; }
"u64"           { yylval.data_type = TYPE_U64; return U64; }
"u128"          { yylval.data_type = TYPE_U128; return U128; }
"usize"         { yylval.data_type = TYPE_USIZE; return USIZE; }
"bool"          { yylval.data_type = TYPE_BOOL; return BOOL; }
"void"          { yylval.data_type = TYPE_VOID; return VOID; }
"str"           { yylval.data_type = TYPE_STR; return STR; }
"char"          { yylval.data_type = TYPE_CHAR; return CHAR; }
"f32"           { yylval.data_type = TYPE_F32; return F32; }
"f64"           { yylval.data_type = TYPE_F64; return F64; }
"struct"        { return STRUCT; }
"let"           { return LET; }
"const"         { return CONST; }
"return"        { return RETURN; }
[0-9]+          { yylval.num = atoi(yytext); return NUMBER; }
\"[^\"]*\"      { yylval.str = strdup(yytext); return STRING; }
\'.\'           { yylval.ch = yytext[1]; return CHARACTER; }
[a-zA-Z_][a-zA-Z0-9_]*  { yylval.str = strdup(yytext); return IDENTIFIER; }
"+"             { return PLUS; }
"-"             { return MINUS; }
"*"             { return MULT; }
"/"             { return DIV; }
"^"             { return POW; }
"%"             { return MOD; }
"&"             { return ADDRESS; }
"."             { return DOT; }
"("             { return LPAREN; }
")"             { return RPAREN; }
"{"             { return LBRACE; }
"}"             { return RBRACE; }
";"             { return SEMICOLON; }
":"             { return COLON; }
","             { return COMMA; }
"//".*          ; /* Single-line comment */
[ \t\n]         ; /* Skip whitespace */
.               { printf("Unknown character at line %d: %s\n", yylineno, yytext); }

%%

int yywrap() { return 1; }
