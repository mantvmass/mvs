/*
 * common.h - backend core that is "architecture independent"
 *
 * Contains the state structure (Gen) and helper functions shared by every backend:
 *   - type system (sizes, signed/float, struct layout, type inference)
 *   - variable/function symbol table + variable slot allocation
 *   - reachability analysis (tree-shaking)
 *   - io.out format string conversion
 *
 * Each backend (e.g. arch/x86_64/win.c) just includes this file and writes only the
 * "instruction emission" part for its architecture; the shared code is not duplicated.
 *
 * Note: type_size assumes pointers are 8 bytes (all current targets are 64-bit).
 */
#ifndef MVS_ARCH_COMMON_H
#define MVS_ARCH_COMMON_H

#include <stdio.h>
#include "../ast.h"

/* Table sizes. These are ceilings on ONE program, so they must be generous
 * enough for a real one: a few thousand functions and a few thousand strings
 * is an ordinary size for a project, not an abuse. Every table lives inside
 * Gen, which the backends keep in static storage (a Gen this size would blow
 * the stack as a local). Exceeding any of them is a diagnosed error, never
 * silent truncation. */
#define MAX_SYM   4096   /* locals per function (a pre-pass reserves them all) */
#define MAX_FUNC  2048   /* functions, and separately structs, per program */
#define MAX_STR   4096   /* distinct string literals per program */
#define MAX_LOOP  64
#define MAX_ARGS  4   /* supports 4 register arguments (extras go on the stack) */
#define LABEL_MAX 720 /* label buffer size (fits long generic instance names); globals get prefix mvs_gv_ vs funcs */

/* Symbol table entry for variables */
typedef struct {
    char    *name;
    DataType type;
    int      ptr;       /* pointer depth (0 = not a pointer) */
    int      arr;       /* array length for [T; N] (0 = not an array); type/ptr describe the element */
    char    *sname;     /* struct name when type == TYPE_STRUCT */
    int      size;      /* size of the variable in bytes (element size * length for arrays) */
    int      is_global; /* 1 = global variable, 0 = local variable on the stack */
    int      offset;    /* for locals: distance from rbp (positive, used as [rbp - offset]) */
    Node    *sig;       /* signature when type == TYPE_FUNC (function pointer), else NULL */
} Sym;

/* One field inside a struct (with offset and size, used when accessing members) */
typedef struct {
    char    *name;
    DataType type;
    int      ptr;
    int      arr;       /* array length for [T; N] fields (0 = not an array) */
    char    *sname;     /* struct name when the field is a nested struct */
    int      offset;    /* offset of the field within the struct (bytes) */
    int      size;      /* size of the field (bytes; element size * length for arrays) */
    Node    *sig;       /* signature when the field is TYPE_FUNC (function pointer), else NULL */
} Field;

/* Layout information for one struct type */
typedef struct {
    char  *name;
    char  *display;     /* name shown by io.out: "Pair<i64,str>" for a generic
                         * instance whose internal name is the mangled "Pair__i64_str" */
    Field  fields[64];
    int    nfields;
    int    size;        /* total size of the struct (bytes) */
} StructInfo;

/* Type of an expression (tracks type/size/pointer depth during codegen) */
typedef struct {
    DataType base;
    int      ptr;
    char    *sname;
    Node    *sig;       /* signature when base == TYPE_FUNC (function pointer), else NULL */
    int      arr;       /* array length when the expression names a whole [T; N] (0 = not an array) */
} ExprType;

/* All backend state gathered into one structure (the compiler processes one file at a time) */
typedef struct {
    FILE *out;
    Node *program;          /* the whole merged program (used to look up traits for dyn dispatch) */

    Sym  locals[MAX_SYM];   int nlocals;   /* variables of the current function (all reserved in a pre-pass) */
    Sym  globals[MAX_SYM];  int nglobals;  /* global variables of the whole program */

    /* Stack of variables currently "visible" per scope; indices point into locals[].
     * Implements scope shadowing: an inner-block variable hides the outer one and disappears on block exit. */
    int  visible[MAX_SYM];  int nvisible;

    /* Function table; stores the ND_FUNC nodes for use when generating calls (includes namespace/extern) */
    Node *funcs[MAX_FUNC]; int nfuncs;

    /* Layout table for every struct type */
    StructInfo structs[MAX_FUNC]; int nstructs;

    int sret_off;           /* offset of the hidden-pointer slot for struct-returning functions (0 = none) */

    const char *cur_ns;     /* namespace used to resolve unqualified calls inside the current function */
    int cur_ret_float;      /* 1 if the function being generated returns a float (must return via xmm0) */
    int cur_ret_f32c;       /* 1 if an exported function returns f32 (must convert double -> single for C) */
    int cur_ret_i128;       /* 1 if the function returns i128/u128 (goes through the hidden sret pointer) */
    const char *cur_ret_dyn;/* trait name if the function returns dyn Trait (hidden sret pointer), else NULL */
    int io_imported;        /* 1 if the io module was imported (gates the built-in io.out function) */

    /* String pool: stores string bytes to be declared in the .data section */
    struct { unsigned char *data; int len; } strs[MAX_STR]; int nstrs;

    /* Loop stack for break/continue (stores label numbers) */
    struct { int brk; int cont; } loops[MAX_LOOP]; int nloops;

    int label_id;           /* counter for unique labels */
    int need_i128;          /* 1 = emit the 128-bit divmod/print helper routines at the end */
    int need_vtables;       /* 1 = the program uses dyn Trait; emit vtables + keep impl methods */
    int had_error;
} Gen;

