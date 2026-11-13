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
#include "../../ir.h"
#include "../../lower.h"

/* ---------- code emission helpers ---------- */

/* Materialize [x29 - off] into reg (offsets can exceed the immediate range) */
static void addr_local(Gen *g, const char *reg, int off) {
    if (off <= 4095) fprintf(g->out, "    sub %s, x29, #%d\n", reg, off);
    else             fprintf(g->out, "    mov %s, #%d\n    sub %s, x29, %s\n", reg, off, reg, reg);
}

/* Materialize the address of a global/label into reg */
static void addr_label(Gen *g, const char *reg, const char *lbl) {
    fprintf(g->out, "    adrp %s, %s\n    add %s, %s, :lo12:%s\n", reg, lbl, reg, reg, lbl);
}

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

static const char *REGVARS[] = { "x19", "x20", "x21", "x22" };
#define NREGVARS ((int)(sizeof(REGVARS) / sizeof(REGVARS[0])))

static void gen_regsave(Gen *g) {
    for (int i = 0; i < g->plan.n && i < NREGVARS; i++) {
        addr_local(g, "x9", g->regsave_off - i * 8);
        fprintf(g->out, "    str %s, [x9]\n", REGVARS[i]);
    }
}
static void gen_regrestore(Gen *g) {
    for (int i = 0; i < g->plan.n && i < NREGVARS; i++) {
        addr_local(g, "x9", g->regsave_off - i * 8);
        fprintf(g->out, "    ldr %s, [x9]\n", REGVARS[i]);
    }
}
static void gen_epilogue(Gen *g) {
    gen_regrestore(g);
    fprintf(g->out, "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");
}

