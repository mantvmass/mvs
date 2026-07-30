/*
 * arch/arm64/linux.c - backend emitting AArch64 (GNU as syntax) for Linux (AAPCS64)
 *
 * This file holds only the architecture-dependent parts: instruction emission,
 * registers, and the calling convention. Shared parts (type system, struct layout,
 * symbol table, reachability, format) live in arch/common.c
 *
 * AAPCS64: integer/pointer arguments -> x0..x7 and float arguments -> d0..d7
 *          (separate classes); the hidden struct-return pointer travels in x8;
 *          extras go on the stack; sp stays 16-byte aligned at all times.
 *          Variadic calls need nothing special on Linux (va_list saves regs).
 * Value model (same as the x86 backends): every expression leaves its value in
 *          x0 (floats as double bit-patterns; structs/i128/dyn as an address).
 * Output is assembled with `aarch64-linux-gnu-gcc -c` (or clang --target=...)
 * and linked/run on Linux or under qemu-aarch64.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linux.h"
#include "../common.h"

/* ---------- code emission helpers ---------- */

/* Push x0 onto a temporary stack slot (16 bytes keeps sp aligned) */
static void push_tmp(Gen *g) {
    fprintf(g->out, "    str x0, [sp, #-16]!\n");
}

/* Pop the newest temp slot into the given register */
static void pop_tmp(Gen *g, const char *reg) {
    fprintf(g->out, "    ldr %s, [sp], #16\n", reg);
}

/* Materialize [x29 - off] into reg (offsets can exceed the immediate range) */
static void addr_local(Gen *g, const char *reg, int off) {
    if (off <= 4095) fprintf(g->out, "    sub %s, x29, #%d\n", reg, off);
    else             fprintf(g->out, "    mov %s, #%d\n    sub %s, x29, %s\n", reg, off, reg, reg);
}

/* Materialize the address of a global/label into reg */
static void addr_label(Gen *g, const char *reg, const char *lbl) {
    fprintf(g->out, "    adrp %s, %s\n    add %s, %s, :lo12:%s\n", reg, lbl, reg, reg, lbl);
}

static void gen_expr(Gen *g, Node *n);
static void gen_stmt(Gen *g, Node *n);
static void gen_addr(Gen *g, Node *n);
static void gen_store_struct(Gen *g, Node *value);
static void gen_call(Gen *g, Node *n, int has_sret);
static void gen_dyn_store(Gen *g, ExprType rhs_t, const char *trait);

/* Load the value at [x0] by size/signedness into x0 */
static void gen_load_at(Gen *g, int size, int is_signed) {
    switch (size) {
        case 1: fprintf(g->out, is_signed ? "    ldrsb x0, [x0]\n" : "    ldrb w0, [x0]\n"); break;
        case 2: fprintf(g->out, is_signed ? "    ldrsh x0, [x0]\n" : "    ldrh w0, [x0]\n"); break;
        case 4: fprintf(g->out, is_signed ? "    ldrsw x0, [x0]\n" : "    ldr w0, [x0]\n"); break;
        default: fprintf(g->out, "    ldr x0, [x0]\n"); break;
    }
}

/* Store x1 to [x0] by size */
static void gen_store_at(Gen *g, int size) {
    switch (size) {
        case 1: fprintf(g->out, "    strb w1, [x0]\n"); break;
        case 2: fprintf(g->out, "    strh w1, [x0]\n"); break;
        case 4: fprintf(g->out, "    str w1, [x0]\n"); break;
        default: fprintf(g->out, "    str x1, [x0]\n"); break;
    }
}

/* Load [x0] by type into x0 (floats become double bit-patterns) */
static void gen_load_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    ldr s0, [x0]\n    fcvt d0, s0\n    fmov x0, d0\n");
    } else {
        gen_load_at(g, size, is_signed_type(base));
    }
}

/* Store x1 (double bits for floats) to [x0] by type */
static void gen_store_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    fmov d0, x1\n    fcvt s0, d0\n    str s0, [x0]\n");
    } else {
        gen_store_at(g, size);
    }
}

/* Load a variable's value into x0 (struct/array/i128/dyn = its address) */
static void gen_load_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x0", lbl); }
    else              addr_local(g, "x0", s->offset);
    if (s->arr > 0) return;
    if (s->type == TYPE_STRUCT && s->ptr == 0) return;
    if (is_blob16(s->type, s->ptr)) return;
    gen_load_typed(g, s->ptr > 0 ? TYPE_USIZE : s->type, s->size);
}

/* Store x0 into a variable (width/f32 conversion applied) */
static void gen_store_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    fprintf(g->out, "    mov x1, x0\n");
    if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x0", lbl); }
    else              addr_local(g, "x0", s->offset);
    gen_store_typed(g, s->ptr > 0 ? TYPE_USIZE : (s->ptr == 0 && s->type == TYPE_F32 ? TYPE_F32 : s->type), s->size);
    fprintf(g->out, "    mov x0, x1\n");
}

