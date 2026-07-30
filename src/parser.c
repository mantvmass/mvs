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
#include "diag.h"

#define MAX_PARSE_ERRORS 20  /* stop reporting after this many; the file is likely mangled */

/* ---------- Basic parser helper functions ---------- */

/* Move to the next token, replacing the current one (do not free the lexeme; the AST may still reference it) */
static void advance(Parser *p) {
    p->cur = lexer_next(p->lx);
    ast_set_col(p->cur.col);   /* nodes created from here on point at this column */
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

/* Report a syntax error with its location. Sets panic mode: further errors are
 * suppressed until synchronize() finds a safe point to resume from, so one real
 * mistake does not cascade into a wall of noise. */
static void error(Parser *p, const char *msg) {
    if (p->trying) { p->panic = 1; return; }  /* speculative parse: fail quietly, caller restores */
    p->had_error = 1;
    if (p->panic || p->fatal) return;      /* one report per bad region */
    p->panic = 1;
    if (++p->nerrors >= MAX_PARSE_ERRORS) {
        fprintf(stderr, "%s: too many syntax errors; giving up on this file\n", p->lx->filename);
        p->fatal = 1;
        return;
    }
    diag_print(p->lx->filename, p->cur.line, p->cur.col, "syntax error",
               "%s (near '%s')", msg, p->cur.lexeme);
}

/* Skip ahead to a point where parsing can safely continue: just past a ';',
 * in front of a '}', or at a token that clearly starts a new statement/declaration */
static void synchronize(Parser *p) {
    p->panic = 0;
    for (;;) {
        switch (p->cur.type) {
            case TK_EOF: case TK_RBRACE:
                return;
            case TK_SEMICOLON:
                advance(p); return;
            case TK_FUNC: case TK_STRUCT: case TK_IMPL: case TK_TRAIT: case TK_ENUM:
            case TK_IMPORT: case TK_EXTERN: case TK_EXPORT:
            case TK_LET: case TK_CONST: case TK_IF: case TK_WHILE: case TK_FOR:
            case TK_DO: case TK_SWITCH: case TK_MATCH: case TK_RETURN: case TK_BREAK: case TK_CONTINUE:
                return;
            default:
                advance(p);
        }
    }
}

/* Top-level recovery: skip to the next token that can start a top-level declaration */
static void synchronize_top(Parser *p) {
    p->panic = 0;
    for (;;) {
        switch (p->cur.type) {
            case TK_EOF:
            case TK_FUNC: case TK_STRUCT: case TK_IMPL: case TK_TRAIT: case TK_ENUM:
            case TK_IMPORT: case TK_EXTERN: case TK_EXPORT:
            case TK_LET: case TK_CONST: case TK_AT:
                return;
            default:
                advance(p);
        }
    }
}

/* Require that the current token is of the given kind, otherwise error */
static void expect(Parser *p, TokenType t, const char *what) {
    if (!match(p, t)) error(p, what);
}

/* Check whether the current token is a data type keyword */
static int is_type_token(TokenType t) {
    return datatype_from_token(t) != TYPE_UNKNOWN;
}

/* Parse a '+'-separated trait bound list (A + B + C) into buf as "A+B+C" */
static void parse_bound_list(Parser *p, char *buf, size_t cap) {
    size_t bl = 0; buf[0] = '\0';
    do {
        if (!check(p, TK_IDENT)) { error(p, "expected a trait name in bound"); break; }
        if (bl && bl + 1 < cap) buf[bl++] = '+';
        bl += (size_t)snprintf(buf + bl, cap - bl, "%s", p->cur.lexeme);
        if (bl >= cap) bl = cap - 1;
        advance(p);
    } while (match(p, TK_PLUS));
}

/* Forward declarations (mutual recursion) */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static DataType parse_type(Parser *p, int *ptr, char **type_name, Node **sig_out, int *arr_out);
static int parse_generic_cname(Parser *p, const char *base, char *out, size_t on,
                               char *out_args[4], int *out_nargs);
static Node *parse_match(Parser *p);

/* Consume one closing '>' of a generic argument list. A '>>' (lexed as TK_SHR when
 * generics nest, e.g. Vec<Vec<i64>>) counts as two: the first consumer takes the
 * token and leaves a debt; the second consumer just clears the debt. */
static int expect_gt(Parser *p) {
    if (p->gt_debt) { p->gt_debt = 0; return 1; }
    if (check(p, TK_GT)) { advance(p); return 1; }
    if (check(p, TK_SHR)) { advance(p); p->gt_debt = 1; return 1; }
    return 0;
}

#define MAX_PARSE_DEPTH 300  /* guard against stack overflow from abnormally deep expressions/blocks/unary chains */

/* ---------- Expression parsing ---------- */

/* The body of a struct literal after its (possibly generic) name: '{' fields '}'.
 * Takes ownership of name. Each field is an ND_ASSIGN: lhs = field name, rhs = value */
static Node *parse_struct_lit_body(Parser *p, char *name, int line) {
    Node *lit = node_new(ND_STRUCT_LIT, line);
    lit->name = name;
    advance(p); /* consume { */
    if (!check(p, TK_RBRACE)) {
        do {
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
        /* Name<...> in an expression: a generic struct literal (Vec<i64> { ... }) or an
         * explicit-argument generic call (none<i64>()). '<' is also plain less-than, so
         * parse speculatively and restore on anything else. */
        if (check(p, TK_LT)) {
            Lexer lsave = *p->lx;
            Token csave = p->cur;
            int sav_err = p->had_error, sav_panic = p->panic;
            int sav_debt = p->gt_debt, sav_n = p->nerrors, sav_try = p->trying;
            p->trying = 1;
            char cname[256];
            char *gargs[4]; int ngargs = 0;
            int ok = parse_generic_cname(p, idname, cname, sizeof(cname), gargs, &ngargs);
            p->trying = sav_try;
            if (ok && !p->panic && check(p, TK_LBRACE)) {
                free(idname);
                for (int i = 0; i < ngargs; i++) free(gargs[i]);
                return parse_struct_lit_body(p, strdup(cname), line);
            }
            if (ok && !p->panic && check(p, TK_LPAREN)) {
                /* the args ride on the callee ident; monomorphize reads them instead of inferring */
                Node *n = node_new(ND_IDENT, line);
                n->name = idname; n->type = TYPE_UNKNOWN;
                for (int i = 0; i < ngargs && i < 4; i++) n->gen[i] = gargs[i];
                n->ngen = ngargs;
                return n;   /* parse_postfix attaches the call */
            }
            if (ok && !p->panic && check(p, TK_COLONCOLON)) {
                /* associated function on a generic struct: Vec<i64>::new(...).
                 * The ident keeps the canonical name; monomorphize rewrites it to
                 * the instance and parse_postfix builds the :: member call. */
                free(idname);
                for (int i = 0; i < ngargs; i++) free(gargs[i]);
                Node *n = node_new(ND_IDENT, line);
                n->name = strdup(cname); n->type = TYPE_UNKNOWN;
                return n;
            }
            /* plain less-than after all: rewind and fall through */
            for (int i = 0; i < ngargs; i++) free(gargs[i]);
            *p->lx = lsave; p->cur = csave;
            p->had_error = sav_err; p->panic = sav_panic;
            p->gt_debt = sav_debt; p->nerrors = sav_n;
        }
        /* If followed by '{', this is a struct literal: Name { field: expr, ... } */
        if (check(p, TK_LBRACE)) return parse_struct_lit_body(p, idname, line);
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
    if (check(p, TK_LBRACKET)) {
        /* Array literal [e1, e2, ...]; valid only as a variable initializer (checked later) */
        Node *lit = node_new(ND_ARRAY_LIT, t.line);
        advance(p); /* consume [ */
        if (!check(p, TK_RBRACKET)) {
            do {
                node_add_item(lit, parse_expr(p));
            } while (match(p, TK_COMMA));
        }
        expect(p, TK_RBRACKET, "expected ']' to close array literal");
        return lit;
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
        } else if (match(p, TK_LBRACKET)) {
            /* Indexing a[i]: works on arrays and on pointers (p[i] = *(p + i)) */
            Node *ix = node_new(ND_INDEX, p->cur.line);
            ix->lhs = node;
            ix->rhs = parse_expr(p);
            expect(p, TK_RBRACKET, "expected ']' after index");
            node = ix;
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
        c->type = parse_type(p, &c->ptr, &c->type_name, &c->sig, NULL);
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

/* type := '[' type ';' INT ']' | ('*')* (type_keyword | IDENT)
 * Leading '*' counts as pointer depth; IDENT = struct name.
 * Stores pointer depth into *ptr and the struct name into *type_name (NULL if not a struct).
 * arr_out receives the element count of a [T; N] array type (NULL = arrays not allowed here) */
static DataType parse_type(Parser *p, int *ptr, char **type_name, Node **sig_out, int *arr_out) {
    if (arr_out) *arr_out = 0;
    /* array type [T; N]: parse the element type recursively, then the fixed length */
    if (check(p, TK_LBRACKET)) {
        if (!arr_out) {
            error(p, "an array type [T; N] is not allowed here (only variables and struct fields)");
            /* keep parsing to recover sensibly */
        }
        advance(p); /* consume [ */
        DataType elem = parse_type(p, ptr, type_name, sig_out, NULL); /* no arrays of arrays */
        expect(p, TK_SEMICOLON, "expected ';' between element type and length in [T; N]");
        int len = 0;
        if (check(p, TK_INT)) { len = (int)p->cur.int_val; advance(p); }
        else error(p, "expected a constant integer length in [T; N]");
        if (len <= 0 && !p->panic) error(p, "array length must be at least 1");
        expect(p, TK_RBRACKET, "expected ']' to close the array type");
        if (arr_out) *arr_out = len;
        return elem;
    }
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
                par->type = parse_type(p, &par->ptr, &par->type_name, &par->sig, NULL);
                node_add_item(sig, par);
            } while (match(p, TK_COMMA));
        }
        expect(p, TK_RPAREN, "expected ')' in function-pointer type");
        expect(p, TK_ARROW, "expected '->' before return type in function-pointer type");
        sig->type = parse_type(p, &sig->ptr, &sig->type_name, NULL, NULL); /* nested func-ptr return not supported */
        if (sig_out) *sig_out = sig;
        return TYPE_FUNC;
    }
    if (check(p, TK_DYN)) {
        /* trait object: dyn Trait (a fat pointer {data, vtable}); type_name = the trait */
        advance(p);
        if (!check(p, TK_IDENT)) { error(p, "expected a trait name after 'dyn'"); return TYPE_UNKNOWN; }
        *type_name = strdup(p->cur.lexeme);
        advance(p);
        return TYPE_DYN;
    }
    if (is_type_token(p->cur.type)) {
        DataType dt = datatype_from_token(p->cur.type);
        advance(p);
        return dt;
    }
    if (check(p, TK_IDENT)) { /* struct name, possibly generic: Vec<i64>, Pair<i64,str> */
        char base[128];
        snprintf(base, sizeof(base), "%s", p->cur.lexeme);
        advance(p);
        if (check(p, TK_LT)) {
            char cname[256];
            if (!parse_generic_cname(p, base, cname, sizeof(cname), NULL, NULL))
                return TYPE_UNKNOWN;
            *type_name = strdup(cname);   /* canonical "Vec<i64>"; monomorphized later */
            return TYPE_STRUCT;
        }
        *type_name = strdup(base);
        return TYPE_STRUCT;
    }
    error(p, "expected a type");
    return TYPE_UNKNOWN;
}

/* After Name, parse '<' type {',' type} '>' into out = the canonical "Name<a,b>"
 * (no spaces; nested generics keep their own angles). Optionally captures each
 * top-level argument's canonical text in out_args (strdup'd, max 4).
 * Returns 1 on success; on failure reports an error (silently in trying mode). */
static int parse_generic_cname(Parser *p, const char *base, char *out, size_t on,
                               char *out_args[4], int *out_nargs) {
    advance(p);   /* consume '<' */
    size_t len = (size_t)snprintf(out, on, "%s<", base);
    if (out_nargs) *out_nargs = 0;
    do {
        int aptr = 0, aarr = 0;
        char *aname = NULL;
        DataType at = parse_type(p, &aptr, &aname, NULL, &aarr);
        if (p->panic || p->fatal) { free(aname); return 0; }
        if (aptr > 0 || aarr > 0 || at == TYPE_FUNC || at == TYPE_DYN ||
            at == TYPE_UNKNOWN || at == TYPE_VOID) {
            free(aname);
            error(p, "generic type arguments must be primitive types or struct names");
            return 0;
        }
        const char *an = (at == TYPE_STRUCT) ? aname : datatype_name(at);
        if (len + strlen(an) + 3 >= on) { free(aname); error(p, "generic type name too long"); return 0; }
        if (out[len - 1] != '<') out[len++] = ',';
        strcpy(out + len, an);
        len += strlen(an);
        if (out_args && out_nargs && *out_nargs < 4) out_args[(*out_nargs)++] = strdup(an);
        free(aname);
    } while (match(p, TK_COMMA));
    if (!expect_gt(p)) {
        error(p, "expected '>' to close the generic type arguments");
        return 0;
    }
    out[len++] = '>';
    out[len] = '\0';
    return 1;
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
    n->type = parse_type(p, &n->ptr, &n->type_name, &n->sig, &n->arr);
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
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
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
        if (p->panic) synchronize(p);
        /* Statements of this case: read until the next case/default/} */
        while (!check(p, TK_CASE) && !check(p, TK_DEFAULT) &&
               !check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
            node_add_item(c, parse_stmt(p));
            if (p->panic) synchronize(p);
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
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        node_add_item(n, parse_stmt(p));
        if (p->panic) synchronize(p);   /* recover so later statements still get checked */
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
    if (check(p, TK_MATCH))  return parse_match(p);
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
            /* Trait bound: <T: Display> or <T: A + B>; stored as "A+B" and checked at instantiation */
            if (match(p, TK_COLON)) {
                char bounds[256];
                parse_bound_list(p, bounds, sizeof(bounds));
                fn->gen_bound[gi] = strdup(bounds);
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
            /* variadic tail: `args: ...dyn Trait` collects extra call arguments as a slice.
             * The parameter itself becomes a POINTER to the element blobs, and a hidden
             * `<name>_len: usize` parameter carrying the count is appended after it. */
            if (match(p, TK_ELLIPSIS)) {
                param->variadic = 1;
                fn->variadic = 1;
                param->type = parse_type(p, &param->ptr, &param->type_name, &param->sig, NULL);
                if (param->type != TYPE_DYN)
                    error(p, "a variadic parameter must be a trait object (args: ...dyn Trait)");
                param->ptr += 1;              /* the callee sees a pointer to the elements */
                node_add_item(fn, param);
                Node *plen = node_new(ND_PARAM, param->line);
                char lname[256];
                snprintf(lname, sizeof(lname), "%s_len", param->name ? param->name : "args");
                plen->name = strdup(lname);
                plen->type = TYPE_USIZE;
                node_add_item(fn, plen);
                if (!check(p, TK_RPAREN))
                    error(p, "the variadic parameter must be the last parameter");
                break;                        /* nothing may follow the variadic tail */
            }
            param->type = parse_type(p, &param->ptr, &param->type_name, &param->sig, NULL); /* pass *T, not [T; N] */
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
            else if (seen_default && !fn->variadic) {
                error(p, "parameter without a default follows a parameter with a default");
                break;
            }
        }
    }
    expect(p, TK_ARROW, "expected '->' before return type");
    fn->type = parse_type(p, &fn->ptr, &fn->type_name, NULL, NULL); /* nested func-ptr return not supported */
    /* where clause: `where T: A + B, U: C` fills the same bound slots as <T: A> */
    if (match(p, TK_WHERE)) {
        do {
            if (!check(p, TK_IDENT)) { error(p, "expected a generic parameter name in where clause"); break; }
            int gi = -1;
            for (int i = 0; i < fn->ngen; i++)
                if (strcmp(fn->gen[i], p->cur.lexeme) == 0) { gi = i; break; }
            if (gi < 0) error(p, "where clause names a type that is not a generic parameter");
            advance(p);
            expect(p, TK_COLON, "expected ':' in where clause");
            char bounds[256];
            parse_bound_list(p, bounds, sizeof(bounds));
            if (gi >= 0) { free(fn->gen_bound[gi]); fn->gen_bound[gi] = strdup(bounds); }
        } while (match(p, TK_COMMA));
    }
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
    char *gnames[4]; int ngnames = 0;
    if (!check(p, TK_IDENT)) error(p, "expected type/trait name after impl");
    else { first = strdup(p->cur.lexeme); advance(p); }
    /* generic impl: impl Vec<T> { ... } (the methods become templates tied to the struct) */
    if (match(p, TK_LT)) {
        do {
            if (!check(p, TK_IDENT)) { error(p, "expected a generic parameter name"); break; }
            if (ngnames < 4) gnames[ngnames++] = strdup(p->cur.lexeme);
            else error(p, "too many generic parameters (max 4)");
            advance(p);
        } while (match(p, TK_COMMA));
        expect(p, TK_GT, "expected '>' after the generic parameters");
    }
    if (match(p, TK_FOR)) {                 /* impl Trait for Type (structs AND primitives) */
        if (ngnames > 0) error(p, "generic trait impls are not supported yet");
        trait = first;
        if (check(p, TK_IDENT) || is_type_token(p->cur.type)) {
            sname = strdup(p->cur.lexeme);  /* a primitive keyword keeps its spelling, e.g. "i64" */
            advance(p);
        } else error(p, "expected a type name after 'for'");
    } else {                                /* impl Type */
        sname = first;
    }
    expect(p, TK_LBRACE, "expected '{' after impl type");
    while (check(p, TK_FUNC) && !p->fatal) {
        Node *m = parse_func_decl(p, 0);
        m->ns = sname ? strdup(sname) : NULL; /* method/associated fn lives in the type's namespace */
        m->is_method = 1;
        /* methods of a generic struct carry its parameters (template until instantiated) */
        for (int gi = 0; gi < ngnames && m->ngen < 4; gi++)
            m->gen[m->ngen++] = strdup(gnames[gi]);
        node_add_item(n, m);
        if (p->panic) synchronize(p);
    }
    for (int gi = 0; gi < ngnames; gi++) free(gnames[gi]);
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
    while (check(p, TK_FUNC) && !p->fatal) {
        /* Method in a trait: ends with ';' = pure signature, or has { } = default method */
        Node *sig = parse_func_decl(p, 0);
        node_add_item(n, sig);
        if (p->panic) synchronize(p);
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

/* enum_decl := 'enum' IDENT '{' variant (',' variant)* ','? '}'
 * variant   := IDENT [ '(' type (',' type)* ')' ]
 * Stored as ND_ENUM_DECL; desugared into a tagged struct + associated
 * constructors right after module load (see desugar_enums) */
static Node *parse_enum(Parser *p) {
    Node *n = node_new(ND_ENUM_DECL, p->cur.line);
    advance(p); /* consume enum */
    if (!check(p, TK_IDENT)) error(p, "expected enum name");
    else { n->name = strdup(p->cur.lexeme); advance(p); }
    expect(p, TK_LBRACE, "expected '{' after enum name");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        Node *v = node_new(ND_PARAM, p->cur.line);
        if (!check(p, TK_IDENT)) { error(p, "expected a variant name"); break; }
        v->name = strdup(p->cur.lexeme);
        advance(p);
        if (match(p, TK_LPAREN)) {          /* payload types: Circle(f64), Rect(f64, f64) */
            do {
                Node *pt = node_new(ND_PARAM, p->cur.line);
                pt->type = parse_type(p, &pt->ptr, &pt->type_name, &pt->sig, NULL);
                node_add_item(v, pt);
            } while (match(p, TK_COMMA));
            expect(p, TK_RPAREN, "expected ')' after the variant's payload types");
        }
        node_add_item(n, v);
        if (!match(p, TK_COMMA)) break;      /* trailing comma optional */
    }
    expect(p, TK_RBRACE, "expected '}' to close enum");
    return n;
}

/* match_stmt := 'match' '(' expr ')' '{' arm* '}'
 * arm        := ENUM '::' VARIANT [ '(' IDENT (',' IDENT)* ')' ] '=>' block [',']
 *             | '_' '=>' block [',']  */
static Node *parse_match(Parser *p) {
    Node *n = node_new(ND_MATCH, p->cur.line);
    advance(p); /* consume match */
    expect(p, TK_LPAREN, "expected '(' after match");
    n->cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after the match value");
    expect(p, TK_LBRACE, "expected '{' to open the match arms");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        Node *arm = node_new(ND_MARM, p->cur.line);
        if (check(p, TK_IDENT) && strcmp(p->cur.lexeme, "_") == 0) {
            advance(p);                      /* the catch-all arm: '_' */
        } else if (check(p, TK_IDENT)) {
            arm->type_name = strdup(p->cur.lexeme);   /* enum name */
            advance(p);
            expect(p, TK_COLONCOLON, "expected '::' in the match pattern (Enum::Variant)");
            if (!check(p, TK_IDENT)) { error(p, "expected a variant name after '::'"); break; }
            arm->name = strdup(p->cur.lexeme);
            advance(p);
            if (match(p, TK_LPAREN)) {       /* payload bindings: Circle(r), Rect(w, h) */
                do {
                    if (!check(p, TK_IDENT)) { error(p, "expected a binding name in the pattern"); break; }
                    Node *b = node_new(ND_IDENT, p->cur.line);
                    b->name = strdup(p->cur.lexeme);
                    node_add_item(arm, b);
                    advance(p);
                } while (match(p, TK_COMMA));
                expect(p, TK_RPAREN, "expected ')' after the pattern bindings");
            }
        } else {
            error(p, "expected a match pattern (Enum::Variant(...) or _)");
            break;
        }
        expect(p, TK_FATARROW, "expected '=>' after the match pattern");
        if (!check(p, TK_LBRACE)) { error(p, "expected a '{ ... }' block as the arm's body"); break; }
        arm->body = parse_block(p);
        match(p, TK_COMMA);                  /* trailing comma optional */
        node_add_item(n, arm);
        if (p->panic) synchronize(p);
    }
    expect(p, TK_RBRACE, "expected '}' to close the match");
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
    /* generic struct: struct Vec<T> / struct Pair<T, U> (a template; instances are
     * created per concrete argument list during monomorphization) */
    if (match(p, TK_LT)) {
        do {
            if (!check(p, TK_IDENT)) { error(p, "expected a generic parameter name"); break; }
            if (n->ngen < 4) n->gen[n->ngen++] = strdup(p->cur.lexeme);
            else error(p, "too many generic parameters (max 4)");
            advance(p);
        } while (match(p, TK_COMMA));
        expect(p, TK_GT, "expected '>' after the generic parameters");
    }
    expect(p, TK_LBRACE, "expected '{' after struct name");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        Node *f = node_new(ND_PARAM, p->cur.line);
        if (!check(p, TK_IDENT)) { error(p, "expected field name"); break; }
        f->name = strdup(p->cur.lexeme);
        advance(p);
        expect(p, TK_COLON, "expected ':' after field name");
        f->type = parse_type(p, &f->ptr, &f->type_name, &f->sig, &f->arr);
        node_add_item(n, f);
        /* Field separator: ';' or ',' (flexible) */
        if (!match(p, TK_SEMICOLON)) match(p, TK_COMMA);
    }
    expect(p, TK_RBRACE, "expected '}' to close struct");
    return n;
}

/* attribute := '@' 'compile' '(' key '=' STRING (',' key '=' STRING)* ')'
 *            | '@' 'test'
 * key := 'target_os' | 'target_arch'
 * @compile gates the NEXT top-level item (kept only when the values match the
 * current target; unknown values warn since they never match). @test marks the
 * next function as a test for `mvs test` / --test-main. */
static void parse_attribute(Parser *p, char **os, char **arch, int *is_test) {
    advance(p); /* consume '@' */
    if (check(p, TK_IDENT) && strcmp(p->cur.lexeme, "test") == 0) {
        advance(p); /* @test takes no arguments */
        *is_test = 1;
        return;
    }
    if (!check(p, TK_IDENT) || strcmp(p->cur.lexeme, "compile") != 0) {
        error(p, "unknown attribute; only @compile(...) and @test are supported");
        return;
    }
    advance(p); /* consume 'compile' */
    expect(p, TK_LPAREN, "expected '(' after @compile");
    do {
        if (!check(p, TK_IDENT)) {
            error(p, "expected 'target_os' or 'target_arch' inside @compile(...)");
            return;
        }
        char key[32];
        snprintf(key, sizeof(key), "%s", p->cur.lexeme);
        int kline = p->cur.line, kcol = p->cur.col;
        advance(p);
        expect(p, TK_ASSIGN, "expected '=' after the @compile key");
        if (!check(p, TK_STRING)) {
            error(p, "expected a string value, e.g. @compile(target_os = \"linux\")");
            return;
        }
        const char *val = p->cur.lexeme;
        if (strcmp(key, "target_os") == 0) {
            if (strcmp(val, "windows") != 0 && strcmp(val, "linux") != 0 && diag_is_primary(p->lx->filename))
                diag_print(p->lx->filename, p->cur.line, p->cur.col, "warning",
                           "unknown target_os \"%s\" (known: \"windows\", \"linux\"); this item will never be compiled", val);
            free(*os);
            *os = strdup(val);
        } else if (strcmp(key, "target_arch") == 0) {
            if (strcmp(val, "x86_64") != 0 && strcmp(val, "aarch64") != 0 && diag_is_primary(p->lx->filename))
                diag_print(p->lx->filename, p->cur.line, p->cur.col, "warning",
                           "unknown target_arch \"%s\" (known: \"x86_64\", \"aarch64\"); this item will never be compiled", val);
            free(*arch);
            *arch = strdup(val);
        } else {
            diag_print(p->lx->filename, kline, kcol, "syntax error",
                       "unknown @compile key '%s'; expected 'target_os' or 'target_arch'", key);
            p->had_error = 1;
            p->panic = 1;
            return;
        }
        advance(p); /* consume the string value */
    } while (match(p, TK_COMMA));
    expect(p, TK_RPAREN, "expected ')' to close @compile(...)");
}

/* program := (attribute* (import | struct_decl | func_def | extern_func | global var_decl ';'))* */
Node *parse_program(const char *src, const char *filename, int *had_error) {
    Lexer lx;
    lexer_init(&lx, src, filename);
    Parser p;
    p.lx = &lx;
    p.had_error = 0;
    p.nerrors = 0;
    p.panic = 0;
    p.fatal = 0;
    p.depth = 0;
    p.gt_debt = 0;
    p.trying = 0;
    ast_set_file(filename);  /* nodes made during this parse belong to this file */
    advance(&p); /* load the first token */

    Node *prog = node_new(ND_PROGRAM, 1);
    while (!check(&p, TK_EOF) && !p.fatal) {
        /* attributes gate the NEXT declaration; several attribute lines may stack */
        char *cfg_os = NULL, *cfg_arch = NULL;
        int cfg_test = 0;
        while (check(&p, TK_AT) && !p.fatal && !p.panic)
            parse_attribute(&p, &cfg_os, &cfg_arch, &cfg_test);
        int cfg_start = prog->nitems;   /* stamp every item added by this iteration */
        if (p.panic) {
            /* the attribute itself failed to parse; skip stamping and recover below */
        } else if (check(&p, TK_IMPORT)) {
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
        } else if (check(&p, TK_ENUM)) {
            node_add_item(prog, parse_enum(&p));
            match(&p, TK_SEMICOLON); /* trailing ';' is optional */
        } else if (check(&p, TK_LET) || check(&p, TK_CONST)) {
            Node *n = parse_var_decl(&p);
            expect(&p, TK_SEMICOLON, "expected ';' after global declaration");
            node_add_item(prog, n);
        } else {
            error(&p, "expected an import, function, or global declaration");
        }
        /* apply pending attributes to everything this iteration produced
         * (an impl block spreads several methods; all of them are gated together) */
        if (cfg_os || cfg_arch || cfg_test) {
            for (int i = cfg_start; i < prog->nitems; i++) {
                Node *it = prog->items[i];
                if (cfg_os && !it->cfg_os) it->cfg_os = strdup(cfg_os);
                if (cfg_arch && !it->cfg_arch) it->cfg_arch = strdup(cfg_arch);
                if (cfg_test && it->kind == ND_FUNC) it->is_test = 1;
            }
            free(cfg_os);
            free(cfg_arch);
        }
        if (p.panic) synchronize_top(&p);   /* recover and keep checking the rest of the file */
    }

    *had_error = p.had_error || lx.had_error;
    return prog;
}
