/*
 * ir.h - intermediate representation between the AST and the backends
 *
 * Three-address code with virtual registers (unlimited, numbered).
 * Linear, not SSA: labels and conditional/unconditional jumps for control
 * flow. Each instruction carries enough type info that the backend never
 * calls type_of().
 *
 * The lowering pass (lower.c) walks the AST once, producing an IRFunc per
 * function. Each backend then translates that IRFunc to assembly. This
 * is the only code path (there is no AST-walk fallback).
 */
#ifndef MVS_IR_H
#define MVS_IR_H

#include "ast.h"

/* A virtual register: just an integer id. -1 means "no register" (void
 * result, or unused operand). The lowering pass allocates them upward
 * from 0; the backend maps them to physical locations. */
#define IR_NONE (-1)

/* Comparison sub-operations, shared by IR_CMP and IR_FCMP */
typedef enum {
    CMP_EQ, CMP_NE,
    CMP_SLT, CMP_SGT, CMP_SLE, CMP_SGE,   /* signed integer */
    CMP_ULT, CMP_UGT, CMP_ULE, CMP_UGE,   /* unsigned integer */
    CMP_FLT, CMP_FGT, CMP_FLE, CMP_FGE,   /* float (unordered compare) */
    CMP_FEQ, CMP_FNE
} IRCmpOp;

/* Atomic sub-operations for IR_ATOMIC */
typedef enum {
    ATOMIC_LOAD, ATOMIC_STORE, ATOMIC_ADD, ATOMIC_SUB, ATOMIC_CAS, ATOMIC_XCHG
} IRAtomicOp;

/* 128-bit arithmetic sub-operations */
typedef enum {
    I128_ADD, I128_SUB, I128_MUL, I128_SDIV, I128_UDIV, I128_SMOD, I128_UMOD,
    I128_AND, I128_OR, I128_XOR, I128_SHL, I128_SHR,
    I128_NEG, I128_NOT
} IR128Op;

typedef enum {
    /* --- constants and addresses --- */
    IR_IMM,         /* dst = imm (integer constant or float bit-pattern) */
    IR_STR,         /* dst = &string_pool[imm] */
    IR_FUNC_ADDR,   /* dst = &function[name] (function pointer value) */
    IR_LOCAL_ADDR,  /* dst = frame_base + imm (address of a local variable) */
    IR_GLOBAL_ADDR, /* dst = &global[name] */

    /* --- memory --- */
    IR_LOAD,        /* dst = load(a), typed by (type, ptr, size, is_signed) */
    IR_STORE,       /* store(a=addr, b=value), typed by (type, ptr, size) */
    IR_COPY,        /* memcpy(a=dst_addr, b=src_addr, imm=byte_count) */

    /* --- address arithmetic --- */
    IR_OFFSET,      /* dst = a + imm (struct field: base address + byte offset) */
    IR_INDEX,       /* dst = a + b * imm (array: base + index * element_size) */

    /* --- integer arithmetic --- */
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_SDIV,        /* signed division */
    IR_UDIV,        /* unsigned division */
    IR_SMOD,        /* signed modulo */
    IR_UMOD,        /* unsigned modulo */
    IR_SHL,         /* shift left */
    IR_SAR,         /* shift arithmetic right (sign-extending) */
    IR_SHR,         /* shift logical right (zero-extending) */
    IR_BAND,        /* bitwise AND */
    IR_BOR,         /* bitwise OR */
    IR_BXOR,        /* bitwise XOR */
    IR_NEG,         /* dst = -a */
    IR_BNOT,        /* dst = ~a */

    /* --- integer comparison (result is 0 or 1) --- */
    IR_CMP,         /* dst = (a cmp b), sub-op in cmp field */

    /* --- float arithmetic (operands are double bit-patterns in i64) --- */
    IR_FADD,
    IR_FSUB,
    IR_FMUL,
    IR_FDIV,
    IR_FNEG,        /* dst = -a (negate double) */
    IR_FPOW,        /* dst = a ** b (float base, integer exponent) */

    /* --- float comparison --- */
    IR_FCMP,        /* dst = (a fcmp b), sub-op in cmp field */

    /* --- type conversion --- */
    IR_ITOF,        /* dst = (double)a (signed i64 to f64 bits) */
    IR_UTOF,        /* dst = (double)a (unsigned u64 to f64 bits) */
    IR_FTOI,        /* dst = (i64)a (f64 bits to signed integer) */
    IR_SEXT,        /* dst = sign_extend(a, from=size, to=imm) */
    IR_ZEXT,        /* dst = zero_extend(a, from=size, to=imm) */
    IR_TRUNC,       /* dst = truncate(a, to_size=imm) */
    IR_F32_F64,     /* dst = widen single to double (movd+cvtss2sd) */
    IR_F64_F32,     /* dst = narrow double to single (cvtsd2ss+movd) */
    IR_BOOL,        /* dst = (a != 0) ? 1 : 0 (normalize to boolean) */

    /* --- control flow --- */
    IR_LABEL,       /* emit label (label field) */
    IR_BR,          /* if (a != 0) goto label else goto label2 */
    IR_JMP,         /* goto label */
    IR_RET,         /* return a (IR_NONE = void return) */

    /* --- function calls --- */
    IR_CALL,        /* dst = call name(args...) [direct call] */
    IR_ICALL,       /* dst = call a(args...) [indirect through vreg] */

    /* --- intrinsics that stay high-level in the IR --- */
    IR_ASM,         /* inline assembly: node->items[] are string literals,
                     * each emitted verbatim as one line of assembly */
    IR_ATOMIC,      /* atomic operation: atomic_op = sub-op, a = ptr vreg,
                     * b = val vreg (store/add/sub/xchg/cas:desired),
                     * label = expected vreg (cas only), dst = old value */

    /* --- 128-bit integer (address-as-value, 16-byte blobs) --- */
    IR_I128_BINOP,  /* dst_addr = op(a_addr, b_addr); sub-op in i128_op */
    IR_I128_UNOP,   /* dst_addr = op(a_addr); sub-op in i128_op */
    IR_I128_CMP,    /* dst = cmp(a_addr, b_addr); sub-op in cmp field */
    IR_I128_FROM,   /* dst_addr = widen(a, from_type); converts i64/u64/f64 to i128 */
    IR_I128_STORE,  /* store 128-bit: memcpy(a=dst_addr, b=src_addr_or_val, imm=from_type_tag) */
    IR_I128_LOAD,   /* dst = load_qword(a_addr, imm=0 for low, 1 for high) */

    /* --- dyn Trait (16-byte fat pointer {data, vtable}) --- */
    IR_DYN_WRAP,    /* dst_addr = wrap(a=data, b=vtable_label_idx); name = trait */
    IR_DYN_STORE,   /* store dyn: a=dst_addr, b=value; name=trait, sname=concrete type */
    IR_DYN_CALL,    /* dst = vtable dispatch; carries call info like IR_CALL */

    /* --- debug --- */
    IR_LINE,        /* source location for -g DWARF; line and file fields */

    /* --- misc --- */
    IR_MOV,         /* dst = a (register-to-register move; used by lowering for clarity) */
    IR_NOP
} IROp;

