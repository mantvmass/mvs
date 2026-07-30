/*
 * parser.c - Recursive descent parser for the MVS language
 *
 * Top-level grammar (basic subset):
 *   program  := topdecl*
 *   topdecl  := func_def | var_decl ';'        (var_decl at global level)
 *   func_def := 'func' IDENT '(' params? ')' '->' type block
 *
 * Operator precedence (low to high):
 *   assignment -> '||' -> '&&' -> ('=='|'!=') -> comparison ->
 *   ('+'|'-') -> ('*'|'/'|'%') -> '^' -> unary -> postfix -> primary
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ---------- Basic parser helper functions ---------- */

/* Move to the next token, replacing the current one (do not free the lexeme; the AST may still reference it) */
static void advance(Parser *p) {
    p->cur = lexer_next(p->lx);
}

/* Check whether the current token is of the given kind */
static int check(Parser *p, TokenType t) {
    return p->cur.type == t;
}

/* If the current token matches the kind, consume it and return 1; otherwise return 0 */
static int match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return 1; }
    return 0;
}

/* Report a syntax error with its location */
static void error(Parser *p, const char *msg) {
    if (p->had_error) return; /* report only the first error to reduce noise */
    fprintf(stderr, "%s:%d:%d: syntax error: %s (near '%s')\n",
            p->lx->filename, p->cur.line, p->cur.col, msg, p->cur.lexeme);
    p->had_error = 1;
}

/* Require that the current token is of the given kind, otherwise error */
static void expect(Parser *p, TokenType t, const char *what) {
    if (!match(p, t)) error(p, what);
}

/* Check whether the current token is a data type keyword */
static int is_type_token(TokenType t) {
    return datatype_from_token(t) != TYPE_UNKNOWN;
}

/* Forward declarations (mutual recursion) */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static DataType parse_type(Parser *p, int *ptr, char **type_name, Node **sig_out);

#define MAX_PARSE_DEPTH 300  /* guard against stack overflow from abnormally deep expressions/blocks/unary chains */

/* ---------- Expression parsing ---------- */

/* primary := INT | FLOAT | STRING | CHAR | true | false | IDENT | '(' expr ')' */
static Node *parse_primary(Parser *p) {
    Token t = p->cur;
    if (check(p, TK_INT)) {
        Node *n = node_new(ND_INT, t.line);
        n->int_val = t.int_val; n->type = TYPE_I64;
        advance(p); return n;
    }
    if (check(p, TK_FLOAT)) {
        /* Store the double's bit-pattern in int_val so codegen can load it into rax directly */
        Node *n = node_new(ND_FLOAT, t.line);
        double d = t.float_val;
        unsigned long long bits;
        memcpy(&bits, &d, sizeof(bits));
        n->int_val = (long long)bits;
        n->type = TYPE_F64;
        advance(p); return n;
    }
    if (check(p, TK_STRING)) {
        Node *n = node_new(ND_STR, t.line);
        /* A string may contain an embedded '\0' (escape \0); copy by the real length (int_val),
         * not strdup which stops at NUL */
        n->str_len = (int)t.int_val;
        n->str_val = (char *)malloc((size_t)n->str_len + 1);
        memcpy(n->str_val, t.lexeme, (size_t)n->str_len);
        n->str_val[n->str_len] = '\0';
        n->type = TYPE_STR;
        advance(p); return n;
    }
    if (check(p, TK_CHAR)) {
        Node *n = node_new(ND_CHAR, t.line);
        n->int_val = t.int_val; n->type = TYPE_CHAR;
        advance(p); return n;
    }
    if (check(p, TK_TRUE) || check(p, TK_FALSE)) {
        Node *n = node_new(ND_BOOL, t.line);
        n->int_val = check(p, TK_TRUE) ? 1 : 0; n->type = TYPE_BOOL;
        advance(p); return n;
    }
    if (check(p, TK_IDENT)) {
        char *idname = strdup(t.lexeme);
        int line = t.line;
        advance(p);
        /* If followed by '{', this is a struct literal: Name { field: expr, ... } */
        if (check(p, TK_LBRACE)) {
            Node *lit = node_new(ND_STRUCT_LIT, line);
            lit->name = idname;
            advance(p); /* consume { */
            if (!check(p, TK_RBRACE)) {
                do {
                    /* Each field is stored as ND_ASSIGN: lhs = field name, rhs = expression */
                    Node *fi = node_new(ND_ASSIGN, p->cur.line);
                    Node *fname = node_new(ND_IDENT, p->cur.line);
                    if (!check(p, TK_IDENT)) { error(p, "expected field name in struct literal"); break; }
                    fname->name = strdup(p->cur.lexeme); advance(p);
                    expect(p, TK_COLON, "expected ':' in struct literal");
                    fi->lhs = fname;
                    fi->rhs = parse_expr(p);
                    node_add_item(lit, fi);
                } while (match(p, TK_COMMA));
            }
            expect(p, TK_RBRACE, "expected '}' to close struct literal");
            return lit;
        }
        Node *n = node_new(ND_IDENT, line);
        n->name = idname; n->type = TYPE_UNKNOWN;
        return n;
    }
    if (match(p, TK_LPAREN)) {
        Node *n = parse_expr(p);
        expect(p, TK_RPAREN, "expected ')'");
        n->paren = 1;   /* remember the parentheses: (-2) ** 2 must not be re-anchored by parse_power */
        return n;
    }
    error(p, "expected expression");
    advance(p); /* consume the bad token to avoid an infinite loop */
    return node_new(ND_INT, t.line);
}