/* Copy x11 bytes from [x9] to [x10] (byte loop; only scratch registers) */
static void gen_memcpy(Gen *g, int size) {
    int l = new_label(g);
    fprintf(g->out, "    mov x11, #%d\n", size);
    fprintf(g->out, ".Lmc%d:\n    cbz x11, .Lmce%d\n", l, l);
    fprintf(g->out, "    ldrb w12, [x9], #1\n    strb w12, [x10], #1\n");
    fprintf(g->out, "    sub x11, x11, #1\n    b .Lmc%d\n.Lmce%d:\n", l, l);
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
/* ------------------------------------------------------------------ */
/*  IR emitter: translate IRFunc instructions to GNU as (AAPCS64)     */
/* ------------------------------------------------------------------ */

#define VREG_OFF(fs, v) ((fs) + 8 + (v) * 8)
#define I128_TEMP(fs, nv, idx) (VREG_OFF(fs, nv) + (idx) * 16 + 8)
#define LOAD_A() do { if (inst->a != rv) { addr_local(g, "x9", VREG_OFF(fs, inst->a)); fprintf(g->out, "    ldr x0, [x9]\n"); } } while(0)

static void emit_ir_func(Gen *g, IRFunc *f) {
    Node *fn = f->fn;
    int fs = f->frame_size;
    int vreg_area = f->next_vreg * 8;
    int i128_area = f->n_i128_temps * 16;
    int total_frame = ((fs + vreg_area + i128_area) + 15) / 16 * 16;

    fprintf(g->out, "\n%s:\n", f->name);
    fprintf(g->out, "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n");
    if (total_frame > 0) {
        if (total_frame <= 4095) fprintf(g->out, "    sub sp, sp, #%d\n", total_frame);
        else fprintf(g->out, "    mov x9, #%d\n    sub sp, sp, x9\n", total_frame);
    }
    gen_regsave(g);

    int returns_struct = ((fn->type == TYPE_STRUCT || is_blob16(fn->type, fn->ptr)) &&
                          fn->ptr == 0 && !fn->is_extern);
    int png = 0, pnf = 0, pnstk = 0;
    if (returns_struct) {
        addr_local(g, "x9", g->sret_off);
        fprintf(g->out, "    str x8, [x9]\n");
    }
    for (int i = 0; i < fn->nitems; i++) {
        Node *p = fn->items[i];
        Sym *psym = &g->locals[f->param_start + i];
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
            if (fn->is_export && p->type == TYPE_F32 && p->ptr == 0) {
                if (pnf < 8) {
                    addr_local(g, "x9", psym->offset);
                    fprintf(g->out, "    str s%d, [x9]\n", pnf++);
                } else {
                    fprintf(g->out, "    ldr w0, [x29, #%d]\n", 16 + 8 * pnstk++);
                    addr_local(g, "x9", psym->offset);
                    fprintf(g->out, "    str w0, [x9]\n");
                }
            } else {
                if (pnf < 8) {
                    fprintf(g->out, "    fmov x0, d%d\n", pnf++);
                } else {
                    fprintf(g->out, "    ldr x0, [x29, #%d]\n", 16 + 8 * pnstk++);
                }
                addr_local(g, "x9", psym->offset);
                fprintf(g->out, "    str x0, [x9]\n");
            }
        } else {
            if (png < 8) fprintf(g->out, "    mov x9, x%d\n", png++);
            else         fprintf(g->out, "    ldr x9, [x29, #%d]\n", 16 + 8 * pnstk++);
            addr_local(g, "x10", psym->offset);
            fprintf(g->out, "    str x9, [x10]\n");
        }
    }

    int lbase = g->label_id;
    g->label_id += f->next_label;

    int rv = -1;
    for (int ip = 0; ip < f->ninsts; ip++) {
        IRInst *inst = &f->insts[ip];

        switch (inst->op) {
        case IR_NOP:
            if (inst->imm == 1) fprintf(g->out, "    brk #1\n");
            break;

        case IR_IMM:
            if (inst->imm == 0)
                fprintf(g->out, "    mov x0, xzr\n");
            else
                fprintf(g->out, "    ldr x0, =%lld\n", inst->imm);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_STR:
            fprintf(g->out, "    adrp x0, mvs_str_%lld\n    add x0, x0, :lo12:mvs_str_%lld\n",
                    inst->imm, inst->imm);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_FUNC_ADDR:
            fprintf(g->out, "    adrp x0, %s\n    add x0, x0, :lo12:%s\n", inst->name, inst->name);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_LOCAL_ADDR:
            addr_local(g, "x0", (int)inst->imm);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_GLOBAL_ADDR: {
            char lbl[LABEL_MAX]; global_label(inst->name, lbl);
            addr_label(g, "x0", lbl);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_LOAD:
            LOAD_A();
            if (inst->type == TYPE_F32 && inst->ptr == 0)
                fprintf(g->out, "    ldr w0, [x0]\n");
            else
                gen_load_at(g, inst->size, inst->is_signed);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_STORE:
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            LOAD_A();
            if (inst->type == TYPE_F32 && inst->ptr == 0)
                fprintf(g->out, "    fmov d0, x1\n    fcvt s0, d0\n    str s0, [x0]\n");
            else
                gen_store_at(g, inst->size);
            break;

        case IR_COPY:
            addr_local(g, "x14", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x9, [x14]\n");
            addr_local(g, "x14", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x10, [x14]\n");
            gen_memcpy(g, (int)inst->imm);
            break;

        case IR_OFFSET:
            LOAD_A();
            if (inst->imm) {
                if (inst->imm > 0 && inst->imm <= 4095)
                    fprintf(g->out, "    add x0, x0, #%lld\n", inst->imm);
                else
                    fprintf(g->out, "    ldr x9, =%lld\n    add x0, x0, x9\n", inst->imm);
            }
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_INDEX:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            if (inst->imm != 1)
                fprintf(g->out, "    mov x9, #%lld\n    mul x1, x1, x9\n", inst->imm);
            fprintf(g->out, "    add x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_BAND: case IR_BOR: case IR_BXOR: {
            int ov = mvs_overflow_checks && inst->is_signed &&
                     (inst->op == IR_ADD || inst->op == IR_SUB || inst->op == IR_MUL);
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            if (ov && inst->op == IR_MUL && inst->size >= 8) {
                int l = new_label(g);
                fprintf(g->out,
                    "    smulh x9, x0, x1\n    mul x0, x0, x1\n"
                    "    cmp x9, x0, asr #63\n    b.eq .Lok%d\n    brk #1\n.Lok%d:\n", l, l);
            } else {
                const char *op;
                if (ov && inst->size >= 8 && inst->op == IR_ADD)      op = "adds";
                else if (ov && inst->size >= 8 && inst->op == IR_SUB) op = "subs";
                else if (inst->op == IR_ADD)  op = "add";
                else if (inst->op == IR_SUB)  op = "sub";
                else if (inst->op == IR_MUL)  op = "mul";
                else if (inst->op == IR_BAND) op = "and";
                else if (inst->op == IR_BOR)  op = "orr";
                else                          op = "eor";
                fprintf(g->out, "    %s x0, x0, x1\n", op);
                if (ov && inst->size >= 8) {
                    int l = new_label(g);
                    fprintf(g->out, "    b.vc .Lok%d\n    brk #1\n.Lok%d:\n", l, l);
                }
            }
            if (ov && inst->size < 8) {
                int l = new_label(g);
                const char *ext = inst->size == 4 ? "sxtw" : inst->size == 2 ? "sxth" : "sxtb";
                fprintf(g->out, "    %s x9, w0\n    cmp x0, x9\n    b.eq .Lok%d\n    brk #1\n.Lok%d:\n", ext, l, l);
            }
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }
        case IR_SDIV:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    sdiv x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_UDIV:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    udiv x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_SMOD:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    sdiv x2, x0, x1\n    msub x0, x2, x1, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_UMOD:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    udiv x2, x0, x1\n    msub x0, x2, x1, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_SHL:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    lsl x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_SAR:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    asr x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_SHR:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    lsr x0, x0, x1\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_NEG:
            LOAD_A();
            fprintf(g->out, "    neg x0, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_BNOT:
            LOAD_A();
            fprintf(g->out, "    mvn x0, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_CMP: {
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            fprintf(g->out, "    cmp x0, x1\n");
            const char *cc;
            switch (inst->cmp) {
                case CMP_EQ:  cc="eq"; break; case CMP_NE:  cc="ne"; break;
                case CMP_SLT: cc="lt"; break; case CMP_SGT: cc="gt"; break;
                case CMP_SLE: cc="le"; break; case CMP_SGE: cc="ge"; break;
                case CMP_ULT: cc="lo"; break; case CMP_UGT: cc="hi"; break;
                case CMP_ULE: cc="ls"; break; case CMP_UGE: cc="hs"; break;
                default:      cc="eq"; break;
            }
            fprintf(g->out, "    cset x0, %s\n", cc);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            const char *op = inst->op==IR_FADD ? "fadd" : inst->op==IR_FSUB ? "fsub" :
                             inst->op==IR_FMUL ? "fmul" : "fdiv";
            LOAD_A();
            fprintf(g->out, "    fmov d0, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n    fmov d1, x1\n");
            fprintf(g->out, "    %s d0, d0, d1\n    fmov x0, d0\n", op);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }
        case IR_FNEG:
            LOAD_A();
            fprintf(g->out, "    fmov d0, x0\n");
            fprintf(g->out, "    fneg d0, d0\n    fmov x0, d0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_FCMP: {
            LOAD_A();
            fprintf(g->out, "    fmov d0, x0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n    fmov d1, x1\n");
            fprintf(g->out, "    fcmp d0, d1\n");
            const char *cc;
            switch (inst->cmp) {
                case CMP_FEQ: cc="eq"; break; case CMP_FNE: cc="ne"; break;
                case CMP_FLT: cc="mi"; break; case CMP_FGT: cc="gt"; break;
                case CMP_FLE: cc="ls"; break; case CMP_FGE: cc="ge"; break;
                default:      cc="eq"; break;
            }
            fprintf(g->out, "    cset x0, %s\n", cc);
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_ITOF:
            LOAD_A();
            fprintf(g->out, "    scvtf d0, x0\n    fmov x0, d0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_UTOF:
            LOAD_A();
            fprintf(g->out, "    ucvtf d0, x0\n    fmov x0, d0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_FTOI:
            LOAD_A();
            fprintf(g->out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_SEXT:
            LOAD_A();
            switch (inst->size) {
                case 1: fprintf(g->out, "    sxtb x0, w0\n"); break;
                case 2: fprintf(g->out, "    sxth x0, w0\n"); break;
                case 4: fprintf(g->out, "    sxtw x0, w0\n"); break;
                default: break;
            }
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_ZEXT:
            LOAD_A();
            switch (inst->size) {
                case 1: fprintf(g->out, "    and x0, x0, #0xff\n"); break;
                case 2: fprintf(g->out, "    and x0, x0, #0xffff\n"); break;
                case 4: fprintf(g->out, "    mov w0, w0\n"); break;
                default: break;
            }
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_F32_F64:
            LOAD_A();
            fprintf(g->out, "    fmov s0, w0\n    fcvt d0, s0\n    fmov x0, d0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_F64_F32:
            LOAD_A();
            fprintf(g->out, "    fmov d0, x0\n    fcvt s0, d0\n    fmov w0, s0\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_BOOL:
            LOAD_A();
            fprintf(g->out, "    cmp x0, xzr\n    cset x0, ne\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        case IR_MOV:
            if (inst->a != IR_NONE) {
                LOAD_A();
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
            }
            break;

        case IR_LABEL:
            fprintf(g->out, ".L%d:\n", lbase + inst->label);
            break;
        case IR_BR:
            LOAD_A();
            fprintf(g->out, "    cbnz x0, .L%d\n", lbase + inst->label);
            fprintf(g->out, "    b .L%d\n", lbase + inst->label2);
            break;
        case IR_JMP:
            fprintf(g->out, "    b .L%d\n", lbase + inst->label);
            break;
        case IR_RET:
            if (inst->a != IR_NONE) {
                LOAD_A();
                if (g->cur_ret_float) fprintf(g->out, "    fmov d0, x0\n");
                if (g->cur_ret_f32c)  fprintf(g->out, "    fcvt s0, d0\n");
            }
            gen_epilogue(g);
            break;

        case IR_CALL:
        case IR_ICALL: {
            int nargs = inst->nargs;
            int astart = inst->arg_start;
            int rsc = inst->sig &&
                ((inst->sig->type == TYPE_STRUCT || is_blob16(inst->sig->type, inst->sig->ptr)) &&
                 inst->sig->ptr == 0);
            int ng = 0, nfp = 0, nstk = 0;
            int cls_gpr[80], cls_fp[80], cls_stk[80];
            for (int ai = 0; ai < nargs && ai < 80; ai++) {
                cls_gpr[ai] = cls_fp[ai] = cls_stk[ai] = -1;
                if (rsc && ai == 0) continue;
                IRArg *arg = &f->args[astart + ai];
                int is_fp = is_float_type(arg->type) && arg->ptr == 0;
                if (is_fp) { if (nfp < 8) cls_fp[ai] = nfp++; else cls_stk[ai] = nstk++; }
                else       { if (ng < 8) cls_gpr[ai] = ng++;   else cls_stk[ai] = nstk++; }
            }
            int callspace = (nstk * 8 + 15) / 16 * 16;
            if (callspace) {
                if (callspace <= 4095) fprintf(g->out, "    sub sp, sp, #%d\n", callspace);
                else fprintf(g->out, "    mov x9, #%d\n    sub sp, sp, x9\n", callspace);
            }
            for (int ai = 0; ai < nargs && ai < 80; ai++) {
                IRArg *arg = &f->args[astart + ai];
                int av = arg->vreg;
                if (av == IR_NONE) continue;
                addr_local(g, "x14", VREG_OFF(fs, av));
                fprintf(g->out, "    ldr x15, [x14]\n");
                if (rsc && ai == 0) {
                    fprintf(g->out, "    mov x8, x15\n");
                } else if (cls_fp[ai] >= 0) {
                    fprintf(g->out, "    fmov d%d, x15\n", cls_fp[ai]);
                } else if (cls_gpr[ai] >= 0) {
                    fprintf(g->out, "    mov x%d, x15\n", cls_gpr[ai]);
                } else {
                    fprintf(g->out, "    str x15, [sp, #%d]\n", cls_stk[ai] * 8);
                }
            }
            if (inst->op == IR_CALL)
                fprintf(g->out, "    bl %s\n", inst->name);
            else {
                addr_local(g, "x14", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    ldr x9, [x14]\n    blr x9\n");
            }
            if (callspace) {
                if (callspace <= 4095) fprintf(g->out, "    add sp, sp, #%d\n", callspace);
                else fprintf(g->out, "    mov x9, #%d\n    add sp, sp, x9\n", callspace);
            }
            if (inst->dst != IR_NONE) {
                int c_abi = inst->sig && (inst->sig->is_extern || inst->sig->is_export);
                if (inst->sig && is_float_type(inst->sig->type) && inst->sig->ptr == 0) {
                    if (c_abi && inst->sig->type == TYPE_F32)
                        fprintf(g->out, "    fcvt d0, s0\n");
                    fprintf(g->out, "    fmov x0, d0\n");
                }
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
            }
            break;
        }

        case IR_ASM:
            if (inst->node) {
                for (int ai = 0; ai < inst->node->nitems; ai++) {
                    if (inst->node->items[ai]->kind == ND_STR)
                        fprintf(g->out, "    %s\n", inst->node->items[ai]->str_val);
                }
            }
            break;

        case IR_ATOMIC: {
            int lbl_retry = g->label_id++;
            int lbl_done  = g->label_id++;
            switch (inst->atomic_op) {
            case ATOMIC_LOAD:
                LOAD_A();
                fprintf(g->out, "    ldar x0, [x0]\n");
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
                break;
            case ATOMIC_STORE:
                addr_local(g, "x9", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    ldr x2, [x9]\n");
                LOAD_A();
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, ".L%d:\n", lbl_retry);
                fprintf(g->out, "    ldaxr x3, [x1]\n");
                fprintf(g->out, "    stlxr w4, x2, [x1]\n");
                fprintf(g->out, "    cbnz w4, .L%d\n", lbl_retry);
                fprintf(g->out, "    mov x0, #0\n");
                break;
            case ATOMIC_ADD:
                addr_local(g, "x9", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    ldr x2, [x9]\n");
                LOAD_A();
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, ".L%d:\n", lbl_retry);
                fprintf(g->out, "    ldaxr x0, [x1]\n");
                fprintf(g->out, "    add x3, x0, x2\n");
                fprintf(g->out, "    stlxr w4, x3, [x1]\n");
                fprintf(g->out, "    cbnz w4, .L%d\n", lbl_retry);
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
                break;
            case ATOMIC_SUB:
                addr_local(g, "x9", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    ldr x2, [x9]\n");
                LOAD_A();
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, ".L%d:\n", lbl_retry);
                fprintf(g->out, "    ldaxr x0, [x1]\n");
                fprintf(g->out, "    sub x3, x0, x2\n");
                fprintf(g->out, "    stlxr w4, x3, [x1]\n");
                fprintf(g->out, "    cbnz w4, .L%d\n", lbl_retry);
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
                break;
            case ATOMIC_CAS:
                addr_local(g, "x9", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    ldr x2, [x9]\n");
                addr_local(g, "x9", VREG_OFF(fs, inst->label));
                fprintf(g->out, "    ldr x5, [x9]\n");
                LOAD_A();
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, ".L%d:\n", lbl_retry);
                fprintf(g->out, "    ldaxr x0, [x1]\n");
                fprintf(g->out, "    cmp x0, x5\n");
                fprintf(g->out, "    b.ne .L%d\n", lbl_done);
                fprintf(g->out, "    stlxr w4, x2, [x1]\n");
                fprintf(g->out, "    cbnz w4, .L%d\n", lbl_retry);
                fprintf(g->out, ".L%d:\n", lbl_done);
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
                break;
            case ATOMIC_XCHG:
                addr_local(g, "x9", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    ldr x2, [x9]\n");
                LOAD_A();
                fprintf(g->out, "    mov x1, x0\n");
                fprintf(g->out, ".L%d:\n", lbl_retry);
                fprintf(g->out, "    ldaxr x0, [x1]\n");
                fprintf(g->out, "    stlxr w4, x2, [x1]\n");
                fprintf(g->out, "    cbnz w4, .L%d\n", lbl_retry);
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
                break;
            }
            break;
        }

        case IR_LINE:
            if (inst->line > 0 && inst->file) {
                static const char *arm64_last_file = NULL;
                static int arm64_file_idx = 0;
                if (inst->file != arm64_last_file) {
                    arm64_file_idx++;
                    fprintf(g->out, "    .file %d \"%s\"\n", arm64_file_idx, inst->file);
                    arm64_last_file = inst->file;
                }
                fprintf(g->out, "    .loc %d %d 0\n", arm64_file_idx, inst->line);
            }
            break;

        case IR_TRUNC:
            LOAD_A();
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;

        case IR_FPOW:
            fprintf(stderr, "emit_ir error: IR_FPOW should be lowered before emission\n");
            g->had_error = 1;
            break;

        case IR_I128_BINOP: {
            int nv = f->next_vreg;
            int ao = I128_TEMP(fs, nv, inst->imm);
            int bo = I128_TEMP(fs, nv, inst->imm + 1);
            int ro = I128_TEMP(fs, nv, inst->imm + 2);
            int uns = !inst->is_signed;
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x10, [x9]\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x11, [x9]\n");
            fprintf(g->out, "    ldp x2, x3, [x10]\n");
            addr_local(g, "x9", ao);
            fprintf(g->out, "    stp x2, x3, [x9]\n");
            fprintf(g->out, "    ldp x4, x5, [x11]\n");
            addr_local(g, "x9", bo);
            fprintf(g->out, "    stp x4, x5, [x9]\n");
            switch (inst->i128_op) {
                case I128_ADD:
                    fprintf(g->out, "    adds x2, x2, x4\n    adc x3, x3, x5\n");
                    break;
                case I128_SUB:
                    fprintf(g->out, "    subs x2, x2, x4\n    sbc x3, x3, x5\n");
                    break;
                case I128_AND:
                    fprintf(g->out, "    and x2, x2, x4\n    and x3, x3, x5\n");
                    break;
                case I128_OR:
                    fprintf(g->out, "    orr x2, x2, x4\n    orr x3, x3, x5\n");
                    break;
                case I128_XOR:
                    fprintf(g->out, "    eor x2, x2, x4\n    eor x3, x3, x5\n");
                    break;
                case I128_MUL:
                    fprintf(g->out, "    umulh x6, x2, x4\n");
                    fprintf(g->out, "    madd x6, x2, x5, x6\n");
                    fprintf(g->out, "    madd x6, x3, x4, x6\n");
                    fprintf(g->out, "    mul x2, x2, x4\n    mov x3, x6\n");
                    break;
                case I128_SHL: {
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
                case I128_SHR: {
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
                case I128_SDIV: case I128_UDIV: case I128_SMOD: case I128_UMOD: {
                    g->need_i128 = 1;
                    int div_uns = (inst->i128_op == I128_UDIV || inst->i128_op == I128_UMOD);
                    const char *fn = div_uns ? "mvs_u128_divmod" : "mvs_s128_divmod";
                    int is_mod = (inst->i128_op == I128_SMOD || inst->i128_op == I128_UMOD);
                    int qoff = is_mod ? ao : ro;
                    int roff = is_mod ? ro : ao;
                    addr_local(g, "x0", ao);
                    addr_local(g, "x1", bo);
                    addr_local(g, "x2", qoff);
                    addr_local(g, "x3", roff);
                    fprintf(g->out, "    bl %s\n", fn);
                    addr_local(g, "x0", ro);
                    addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                    fprintf(g->out, "    str x0, [x9]\n");
                    break;
                }
                default: break;
            }
            if (inst->i128_op != I128_SDIV && inst->i128_op != I128_UDIV &&
                inst->i128_op != I128_SMOD && inst->i128_op != I128_UMOD) {
                addr_local(g, "x9", ro);
                fprintf(g->out, "    stp x2, x3, [x9]\n");
                fprintf(g->out, "    mov x0, x9\n");
                addr_local(g, "x9", VREG_OFF(fs, inst->dst));
                fprintf(g->out, "    str x0, [x9]\n");
            }
            break;
        }

        case IR_I128_UNOP: {
            int nv = f->next_vreg;
            int ao = I128_TEMP(fs, nv, inst->imm);
            int ro = I128_TEMP(fs, nv, inst->imm + 1);
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x10, [x9]\n");
            fprintf(g->out, "    ldp x2, x3, [x10]\n");
            addr_local(g, "x9", ao);
            fprintf(g->out, "    stp x2, x3, [x9]\n");
            if (inst->i128_op == I128_NEG)
                fprintf(g->out, "    mvn x2, x2\n    mvn x3, x3\n    adds x2, x2, #1\n    adc x3, x3, xzr\n");
            else
                fprintf(g->out, "    mvn x2, x2\n    mvn x3, x3\n");
            addr_local(g, "x9", ro);
            fprintf(g->out, "    stp x2, x3, [x9]\n    mov x0, x9\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_I128_CMP: {
            int uns = !inst->is_signed;
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x10, [x9]\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x11, [x9]\n");
            fprintf(g->out, "    ldp x2, x3, [x10]\n");
            fprintf(g->out, "    ldp x4, x5, [x11]\n");
            switch (inst->cmp) {
                case CMP_EQ: case CMP_NE:
                    fprintf(g->out, "    eor x2, x2, x4\n    eor x3, x3, x5\n    orr x2, x2, x3\n");
                    fprintf(g->out, "    cmp x2, #0\n    cset x0, %s\n", inst->cmp == CMP_EQ ? "eq" : "ne");
                    break;
                default: {
                    int l = new_label(g);
                    const char *hi_cc, *lo_cc;
                    switch (inst->cmp) {
                        case CMP_SLT: case CMP_ULT: hi_cc = uns ? "lo" : "lt"; lo_cc = "lo"; break;
                        case CMP_SGT: case CMP_UGT: hi_cc = uns ? "hi" : "gt"; lo_cc = "hi"; break;
                        case CMP_SLE: case CMP_ULE: hi_cc = uns ? "lo" : "lt"; lo_cc = "ls"; break;
                        case CMP_SGE: case CMP_UGE: hi_cc = uns ? "hi" : "gt"; lo_cc = "hs"; break;
                        default: hi_cc = "lt"; lo_cc = "lo"; break;
                    }
                    fprintf(g->out, "    cmp x3, x5\n    b.ne .Lic%d\n", l);
                    fprintf(g->out, "    cmp x2, x4\n    cset x0, %s\n    b .Lid%d\n", lo_cc, l);
                    fprintf(g->out, ".Lic%d:\n    cset x0, %s\n.Lid%d:\n", l, hi_cc, l);
                    break;
                }
            }
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_I128_FROM: {
            int nv = f->next_vreg;
            int to = I128_TEMP(fs, nv, inst->imm);
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x2, [x9]\n");
            addr_local(g, "x9", to);
            if (inst->is_signed)
                fprintf(g->out, "    asr x3, x2, #63\n    stp x2, x3, [x9]\n");
            else
                fprintf(g->out, "    stp x2, xzr, [x9]\n");
            fprintf(g->out, "    mov x0, x9\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_I128_STORE: {
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x0, [x9]\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            if (is_i128(inst->type, inst->ptr)) {
                fprintf(g->out, "    ldp x2, x3, [x1]\n    stp x2, x3, [x0]\n");
            } else if (!inst->is_signed) {
                fprintf(g->out, "    stp x1, xzr, [x0]\n");
            } else {
                fprintf(g->out, "    asr x2, x1, #63\n    stp x1, x2, [x0]\n");
            }
            break;
        }

        case IR_I128_LOAD: {
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x10, [x9]\n");
            if (inst->imm == 0)
                fprintf(g->out, "    ldr x0, [x10]\n");
            else
                fprintf(g->out, "    ldr x0, [x10, #8]\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->dst));
            fprintf(g->out, "    str x0, [x9]\n");
            break;
        }

        case IR_DYN_STORE: {
            addr_local(g, "x9", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    ldr x0, [x9]\n");
            addr_local(g, "x9", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ldr x1, [x9]\n");
            if (is_dyn(inst->type, inst->ptr)) {
                fprintf(g->out, "    ldp x2, x3, [x1]\n    stp x2, x3, [x0]\n");
            } else if (inst->ptr == 1 && inst->name && inst->sname) {
                g->need_vtables = 1;
                fprintf(g->out, "    str x1, [x0]\n");
                char vt_label[256];
                snprintf(vt_label, sizeof(vt_label), "mvs_vt_%s_%s", inst->name, inst->sname);
                addr_label(g, "x2", vt_label);
                fprintf(g->out, "    str x2, [x0, #8]\n");
            } else {
                fprintf(stderr, "emit_ir error: IR_DYN_STORE with unexpected source type\n");
                g->had_error = 1;
            }
            break;
        }

        default:
            break;
        }

        switch (inst->op) {
        case IR_LABEL: case IR_BR: case IR_JMP: case IR_RET:
        case IR_ASM: case IR_ATOMIC:
            rv = -1; break;
        case IR_CALL: case IR_ICALL:
            rv = (inst->dst != IR_NONE) ? inst->dst : -1; break;
        case IR_STORE: case IR_I128_STORE: case IR_DYN_STORE:
            rv = inst->a; break;
        case IR_COPY:
        case IR_I128_BINOP: case IR_I128_UNOP: case IR_I128_CMP:
        case IR_I128_FROM: case IR_I128_LOAD:
            rv = -1; break;
        case IR_NOP: case IR_LINE:
            break;
        default:
            if (inst->dst != IR_NONE) rv = inst->dst;
            break;
        }
    }

    fprintf(g->out, "    mov x0, xzr\n");
    gen_epilogue(g);
}

#undef LOAD_A
#undef VREG_OFF

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
    /* static, not a local: Gen holds the symbol/struct/string tables and is far
     * too large for the stack */
    static Gen g;
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
            s->arr2 = d->arr2;
            s->sig = d->sig;   /* a global holding a function pointer keeps its signature */
            s->size = type_size(&g, d->type, d->ptr, d->type_name);
            if (s->arr2 > 0) s->size *= s->arr2;
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
        {
            IRFunc *irf = lower_func(&g, d, program, NREGVARS);
            if (irf && !g.had_error) emit_ir_func(&g, irf);
            if (irf) ir_func_free(irf);
        }
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

    emit_dwarf_gas(&g, program->file ? program->file : "unknown.mvs");

    fprintf(out, "\n    .section .note.GNU-stack,\"\",@progbits\n");

    return g.had_error ? 1 : 0;
}
