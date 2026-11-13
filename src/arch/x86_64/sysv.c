/*
 * arch/x86_64/sysv.c - backend emitting NASM (Intel) assembly for x86-64 Linux/ELF (SysV ABI)
 *
 * This file holds only the architecture-dependent parts: instruction emission, registers,
 * and the calling convention (System V AMD64). Shared parts (type system, struct layout,
 * symbol table, reachability, format) live in arch/common.c
 *
 * SysV AMD64 ABI: integer/pointer arguments -> rdi, rsi, rdx, rcx, r8, r9 (6 registers) and
 *            float arguments -> xmm0..xmm7, each class counted separately; extras go on the
 *            stack; there is NO shadow space; variadic calls set AL = number of xmm registers
 *            used; rsp is 16-byte aligned at the call; rsi/rdi are volatile (caller-saved).
 * Output is assembled with `nasm -f elf64` and linked on a Linux system (e.g. gcc file.o).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysv.h"
#include "../common.h"
#include "../../ir.h"
#include "../../lower.h"

/* ---------- code emission helpers ---------- */


/* Copy size bytes of memory: src is in r10, dst is in r11.
 * Touches only rax/r10/r11. It must not touch any other argument register: the
 * prologue copies 16-byte parameters while LATER parameters are still sitting
 * in their registers, and on SysV rcx is argument 4, so a counter in rcx would
 * copy the next parameter from address zero. Small sizes unroll; a big struct
 * falls back to a loop with the counter saved and restored around it. */
static void gen_memcpy(Gen *g, int size) {
    if (size <= 128) {
        int off = 0;
        while (off + 8 <= size) { fprintf(g->out, "    mov rax, [r10 + %d]\n    mov [r11 + %d], rax\n", off, off); off += 8; }
        if (off + 4 <= size) { fprintf(g->out, "    mov eax, [r10 + %d]\n    mov [r11 + %d], eax\n", off, off); off += 4; }
        if (off + 2 <= size) { fprintf(g->out, "    mov ax, [r10 + %d]\n    mov [r11 + %d], ax\n", off, off); off += 2; }
        if (off < size)      { fprintf(g->out, "    mov al, [r10 + %d]\n    mov [r11 + %d], al\n", off, off); }
        return;
    }
    int l = new_label(g);
    fprintf(g->out,
        "    push rcx\n"
        "    mov ecx, %d\n"           /* counter (zero-extended into rcx) */
        ".Lcpy%d:\n"
        "    test rcx, rcx\n"
        "    jz .Lcpyend%d\n"
        "    mov al, [r10]\n"
        "    mov [r11], al\n"
        "    inc r10\n    inc r11\n    dec rcx\n"
        "    jmp .Lcpy%d\n"
        ".Lcpyend%d:\n"
        "    pop rcx\n", size, l, l, l, l);
}

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
/* Callee-saved registers a local may live in. SysV preserves these across a
 * call, so a promoted variable needs no spilling around calls and C code that
 * calls an exported MVS function still gets its registers back. */
static const char *REGVARS[] = { "rbx", "r12", "r13", "r14" };
#define NREGVARS ((int)(sizeof(REGVARS) / sizeof(REGVARS[0])))

/* park the borrowed callee-saved registers in the frame, and get them back */
static void gen_regsave(Gen *g) {
    for (int i = 0; i < g->plan.n && i < NREGVARS; i++)
        fprintf(g->out, "    mov [rbp - %d], %s\n", g->regsave_off - i * 8, REGVARS[i]);
}
static void gen_regrestore(Gen *g) {
    for (int i = 0; i < g->plan.n && i < NREGVARS; i++)
        fprintf(g->out, "    mov %s, [rbp - %d]\n", REGVARS[i], g->regsave_off - i * 8);
}
static void gen_epilogue(Gen *g) {
    gen_regrestore(g);
    fprintf(g->out, "    leave\n    ret\n");
}

/* Emit the 128-bit helper routines (once per output file, only when used):
 *   mvs_u128_divmod(rdi=&dividend, rsi=&divisor, rdx=&quotient, rcx=&remainder)
 *       classic shift-subtract long division, 128 iterations
 *   mvs_s128_divmod  signed wrapper (C truncation: rem sign = dividend sign)
 *   mvs_u128_str / mvs_i128_str(rdi=&value) -> rax = decimal C string
 *       written backwards into a 4-slot ring of static buffers (io.out uses %s) */