/* postfix := primary ( '.' IDENT | '(' args ')' | '++' | '--' )*
 * Supports member access, function calls, and postfix increment/decrement */
static Node *parse_postfix(Parser *p) {
    Node *node = parse_primary(p);
    for (;;) {
        if (match(p, TK_DOT) || match(p, TK_COLONCOLON)) {
            /* Member access a.b (instance) or associated function Type::func.
             * Same AST shape (ND_MEMBER); gen_call disambiguates by the base's kind at resolve time:
             * base is a struct variable -> method (inject self), base is a type/module name -> function in that ns */
            Node *m = node_new(ND_MEMBER, p->cur.line);
            m->operand = node;
            /* Member/method name: accept an identifier, and also allow 'from' (an import keyword)
             * so Rust-style names like String::from work */
            if (!check(p, TK_IDENT) && !check(p, TK_FROM)) { error(p, "expected name after '.' or '::'"); }
            m->name = strdup(p->cur.lexeme);
            advance(p);
            node = m;
        } else if (match(p, TK_LPAREN)) {
            /* Function call: callee stored in operand, arguments stored in items */
            Node *call = node_new(ND_CALL, p->cur.line);
            call->operand = node;
            if (!check(p, TK_RPAREN)) {
                do {
                    node_add_item(call, parse_expr(p));
                } while (match(p, TK_COMMA));
            }
            expect(p, TK_RPAREN, "expected ')' after arguments");
            node = call;
        } else if (check(p, TK_PLUSPLUS) || check(p, TK_MINUSMINUS)) {
            /* Postfix increment/decrement: i++ , i-- */
            Node *u = node_new(ND_UNARY, p->cur.line);
            u->op = p->cur.type;
            u->operand = node;
            advance(p);
            node = u;
        } else {
            break;
        }
    }
    return node;
}

/* unary := ('-'|'!'|'~'|'*'|'&') unary | '**' unary (= double deref) | postfix
 * '~' = bitwise NOT; '**' in prefix position means a double dereference (**ptr) */
static Node *parse_unary(Parser *p) {
    if (++p->depth > MAX_PARSE_DEPTH) {       /* guard against abnormally long unary chains (e.g. !!!...) overflowing the stack */
        if (!p->had_error) error(p, "expression nested too deeply");
        p->depth--;
        return node_new(ND_INT, p->cur.line);
    }
    Node *res;
    if (check(p, TK_STARSTAR)) {
        /* **x = *( *x )  (double deref); disambiguated from exponentiation by the prefix position */
        int line = p->cur.line;
        advance(p);
        Node *operand = parse_unary(p);
        Node *inner = node_new(ND_UNARY, line); inner->op = TK_STAR; inner->operand = operand;
        Node *outer = node_new(ND_UNARY, line); outer->op = TK_STAR; outer->operand = inner;
        res = outer;
    } else if (check(p, TK_MINUS) || check(p, TK_NOT) || check(p, TK_TILDE) ||
               check(p, TK_STAR) || check(p, TK_AMP)) {
        Node *u = node_new(ND_UNARY, p->cur.line);
        u->op = p->cur.type;
        advance(p);
        u->operand = parse_unary(p);
        res = u;
    } else {
        res = parse_postfix(p);
    }
    p->depth--;
    return res;
}

