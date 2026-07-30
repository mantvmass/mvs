/*
 * ast.c - helpers for creating and managing AST nodes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* Source position the parser is currently at; stamped into every new node */
static const char *g_cur_file = NULL;
static int g_cur_col = 0;
void ast_set_file(const char *file) { g_cur_file = file; }
void ast_set_col(int col) { g_cur_col = col; }

/* Create a new node; every field starts zeroed/NULL */
Node *node_new(NodeKind kind, int line) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col = g_cur_col;
    n->file = g_cur_file;
    n->type = TYPE_UNKNOWN;
    return n;
}

/* Append a child node to the items[] array, growing it one slot at a time (short, easy to follow) */
void node_add_item(Node *parent, Node *child) {
    parent->items = (Node **)realloc(parent->items, sizeof(Node *) * (parent->nitems + 1));
    parent->items[parent->nitems++] = child;
}

/* Safe string copy (NULL -> NULL) */
static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

/* Deep-copy an AST tree; used when instantiating a generic function */
Node *node_clone(Node *n) {
    if (!n) return NULL;
    Node *c = (Node *)malloc(sizeof(Node));
    *c = *n; /* copy all scalar fields first */
    /* duplicate strings so the copy is independent of the original */
    c->str_val = dup_or_null(n->str_val);
    c->name = dup_or_null(n->name);
    c->type_name = dup_or_null(n->type_name);
    c->ns = dup_or_null(n->ns);
    c->mod = dup_or_null(n->mod);
    for (int i = 0; i < n->ngen; i++) { c->gen[i] = dup_or_null(n->gen[i]); c->gen_bound[i] = dup_or_null(n->gen_bound[i]); }
    /* deep-copy child nodes */
    c->lhs = node_clone(n->lhs);
    c->rhs = node_clone(n->rhs);
    c->operand = node_clone(n->operand);
    c->cond = node_clone(n->cond);
    c->then_branch = node_clone(n->then_branch);
    c->else_branch = node_clone(n->else_branch);
    c->init = node_clone(n->init);
    c->step = node_clone(n->step);
    c->body = node_clone(n->body);
    c->sig  = node_clone(n->sig);   /* function pointer signature (if any) */
    /* copy the items array */
    c->items = NULL; c->nitems = 0;
    for (int i = 0; i < n->nitems; i++) node_add_item(c, node_clone(n->items[i]));
    return c;
}

/* Convert a type token to the internal DataType */
DataType datatype_from_token(TokenType t) {
    switch (t) {
        case TK_TYPE_I8:    return TYPE_I8;
        case TK_TYPE_I16:   return TYPE_I16;
        case TK_TYPE_I32:   return TYPE_I32;
        case TK_TYPE_I64:   return TYPE_I64;
        case TK_TYPE_I128:  return TYPE_I128;
        case TK_TYPE_ISIZE: return TYPE_ISIZE;
        case TK_TYPE_U8:    return TYPE_U8;
        case TK_TYPE_U16:   return TYPE_U16;
        case TK_TYPE_U32:   return TYPE_U32;
        case TK_TYPE_U64:   return TYPE_U64;
        case TK_TYPE_U128:  return TYPE_U128;
        case TK_TYPE_USIZE: return TYPE_USIZE;
        case TK_TYPE_BOOL:  return TYPE_BOOL;
        case TK_TYPE_VOID:  return TYPE_VOID;
        case TK_TYPE_STR:   return TYPE_STR;
        case TK_TYPE_CHAR:  return TYPE_CHAR;
        case TK_TYPE_F32:   return TYPE_F32;
        case TK_TYPE_F64:   return TYPE_F64;
        default:            return TYPE_UNKNOWN;
    }
}

/* Data type name for display */
const char *datatype_name(DataType t) {
    switch (t) {
        case TYPE_I8: return "i8";       case TYPE_I16: return "i16";
        case TYPE_I32: return "i32";     case TYPE_I64: return "i64";
        case TYPE_I128: return "i128";   case TYPE_ISIZE: return "isize";
        case TYPE_U8: return "u8";       case TYPE_U16: return "u16";
        case TYPE_U32: return "u32";     case TYPE_U64: return "u64";
        case TYPE_U128: return "u128";   case TYPE_USIZE: return "usize";
        case TYPE_BOOL: return "bool";   case TYPE_VOID: return "void";
        case TYPE_STR: return "str";     case TYPE_CHAR: return "char";
        case TYPE_F32: return "f32";     case TYPE_F64: return "f64";
        case TYPE_STRUCT: return "struct"; case TYPE_FUNC: return "func";
        default: return "unknown";
    }
}

