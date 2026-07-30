/*
 * arch/x86_64/win.c - backend emitting NASM (Intel) assembly for x86-64 Windows (win64 ABI)
 *
 * This file holds only the architecture-dependent parts: instruction emission, registers,
 * and the calling convention (win64). Shared parts (type system, struct layout, symbol table,
 * reachability, format) live in arch/common.c
 *
 * win64 ABI: first 4 integer arguments -> rcx, rdx, r8, r9; extras go on the stack;
 *            reserve 32 bytes of shadow space before a call; rsp is 16-byte aligned at the call
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "../common.h"

/* ---------- code emission helpers ---------- */

/* Push the value in rax onto a temporary stack slot; uses 16 bytes to preserve 16-byte alignment */
static void push_tmp(Gen *g) {
    fprintf(g->out, "    sub rsp, 16\n    mov [rsp], rax\n");
}
/* Pop the value from the temporary stack slot back into register reg */
static void pop_tmp(Gen *g, const char *reg) {
    fprintf(g->out, "    mov %s, [rsp]\n    add rsp, 16\n", reg);
}

/* Copy size bytes of memory: src is in r10, dst is in r11.
 * Uses only volatile registers (r10/r11/rcx/rax); never touches rsi/rdi, which win64 treats as
 * callee-saved, and never clashes with argument registers (rcx/rdx/r8/r9) that may still hold
 * parameters not yet saved */
static void gen_memcpy(Gen *g, int size) {
    int l = new_label(g);
    fprintf(g->out,
        "    mov ecx, %d\n"           /* counter (zero-extended into rcx) */
        ".Lcpy%d:\n"
        "    test rcx, rcx\n"
        "    jz .Lcpyend%d\n"
        "    mov al, [r10]\n"
        "    mov [r11], al\n"
        "    inc r10\n    inc r11\n    dec rcx\n"
        "    jmp .Lcpy%d\n"
        ".Lcpyend%d:\n", size, l, l, l, l);
}

/* forward declarations */
static void gen_expr(Gen *g, Node *n);
static void gen_stmt(Gen *g, Node *n);
static void gen_addr(Gen *g, Node *n);
static void gen_store_struct(Gen *g, Node *value);
static void gen_call(Gen *g, Node *n, int has_sret);

/* Load the value at [rax] (rax = address) by size/signedness; result goes into rax */
static void gen_load_at(Gen *g, int size, int is_signed) {
    switch (size) {
        case 1: fprintf(g->out, is_signed ? "    movsx rax, byte [rax]\n"  : "    movzx rax, byte [rax]\n"); break;
        case 2: fprintf(g->out, is_signed ? "    movsx rax, word [rax]\n"  : "    movzx rax, word [rax]\n"); break;
        case 4: fprintf(g->out, is_signed ? "    movsxd rax, dword [rax]\n" : "    mov eax, dword [rax]\n"); break;
        default: fprintf(g->out, "    mov rax, [rax]\n"); break;
    }
}

/* Store the value in rcx to [rax] (rax = address) by size (uses sub-registers of rcx) */
static void gen_store_at(Gen *g, int size) {
    switch (size) {
        case 1: fprintf(g->out, "    mov [rax], cl\n"); break;
        case 2: fprintf(g->out, "    mov [rax], cx\n"); break;
        case 4: fprintf(g->out, "    mov [rax], ecx\n"); break;
        default: fprintf(g->out, "    mov [rax], rcx\n"); break;
    }
}

/* Load the value at [rax] by type, result in rax (internally always double)
 *   - f32: read a 4-byte single, then convert to double  -> bit-pattern in rax
 *   - f64: read 8 bytes directly (already double bits)
 *   - integers: by size/signedness (movsx/movzx) */
static void gen_load_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    movd xmm0, dword [rax]\n    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n");
    } else {
        gen_load_at(g, size, is_signed_type(base));
    }
}

/* Store the value in rcx (double bits for floats) to [rax] by type
 *   - f32: convert double -> single, then write 4 bytes
 *   - f64/integers: write by size */
static void gen_store_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    movq xmm0, rcx\n    cvtsd2ss xmm0, xmm0\n    movd dword [rax], xmm0\n");
    } else {
        gen_store_at(g, size);
    }
}

/* Load a variable's value into rax (struct value -> its address, scalar -> value by size/type) */
static void gen_load_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    lea rax, [rel %s]\n", lbl); }
    else              { fprintf(g->out, "    lea rax, [rbp - %d]\n", s->offset); }
    if (s->type == TYPE_STRUCT && s->ptr == 0) return; /* struct: keep the address in rax */
    gen_load_typed(g, s->ptr > 0 ? TYPE_USIZE : s->type, s->size); /* pointer = load 8 bytes */
}

/* Store the value in rax into a variable (truncate by size / convert for f32 by type) */
static void gen_store_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    if (s->ptr == 0 && s->type == TYPE_F32) {
        /* convert the double in rax -> single, then write 4 bytes */
        fprintf(g->out, "    movq xmm0, rax\n    cvtsd2ss xmm0, xmm0\n");
        if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    movd dword [rel %s], xmm0\n", lbl); }
        else              { fprintf(g->out, "    movd dword [rbp - %d], xmm0\n", s->offset); }
        return;
    }
    const char *reg = s->size == 1 ? "al" : s->size == 2 ? "ax" : s->size == 4 ? "eax" : "rax";
    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    mov [rel %s], %s\n", lbl, reg); }
    else              { fprintf(g->out, "    mov [rbp - %d], %s\n", s->offset, reg); }
}

/* Implicitly convert the value in rax when crossing the int<->float line (e.g. let x: f64 = 5;
 * passing an int to an f64 parameter; returning an int from a float-returning function), matching
 * the promotion done in mixed expressions (never emit garbage) */
static void gen_coerce_num(Gen *g, ExprType from, ExprType to) {
    if (from.ptr > 0 || to.ptr > 0) return;
    int ff = is_float_type(from.base), tf = is_float_type(to.base);
    if (ff == tf) return;
    if (!ff && tf) fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n");  /* int -> double */
    else           fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* double -> int */
}

/* Put the address of an lvalue into rax (used for &x, assignments, member access) */
static void gen_addr(Gen *g, Node *n) {
    switch (n->kind) {
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (!s) { fprintf(stderr, "codegen error: undefined variable '%s'\n", n->name); g->had_error = 1; return; }
            char lbl[LABEL_MAX];
            if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    lea rax, [rel %s]\n", lbl); }
            else              { fprintf(g->out, "    lea rax, [rbp - %d]\n", s->offset); }
            break;
        }
        case ND_FRAMEREF:   /* address of a reserved temp slot (used by io.out to materialize a struct once) */
            fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val);
            break;
        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *s = find_struct(g, bt.sname);
            if (!s) { fprintf(stderr, "codegen error: member access on non-struct\n"); g->had_error = 1; return; }
            Field *f = find_field(s, n->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", n->name, s->name); g->had_error = 1; return; }
            if (bt.ptr > 0) gen_expr(g, n->operand);  /* base is pointer-to-struct: use its value as address */
            else            gen_addr(g, n->operand);  /* base is a struct value: use its address */
            if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
            break;
        }
        case ND_UNARY:
            if (n->op == TK_STAR) { gen_expr(g, n->operand); break; } /* *p: the pointer's value is the address */
            fprintf(stderr, "codegen error: expression is not an lvalue\n");
            g->had_error = 1;
            break;
        case ND_CALL: {
            /* address of a struct result from a function: materialize into a temp slot first (e.g. make().field) */
            ExprType rt = type_of(g, n);
            if (rt.base == TYPE_STRUCT && rt.ptr == 0 && n->int_val) {
                fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val);
                gen_call(g, n, 1);
                fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val);
            } else {
                fprintf(stderr, "codegen error: cannot take the address of this function result\n");
                g->had_error = 1;
            }
            break;
        }
        default:
            fprintf(stderr, "codegen error: expression is not an lvalue\n");
            g->had_error = 1;
    }
}