/* cast := unary ('as' type)*  Explicit type conversion, e.g. (x as i32), (i as f64).
 * Binds tighter than binary operators: a as i32 * b = (a as i32) * b */
static Node *parse_cast(Parser *p) {
    Node *node = parse_unary(p);
    while (check(p, TK_AS)) {
        Node *c = node_new(ND_CAST, p->cur.line);
        advance(p); /* consume 'as' */
        c->operand = node;
        c->type = parse_type(p, &c->ptr, &c->type_name, &c->sig);
        node = c;
    }
    return node;
}

/* power := cast ('**' power)?  Exponentiation is right-associative (2**3**2 = 2**(3**2)).
 * '**' binds tighter than prefix '-'/'~' (math convention): -2 ** 2 = -(2 ** 2) = -4,
 * while an explicit (-2) ** 2 keeps the minus inside (the paren flag blocks re-anchoring) */
static Node *parse_power(Parser *p) {
    Node *left = parse_cast(p);
    if (check(p, TK_STARSTAR)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_STARSTAR;
        advance(p);
        b->rhs = parse_power(p); /* recurse on itself to get right-associativity */
        /* re-anchor the power under any prefix -/~ chain: the chain's innermost operand
         * becomes the power's base, so the unary applies to the whole power result */
        Node **slot = &left;
        while ((*slot)->kind == ND_UNARY && !(*slot)->paren &&
               ((*slot)->op == TK_MINUS || (*slot)->op == TK_TILDE))
            slot = &(*slot)->operand;
        b->lhs = *slot;
        *slot = b;
        return left;
    }
    return left;
}

/* factor := power (('*'|'/'|'%') power)* */
static Node *parse_factor(Parser *p) {
    Node *left = parse_power(p);
    while (check(p, TK_STAR) || check(p, TK_SLASH) || check(p, TK_PERCENT)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = p->cur.type;
        advance(p);
        b->lhs = left;
        b->rhs = parse_power(p);
        left = b;
    }
    return left;
}

/* term := factor (('+'|'-') factor)* */
static Node *parse_term(Parser *p) {
    Node *left = parse_factor(p);
    while (check(p, TK_PLUS) || check(p, TK_MINUS)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = p->cur.type;
        advance(p);
        b->lhs = left;
        b->rhs = parse_factor(p);
        left = b;
    }
    return left;
}

/* shift := term (('<<'|'>>') term)*  (bit shifts sit between add/subtract and comparison) */
static Node *parse_shift(Parser *p) {
    Node *left = parse_term(p);
    while (check(p, TK_SHL) || check(p, TK_SHR)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = p->cur.type;
        advance(p);
        b->lhs = left;
        b->rhs = parse_term(p);
        left = b;
    }
    return left;
}

/* comparison := shift (('<'|'>'|'<='|'>=') shift)* */
static Node *parse_comparison(Parser *p) {
    Node *left = parse_shift(p);
    while (check(p, TK_LT) || check(p, TK_GT) || check(p, TK_LE) || check(p, TK_GE)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = p->cur.type;
        advance(p);
        b->lhs = left;
        b->rhs = parse_shift(p);
        b->type = TYPE_BOOL;
        left = b;
    }
    return left;
}

/* equality := comparison (('=='|'!=') comparison)* */
static Node *parse_equality(Parser *p) {
    Node *left = parse_comparison(p);
    while (check(p, TK_EQ) || check(p, TK_NEQ)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = p->cur.type;
        advance(p);
        b->lhs = left;
        b->rhs = parse_comparison(p);
        b->type = TYPE_BOOL;
        left = b;
    }
    return left;
}

/* bitand := equality ('&' equality)*  (bitwise AND '&' in infix position) */
static Node *parse_bitand(Parser *p) {
    Node *left = parse_equality(p);
    while (check(p, TK_AMP)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_AMP;
        advance(p);
        b->lhs = left;
        b->rhs = parse_equality(p);
        left = b;
    }
    return left;
}

/* bitxor := bitand ('^' bitand)*  (bitwise XOR sits between & and |) */
static Node *parse_bitxor(Parser *p) {
    Node *left = parse_bitand(p);
    while (check(p, TK_CARET)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_CARET;
        advance(p);
        b->lhs = left;
        b->rhs = parse_bitand(p);
        left = b;
    }
    return left;
}

