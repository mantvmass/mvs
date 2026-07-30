/*
 * ast.h - Abstract Syntax Tree definitions
 *
 * The parser builds this tree from tokens, then codegen walks it to emit assembly.
 * A single Node type represents every node kind, distinguished by the kind field.
 * Which fields apply to which kind is documented in each NodeKind's comment.
 */
#ifndef MVS_AST_H
#define MVS_AST_H

#include "token.h"

/* Data types of the MVS language */
typedef enum {
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64, TYPE_I128, TYPE_ISIZE,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64, TYPE_U128, TYPE_USIZE,
    TYPE_BOOL, TYPE_VOID, TYPE_STR, TYPE_CHAR, TYPE_F32, TYPE_F64,
    TYPE_STRUCT, TYPE_FUNC,
    TYPE_DYN,      /* trait object `dyn Trait`: a 16-byte fat pointer {data, vtable};
                    * type_name holds the trait name; uses address-as-value like structs */
    TYPE_UNKNOWN
} DataType;

/* Kinds of AST nodes */
typedef enum {
    /* --- expressions --- */
    ND_INT,        /* integer constant: uses int_val */
    ND_FLOAT,      /* float constant: int_val holds the bit pattern of a double (IEEE-754) */
    ND_STR,        /* string constant: uses str_val, str_len */
    ND_CHAR,       /* character constant: uses int_val */
    ND_BOOL,       /* boolean constant: uses int_val (0/1) */
    ND_IDENT,      /* variable reference: uses name */
    ND_BINARY,     /* binary operation: uses op, lhs, rhs */
    ND_UNARY,      /* unary operation: uses op, operand (e.g. -x, !x) */
    ND_CALL,       /* function call: uses name (function name), items (arguments) */
    ND_MEMBER,     /* member access a.b: uses operand (base), name (member name) */
    ND_ASSIGN,     /* assignment: uses op, lhs (target), rhs (value) */
    ND_CAST,       /* type cast x as T: uses operand (value), type/ptr/type_name (target type) */
    ND_INDEX,      /* indexing a[i]: lhs = base (array or pointer), rhs = index */
    ND_ARRAY_LIT,  /* array literal [e1, e2, ...]: items = elements (only valid as an initializer) */
    ND_FRAMEREF,   /* lvalue at a temp frame slot [rbp - int_val] (internal to codegen io.out) */

    /* --- statements --- */
    ND_VAR_DECL,   /* variable declaration: uses name, type, operand (initializer), is_const */
    ND_EXPR_STMT,  /* expression statement: uses operand */
    ND_RETURN,     /* return: uses operand (return value, may be NULL) */
    ND_IF,         /* if/elseif/else: uses cond, then_branch, else_branch */
    ND_WHILE,      /* while: uses cond, body */
    ND_FOR,        /* for: uses init, cond, step, body */
    ND_BLOCK,      /* block { ... }: uses items (statement list) */
    ND_BREAK,      /* break */
    ND_CONTINUE,   /* continue */
    ND_DOWHILE,    /* do-while: uses body, cond */
    ND_SWITCH,     /* switch: uses cond (value compared), items (list of ND_CASE) */
    ND_CASE,       /* case/default: uses operand (value compared, NULL = default), items (case statements) */

    /* --- struct --- */
    ND_STRUCT_DECL,/* struct declaration: uses name, items (fields as ND_PARAM) */
    ND_STRUCT_LIT, /* struct literal: uses name (struct name), items (field inits as ND_ASSIGN) */

    /* --- top level --- */
    ND_PARAM,      /* function parameter/struct field: uses name, type, ptr, type_name;
                    *  operand = default value expression (parameters only, NULL = no default) */
    ND_FUNC,       /* function definition: uses name, type (return type), items (parameters), body.
                    *  If is_extern = 1 and body = NULL, this declares a foreign function */
    ND_IMPORT,     /* module import: uses str_val (path), items (imported names as ND_IDENT) */
    ND_TRAIT,      /* trait definition: uses name (trait name), items (method signatures as bodyless ND_FUNC) */
    ND_TRAIT_IMPL, /* marker that a type impls a trait: uses name (trait name), type_name (type name) */
    ND_PROGRAM     /* whole program: uses items (list of top-level definitions) */
} NodeKind;