/* Whether this value counts as unsigned (used to pick unsigned div/setcc)
 * - pointer = address (unsigned); unsigned integer types/bool/char = unsigned */
static int is_unsigned_val(DataType base, int ptr) {
    if (ptr > 0) return 1;
    switch (base) {
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
        case TYPE_BOOL: case TYPE_CHAR:
            return 1;
        default:
            return 0;
    }
}

/* Gen an arithmetic/comparison operation: lhs is in rax, rhs is in rcx
 * uns = 1 uses unsigned instructions (div, setb/seta) for unsigned types */
static void gen_binop_apply(Gen *g, TokenType op, int uns) {
    switch (op) {
        case TK_PLUS:    fprintf(g->out, "    add rax, rcx\n"); break;
        case TK_MINUS:   fprintf(g->out, "    sub rax, rcx\n"); break;
        case TK_STAR:    fprintf(g->out, "    imul rax, rcx\n"); break;  /* low bits identical signed/unsigned */
        /* bitwise operators */
        case TK_AMP:     fprintf(g->out, "    and rax, rcx\n"); break;   /* bitwise AND */
        case TK_PIPE:    fprintf(g->out, "    or rax, rcx\n"); break;    /* bitwise OR */
        case TK_CARET:   fprintf(g->out, "    xor rax, rcx\n"); break;   /* bitwise XOR */
        case TK_SHL:     fprintf(g->out, "    shl rax, cl\n"); break;    /* shift left (count in cl) */
        case TK_SHR:     fprintf(g->out, uns ? "    shr rax, cl\n" : "    sar rax, cl\n"); break; /* right: logical/arith */
        case TK_SLASH:   /* quotient in rax */
            if (uns) fprintf(g->out, "    xor edx, edx\n    div rcx\n");      /* unsigned: zero-extend */
            else     fprintf(g->out, "    cqo\n    idiv rcx\n");             /* signed: sign-extend */
            break;
        case TK_PERCENT: /* remainder in rdx */
            if (uns) fprintf(g->out, "    xor edx, edx\n    div rcx\n    mov rax, rdx\n");
            else     fprintf(g->out, "    cqo\n    idiv rcx\n    mov rax, rdx\n");
            break;
        case TK_STARSTAR: { /* exponentiation by a repeated-multiply loop: rax=base, rcx=exponent */
            int l = new_label(g);
            fprintf(g->out,
                "    mov r8, rax\n"      /* r8 = base */
                "    mov r9, rcx\n"      /* r9 = exponent counter */
                "    mov rax, 1\n"       /* result starts at 1 */
                ".Lpow%d:\n"
                "    test r9, r9\n"
                "    jle .Lpowend%d\n"
                "    imul rax, r8\n"
                "    dec r9\n"
                "    jmp .Lpow%d\n"
                ".Lpowend%d:\n", l, l, l, l);
            break;
        }
        /* comparisons: set 0/1 into rax (pick signed/unsigned setcc) */
        case TK_EQ:  fprintf(g->out, "    cmp rax, rcx\n    sete al\n    movzx rax, al\n"); break;
        case TK_NEQ: fprintf(g->out, "    cmp rax, rcx\n    setne al\n    movzx rax, al\n"); break;
        case TK_LT:  fprintf(g->out, "    cmp rax, rcx\n    %s al\n    movzx rax, al\n", uns ? "setb"  : "setl");  break;
        case TK_GT:  fprintf(g->out, "    cmp rax, rcx\n    %s al\n    movzx rax, al\n", uns ? "seta"  : "setg");  break;
        case TK_LE:  fprintf(g->out, "    cmp rax, rcx\n    %s al\n    movzx rax, al\n", uns ? "setbe" : "setle"); break;
        case TK_GE:  fprintf(g->out, "    cmp rax, rcx\n    %s al\n    movzx rax, al\n", uns ? "setae" : "setge"); break;
        default:
            fprintf(stderr, "codegen error: unknown binary operator\n");
            g->had_error = 1;
    }
}