/* bitor := bitxor ('|' bitxor)*  (bitwise OR) */
static Node *parse_bitor(Parser *p) {
    Node *left = parse_bitxor(p);
    while (check(p, TK_PIPE)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_PIPE;
        advance(p);
        b->lhs = left;
        b->rhs = parse_bitxor(p);
        left = b;
    }
    return left;
}

/* logic_and := bitor ('&&' bitor)* */
static Node *parse_logic_and(Parser *p) {
    Node *left = parse_bitor(p);
    while (check(p, TK_AND)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_AND;
        advance(p);
        b->lhs = left;
        b->rhs = parse_bitor(p);
        b->type = TYPE_BOOL;
        left = b;
    }
    return left;
}

/* logic_or := logic_and ('||' logic_and)* */
static Node *parse_logic_or(Parser *p) {
    Node *left = parse_logic_and(p);
    while (check(p, TK_OR)) {
        Node *b = node_new(ND_BINARY, p->cur.line);
        b->op = TK_OR;
        advance(p);
        b->lhs = left;
        b->rhs = parse_logic_and(p);
        b->type = TYPE_BOOL;
        left = b;
    }
    return left;
}

/* assignment := logic_or ( ('='|'+='|'-='|'*='|'/=') assignment )?
 * Assignment is right-associative and the left side must be an lvalue (checked during codegen) */
static Node *parse_assignment(Parser *p) {
    Node *left = parse_logic_or(p);
    if (check(p, TK_ASSIGN) || check(p, TK_PLUS_ASSIGN) || check(p, TK_MINUS_ASSIGN) ||
        check(p, TK_STAR_ASSIGN) || check(p, TK_SLASH_ASSIGN)) {
        Node *a = node_new(ND_ASSIGN, p->cur.line);
        a->op = p->cur.type;
        advance(p);
        a->lhs = left;
        a->rhs = parse_assignment(p);
        return a;
    }
    return left;
}

/* Entry point of expression parsing */
static Node *parse_expr(Parser *p) {
    if (++p->depth > MAX_PARSE_DEPTH) {
        if (!p->had_error) error(p, "expression nested too deeply");
        p->depth--;
        return node_new(ND_INT, p->cur.line);
    }
    Node *r = parse_assignment(p);
    p->depth--;
    return r;
}

/* ---------- Type parsing ---------- */

/* type := ('*')* (type_keyword | IDENT)
 * Leading '*' counts as pointer depth; IDENT = struct name.
 * Stores pointer depth into *ptr and the struct name into *type_name (NULL if not a struct) */
static DataType parse_type(Parser *p, int *ptr, char **type_name, Node **sig_out) {
    int depth = 0;
    /* Count pointer depth: '*' = 1 level, '**' (a single token) = 2 levels */
    for (;;) {
        if (match(p, TK_STAR)) depth++;
        else if (match(p, TK_STARSTAR)) depth += 2;
        else break;
    }
    *ptr = depth;
    *type_name = NULL;
    if (sig_out) *sig_out = NULL;
    /* Function pointer type:  func ( T, T, ... ) -> R
     * The signature is stored as an ND_FUNC node (items = parameters, type/ptr/type_name = return type) */
    if (check(p, TK_FUNC)) {
        int line = p->cur.line;
        advance(p); /* consume 'func' */
        Node *sig = node_new(ND_FUNC, line);
        expect(p, TK_LPAREN, "expected '(' in function-pointer type");
        if (!check(p, TK_RPAREN)) {
            do {
                Node *par = node_new(ND_PARAM, p->cur.line);
                par->type = parse_type(p, &par->ptr, &par->type_name, &par->sig);
                node_add_item(sig, par);
            } while (match(p, TK_COMMA));
        }
        expect(p, TK_RPAREN, "expected ')' in function-pointer type");
        expect(p, TK_ARROW, "expected '->' before return type in function-pointer type");
        sig->type = parse_type(p, &sig->ptr, &sig->type_name, NULL); /* nested func-ptr return not supported */
        if (sig_out) *sig_out = sig;
        return TYPE_FUNC;
    }
    if (is_type_token(p->cur.type)) {
        DataType dt = datatype_from_token(p->cur.type);
        advance(p);
        return dt;
    }
    if (check(p, TK_IDENT)) { /* struct name */
        *type_name = strdup(p->cur.lexeme);
        advance(p);
        return TYPE_STRUCT;
    }
    error(p, "expected a type");
    return TYPE_UNKNOWN;
}

