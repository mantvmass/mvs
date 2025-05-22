%{
#include "ast.h"
#include "mvs.tab.h"
%}

%%
"func"          { return FUNC; }
"main"          { return MAIN; }
"->"            { return ARROW; }
"i8"            { return I8; }
"return"        { return RETURN; }
[0-9]+          { yylval.num = atoi(yytext); return NUMBER; }
"+"             { return PLUS; }
"-"             { return MINUS; }
"*"             { return MULT; }
"/"             { return DIV; }
"^"             { return POW; }
"%"             { return MOD; }
"("             { return LPAREN; }
")"             { return RPAREN; }
"{"             { return LBRACE; }
"}"             { return RBRACE; }
";"             { return SEMICOLON; }
[ \t\n]         ; /* skip whitespace */
.               { printf("Unknown character: %s\n", yytext); }
%%

int yywrap() { return 1; }