/* Gen an expression; the result is always in rax */
static void gen_expr(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_INT:
        case ND_FLOAT:   /* the double value is stored as a bit-pattern in int_val */
        case ND_CHAR:
        case ND_BOOL:
            fprintf(g->out, "    mov rax, %lld\n", n->int_val);
            break;
        case ND_STR: {
            int idx = intern_string(g, n->str_val, n->str_len);
            fprintf(g->out, "    lea rax, [rel mvs_str_%d]\n", idx);
            break;
        }
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (s) { gen_load_var(g, s); break; }
            /* not a variable: if it is a function name, use it as a function pointer value (label address) */
            Node *f = find_func(g, g->cur_ns ? g->cur_ns : "", n->name);
            if (!f) f = find_func(g, "", n->name);
            if (f) {
                char flbl[LABEL_MAX]; func_label_of(f, flbl);
                fprintf(g->out, "    lea rax, [rel %s]\n", flbl);
                break;
            }
            fprintf(stderr, "codegen error: undefined variable '%s'\n", n->name); g->had_error = 1;
            break;
        }
        case ND_BINARY: {
            /* logical && || use short-circuit evaluation */
            if (n->op == TK_AND || n->op == TK_OR) {
                int lend = new_label(g);
                gen_expr(g, n->lhs);
                fprintf(g->out, "    cmp rax, 0\n");
                if (n->op == TK_AND) fprintf(g->out, "    je .Llog%d\n", lend);   /* left false -> result false */
                else                 fprintf(g->out, "    jne .Llog%d\n", lend);  /* left true -> result true */
                gen_expr(g, n->rhs);
                fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                fprintf(g->out, ".Llog%d:\n", lend);
                if (n->op == TK_AND) {
                    /* if we reached the label because the left side was false, rax is still 0, so it is correct */
                } else {
                    /* for ||, if we jumped here because the left side was true, force the result to 1 */
                    fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                }
                break;
            }
            ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);

            /* floating point operations (use xmm registers)
             * supports + - * / and comparisons; if either side is an int it is converted to double first */
            {
                int fop = ((is_float_type(lt.base) && lt.ptr == 0) || (is_float_type(rt.base) && rt.ptr == 0));
                int is_cmp = (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||n->op==TK_LE||n->op==TK_GE);
                int is_arith = (n->op==TK_PLUS||n->op==TK_MINUS||n->op==TK_STAR||n->op==TK_SLASH);
                /* power with a float base: repeatedly multiply the double by the (integer) exponent in xmm */
                if (fop && n->op == TK_STARSTAR) {
                    gen_expr(g, n->lhs);
                    push_tmp(g);
                    gen_expr(g, n->rhs);                       /* exponent -> rcx (integer) */
                    if (is_float_type(rt.base) && rt.ptr == 0)
                        fprintf(g->out, "    movq xmm2, rax\n    cvttsd2si rcx, xmm2\n");
                    else
                        fprintf(g->out, "    mov rcx, rax\n");
                    pop_tmp(g, "rax");
                    fprintf(g->out, is_float_type(lt.base) ? "    movq xmm1, rax\n" : "    cvtsi2sd xmm1, rax\n"); /* base -> xmm1 */
                    int l = new_label(g);
                    fprintf(g->out,
                        "    mov rax, 0x3FF0000000000000\n    movq xmm0, rax\n"  /* result starts at 1.0 */
                        ".Lfpow%d:\n"
                        "    cmp rcx, 0\n    jle .Lfpowend%d\n"
                        "    mulsd xmm0, xmm1\n    dec rcx\n    jmp .Lfpow%d\n"
                        ".Lfpowend%d:\n"
                        "    movq rax, xmm0\n", l, l, l, l);
                    break;
                }
                if (fop && (is_cmp || is_arith)) {
                    gen_expr(g, n->lhs);
                    push_tmp(g);
                    gen_expr(g, n->rhs);
                    /* rhs -> xmm1 (converted from int if needed) */
                    fprintf(g->out, is_float_type(rt.base) ? "    movq xmm1, rax\n" : "    cvtsi2sd xmm1, rax\n");
                    pop_tmp(g, "rax");
                    fprintf(g->out, is_float_type(lt.base) ? "    movq xmm0, rax\n" : "    cvtsi2sd xmm0, rax\n");
                    if (is_arith) {
                        const char *op = n->op==TK_PLUS ? "addsd" : n->op==TK_MINUS ? "subsd" :
                                         n->op==TK_STAR ? "mulsd" : "divsd";
                        fprintf(g->out, "    %s xmm0, xmm1\n    movq rax, xmm0\n", op);
                    } else {
                        /* compare floats via ucomisd (sets flags unsigned-style) */
                        const char *cc = n->op==TK_EQ ? "sete" : n->op==TK_NEQ ? "setne" :
                                         n->op==TK_LT ? "setb" : n->op==TK_GT ? "seta" :
                                         n->op==TK_LE ? "setbe" : "setae";
                        fprintf(g->out, "    ucomisd xmm0, xmm1\n    %s al\n    movzx rax, al\n", cc);
                    }
                    break;
                }
            }

            /* generic binary operation (integers) */
            gen_expr(g, n->lhs);
            push_tmp(g);
            gen_expr(g, n->rhs);
            fprintf(g->out, "    mov rcx, rax\n"); /* rhs -> rcx */
            pop_tmp(g, "rax");                     /* lhs -> rax */

            /* pointer arithmetic: scale the integer side by the size of the pointee type
             *   ptr + int / ptr - int  -> int *= sizeof(*ptr);  int + ptr -> int *= sizeof(*ptr)
             *   ptr - ptr              -> (byte difference) / sizeof(*ptr) = element count (after the sub) */
            int ptrdiff_scale = 0;
            if (n->op == TK_PLUS || n->op == TK_MINUS) {
                if (lt.ptr > 0 && rt.ptr > 0 && n->op == TK_MINUS) {
                    ptrdiff_scale = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                } else if (lt.ptr > 0 && rt.ptr == 0) {
                    int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                    if (sc != 1) fprintf(g->out, "    imul rcx, %d\n", sc);   /* rhs (int) *= size */
                } else if (n->op == TK_PLUS && rt.ptr > 0 && lt.ptr == 0) {
                    int sc = type_size(g, rt.base, rt.ptr - 1, rt.sname);
                    if (sc != 1) fprintf(g->out, "    imul rax, %d\n", sc);   /* lhs (int) *= size */
                }
            }
            /* pick the signedness matching the operand types:
             *   - shift / div / mod : based on the left operand only (the shifted value / the dividend)
             *     (a shift count is naturally unsigned; OR-ing it in would turn signed >> n into a logical shift)
             *   - comparison : if either side is unsigned, treat as unsigned (so big u64 values are not seen as negative) */
            int uns;
            if (n->op==TK_SHL || n->op==TK_SHR || n->op==TK_SLASH || n->op==TK_PERCENT)
                uns = is_unsigned_val(lt.base, lt.ptr);
            else
                uns = is_unsigned_val(lt.base, lt.ptr) || is_unsigned_val(rt.base, rt.ptr);
            gen_binop_apply(g, n->op, uns);
            /* ptr - ptr : divide the byte difference by the element size to get the element count */
            if (ptrdiff_scale > 1)
                fprintf(g->out, "    cqo\n    mov rcx, %d\n    idiv rcx\n", ptrdiff_scale);
            break;
        }
        case ND_UNARY: {
            if (n->op == TK_MINUS) {
                gen_expr(g, n->operand);
                ExprType ot = type_of(g, n->operand);
                if (is_float_type(ot.base) && ot.ptr == 0)
                    fprintf(g->out, "    mov rcx, 0x8000000000000000\n    xor rax, rcx\n"); /* flip the double's sign bit */
                else
                    fprintf(g->out, "    neg rax\n");
            } else if (n->op == TK_NOT) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    cmp rax, 0\n    sete al\n    movzx rax, al\n");
            } else if (n->op == TK_TILDE) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    not rax\n");   /* bitwise NOT (invert all bits) */
                /* truncate the result to the type's width (e.g. ~(u8)0 = 255, not full 64 bits) */
                ExprType ot = type_of(g, n->operand);
                if (ot.ptr == 0) {
                    int sz = type_size(g, ot.base, 0, ot.sname);
                    int sgn = is_signed_type(ot.base);
                    if (sz == 1)      fprintf(g->out, sgn ? "    movsx rax, al\n"   : "    movzx rax, al\n");
                    else if (sz == 2) fprintf(g->out, sgn ? "    movsx rax, ax\n"   : "    movzx rax, ax\n");
                    else if (sz == 4) fprintf(g->out, sgn ? "    movsxd rax, eax\n" : "    mov eax, eax\n");
                }
            } else if (n->op == TK_AMP) {
                /* &x : address of the lvalue */
                gen_addr(g, n->operand);
            } else if (n->op == TK_STAR) {
                /* *p : read the value at the address the pointer points to */
                gen_expr(g, n->operand);                 /* rax = pointer value = address */
                ExprType pt = type_of(g, n->operand);
                int pptr = pt.ptr > 0 ? pt.ptr - 1 : 0;
                if (pt.base == TYPE_STRUCT && pptr == 0) break; /* pointer-to-struct: keep the address */
                gen_load_typed(g, pptr > 0 ? TYPE_USIZE : pt.base, type_size(g, pt.base, pptr, pt.sname));
            } else if (n->op == TK_PLUSPLUS || n->op == TK_MINUSMINUS) {
                /* increment/decrement a variable: must be an lvalue (ident) */
                if (n->operand->kind != ND_IDENT) {
                    fprintf(stderr, "codegen error: ++/-- requires a variable\n"); g->had_error = 1; break;
                }
                Sym *s = find_var(g, n->operand->name);
                if (!s) { fprintf(stderr, "codegen error: undefined variable '%s'\n", n->operand->name); g->had_error = 1; break; }
                if (is_float_type(s->type) && s->ptr == 0) {
                    fprintf(stderr, "codegen error: '++'/'--' is not supported on floating-point; use 'x = x + 1.0'\n");
                    g->had_error = 1; break;
                }
                gen_load_var(g, s);                                 /* rax = old value (the expression's result) */
                fprintf(g->out, "    mov rcx, rax\n");
                /* a pointer steps by sizeof(*p); other types step by 1 */
                int inc = (s->ptr > 0) ? type_size(g, s->type, s->ptr - 1, s->sname) : 1;
                fprintf(g->out, n->op == TK_PLUSPLUS ? "    add rcx, %d\n" : "    sub rcx, %d\n", inc);
                /* store the new value back into the variable by size, without clobbering rax (old value) */
                const char *reg = s->size == 1 ? "cl" : s->size == 2 ? "cx" : s->size == 4 ? "ecx" : "rcx";
                if (s->is_global) { char lbl[LABEL_MAX]; global_label(s->name, lbl); fprintf(g->out, "    mov [rel %s], %s\n", lbl, reg); }
                else              { fprintf(g->out, "    mov [rbp - %d], %s\n", s->offset, reg); }
            }
            break;
        }
        case ND_CAST: {
            /* explicit type cast: the source value was gen'ed into rax (int = integer bits, float = double bits) */
            gen_expr(g, n->operand);
            ExprType st = type_of(g, n->operand);
            int src_f = is_float_type(st.base) && st.ptr == 0;
            int dst_f = is_float_type(n->type) && n->ptr == 0;
            if (!src_f && dst_f) {
                /* int -> double; u64/usize values >= 2^63 need the unsigned path (cvtsi2sd treats input as signed) */
                if (st.ptr == 0 && is_unsigned_val(st.base, 0) && type_size(g, st.base, 0, NULL) >= 8) {
                    int l = new_label(g);
                    fprintf(g->out,
                        "    test rax, rax\n    js .Lu2d%d\n"
                        "    cvtsi2sd xmm0, rax\n    jmp .Lu2de%d\n"
                        ".Lu2d%d:\n"                                   /* high bit set: (x>>1)|(x&1) then double */
                        "    mov rcx, rax\n    shr rcx, 1\n    and rax, 1\n    or rcx, rax\n"
                        "    cvtsi2sd xmm0, rcx\n    addsd xmm0, xmm0\n"
                        ".Lu2de%d:\n    movq rax, xmm0\n", l, l, l, l);
                } else {
                    fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n");
                }
                break;
            }
            if (src_f && dst_f) break;                            /* float -> float: same value domain (double bits) */
            if (src_f && !dst_f) {
                /* float -> int; u64/usize takes the unsigned path (cvttsd2si overflows at >= 2^63) */
                if (n->ptr == 0 && is_unsigned_val(n->type, 0) && type_size(g, n->type, 0, NULL) >= 8) {
                    int l = new_label(g);
                    fprintf(g->out,
                        "    movq xmm0, rax\n"
                        "    mov rcx, 0x43E0000000000000\n    movq xmm1, rcx\n"  /* 2^63 as a double */
                        "    ucomisd xmm0, xmm1\n    jae .Ld2u%d\n"
                        "    cvttsd2si rax, xmm0\n    jmp .Ld2ue%d\n"
                        ".Ld2u%d:\n"
                        "    subsd xmm0, xmm1\n    cvttsd2si rax, xmm0\n"
                        "    mov rcx, 0x8000000000000000\n    or rax, rcx\n"
                        ".Ld2ue%d:\n", l, l, l, l);
                } else {
                    fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* float -> int: truncate */
                }
            }
            /* cast to bool: normalize to 0/1 (any nonzero -> 1), not just truncating to the low byte */
            if (n->ptr == 0 && n->type == TYPE_BOOL) {
                fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                break;
            }
            /* integer destination (int->int or float->int): adjust width/signedness to match the type */
            if (n->ptr == 0 && !dst_f && n->type != TYPE_STR && n->type != TYPE_VOID && n->type != TYPE_STRUCT) {
                int sz = type_size(g, n->type, 0, NULL);
                int sgn = is_signed_type(n->type);
                if (sz == 1)      fprintf(g->out, sgn ? "    movsx rax, al\n"   : "    movzx rax, al\n");
                else if (sz == 2) fprintf(g->out, sgn ? "    movsx rax, ax\n"   : "    movzx rax, ax\n");
                else if (sz == 4) fprintf(g->out, sgn ? "    movsxd rax, eax\n" : "    mov eax, eax\n");
                /* sz == 8: already full width */
            }
            break;
        }
        case ND_ASSIGN: {
            Node *target = n->lhs;
            /* the target must be an lvalue: ident, member (a.b), or deref (*p) */
            if (target->kind != ND_IDENT && target->kind != ND_MEMBER &&
                !(target->kind == ND_UNARY && target->op == TK_STAR)) {
                fprintf(stderr, "codegen error: invalid assignment target\n"); g->had_error = 1; break;
            }
            ExprType tt = type_of(g, target);
            /* whole-struct assignment (copy/literal) */
            if (tt.base == TYPE_STRUCT && tt.ptr == 0 && n->op == TK_ASSIGN) {
                gen_addr(g, target);          /* rax = destination address */
                gen_store_struct(g, n->rhs);  /* write the struct value to the destination */
                break;
            }
            int sz = type_size(g, tt.base, tt.ptr, tt.sname);
            DataType st = tt.ptr > 0 ? TYPE_USIZE : tt.base;  /* type used for load/store (pointer = 8 bytes) */
            if (n->op == TK_ASSIGN) {
                gen_expr(g, n->rhs);          /* rax = value */
                { ExprType rvt = type_of(g, n->rhs); gen_coerce_num(g, rvt, tt); } /* implicit int<->float */
                push_tmp(g);
                gen_addr(g, target);          /* rax = address */
                pop_tmp(g, "rcx");            /* rcx = value */
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov rax, rcx\n"); /* the expression's result = the assigned value */
            } else {
                /* x op= y : load the old value, compute, store back (computes the address twice) */
                TokenType bop = n->op == TK_PLUS_ASSIGN ? TK_PLUS :
                                n->op == TK_MINUS_ASSIGN ? TK_MINUS :
                                n->op == TK_STAR_ASSIGN ? TK_STAR : TK_SLASH;
                gen_expr(g, n->rhs); push_tmp(g);                 /* save rhs */
                gen_addr(g, target);
                gen_load_typed(g, st, sz);                         /* rax = old value */
                pop_tmp(g, "rcx");                                /* rcx = rhs */
                if (is_float_type(tt.base) && tt.ptr == 0) {
                    /* compute in floating point (old value in xmm0, rhs in xmm1) */
                    ExprType rt2 = type_of(g, n->rhs);
                    fprintf(g->out, "    movq xmm0, rax\n");
                    fprintf(g->out, is_float_type(rt2.base) ? "    movq xmm1, rcx\n" : "    cvtsi2sd xmm1, rcx\n");
                    const char *fop = bop==TK_PLUS ? "addsd" : bop==TK_MINUS ? "subsd" : bop==TK_STAR ? "mulsd" : "divsd";
                    fprintf(g->out, "    %s xmm0, xmm1\n    movq rax, xmm0\n", fop);
                } else {
                    /* pointer += / -= int : scale rhs by the size of the pointee type */
                    if (tt.ptr > 0 && (bop == TK_PLUS || bop == TK_MINUS)) {
                        int sc = type_size(g, tt.base, tt.ptr - 1, tt.sname);
                        if (sc != 1) fprintf(g->out, "    imul rcx, %d\n", sc);
                    }
                    gen_binop_apply(g, bop, is_unsigned_val(tt.base, tt.ptr)); /* rax = old op rhs */
                }
                fprintf(g->out, "    mov rcx, rax\n");
                gen_addr(g, target);                              /* rax = address (again) */
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov rax, rcx\n");
            }
            break;
        }
        case ND_CALL: {
            Node *callee = n->operand;

            /* built-in format function: io.out("...{}...", args...) (Rust style)
             * it is a compiler intrinsic because the format string must be parsed at compile time
             * and a specifier picked per argument type, much like Rust's println! being a macro */
            if (callee->kind == ND_MEMBER && callee->operand->kind == ND_IDENT &&
                strcmp(callee->operand->name, "io") == 0 && strcmp(callee->name, "out") == 0) {
                if (!g->io_imported) {
                    fprintf(stderr, "codegen error: io is not imported (use: import { io } from \"std\";)\n");
                    g->had_error = 1; break;
                }
                if (n->nitems < 1 || n->items[0]->kind != ND_STR) {
                    fprintf(stderr, "codegen error: io.out: first argument must be a string literal\n");
                    g->had_error = 1; break;
                }
                int flen, nph, nv;
                Node *vals[256];   /* the values actually passed to printf (structs expanded per field) */
                char *cf = build_c_format(g, n, n->items[0]->str_val, &flen, &nph, vals, &nv, 256);
                int nvals = n->nitems - 1;
                if (nph != nvals) {
                    fprintf(stderr, "codegen error: io.out: %d placeholder(s) but %d argument(s)\n", nph, nvals);
                    g->had_error = 1; free(cf); break;
                }
                int idx = intern_string(g, cf, flen);
                free(cf);
                /* struct results from functions being printed ({} struct): call/materialize into the temp
                 * slot once, here; all fields then read from that temp (via ND_FRAMEREF), preventing one
                 * function call per field */
                for (int k = 1; k < n->nitems; k++) {
                    Node *a = n->items[k];
                    if (a->kind == ND_CALL && a->int_val) {
                        ExprType at = type_of(g, a);
                        if (at.base == TYPE_STRUCT && at.ptr == 0) gen_expr(g, a); /* fill the temp; discard rax */
                    }
                }
                /* pass to printf per the win64 ABI: format + every value (first 4 in registers, rest on
                 * the stack); printf is variadic: float values must go in both the GPR and the xmm reg */
                int total = 1 + nv;                          /* format + expanded values */
                int nstack = total > 4 ? total - 4 : 0;
                int callspace = 32 + nstack * 8;
                callspace = (callspace + 15) / 16 * 16;
                const char *regs[4] = { "rcx", "rdx", "r8", "r9" };
                fprintf(g->out, "    lea rax, [rel mvs_str_%d]\n", idx);
                push_tmp(g);
                for (int i = 0; i < nv; i++) { gen_expr(g, vals[i]); push_tmp(g); }
                fprintf(g->out, "    sub rsp, %d\n", callspace);
                for (int i = 0; i < total; i++) {
                    int srcoff = callspace + (total - 1 - i) * 16;
                    fprintf(g->out, "    mov rax, [rsp + %d]\n", srcoff);
                    if (i < 4) fprintf(g->out, "    mov %s, rax\n", regs[i]);
                    else       fprintf(g->out, "    mov [rsp + %d], rax\n", 32 + (i - 4) * 8);
                    if (i >= 1 && i < 4 && is_float_type(infer_type(g, vals[i - 1])))
                        fprintf(g->out, "    movq xmm%d, rax\n", i);
                }
                fprintf(g->out, "    call printf\n");
                fprintf(g->out, "    add rsp, %d\n", callspace);
                fprintf(g->out, "    add rsp, %d\n", total * 16);
                break;
            }

            /* ordinary function call: if it returns a struct, materialize into the temp slot and yield
             * the address (a struct uses its address as its value), so a struct result works as an
             * rvalue, e.g. f(make()) */
            {
                ExprType rt = type_of(g, n);
                if (rt.base == TYPE_STRUCT && rt.ptr == 0 && n->int_val) {
                    fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val); /* sret = the temp slot */
                    gen_call(g, n, 1);
                    fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val); /* result = struct address */
                } else {
                    gen_call(g, n, 0);
                }
            }
            break;
        }
        case ND_MEMBER: {
            /* member access a.b as an rvalue: find the field's address, then load by size */
            ExprType et = type_of(g, n);
            gen_addr(g, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) break; /* nested struct field: keep the address */
            gen_load_typed(g, et.ptr > 0 ? TYPE_USIZE : et.base, type_size(g, et.base, et.ptr, et.sname));
            break;
        }
        case ND_FRAMEREF:   /* struct temp slot: the value is its address (a struct uses its address as value) */
            gen_addr(g, n);
            break;
        case ND_STRUCT_LIT:
            fprintf(stderr, "codegen error: struct literal can only initialize a variable or be returned\n");
            g->had_error = 1;
            break;
        default:
            fprintf(stderr, "codegen error: cannot generate expression (kind %d)\n", n->kind);
            g->had_error = 1;
    }
}