/* ---------- Statement parsing ---------- */

/* var_decl := ('let'|'const') IDENT ':' type ['=' expr]  (does not consume the ';') */
static Node *parse_var_decl(Parser *p) {
    int is_const = check(p, TK_CONST);
    advance(p); /* consume let/const */
    Node *n = node_new(ND_VAR_DECL, p->cur.line);
    n->is_const = is_const;
    if (!check(p, TK_IDENT)) error(p, "expected variable name");
    n->name = strdup(p->cur.lexeme);
    advance(p);
    expect(p, TK_COLON, "expected ':' after variable name");
    n->type = parse_type(p, &n->ptr, &n->type_name, &n->sig);
    if (match(p, TK_ASSIGN)) {
        n->operand = parse_expr(p); /* initial value */
    } else if (is_const) {
        /* a const without a value can never be given one later (writes are rejected) */
        error(p, "const declaration requires an initializer");
    }
    return n;
}

/* Parse 'if' with elseif/else, converting elseif into a nested if in the else branch */
static Node *parse_if(Parser *p) {
    Node *n = node_new(ND_IF, p->cur.line);
    advance(p); /* consume if/elseif */
    expect(p, TK_LPAREN, "expected '(' after if");
    n->cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after condition");
    n->then_branch = parse_block(p);
    if (check(p, TK_ELSEIF)) {
        n->else_branch = parse_if(p); /* elseif = nested if in the else branch */
    } else if (match(p, TK_ELSE)) {
        n->else_branch = parse_block(p);
    }
    return n;
}

/* while := 'while' '(' expr ')' block */
static Node *parse_while(Parser *p) {
    Node *n = node_new(ND_WHILE, p->cur.line);
    advance(p); /* consume while */
    expect(p, TK_LPAREN, "expected '(' after while");
    n->cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after condition");
    n->body = parse_block(p);
    return n;
}

/* for := 'for' '(' [var_decl|expr] ';' [expr] ';' [expr] ')' block */
static Node *parse_for(Parser *p) {
    Node *n = node_new(ND_FOR, p->cur.line);
    advance(p); /* consume for */
    expect(p, TK_LPAREN, "expected '(' after for");
    /* Init part */
    if (!check(p, TK_SEMICOLON)) {
        if (check(p, TK_LET) || check(p, TK_CONST)) n->init = parse_var_decl(p);
        else n->init = parse_expr(p);
    }
    expect(p, TK_SEMICOLON, "expected ';' after for-init");
    /* Condition */
    if (!check(p, TK_SEMICOLON)) n->cond = parse_expr(p);
    expect(p, TK_SEMICOLON, "expected ';' after for-condition");
    /* Step part */
    if (!check(p, TK_RPAREN)) n->step = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after for-clauses");
    n->body = parse_block(p);
    return n;
}

/* do_while := 'do' block 'while' '(' expr ')' ';' */
static Node *parse_do(Parser *p) {
    Node *n = node_new(ND_DOWHILE, p->cur.line);
    advance(p); /* consume do */
    n->body = parse_block(p);
    expect(p, TK_WHILE, "expected 'while' after do block");
    expect(p, TK_LPAREN, "expected '(' after while");
    n->cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after condition");
    expect(p, TK_SEMICOLON, "expected ';' after do-while");
    return n;
}

/* switch := 'switch' '(' expr ')' '{' ('case' expr ':' stmt* | 'default' ':' stmt*)* '}'
 * Each case is stored as ND_CASE (operand = value to compare, NULL = default; items = case statements) */
static Node *parse_switch(Parser *p) {
    Node *n = node_new(ND_SWITCH, p->cur.line);
    advance(p); /* consume switch */
    expect(p, TK_LPAREN, "expected '(' after switch");
    n->cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after switch value");
    expect(p, TK_LBRACE, "expected '{' after switch");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->had_error) {
        Node *c = node_new(ND_CASE, p->cur.line);
        if (match(p, TK_CASE)) {
            c->operand = parse_expr(p);
            expect(p, TK_COLON, "expected ':' after case value");
        } else if (match(p, TK_DEFAULT)) {
            c->operand = NULL; /* NULL = default */
            expect(p, TK_COLON, "expected ':' after default");
        } else {
            error(p, "expected 'case' or 'default' in switch");
            break;
        }
        /* Statements of this case: read until the next case/default/} */
        while (!check(p, TK_CASE) && !check(p, TK_DEFAULT) &&
               !check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->had_error) {
            node_add_item(c, parse_stmt(p));
        }
        node_add_item(n, c);
    }
    expect(p, TK_RBRACE, "expected '}' to close switch");
    return n;
}