/* Is this a full 128-bit integer VALUE (not a pointer to one)?
 * These use the address-as-value convention (like structs): rax holds the
 * address of a 16-byte blob, and arithmetic goes through pair-wise qword ops. */
static inline int is_i128(DataType base, int ptr) {
    return ptr == 0 && (base == TYPE_I128 || base == TYPE_U128);
}

/* Trait object VALUE (dyn Trait): a 16-byte fat pointer {data, vtable}, address-as-value */
static inline int is_dyn(DataType base, int ptr) {
    return ptr == 0 && base == TYPE_DYN;
}

/* Any 16-byte address-as-value kind (i128/u128 or dyn Trait) */
static inline int is_blob16(DataType base, int ptr) {
    return is_i128(base, ptr) || is_dyn(base, ptr);
}

/* --- label counter and symbol naming (rules shared by all backends) --- */
int   new_label(Gen *g);
void  func_label_of(Node *fn, char *buf);   /* label name of a function (extern/export = raw name) */
void  global_label(const char *name, char *buf);

/* --- function / variable / string tables --- */
Node *find_func(Gen *g, const char *ns, const char *name);
Sym  *find_var(Gen *g, const char *name);
int   intern_string(Gen *g, const char *data, int len);

/* Runtime bounds checking for [T; N] indexing with a non-constant index.
 * 1 = emit a check that traps on an out-of-range index (the default; constant
 * indices are already rejected at compile time), 0 = trust the program
 * (`--no-check`). A trap is used instead of a message so the same code works
 * hosted and under --nostd with no runtime dependency at all. */
extern int mvs_bounds_checks;

/* Debug line information (`-g`). When set, every statement is preceded by a
 * line directive naming the .mvs file and line, so the assembler builds a
 * DWARF line table and a debugger steps through MVS source, not assembly.
 * NASM targets use `%line`, the GNU-as target uses `.loc`. */
extern int mvs_debug_lines;

/* Emit the line directive for a node (no-op when -g is off or the node has no
 * position). Backend-specific syntax lives in each backend's emit_line. */
void emit_line_directive(FILE *out, const Node *n, int gnu_as);

/* --- type system and struct layout --- */
StructInfo *find_struct(Gen *g, const char *name);
int   type_size(Gen *g, DataType base, int ptr, const char *sname);
int   is_float_type(DataType t);
int   is_signed_type(DataType t);
void  register_struct(Gen *g, Node *decl);
void  layout_structs(Gen *g);   /* compute offset/size of every struct (fixpoint, supports forward references) */
Field *find_field(StructInfo *s, const char *name);
ExprType type_of(Gen *g, Node *n);
DataType infer_type(Gen *g, Node *n);
/* Signature if the callee is a function pointer value (TYPE_FUNC variable/field), else NULL (= direct call) */
Node *expr_func_sig(Gen *g, Node *callee);

/* --- convert an io.out format string (Rust-style {}) into a C printf format ---
 * Fills vals[] = the values actually passed to printf (structs expanded field by field), *out_nv = count */
char *build_c_format(Gen *g, Node *call, const char *fmt, int *out_len, int *out_nph,
                     Node **vals, int *out_nv, int vals_cap);

/* --- reserving variable space on the stack (frame layout) --- */
int   add_local(Gen *g, const char *name, DataType type, int ptr, int arr, char *sname, Node *sig, int *frame);
void  collect_locals(Gen *g, Node *n, int *frame);
void  collect_struct_temps(Gen *g, Node *n, int *frame);  /* reserve temps for struct-returning calls (rvalues) */

/* --- reachability analysis (tree-shaking) --- */
int   func_index(Gen *g, Node *f);
void  reach_func(Gen *g, int idx, char *reached);
void  reach_node(Gen *g, Node *n, const char *ns, char *reached);

#endif /* MVS_ARCH_COMMON_H */