/* Implicit int<->float conversion of x0 when crossing the numeric line */
static void gen_coerce_num(Gen *g, ExprType from, ExprType to) {
    if (from.ptr > 0 || to.ptr > 0) return;
    int ff = is_float_type(from.base), tf = is_float_type(to.base);
    if (ff == tf) return;
    if (!ff && tf) fprintf(g->out, "    scvtf d0, x0\n    fmov x0, d0\n");
    else           fprintf(g->out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
}

/* Copy x11 bytes from [x9] to [x10] (byte loop; only scratch registers) */
static void gen_memcpy(Gen *g, int size) {
    int l = new_label(g);
    fprintf(g->out, "    mov x11, #%d\n", size);
    fprintf(g->out, ".Lmc%d:\n    cbz x11, .Lmce%d\n", l, l);
    fprintf(g->out, "    ldrb w12, [x9], #1\n    strb w12, [x10], #1\n");
    fprintf(g->out, "    sub x11, x11, #1\n    b .Lmc%d\n.Lmce%d:\n", l, l);
}

/* Unsigned value? (pointer/unsigned int/bool/char) */
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

/* ---------- lvalue addresses ---------- */

static void gen_addr(Gen *g, Node *n) {
    switch (n->kind) {
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (!s) { fprintf(stderr, "codegen error: undefined variable '%s'\n", n->name); g->had_error = 1; return; }
            char lbl[LABEL_MAX];
            if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x0", lbl); }
            else              addr_local(g, "x0", s->offset);
            break;
        }
        case ND_FRAMEREF:
            addr_local(g, "x0", (int)n->int_val);
            break;
        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *s = find_struct(g, bt.sname);
            if (!s) { fprintf(stderr, "codegen error: member access on non-struct\n"); g->had_error = 1; return; }
            Field *f = find_field(s, n->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", n->name, s->name); g->had_error = 1; return; }
            if (bt.ptr > 0) gen_expr(g, n->operand);
            else            gen_addr(g, n->operand);
            if (f->offset) fprintf(g->out, "    add x0, x0, #%d\n", f->offset);
            break;
        }
        case ND_INDEX: {
            ExprType bt = type_of(g, n->lhs);
            int esz;
            if (bt.arr > 0) {
                esz = type_size(g, bt.base, bt.ptr, bt.sname);
                gen_addr(g, n->lhs);
            } else if (bt.ptr > 0) {
                esz = type_size(g, bt.base, bt.ptr - 1, bt.sname);
                gen_expr(g, n->lhs);
            } else {
                fprintf(stderr, "codegen error: cannot index a non-array, non-pointer value\n");
                g->had_error = 1;
                return;
            }
            push_tmp(g);
            gen_expr(g, n->rhs);
            if (esz != 1) fprintf(g->out, "    mov x9, #%d\n    mul x0, x0, x9\n", esz);
            pop_tmp(g, "x1");
            fprintf(g->out, "    add x0, x0, x1\n");
            break;
        }
        case ND_UNARY:
            if (n->op == TK_STAR) { gen_expr(g, n->operand); break; }
            fprintf(stderr, "codegen error: expression is not an lvalue\n");
            g->had_error = 1;
            break;
        case ND_CALL: {
            ExprType rt = type_of(g, n);
            if ((rt.base == TYPE_STRUCT || is_blob16(rt.base, rt.ptr)) && rt.ptr == 0 && n->int_val) {
                addr_local(g, "x0", (int)n->int_val);
                gen_call(g, n, 1);
                addr_local(g, "x0", (int)n->int_val);
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

/* Apply a binary op: lhs in x0, rhs in x1; result in x0 */
static void gen_binop_apply(Gen *g, TokenType op, int uns) {
    switch (op) {
        case TK_PLUS:  fprintf(g->out, "    add x0, x0, x1\n"); break;
        case TK_MINUS: fprintf(g->out, "    sub x0, x0, x1\n"); break;
        case TK_STAR:  fprintf(g->out, "    mul x0, x0, x1\n"); break;
        case TK_AMP:   fprintf(g->out, "    and x0, x0, x1\n"); break;
        case TK_PIPE:  fprintf(g->out, "    orr x0, x0, x1\n"); break;
        case TK_CARET: fprintf(g->out, "    eor x0, x0, x1\n"); break;
        case TK_SHL:   fprintf(g->out, "    lsl x0, x0, x1\n"); break;
        case TK_SHR:   fprintf(g->out, uns ? "    lsr x0, x0, x1\n" : "    asr x0, x0, x1\n"); break;
        case TK_SLASH:
            fprintf(g->out, uns ? "    udiv x0, x0, x1\n" : "    sdiv x0, x0, x1\n");
            break;
        case TK_PERCENT:
            fprintf(g->out, uns ? "    udiv x9, x0, x1\n" : "    sdiv x9, x0, x1\n");
            fprintf(g->out, "    msub x0, x9, x1, x0\n");
            break;
        case TK_STARSTAR: {
            int l = new_label(g);
            fprintf(g->out,
                "    mov x9, x0\n"        /* base */
                "    mov x10, x1\n"       /* exponent counter */
                "    mov x0, #1\n"
                ".Lpow%d:\n"
                "    cmp x10, #0\n"
                "    b.le .Lpowend%d\n"
                "    mul x0, x0, x9\n"
                "    sub x10, x10, #1\n"
                "    b .Lpow%d\n"
                ".Lpowend%d:\n", l, l, l, l);
            break;
        }
        case TK_EQ:  fprintf(g->out, "    cmp x0, x1\n    cset x0, eq\n"); break;
        case TK_NEQ: fprintf(g->out, "    cmp x0, x1\n    cset x0, ne\n"); break;
        case TK_LT:  fprintf(g->out, "    cmp x0, x1\n    cset x0, %s\n", uns ? "lo" : "lt"); break;
        case TK_GT:  fprintf(g->out, "    cmp x0, x1\n    cset x0, %s\n", uns ? "hi" : "gt"); break;
        case TK_LE:  fprintf(g->out, "    cmp x0, x1\n    cset x0, %s\n", uns ? "ls" : "le"); break;
        case TK_GE:  fprintf(g->out, "    cmp x0, x1\n    cset x0, %s\n", uns ? "hs" : "ge"); break;
        default:
            fprintf(stderr, "codegen error: unknown binary operator\n");
            g->had_error = 1;
    }
}

/* ---------- dyn Trait ---------- */

static void gen_dyn_store(Gen *g, ExprType rhs_t, const char *trait) {
    if (is_dyn(rhs_t.base, rhs_t.ptr)) {
        fprintf(g->out, "    ldp x2, x3, [x1]\n    stp x2, x3, [x0]\n");
        return;
    }
    if (rhs_t.ptr == 1 && trait) {
        const char *tn = rhs_t.base == TYPE_STRUCT ? rhs_t.sname : datatype_name(rhs_t.base);
        if (tn) {
            char vt[LABEL_MAX];
            snprintf(vt, sizeof(vt), "mvs_vt_%s_%s", trait, tn);
            g->need_vtables = 1;
            fprintf(g->out, "    str x1, [x0]\n");
            addr_label(g, "x2", vt);
            fprintf(g->out, "    str x2, [x0, #8]\n");
            return;
        }
    }
    fprintf(stderr, "codegen error: a dyn value can only come from another dyn or a pointer to an implementing type\n");
    g->had_error = 1;
}

/* Dynamic dispatch d.m(args): [sret in x8]? [self=data] args..., call [vtable + slot*8] */
static void gen_dyn_call(Gen *g, Node *n, int has_sret, const char *trait) {
    Node *tr = NULL;
    for (int i = 0; g->program && i < g->program->nitems; i++) {
        Node *d = g->program->items[i];
        if (d->kind == ND_TRAIT && d->name && strcmp(d->name, trait) == 0) { tr = d; break; }
    }
    if (!tr) { fprintf(stderr, "codegen error: unknown trait '%s'\n", trait); g->had_error = 1; return; }
    int idx = -1, mi = 0;
    Node *msig = NULL;
    for (int i = 0; i < tr->nitems; i++) {
        if (tr->items[i]->kind != ND_FUNC) continue;
        if (strcmp(tr->items[i]->name, n->operand->name) == 0) { idx = mi; msig = tr->items[i]; break; }
        mi++;
    }
    if (idx < 0) {
        fprintf(stderr, "codegen error: trait '%s' has no method '%s'\n", trait, n->operand->name);
        g->had_error = 1; return;
    }
    g->need_vtables = 1;

    /* values (excluding sret, which travels in x8): [self] args... */
    int total = n->nitems + 1 + (has_sret ? 1 : 0);
    int selfpos = has_sret ? 1 : 0;
    int cls_gpr[64], cls_fp[64], cls_stk[64];
    int ng = 0, nf = 0, nstk = 0;
    for (int i = 0; i < total && i < 64; i++) {
        int isf = 0;
        if (i > selfpos) {
            int pi = i - selfpos;
            if (pi < msig->nitems && is_float_type(msig->items[pi]->type) && msig->items[pi]->ptr == 0) isf = 1;
        }
        cls_gpr[i] = cls_fp[i] = cls_stk[i] = -1;
        if (has_sret && i == 0) continue;                 /* sret -> x8, no class */
        if (isf) { if (nf < 8) cls_fp[i] = nf++; else cls_stk[i] = nstk++; }
        else     { if (ng < 8) cls_gpr[i] = ng++; else cls_stk[i] = nstk++; }
    }
    int callspace = (nstk * 8 + 15) / 16 * 16;

    if (has_sret) push_tmp(g);
    gen_expr(g, n->operand->operand);                     /* x0 = blob address */
    push_tmp(g);
    for (int i = 0; i < n->nitems; i++) {
        gen_expr(g, n->items[i]);
        if (i + 1 < msig->nitems) {
            Node *pp = msig->items[i + 1];
            ExprType pt = { pp->type, pp->ptr, pp->type_name, pp->sig, 0 }, at = type_of(g, n->items[i]);
            gen_coerce_num(g, at, pt);
        }
        push_tmp(g);
    }
    fprintf(g->out, "    ldr x0, [sp, #%d]\n", (n->nitems) * 16);   /* the self slot */
    push_tmp(g);                                          /* extra blob temp on top */

    if (callspace) fprintf(g->out, "    sub sp, sp, #%d\n", callspace);
    for (int i = 0; i < total; i++) {
        int srcoff = callspace + (total + 1 - 1 - i) * 16;
        fprintf(g->out, "    ldr x9, [sp, #%d]\n", srcoff);
        if (i == selfpos) fprintf(g->out, "    ldr x9, [x9]\n");    /* blob -> data */
        if (has_sret && i == 0)   fprintf(g->out, "    mov x8, x9\n");
        else if (cls_fp[i] >= 0)  fprintf(g->out, "    fmov d%d, x9\n", cls_fp[i]);
        else if (cls_gpr[i] >= 0) fprintf(g->out, "    mov x%d, x9\n", cls_gpr[i]);
        else                      fprintf(g->out, "    str x9, [sp, #%d]\n", cls_stk[i] * 8);
    }
    fprintf(g->out, "    ldr x9, [sp, #%d]\n", callspace);          /* blob address */
    fprintf(g->out, "    ldr x9, [x9, #8]\n");                      /* vtable */
    fprintf(g->out, "    ldr x9, [x9, #%d]\n", idx * 8);
    fprintf(g->out, "    blr x9\n");
    if (callspace) fprintf(g->out, "    add sp, sp, #%d\n", callspace);
    fprintf(g->out, "    add sp, sp, #%d\n", (total + 1) * 16);
    if (is_float_type(msig->type) && msig->ptr == 0)
        fprintf(g->out, "    fmov x0, d0\n");
}

/* ---------- 128-bit integers (address-as-value) ---------- */

static void i128_copy_to_slot(Gen *g, int off) {
    fprintf(g->out, "    ldp x2, x3, [x0]\n");
    addr_local(g, "x9", off);
    fprintf(g->out, "    stp x2, x3, [x9]\n");
}

static void gen_i128_operand(Gen *g, Node *e, int off) {
    ExprType t = type_of(g, e);
    gen_expr(g, e);
    if (is_i128(t.base, t.ptr)) { i128_copy_to_slot(g, off); return; }
    addr_local(g, "x9", off);
    if (is_unsigned_val(t.base, t.ptr))
        fprintf(g->out, "    stp x0, xzr, [x9]\n");
    else
        fprintf(g->out, "    asr x2, x0, #63\n    stp x0, x2, [x9]\n");
}

static void gen_i128_store(Gen *g, ExprType rhs_t) {
    if (is_i128(rhs_t.base, rhs_t.ptr)) {
        fprintf(g->out, "    ldp x2, x3, [x1]\n    stp x2, x3, [x0]\n");
    } else if (is_unsigned_val(rhs_t.base, rhs_t.ptr)) {
        fprintf(g->out, "    stp x1, xzr, [x0]\n");
    } else {
        fprintf(g->out, "    asr x2, x1, #63\n    stp x1, x2, [x0]\n");
    }
}

/* 128-bit binary ops; comparisons leave 0/1 in x0, others the result address */
static void gen_i128_binop(Gen *g, Node *n) {
    int ro = (int)n->int_val, ao = ro - 16, bo = ro - 32;
    ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);
    int uns = (lt.base == TYPE_U128 || rt.base == TYPE_U128 ||
               (is_unsigned_val(lt.base, lt.ptr) && is_unsigned_val(rt.base, rt.ptr)));
    gen_i128_operand(g, n->lhs, ao);
    gen_i128_operand(g, n->rhs, bo);
    addr_local(g, "x9", ao);
    fprintf(g->out, "    ldp x2, x3, [x9]\n");
    addr_local(g, "x9", bo);
    fprintf(g->out, "    ldp x4, x5, [x9]\n");
    switch (n->op) {
        case TK_PLUS:
            fprintf(g->out, "    adds x2, x2, x4\n    adc x3, x3, x5\n");
            break;
        case TK_MINUS:
            fprintf(g->out, "    subs x2, x2, x4\n    sbc x3, x3, x5\n");
            break;
        case TK_AMP:
            fprintf(g->out, "    and x2, x2, x4\n    and x3, x3, x5\n");
            break;
        case TK_PIPE:
            fprintf(g->out, "    orr x2, x2, x4\n    orr x3, x3, x5\n");
            break;
        case TK_CARET:
            fprintf(g->out, "    eor x2, x2, x4\n    eor x3, x3, x5\n");
            break;
        case TK_STAR:
            /* lo = lo(a.lo*b.lo); hi = hi(a.lo*b.lo) + a.lo*b.hi + a.hi*b.lo */
            fprintf(g->out, "    umulh x6, x2, x4\n");
            fprintf(g->out, "    madd x6, x2, x5, x6\n");
            fprintf(g->out, "    madd x6, x3, x4, x6\n");
            fprintf(g->out, "    mul x2, x2, x4\n    mov x3, x6\n");
            break;
        case TK_SHL: {
            int l = new_label(g);
            fprintf(g->out, "    and x4, x4, #127\n");
            fprintf(g->out, "    cbz x4, .Lshd%d\n", l);
            fprintf(g->out, "    cmp x4, #64\n    b.lt .Lsh%d\n", l);
            fprintf(g->out, "    sub x4, x4, #64\n    lsl x3, x2, x4\n    mov x2, xzr\n    b .Lshd%d\n", l);
            fprintf(g->out, ".Lsh%d:\n", l);
            fprintf(g->out, "    lsl x3, x3, x4\n    mov x5, #64\n    sub x5, x5, x4\n");
            fprintf(g->out, "    lsr x6, x2, x5\n    orr x3, x3, x6\n    lsl x2, x2, x4\n");
            fprintf(g->out, ".Lshd%d:\n", l);
            break;
        }
        case TK_SHR: {
            int l = new_label(g);
            const char *hi_shift = uns ? "lsr" : "asr";
            fprintf(g->out, "    and x4, x4, #127\n");
            fprintf(g->out, "    cbz x4, .Lshd%d\n", l);
            fprintf(g->out, "    cmp x4, #64\n    b.lt .Lsh%d\n", l);
            fprintf(g->out, "    sub x4, x4, #64\n    %s x2, x3, x4\n", hi_shift);
            if (uns) fprintf(g->out, "    mov x3, xzr\n");
            else     fprintf(g->out, "    asr x3, x3, #63\n");
            fprintf(g->out, "    b .Lshd%d\n.Lsh%d:\n", l, l);
            fprintf(g->out, "    lsr x2, x2, x4\n    mov x5, #64\n    sub x5, x5, x4\n");
            fprintf(g->out, "    lsl x6, x3, x5\n    orr x2, x2, x6\n    %s x3, x3, x4\n", hi_shift);
            fprintf(g->out, ".Lshd%d:\n", l);
            break;
        }
        case TK_SLASH: case TK_PERCENT: {
            g->need_i128 = 1;
            const char *fn = uns ? "mvs_u128_divmod" : "mvs_s128_divmod";
            int qoff = n->op == TK_SLASH ? ro : ao;
            int roff = n->op == TK_SLASH ? ao : ro;
            addr_local(g, "x0", ao);
            addr_local(g, "x1", bo);
            addr_local(g, "x2", qoff);
            addr_local(g, "x3", roff);
            fprintf(g->out, "    bl %s\n", fn);
            addr_local(g, "x0", ro);
            return;
        }
        case TK_EQ: case TK_NEQ:
            fprintf(g->out, "    eor x2, x2, x4\n    eor x3, x3, x5\n    orr x2, x2, x3\n");
            fprintf(g->out, "    cmp x2, #0\n    cset x0, %s\n", n->op == TK_EQ ? "eq" : "ne");
            return;
        case TK_LT: case TK_GT: case TK_LE: case TK_GE: {
            int l = new_label(g);
            const char *hi_cc, *lo_cc;
            switch (n->op) {
                case TK_LT: hi_cc = uns ? "lo" : "lt"; lo_cc = "lo"; break;
                case TK_GT: hi_cc = uns ? "hi" : "gt"; lo_cc = "hi"; break;
                case TK_LE: hi_cc = uns ? "lo" : "lt"; lo_cc = "ls"; break;
                default:    hi_cc = uns ? "hi" : "gt"; lo_cc = "hs"; break;
            }
            fprintf(g->out, "    cmp x3, x5\n    b.ne .Lic%d\n", l);
            fprintf(g->out, "    cmp x2, x4\n    cset x0, %s\n    b .Lid%d\n", lo_cc, l);
            fprintf(g->out, ".Lic%d:\n    cset x0, %s\n.Lid%d:\n", l, hi_cc, l);
            return;
        }
        default:
            fprintf(stderr, "codegen error: this operator is not supported on 128-bit integers\n");
            g->had_error = 1;
            return;
    }
    addr_local(g, "x9", ro);
    fprintf(g->out, "    stp x2, x3, [x9]\n");
    fprintf(g->out, "    mov x0, x9\n");
}

static void gen_i128_unary(Gen *g, Node *n) {
    int ro = (int)n->int_val, ao = ro - 16;
    gen_i128_operand(g, n->operand, ao);
    addr_local(g, "x9", ao);
    fprintf(g->out, "    ldp x2, x3, [x9]\n");
    if (n->op == TK_MINUS)
        fprintf(g->out, "    mvn x2, x2\n    mvn x3, x3\n    adds x2, x2, #1\n    adc x3, x3, xzr\n");
    else
        fprintf(g->out, "    mvn x2, x2\n    mvn x3, x3\n");
    addr_local(g, "x9", ro);
    fprintf(g->out, "    stp x2, x3, [x9]\n    mov x0, x9\n");
}

/* ---------- expressions ---------- */

static void gen_expr(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_INT:
        case ND_FLOAT:
        case ND_CHAR:
        case ND_BOOL:
            fprintf(g->out, "    ldr x0, =%lld\n", n->int_val);
            break;
        case ND_STR: {
            int idx = intern_string(g, n->str_val, n->str_len);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "mvs_str_%d", idx);
            addr_label(g, "x0", lbl);
            break;
        }
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (s) { gen_load_var(g, s); break; }
            Node *f = find_func(g, g->cur_ns ? g->cur_ns : "", n->name);
            if (!f) f = find_func(g, "", n->name);
            if (f) {
                char flbl[LABEL_MAX]; func_label_of(f, flbl);
                addr_label(g, "x0", flbl);
                break;
            }
            fprintf(stderr, "codegen error: undefined variable '%s'\n", n->name); g->had_error = 1;
            break;
        }
        case ND_BINARY: {
            if (n->op == TK_AND || n->op == TK_OR) {
                int lend = new_label(g);
                gen_expr(g, n->lhs);
                if (n->op == TK_AND) fprintf(g->out, "    cbz x0, .Llog%d\n", lend);
                else                 fprintf(g->out, "    cbnz x0, .Llog%d\n", lend);
                gen_expr(g, n->rhs);
                fprintf(g->out, "    cmp x0, #0\n    cset x0, ne\n");
                fprintf(g->out, ".Llog%d:\n", lend);
                if (n->op == TK_OR) fprintf(g->out, "    cmp x0, #0\n    cset x0, ne\n");
                break;
            }
            ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);
            if (is_i128(lt.base, lt.ptr) || is_i128(rt.base, rt.ptr)) {
                gen_i128_binop(g, n);
                break;
            }
            int fop = ((is_float_type(lt.base) && lt.ptr == 0) || (is_float_type(rt.base) && rt.ptr == 0));
            int is_cmp = (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||n->op==TK_LE||n->op==TK_GE);
            if (fop && n->op == TK_STARSTAR) {
                /* float base ** integer exponent: repeated multiply */
                int l = new_label(g);
                gen_expr(g, n->lhs); push_tmp(g);
                gen_expr(g, n->rhs);
                if (is_float_type(rt.base)) fprintf(g->out, "    fmov d2, x0\n    fcvtzs x10, d2\n");
                else                        fprintf(g->out, "    mov x10, x0\n");
                pop_tmp(g, "x1");
                fprintf(g->out, is_float_type(lt.base) ? "    fmov d1, x1\n" : "    scvtf d1, x1\n");
                fprintf(g->out,
                    "    ldr x0, =4607182418800017408\n"    /* 1.0 */
                    "    fmov d0, x0\n"
                    ".Lfpow%d:\n"
                    "    cmp x10, #0\n"
                    "    b.le .Lfpowe%d\n"
                    "    fmul d0, d0, d1\n"
                    "    sub x10, x10, #1\n"
                    "    b .Lfpow%d\n"
                    ".Lfpowe%d:\n    fmov x0, d0\n", l, l, l, l);
                break;
            }
            if (fop) {
                gen_expr(g, n->lhs); push_tmp(g);
                gen_expr(g, n->rhs);
                fprintf(g->out, is_float_type(rt.base) ? "    fmov d1, x0\n" : "    scvtf d1, x0\n");
                pop_tmp(g, "x1");
                fprintf(g->out, is_float_type(lt.base) ? "    fmov d0, x1\n" : "    scvtf d0, x1\n");
                if (is_cmp) {
                    const char *cc = n->op==TK_EQ ? "eq" : n->op==TK_NEQ ? "ne" :
                                     n->op==TK_LT ? "mi" : n->op==TK_GT ? "gt" :
                                     n->op==TK_LE ? "ls" : "ge";
                    fprintf(g->out, "    fcmp d0, d1\n    cset x0, %s\n", cc);
                } else {
                    const char *op = n->op==TK_PLUS ? "fadd" : n->op==TK_MINUS ? "fsub" :
                                     n->op==TK_STAR ? "fmul" : "fdiv";
                    fprintf(g->out, "    %s d0, d0, d1\n    fmov x0, d0\n", op);
                }
                break;
            }
            /* integer path; pointer +/- scales by the pointee size */
            gen_expr(g, n->lhs); push_tmp(g);
            gen_expr(g, n->rhs);
            fprintf(g->out, "    mov x1, x0\n");
            pop_tmp(g, "x0");
            if ((n->op == TK_PLUS || n->op == TK_MINUS) && lt.ptr > 0 && rt.ptr > 0) {
                int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                fprintf(g->out, "    sub x0, x0, x1\n");
                if (sc != 1) fprintf(g->out, "    mov x9, #%d\n    sdiv x0, x0, x9\n", sc);
                break;
            }
            if ((n->op == TK_PLUS || n->op == TK_MINUS) && lt.ptr > 0 && rt.ptr == 0) {
                int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                if (sc != 1) fprintf(g->out, "    mov x9, #%d\n    mul x1, x1, x9\n", sc);
            } else if (n->op == TK_PLUS && rt.ptr > 0 && lt.ptr == 0) {
                int sc = type_size(g, rt.base, rt.ptr - 1, rt.sname);
                if (sc != 1) fprintf(g->out, "    mov x9, #%d\n    mul x0, x0, x9\n", sc);
            }
            gen_binop_apply(g, n->op, is_unsigned_val(lt.base, lt.ptr) || is_unsigned_val(rt.base, rt.ptr) ?
                            (n->op==TK_SHR || n->op==TK_SLASH || n->op==TK_PERCENT ?
                             is_unsigned_val(lt.base, lt.ptr) : 1) : 0);
            break;
        }
        case ND_UNARY: {
            if (n->op == TK_MINUS || n->op == TK_TILDE) {
                ExprType ot0 = type_of(g, n->operand);
                if (is_i128(ot0.base, ot0.ptr)) { gen_i128_unary(g, n); break; }
            }
            if (n->op == TK_MINUS) {
                gen_expr(g, n->operand);
                ExprType ot = type_of(g, n->operand);
                if (is_float_type(ot.base) && ot.ptr == 0)
                    fprintf(g->out, "    fmov d0, x0\n    fneg d0, d0\n    fmov x0, d0\n");
                else
                    fprintf(g->out, "    neg x0, x0\n");
            } else if (n->op == TK_NOT) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    cmp x0, #0\n    cset x0, eq\n");
            } else if (n->op == TK_TILDE) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    mvn x0, x0\n");
                ExprType ot = type_of(g, n->operand);
                if (ot.ptr == 0) {
                    int sz = type_size(g, ot.base, 0, ot.sname);
                    int sgn = is_signed_type(ot.base);
                    if (sz == 1)      fprintf(g->out, sgn ? "    sxtb x0, w0\n" : "    uxtb w0, w0\n");
                    else if (sz == 2) fprintf(g->out, sgn ? "    sxth x0, w0\n" : "    uxth w0, w0\n");
                    else if (sz == 4) fprintf(g->out, sgn ? "    sxtw x0, w0\n" : "    mov w0, w0\n");
                }
            } else if (n->op == TK_AMP) {
                gen_addr(g, n->operand);
            } else if (n->op == TK_STAR) {
                gen_expr(g, n->operand);
                ExprType pt = type_of(g, n->operand);
                int pptr = pt.ptr > 0 ? pt.ptr - 1 : 0;
                if (pt.base == TYPE_STRUCT && pptr == 0) break;
                gen_load_typed(g, pptr > 0 ? TYPE_USIZE : pt.base, type_size(g, pt.base, pptr, pt.sname));
            } else if (n->op == TK_PLUSPLUS || n->op == TK_MINUSMINUS) {
                if (n->operand->kind != ND_IDENT) {
                    fprintf(stderr, "codegen error: ++/-- requires a variable\n"); g->had_error = 1; break;
                }
                Sym *s = find_var(g, n->operand->name);
                if (!s) { fprintf(stderr, "codegen error: undefined variable '%s'\n", n->operand->name); g->had_error = 1; break; }
                if (is_float_type(s->type) && s->ptr == 0) {
                    fprintf(stderr, "codegen error: '++'/'--' is not supported on floating-point; use 'x = x + 1.0'\n");
                    g->had_error = 1; break;
                }
                gen_load_var(g, s);                       /* x0 = old value */
                int inc = (s->ptr > 0) ? type_size(g, s->type, s->ptr - 1, s->sname) : 1;
                fprintf(g->out, "    mov x9, #%d\n", inc);
                fprintf(g->out, n->op == TK_PLUSPLUS ? "    add x1, x0, x9\n" : "    sub x1, x0, x9\n");
                char lbl[LABEL_MAX];
                if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x10", lbl); }
                else              addr_local(g, "x10", s->offset);
                switch (s->size) {
                    case 1: fprintf(g->out, "    strb w1, [x10]\n"); break;
                    case 2: fprintf(g->out, "    strh w1, [x10]\n"); break;
                    case 4: fprintf(g->out, "    str w1, [x10]\n"); break;
                    default: fprintf(g->out, "    str x1, [x10]\n"); break;
                }
            }
            break;
        }
        case ND_CAST: {
            ExprType st0 = type_of(g, n->operand);
            if (is_i128(n->type, n->ptr)) {
                int ro = (int)n->int_val;
                gen_i128_operand(g, n->operand, ro);
                addr_local(g, "x0", ro);
                break;
            }
            if (is_i128(st0.base, st0.ptr)) {
                gen_expr(g, n->operand);
                if (n->ptr == 0 && n->type == TYPE_BOOL) {
                    fprintf(g->out, "    ldp x1, x2, [x0]\n    orr x1, x1, x2\n"
                                    "    cmp x1, #0\n    cset x0, ne\n");
                    break;
                }
                fprintf(g->out, "    ldr x0, [x0]\n");
                if (n->ptr == 0 && !is_float_type(n->type) &&
                    n->type != TYPE_STR && n->type != TYPE_VOID && n->type != TYPE_STRUCT) {
                    int sz = type_size(g, n->type, 0, NULL);
                    int sgn = is_signed_type(n->type);
                    if (sz == 1)      fprintf(g->out, sgn ? "    sxtb x0, w0\n" : "    uxtb w0, w0\n");
                    else if (sz == 2) fprintf(g->out, sgn ? "    sxth x0, w0\n" : "    uxth w0, w0\n");
                    else if (sz == 4) fprintf(g->out, sgn ? "    sxtw x0, w0\n" : "    mov w0, w0\n");
                }
                break;
            }
            gen_expr(g, n->operand);
            ExprType st = type_of(g, n->operand);
            int src_f = is_float_type(st.base) && st.ptr == 0;
            int dst_f = is_float_type(n->type) && n->ptr == 0;
            if (!src_f && dst_f) {
                if (st.ptr == 0 && is_unsigned_val(st.base, 0))
                    fprintf(g->out, "    ucvtf d0, x0\n    fmov x0, d0\n");
                else
                    fprintf(g->out, "    scvtf d0, x0\n    fmov x0, d0\n");
                break;
            }
            if (src_f && dst_f) break;
            if (src_f && !dst_f) {
                if (n->ptr == 0 && is_unsigned_val(n->type, 0))
                    fprintf(g->out, "    fmov d0, x0\n    fcvtzu x0, d0\n");
                else
                    fprintf(g->out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
            }
            if (n->ptr == 0 && n->type == TYPE_BOOL) {
                fprintf(g->out, "    cmp x0, #0\n    cset x0, ne\n");
                break;
            }
            if (n->ptr == 0 && !dst_f && n->type != TYPE_STR && n->type != TYPE_VOID && n->type != TYPE_STRUCT) {
                int sz = type_size(g, n->type, 0, NULL);
                int sgn = is_signed_type(n->type);
                if (sz == 1)      fprintf(g->out, sgn ? "    sxtb x0, w0\n" : "    uxtb w0, w0\n");
                else if (sz == 2) fprintf(g->out, sgn ? "    sxth x0, w0\n" : "    uxth w0, w0\n");
                else if (sz == 4) fprintf(g->out, sgn ? "    sxtw x0, w0\n" : "    mov w0, w0\n");
            }
            break;
        }
        case ND_ASSIGN: {
            Node *target = n->lhs;
            if (target->kind != ND_IDENT && target->kind != ND_MEMBER && target->kind != ND_INDEX &&
                !(target->kind == ND_UNARY && target->op == TK_STAR)) {
                fprintf(stderr, "codegen error: invalid assignment target\n"); g->had_error = 1; break;
            }
            ExprType tt = type_of(g, target);
            if (tt.arr > 0) {
                fprintf(stderr, "codegen error: whole-array assignment is not supported; copy element by element\n");
                g->had_error = 1; break;
            }
            if (is_i128(tt.base, tt.ptr)) {
                if (n->op != TK_ASSIGN) {
                    fprintf(stderr, "codegen error: compound assignment is not supported on 128-bit integers; write x = x + y\n");
                    g->had_error = 1; break;
                }
                ExprType rvt = type_of(g, n->rhs);
                gen_expr(g, n->rhs);
                push_tmp(g);
                gen_addr(g, target);
                pop_tmp(g, "x1");
                gen_i128_store(g, rvt);
                break;
            }
            if (is_dyn(tt.base, tt.ptr)) {
                ExprType rvt = type_of(g, n->rhs);
                gen_expr(g, n->rhs);
                push_tmp(g);
                gen_addr(g, target);
                pop_tmp(g, "x1");
                gen_dyn_store(g, rvt, tt.sname);
                break;
            }
            if (tt.base == TYPE_STRUCT && tt.ptr == 0 && n->op == TK_ASSIGN) {
                gen_addr(g, target);
                gen_store_struct(g, n->rhs);
                break;
            }
            int sz = type_size(g, tt.base, tt.ptr, tt.sname);
            DataType st = tt.ptr > 0 ? TYPE_USIZE : tt.base;
            if (n->op == TK_ASSIGN) {
                gen_expr(g, n->rhs);
                { ExprType rvt = type_of(g, n->rhs); gen_coerce_num(g, rvt, tt); }
                push_tmp(g);
                gen_addr(g, target);
                pop_tmp(g, "x1");
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov x0, x1\n");
            } else {
                TokenType bop = n->op == TK_PLUS_ASSIGN ? TK_PLUS :
                                n->op == TK_MINUS_ASSIGN ? TK_MINUS :
                                n->op == TK_STAR_ASSIGN ? TK_STAR : TK_SLASH;
                gen_addr(g, target); push_tmp(g);          /* stash the address (compute once) */
                gen_expr(g, n->rhs);
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, "    ldr x0, [sp]\n");
                gen_load_typed(g, st, sz);                  /* x0 = old, x1 = rhs */
                if (is_float_type(tt.base) && tt.ptr == 0) {
                    ExprType rt2 = type_of(g, n->rhs);
                    fprintf(g->out, "    fmov d0, x0\n");
                    fprintf(g->out, is_float_type(rt2.base) ? "    fmov d1, x1\n" : "    scvtf d1, x1\n");
                    const char *fop = bop==TK_PLUS ? "fadd" : bop==TK_MINUS ? "fsub" : bop==TK_STAR ? "fmul" : "fdiv";
                    fprintf(g->out, "    %s d0, d0, d1\n    fmov x0, d0\n", fop);
                } else {
                    if (tt.ptr > 0 && (bop == TK_PLUS || bop == TK_MINUS)) {
                        int sc = type_size(g, tt.base, tt.ptr - 1, tt.sname);
                        if (sc != 1) fprintf(g->out, "    mov x9, #%d\n    mul x1, x1, x9\n", sc);
                    }
                    gen_binop_apply(g, bop, is_unsigned_val(tt.base, tt.ptr));
                }
                fprintf(g->out, "    mov x1, x0\n");
                pop_tmp(g, "x0");                           /* the stashed address */
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov x0, x1\n");
            }
            break;
        }
        case ND_CALL: {
            Node *callee = n->operand;
            /* inline assembly intrinsic: asm("instruction") emits the text verbatim.
             * The compiler assumes NOTHING about it (no clobber tracking), so it
             * belongs in tiny leaf helpers like the ones in core/arch. */
            if (callee->kind == ND_IDENT && strcmp(callee->name, "asm") == 0) {
                if (n->nitems < 1) {
                    fprintf(stderr, "codegen error: asm() needs at least one string literal\n");
                    g->had_error = 1; break;
                }
                for (int i = 0; i < n->nitems; i++) {
                    if (n->items[i]->kind != ND_STR) {
                        fprintf(stderr, "codegen error: asm(): every argument must be a string literal\n");
                        g->had_error = 1; break;
                    }
                    fprintf(g->out, "    %s\n", n->items[i]->str_val);
                }
                break;
            }
            if (callee->kind == ND_MEMBER && callee->operand->kind == ND_IDENT &&
                strcmp(callee->operand->name, "io") == 0 && strcmp(callee->name, "out") == 0) {
                if (!g->io_imported) {
                    fprintf(stderr, "codegen error: io is not imported (use: import { io } from \"std\";)\n");
                    g->had_error = 1; break;
                }
                if (n->nitems < 1 || n->items[0]->kind != ND_STR) {
                    fprintf(stderr, "codegen error: io.out needs a literal format string\n");
                    g->had_error = 1; break;
                }
                Node *vals[64]; int nv = 0, nph = 0; int flen = 0;
                char *cf = build_c_format(g, n, n->items[0]->str_val, &flen, &nph, vals, &nv, 64);
                int nvals = n->nitems - 1;
                if (nph != nvals) {
                    fprintf(stderr, "codegen error: io.out: %d placeholder(s) but %d argument(s)\n", nph, nvals);
                    g->had_error = 1; free(cf); break;
                }
                int idx = intern_string(g, cf, flen);
                free(cf);
                for (int k = 1; k < n->nitems; k++) {
                    Node *a = n->items[k];
                    if (a->kind == ND_CALL && a->int_val) {
                        ExprType at = type_of(g, a);
                        if (at.base == TYPE_STRUCT && at.ptr == 0) gen_expr(g, a);
                    }
                }
                /* printf per AAPCS64: ints x0.., floats d0.., stack for overflow */
                int total = 1 + nv;
                int cls_gpr[80], cls_fp[80], cls_stk[80];
                int ng = 0, nf = 0, nstk = 0;
                for (int i = 0; i < total && i < 80; i++) {
                    int isf = i >= 1 && is_float_type(infer_type(g, vals[i - 1]));
                    cls_gpr[i] = cls_fp[i] = cls_stk[i] = -1;
                    if (isf) { if (nf < 8) cls_fp[i] = nf++; else cls_stk[i] = nstk++; }
                    else     { if (ng < 8) cls_gpr[i] = ng++; else cls_stk[i] = nstk++; }
                }
                int callspace = (nstk * 8 + 15) / 16 * 16;
                char flbl[64]; snprintf(flbl, sizeof(flbl), "mvs_str_%d", idx);
                addr_label(g, "x0", flbl);
                push_tmp(g);
                for (int i = 0; i < nv; i++) {
                    gen_expr(g, vals[i]);
                    ExprType vt128 = type_of(g, vals[i]);
                    if (is_i128(vt128.base, vt128.ptr)) {
                        g->need_i128 = 1;
                        fprintf(g->out, "    bl %s\n",
                                vt128.base == TYPE_U128 ? "mvs_u128_str" : "mvs_i128_str");
                    }
                    push_tmp(g);
                }
                if (callspace) fprintf(g->out, "    sub sp, sp, #%d\n", callspace);
                for (int i = 0; i < total; i++) {
                    int srcoff = callspace + (total - 1 - i) * 16;
                    fprintf(g->out, "    ldr x9, [sp, #%d]\n", srcoff);
                    if (cls_fp[i] >= 0)       fprintf(g->out, "    fmov d%d, x9\n", cls_fp[i]);
                    else if (cls_gpr[i] >= 0) fprintf(g->out, "    mov x%d, x9\n", cls_gpr[i]);
                    else                      fprintf(g->out, "    str x9, [sp, #%d]\n", cls_stk[i] * 8);
                }
                fprintf(g->out, "    bl printf\n");
                if (callspace) fprintf(g->out, "    add sp, sp, #%d\n", callspace);
                fprintf(g->out, "    add sp, sp, #%d\n", total * 16);
                break;
            }
            {
                ExprType rt = type_of(g, n);
                if ((rt.base == TYPE_STRUCT || is_blob16(rt.base, rt.ptr)) && rt.ptr == 0 && n->int_val) {
                    addr_local(g, "x0", (int)n->int_val);
                    gen_call(g, n, 1);
                    addr_local(g, "x0", (int)n->int_val);
                } else {
                    gen_call(g, n, 0);
                }
            }
            break;
        }
        case ND_MEMBER: {
            {
                ExprType bt = type_of(g, n->operand);
                if (bt.arr > 0 && n->name && strcmp(n->name, "len") == 0) {
                    fprintf(g->out, "    mov x0, #%d\n", bt.arr);
                    break;
                }
            }
            ExprType et = type_of(g, n);
            gen_addr(g, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) break;
            if (et.arr > 0) break;
            if (is_blob16(et.base, et.ptr)) break;
            gen_load_typed(g, et.ptr > 0 ? TYPE_USIZE : et.base, type_size(g, et.base, et.ptr, et.sname));
            break;
        }
        case ND_INDEX: {
            ExprType et = type_of(g, n);
            gen_addr(g, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) break;
            if (is_blob16(et.base, et.ptr)) break;
            gen_load_typed(g, et.ptr > 0 ? TYPE_USIZE : et.base, type_size(g, et.base, et.ptr, et.sname));
            break;
        }
        case ND_ARRAY_LIT:
            fprintf(stderr, "codegen error: an array literal can only initialize a variable\n");
            g->had_error = 1;
            break;
        case ND_FRAMEREF:
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