/* block := '{' stmt* '}' */
static Node *parse_block(Parser *p) {
    Node *n = node_new(ND_BLOCK, p->cur.line);
    if (++p->depth > MAX_PARSE_DEPTH) {
        if (!p->had_error) error(p, "block nested too deeply");
        p->depth--;
        return n;
    }
    expect(p, TK_LBRACE, "expected '{'");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->had_error) {
        node_add_item(n, parse_stmt(p));
    }
    expect(p, TK_RBRACE, "expected '}'");
    p->depth--;
    return n;
}

/* stmt := var_decl ';' | return | if | while | for | break | continue | block | expr ';' */
static Node *parse_stmt(Parser *p) {
    if (check(p, TK_LET) || check(p, TK_CONST)) {
        Node *n = parse_var_decl(p);
        expect(p, TK_SEMICOLON, "expected ';' after declaration");
        return n;
    }
    if (check(p, TK_RETURN)) {
        Node *n = node_new(ND_RETURN, p->cur.line);
        advance(p);
        if (!check(p, TK_SEMICOLON)) n->operand = parse_expr(p);
        expect(p, TK_SEMICOLON, "expected ';' after return");
        return n;
    }
    if (check(p, TK_IF))     return parse_if(p);
    if (check(p, TK_WHILE))  return parse_while(p);
    if (check(p, TK_FOR))    return parse_for(p);
    if (check(p, TK_DO))     return parse_do(p);
    if (check(p, TK_SWITCH)) return parse_switch(p);
    if (check(p, TK_BREAK)) {
        Node *n = node_new(ND_BREAK, p->cur.line);
        advance(p);
        expect(p, TK_SEMICOLON, "expected ';' after break");
        return n;
    }
    if (check(p, TK_CONTINUE)) {
        Node *n = node_new(ND_CONTINUE, p->cur.line);
        advance(p);
        expect(p, TK_SEMICOLON, "expected ';' after continue");
        return n;
    }
    if (check(p, TK_LBRACE)) return parse_block(p);

    /* Anything else is treated as an expression statement (assignment / call / ++/--) */
    Node *e = node_new(ND_EXPR_STMT, p->cur.line);
    e->operand = parse_expr(p);
    expect(p, TK_SEMICOLON, "expected ';' after expression");
    return e;
}

/* ---------- Top-level parsing ---------- */

/* func_def    := 'func' IDENT '(' params? ')' '->' type block
 * extern_func := 'extern' 'func' IDENT '(' params? ')' '->' type ';'
 * is_extern = 1 means a foreign function declaration (no body, terminated by ';') */
static Node *parse_func_decl(Parser *p, int is_extern) {
    Node *fn = node_new(ND_FUNC, p->cur.line);
    fn->is_extern = is_extern;
    expect(p, TK_FUNC, "expected 'func'");
    if (!check(p, TK_IDENT) && !check(p, TK_FROM)) error(p, "expected function name"); /* allow 'from' (Rust idiom: String::from) */
    fn->name = strdup(p->cur.lexeme);
    advance(p);
    /* generic type parameters: func name<T, U>(...) */
    if (check(p, TK_LT)) {
        advance(p); /* consume < */
        do {
            if (!check(p, TK_IDENT)) { error(p, "expected generic type parameter name"); break; }
            int gi = fn->ngen < 4 ? fn->ngen : 3;
            if (fn->ngen < 4) fn->gen[fn->ngen++] = strdup(p->cur.lexeme);
            advance(p);
            /* Trait bound: <T: Display> stores the trait name to check at instantiation time */
            if (match(p, TK_COLON)) {
                if (!check(p, TK_IDENT)) { error(p, "expected trait name after ':'"); break; }
                fn->gen_bound[gi] = strdup(p->cur.lexeme);
                advance(p);
            }
        } while (match(p, TK_COMMA));
        expect(p, TK_GT, "expected '>' after generic type parameters");
    }
    expect(p, TK_LPAREN, "expected '(' after function name");
    if (!check(p, TK_RPAREN)) {
        do {
            Node *param = node_new(ND_PARAM, p->cur.line);
            if (!check(p, TK_IDENT)) error(p, "expected parameter name");
            param->name = strdup(p->cur.lexeme);
            advance(p);
            expect(p, TK_COLON, "expected ':' after parameter name");
            param->type = parse_type(p, &param->ptr, &param->type_name, &param->sig);
            /* Default value: stored in the param's operand; call sites missing trailing
             * arguments get a clone of this expression (fill_default_args in generic.c) */
            if (match(p, TK_ASSIGN)) param->operand = parse_expr(p);
            node_add_item(fn, param);
        } while (match(p, TK_COMMA));
    }
    expect(p, TK_RPAREN, "expected ')' after parameters");
    /* defaults must be trailing: once one parameter has a default, all later ones need one too */
    {
        int seen_default = 0;
        for (int i = 0; i < fn->nitems; i++) {
            if (fn->items[i]->operand) seen_default = 1;
            else if (seen_default) {
                error(p, "parameter without a default follows a parameter with a default");
                break;
            }
        }
    }
    expect(p, TK_ARROW, "expected '->' before return type");
    fn->type = parse_type(p, &fn->ptr, &fn->type_name, NULL); /* nested func-ptr return not supported */
    if (is_extern) {
        expect(p, TK_SEMICOLON, "expected ';' after extern declaration");
        fn->body = NULL; /* foreign function has no body */
    } else if (match(p, TK_SEMICOLON)) {
        fn->body = NULL; /* signature only (used in traits: a method without a default) */
    } else {
        fn->body = parse_block(p);
    }
    return fn;
}