static void emit_i128_helpers(Gen *g) {
    fputs(
        "\nmvs_u128_divmod:\n"
        "    push rbp\n    mov rbp, rsp\n    sub rsp, 80\n"
        "    mov rax, [rdi]\n    mov [rbp - 16], rax\n"        /* dividend lo/hi */
        "    mov rax, [rdi + 8]\n    mov [rbp - 8], rax\n"
        "    mov rax, [rsi]\n    mov [rbp - 32], rax\n"        /* divisor lo/hi */
        "    mov rax, [rsi + 8]\n    mov [rbp - 24], rax\n"
        "    mov [rbp - 72], rdx\n    mov [rbp - 80], rcx\n"   /* output pointers */
        "    xor eax, eax\n"
        "    mov [rbp - 48], rax\n    mov [rbp - 40], rax\n"   /* remainder = 0 */
        "    mov [rbp - 64], rax\n    mov [rbp - 56], rax\n"   /* quotient = 0 */
        "    mov r10, 127\n"
        ".Ludm_loop:\n"
        "    mov rax, [rbp - 48]\n    mov rdx, [rbp - 40]\n"   /* rem <<= 1 */
        "    shld rdx, rax, 1\n    shl rax, 1\n"
        "    mov rcx, r10\n    cmp rcx, 64\n    jb .Ludm_lo\n" /* bit i of the dividend */
        "    mov r11, [rbp - 8]\n    sub rcx, 64\n    shr r11, cl\n    jmp .Ludm_got\n"
        ".Ludm_lo:\n    mov r11, [rbp - 16]\n    shr r11, cl\n"
        ".Ludm_got:\n    and r11, 1\n    or rax, r11\n"
        "    mov [rbp - 48], rax\n    mov [rbp - 40], rdx\n"
        "    mov rdx, [rbp - 40]\n    cmp rdx, [rbp - 24]\n"   /* rem >= divisor ? */
        "    ja .Ludm_ge\n    jb .Ludm_next\n"
        "    mov rax, [rbp - 48]\n    cmp rax, [rbp - 32]\n    jb .Ludm_next\n"
        ".Ludm_ge:\n"
        "    mov rax, [rbp - 48]\n    sub rax, [rbp - 32]\n    mov [rbp - 48], rax\n"
        "    mov rax, [rbp - 40]\n    sbb rax, [rbp - 24]\n    mov [rbp - 40], rax\n"
        "    mov rcx, r10\n    cmp rcx, 64\n    jb .Ludm_ql\n" /* quotient bit i = 1 */
        "    sub rcx, 64\n    mov rax, 1\n    shl rax, cl\n    or [rbp - 56], rax\n    jmp .Ludm_next\n"
        ".Ludm_ql:\n    mov rax, 1\n    shl rax, cl\n    or [rbp - 64], rax\n"
        ".Ludm_next:\n    dec r10\n    jns .Ludm_loop\n"
        "    mov r8, [rbp - 72]\n"
        "    mov rax, [rbp - 64]\n    mov [r8], rax\n"
        "    mov rax, [rbp - 56]\n    mov [r8 + 8], rax\n"
        "    mov r9, [rbp - 80]\n"
        "    mov rax, [rbp - 48]\n    mov [r9], rax\n"
        "    mov rax, [rbp - 40]\n    mov [r9 + 8], rax\n"
        "    leave\n    ret\n"
        "\nmvs_s128_divmod:\n"
        "    push rbp\n    mov rbp, rsp\n    sub rsp, 96\n"
        "    mov [rbp - 72], rdx\n    mov [rbp - 80], rcx\n"
        "    mov r11, rsi\n"
        "    mov rax, [rdi + 8]\n    mov r10, rax\n    sar r10, 63\n"  /* sa = sign mask of a */
        "    mov [rbp - 96], r10\n"
        "    mov rax, [rdi]\n    mov rdx, [rdi + 8]\n"                 /* |a| */
        "    xor rax, r10\n    xor rdx, r10\n    sub rax, r10\n    sbb rdx, r10\n"
        "    mov [rbp - 16], rax\n    mov [rbp - 8], rdx\n"
        "    mov rax, [r11 + 8]\n    mov rcx, rax\n    sar rcx, 63\n"  /* sb */
        "    mov [rbp - 88], rcx\n"
        "    mov rax, [r11]\n    mov rdx, [r11 + 8]\n"                 /* |b| */
        "    xor rax, rcx\n    xor rdx, rcx\n    sub rax, rcx\n    sbb rdx, rcx\n"
        "    mov [rbp - 32], rax\n    mov [rbp - 24], rdx\n"
        "    lea rdi, [rbp - 16]\n    lea rsi, [rbp - 32]\n"
        "    lea rdx, [rbp - 64]\n    lea rcx, [rbp - 48]\n"
        "    call mvs_u128_divmod\n"
        "    mov r10, [rbp - 96]\n    mov rcx, [rbp - 88]\n"
        "    mov r11, r10\n    xor r11, rcx\n"                         /* quotient sign = sa ^ sb */
        "    mov rax, [rbp - 64]\n    mov rdx, [rbp - 56]\n"
        "    xor rax, r11\n    xor rdx, r11\n    sub rax, r11\n    sbb rdx, r11\n"
        "    mov r8, [rbp - 72]\n    mov [r8], rax\n    mov [r8 + 8], rdx\n"
        "    mov rax, [rbp - 48]\n    mov rdx, [rbp - 40]\n"           /* remainder sign = sa */
        "    xor rax, r10\n    xor rdx, r10\n    sub rax, r10\n    sbb rdx, r10\n"
        "    mov r9, [rbp - 80]\n    mov [r9], rax\n    mov [r9 + 8], rdx\n"
        "    leave\n    ret\n"
        "\nmvs_u128_str:\n"
        "    push rbp\n    mov rbp, rsp\n    sub rsp, 80\n"
        "    mov rax, [rdi]\n    mov [rbp - 16], rax\n"
        "    mov rax, [rdi + 8]\n    mov [rbp - 8], rax\n"
        "    mov qword [rbp - 32], 10\n    mov qword [rbp - 24], 0\n"
        "    mov eax, [rel mvs_i128_bufidx]\n"                         /* next ring slot */
        "    inc eax\n    and eax, 3\n    mov [rel mvs_i128_bufidx], eax\n"
        "    imul eax, 48\n"
        "    lea r10, [rel mvs_i128_buf]\n    add r10, rax\n"
        "    add r10, 47\n    mov byte [r10], 0\n"                     /* build backwards */
        ".Lu128s_loop:\n"
        "    mov [rbp - 72], r10\n"
        "    lea rdi, [rbp - 16]\n    lea rsi, [rbp - 32]\n"
        "    lea rdx, [rbp - 48]\n    lea rcx, [rbp - 64]\n"
        "    call mvs_u128_divmod\n"
        "    mov r10, [rbp - 72]\n"
        "    mov rax, [rbp - 64]\n    add rax, 48\n"                   /* digit -> ASCII */
        "    dec r10\n    mov [r10], al\n"
        "    mov rax, [rbp - 48]\n    mov [rbp - 16], rax\n"           /* value = quotient */
        "    mov rax, [rbp - 40]\n    mov [rbp - 8], rax\n"
        "    mov rax, [rbp - 16]\n    or rax, [rbp - 8]\n    jnz .Lu128s_loop\n"
        "    mov rax, r10\n"
        "    leave\n    ret\n"
        "\nmvs_i128_str:\n"
        "    push rbp\n    mov rbp, rsp\n    sub rsp, 48\n"
        "    mov [rbp - 48], rdi\n"                                    /* save rdi across calls */
        "    mov rax, [rdi + 8]\n    test rax, rax\n    js .Li128s_neg\n"
        "    call mvs_u128_str\n"
        "    leave\n    ret\n"
        ".Li128s_neg:\n"
        "    mov rdi, [rbp - 48]\n"
        "    mov rax, [rdi]\n    mov rdx, [rdi + 8]\n"                 /* |v| then prepend '-' */
        "    not rax\n    not rdx\n    add rax, 1\n    adc rdx, 0\n"
        "    mov [rbp - 16], rax\n    mov [rbp - 8], rdx\n"
        "    lea rdi, [rbp - 16]\n"
        "    call mvs_u128_str\n"
        "    dec rax\n    mov byte [rax], 45\n"
        "    leave\n    ret\n",
        g->out);
}