/* Write a struct value to the destination address in x0 */
static void gen_store_struct(Gen *g, Node *value) {
    push_tmp(g);   /* [sp] = destination address */
    if (value->kind == ND_STRUCT_LIT) {
        StructInfo *s = find_struct(g, value->name);
        if (!s) { fprintf(stderr, "codegen error: unknown struct '%s'\n", value->name); g->had_error = 1; }
        else for (int i = 0; i < value->nitems; i++) {
            Node *fi = value->items[i];
            Field *f = find_field(s, fi->lhs->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", fi->lhs->name, s->name); g->had_error = 1; continue; }
            if (f->arr > 0) {
                if (fi->rhs->kind != ND_ARRAY_LIT) {
                    fprintf(stderr, "codegen error: array field '%s' can only be initialized with an array literal\n", f->name);
                    g->had_error = 1; continue;
                }
                int esz = f->size / f->arr;
                for (int j = 0; j < fi->rhs->nitems && j < f->arr; j++) {
                    Node *el = fi->rhs->items[j];
                    if (f->type == TYPE_STRUCT && f->ptr == 0) {
                        fprintf(g->out, "    ldr x0, [sp]\n    add x0, x0, #%d\n", f->offset + j * esz);
                        gen_store_struct(g, el);
                    } else {
                        gen_expr(g, el);
                        ExprType vt2 = type_of(g, el), dt2 = { f->type, f->ptr, f->sname, f->sig, 0 };
                        gen_coerce_num(g, vt2, dt2);
                        fprintf(g->out, "    mov x1, x0\n");
                        fprintf(g->out, "    ldr x0, [sp]\n");
                        if (f->offset + j * esz) fprintf(g->out, "    add x0, x0, #%d\n", f->offset + j * esz);
                        if (is_dyn(f->type, f->ptr))       gen_dyn_store(g, vt2, f->sname);
                        else if (is_i128(f->type, f->ptr)) gen_i128_store(g, vt2);
                        else gen_store_typed(g, f->ptr > 0 ? TYPE_USIZE : f->type, esz);
                    }
                }
            } else if (f->type == TYPE_STRUCT && f->ptr == 0) {
                fprintf(g->out, "    ldr x0, [sp]\n");
                if (f->offset) fprintf(g->out, "    add x0, x0, #%d\n", f->offset);
                gen_store_struct(g, fi->rhs);
            } else {
                gen_expr(g, fi->rhs);
                ExprType vt2 = type_of(g, fi->rhs), dt2 = { f->type, f->ptr, f->sname, f->sig, 0 };
                gen_coerce_num(g, vt2, dt2);            /* int literal into a float field etc. */
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, "    ldr x0, [sp]\n");
                if (f->offset) fprintf(g->out, "    add x0, x0, #%d\n", f->offset);
                if (is_i128(f->type, f->ptr)) {
                    gen_i128_store(g, vt2);             /* 16-byte copy; widens a 64-bit rhs */
                } else if (is_dyn(f->type, f->ptr)) {
                    gen_dyn_store(g, vt2, f->sname);    /* fat-pointer copy / wrap */
                } else {
                    gen_store_typed(g, f->ptr > 0 ? TYPE_USIZE : f->type, f->size);
                }
            }
        }
    } else if (value->kind == ND_CALL) {
        fprintf(g->out, "    ldr x0, [sp]\n");
        gen_call(g, value, 1);
    } else {
        ExprType vt = type_of(g, value);
        StructInfo *s = find_struct(g, vt.sname);
        int size = s ? s->size : 0;
        gen_addr(g, value);
        fprintf(g->out, "    mov x9, x0\n");
        fprintf(g->out, "    ldr x10, [sp]\n");
        gen_memcpy(g, size);
    }
    fprintf(g->out, "    add sp, sp, #16\n");
}