/* impl := 'impl' IDENT '{' func_def* '}'
 * Each method is tagged with ns = struct name and is_method = 1, then added to the program
 * (kept temporarily in the ND_IMPL node's items for parse_program to spread out) */
static Node *parse_impl(Parser *p) {
    Node *n = node_new(ND_PROGRAM, p->cur.line); /* used as a temporary container for methods */
    advance(p); /* consume impl */
    /* Forms:  impl Type { ... }   (inherent)
     *         impl Trait for Type { ... }   (trait impl) */
    char *first = NULL, *trait = NULL, *sname = NULL;
    if (!check(p, TK_IDENT)) error(p, "expected type/trait name after impl");
    else { first = strdup(p->cur.lexeme); advance(p); }
    if (match(p, TK_FOR)) {                 /* impl Trait for Type */
        trait = first;
        if (!check(p, TK_IDENT)) error(p, "expected type name after 'for'");
        else { sname = strdup(p->cur.lexeme); advance(p); }
    } else {                                /* impl Type */
        sname = first;
    }
    expect(p, TK_LBRACE, "expected '{' after impl type");
    while (check(p, TK_FUNC) && !p->had_error) {
        Node *m = parse_func_decl(p, 0);
        m->ns = sname ? strdup(sname) : NULL; /* method/associated fn lives in the type's namespace */
        m->is_method = 1;
        node_add_item(n, m);
    }
    expect(p, TK_RBRACE, "expected '}' to close impl block");
    /* If this is a trait impl, leave a marker for the checker (which type impls which trait) */
    if (trait && sname) {
        Node *mark = node_new(ND_TRAIT_IMPL, p->cur.line);
        mark->name = strdup(trait); mark->type_name = strdup(sname);
        node_add_item(n, mark);
    }
    free(trait); free(sname);
    return n;
}

/* trait := 'trait' IDENT '{' (func_sig ';')* '}'
 * Stores only signatures (no bodies) in the ND_TRAIT node's items,
 * used to verify a type implements everything */
static Node *parse_trait(Parser *p) {
    Node *n = node_new(ND_TRAIT, p->cur.line);
    advance(p); /* consume trait */
    if (!check(p, TK_IDENT)) error(p, "expected trait name");
    else { n->name = strdup(p->cur.lexeme); advance(p); }
    expect(p, TK_LBRACE, "expected '{' after trait name");
    while (check(p, TK_FUNC) && !p->had_error) {
        /* Method in a trait: ends with ';' = pure signature, or has { } = default method */
        Node *sig = parse_func_decl(p, 0);
        node_add_item(n, sig);
    }
    expect(p, TK_RBRACE, "expected '}' to close trait block");
    return n;
}

/* import := 'import' '{' IDENT (',' IDENT)* '}' 'from' STRING ';'
 * Stores the path in str_val and the imported names in items (as ND_IDENT nodes) */