/* One typed argument in a function call */
typedef struct {
    int      vreg;      /* the virtual register holding the argument value */
    DataType type;
    int      ptr;
    char    *sname;     /* struct name when type == TYPE_STRUCT */
    int      size;      /* byte size */
    Node    *sig;       /* function pointer signature, if TYPE_FUNC */
    long long blob_off; /* frame offset of the dyn-wrapping blob (0 = none) */
} IRArg;

/* Pool capacity for call arguments per function */
#define IR_MAX_ARGS_POOL 4096

/* One IR instruction */
typedef struct {
    IROp     op;

    int      dst;       /* destination vreg (IR_NONE = no result) */
    int      a, b;      /* source operand vregs (IR_NONE = unused) */
    long long imm;      /* immediate value, byte offset, byte count, etc. */

    /* type annotation (describes the VALUE being operated on) */
    DataType type;
    int      ptr;
    char    *sname;
    int      size;      /* byte size for loads/stores/copies */
    int      is_signed; /* 1 = signed integer operation */

    IRCmpOp  cmp;       /* comparison sub-op (for IR_CMP/IR_FCMP/IR_I128_CMP) */
    IR128Op  i128_op;   /* 128-bit arithmetic sub-op */
    IRAtomicOp atomic_op; /* atomic sub-op (for IR_ATOMIC) */

    /* control flow */
    int      label;     /* label number (for IR_LABEL, IR_JMP, IR_BR true) */
    int      label2;    /* IR_BR false target */

    /* names */
    char    *name;      /* function label, global name, trait name, etc. */

    /* call arguments: index and count into IRFunc.args[] pool */
    int      arg_start;
    int      nargs;

    /* the function signature for calls (carries is_extern, is_export,
     * return type, parameter types for ABI decisions in the backend) */
    Node    *sig;

    /* for IR_ASM / IR_ATOMIC: the original AST node */
    Node    *node;

    /* source location (for IR_LINE and for error reports) */
    int      line;
    const char *file;
} IRInst;

/* One function lowered to IR */
typedef struct {
    char    *name;          /* function label */
    IRInst  *insts;         /* instruction array (dynamic) */
    int      ninsts;
    int      cap;           /* allocated capacity for insts[] */

    int      next_vreg;     /* counter for virtual registers */
    int      next_label;    /* counter for labels */

    /* call argument pool: all IRArg entries for all calls in this function */
    IRArg   *args;
    int      nargs;
    int      args_cap;

    /* the AST function node (for the emitter to read metadata: is_export, etc.) */
    Node    *fn;

    int      frame_size;    /* locals frame size (from add_local/collect_locals); the emitter
                             * extends this by next_vreg * 8 for the vreg spill area */
    int      n_i128_temps;  /* 16-byte temporary slots for i128 operations; the emitter
                             * adds n_i128_temps * 16 beyond the vreg area */
    int      param_start;   /* index of first parameter in g->locals[] */
    int      nparams;       /* number of parameters */
} IRFunc;

/* --- construction helpers --- */

/* Create a new empty IRFunc for the given function */
IRFunc *ir_func_new(const char *name, Node *fn);

/* Free an IRFunc and its instruction/arg arrays */
void ir_func_free(IRFunc *f);

/* Allocate a fresh virtual register */
int ir_vreg(IRFunc *f);

/* Allocate a fresh label number */
int ir_label(IRFunc *f);

/* Append an instruction to the function; returns a pointer to the
 * appended instruction so the caller can fill in extra fields */
IRInst *ir_emit(IRFunc *f, IROp op, int dst, int a, int b);

/* Push a call argument into the pool; returns the index.
 * Call ir_arg_start() before the first arg, ir_arg() for each,
 * then store (start, count) on the IR_CALL instruction. */
int ir_arg_start(IRFunc *f);
int ir_arg(IRFunc *f, int vreg, DataType type, int ptr, char *sname, int size);

/* --- debug --- */

/* Print the IR for a function to stderr (for development) */
void ir_dump(IRFunc *f);

/* Name of an IR opcode (for debug printing) */
const char *ir_op_name(IROp op);

#endif /* MVS_IR_H */