/* ---------- calls (AAPCS64) ---------- */

static void gen_call(Gen *g, Node *n, int has_sret) {
    Node *callee = n->operand;
    Node *target = NULL;
    Node *self_expr = NULL;
    int   self_is_ptr = 0;

    if (callee->kind == ND_MEMBER) {
        ExprType bt0 = type_of(g, callee->operand);
        if (is_dyn(bt0.base, bt0.ptr) && bt0.sname) {
            gen_dyn_call(g, n, has_sret, bt0.sname);
            return;
        }
    }

    Node *sig = expr_func_sig(g, callee);
    int indirect = (sig != NULL);

    if (!indirect) {
        if (callee->kind == ND_IDENT) {
            target = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
            if (!target) target = find_func(g, "", callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function '%s'\n", callee->name); g->had_error = 1; return; }
        } else if (callee->kind == ND_MEMBER) {
            ExprType bt = type_of(g, callee->operand);
            if (bt.base == TYPE_STRUCT && bt.sname) {
                target = find_func(g, bt.sname, callee->name);
                if (target) { self_expr = callee->operand; self_is_ptr = (bt.ptr > 0); }
            }
            if (!target && bt.ptr == 0 && bt.base != TYPE_STRUCT && bt.base != TYPE_DYN &&
                bt.base != TYPE_FUNC && bt.base != TYPE_VOID && bt.base != TYPE_UNKNOWN &&
                (callee->operand->kind != ND_IDENT || find_var(g, callee->operand->name))) {
                target = find_func(g, datatype_name(bt.base), callee->name);
                if (target) { self_expr = callee->operand; self_is_ptr = 0; }
            }
            if (!target && callee->operand->kind == ND_IDENT)
                target = find_func(g, callee->operand->name, callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function/method '%s' (did you import it?)\n", callee->name); g->had_error = 1; return; }
        } else {
            fprintf(stderr, "codegen error: unsupported call target\n"); g->had_error = 1; return;
        }
        sig = target;
    }

    int returns_struct = ((sig->type == TYPE_STRUCT || is_blob16(sig->type, sig->ptr)) && sig->ptr == 0);
    if (returns_struct && !has_sret) {
        fprintf(stderr, "codegen error: a struct-returning call must be assigned to a variable\n");
        g->had_error = 1; return;
    }

    /* variadic MVS call: extras are packed into dyn blobs; the ABI sees fixed + ptr + len */
    int va_fixed = -1, va_extra = 0; long long va_base = 0;
    if (!indirect && target && target->variadic && !target->is_extern && !self_expr) {
        va_fixed = target->nitems - 2;
        va_extra = n->nitems - va_fixed; if (va_extra < 0) va_extra = 0;
        va_base = n->lhs ? n->lhs->int_val : 0;
    }
    int n_abi = va_fixed >= 0 ? va_fixed + 2 : n->nitems;

    int nself = self_expr ? 1 : 0;
    int total = n_abi + nself + (has_sret ? 1 : 0);
    int arg_start0 = (has_sret ? 1 : 0) + nself;
    int poff0 = nself;
    int cls_gpr[80], cls_fp[80], cls_stk[80];
    int ng = 0, nf = 0, nstk = 0;
    for (int i = 0; i < total && i < 80; i++) {
        int isf = 0;
        if (i >= arg_start0) {
            int pidx0 = i - arg_start0, pi0 = pidx0 + poff0;
            if (pi0 < sig->nitems) isf = is_float_type(sig->items[pi0]->type) && sig->items[pi0]->ptr == 0;
            else if (pidx0 < n->nitems) { ExprType at0 = type_of(g, n->items[pidx0]); isf = is_float_type(at0.base) && at0.ptr == 0; }
        }
        cls_gpr[i] = cls_fp[i] = cls_stk[i] = -1;
        if (has_sret && i == 0) continue;                 /* sret rides in x8 */
        if (isf) { if (nf < 8) cls_fp[i] = nf++; else cls_stk[i] = nstk++; }
        else     { if (ng < 8) cls_gpr[i] = ng++; else cls_stk[i] = nstk++; }
    }
    int callspace = (nstk * 8 + 15) / 16 * 16;

    if (has_sret) push_tmp(g);                            /* destination address (in x0) */
    if (self_expr) {
        if (self_is_ptr) gen_expr(g, self_expr);
        else             gen_addr(g, self_expr);
        push_tmp(g);
    }
    int poff = self_expr ? 1 : 0;
    for (int i = 0; i < (va_fixed >= 0 ? va_fixed : n->nitems); i++) {
        gen_expr(g, n->items[i]);
        if (i + poff < sig->nitems) {
            Node *pp = sig->items[i + poff];
            ExprType pt = { pp->type, pp->ptr, pp->type_name, pp->sig, 0 }, at = type_of(g, n->items[i]);
            gen_coerce_num(g, at, pt);
            if (is_dyn(pp->type, pp->ptr) && !is_dyn(at.base, at.ptr) && n->items[i]->int_val) {
                fprintf(g->out, "    mov x1, x0\n");
                addr_local(g, "x0", (int)n->items[i]->int_val);
                gen_dyn_store(g, at, pp->type_name);
                addr_local(g, "x0", (int)n->items[i]->int_val);
            }
        }
        push_tmp(g);
    }
    if (va_fixed >= 0) {
        const char *trait = sig->items[va_fixed]->type_name;
        for (int k = 0; k < va_extra; k++) {
            Node *arg = n->items[va_fixed + k];
            ExprType at = type_of(g, arg);
            int boff = (int)va_base - k * 16;
            int voff = (int)va_base - (va_extra + k) * 16;
            gen_expr(g, arg);
            if (is_dyn(at.base, at.ptr)) {
                fprintf(g->out, "    mov x1, x0\n");
                addr_local(g, "x0", boff);
                gen_dyn_store(g, at, trait);
            } else if ((at.base == TYPE_STRUCT && at.ptr == 0) || is_i128(at.base, at.ptr)) {
                g->need_vtables = 1;
                char vt[LABEL_MAX];
                snprintf(vt, sizeof(vt), "mvs_vt_%s_%s", trait, at.sname ? at.sname : datatype_name(at.base));
                fprintf(g->out, "    mov x1, x0\n");
                addr_local(g, "x0", boff);
                fprintf(g->out, "    str x1, [x0]\n");
                addr_label(g, "x2", vt);
                fprintf(g->out, "    str x2, [x0, #8]\n");
            } else {
                g->need_vtables = 1;
                char vt[LABEL_MAX];
                snprintf(vt, sizeof(vt), "mvs_vt_%s_%s", trait, datatype_name(at.base));
                fprintf(g->out, "    mov x1, x0\n");
                addr_local(g, "x0", voff);
                gen_store_typed(g, at.ptr > 0 ? TYPE_USIZE : at.base,
                                type_size(g, at.base, at.ptr, at.sname));
                addr_local(g, "x1", voff);
                addr_local(g, "x0", boff);
                fprintf(g->out, "    str x1, [x0]\n");
                addr_label(g, "x2", vt);
                fprintf(g->out, "    str x2, [x0, #8]\n");
            }
        }
        if (va_extra) addr_local(g, "x0", (int)va_base);
        else          fprintf(g->out, "    mov x0, xzr\n");
        push_tmp(g);
        fprintf(g->out, "    mov x0, #%d\n", va_extra);
        push_tmp(g);
    }
    int extra = indirect ? 1 : 0;
    if (indirect) { gen_expr(g, callee); push_tmp(g); }

    int arg_start = arg_start0;
    if (callspace) fprintf(g->out, "    sub sp, sp, #%d\n", callspace);
    int c_abi = sig->is_extern || sig->is_export;
    for (int i = 0; i < total; i++) {
        int srcoff = callspace + (total + extra - 1 - i) * 16;
        int pidx = i - arg_start;
        int pi = pidx + poff;
        int p_is_f32 = i >= arg_start && pi < sig->nitems &&
                       sig->items[pi]->type == TYPE_F32 && sig->items[pi]->ptr == 0;
        fprintf(g->out, "    ldr x9, [sp, #%d]\n", srcoff);
        if (has_sret && i == 0) {
            fprintf(g->out, "    mov x8, x9\n");
        } else if (cls_fp[i] >= 0) {
            fprintf(g->out, "    fmov d%d, x9\n", cls_fp[i]);
            if (c_abi && p_is_f32)
                fprintf(g->out, "    fcvt s%d, d%d\n", cls_fp[i], cls_fp[i]);
        } else if (cls_gpr[i] >= 0) {
            fprintf(g->out, "    mov x%d, x9\n", cls_gpr[i]);
        } else {
            if (c_abi && p_is_f32)
                fprintf(g->out, "    fmov d16, x9\n    fcvt s16, d16\n    fmov w9, s16\n");
            fprintf(g->out, "    str x9, [sp, #%d]\n", cls_stk[i] * 8);
        }
    }

    if (indirect) {
        fprintf(g->out, "    ldr x9, [sp, #%d]\n", callspace);
        fprintf(g->out, "    blr x9\n");
    } else {
        char lbl[LABEL_MAX]; func_label_of(target, lbl);
        fprintf(g->out, "    bl %s\n", lbl);
    }
    if (callspace) fprintf(g->out, "    add sp, sp, #%d\n", callspace);
    fprintf(g->out, "    add sp, sp, #%d\n", (total + extra) * 16);
    if (is_float_type(sig->type) && sig->ptr == 0) {
        if (c_abi && sig->type == TYPE_F32)
            fprintf(g->out, "    fcvt d0, s0\n    fmov x0, d0\n");
        else
            fprintf(g->out, "    fmov x0, d0\n");
    }
}

/* ---------- statements ---------- */

static void gen_stmt(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_VAR_DECL: {
            Sym *s = &g->locals[n->int_val];
            if (n->operand && s) {
                if (s->arr > 0) {
                    if (n->operand->kind != ND_ARRAY_LIT) {
                        fprintf(stderr, "codegen error: an array variable can only be initialized with an array literal\n");
                        g->had_error = 1; break;
                    }
                    int esz = type_size(g, s->type, s->ptr, s->sname);
                    for (int i = 0; i < n->operand->nitems && i < s->arr; i++) {
                        Node *el = n->operand->items[i];
                        int eoff = s->offset - i * esz;
                        if (s->type == TYPE_STRUCT && s->ptr == 0) {
                            addr_local(g, "x0", eoff);
                            gen_store_struct(g, el);
                        } else {
                            gen_expr(g, el);
                            ExprType vt2 = type_of(g, el), dt2 = { s->type, s->ptr, s->sname, s->sig, 0 };
                            gen_coerce_num(g, vt2, dt2);
                            fprintf(g->out, "    mov x1, x0\n");
                            addr_local(g, "x0", eoff);
                            if (is_dyn(s->type, s->ptr))       gen_dyn_store(g, vt2, s->sname);
                            else if (is_i128(s->type, s->ptr)) gen_i128_store(g, vt2);
                            else gen_store_typed(g, s->ptr > 0 ? TYPE_USIZE : s->type, esz);
                        }
                    }
                } else if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    char lbl[LABEL_MAX];
                    if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x0", lbl); }
                    else              addr_local(g, "x0", s->offset);
                    gen_store_struct(g, n->operand);
                } else if (is_blob16(s->type, s->ptr)) {
                    ExprType rvt = type_of(g, n->operand);
                    gen_expr(g, n->operand);
                    fprintf(g->out, "    mov x1, x0\n");
                    char lbl[LABEL_MAX];
                    if (s->is_global) { global_label(s->name, lbl); addr_label(g, "x0", lbl); }
                    else              addr_local(g, "x0", s->offset);
                    if (is_dyn(s->type, s->ptr)) gen_dyn_store(g, rvt, s->sname);
                    else                         gen_i128_store(g, rvt);
                } else {
                    gen_expr(g, n->operand);
                    ExprType vt = type_of(g, n->operand), dt = { s->type, s->ptr, s->sname, s->sig, s->arr };
                    gen_coerce_num(g, vt, dt);
                    gen_store_var(g, s);
                }
            }
            g->visible[g->nvisible++] = (int)n->int_val;
            break;
        }
        case ND_EXPR_STMT:
            gen_expr(g, n->operand);
            break;
        case ND_RETURN:
            if (g->sret_off != 0 && n->operand) {
                if (g->cur_ret_i128 || g->cur_ret_dyn) {
                    ExprType rvt = type_of(g, n->operand);
                    gen_expr(g, n->operand);
                    fprintf(g->out, "    mov x1, x0\n");
                    addr_local(g, "x9", g->sret_off);
                    fprintf(g->out, "    ldr x0, [x9]\n");
                    if (g->cur_ret_dyn) gen_dyn_store(g, rvt, g->cur_ret_dyn);
                    else                gen_i128_store(g, rvt);
                    addr_local(g, "x9", g->sret_off);
                    fprintf(g->out, "    ldr x0, [x9]\n");
                } else {
                    addr_local(g, "x9", g->sret_off);
                    fprintf(g->out, "    ldr x0, [x9]\n");
                    gen_store_struct(g, n->operand);
                    addr_local(g, "x9", g->sret_off);
                    fprintf(g->out, "    ldr x0, [x9]\n");
                }
            } else if (n->operand) {
                gen_expr(g, n->operand);
                ExprType vt = type_of(g, n->operand);
                int vf = is_float_type(vt.base) && vt.ptr == 0;
                if (g->cur_ret_float && !vf) fprintf(g->out, "    scvtf d0, x0\n    fmov x0, d0\n");
                else if (!g->cur_ret_float && vf) fprintf(g->out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
                if (g->cur_ret_float) {
                    fprintf(g->out, "    fmov d0, x0\n");
                    if (g->cur_ret_f32c) fprintf(g->out, "    fcvt s0, d0\n");
                }
            } else {
                fprintf(g->out, "    mov x0, xzr\n");
            }
            fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
            break;
        case ND_IF: {
            int lelse = new_label(g), lend = new_label(g);
            gen_expr(g, n->cond);
            fprintf(g->out, "    cbz x0, .Lelse%d\n", lelse);
            gen_stmt(g, n->then_branch);
            fprintf(g->out, "    b .Lend%d\n.Lelse%d:\n", lend, lelse);
            gen_stmt(g, n->else_branch);
            fprintf(g->out, ".Lend%d:\n", lend);
            break;
        }
        case ND_WHILE: {
            int lbeg = new_label(g), lend = new_label(g);
            if (g->nloops < MAX_LOOP) { g->loops[g->nloops].brk = lend; g->loops[g->nloops].cont = lbeg; g->nloops++; }
            fprintf(g->out, ".Lbeg%d:\n.Lcont%d:\n", lbeg, lbeg);   /* continue re-tests the condition */
            gen_expr(g, n->cond);
            fprintf(g->out, "    cbz x0, .Lend%d\n", lend);
            gen_stmt(g, n->body);
            fprintf(g->out, "    b .Lbeg%d\n.Lend%d:\n", lbeg, lend);
            if (g->nloops > 0) g->nloops--;
            break;
        }
        case ND_DOWHILE: {
            int lbeg = new_label(g), lend = new_label(g), lcond = new_label(g);
            if (g->nloops < MAX_LOOP) { g->loops[g->nloops].brk = lend; g->loops[g->nloops].cont = lcond; g->nloops++; }
            fprintf(g->out, ".Lbeg%d:\n", lbeg);
            gen_stmt(g, n->body);
            fprintf(g->out, ".Lcont%d:\n", lcond);                  /* continue jumps to the test */
            gen_expr(g, n->cond);
            fprintf(g->out, "    cbnz x0, .Lbeg%d\n.Lend%d:\n", lbeg, lend);
            if (g->nloops > 0) g->nloops--;
            break;
        }
        case ND_FOR: {
            int lbeg = new_label(g), lend = new_label(g), lstep = new_label(g);
            int save_vis = g->nvisible;
            if (n->init) {
                if (n->init->kind == ND_VAR_DECL) gen_stmt(g, n->init);
                else gen_expr(g, n->init);
            }
            if (g->nloops < MAX_LOOP) { g->loops[g->nloops].brk = lend; g->loops[g->nloops].cont = lstep; g->nloops++; }
            fprintf(g->out, ".Lbeg%d:\n", lbeg);
            if (n->cond) {
                gen_expr(g, n->cond);
                fprintf(g->out, "    cbz x0, .Lend%d\n", lend);
            }
            gen_stmt(g, n->body);
            fprintf(g->out, ".Lcont%d:\n", lstep);                  /* continue runs the step */
            if (n->step) gen_expr(g, n->step);
            fprintf(g->out, "    b .Lbeg%d\n.Lend%d:\n", lbeg, lend);
            if (g->nloops > 0) g->nloops--;
            g->nvisible = save_vis;
            break;
        }
        case ND_SWITCH: {
            int lend = new_label(g);
            if (g->nloops < MAX_LOOP) {
                int outer_cont = g->nloops > 0 ? g->loops[g->nloops - 1].cont : lend;
                g->loops[g->nloops].brk = lend; g->loops[g->nloops].cont = outer_cont; g->nloops++;
            }
            gen_expr(g, n->cond);
            addr_local(g, "x9", (int)n->int_val);
            fprintf(g->out, "    str x0, [x9]\n");
            int *labels = (int *)malloc(sizeof(int) * (n->nitems > 0 ? n->nitems : 1));
            int ldefault = -1;
            for (int i = 0; i < n->nitems; i++) {
                labels[i] = new_label(g);
                Node *cs = n->items[i];
                if (cs->operand) {
                    gen_expr(g, cs->operand);
                    addr_local(g, "x9", (int)n->int_val);
                    fprintf(g->out, "    ldr x1, [x9]\n");
                    fprintf(g->out, "    cmp x1, x0\n    b.eq .Lcase%d\n", labels[i]);
                } else ldefault = i;
            }
            if (ldefault >= 0) fprintf(g->out, "    b .Lcase%d\n", labels[ldefault]);
            else               fprintf(g->out, "    b .Lend%d\n", lend);
            for (int i = 0; i < n->nitems; i++) {
                fprintf(g->out, ".Lcase%d:\n", labels[i]);
                Node *cs = n->items[i];
                int save_vis = g->nvisible;
                for (int j = 0; j < cs->nitems; j++) gen_stmt(g, cs->items[j]);
                g->nvisible = save_vis;
            }
            fprintf(g->out, ".Lend%d:\n", lend);
            free(labels);
            if (g->nloops > 0) g->nloops--;
            break;
        }
        case ND_BREAK:
            if (g->nloops > 0) fprintf(g->out, "    b .Lend%d\n", g->loops[g->nloops - 1].brk);
            else { fprintf(stderr, "codegen error: break outside a loop\n"); g->had_error = 1; }
            break;
        case ND_CONTINUE:
            if (g->nloops > 0) fprintf(g->out, "    b .Lcont%d\n", g->loops[g->nloops - 1].cont);
            else { fprintf(stderr, "codegen error: continue outside a loop\n"); g->had_error = 1; }
            break;
        case ND_BLOCK: {
            int save_vis = g->nvisible;
            for (int i = 0; i < n->nitems; i++) gen_stmt(g, n->items[i]);
            g->nvisible = save_vis;
            break;
        }
        default:
            gen_expr(g, n);
    }
}

/* ---------- functions ---------- */

static void gen_func(Gen *g, Node *fn, Node *program) {
    char lbl[LABEL_MAX];
    func_label_of(fn, lbl);
    g->cur_ns = fn->mod ? fn->mod : "";

    g->nlocals = 0;
    g->nvisible = 0;
    int frame = 0;
    g->sret_off = 0;

    int returns_struct = ((fn->type == TYPE_STRUCT || is_blob16(fn->type, fn->ptr)) && fn->ptr == 0 && !fn->is_extern);
    if (returns_struct)
        g->sret_off = add_local(g, "$sret", TYPE_USIZE, 0, 0, NULL, NULL, &frame);
    g->cur_ret_float = is_float_type(fn->type) && fn->ptr == 0;
    g->cur_ret_f32c = fn->is_export && fn->type == TYPE_F32 && fn->ptr == 0;
    g->cur_ret_i128 = is_i128(fn->type, fn->ptr);
    g->cur_ret_dyn = is_dyn(fn->type, fn->ptr) ? fn->type_name : NULL;

    int param_start = g->nlocals;
    for (int i = 0; i < fn->nitems; i++)
        add_local(g, fn->items[i]->name, fn->items[i]->type, fn->items[i]->ptr, 0, fn->items[i]->type_name, fn->items[i]->sig, &frame);
    collect_locals(g, fn->body, &frame);
    /* only the parameters are pre-visible; collect_struct_temps scopes the body
     * itself so shadowed names resolve as they will during real gen */
    int saved_vis = g->nvisible;
    for (int i = 0; i < fn->nitems; i++) g->visible[g->nvisible++] = param_start + i;
    collect_struct_temps(g, fn->body, &frame);
    g->nvisible = saved_vis;

    int frame_size = (frame + 15) / 16 * 16;

    fprintf(g->out, "\n%s:\n", lbl);
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    if (frame_size > 0) {
        if (frame_size <= 4095) fprintf(g->out, "    sub sp, sp, #%d\n", frame_size);
        else fprintf(g->out, "    mov x9, #%d\n    sub sp, sp, x9\n", frame_size);
    }

    /* spill parameters: ints from x0.., floats from d0.., overflow at [x29, #16 + k*8];
     * the hidden struct-return pointer arrives in x8 */
    int png = 0, pnf = 0, pnstk = 0;
    if (returns_struct) {
        addr_local(g, "x9", g->sret_off);
        fprintf(g->out, "    str x8, [x9]\n");
    }
    for (int i = 0; i < fn->nitems; i++) {
        Node *p = fn->items[i];
        Sym *psym = &g->locals[param_start + i];
        int is_float_param = is_float_type(p->type) && p->ptr == 0;
        if ((p->type == TYPE_STRUCT || p->type == TYPE_I128 || p->type == TYPE_U128 || p->type == TYPE_DYN) && p->ptr == 0) {
            int ssize = type_size(g, p->type, 0, p->type_name);
            if (png < 8) fprintf(g->out, "    mov x9, x%d\n", png++);
            else         fprintf(g->out, "    ldr x9, [x29, #%d]\n", 16 + 8 * pnstk++);
            addr_local(g, "x10", psym->offset);
            gen_memcpy(g, ssize);
            continue;
        }
        if (is_float_param) {
            if (pnf < 8) {
                if (fn->is_export && p->type == TYPE_F32)
                    fprintf(g->out, "    fcvt d%d, s%d\n", pnf, pnf);
                fprintf(g->out, "    fmov x0, d%d\n", pnf++);
            } else if (fn->is_export && p->type == TYPE_F32) {
                fprintf(g->out, "    ldr s0, [x29, #%d]\n    fcvt d0, s0\n    fmov x0, d0\n", 16 + 8 * pnstk++);
            } else {
                fprintf(g->out, "    ldr x0, [x29, #%d]\n", 16 + 8 * pnstk++);
            }
            gen_store_var(g, psym);
        } else {
            /* spill through x9/x10 ONLY: x0..x7 still carry the remaining parameters */
            if (png < 8) fprintf(g->out, "    mov x9, x%d\n", png++);
            else         fprintf(g->out, "    ldr x9, [x29, #%d]\n", 16 + 8 * pnstk++);
            addr_local(g, "x10", psym->offset);
            switch (psym->size) {
                case 1:  fprintf(g->out, "    strb w9, [x10]\n"); break;
                case 2:  fprintf(g->out, "    strh w9, [x10]\n"); break;
                case 4:  fprintf(g->out, "    str w9, [x10]\n"); break;
                default: fprintf(g->out, "    str x9, [x10]\n"); break;
            }
        }
    }
    for (int i = 0; i < fn->nitems; i++) g->visible[g->nvisible++] = param_start + i;

    if ((fn->ns == NULL || fn->ns[0] == 0) && strcmp(fn->name, "main") == 0) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind == ND_VAR_DECL && d->operand) {
                Sym *s = find_var(g, d->name);
                if (!s) continue;
                char gl[LABEL_MAX]; global_label(s->name, gl);
                if (s->arr > 0) {
                    if (d->operand->kind != ND_ARRAY_LIT) {
                        fprintf(stderr, "codegen error: an array variable can only be initialized with an array literal\n");
                        g->had_error = 1; continue;
                    }
                    int esz = type_size(g, s->type, s->ptr, s->sname);
                    for (int j = 0; j < d->operand->nitems && j < s->arr; j++) {
                        Node *el = d->operand->items[j];
                        if (s->type == TYPE_STRUCT && s->ptr == 0) {
                            addr_label(g, "x0", gl);
                            if (j) fprintf(g->out, "    add x0, x0, #%d\n", j * esz);
                            gen_store_struct(g, el);
                        } else {
                            gen_expr(g, el);
                            ExprType vt2 = type_of(g, el), dt2 = { s->type, s->ptr, s->sname, s->sig, 0 };
                            gen_coerce_num(g, vt2, dt2);
                            fprintf(g->out, "    mov x1, x0\n");
                            addr_label(g, "x0", gl);
                            if (j) fprintf(g->out, "    add x0, x0, #%d\n", j * esz);
                            if (is_dyn(s->type, s->ptr))       gen_dyn_store(g, vt2, s->sname);
                            else if (is_i128(s->type, s->ptr)) gen_i128_store(g, vt2);
                            else gen_store_typed(g, s->ptr > 0 ? TYPE_USIZE : s->type, esz);
                        }
                    }
                } else if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    addr_label(g, "x0", gl);
                    gen_store_struct(g, d->operand);
                } else if (is_blob16(s->type, s->ptr)) {
                    ExprType rvt = type_of(g, d->operand);
                    gen_expr(g, d->operand);
                    fprintf(g->out, "    mov x1, x0\n");
                    addr_label(g, "x0", gl);
                    if (is_dyn(s->type, s->ptr)) gen_dyn_store(g, rvt, s->sname);
                    else                         gen_i128_store(g, rvt);
                } else {
                    gen_expr(g, d->operand);
                    gen_store_var(g, s);
                }
            }
        }
    }

    gen_stmt(g, fn->body);

    fprintf(g->out, "    mov x0, xzr\n    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
}