static Node *parse_import(Parser *p) {
    Node *n = node_new(ND_IMPORT, p->cur.line);
    advance(p); /* consume import */
    if (check(p, TK_IDENT)) {
        /* Alias form: import lib from "path"  -> the whole module gets namespace = lib (stored in n->name) */
        n->name = strdup(p->cur.lexeme);
        advance(p);
    } else {
        /* Braced form: import { a, b } from "path" */
        expect(p, TK_LBRACE, "expected '{' or an alias name after import");
        if (!check(p, TK_RBRACE)) {
            do {
                if (!check(p, TK_IDENT)) { error(p, "expected name in import list"); break; }
                Node *id = node_new(ND_IDENT, p->cur.line);
                id->name = strdup(p->cur.lexeme);
                node_add_item(n, id);
                advance(p);
            } while (match(p, TK_COMMA));
        }
        expect(p, TK_RBRACE, "expected '}' to close import list");
    }
    expect(p, TK_FROM, "expected 'from' after import list");
    if (!check(p, TK_STRING)) error(p, "expected module path string");
    else { n->str_val = strdup(p->cur.lexeme); advance(p); }
    expect(p, TK_SEMICOLON, "expected ';' after import");
    return n;
}

/* struct_decl := 'struct' IDENT '{' (IDENT ':' type (';'|',')?)* '}'
 * Each field is stored as an ND_PARAM node (with name, type, ptr, type_name) */
static Node *parse_struct(Parser *p) {
    Node *n = node_new(ND_STRUCT_DECL, p->cur.line);
    advance(p); /* consume struct */
    if (!check(p, TK_IDENT)) error(p, "expected struct name");
    n->name = strdup(p->cur.lexeme);
    advance(p);
    expect(p, TK_LBRACE, "expected '{' after struct name");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->had_error) {
        Node *f = node_new(ND_PARAM, p->cur.line);
        if (!check(p, TK_IDENT)) { error(p, "expected field name"); break; }
        f->name = strdup(p->cur.lexeme);
        advance(p);
        expect(p, TK_COLON, "expected ':' after field name");
        f->type = parse_type(p, &f->ptr, &f->type_name, &f->sig);
        node_add_item(n, f);
        /* Field separator: ';' or ',' (flexible) */
        if (!match(p, TK_SEMICOLON)) match(p, TK_COMMA);
    }
    expect(p, TK_RBRACE, "expected '}' to close struct");
    return n;
}

/* program := (import | struct_decl | func_def | extern_func | global var_decl ';')* */
Node *parse_program(const char *src, const char *filename, int *had_error) {
    Lexer lx;
    lexer_init(&lx, src, filename);
    Parser p;
    p.lx = &lx;
    p.had_error = 0;
    p.depth = 0;
    advance(&p); /* load the first token */

    Node *prog = node_new(ND_PROGRAM, 1);
    while (!check(&p, TK_EOF) && !p.had_error) {
        if (check(&p, TK_IMPORT)) {
            node_add_item(prog, parse_import(&p));
        } else if (check(&p, TK_FUNC)) {
            node_add_item(prog, parse_func_decl(&p, 0));
        } else if (check(&p, TK_EXPORT)) {
            advance(&p); /* consume export, then expect func */
            Node *fn = parse_func_decl(&p, 0);
            fn->is_export = 1; /* exported so C can call it (raw symbol name) */
            node_add_item(prog, fn);
        } else if (check(&p, TK_IMPL)) {
            /* impl block: spread every method (+ the trait impl marker) into the program */
            Node *blk = parse_impl(&p);
            for (int i = 0; i < blk->nitems; i++) node_add_item(prog, blk->items[i]);
        } else if (check(&p, TK_TRAIT)) {
            node_add_item(prog, parse_trait(&p));   /* trait definition (signatures kept for checking) */
        } else if (check(&p, TK_EXTERN)) {
            advance(&p); /* consume extern, then expect func */
            node_add_item(prog, parse_func_decl(&p, 1));
        } else if (check(&p, TK_STRUCT)) {
            node_add_item(prog, parse_struct(&p));
            match(&p, TK_SEMICOLON); /* trailing ';' is optional */
        } else if (check(&p, TK_LET) || check(&p, TK_CONST)) {
            Node *n = parse_var_decl(&p);
            expect(&p, TK_SEMICOLON, "expected ';' after global declaration");
            node_add_item(prog, n);
        } else {
            error(&p, "expected an import, function, or global declaration");
        }
    }

    *had_error = p.had_error || lx.had_error;
    return prog;
}