/* Gen a statement */
static void gen_stmt(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_VAR_DECL: {
            Sym *s = &g->locals[n->int_val]; /* the slot reserved in the pre-pass (this exact one, not a shadow) */
            if (n->operand && s) { /* has an initializer */
                if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    /* struct variable: write the value (literal/copy/function result) into its space */
                    char lbl[LABEL_MAX];
                    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    lea rax, [rel %s]\n", lbl); }
                    else              { fprintf(g->out, "    lea rax, [rbp - %d]\n", s->offset); }
                    gen_store_struct(g, n->operand);
                } else {
                    gen_expr(g, n->operand);     /* rax = value */
                    ExprType vt = type_of(g, n->operand), dt = { s->type, s->ptr, s->sname, s->sig };
                    gen_coerce_num(g, vt, dt);   /* implicit int<->float conversion if needed */
                    gen_store_var(g, s);
                }
            }
            /* the variable becomes visible after its declaration (so let x = x can refer to the outer one) */
            g->visible[g->nvisible++] = (int)n->int_val;
            break;
        }
        case ND_EXPR_STMT:
            gen_expr(g, n->operand);
            break;
        case ND_RETURN:
            if (g->sret_off != 0 && n->operand) {
                /* function returns a struct: write the value through the hidden pointer, then return it */
                fprintf(g->out, "    mov rax, [rbp - %d]\n", g->sret_off);
                gen_store_struct(g, n->operand);
                fprintf(g->out, "    mov rax, [rbp - %d]\n", g->sret_off);
            } else if (n->operand) {
                gen_expr(g, n->operand);         /* the return value is in rax */
                ExprType vt = type_of(g, n->operand);
                int vf = is_float_type(vt.base) && vt.ptr == 0;
                if (g->cur_ret_float && !vf) fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n"); /* int -> float */
                else if (!g->cur_ret_float && vf) fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* float -> int */
                if (g->cur_ret_float) {
                    fprintf(g->out, "    movq xmm0, rax\n"); /* return the float via xmm0 */
                    if (g->cur_ret_f32c) fprintf(g->out, "    cvtsd2ss xmm0, xmm0\n"); /* C expects a single */
                }
            } else {
                fprintf(g->out, "    xor eax, eax\n");
            }
            fprintf(g->out, "    leave\n    ret\n");  /* leave = restore rsp/rbp, then ret */
            break;
        case ND_BLOCK: {
            int mark = g->nvisible;   /* open a new scope */
            for (int i = 0; i < n->nitems; i++) gen_stmt(g, n->items[i]);
            g->nvisible = mark;       /* close the scope: the block's variables drop out of visibility */
            break;
        }
        case ND_IF: {
            int lelse = new_label(g);
            int lend = new_label(g);
            gen_expr(g, n->cond);
            fprintf(g->out, "    cmp rax, 0\n    je .Lelse%d\n", lelse);
            gen_stmt(g, n->then_branch);
            fprintf(g->out, "    jmp .Lend%d\n", lend);
            fprintf(g->out, ".Lelse%d:\n", lelse);
            if (n->else_branch) gen_stmt(g, n->else_branch);
            fprintf(g->out, ".Lend%d:\n", lend);
            break;
        }
        case ND_WHILE: {
            int lcont = new_label(g);  /* target of continue and the loop-back point */
            int lbrk  = new_label(g);  /* target of break and the loop exit */
            if (g->nloops >= MAX_LOOP) { fprintf(stderr, "codegen error: too many nested loops/switch (max %d)\n", MAX_LOOP); g->had_error = 1; break; }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            fprintf(g->out, ".Lcont%d:\n", lcont);
            gen_expr(g, n->cond);
            fprintf(g->out, "    cmp rax, 0\n    je .Lbrk%d\n", lbrk);
            gen_stmt(g, n->body);
            fprintf(g->out, "    jmp .Lcont%d\n", lcont);
            fprintf(g->out, ".Lbrk%d:\n", lbrk);
            g->nloops--;
            break;
        }
        case ND_FOR: {
            int lcond = new_label(g);  /* condition check point */
            int lcont = new_label(g);  /* target of continue (jumps to the step part) */
            int lbrk  = new_label(g);  /* target of break */
            int mark = g->nvisible;    /* variables in for-init are scoped to the loop */
            /* the init part: may be a variable declaration or an expression */
            if (n->init) {
                if (n->init->kind == ND_VAR_DECL) gen_stmt(g, n->init);
                else gen_expr(g, n->init);
            }
            fprintf(g->out, ".Lcond%d:\n", lcond);
            if (n->cond) {
                gen_expr(g, n->cond);
                fprintf(g->out, "    cmp rax, 0\n    je .Lbrk%d\n", lbrk);
            }
            if (g->nloops >= MAX_LOOP) { fprintf(stderr, "codegen error: too many nested loops/switch (max %d)\n", MAX_LOOP); g->had_error = 1; break; }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            gen_stmt(g, n->body);
            g->nloops--;
            fprintf(g->out, ".Lcont%d:\n", lcont);
            if (n->step) gen_expr(g, n->step);
            fprintf(g->out, "    jmp .Lcond%d\n", lcond);
            fprintf(g->out, ".Lbrk%d:\n", lbrk);
            g->nvisible = mark;        /* close the for scope */
            break;
        }
        case ND_DOWHILE: {
            /* run the body once first, then check the condition (repeat while true) */
            int lstart = new_label(g);
            int lcont = new_label(g);   /* continue jumps to the condition check */
            int lbrk = new_label(g);
            fprintf(g->out, ".Ldo%d:\n", lstart);
            if (g->nloops >= MAX_LOOP) { fprintf(stderr, "codegen error: too many nested loops/switch (max %d)\n", MAX_LOOP); g->had_error = 1; break; }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            gen_stmt(g, n->body);
            g->nloops--;
            fprintf(g->out, ".Lcont%d:\n", lcont);
            gen_expr(g, n->cond);
            fprintf(g->out, "    cmp rax, 0\n    jne .Ldo%d\n", lstart);
            fprintf(g->out, ".Lbrk%d:\n", lbrk);
            break;
        }
        case ND_SWITCH: {
            /* evaluate the compared value once and keep it in a temporary stack slot,
             * then compare against each case (jump to the label of the matching case) */
            int lbrk = new_label(g);
            int off = (int)n->int_val;            /* frame slot holding the compared value (reserved in pre-pass) */
            int *caselbl = (int *)malloc(sizeof(int) * (n->nitems > 0 ? n->nitems : 1));
            int ldefault = -1;
            gen_expr(g, n->cond);
            fprintf(g->out, "    mov [rbp - %d], rax\n", off);   /* keep the compared value on the frame */
            for (int i = 0; i < n->nitems; i++) {
                caselbl[i] = new_label(g);
                Node *c = n->items[i];
                if (!c->operand) { ldefault = caselbl[i]; continue; } /* default skips the comparison */
                gen_expr(g, c->operand);
                fprintf(g->out, "    mov rcx, rax\n");
                fprintf(g->out, "    mov rax, [rbp - %d]\n", off);
                fprintf(g->out, "    cmp rax, rcx\n    je .Lcase%d\n", caselbl[i]);
            }
            if (ldefault >= 0) fprintf(g->out, "    jmp .Lcase%d\n", ldefault);
            else               fprintf(g->out, "    jmp .Lbrk%d\n", lbrk);
            /* break exits the switch; continue is forwarded to the enclosing loop (cont = -1) */
            if (g->nloops >= MAX_LOOP) { fprintf(stderr, "codegen error: too many nested loops/switch (max %d)\n", MAX_LOOP); g->had_error = 1; break; }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = -1;
            g->nloops++;
            for (int i = 0; i < n->nitems; i++) {
                fprintf(g->out, ".Lcase%d:\n", caselbl[i]);  /* C-style fallthrough */
                for (int j = 0; j < n->items[i]->nitems; j++) gen_stmt(g, n->items[i]->items[j]);
            }
            g->nloops--;
            fprintf(g->out, ".Lbrk%d:\n", lbrk);
            free(caselbl);
            break;
        }
        case ND_BREAK: {
            if (g->nloops == 0) { fprintf(stderr, "codegen error: 'break' outside loop/switch\n"); g->had_error = 1; break; }
            fprintf(g->out, "    jmp .Lbrk%d\n", g->loops[g->nloops - 1].brk);
            break;
        }
        case ND_CONTINUE: {
            /* find the nearest continue target that is a real loop (skip switches, whose cont = -1) */
            int target = -1;
            for (int i = g->nloops - 1; i >= 0; i--)
                if (g->loops[i].cont >= 0) { target = g->loops[i].cont; break; }
            if (target < 0) { fprintf(stderr, "codegen error: 'continue' outside loop\n"); g->had_error = 1; break; }
            fprintf(g->out, "    jmp .Lcont%d\n", target);
            break;
        }
        default:
            fprintf(stderr, "codegen error: cannot generate statement (kind %d)\n", n->kind);
            g->had_error = 1;
    }
}