/* ---------- 128-bit helper routines (A64) ---------- */

static void emit_i128_helpers(Gen *g) {
    fputs(
        "\nmvs_u128_divmod:\n"                    /* x0=&dividend x1=&divisor x2=&quot x3=&rem */
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"
        "    ldp x4, x5, [x0]\n"                  /* dividend */
        "    ldp x6, x7, [x1]\n"                  /* divisor */
        "    mov x9, xzr\n    mov x10, xzr\n"     /* remainder */
        "    mov x11, xzr\n    mov x12, xzr\n"    /* quotient */
        "    mov x13, #127\n"
        ".Ludm_loop:\n"
        "    extr x10, x10, x9, #63\n"            /* rem <<= 1 */
        "    lsl x9, x9, #1\n"
        "    cmp x13, #64\n    b.lt .Ludm_lo\n"
        "    sub x14, x13, #64\n    lsr x15, x5, x14\n    b .Ludm_got\n"
        ".Ludm_lo:\n    lsr x15, x4, x13\n"
        ".Ludm_got:\n    and x15, x15, #1\n    orr x9, x9, x15\n"
        "    cmp x10, x7\n    b.hi .Ludm_ge\n    b.lo .Ludm_next\n"
        "    cmp x9, x6\n    b.lo .Ludm_next\n"
        ".Ludm_ge:\n"
        "    subs x9, x9, x6\n    sbc x10, x10, x7\n"
        "    cmp x13, #64\n    b.lt .Ludm_ql\n"
        "    sub x14, x13, #64\n    mov x15, #1\n    lsl x15, x15, x14\n    orr x12, x12, x15\n    b .Ludm_next\n"
        ".Ludm_ql:\n    mov x15, #1\n    lsl x15, x15, x13\n    orr x11, x11, x15\n"
        ".Ludm_next:\n    subs x13, x13, #1\n    b.ge .Ludm_loop\n"
        "    stp x11, x12, [x2]\n"
        "    stp x9, x10, [x3]\n"
        "    ldp x29, x30, [sp], #16\n    ret\n"
        "\nmvs_s128_divmod:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #96\n"
        "    stp x2, x3, [sp, #64]\n"             /* output pointers */
        "    ldp x4, x5, [x0]\n"
        "    asr x6, x5, #63\n"                   /* sa */
        "    eor x4, x4, x6\n    eor x5, x5, x6\n    subs x4, x4, x6\n    sbc x5, x5, x6\n"
        "    stp x4, x5, [sp]\n"                  /* |a| */
        "    ldp x4, x5, [x1]\n"
        "    asr x7, x5, #63\n"                   /* sb */
        "    eor x4, x4, x7\n    eor x5, x5, x7\n    subs x4, x4, x7\n    sbc x5, x5, x7\n"
        "    stp x4, x5, [sp, #16]\n"             /* |b| */
        "    stp x6, x7, [sp, #80]\n"
        "    mov x0, sp\n    add x1, sp, #16\n    add x2, sp, #32\n    add x3, sp, #48\n"
        "    bl mvs_u128_divmod\n"
        "    ldp x6, x7, [sp, #80]\n"
        "    eor x8, x6, x7\n"                    /* quotient sign */
        "    ldp x4, x5, [sp, #32]\n"
        "    eor x4, x4, x8\n    eor x5, x5, x8\n    subs x4, x4, x8\n    sbc x5, x5, x8\n"
        "    ldp x2, x3, [sp, #64]\n"
        "    stp x4, x5, [x2]\n"
        "    ldp x4, x5, [sp, #48]\n"             /* remainder sign = sa */
        "    eor x4, x4, x6\n    eor x5, x5, x6\n    subs x4, x4, x6\n    sbc x5, x5, x6\n"
        "    stp x4, x5, [x3]\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n"
        "\nmvs_u128_str:\n"                       /* x0=&value -> x0 = decimal C string */
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #80\n"
        "    ldp x4, x5, [x0]\n    stp x4, x5, [sp]\n"
        "    mov x4, #10\n    stp x4, xzr, [sp, #16]\n"
        "    adrp x9, mvs_i128_bufidx\n    add x9, x9, :lo12:mvs_i128_bufidx\n"
        "    ldr w10, [x9]\n    add w10, w10, #1\n    and w10, w10, #3\n    str w10, [x9]\n"
        "    mov w11, #48\n    mul w10, w10, w11\n"
        "    adrp x11, mvs_i128_buf\n    add x11, x11, :lo12:mvs_i128_buf\n"
        "    add x11, x11, x10\n    add x11, x11, #47\n"
        "    strb wzr, [x11]\n"
        "    str x11, [sp, #64]\n"
        ".Lu128s_loop:\n"
        "    mov x0, sp\n    add x1, sp, #16\n    add x2, sp, #32\n    add x3, sp, #48\n"
        "    bl mvs_u128_divmod\n"
        "    ldr x11, [sp, #64]\n"
        "    ldr x4, [sp, #48]\n    add w4, w4, #48\n"
        "    sub x11, x11, #1\n    strb w4, [x11]\n    str x11, [sp, #64]\n"
        "    ldp x4, x5, [sp, #32]\n    stp x4, x5, [sp]\n"
        "    orr x4, x4, x5\n    cbnz x4, .Lu128s_loop\n"
        "    ldr x0, [sp, #64]\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n"
        "\nmvs_i128_str:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #16\n"
        "    ldr x4, [x0, #8]\n    tbnz x4, #63, .Li128s_neg\n"
        "    bl mvs_u128_str\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n"
        ".Li128s_neg:\n"
        "    ldp x4, x5, [x0]\n"
        "    mvn x4, x4\n    mvn x5, x5\n    adds x4, x4, #1\n    adc x5, x5, xzr\n"
        "    stp x4, x5, [sp]\n"
        "    mov x0, sp\n    bl mvs_u128_str\n"
        "    sub x0, x0, #1\n    mov w4, #45\n    strb w4, [x0]\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n",
        g->out);
}