/* One node of the AST */
typedef struct Node {
    NodeKind kind;        /* node kind */
    int      line;        /* source line, used in error reports */
    int      col;         /* source column of the token the node was created at (0 = unknown) */
    const char *file;     /* source file name (a stable pointer from diag_register_source) */

    DataType type;        /* data type of the node (for exprs) or return type (for funcs) */
    int      ptr;         /* pointer depth (0 = not a pointer, 1 = *T, 2 = **T) */
    int      arr;         /* array length for [T; N] declarations (0 = not an array);
                           * type/ptr/type_name then describe the ELEMENT type */
    char    *type_name;   /* struct name when type == TYPE_STRUCT */
    struct Node *sig;     /* signature when type == TYPE_FUNC (function pointer):
                           *   an ND_FUNC-style node; items = parameters (ND_PARAM),
                           *   type/ptr/type_name = the return type */

    /* constants */
    long long int_val;    /* used by ND_INT, ND_CHAR, ND_BOOL */
    char     *str_val;    /* used by ND_STR */
    int       str_len;    /* string length */

    /* names */
    char     *name;       /* variable/function/member/parameter name */

    /* operators */
    TokenType op;         /* used by ND_BINARY, ND_UNARY, ND_ASSIGN */

    /* child nodes (meaning depends on kind; see comments above) */
    struct Node *lhs;     /* left operand (binary/assign) */
    struct Node *rhs;     /* right operand (binary/assign) */
    struct Node *operand; /* single child (unary/return/member base/var init/expr-stmt) */
    struct Node *cond;        /* condition (if/while/for) */
    struct Node *then_branch; /* then branch (if) */
    struct Node *else_branch; /* else branch (if); may be the next ND_IF for elseif */
    struct Node *init;        /* init clause (for) */
    struct Node *step;        /* step clause (for) */
    struct Node *body;        /* loop/function body (while/for/func) */

    /* unbounded child list (block stmts / call args / func params / program decls) */
    struct Node **items;
    int           nitems;
    int           paren;      /* 1 = this expression was written in parentheses; used by the parser
                               * so (-2) ** 2 keeps the unary minus inside while -2 ** 2 = -(2 ** 2) */
    int           is_const;   /* used by ND_VAR_DECL: 1 = const, 0 = let */
    int           variadic;   /* ND_FUNC: takes a variadic tail; ND_PARAM: this is the variadic
                               * slice parameter (typed pointer-to-element; a hidden usize
                               * parameter named <name>_len follows it with the count) */
    int           is_extern;  /* used by ND_FUNC: 1 = foreign function (no body) */
    int           is_export;  /* used by ND_FUNC: 1 = exported for C to call (raw symbol name) */
    int           is_method;  /* used by ND_FUNC: 1 = struct method (ns = struct name) */
    char         *gen[4];     /* used by ND_FUNC: generic type parameter names (e.g. "T") */
    char         *gen_bound[4];/* used by ND_FUNC: trait bound per generic param (NULL = none), e.g. <T: Display> */
    int           ngen;       /* number of generic type parameters (0 = normal function) */
    char         *ns;         /* namespace for the "symbol label" (module for plain funcs, struct name for methods) */
    char         *mod;        /* namespace of the "module" the function belongs to; resolves unqualified calls inside methods */

    /* conditional compilation: set by an @compile(...) attribute on a top-level item.
     * NULL = unconditional. The module loader drops items whose values do not match
     * the current target (AND semantics when both are set). */
    char         *cfg_os;     /* required target OS, e.g. "windows" or "linux" */
    char         *cfg_arch;   /* required target arch, e.g. "x86_64" or "aarch64" */
    int           is_test;    /* ND_FUNC: marked with @test; --test-main calls it from the
                               * generated main (functions named test_* count as well) */
} Node;

/* --- node construction helpers --- */
Node *node_new(NodeKind kind, int line);
/* Source position for nodes created from here on (set by the parser per file / per token);
 * node_new stamps these into every new node so diagnostics can point at real source */
void  ast_set_file(const char *file);
void  ast_set_col(int col);
/* Append a child node to items[] (grows the array automatically) */
void  node_add_item(Node *parent, Node *child);
/* Deep-copy an AST tree (used when instantiating a generic function) */
Node *node_clone(Node *n);

/* --- data type helpers --- */
/* Convert a type token (e.g. TK_TYPE_I32) to a DataType */
DataType datatype_from_token(TokenType t);
/* Data type name for display/debug */
const char *datatype_name(DataType t);
/* Inverse of datatype_name: "i64" -> TYPE_I64; TYPE_UNKNOWN when the name is no primitive.
 * Used for impl-on-primitive (`impl Display for i64` stores the type as its name string) */
DataType datatype_from_name(const char *name);

#endif /* MVS_AST_H */