/* Write a struct value to the destination whose address is in rax at call time.
 * Supports 3 kinds of value: struct literal, struct-returning call result (sret), copy from another struct */
static void gen_store_struct(Gen *g, Node *value) {
    push_tmp(g); /* [rsp] = destination address */
    if (value->kind == ND_STRUCT_LIT) {
        StructInfo *s = find_struct(g, value->name);
        if (!s) { fprintf(stderr, "codegen error: unknown struct '%s'\n", value->name); g->had_error = 1; }
        else for (int i = 0; i < value->nitems; i++) {
            Node *fi = value->items[i];                 /* ND_ASSIGN: lhs = field name, rhs = expression */
            Field *f = find_field(s, fi->lhs->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", fi->lhs->name, s->name); g->had_error = 1; continue; }
            if (f->type == TYPE_STRUCT && f->ptr == 0) {
                /* nested struct field: compute the field's address and write the value recursively */
                fprintf(g->out, "    mov rax, [rsp]\n");
                if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
                gen_store_struct(g, fi->rhs);
            } else {
                gen_expr(g, fi->rhs);                   /* rax = value */
                fprintf(g->out, "    mov rcx, rax\n");
                fprintf(g->out, "    mov rax, [rsp]\n"); /* base address of the destination */
                if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
                gen_store_typed(g, f->ptr > 0 ? TYPE_USIZE : f->type, f->size);
            }
        }
    } else if (value->kind == ND_CALL) {
        /* struct-returning function: pass the destination address as a hidden argument; the callee writes it */
        fprintf(g->out, "    mov rax, [rsp]\n");
        gen_call(g, value, 1);
    } else {
        /* copy the struct from a source lvalue (ident/member/deref) */
        ExprType vt = type_of(g, value);
        StructInfo *s = find_struct(g, vt.sname);
        int size = s ? s->size : 0;
        gen_addr(g, value);                  /* rax = source address */
        fprintf(g->out, "    mov r10, rax\n");   /* src */
        fprintf(g->out, "    mov r11, [rsp]\n"); /* dst */
        gen_memcpy(g, size);
    }
    fprintf(g->out, "    add rsp, 16\n");    /* release the space holding the destination address */
}

/* Gen a function call (resolve the target, lay out arguments per the win64 ABI)
 * has_sret = 1 means the call returns a struct; the destination address is in rax before the call */
static void gen_call(Gen *g, Node *n, int has_sret) {
    Node *callee = n->operand;
    Node *target = NULL;
    Node *self_expr = NULL;   /* if this is a method: the receiver expression */
    int   self_is_ptr = 0;    /* whether the receiver is already a pointer (if so, pass it directly, no &) */

    /* call through a function pointer (indirect): the callee is a TYPE_FUNC value (variable/field)
     * sig = the signature (parameters + return type); no self; invoked via call rax */
    Node *sig = expr_func_sig(g, callee);
    int indirect = (sig != NULL);

    if (!indirect) {
        if (callee->kind == ND_IDENT) {
            target = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
            if (!target) target = find_func(g, "", callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function '%s'\n", callee->name); g->had_error = 1; return; }
        } else if (callee->kind == ND_MEMBER) {
            /* try interpreting it as a method call first: the receiver is an expression of struct type */
            ExprType bt = type_of(g, callee->operand);
            if (bt.base == TYPE_STRUCT && bt.sname) {
                target = find_func(g, bt.sname, callee->name);  /* methods live in namespace = struct name */
                if (target) { self_expr = callee->operand; self_is_ptr = (bt.ptr > 0); }
            }
            /* if it is not a method, interpret the base as a module namespace (e.g. net.TcpServer) */
            if (!target && callee->operand->kind == ND_IDENT)
                target = find_func(g, callee->operand->name, callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function/method '%s' (did you import it?)\n", callee->name); g->had_error = 1; return; }
        } else {
            fprintf(stderr, "codegen error: unsupported call target\n"); g->had_error = 1; return;
        }
        sig = target;   /* direct call: target is both the signature and the destination */
    }

    int returns_struct = (sig->type == TYPE_STRUCT && sig->ptr == 0);
    if (returns_struct && !has_sret) {
        fprintf(stderr, "codegen error: a struct-returning call must be assigned to a variable\n");
        g->had_error = 1; return;
    }
    /* full value order: [hidden sret pointer]?, [self if a method]?, then the actual arguments
     * first 4 go in registers (rcx,rdx,r8,r9); the rest go on the stack above the shadow space (win64 ABI) */
    int nself = self_expr ? 1 : 0;
    int total = n->nitems + nself + (has_sret ? 1 : 0);
    int nstack = total > 4 ? total - 4 : 0;
    int callspace = 32 + nstack * 8;          /* 32 bytes of shadow space + slots for excess arguments */
    callspace = (callspace + 15) / 16 * 16;    /* round up to keep 16-byte alignment */
    const char *regs[4] = { "rcx", "rdx", "r8", "r9" };

    /* 1) evaluate every value in order, pushing each onto the temp stack (16 bytes each)
     * sret must be pushed first (uses the rax the caller set as the destination address);
     * no other expression may clobber rax before that */
    if (has_sret) push_tmp(g);                 /* first value = destination address (in rax) */
    if (self_expr) {                           /* self = pointer to the receiver */
        if (self_is_ptr) gen_expr(g, self_expr);  /* receiver is already a pointer */
        else             gen_addr(g, self_expr);  /* receiver is a struct value -> use its address */
        push_tmp(g);
    }
    int poff = self_expr ? 1 : 0;  /* method: items[0] of sig is self */
    for (int i = 0; i < n->nitems; i++) {
        gen_expr(g, n->items[i]);
        if (i + poff < sig->nitems) {     /* coerce int<->float to the parameter type (except varargs) */
            Node *pp = sig->items[i + poff];
            ExprType pt = { pp->type, pp->ptr, pp->type_name, pp->sig }, at = type_of(g, n->items[i]);
            gen_coerce_num(g, at, pt);
        }
        push_tmp(g);
    }
    /* indirect: evaluate the function address, push it as the last temp (topmost), load it for call rax later
     * - done after the args so rax survives for sret; kept on the stack so rcx/rdx/... cannot clobber it */
    int extra = indirect ? 1 : 0;
    if (indirect) { gen_expr(g, callee); push_tmp(g); }

    /* 2) reserve the call area, then move values from temps to their destinations (registers/stack) */
    int arg_start = (has_sret ? 1 : 0) + nself;  /* index of the first value that is an actual argument */
    fprintf(g->out, "    sub rsp, %d\n", callspace);
    for (int i = 0; i < total; i++) {
        int srcoff = callspace + (total + extra - 1 - i) * 16; /* +extra: the function-address temp on top */
        fprintf(g->out, "    mov rax, [rsp + %d]\n", srcoff);
        if (i < 4) fprintf(g->out, "    mov %s, rax\n", regs[i]);
        else       fprintf(g->out, "    mov [rsp + %d], rax\n", 32 + (i - 4) * 8);
        /* float arguments must also be in xmm registers; decided by the parameter type (after coercion)
         * varargs (beyond the declared params, e.g. printf) use the argument's type instead */
        if (i >= arg_start && i < 4) {
            int pidx = i - arg_start;
            int pi = pidx + poff;                           /* actual parameter index (including self) */
            int is_f;
            if (pi < sig->nitems) is_f = is_float_type(sig->items[pi]->type) && sig->items[pi]->ptr == 0;
            else { ExprType at = type_of(g, n->items[pidx]); is_f = is_float_type(at.base) && at.ptr == 0; }
            if (is_f) {
                fprintf(g->out, "    movq xmm%d, rax\n", i);
                if (sig->is_extern && pi < sig->nitems &&
                    sig->items[pi]->type == TYPE_F32 && sig->items[pi]->ptr == 0)
                    fprintf(g->out, "    cvtsd2ss xmm%d, xmm%d\n", i, i);
            }
        }
    }

    /* 3) call: direct = call <label>; indirect = load the function address (top temp, offset = callspace), call rax */
    if (indirect) {
        fprintf(g->out, "    mov rax, [rsp + %d]\n", callspace); /* function address = topmost temp */
        fprintf(g->out, "    call rax\n");
    } else {
        char lbl[LABEL_MAX]; func_label_of(target, lbl);
        fprintf(g->out, "    call %s\n", lbl);
    }
    fprintf(g->out, "    add rsp, %d\n", callspace);
    fprintf(g->out, "    add rsp, %d\n", (total + extra) * 16); /* includes the function-address temp if indirect */
    /* float return values arrive in xmm0 -> bring them back to our convention (bit-pattern in rax) */
    if (is_float_type(sig->type) && sig->ptr == 0) {
        if (sig->is_extern && sig->type == TYPE_F32)
            fprintf(g->out, "    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n"); /* C returns single -> double */
        else
            fprintf(g->out, "    movq rax, xmm0\n");
    }
}

/* Gen one function */
static void gen_func(Gen *g, Node *fn, Node *program) {
    char lbl[LABEL_MAX];
    func_label_of(fn, lbl);
    /* namespace for resolving unqualified calls = the module it belongs to (mod)
     * this matters for methods: cur_ns must be the module, not the struct name, or a method calls itself */
    g->cur_ns = fn->mod ? fn->mod : "";

    /* clear the local table, then reserve space */
    g->nlocals = 0;
    g->nvisible = 0;
    int frame = 0;
    g->sret_off = 0;

    /* a struct-returning function uses a hidden pointer (sret): reserve a slot for that pointer first,
     * and the real parameters shift one register position (rcx is taken by sret) */
    int returns_struct = (fn->type == TYPE_STRUCT && fn->ptr == 0 && !fn->is_extern);
    if (returns_struct)
        g->sret_off = add_local(g, "$sret", TYPE_USIZE, 0, NULL, NULL, &frame);
    /* a float-returning function must return via xmm0 (flag used at return time) */
    g->cur_ret_float = is_float_type(fn->type) && fn->ptr == 0;
    g->cur_ret_f32c = fn->is_export && fn->type == TYPE_F32 && fn->ptr == 0; /* export f32 -> return single to C */

    int param_start = g->nlocals;
    for (int i = 0; i < fn->nitems; i++) /* parameters */
        add_local(g, fn->items[i]->name, fn->items[i]->type, fn->items[i]->ptr, fn->items[i]->type_name, fn->items[i]->sig, &frame);
    collect_locals(g, fn->body, &frame);   /* locals inside the body */
    /* temp slots for structs returned from functions when used as rvalues
     * every local must be temporarily visible so type_of can infer method receiver types
     * (e.g. p.fmt() must know which struct p is), then reset before real gen begins */
    int saved_vis = g->nvisible;
    for (int i = 0; i < g->nlocals; i++) g->visible[g->nvisible++] = i;
    collect_struct_temps(g, fn->body, &frame);
    g->nvisible = saved_vis;

    /* round the frame size up to a multiple of 16 to keep the stack aligned */
    int frame_size = (frame + 15) / 16 * 16;

    /* function prologue */
    fprintf(g->out, "\n%s:\n", lbl);
    fprintf(g->out, "    push rbp\n    mov rbp, rsp\n");
    if (frame_size > 0) fprintf(g->out, "    sub rsp, %d\n", frame_size);

    /* save the parameters (and the hidden pointer) into their own stack slots
     *   - ABI positions 0..3 come from registers rcx,rdx,r8,r9
     *   - positions 4 and up were placed on the stack by the caller, read from [rbp + 48 + (pos-4)*8]
     *     (8 = return address, 16..48 = the caller's shadow space) */
    const char *argregs[4] = { "rcx", "rdx", "r8", "r9" };
    int base = returns_struct ? 1 : 0;
    if (returns_struct) fprintf(g->out, "    mov [rbp - %d], rcx\n", g->sret_off);
    for (int i = 0; i < fn->nitems; i++) {
        Node *p = fn->items[i];
        Sym *psym = &g->locals[param_start + i];
        int pos = i + base;            /* ABI position (including the hidden sret pointer) */
        int is_float_param = is_float_type(p->type) && p->ptr == 0;
        /* struct by-value parameter: the caller passes the struct's address
         * the callee copies the struct into its own slot (by-value: mutation does not affect the caller) */
        if (p->type == TYPE_STRUCT && p->ptr == 0) {
            int ssize = type_size(g, p->type, 0, p->type_name);
            if (pos < 4) fprintf(g->out, "    mov r10, %s\n", argregs[pos]);              /* src ptr */
            else         fprintf(g->out, "    mov r10, [rbp + %d]\n", 48 + (pos - 4) * 8);
            fprintf(g->out, "    lea r11, [rbp - %d]\n", psym->offset);                   /* dst slot */
            gen_memcpy(g, ssize);
            continue;
        }
        if (pos < 4) {
            if (is_float_param) {
                /* float parameters arrive in register xmm<pos>
                 * an export function called from C: f32 arrives as a single -> widen to double first
                 * (the MVS side stores float values as double bit-patterns) */
                if (fn->is_export && p->type == TYPE_F32)
                    fprintf(g->out, "    cvtss2sd xmm%d, xmm%d\n", pos, pos);
                fprintf(g->out, "    movq rax, xmm%d\n", pos);
                gen_store_var(g, psym);   /* convert/truncate to the type (e.g. f32 -> single) */
            } else {
                fprintf(g->out, "    mov [rbp - %d], %s\n", psym->offset, argregs[pos]);
            }
        } else {
            /* the caller placed parameter 5+ on the stack at [rbp + 48 + (pos-4)*8] */
            fprintf(g->out, "    mov rax, [rbp + %d]\n", 48 + (pos - 4) * 8);
            gen_store_var(g, psym);
        }
    }
    /* parameters are visible for the whole function */
    for (int i = 0; i < fn->nitems; i++) g->visible[g->nvisible++] = param_start + i;

    /* if this is main (empty namespace), initialize globals that have initializers before the body */
    if ((fn->ns == NULL || fn->ns[0] == 0) && strcmp(fn->name, "main") == 0) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind == ND_VAR_DECL && d->operand) {
                Sym *s = find_var(g, d->name); /* must be a global */
                if (!s) continue;
                if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    char gl[LABEL_MAX]; global_label(s->name, gl);
                    fprintf(g->out, "    lea rax, [rel %s]\n", gl);
                    gen_store_struct(g, d->operand);
                } else {
                    gen_expr(g, d->operand);
                    gen_store_var(g, s);
                }
            }
        }
    }

    /* the function body */
    gen_stmt(g, fn->body);

    /* epilogue in case there is no trailing return (e.g. a void function) */
    fprintf(g->out, "    xor eax, eax\n    leave\n    ret\n");
}