/* Main entry point of the backend */
/* Does any node in the tree mention a dyn Trait type? Decides whether vtables are
 * needed and whether trait-impl methods must be kept by tree-shaking. */
/* ------------------------------------------------------------------ */
/*  IR emitter: translate IRFunc instructions to NASM (SysV ABI)      */
/* ------------------------------------------------------------------ */

#define VREG_OFF(fs, v) ((fs) + 8 + (v) * 8)
#define I128_TEMP(fs, nv, idx) (VREG_OFF(fs, nv) + (idx) * 16 + 8)
#define LOAD_A() do { if (inst->a != rv) fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a)); } while(0)

static void emit_ir_func(Gen *g, IRFunc *f) {
    Node *fn = f->fn;
    int fs = f->frame_size;
    int vreg_area = f->next_vreg * 8;
    int i128_area = f->n_i128_temps * 16;
    int total_frame = ((fs + vreg_area + i128_area) + 15) / 16 * 16;

    fprintf(g->out, "\n%s:\n", f->name);
    fprintf(g->out, "    push rbp\n    mov rbp, rsp\n");
    if (total_frame > 0) fprintf(g->out, "    sub rsp, %d\n", total_frame);
    gen_regsave(g);

    int returns_struct = ((fn->type == TYPE_STRUCT || is_blob16(fn->type, fn->ptr)) &&
                          fn->ptr == 0 && !fn->is_extern);
    static const char *argregs[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
    int png = 0, pnx = 0, pnstk = 0;
    if (returns_struct) { fprintf(g->out, "    mov [rbp - %d], rdi\n", g->sret_off); png++; }
    for (int i = 0; i < fn->nitems; i++) {
        Node *p = fn->items[i];
        Sym *psym = &g->locals[f->param_start + i];
        int is_float_param = is_float_type(p->type) && p->ptr == 0;
        if ((p->type == TYPE_STRUCT || p->type == TYPE_I128 || p->type == TYPE_U128 || p->type == TYPE_DYN) && p->ptr == 0) {
            int ssize = type_size(g, p->type, 0, p->type_name);
            if (png < 6) fprintf(g->out, "    mov r10, %s\n", argregs[png++]);
            else         fprintf(g->out, "    mov r10, [rbp + %d]\n", 16 + 8 * pnstk++);
            fprintf(g->out, "    lea r11, [rbp - %d]\n", psym->offset);
            gen_memcpy(g, ssize);
            continue;
        }
        if (is_float_param) {
            if (p->type == TYPE_F32 && p->ptr == 0) {
                if (pnx < 8) {
                    if (!fn->is_export)
                        fprintf(g->out, "    cvtsd2ss xmm%d, xmm%d\n", pnx, pnx);
                    fprintf(g->out, "    movd dword [rbp - %d], xmm%d\n", psym->offset, pnx++);
                } else {
                    if (!fn->is_export) {
                        fprintf(g->out, "    movq xmm0, [rbp + %d]\n", 16 + 8 * pnstk);
                        fprintf(g->out, "    cvtsd2ss xmm0, xmm0\n");
                        fprintf(g->out, "    movd eax, xmm0\n");
                    } else {
                        fprintf(g->out, "    mov eax, dword [rbp + %d]\n", 16 + 8 * pnstk);
                    }
                    pnstk++;
                    fprintf(g->out, "    mov dword [rbp - %d], eax\n", psym->offset);
                }
            } else {
                if (pnx < 8) {
                    fprintf(g->out, "    movq rax, xmm%d\n", pnx++);
                } else {
                    fprintf(g->out, "    mov rax, [rbp + %d]\n", 16 + 8 * pnstk++);
                }
                fprintf(g->out, "    mov [rbp - %d], rax\n", psym->offset);
            }
        } else {
            if (png < 6) fprintf(g->out, "    mov [rbp - %d], %s\n", psym->offset, argregs[png++]);
            else {
                fprintf(g->out, "    mov rax, [rbp + %d]\n", 16 + 8 * pnstk++);
                fprintf(g->out, "    mov [rbp - %d], rax\n", psym->offset);
            }
        }
    }

    int lbase = g->label_id;
    g->label_id += f->next_label;

    int rv = -1;
    for (int ip = 0; ip < f->ninsts; ip++) {
        IRInst *inst = &f->insts[ip];

        switch (inst->op) {
        case IR_NOP:
            if (inst->imm == 1) fprintf(g->out, "    ud2\n");
            break;

        case IR_IMM:
            if (inst->imm == 0)
                fprintf(g->out, "    xor eax, eax\n");
            else if (inst->imm > 0 && inst->imm <= 0x7FFFFFFF)
                fprintf(g->out, "    mov eax, %lld\n", inst->imm);
            else
                fprintf(g->out, "    mov rax, %lld\n", inst->imm);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_STR:
            fprintf(g->out, "    lea rax, [rel mvs_str_%lld]\n", inst->imm);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_FUNC_ADDR:
            fprintf(g->out, "    lea rax, [rel %s]\n", inst->name);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_LOCAL_ADDR:
            fprintf(g->out, "    lea rax, [rbp - %lld]\n", inst->imm);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_GLOBAL_ADDR: {
            char lbl[LABEL_MAX]; global_label(inst->name, lbl);
            fprintf(g->out, "    lea rax, [rel %s]\n", lbl);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_LOAD:
            LOAD_A();
            if (inst->type == TYPE_F32 && inst->ptr == 0)
                fprintf(g->out, "    mov eax, dword [rax]\n");
            else
                gen_load_at(g, inst->size, inst->is_signed);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_STORE:
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            LOAD_A();
            if (inst->type == TYPE_F32 && inst->ptr == 0)
                fprintf(g->out, "    movq xmm0, rcx\n    cvtsd2ss xmm0, xmm0\n    movd dword [rax], xmm0\n");
            else
                gen_store_at(g, inst->size);
            break;

        case IR_COPY:
            fprintf(g->out, "    mov r10, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    mov r11, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            gen_memcpy(g, (int)inst->imm);
            break;

        case IR_OFFSET:
            LOAD_A();
            if (inst->imm) fprintf(g->out, "    add rax, %lld\n", inst->imm);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_INDEX:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            if (inst->imm != 1) fprintf(g->out, "    imul rcx, %lld\n", inst->imm);
            fprintf(g->out, "    add rax, rcx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_BAND: case IR_BOR: case IR_BXOR: {
            const char *op = inst->op==IR_ADD ? "add" : inst->op==IR_SUB ? "sub" :
                             inst->op==IR_MUL ? "imul" : inst->op==IR_BAND ? "and" :
                             inst->op==IR_BOR ? "or" : "xor";
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    %s rax, rcx\n", op);
            if (mvs_overflow_checks && inst->is_signed &&
                (inst->op == IR_ADD || inst->op == IR_SUB || inst->op == IR_MUL)) {
                int l = new_label(g);
                if (inst->size >= 8)
                    fprintf(g->out, "    jno .Lok%d\n    ud2\n.Lok%d:\n", l, l);
                else if (inst->size == 4)
                    fprintf(g->out, "    movsxd rcx, eax\n    cmp rax, rcx\n    je .Lok%d\n    ud2\n.Lok%d:\n", l, l);
                else if (inst->size == 2)
                    fprintf(g->out, "    movsx rcx, ax\n    cmp rax, rcx\n    je .Lok%d\n    ud2\n.Lok%d:\n", l, l);
                else
                    fprintf(g->out, "    movsx rcx, al\n    cmp rax, rcx\n    je .Lok%d\n    ud2\n.Lok%d:\n", l, l);
            }
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }
        case IR_SDIV:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    cqo\n    idiv rcx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_UDIV:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    xor edx, edx\n    div rcx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_SMOD:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    cqo\n    idiv rcx\n    mov rax, rdx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_UMOD:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    xor edx, edx\n    div rcx\n    mov rax, rdx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_SHL:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    shl rax, cl\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_SAR:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    sar rax, cl\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_SHR:
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    shr rax, cl\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_NEG:
            LOAD_A();
            fprintf(g->out, "    neg rax\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_BNOT:
            LOAD_A();
            fprintf(g->out, "    not rax\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_CMP: {
            LOAD_A();
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    cmp rax, rcx\n");
            const char *cc;
            switch (inst->cmp) {
                case CMP_EQ:  cc="sete";  break; case CMP_NE:  cc="setne"; break;
                case CMP_SLT: cc="setl";  break; case CMP_SGT: cc="setg";  break;
                case CMP_SLE: cc="setle"; break; case CMP_SGE: cc="setge"; break;
                case CMP_ULT: cc="setb";  break; case CMP_UGT: cc="seta";  break;
                case CMP_ULE: cc="setbe"; break; case CMP_UGE: cc="setae"; break;
                default:      cc="sete";  break;
            }
            fprintf(g->out, "    %s al\n    movzx rax, al\n", cc);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            const char *op = inst->op==IR_FADD ? "addsd" : inst->op==IR_FSUB ? "subsd" :
                             inst->op==IR_FMUL ? "mulsd" : "divsd";
            LOAD_A();
            fprintf(g->out, "    movq xmm0, rax\n");
            fprintf(g->out, "    mov rax, [rbp - %d]\n    movq xmm1, rax\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    %s xmm0, xmm1\n    movq rax, xmm0\n", op);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }
        case IR_FNEG:
            LOAD_A();
            fprintf(g->out, "    mov rcx, 0x8000000000000000\n    xor rax, rcx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_FCMP: {
            LOAD_A();
            fprintf(g->out, "    movq xmm0, rax\n");
            fprintf(g->out, "    mov rax, [rbp - %d]\n    movq xmm1, rax\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    ucomisd xmm0, xmm1\n");
            switch (inst->cmp) {
                case CMP_FEQ:
                    fprintf(g->out, "    sete al\n    setnp cl\n    and al, cl\n    movzx rax, al\n"); break;
                case CMP_FNE:
                    fprintf(g->out, "    setne al\n    setp cl\n    or al, cl\n    movzx rax, al\n"); break;
                case CMP_FLT:
                    fprintf(g->out, "    setb al\n    setnp cl\n    and al, cl\n    movzx rax, al\n"); break;
                case CMP_FLE:
                    fprintf(g->out, "    setbe al\n    setnp cl\n    and al, cl\n    movzx rax, al\n"); break;
                default: {
                    const char *cc = inst->cmp == CMP_FGT ? "seta" : "setae";
                    fprintf(g->out, "    %s al\n    movzx rax, al\n", cc); break;
                }
            }
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_ITOF:
            LOAD_A();
            fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_UTOF: {
            int lok = new_label(g);
            LOAD_A();
            fprintf(g->out, "    test rax, rax\n    js .Lutof_big%d\n", lok);
            fprintf(g->out, "    cvtsi2sd xmm0, rax\n    jmp .Lutof_done%d\n", lok);
            fprintf(g->out, ".Lutof_big%d:\n", lok);
            fprintf(g->out, "    mov rcx, rax\n    shr rcx, 1\n    and eax, 1\n    or rax, rcx\n");
            fprintf(g->out, "    cvtsi2sd xmm0, rax\n    addsd xmm0, xmm0\n");
            fprintf(g->out, ".Lutof_done%d:\n", lok);
            fprintf(g->out, "    movq rax, xmm0\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }
        case IR_FTOI:
            LOAD_A();
            fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_SEXT:
            LOAD_A();
            switch (inst->size) {
                case 1: fprintf(g->out, "    movsx rax, al\n"); break;
                case 2: fprintf(g->out, "    movsx rax, ax\n"); break;
                case 4: fprintf(g->out, "    movsxd rax, eax\n"); break;
                default: break;
            }
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_ZEXT:
            LOAD_A();
            switch (inst->size) {
                case 1: fprintf(g->out, "    movzx rax, al\n"); break;
                case 2: fprintf(g->out, "    movzx rax, ax\n"); break;
                case 4: fprintf(g->out, "    mov eax, eax\n"); break;
                default: break;
            }
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_F32_F64:
            LOAD_A();
            fprintf(g->out, "    movd xmm0, eax\n    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_F64_F32:
            LOAD_A();
            fprintf(g->out, "    movq xmm0, rax\n    cvtsd2ss xmm0, xmm0\n    movd eax, xmm0\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_BOOL:
            LOAD_A();
            fprintf(g->out, "    test rax, rax\n    setne al\n    movzx rax, al\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        case IR_MOV:
            if (inst->a != IR_NONE) {
                LOAD_A();
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            }
            break;

        case IR_LABEL:
            fprintf(g->out, ".L%d:\n", lbase + inst->label);
            break;
        case IR_BR:
            LOAD_A();
            fprintf(g->out, "    test rax, rax\n");
            fprintf(g->out, "    jnz .L%d\n", lbase + inst->label);
            fprintf(g->out, "    jmp .L%d\n", lbase + inst->label2);
            break;
        case IR_JMP:
            fprintf(g->out, "    jmp .L%d\n", lbase + inst->label);
            break;
        case IR_RET:
            if (inst->a != IR_NONE) {
                LOAD_A();
                if (g->cur_ret_float) fprintf(g->out, "    movq xmm0, rax\n");
                if (g->cur_ret_f32c)  fprintf(g->out, "    cvtsd2ss xmm0, xmm0\n");
            }
            gen_epilogue(g);
            break;

        case IR_CALL:
        case IR_ICALL: {
            static const char *gpr[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
            int nargs = inst->nargs;
            int astart = inst->arg_start;
            int ng = 0, nx = 0, nstk = 0;
            int cls_gpr[80], cls_xmm[80], cls_stk[80];
            for (int ai = 0; ai < nargs && ai < 80; ai++) {
                IRArg *arg = &f->args[astart + ai];
                int is_fp = is_float_type(arg->type) && arg->ptr == 0;
                cls_gpr[ai] = cls_xmm[ai] = cls_stk[ai] = -1;
                if (is_fp) { if (nx < 8) cls_xmm[ai] = nx++; else cls_stk[ai] = nstk++; }
                else       { if (ng < 6) cls_gpr[ai] = ng++; else cls_stk[ai] = nstk++; }
            }
            int callspace = (nstk * 8 + 15) / 16 * 16;
            if (callspace) fprintf(g->out, "    sub rsp, %d\n", callspace);
            for (int ai = 0; ai < nargs && ai < 80; ai++) {
                IRArg *arg = &f->args[astart + ai];
                int av = arg->vreg;
                if (av == IR_NONE) continue;
                int is_fp = is_float_type(arg->type) && arg->ptr == 0;
                if (cls_xmm[ai] >= 0) {
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    movq xmm%d, rax\n",
                            VREG_OFF(fs, av), cls_xmm[ai]);
                } else if (cls_gpr[ai] >= 0) {
                    if (is_fp)
                        fprintf(g->out, "    mov rax, [rbp - %d]\n    movq xmm0, rax\n    mov %s, rax\n",
                                VREG_OFF(fs, av), gpr[cls_gpr[ai]]);
                    else
                        fprintf(g->out, "    mov %s, [rbp - %d]\n", gpr[cls_gpr[ai]], VREG_OFF(fs, av));
                } else {
                    fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, av));
                    fprintf(g->out, "    mov [rsp + %d], rax\n", cls_stk[ai] * 8);
                }
            }
            if (inst->sig && inst->sig->is_extern)
                fprintf(g->out, "    mov al, %d\n", nx);
            if (inst->op == IR_CALL)
                fprintf(g->out, "    call %s\n", inst->name);
            else {
                fprintf(g->out, "    mov rax, [rbp - %d]\n    call rax\n", VREG_OFF(fs, inst->a));
            }
            if (callspace) fprintf(g->out, "    add rsp, %d\n", callspace);
            if (inst->dst != IR_NONE) {
                if (inst->sig && is_float_type(inst->sig->type) && inst->sig->ptr == 0) {
                    if (inst->sig->type == TYPE_F32)
                        fprintf(g->out, "    cvtss2sd xmm0, xmm0\n");
                    fprintf(g->out, "    movq rax, xmm0\n");
                }
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
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
            switch (inst->atomic_op) {
            case ATOMIC_LOAD:
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    mov rax, [rax]\n");
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
                break;
            case ATOMIC_STORE:
                fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    xchg [rax], rcx\n");
                fprintf(g->out, "    xor eax, eax\n");
                break;
            case ATOMIC_ADD:
                fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    lock xadd [rax], rcx\n");
                fprintf(g->out, "    mov rax, rcx\n");
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
                break;
            case ATOMIC_SUB:
                fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    neg rcx\n");
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    lock xadd [rax], rcx\n");
                fprintf(g->out, "    mov rax, rcx\n");
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
                break;
            case ATOMIC_CAS:
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->label));
                fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    mov rdx, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    lock cmpxchg [rdx], rcx\n");
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
                break;
            case ATOMIC_XCHG:
                fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
                fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
                fprintf(g->out, "    xchg [rax], rcx\n");
                fprintf(g->out, "    mov rax, rcx\n");
                fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
                break;
            }
            break;
        }

        case IR_LINE:
            if (inst->line > 0)
                fprintf(g->out, "%%line %d+0 %s\n",
                        inst->line, inst->file ? inst->file : "unknown.mvs");
            break;

        case IR_TRUNC:
            LOAD_A();
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_FPOW:
            fprintf(stderr, "emit_ir error: IR_FPOW should have been lowered\n");
            g->had_error = 1;
            break;

        case IR_I128_BINOP: {
            int nv = f->next_vreg;
            int ao = I128_TEMP(fs, nv, inst->imm);
            int bo = I128_TEMP(fs, nv, inst->imm + 1);
            int ro = I128_TEMP(fs, nv, inst->imm + 2);
            int uns = !inst->is_signed;
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov r10, rax\n");
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    mov r11, rax\n");
            fprintf(g->out, "    mov rcx, [r10]\n    mov rdx, [r10 + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rcx\n    mov [rbp - %d], rdx\n", ao, ao - 8);
            fprintf(g->out, "    mov rcx, [r11]\n    mov rdx, [r11 + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rcx\n    mov [rbp - %d], rdx\n", bo, bo - 8);
            switch (inst->i128_op) {
                case I128_ADD:
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    mov rdx, [rbp - %d]\n", ao, ao - 8);
                    fprintf(g->out, "    add rax, [rbp - %d]\n    adc rdx, [rbp - %d]\n", bo, bo - 8);
                    fprintf(g->out, "    mov [rbp - %d], rax\n    mov [rbp - %d], rdx\n", ro, ro - 8);
                    break;
                case I128_SUB:
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    mov rdx, [rbp - %d]\n", ao, ao - 8);
                    fprintf(g->out, "    sub rax, [rbp - %d]\n    sbb rdx, [rbp - %d]\n", bo, bo - 8);
                    fprintf(g->out, "    mov [rbp - %d], rax\n    mov [rbp - %d], rdx\n", ro, ro - 8);
                    break;
                case I128_AND: case I128_OR: case I128_XOR: {
                    const char *op = inst->i128_op == I128_AND ? "and" :
                                     inst->i128_op == I128_OR  ? "or"  : "xor";
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    %s rax, [rbp - %d]\n    mov [rbp - %d], rax\n", ao, op, bo, ro);
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    %s rax, [rbp - %d]\n    mov [rbp - %d], rax\n", ao - 8, op, bo - 8, ro - 8);
                    break;
                }
                case I128_SHL: case I128_SHR: {
                    int l = new_label(g);
                    fprintf(g->out, "    mov rcx, [rbp - %d]\n    and rcx, 127\n", bo);
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    mov rdx, [rbp - %d]\n", ao, ao - 8);
                    if (inst->i128_op == I128_SHL) {
                        fprintf(g->out,
                            "    cmp cl, 64\n    jb .Lsh%d\n"
                            "    mov rdx, rax\n    xor eax, eax\n    sub cl, 64\n    shl rdx, cl\n    jmp .Lshd%d\n"
                            ".Lsh%d:\n    shld rdx, rax, cl\n    shl rax, cl\n"
                            ".Lshd%d:\n", l, l, l, l);
                    } else if (uns) {
                        fprintf(g->out,
                            "    cmp cl, 64\n    jb .Lsh%d\n"
                            "    mov rax, rdx\n    xor edx, edx\n    sub cl, 64\n    shr rax, cl\n    jmp .Lshd%d\n"
                            ".Lsh%d:\n    shrd rax, rdx, cl\n    shr rdx, cl\n"
                            ".Lshd%d:\n", l, l, l, l);
                    } else {
                        fprintf(g->out,
                            "    cmp cl, 64\n    jb .Lsh%d\n"
                            "    mov rax, rdx\n    sar rdx, 63\n    sub cl, 64\n    sar rax, cl\n    jmp .Lshd%d\n"
                            ".Lsh%d:\n    shrd rax, rdx, cl\n    sar rdx, cl\n"
                            ".Lshd%d:\n", l, l, l, l);
                    }
                    fprintf(g->out, "    mov [rbp - %d], rax\n    mov [rbp - %d], rdx\n", ro, ro - 8);
                    break;
                }
                case I128_MUL:
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    mul qword [rbp - %d]\n", ao, bo);
                    fprintf(g->out, "    mov [rbp - %d], rax\n    mov rcx, rdx\n", ro);
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    imul rax, [rbp - %d]\n    add rcx, rax\n", ao, bo - 8);
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    imul rax, [rbp - %d]\n    add rcx, rax\n", ao - 8, bo);
                    fprintf(g->out, "    mov [rbp - %d], rcx\n", ro - 8);
                    break;
                case I128_SDIV: case I128_UDIV: case I128_SMOD: case I128_UMOD: {
                    g->need_i128 = 1;
                    int div_uns = (inst->i128_op == I128_UDIV || inst->i128_op == I128_UMOD);
                    const char *fn = div_uns ? "mvs_u128_divmod" : "mvs_s128_divmod";
                    int is_mod = (inst->i128_op == I128_SMOD || inst->i128_op == I128_UMOD);
                    int qoff = is_mod ? ao : ro;
                    int roff = is_mod ? ro : ao;
                    fprintf(g->out, "    lea rdi, [rbp - %d]\n    lea rsi, [rbp - %d]\n", ao, bo);
                    fprintf(g->out, "    lea rdx, [rbp - %d]\n    lea rcx, [rbp - %d]\n", qoff, roff);
                    fprintf(g->out, "    call %s\n", fn);
                    break;
                }
                default: break;
            }
            fprintf(g->out, "    lea rax, [rbp - %d]\n", ro);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_I128_UNOP: {
            int nv = f->next_vreg;
            int ao = I128_TEMP(fs, nv, inst->imm);
            int ro = I128_TEMP(fs, nv, inst->imm + 1);
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov rcx, [rax]\n    mov rdx, [rax + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rcx\n    mov [rbp - %d], rdx\n", ao, ao - 8);
            fprintf(g->out, "    mov rax, [rbp - %d]\n    mov rdx, [rbp - %d]\n", ao, ao - 8);
            if (inst->i128_op == I128_NEG)
                fprintf(g->out, "    not rax\n    not rdx\n    add rax, 1\n    adc rdx, 0\n");
            else
                fprintf(g->out, "    not rax\n    not rdx\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n    mov [rbp - %d], rdx\n", ro, ro - 8);
            fprintf(g->out, "    lea rax, [rbp - %d]\n", ro);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_I128_CMP: {
            int nv = f->next_vreg;
            int ao = I128_TEMP(fs, nv, inst->imm);
            int bo = I128_TEMP(fs, nv, inst->imm + 1);
            int uns = !inst->is_signed;
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov r10, rax\n");
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            fprintf(g->out, "    mov r11, rax\n");
            fprintf(g->out, "    mov rcx, [r10]\n    mov rdx, [r10 + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rcx\n    mov [rbp - %d], rdx\n", ao, ao - 8);
            fprintf(g->out, "    mov rcx, [r11]\n    mov rdx, [r11 + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rcx\n    mov [rbp - %d], rdx\n", bo, bo - 8);
            switch (inst->cmp) {
                case CMP_EQ: case CMP_NE:
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    xor rax, [rbp - %d]\n", ao, bo);
                    fprintf(g->out, "    mov rcx, [rbp - %d]\n    xor rcx, [rbp - %d]\n    or rax, rcx\n", ao - 8, bo - 8);
                    fprintf(g->out, inst->cmp == CMP_EQ ?
                        "    cmp rax, 0\n    sete al\n    movzx rax, al\n" :
                        "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                    break;
                default: {
                    int l = new_label(g);
                    const char *hi_cc, *lo_cc;
                    switch (inst->cmp) {
                        case CMP_SLT: case CMP_ULT: hi_cc = uns ? "setb" : "setl"; lo_cc = "setb";  break;
                        case CMP_SGT: case CMP_UGT: hi_cc = uns ? "seta" : "setg"; lo_cc = "seta";  break;
                        case CMP_SLE: case CMP_ULE: hi_cc = uns ? "setb" : "setl"; lo_cc = "setbe"; break;
                        case CMP_SGE: case CMP_UGE: hi_cc = uns ? "seta" : "setg"; lo_cc = "setae"; break;
                        default: hi_cc = "setl"; lo_cc = "setb"; break;
                    }
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    cmp rax, [rbp - %d]\n    jne .Li128c%d\n", ao - 8, bo - 8, l);
                    fprintf(g->out, "    mov rax, [rbp - %d]\n    cmp rax, [rbp - %d]\n    %s al\n    jmp .Li128d%d\n", ao, bo, lo_cc, l);
                    fprintf(g->out, ".Li128c%d:\n    %s al\n", l, hi_cc);
                    fprintf(g->out, ".Li128d%d:\n    movzx rax, al\n", l);
                    break;
                }
            }
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_I128_FROM: {
            int nv = f->next_vreg;
            int to = I128_TEMP(fs, nv, inst->imm);
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov [rbp - %d], rax\n", to);
            if (inst->is_signed)
                fprintf(g->out, "    mov rcx, rax\n    sar rcx, 63\n    mov [rbp - %d], rcx\n", to - 8);
            else
                fprintf(g->out, "    mov qword [rbp - %d], 0\n", to - 8);
            fprintf(g->out, "    lea rax, [rbp - %d]\n", to);
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;
        }

        case IR_I128_STORE: {
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            if (is_i128(inst->type, inst->ptr)) {
                fprintf(g->out, "    mov rdx, [rcx]\n    mov [rax], rdx\n");
                fprintf(g->out, "    mov rdx, [rcx + 8]\n    mov [rax + 8], rdx\n");
            } else if (!inst->is_signed) {
                fprintf(g->out, "    mov [rax], rcx\n    mov qword [rax + 8], 0\n");
            } else {
                fprintf(g->out, "    mov [rax], rcx\n    mov rdx, rcx\n    sar rdx, 63\n    mov [rax + 8], rdx\n");
            }
            break;
        }

        case IR_I128_LOAD:
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            if (inst->imm == 0)
                fprintf(g->out, "    mov rax, [rax]\n");
            else
                fprintf(g->out, "    mov rax, [rax + 8]\n");
            fprintf(g->out, "    mov [rbp - %d], rax\n", VREG_OFF(fs, inst->dst));
            break;

        case IR_DYN_STORE: {
            fprintf(g->out, "    mov rax, [rbp - %d]\n", VREG_OFF(fs, inst->a));
            fprintf(g->out, "    mov rcx, [rbp - %d]\n", VREG_OFF(fs, inst->b));
            if (is_dyn(inst->type, inst->ptr)) {
                fprintf(g->out, "    mov rdx, [rcx]\n    mov [rax], rdx\n");
                fprintf(g->out, "    mov rdx, [rcx + 8]\n    mov [rax + 8], rdx\n");
            } else if (inst->ptr == 1 && inst->name && inst->sname) {
                g->need_vtables = 1;
                fprintf(g->out, "    mov [rax], rcx\n");
                fprintf(g->out, "    lea rdx, [rel mvs_vt_%s_%s]\n    mov [rax + 8], rdx\n",
                        inst->name, inst->sname);
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

    fprintf(g->out, "    xor eax, eax\n");
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

int x86_64_sysv_generate(Node *program, FILE *out) {
    /* static, not a local: Gen holds the symbol/struct/string tables and is far
     * too large for the stack */
    static Gen g;
    memset(&g, 0, sizeof(g));
    g.out = out;
    g.program = program;   /* traits are looked up here for dyn dispatch */

    /* pass 1: register every struct, then compute the layouts to a fixpoint
     * (two steps so a struct can have fields whose struct type is declared later) */
    for (int i = 0; i < program->nitems; i++)
        if (program->items[i]->kind == ND_STRUCT_DECL && program->items[i]->ngen == 0)
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
            s->arr = d->arr;
            s->arr2 = d->arr2;
            s->sig = d->sig;   /* a global holding a function pointer keeps its signature */
            s->size = type_size(&g, d->type, d->ptr, d->type_name);
            if (s->arr2 > 0) s->size *= s->arr2;
            if (s->arr > 0) s->size *= s->arr;
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

    /* dyn Trait: when trait objects appear anywhere, every `impl Trait for Type` method can be
     * reached through a vtable, so keep them all and emit the vtables later */
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

    /* assembly file header */
    fprintf(out, "; ===== MVS compiler output (x86-64 Linux/ELF, SysV ABI, NASM syntax) =====\n");
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
        {
            IRFunc *irf = lower_func(&g, d, program, NREGVARS);
            emit_ir_func(&g, irf);
            ir_func_free(irf);
        }
    }
    if (g.need_i128) emit_i128_helpers(&g);   /* 128-bit divmod + decimal print routines */

    /* read-only data section: all string constants (including format strings from std/io.mvs) */
    fprintf(out, "\nsection .data\n");
    for (int i = 0; i < g.nstrs; i++) {
        fprintf(out, "mvs_str_%d: db ", i);
        for (int j = 0; j < g.strs[i].len; j++) fprintf(out, "%d,", g.strs[i].data[j]);
        fprintf(out, "0\n"); /* terminate with 0 to make it a C-style string */
    }

    /* vtables: one per `impl Trait for Type`, one qword per trait method in declaration order */
    if (uses_dyn) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind != ND_TRAIT_IMPL) continue;
            for (int t = 0; t < program->nitems; t++) {
                Node *tr = program->items[t];
                if (tr->kind != ND_TRAIT || !tr->name || strcmp(tr->name, d->name) != 0) continue;
                fprintf(out, "mvs_vt_%s_%s:\n", d->name, d->type_name);
                for (int m = 0; m < tr->nitems; m++) {
                    if (tr->items[m]->kind != ND_FUNC) continue;
                    Node *fn = find_func(&g, d->type_name, tr->items[m]->name);
                    if (fn) {
                        char mlbl[LABEL_MAX]; func_label_of(fn, mlbl);
                        fprintf(out, "    dq %s\n", mlbl);
                    } else {
                        fprintf(out, "    dq 0\n");   /* missing impl: already a type error upstream */
                    }
                }
                break;
            }
        }
    }

    /* global variable section (reserve the actual size rounded up to a multiple of 8, zero-initialized) */
    fprintf(out, "\nsection .bss\n");
    for (int i = 0; i < g.nglobals; i++) {
        char lbl[LABEL_MAX]; global_label(g.globals[i].name, lbl);
        int slot = (g.globals[i].size + 7) / 8 * 8;
        if (slot < 8) slot = 8;
        fprintf(out, "%s: resb %d\n", lbl, slot);
    }
    if (g.need_i128) {
        /* ring of 4 conversion buffers + rotation index for mvs_u128_str/mvs_i128_str */
        fprintf(out, "mvs_i128_buf: resb 192\n");
        fprintf(out, "mvs_i128_bufidx: resd 1\n");
    }

    /* mark the stack non-executable (silences the GNU ld warning about missing .note.GNU-stack) */
    fprintf(out, "\nsection .note.GNU-stack noalloc noexec nowrite progbits\n");

    return g.had_error ? 1 : 0;
}