/* Does any node mention a dyn Trait type? */
static int scan_uses_dyn(Node *n) {
    if (!n) return 0;
    if (n->type == TYPE_DYN) return 1;
    if (scan_uses_dyn(n->lhs) || scan_uses_dyn(n->rhs) || scan_uses_dyn(n->operand) ||
        scan_uses_dyn(n->cond) || scan_uses_dyn(n->then_branch) || scan_uses_dyn(n->else_branch) ||
        scan_uses_dyn(n->init) || scan_uses_dyn(n->step) || scan_uses_dyn(n->body) ||
        scan_uses_dyn(n->sig)) return 1;
    for (int i = 0; i < n->nitems; i++) if (scan_uses_dyn(n->items[i])) return 1;
    return 0;
}

/* ---------- entry point ---------- */

int arm64_linux_generate(Node *program, FILE *out) {
    Gen g;
    memset(&g, 0, sizeof(g));
    g.out = out;
    g.program = program;

    for (int i = 0; i < program->nitems; i++)
        if (program->items[i]->kind == ND_STRUCT_DECL && program->items[i]->ngen == 0)
            register_struct(&g, program->items[i]);
    layout_structs(&g);
    if (g.had_error) return 1;

    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind == ND_FUNC) {
            if (g.nfuncs >= MAX_FUNC) { fprintf(stderr, "codegen error: too many functions (max %d)\n", MAX_FUNC); g.had_error = 1; break; }
            g.funcs[g.nfuncs++] = d;
        } else if (d->kind == ND_VAR_DECL) {
            if (g.nglobals >= MAX_SYM) { fprintf(stderr, "codegen error: too many global variables (max %d)\n", MAX_SYM); g.had_error = 1; break; }
            Sym *s = &g.globals[g.nglobals++];
            s->name = d->name; s->type = d->type; s->ptr = d->ptr; s->sname = d->type_name;
            s->arr = d->arr;
            s->size = type_size(&g, d->type, d->ptr, d->type_name);
            if (s->arr > 0) s->size *= s->arr;
            s->is_global = 1; s->offset = 0;
        }
    }

    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->ns && strcmp(g.funcs[i]->ns, "io") == 0) { g.io_imported = 1; break; }

    char reached[MAX_FUNC]; memset(reached, 0, sizeof(reached));
    int has_main = 0;
    for (int i = 0; i < g.nfuncs; i++) {
        Node *f = g.funcs[i];
        if ((f->ns == NULL || f->ns[0] == 0) && strcmp(f->name, "main") == 0 && !f->is_extern) {
            has_main = 1; reach_func(&g, i, reached);
        }
        if (f->is_export) reach_func(&g, i, reached);
    }
    for (int i = 0; i < program->nitems; i++)
        if (program->items[i]->kind == ND_VAR_DECL && program->items[i]->operand)
            reach_node(&g, program->items[i]->operand, "", reached);

    int uses_dyn = scan_uses_dyn(program);
    if (uses_dyn) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind != ND_TRAIT_IMPL) continue;
            for (int t = 0; t < program->nitems; t++) {
                Node *tr = program->items[t];
                if (tr->kind != ND_TRAIT || !tr->name || strcmp(tr->name, d->name) != 0) continue;
                for (int m = 0; m < tr->nitems; m++) {
                    if (tr->items[m]->kind != ND_FUNC) continue;
                    Node *fn = find_func(&g, d->type_name, tr->items[m]->name);
                    if (fn) reach_func(&g, func_index(&g, fn), reached);
                }
                break;
            }
        }
    }

    fprintf(out, "// ===== MVS compiler output (AArch64 Linux, AAPCS64, GNU as syntax) =====\n");
    fprintf(out, "    .text\n");
    if (has_main) fprintf(out, "    .globl main\n");
    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->is_export) fprintf(out, "    .globl %s\n", g.funcs[i]->name);

    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind != ND_FUNC || d->is_extern) continue;
        if (d->ngen > 0) continue;
        if (!reached[func_index(&g, d)]) continue;
        gen_func(&g, d, program);
    }
    if (g.need_i128) emit_i128_helpers(&g);

    fprintf(out, "\n    .data\n");
    for (int i = 0; i < g.nstrs; i++) {
        fprintf(out, "mvs_str_%d:\n    .byte ", i);
        for (int j = 0; j < g.strs[i].len; j++) fprintf(out, "%d,", g.strs[i].data[j]);
        fprintf(out, "0\n");
    }
    if (uses_dyn) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind != ND_TRAIT_IMPL) continue;
            for (int t = 0; t < program->nitems; t++) {
                Node *tr = program->items[t];
                if (tr->kind != ND_TRAIT || !tr->name || strcmp(tr->name, d->name) != 0) continue;
                fprintf(out, "    .balign 8\nmvs_vt_%s_%s:\n", d->name, d->type_name);
                for (int m = 0; m < tr->nitems; m++) {
                    if (tr->items[m]->kind != ND_FUNC) continue;
                    Node *fn = find_func(&g, d->type_name, tr->items[m]->name);
                    if (fn) {
                        char mlbl[LABEL_MAX]; func_label_of(fn, mlbl);
                        fprintf(out, "    .xword %s\n", mlbl);
                    } else {
                        fprintf(out, "    .xword 0\n");
                    }
                }
                break;
            }
        }
    }

    fprintf(out, "\n    .bss\n    .balign 8\n");
    for (int i = 0; i < g.nglobals; i++) {
        char lbl[LABEL_MAX]; global_label(g.globals[i].name, lbl);
        int slot = (g.globals[i].size + 7) / 8 * 8;
        if (slot < 8) slot = 8;
        fprintf(out, "%s:\n    .skip %d\n", lbl, slot);
    }
    if (g.need_i128) {
        fprintf(out, "    .balign 8\nmvs_i128_buf:\n    .skip 192\nmvs_i128_bufidx:\n    .skip 4\n");
    }

    fprintf(out, "\n    .section .note.GNU-stack,\"\",@progbits\n");

    return g.had_error ? 1 : 0;
}