/* Main entry point of the backend */
int x86_64_win_generate(Node *program, FILE *out) {
    Gen g;
    memset(&g, 0, sizeof(g));
    g.out = out;

    /* pass 1: register every struct, then compute the layouts to a fixpoint
     * (two steps so a struct can have fields whose struct type is declared later) */
    for (int i = 0; i < program->nitems; i++)
        if (program->items[i]->kind == ND_STRUCT_DECL)
            register_struct(&g, program->items[i]);
    layout_structs(&g);

    /* pass 2: register functions and global variables (supports forward references) */
    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind == ND_FUNC) {
            if (g.nfuncs >= MAX_FUNC) { fprintf(stderr, "codegen error: too many functions (max %d)\n", MAX_FUNC); g.had_error = 1; break; }
            g.funcs[g.nfuncs++] = d; /* keep the whole node (carries ns/is_extern) */
        } else if (d->kind == ND_VAR_DECL) {
            if (g.nglobals >= MAX_SYM) { fprintf(stderr, "codegen error: too many global variables (max %d)\n", MAX_SYM); g.had_error = 1; break; }
            Sym *s = &g.globals[g.nglobals++];
            s->name = d->name; s->type = d->type; s->ptr = d->ptr; s->sname = d->type_name;
            s->size = type_size(&g, d->type, d->ptr, d->type_name);
            s->is_global = 1; s->offset = 0;
        }
    }

    /* check whether the io module is imported (any function with namespace "io") to gate io.out */
    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->ns && strcmp(g.funcs[i]->ns, "io") == 0) { g.io_imported = 1; break; }

    /* reachability analysis (tree-shaking): start from main and global initializers
     * so only functions actually called get generated, shrinking the output */
    char reached[MAX_FUNC]; memset(reached, 0, sizeof(reached));
    int has_main = 0;
    for (int i = 0; i < g.nfuncs; i++) {
        Node *f = g.funcs[i];
        /* reachability roots: main and exported functions (callable from C) */
        if ((f->ns == NULL || f->ns[0] == 0) && strcmp(f->name, "main") == 0 && !f->is_extern) {
            has_main = 1; reach_func(&g, i, reached);
        }
        if (f->is_export) reach_func(&g, i, reached);
    }
    for (int i = 0; i < program->nitems; i++) /* calls inside global initializers (run in main) */
        if (program->items[i]->kind == ND_VAR_DECL && program->items[i]->operand)
            reach_node(&g, program->items[i]->operand, "", reached);

    /* assembly file header */
    fprintf(out, "; ===== MVS compiler output (x86-64 Windows, NASM syntax) =====\n");
    fprintf(out, "default rel\n");          /* use RIP-relative addressing */
    if (has_main) fprintf(out, "global main\n"); /* export main for the C runtime to call (if present) */
    /* export functions marked export so the C language can call them (raw symbol names) */
    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->is_export) fprintf(out, "global %s\n", g.funcs[i]->name);

    /* declare foreign (extern) functions, only reachable ones, deduped by name */
    for (int i = 0; i < g.nfuncs; i++) {
        if (!g.funcs[i]->is_extern || !reached[i]) continue;
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (g.funcs[j]->is_extern && reached[j] && strcmp(g.funcs[j]->name, g.funcs[i]->name) == 0) { dup = 1; break; }
        if (!dup) fprintf(out, "extern %s\n", g.funcs[i]->name);
    }
    fprintf(out, "\n");

    /* gen every reachable function that has a body (.text section); skip extern and unused functions */
    fprintf(out, "section .text\n");
    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind != ND_FUNC || d->is_extern) continue;
        if (d->ngen > 0) continue;                  /* skip generic templates (only instances remain) */
        if (!reached[func_index(&g, d)]) continue; /* tree-shaking: skip functions never called */
        gen_func(&g, d, program);
    }

    /* read-only data section: all string constants (including format strings from std/io.mvs) */
    fprintf(out, "\nsection .data\n");
    for (int i = 0; i < g.nstrs; i++) {
        fprintf(out, "mvs_str_%d: db ", i);
        for (int j = 0; j < g.strs[i].len; j++) fprintf(out, "%d,", g.strs[i].data[j]);
        fprintf(out, "0\n"); /* terminate with 0 to make it a C-style string */
    }

    /* global variable section (reserve the actual size rounded up to a multiple of 8, zero-initialized) */
    fprintf(out, "\nsection .bss\n");
    for (int i = 0; i < g.nglobals; i++) {
        char lbl[LABEL_MAX]; global_label(g.globals[i].name, lbl);
        int slot = (g.globals[i].size + 7) / 8 * 8;
        if (slot < 8) slot = 8;
        fprintf(out, "%s: resb %d\n", lbl, slot);
    }

    return g.had_error ? 1 : 0;
}
