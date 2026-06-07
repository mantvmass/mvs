/*
 * arch/x86_64/win.c - backend ปล่อยแอสเซมบลี NASM (Intel) สำหรับ x86-64 Windows (win64 ABI)
 *
 * ไฟล์นี้เก็บเฉพาะส่วนที่ "ขึ้นกับสถาปัตยกรรม": การปล่อย instruction, รีจิสเตอร์,
 * และ calling convention (win64). ส่วนกลาง (ระบบชนิด, struct layout, symbol table,
 * reachability, format) อยู่ใน arch/common.c
 *
 * win64 ABI: อาร์กิวเมนต์จำนวนเต็ม 4 ตัวแรก -> rcx, rdx, r8, r9; ที่เกินวางบน stack;
 *            จอง shadow space 32 ไบต์ก่อน call; rsp จัดเรียง 16 ไบต์ ณ จุด call
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "../common.h"

/* ---------- ตัวช่วยปล่อยโค้ด ---------- */

/* ดันค่าใน rax ลง stack ชั่วคราว ใช้พื้นที่ 16 ไบต์เพื่อรักษาการจัดเรียง 16-byte */
static void push_tmp(Gen *g) {
    fprintf(g->out, "    sub rsp, 16\n    mov [rsp], rax\n");
}
/* ดึงค่าจาก stack ชั่วคราวกลับมาที่รีจิสเตอร์ reg */
static void pop_tmp(Gen *g, const char *reg) {
    fprintf(g->out, "    mov %s, [rsp]\n    add rsp, 16\n", reg);
}

/* คัดลอกหน่วยความจำ size ไบต์: src อยู่ใน r10, dst อยู่ใน r11
 * ใช้เฉพาะรีจิสเตอร์ volatile (r10/r11/rcx/rax) — ไม่แตะ rsi/rdi ซึ่ง win64 ถือเป็น callee-saved
 * และไม่ชนกับ argument register (rcx/rdx/r8/r9) ที่อาจยังถือพารามิเตอร์ที่ยังไม่ถูกบันทึก */
static void gen_memcpy(Gen *g, int size) {
    int l = new_label(g);
    fprintf(g->out,
        "    mov ecx, %d\n"           /* ตัวนับ (zero-extend เป็น rcx) */
        ".Lcpy%d:\n"
        "    test rcx, rcx\n"
        "    jz .Lcpyend%d\n"
        "    mov al, [r10]\n"
        "    mov [r11], al\n"
        "    inc r10\n    inc r11\n    dec rcx\n"
        "    jmp .Lcpy%d\n"
        ".Lcpyend%d:\n", size, l, l, l, l);
}

/* ประกาศล่วงหน้า */
static void gen_expr(Gen *g, Node *n);
static void gen_stmt(Gen *g, Node *n);
static void gen_addr(Gen *g, Node *n);
static void gen_store_struct(Gen *g, Node *value);
static void gen_call(Gen *g, Node *n, int has_sret);

/* โหลดค่าจาก [rax] (rax = ที่อยู่) ตามขนาด/เครื่องหมาย เก็บผลใน rax */
static void gen_load_at(Gen *g, int size, int is_signed) {
    switch (size) {
        case 1: fprintf(g->out, is_signed ? "    movsx rax, byte [rax]\n"  : "    movzx rax, byte [rax]\n"); break;
        case 2: fprintf(g->out, is_signed ? "    movsx rax, word [rax]\n"  : "    movzx rax, word [rax]\n"); break;
        case 4: fprintf(g->out, is_signed ? "    movsxd rax, dword [rax]\n" : "    mov eax, dword [rax]\n"); break;
        default: fprintf(g->out, "    mov rax, [rax]\n"); break;
    }
}

/* เก็บค่าใน rcx ลง [rax] (rax = ที่อยู่) ตามขนาด (ใช้ sub-register ของ rcx) */
static void gen_store_at(Gen *g, int size) {
    switch (size) {
        case 1: fprintf(g->out, "    mov [rax], cl\n"); break;
        case 2: fprintf(g->out, "    mov [rax], cx\n"); break;
        case 4: fprintf(g->out, "    mov [rax], ecx\n"); break;
        default: fprintf(g->out, "    mov [rax], rcx\n"); break;
    }
}

/* โหลดค่าจาก [rax] ตามชนิด เก็บผลใน rax (ภายในใช้ double เสมอ)
 *   - f32: อ่าน single 4 ไบต์ แล้วแปลงเป็น double  -> bit-pattern ใน rax
 *   - f64: อ่าน 8 ไบต์ตรง ๆ (เป็น double bits อยู่แล้ว)
 *   - จำนวนเต็ม: ตามขนาด/เครื่องหมาย (movsx/movzx) */
static void gen_load_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    movd xmm0, dword [rax]\n    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n");
    } else {
        gen_load_at(g, size, is_signed_type(base));
    }
}

/* เก็บค่าใน rcx (double bits สำหรับ float) ลง [rax] ตามชนิด
 *   - f32: แปลง double -> single แล้วเขียน 4 ไบต์
 *   - f64/จำนวนเต็ม: เขียนตามขนาด */
static void gen_store_typed(Gen *g, DataType base, int size) {
    if (base == TYPE_F32) {
        fprintf(g->out, "    movq xmm0, rcx\n    cvtsd2ss xmm0, xmm0\n    movd dword [rax], xmm0\n");
    } else {
        gen_store_at(g, size);
    }
}

/* โหลดค่าของตัวแปรเข้าสู่ rax (struct value -> ได้ที่อยู่, scalar -> ได้ค่าตามขนาด/ชนิด) */
static void gen_load_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    lea rax, [rel %s]\n", lbl); }
    else              { fprintf(g->out, "    lea rax, [rbp - %d]\n", s->offset); }
    if (s->type == TYPE_STRUCT && s->ptr == 0) return; /* struct: คงที่อยู่ไว้ใน rax */
    gen_load_typed(g, s->ptr > 0 ? TYPE_USIZE : s->type, s->size); /* pointer = โหลด 8 ไบต์ */
}

/* เก็บค่าจาก rax ลงตัวแปร (truncate ตามขนาด/แปลง f32 ตามชนิด) */
static void gen_store_var(Gen *g, Sym *s) {
    char lbl[LABEL_MAX];
    if (s->ptr == 0 && s->type == TYPE_F32) {
        /* แปลง double ใน rax -> single แล้วเขียน 4 ไบต์ */
        fprintf(g->out, "    movq xmm0, rax\n    cvtsd2ss xmm0, xmm0\n");
        if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    movd dword [rel %s], xmm0\n", lbl); }
        else              { fprintf(g->out, "    movd dword [rbp - %d], xmm0\n", s->offset); }
        return;
    }
    const char *reg = s->size == 1 ? "al" : s->size == 2 ? "ax" : s->size == 4 ? "eax" : "rax";
    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    mov [rel %s], %s\n", lbl, reg); }
    else              { fprintf(g->out, "    mov [rbp - %d], %s\n", s->offset, reg); }
}

/* แปลงค่าใน rax โดยปริยายเมื่อข้ามเส้น int<->float (เช่น let x: f64 = 5; ส่ง int ให้พารามิเตอร์ f64;
 * return int จากฟังก์ชันที่คืน float) — ให้สอดคล้องกับการ promote ในนิพจน์ผสม (ไม่ปล่อยเป็นขยะ) */
static void gen_coerce_num(Gen *g, ExprType from, ExprType to) {
    if (from.ptr > 0 || to.ptr > 0) return;
    int ff = is_float_type(from.base), tf = is_float_type(to.base);
    if (ff == tf) return;
    if (!ff && tf) fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n");  /* int -> double */
    else           fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* double -> int */
}

/* ใส่ "ที่อยู่" (address) ของ lvalue ลงใน rax (ใช้กับ &x, การกำหนดค่า, member access) */
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
        case ND_FRAMEREF:   /* ที่อยู่ของช่อง temp ที่จองไว้ (ใช้กับ io.out ที่ materialize struct ครั้งเดียว) */
            fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val);
            break;
        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *s = find_struct(g, bt.sname);
            if (!s) { fprintf(stderr, "codegen error: member access on non-struct\n"); g->had_error = 1; return; }
            Field *f = find_field(s, n->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", n->name, s->name); g->had_error = 1; return; }
            if (bt.ptr > 0) gen_expr(g, n->operand);  /* base เป็น pointer-to-struct: ใช้ค่าเป็นที่อยู่ */
            else            gen_addr(g, n->operand);  /* base เป็น struct value: ใช้ที่อยู่ของมัน */
            if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
            break;
        }
        case ND_UNARY:
            if (n->op == TK_STAR) { gen_expr(g, n->operand); break; } /* *p: ค่าของ pointer คือที่อยู่ */
            fprintf(stderr, "codegen error: expression is not an lvalue\n");
            g->had_error = 1;
            break;
        case ND_CALL: {
            /* ที่อยู่ของผลลัพธ์ struct จากฟังก์ชัน: materialize ลงช่อง temp ก่อน (เช่น make().field) */
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

/* ค่านี้ถือเป็น unsigned หรือไม่ (ใช้เลือก div/setcc แบบ unsigned)
 * — pointer = ที่อยู่ (unsigned), ชนิดจำนวนเต็มไม่มีเครื่องหมาย/bool/char = unsigned */
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

/* gen การดำเนินการเลขคณิต/เปรียบเทียบ: lhs อยู่ใน rax, rhs อยู่ใน rcx
 * uns = 1 ใช้คำสั่งแบบ unsigned (div, setb/seta) สำหรับชนิดไม่มีเครื่องหมาย */
static void gen_binop_apply(Gen *g, TokenType op, int uns) {
    switch (op) {
        case TK_PLUS:    fprintf(g->out, "    add rax, rcx\n"); break;
        case TK_MINUS:   fprintf(g->out, "    sub rax, rcx\n"); break;
        case TK_STAR:    fprintf(g->out, "    imul rax, rcx\n"); break;  /* บิตล่างเหมือนกันทั้ง signed/unsigned */
        /* ตัวดำเนินการระดับบิต */
        case TK_AMP:     fprintf(g->out, "    and rax, rcx\n"); break;   /* bitwise AND */
        case TK_PIPE:    fprintf(g->out, "    or rax, rcx\n"); break;    /* bitwise OR */
        case TK_CARET:   fprintf(g->out, "    xor rax, rcx\n"); break;   /* bitwise XOR */
        case TK_SHL:     fprintf(g->out, "    shl rax, cl\n"); break;    /* เลื่อนซ้าย (จำนวนใน cl) */
        case TK_SHR:     fprintf(g->out, uns ? "    shr rax, cl\n" : "    sar rax, cl\n"); break; /* ขวา: logical/arith */
        case TK_SLASH:   /* ผลหารใน rax */
            if (uns) fprintf(g->out, "    xor edx, edx\n    div rcx\n");      /* unsigned: ขยายศูนย์ */
            else     fprintf(g->out, "    cqo\n    idiv rcx\n");             /* signed: ขยายเครื่องหมาย */
            break;
        case TK_PERCENT: /* เศษใน rdx */
            if (uns) fprintf(g->out, "    xor edx, edx\n    div rcx\n    mov rax, rdx\n");
            else     fprintf(g->out, "    cqo\n    idiv rcx\n    mov rax, rdx\n");
            break;
        case TK_STARSTAR: { /* ยกกำลังด้วยลูปคูณซ้ำ: rax=ฐาน, rcx=เลขชี้กำลัง */
            int l = new_label(g);
            fprintf(g->out,
                "    mov r8, rax\n"      /* r8 = ฐาน */
                "    mov r9, rcx\n"      /* r9 = ตัวนับเลขชี้กำลัง */
                "    mov rax, 1\n"       /* ผลลัพธ์เริ่มที่ 1 */
                ".Lpow%d:\n"
                "    test r9, r9\n"
                "    jle .Lpowend%d\n"
                "    imul rax, r8\n"
                "    dec r9\n"
                "    jmp .Lpow%d\n"
                ".Lpowend%d:\n", l, l, l, l);
            break;
        }
        /* ตัวเปรียบเทียบ: ตั้งค่า 0/1 ลง rax (เลือก signed/unsigned setcc) */
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


/* gen นิพจน์ ผลลัพธ์อยู่ใน rax เสมอ */
static void gen_expr(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_INT:
        case ND_FLOAT:   /* ค่า double เก็บเป็น bit-pattern ใน int_val */
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
            /* ไม่ใช่ตัวแปร: ถ้าเป็น "ชื่อฟังก์ชัน" ให้ใช้เป็นค่า function pointer (ที่อยู่ของ label) */
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
            /* ตรรกะ && || ใช้การลัดวงจร (short-circuit) */
            if (n->op == TK_AND || n->op == TK_OR) {
                int lend = new_label(g);
                gen_expr(g, n->lhs);
                fprintf(g->out, "    cmp rax, 0\n");
                if (n->op == TK_AND) fprintf(g->out, "    je .Llog%d\n", lend);   /* ซ้ายเท็จ → ผลเท็จ */
                else                 fprintf(g->out, "    jne .Llog%d\n", lend);  /* ซ้ายจริง → ผลจริง */
                gen_expr(g, n->rhs);
                fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                fprintf(g->out, ".Llog%d:\n", lend);
                if (n->op == TK_AND) {
                    /* ถ้ามาถึง label เพราะซ้ายเท็จ rax ยังเป็น 0 อยู่ จึงถูกต้อง */
                } else {
                    /* สำหรับ || หากกระโดดมาเพราะซ้ายจริง ต้องบังคับให้เป็น 1 */
                    fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                }
                break;
            }
            ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);

            /* การดำเนินการแบบ floating point (ใช้รีจิสเตอร์ xmm)
             * รองรับ + - * / และการเปรียบเทียบ; ถ้าฝั่งใดเป็น int จะแปลงเป็น double ก่อน */
            {
                int fop = ((is_float_type(lt.base) && lt.ptr == 0) || (is_float_type(rt.base) && rt.ptr == 0));
                int is_cmp = (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||n->op==TK_LE||n->op==TK_GE);
                int is_arith = (n->op==TK_PLUS||n->op==TK_MINUS||n->op==TK_STAR||n->op==TK_SLASH);
                /* ยกกำลังที่ฐานเป็นทศนิยม: คูณ double ซ้ำตามเลขชี้กำลัง (จำนวนเต็ม) ในรีจิสเตอร์ xmm */
                if (fop && n->op == TK_STARSTAR) {
                    gen_expr(g, n->lhs);
                    push_tmp(g);
                    gen_expr(g, n->rhs);                       /* เลขชี้กำลัง -> rcx (จำนวนเต็ม) */
                    if (is_float_type(rt.base) && rt.ptr == 0)
                        fprintf(g->out, "    movq xmm2, rax\n    cvttsd2si rcx, xmm2\n");
                    else
                        fprintf(g->out, "    mov rcx, rax\n");
                    pop_tmp(g, "rax");
                    fprintf(g->out, is_float_type(lt.base) ? "    movq xmm1, rax\n" : "    cvtsi2sd xmm1, rax\n"); /* ฐาน -> xmm1 */
                    int l = new_label(g);
                    fprintf(g->out,
                        "    mov rax, 0x3FF0000000000000\n    movq xmm0, rax\n"  /* ผลลัพธ์เริ่ม = 1.0 */
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
                    /* rhs -> xmm1 (แปลงจาก int ถ้าจำเป็น) */
                    fprintf(g->out, is_float_type(rt.base) ? "    movq xmm1, rax\n" : "    cvtsi2sd xmm1, rax\n");
                    pop_tmp(g, "rax");
                    fprintf(g->out, is_float_type(lt.base) ? "    movq xmm0, rax\n" : "    cvtsi2sd xmm0, rax\n");
                    if (is_arith) {
                        const char *op = n->op==TK_PLUS ? "addsd" : n->op==TK_MINUS ? "subsd" :
                                         n->op==TK_STAR ? "mulsd" : "divsd";
                        fprintf(g->out, "    %s xmm0, xmm1\n    movq rax, xmm0\n", op);
                    } else {
                        /* เปรียบเทียบทศนิยมด้วย ucomisd (ตั้งแฟล็กแบบ unsigned) */
                        const char *cc = n->op==TK_EQ ? "sete" : n->op==TK_NEQ ? "setne" :
                                         n->op==TK_LT ? "setb" : n->op==TK_GT ? "seta" :
                                         n->op==TK_LE ? "setbe" : "setae";
                        fprintf(g->out, "    ucomisd xmm0, xmm1\n    %s al\n    movzx rax, al\n", cc);
                    }
                    break;
                }
            }

            /* การดำเนินการสองตัวทั่วไป (จำนวนเต็ม) */
            gen_expr(g, n->lhs);
            push_tmp(g);
            gen_expr(g, n->rhs);
            fprintf(g->out, "    mov rcx, rax\n"); /* rhs → rcx */
            pop_tmp(g, "rax");                     /* lhs → rax */

            /* เลขคณิตของ pointer: ปรับสเกลฝั่งจำนวนเต็มด้วยขนาดของชนิดที่ pointer ชี้
             *   ptr + int / ptr - int  -> int *= sizeof(*ptr);  int + ptr -> int *= sizeof(*ptr)
             *   ptr - ptr              -> (ผลต่างไบต์) / sizeof(*ptr) = จำนวน element (หลัง sub) */
            int ptrdiff_scale = 0;
            if (n->op == TK_PLUS || n->op == TK_MINUS) {
                if (lt.ptr > 0 && rt.ptr > 0 && n->op == TK_MINUS) {
                    ptrdiff_scale = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                } else if (lt.ptr > 0 && rt.ptr == 0) {
                    int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                    if (sc != 1) fprintf(g->out, "    imul rcx, %d\n", sc);   /* rhs (int) *= ขนาด */
                } else if (n->op == TK_PLUS && rt.ptr > 0 && lt.ptr == 0) {
                    int sc = type_size(g, rt.base, rt.ptr - 1, rt.sname);
                    if (sc != 1) fprintf(g->out, "    imul rax, %d\n", sc);   /* lhs (int) *= ขนาด */
                }
            }
            /* เลือก signedness ให้ถูกตามชนิดของตัวดำเนินการ:
             *   - shift / div / mod : อิง "ตัวซ้าย" (ตัวที่ถูกเลื่อน/ตัวตั้งหาร) เท่านั้น
             *     (ตัวนับ shift เป็น unsigned โดยธรรมชาติ — ถ้าใช้ OR จะทำให้ signed >> n กลายเป็น logical shift)
             *   - เปรียบเทียบ : ถ้าฝั่งใดเป็น unsigned ให้ถือเป็น unsigned (กันค่า u64 ใหญ่ถูกมองเป็นลบ) */
            int uns;
            if (n->op==TK_SHL || n->op==TK_SHR || n->op==TK_SLASH || n->op==TK_PERCENT)
                uns = is_unsigned_val(lt.base, lt.ptr);
            else
                uns = is_unsigned_val(lt.base, lt.ptr) || is_unsigned_val(rt.base, rt.ptr);
            gen_binop_apply(g, n->op, uns);
            /* ptr - ptr : หารผลต่างไบต์ด้วยขนาด element เพื่อให้ได้จำนวน element */
            if (ptrdiff_scale > 1)
                fprintf(g->out, "    cqo\n    mov rcx, %d\n    idiv rcx\n", ptrdiff_scale);
            break;
        }
        case ND_UNARY: {
            if (n->op == TK_MINUS) {
                gen_expr(g, n->operand);
                ExprType ot = type_of(g, n->operand);
                if (is_float_type(ot.base) && ot.ptr == 0)
                    fprintf(g->out, "    mov rcx, 0x8000000000000000\n    xor rax, rcx\n"); /* พลิก sign bit ของ double */
                else
                    fprintf(g->out, "    neg rax\n");
            } else if (n->op == TK_NOT) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    cmp rax, 0\n    sete al\n    movzx rax, al\n");
            } else if (n->op == TK_TILDE) {
                gen_expr(g, n->operand);
                fprintf(g->out, "    not rax\n");   /* bitwise NOT (กลับทุกบิต) */
                /* ตัดผลให้พอดีความกว้างของชนิด (เช่น ~(u8)0 = 255 ไม่ใช่ 64 บิตเต็ม) */
                ExprType ot = type_of(g, n->operand);
                if (ot.ptr == 0) {
                    int sz = type_size(g, ot.base, 0, ot.sname);
                    int sgn = is_signed_type(ot.base);
                    if (sz == 1)      fprintf(g->out, sgn ? "    movsx rax, al\n"   : "    movzx rax, al\n");
                    else if (sz == 2) fprintf(g->out, sgn ? "    movsx rax, ax\n"   : "    movzx rax, ax\n");
                    else if (sz == 4) fprintf(g->out, sgn ? "    movsxd rax, eax\n" : "    mov eax, eax\n");
                }
            } else if (n->op == TK_AMP) {
                /* &x : ที่อยู่ของ lvalue */
                gen_addr(g, n->operand);
            } else if (n->op == TK_STAR) {
                /* *p : อ่านค่าจากที่อยู่ที่ pointer ชี้ */
                gen_expr(g, n->operand);                 /* rax = ค่าของ pointer = ที่อยู่ */
                ExprType pt = type_of(g, n->operand);
                int pptr = pt.ptr > 0 ? pt.ptr - 1 : 0;
                if (pt.base == TYPE_STRUCT && pptr == 0) break; /* pointer-to-struct: คงที่อยู่ */
                gen_load_typed(g, pptr > 0 ? TYPE_USIZE : pt.base, type_size(g, pt.base, pptr, pt.sname));
            } else if (n->op == TK_PLUSPLUS || n->op == TK_MINUSMINUS) {
                /* เพิ่ม/ลดตัวแปร: ต้องเป็น lvalue (ident) */
                if (n->operand->kind != ND_IDENT) {
                    fprintf(stderr, "codegen error: ++/-- requires a variable\n"); g->had_error = 1; break;
                }
                Sym *s = find_var(g, n->operand->name);
                if (!s) { fprintf(stderr, "codegen error: undefined variable '%s'\n", n->operand->name); g->had_error = 1; break; }
                if (is_float_type(s->type) && s->ptr == 0) {
                    fprintf(stderr, "codegen error: '++'/'--' is not supported on floating-point; use 'x = x + 1.0'\n");
                    g->had_error = 1; break;
                }
                gen_load_var(g, s);                                 /* rax = ค่าเดิม (เป็นผลของนิพจน์) */
                fprintf(g->out, "    mov rcx, rax\n");
                /* pointer เลื่อนทีละ sizeof(*p), ชนิดอื่นเลื่อนทีละ 1 */
                int inc = (s->ptr > 0) ? type_size(g, s->type, s->ptr - 1, s->sname) : 1;
                fprintf(g->out, n->op == TK_PLUSPLUS ? "    add rcx, %d\n" : "    sub rcx, %d\n", inc);
                /* เก็บค่าใหม่กลับลงตัวแปรตามขนาด โดยไม่ทับ rax (ค่าเดิม) */
                const char *reg = s->size == 1 ? "cl" : s->size == 2 ? "cx" : s->size == 4 ? "ecx" : "rcx";
                if (s->is_global) { char lbl[LABEL_MAX]; global_label(s->name, lbl); fprintf(g->out, "    mov [rel %s], %s\n", lbl, reg); }
                else              { fprintf(g->out, "    mov [rbp - %d], %s\n", s->offset, reg); }
            }
            break;
        }
        case ND_CAST: {
            /* แปลงชนิดอย่างชัดเจน: ค่าต้นทางถูก gen ไว้ใน rax (int = บิตจำนวนเต็ม, float = บิต double) */
            gen_expr(g, n->operand);
            ExprType st = type_of(g, n->operand);
            int src_f = is_float_type(st.base) && st.ptr == 0;
            int dst_f = is_float_type(n->type) && n->ptr == 0;
            if (!src_f && dst_f) {
                /* int -> double; u64/usize ค่า ≥ 2^63 ต้องแปลงแบบ unsigned (cvtsi2sd มองเป็น signed) */
                if (st.ptr == 0 && is_unsigned_val(st.base, 0) && type_size(g, st.base, 0, NULL) >= 8) {
                    int l = new_label(g);
                    fprintf(g->out,
                        "    test rax, rax\n    js .Lu2d%d\n"
                        "    cvtsi2sd xmm0, rax\n    jmp .Lu2de%d\n"
                        ".Lu2d%d:\n"                                   /* บิตสูงติด: (x>>1)|(x&1) แล้วคูณสอง */
                        "    mov rcx, rax\n    shr rcx, 1\n    and rax, 1\n    or rcx, rax\n"
                        "    cvtsi2sd xmm0, rcx\n    addsd xmm0, xmm0\n"
                        ".Lu2de%d:\n    movq rax, xmm0\n", l, l, l, l);
                } else {
                    fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n");
                }
                break;
            }
            if (src_f && dst_f) break;                            /* float -> float: ระดับค่าเดียวกัน (double bits) */
            if (src_f && !dst_f) {
                /* float -> int; u64/usize ใช้เส้นทาง unsigned (cvttsd2si overflow ที่ ≥ 2^63) */
                if (n->ptr == 0 && is_unsigned_val(n->type, 0) && type_size(g, n->type, 0, NULL) >= 8) {
                    int l = new_label(g);
                    fprintf(g->out,
                        "    movq xmm0, rax\n"
                        "    mov rcx, 0x43E0000000000000\n    movq xmm1, rcx\n"  /* 2^63 (double) */
                        "    ucomisd xmm0, xmm1\n    jae .Ld2u%d\n"
                        "    cvttsd2si rax, xmm0\n    jmp .Ld2ue%d\n"
                        ".Ld2u%d:\n"
                        "    subsd xmm0, xmm1\n    cvttsd2si rax, xmm0\n"
                        "    mov rcx, 0x8000000000000000\n    or rax, rcx\n"
                        ".Ld2ue%d:\n", l, l, l, l);
                } else {
                    fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* float -> int: ตัดเศษ */
                }
            }
            /* cast เป็น bool: normalize เป็น 0/1 (ค่าที่ไม่ใช่ศูนย์ -> 1) ไม่ใช่ตัด low byte */
            if (n->ptr == 0 && n->type == TYPE_BOOL) {
                fprintf(g->out, "    cmp rax, 0\n    setne al\n    movzx rax, al\n");
                break;
            }
            /* ปลายทางเป็นจำนวนเต็ม (int->int หรือ float->int): ปรับความกว้าง/เครื่องหมายให้ตรงชนิด */
            if (n->ptr == 0 && !dst_f && n->type != TYPE_STR && n->type != TYPE_VOID && n->type != TYPE_STRUCT) {
                int sz = type_size(g, n->type, 0, NULL);
                int sgn = is_signed_type(n->type);
                if (sz == 1)      fprintf(g->out, sgn ? "    movsx rax, al\n"   : "    movzx rax, al\n");
                else if (sz == 2) fprintf(g->out, sgn ? "    movsx rax, ax\n"   : "    movzx rax, ax\n");
                else if (sz == 4) fprintf(g->out, sgn ? "    movsxd rax, eax\n" : "    mov eax, eax\n");
                /* sz == 8: เต็มความกว้างแล้ว */
            }
            break;
        }
        case ND_ASSIGN: {
            Node *target = n->lhs;
            /* เป้าหมายต้องเป็น lvalue: ident, member (a.b), หรือ deref (*p) */
            if (target->kind != ND_IDENT && target->kind != ND_MEMBER &&
                !(target->kind == ND_UNARY && target->op == TK_STAR)) {
                fprintf(stderr, "codegen error: invalid assignment target\n"); g->had_error = 1; break;
            }
            ExprType tt = type_of(g, target);
            /* การกำหนดค่า struct ทั้งก้อน (copy/literal) */
            if (tt.base == TYPE_STRUCT && tt.ptr == 0 && n->op == TK_ASSIGN) {
                gen_addr(g, target);          /* rax = ที่อยู่ปลายทาง */
                gen_store_struct(g, n->rhs);  /* เขียนค่า struct ลงปลายทาง */
                break;
            }
            int sz = type_size(g, tt.base, tt.ptr, tt.sname);
            DataType st = tt.ptr > 0 ? TYPE_USIZE : tt.base;  /* ชนิดสำหรับ load/store (pointer = 8 ไบต์) */
            if (n->op == TK_ASSIGN) {
                gen_expr(g, n->rhs);          /* rax = ค่า */
                { ExprType rvt = type_of(g, n->rhs); gen_coerce_num(g, rvt, tt); } /* int<->float โดยปริยาย */
                push_tmp(g);
                gen_addr(g, target);          /* rax = ที่อยู่ */
                pop_tmp(g, "rcx");            /* rcx = ค่า */
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov rax, rcx\n"); /* ผลของนิพจน์ = ค่าที่กำหนด */
            } else {
                /* x op= y : โหลดค่าเดิม คำนวณ แล้วเก็บกลับ (คำนวณ address สองครั้ง) */
                TokenType bop = n->op == TK_PLUS_ASSIGN ? TK_PLUS :
                                n->op == TK_MINUS_ASSIGN ? TK_MINUS :
                                n->op == TK_STAR_ASSIGN ? TK_STAR : TK_SLASH;
                gen_expr(g, n->rhs); push_tmp(g);                 /* เก็บ rhs */
                gen_addr(g, target);
                gen_load_typed(g, st, sz);                         /* rax = ค่าเดิม */
                pop_tmp(g, "rcx");                                /* rcx = rhs */
                if (is_float_type(tt.base) && tt.ptr == 0) {
                    /* คำนวณแบบ floating point (เดิมใน xmm0, rhs ใน xmm1) */
                    ExprType rt2 = type_of(g, n->rhs);
                    fprintf(g->out, "    movq xmm0, rax\n");
                    fprintf(g->out, is_float_type(rt2.base) ? "    movq xmm1, rcx\n" : "    cvtsi2sd xmm1, rcx\n");
                    const char *fop = bop==TK_PLUS ? "addsd" : bop==TK_MINUS ? "subsd" : bop==TK_STAR ? "mulsd" : "divsd";
                    fprintf(g->out, "    %s xmm0, xmm1\n    movq rax, xmm0\n", fop);
                } else {
                    /* pointer += / -= int : ปรับสเกล rhs ตามขนาดของชนิดที่ pointer ชี้ */
                    if (tt.ptr > 0 && (bop == TK_PLUS || bop == TK_MINUS)) {
                        int sc = type_size(g, tt.base, tt.ptr - 1, tt.sname);
                        if (sc != 1) fprintf(g->out, "    imul rcx, %d\n", sc);
                    }
                    gen_binop_apply(g, bop, is_unsigned_val(tt.base, tt.ptr)); /* rax = เดิม op rhs */
                }
                fprintf(g->out, "    mov rcx, rax\n");
                gen_addr(g, target);                              /* rax = ที่อยู่ (อีกครั้ง) */
                gen_store_typed(g, st, sz);
                fprintf(g->out, "    mov rax, rcx\n");
            }
            break;
        }
        case ND_CALL: {
            Node *callee = n->operand;

            /* ฟังก์ชันในตัวแบบ format: io.out("...{}...", args...) (สไตล์ Rust)
             * เป็น compiler intrinsic เพราะต้องแยกวิเคราะห์ format ตอนคอมไพล์และเลือก
             * specifier ตามชนิดของแต่ละ argument — คล้าย println! ของ Rust ที่เป็น macro */
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
                Node *vals[256];   /* ค่าที่ส่งให้ printf จริง (struct ขยายเป็นรายฟิลด์) */
                char *cf = build_c_format(g, n, n->items[0]->str_val, &flen, &nph, vals, &nv, 256);
                int nvals = n->nitems - 1;
                if (nph != nvals) {
                    fprintf(stderr, "codegen error: io.out: %d placeholder(s) but %d argument(s)\n", nph, nvals);
                    g->had_error = 1; free(cf); break;
                }
                int idx = intern_string(g, cf, flen);
                free(cf);
                /* ผลลัพธ์ struct จากฟังก์ชันที่ถูกพิมพ์ ({} struct): เรียก/materialize ลงช่อง temp "ครั้งเดียว"
                 * ที่นี่ แล้วฟิลด์ทั้งหมดอ่านจาก temp นั้น (ผ่าน ND_FRAMEREF) — กันเรียกฟังก์ชันซ้ำต่อฟิลด์ */
                for (int k = 1; k < n->nitems; k++) {
                    Node *a = n->items[k];
                    if (a->kind == ND_CALL && a->int_val) {
                        ExprType at = type_of(g, a);
                        if (at.base == TYPE_STRUCT && at.ptr == 0) gen_expr(g, a); /* เติม temp; ทิ้งผลใน rax */
                    }
                }
                /* ส่งให้ printf ตาม win64 ABI: format + ค่าทุกตัว (4 ตัวแรกผ่านรีจิสเตอร์ ที่เหลือลง stack)
                 * printf เป็น variadic: ค่าทศนิยมต้องใส่ทั้ง GPR และ xmm */
                int total = 1 + nv;                          /* format + ค่าที่ขยายแล้ว */
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

            /* การเรียกฟังก์ชันทั่วไป: ถ้าคืน struct ให้ materialize ลงช่อง temp แล้วคืน "ที่อยู่"
             * (struct ใช้ที่อยู่เป็นค่า) — ทำให้ใช้ผลลัพธ์ struct เป็น rvalue ได้ เช่น f(make()) */
            {
                ExprType rt = type_of(g, n);
                if (rt.base == TYPE_STRUCT && rt.ptr == 0 && n->int_val) {
                    fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val); /* sret = ช่อง temp */
                    gen_call(g, n, 1);
                    fprintf(g->out, "    lea rax, [rbp - %lld]\n", n->int_val); /* ผลลัพธ์ = ที่อยู่ struct */
                } else {
                    gen_call(g, n, 0);
                }
            }
            break;
        }
        case ND_MEMBER: {
            /* การเข้าถึงสมาชิก a.b เป็น rvalue: หาที่อยู่ของฟิลด์แล้วโหลดตามขนาด */
            ExprType et = type_of(g, n);
            gen_addr(g, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) break; /* ฟิลด์เป็น struct ซ้อน: คงที่อยู่ */
            gen_load_typed(g, et.ptr > 0 ? TYPE_USIZE : et.base, type_size(g, et.base, et.ptr, et.sname));
            break;
        }
        case ND_FRAMEREF:   /* ช่อง temp ของ struct: ค่าคือที่อยู่ (struct ใช้ที่อยู่เป็นค่า) */
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

/* gen คำสั่ง */
static void gen_stmt(Gen *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_VAR_DECL: {
            Sym *s = &g->locals[n->int_val]; /* slot ที่จองไว้ใน pre-pass (เจาะจงตัวนี้ ไม่ใช่ตัว shadow) */
            if (n->operand && s) { /* มีค่าเริ่มต้น */
                if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    /* ตัวแปร struct: เขียนค่า (literal/copy/ผลจากฟังก์ชัน) ลงพื้นที่ของตัวแปร */
                    char lbl[LABEL_MAX];
                    if (s->is_global) { global_label(s->name, lbl); fprintf(g->out, "    lea rax, [rel %s]\n", lbl); }
                    else              { fprintf(g->out, "    lea rax, [rbp - %d]\n", s->offset); }
                    gen_store_struct(g, n->operand);
                } else {
                    gen_expr(g, n->operand);     /* rax = ค่า */
                    ExprType vt = type_of(g, n->operand), dt = { s->type, s->ptr, s->sname, s->sig };
                    gen_coerce_num(g, vt, dt);   /* แปลง int<->float โดยปริยายถ้าจำเป็น */
                    gen_store_var(g, s);
                }
            }
            /* ตัวแปรมองเห็นได้หลังจุดประกาศ (เพื่อให้ let x = x อ้างถึงตัวนอกได้) */
            g->visible[g->nvisible++] = (int)n->int_val;
            break;
        }
        case ND_EXPR_STMT:
            gen_expr(g, n->operand);
            break;
        case ND_RETURN:
            if (g->sret_off != 0 && n->operand) {
                /* ฟังก์ชันคืน struct: เขียนค่าลงที่ hidden pointer แล้วคืน pointer นั้น */
                fprintf(g->out, "    mov rax, [rbp - %d]\n", g->sret_off);
                gen_store_struct(g, n->operand);
                fprintf(g->out, "    mov rax, [rbp - %d]\n", g->sret_off);
            } else if (n->operand) {
                gen_expr(g, n->operand);         /* ค่าที่คืนอยู่ใน rax */
                ExprType vt = type_of(g, n->operand);
                int vf = is_float_type(vt.base) && vt.ptr == 0;
                if (g->cur_ret_float && !vf) fprintf(g->out, "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n"); /* int -> float */
                else if (!g->cur_ret_float && vf) fprintf(g->out, "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n"); /* float -> int */
                if (g->cur_ret_float) {
                    fprintf(g->out, "    movq xmm0, rax\n"); /* คืน float ทาง xmm0 */
                    if (g->cur_ret_f32c) fprintf(g->out, "    cvtsd2ss xmm0, xmm0\n"); /* C รับ single */
                }
            } else {
                fprintf(g->out, "    xor eax, eax\n");
            }
            fprintf(g->out, "    leave\n    ret\n");  /* leave = คืน rsp/rbp แล้ว ret */
            break;
        case ND_BLOCK: {
            int mark = g->nvisible;   /* เปิด scope ใหม่ */
            for (int i = 0; i < n->nitems; i++) gen_stmt(g, n->items[i]);
            g->nvisible = mark;       /* ปิด scope: ตัวแปรใน block หายไปจากการมองเห็น */
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
            int lcont = new_label(g);  /* เป้าหมายของ continue และจุดวนกลับ */
            int lbrk  = new_label(g);  /* เป้าหมายของ break และทางออกลูป */
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
            int lcond = new_label(g);  /* จุดตรวจเงื่อนไข */
            int lcont = new_label(g);  /* เป้าหมายของ continue (ไปทำส่วน step) */
            int lbrk  = new_label(g);  /* เป้าหมายของ break */
            int mark = g->nvisible;    /* ตัวแปรใน for-init มี scope แค่ในลูป */
            /* ส่วนเริ่มต้น: อาจเป็นการประกาศตัวแปรหรือนิพจน์ */
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
            g->nvisible = mark;        /* ปิด scope ของ for */
            break;
        }
        case ND_DOWHILE: {
            /* ทำงาน body ก่อนหนึ่งครั้ง แล้วค่อยตรวจเงื่อนไข (วนซ้ำถ้าจริง) */
            int lstart = new_label(g);
            int lcont = new_label(g);   /* continue ไปตรวจเงื่อนไข */
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
            /* ประเมินค่าที่เทียบหนึ่งครั้งแล้วเก็บไว้บน stack ชั่วคราว
             * จากนั้นเทียบกับแต่ละ case (กระโดดเข้า label ของ case ที่ตรง) */
            int lbrk = new_label(g);
            int off = (int)n->int_val;            /* ช่อง frame เก็บค่าที่เทียบ (จองใน pre-pass) */
            int *caselbl = (int *)malloc(sizeof(int) * (n->nitems > 0 ? n->nitems : 1));
            int ldefault = -1;
            gen_expr(g, n->cond);
            fprintf(g->out, "    mov [rbp - %d], rax\n", off);   /* เก็บค่าที่เทียบไว้บน frame */
            for (int i = 0; i < n->nitems; i++) {
                caselbl[i] = new_label(g);
                Node *c = n->items[i];
                if (!c->operand) { ldefault = caselbl[i]; continue; } /* default ข้ามการเทียบ */
                gen_expr(g, c->operand);
                fprintf(g->out, "    mov rcx, rax\n");
                fprintf(g->out, "    mov rax, [rbp - %d]\n", off);
                fprintf(g->out, "    cmp rax, rcx\n    je .Lcase%d\n", caselbl[i]);
            }
            if (ldefault >= 0) fprintf(g->out, "    jmp .Lcase%d\n", ldefault);
            else               fprintf(g->out, "    jmp .Lbrk%d\n", lbrk);
            /* break ออกจาก switch; continue ส่งต่อไปยังลูปที่ครอบอยู่ (cont = -1) */
            if (g->nloops >= MAX_LOOP) { fprintf(stderr, "codegen error: too many nested loops/switch (max %d)\n", MAX_LOOP); g->had_error = 1; break; }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = -1;
            g->nloops++;
            for (int i = 0; i < n->nitems; i++) {
                fprintf(g->out, ".Lcase%d:\n", caselbl[i]);  /* ตกผ่าน (fallthrough) แบบ C */
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
            /* หาเป้าหมาย continue ที่ใกล้ที่สุดที่เป็นลูปจริง (ข้าม switch ที่ cont = -1) */
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

/* เขียนค่า struct ลงปลายทางที่ "ที่อยู่อยู่ใน rax" ขณะเรียก
 * รองรับค่า 3 แบบ: struct literal, ผลจากฟังก์ชันที่คืน struct (sret), และการคัดลอกจาก struct อื่น */
static void gen_store_struct(Gen *g, Node *value) {
    push_tmp(g); /* [rsp] = ที่อยู่ปลายทาง */
    if (value->kind == ND_STRUCT_LIT) {
        StructInfo *s = find_struct(g, value->name);
        if (!s) { fprintf(stderr, "codegen error: unknown struct '%s'\n", value->name); g->had_error = 1; }
        else for (int i = 0; i < value->nitems; i++) {
            Node *fi = value->items[i];                 /* ND_ASSIGN: lhs = ชื่อฟิลด์, rhs = นิพจน์ */
            Field *f = find_field(s, fi->lhs->name);
            if (!f) { fprintf(stderr, "codegen error: no field '%s' in struct '%s'\n", fi->lhs->name, s->name); g->had_error = 1; continue; }
            if (f->type == TYPE_STRUCT && f->ptr == 0) {
                /* ฟิลด์เป็น struct ซ้อน: คำนวณที่อยู่ของฟิลด์แล้วเขียนค่าลงไปแบบ recursive */
                fprintf(g->out, "    mov rax, [rsp]\n");
                if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
                gen_store_struct(g, fi->rhs);
            } else {
                gen_expr(g, fi->rhs);                   /* rax = ค่า */
                fprintf(g->out, "    mov rcx, rax\n");
                fprintf(g->out, "    mov rax, [rsp]\n"); /* ที่อยู่ฐานของปลายทาง */
                if (f->offset) fprintf(g->out, "    add rax, %d\n", f->offset);
                gen_store_typed(g, f->ptr > 0 ? TYPE_USIZE : f->type, f->size);
            }
        }
    } else if (value->kind == ND_CALL) {
        /* ฟังก์ชันคืน struct: ส่งที่อยู่ปลายทางเป็น hidden argument แล้วให้ callee เขียนเอง */
        fprintf(g->out, "    mov rax, [rsp]\n");
        gen_call(g, value, 1);
    } else {
        /* คัดลอก struct จาก lvalue ต้นทาง (ident/member/deref) */
        ExprType vt = type_of(g, value);
        StructInfo *s = find_struct(g, vt.sname);
        int size = s ? s->size : 0;
        gen_addr(g, value);                  /* rax = ที่อยู่ต้นทาง */
        fprintf(g->out, "    mov r10, rax\n");   /* src */
        fprintf(g->out, "    mov r11, [rsp]\n"); /* dst */
        gen_memcpy(g, size);
    }
    fprintf(g->out, "    add rsp, 16\n");    /* คืนพื้นที่ที่ใช้เก็บที่อยู่ปลายทาง */
}

/* gen การเรียกฟังก์ชัน (resolve เป้าหมาย, จัดอาร์กิวเมนต์ตาม win64 ABI)
 * has_sret = 1 หมายถึงฟังก์ชันคืน struct โดยที่อยู่ปลายทางถูกใส่ไว้ใน rax ก่อนเรียก */
static void gen_call(Gen *g, Node *n, int has_sret) {
    Node *callee = n->operand;
    Node *target = NULL;
    Node *self_expr = NULL;   /* ถ้าเป็น method: นิพจน์ตัวรับ (receiver) */
    int   self_is_ptr = 0;    /* receiver เป็น pointer อยู่แล้วหรือไม่ (ถ้าใช่ ส่งค่าตรง ๆ ไม่ต้อง &) */

    /* การเรียกผ่าน function pointer (indirect): callee เป็นค่าชนิด TYPE_FUNC (ตัวแปร/ฟิลด์)
     * sig = ลายเซ็น (พารามิเตอร์ + ชนิดคืนค่า); ไม่มี self; เรียกด้วย call rax */
    Node *sig = expr_func_sig(g, callee);
    int indirect = (sig != NULL);

    if (!indirect) {
        if (callee->kind == ND_IDENT) {
            target = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
            if (!target) target = find_func(g, "", callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function '%s'\n", callee->name); g->had_error = 1; return; }
        } else if (callee->kind == ND_MEMBER) {
            /* ลองตีความเป็น method call ก่อน: ตัวรับเป็นนิพจน์ที่มีชนิดเป็น struct */
            ExprType bt = type_of(g, callee->operand);
            if (bt.base == TYPE_STRUCT && bt.sname) {
                target = find_func(g, bt.sname, callee->name);  /* method อยู่ใน namespace = ชื่อ struct */
                if (target) { self_expr = callee->operand; self_is_ptr = (bt.ptr > 0); }
            }
            /* ถ้าไม่ใช่ method ให้ตีความ base เป็น namespace ของโมดูล (เช่น net.TcpServer) */
            if (!target && callee->operand->kind == ND_IDENT)
                target = find_func(g, callee->operand->name, callee->name);
            if (!target) { fprintf(stderr, "codegen error: undefined function/method '%s' (did you import it?)\n", callee->name); g->had_error = 1; return; }
        } else {
            fprintf(stderr, "codegen error: unsupported call target\n"); g->had_error = 1; return;
        }
        sig = target;   /* การเรียกตรง: target เป็นทั้งลายเซ็นและปลายทาง */
    }

    int returns_struct = (sig->type == TYPE_STRUCT && sig->ptr == 0);
    if (returns_struct && !has_sret) {
        fprintf(stderr, "codegen error: a struct-returning call must be assigned to a variable\n");
        g->had_error = 1; return;
    }
    /* ลำดับค่าทั้งหมด: [hidden sret pointer]?, [self ถ้าเป็น method]?, ตามด้วยอาร์กิวเมนต์จริง
     * 4 ตัวแรกส่งผ่านรีจิสเตอร์ (rcx,rdx,r8,r9) ที่เหลือวางบน stack เหนือ shadow space (win64 ABI) */
    int nself = self_expr ? 1 : 0;
    int total = n->nitems + nself + (has_sret ? 1 : 0);
    int nstack = total > 4 ? total - 4 : 0;
    int callspace = 32 + nstack * 8;          /* shadow space 32 ไบต์ + ช่องอาร์กิวเมนต์ส่วนเกิน */
    callspace = (callspace + 15) / 16 * 16;    /* ปัดให้รักษาการจัดเรียง 16 ไบต์ */
    const char *regs[4] = { "rcx", "rdx", "r8", "r9" };

    /* 1) ประเมินค่าทุกตัวตามลำดับ ดันลง temp stack (ตัวละ 16 ไบต์)
     * sret ต้องดันก่อน (ใช้ rax ที่ caller ตั้งมาเป็นที่อยู่ปลายทาง) — ห้ามให้นิพจน์อื่นทับ rax ก่อน */
    if (has_sret) push_tmp(g);                 /* ค่าแรก = ที่อยู่ปลายทาง (อยู่ใน rax) */
    if (self_expr) {                           /* self = ตัวชี้ไปยังตัวรับ */
        if (self_is_ptr) gen_expr(g, self_expr);  /* receiver เป็น pointer อยู่แล้ว */
        else             gen_addr(g, self_expr);  /* receiver เป็น struct value -> ใช้ที่อยู่ */
        push_tmp(g);
    }
    int poff = self_expr ? 1 : 0;  /* method: items[0] ของ sig คือ self */
    for (int i = 0; i < n->nitems; i++) {
        gen_expr(g, n->items[i]);
        if (i + poff < sig->nitems) {     /* แปลง int<->float ให้ตรงชนิดพารามิเตอร์ (เว้น vararg) */
            Node *pp = sig->items[i + poff];
            ExprType pt = { pp->type, pp->ptr, pp->type_name, pp->sig }, at = type_of(g, n->items[i]);
            gen_coerce_num(g, at, pt);
        }
        push_tmp(g);
    }
    /* indirect: ประเมิน "ที่อยู่ฟังก์ชัน" ดันลง temp ท้ายสุด (อยู่บนสุด = ใกล้ยอด) แล้วโหลด call rax ทีหลัง
     * — ทำหลัง args เพื่อไม่ให้ทับ rax ตอน sret และเก็บไว้บน stack กัน rcx/rdx/... ทับ */
    int extra = indirect ? 1 : 0;
    if (indirect) { gen_expr(g, callee); push_tmp(g); }

    /* 2) จองพื้นที่สำหรับการเรียก แล้วย้ายค่าจาก temp ไปยังปลายทาง (รีจิสเตอร์/สแต็ก) */
    int arg_start = (has_sret ? 1 : 0) + nself;  /* ดัชนีค่าตัวแรกที่เป็นอาร์กิวเมนต์จริง */
    fprintf(g->out, "    sub rsp, %d\n", callspace);
    for (int i = 0; i < total; i++) {
        int srcoff = callspace + (total + extra - 1 - i) * 16; /* +extra: เผื่อ temp ที่อยู่ฟังก์ชันบนสุด */
        fprintf(g->out, "    mov rax, [rsp + %d]\n", srcoff);
        if (i < 4) fprintf(g->out, "    mov %s, rax\n", regs[i]);
        else       fprintf(g->out, "    mov [rsp + %d], rax\n", 32 + (i - 4) * 8);
        /* อาร์กิวเมนต์ทศนิยมต้องอยู่ในรีจิสเตอร์ xmm ด้วย — ตัดสินจาก "ชนิดพารามิเตอร์" (หลัง coerce)
         * ส่วน vararg (เกินจำนวน param ที่ประกาศ เช่น printf) ใช้ชนิด argument */
        if (i >= arg_start && i < 4) {
            int pidx = i - arg_start;
            int pi = pidx + poff;                           /* ดัชนีพารามิเตอร์จริง (รวม self) */
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

    /* 3) เรียก: ตรง = call <label>; indirect = โหลดที่อยู่ฟังก์ชัน (temp บนสุด ที่ offset = callspace) แล้ว call rax */
    if (indirect) {
        fprintf(g->out, "    mov rax, [rsp + %d]\n", callspace); /* ที่อยู่ฟังก์ชัน = temp บนสุด */
        fprintf(g->out, "    call rax\n");
    } else {
        char lbl[LABEL_MAX]; func_label_of(target, lbl);
        fprintf(g->out, "    call %s\n", lbl);
    }
    fprintf(g->out, "    add rsp, %d\n", callspace);
    fprintf(g->out, "    add rsp, %d\n", (total + extra) * 16); /* รวม temp ที่อยู่ฟังก์ชันด้วยถ้า indirect */
    /* ค่าคืนแบบ float มาทาง xmm0 -> นำกลับเข้าสู่ convention ของเรา (bit-pattern ใน rax) */
    if (is_float_type(sig->type) && sig->ptr == 0) {
        if (sig->is_extern && sig->type == TYPE_F32)
            fprintf(g->out, "    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n"); /* C คืน single -> double */
        else
            fprintf(g->out, "    movq rax, xmm0\n");
    }
}

/* gen ฟังก์ชันหนึ่งตัว */
static void gen_func(Gen *g, Node *fn, Node *program) {
    char lbl[LABEL_MAX];
    func_label_of(fn, lbl);
    /* namespace สำหรับ resolve การเรียกแบบไม่ระบุชื่อ = โมดูลที่สังกัด (mod)
     * สำคัญกับ method: cur_ns ต้องเป็นโมดูล ไม่ใช่ชื่อ struct ไม่งั้นจะเรียกตัวเองวน */
    g->cur_ns = fn->mod ? fn->mod : "";

    /* เคลียร์ตาราง local แล้วจองพื้นที่ */
    g->nlocals = 0;
    g->nvisible = 0;
    int frame = 0;
    g->sret_off = 0;

    /* ฟังก์ชันที่คืน struct ใช้ hidden pointer (sret): จองช่องเก็บ pointer นั้นก่อน
     * และพารามิเตอร์จริงจะเลื่อนรีจิสเตอร์ไปหนึ่งตำแหน่ง (rcx ถูกใช้เป็น sret) */
    int returns_struct = (fn->type == TYPE_STRUCT && fn->ptr == 0 && !fn->is_extern);
    if (returns_struct)
        g->sret_off = add_local(g, "$sret", TYPE_USIZE, 0, NULL, NULL, &frame);
    /* ฟังก์ชันคืนค่าทศนิยมต้องคืนผ่าน xmm0 (ตั้งธงไว้ใช้ตอน return) */
    g->cur_ret_float = is_float_type(fn->type) && fn->ptr == 0;
    g->cur_ret_f32c = fn->is_export && fn->type == TYPE_F32 && fn->ptr == 0; /* export f32 -> คืน single ให้ C */

    int param_start = g->nlocals;
    for (int i = 0; i < fn->nitems; i++) /* พารามิเตอร์ */
        add_local(g, fn->items[i]->name, fn->items[i]->type, fn->items[i]->ptr, fn->items[i]->type_name, fn->items[i]->sig, &frame);
    collect_locals(g, fn->body, &frame);   /* ตัวแปรภายใน body */
    /* ช่อง temp สำหรับ struct ที่คืนจากฟังก์ชันเมื่อใช้เป็น rvalue —
     * ต้องให้ทุก local "มองเห็นได้" ชั่วคราว เพื่อให้ type_of อนุมานชนิดตัวรับ method ได้
     * (เช่น p.fmt() ต้องรู้ว่า p เป็น struct อะไร) แล้วรีเซ็ตก่อนเริ่ม gen จริง */
    int saved_vis = g->nvisible;
    for (int i = 0; i < g->nlocals; i++) g->visible[g->nvisible++] = i;
    collect_struct_temps(g, fn->body, &frame);
    g->nvisible = saved_vis;

    /* ปรับขนาดเฟรมให้เป็นพหุคูณของ 16 เพื่อรักษาการจัดเรียง stack */
    int frame_size = (frame + 15) / 16 * 16;

    /* ส่วนหัวฟังก์ชัน (prologue) */
    fprintf(g->out, "\n%s:\n", lbl);
    fprintf(g->out, "    push rbp\n    mov rbp, rsp\n");
    if (frame_size > 0) fprintf(g->out, "    sub rsp, %d\n", frame_size);

    /* บันทึกพารามิเตอร์ (และ hidden pointer) ลงช่อง stack ของตน
     *   - ตำแหน่ง ABI 0..3 มาจากรีจิสเตอร์ rcx,rdx,r8,r9
     *   - ตำแหน่ง 4 ขึ้นไป ผู้เรียกวางไว้บน stack อ่านได้จาก [rbp + 48 + (pos-4)*8]
     *     (8 = return address, 16..48 = shadow space ของผู้เรียก) */
    const char *argregs[4] = { "rcx", "rdx", "r8", "r9" };
    int base = returns_struct ? 1 : 0;
    if (returns_struct) fprintf(g->out, "    mov [rbp - %d], rcx\n", g->sret_off);
    for (int i = 0; i < fn->nitems; i++) {
        Node *p = fn->items[i];
        Sym *psym = &g->locals[param_start + i];
        int pos = i + base;            /* ตำแหน่งตาม ABI (รวม hidden sret pointer) */
        int is_float_param = is_float_type(p->type) && p->ptr == 0;
        /* พารามิเตอร์แบบ struct by-value: ผู้เรียกส่ง "ที่อยู่" ของ struct มา
         * callee คัดลอกเนื้อ struct เข้าช่องของตัวเอง (ได้ความหมาย by-value, มุทักไม่กระทบผู้เรียก) */
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
                /* พารามิเตอร์ทศนิยมมาทางรีจิสเตอร์ xmm<pos>
                 * ฟังก์ชัน export ที่ถูกเรียกจาก C: f32 มาเป็น single -> ขยายเป็น double ก่อน
                 * (ฝั่ง MVS เก็บค่าทศนิยมเป็น bit-pattern ของ double) */
                if (fn->is_export && p->type == TYPE_F32)
                    fprintf(g->out, "    cvtss2sd xmm%d, xmm%d\n", pos, pos);
                fprintf(g->out, "    movq rax, xmm%d\n", pos);
                gen_store_var(g, psym);   /* แปลง/ตัดขนาดตามชนิด (เช่น f32 -> single) */
            } else {
                fprintf(g->out, "    mov [rbp - %d], %s\n", psym->offset, argregs[pos]);
            }
        } else {
            /* พารามิเตอร์ตัวที่ 5+ ผู้เรียกวางบน stack ที่ [rbp + 48 + (pos-4)*8] */
            fprintf(g->out, "    mov rax, [rbp + %d]\n", 48 + (pos - 4) * 8);
            gen_store_var(g, psym);
        }
    }
    /* พารามิเตอร์มองเห็นได้ตลอดทั้งฟังก์ชัน */
    for (int i = 0; i < fn->nitems; i++) g->visible[g->nvisible++] = param_start + i;

    /* ถ้าเป็น main (namespace ว่าง) ให้ตั้งค่าตัวแปร global ที่มีค่าเริ่มต้นก่อนเข้า body */
    if ((fn->ns == NULL || fn->ns[0] == 0) && strcmp(fn->name, "main") == 0) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind == ND_VAR_DECL && d->operand) {
                Sym *s = find_var(g, d->name); /* ต้องเป็น global */
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

    /* ตัวฟังก์ชัน */
    gen_stmt(g, fn->body);

    /* epilogue เผื่อกรณีไม่มี return ปิดท้าย (เช่น ฟังก์ชัน void) */
    fprintf(g->out, "    xor eax, eax\n    leave\n    ret\n");
}

/* จุดเข้าหลักของ backend */
int x86_64_win_generate(Node *program, FILE *out) {
    Gen g;
    memset(&g, 0, sizeof(g));
    g.out = out;

    /* รอบที่ 1: ลงทะเบียน struct ทุกชนิด แล้วคำนวณ layout แบบ fixpoint
     * (แยกสองขั้นเพื่อรองรับ struct ที่มีฟิลด์เป็น struct ซึ่งประกาศทีหลัง) */
    for (int i = 0; i < program->nitems; i++)
        if (program->items[i]->kind == ND_STRUCT_DECL)
            register_struct(&g, program->items[i]);
    layout_structs(&g);

    /* รอบที่ 2: ลงทะเบียนฟังก์ชันและตัวแปร global (รองรับการอ้างอิงล่วงหน้า) */
    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind == ND_FUNC) {
            if (g.nfuncs >= MAX_FUNC) { fprintf(stderr, "codegen error: too many functions (max %d)\n", MAX_FUNC); g.had_error = 1; break; }
            g.funcs[g.nfuncs++] = d; /* เก็บโหนดทั้งตัว (มี ns/is_extern ครบ) */
        } else if (d->kind == ND_VAR_DECL) {
            if (g.nglobals >= MAX_SYM) { fprintf(stderr, "codegen error: too many global variables (max %d)\n", MAX_SYM); g.had_error = 1; break; }
            Sym *s = &g.globals[g.nglobals++];
            s->name = d->name; s->type = d->type; s->ptr = d->ptr; s->sname = d->type_name;
            s->size = type_size(&g, d->type, d->ptr, d->type_name);
            s->is_global = 1; s->offset = 0;
        }
    }

    /* ตรวจว่ามีการ import โมดูล io หรือไม่ (มีฟังก์ชันที่ namespace = "io") เพื่อ gate io.out */
    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->ns && strcmp(g.funcs[i]->ns, "io") == 0) { g.io_imported = 1; break; }

    /* วิเคราะห์การเข้าถึงได้ (tree-shaking): เริ่มจาก main และค่าเริ่มต้นของ global
     * เพื่อ gen เฉพาะฟังก์ชันที่ถูกเรียกจริง ลดขนาดเอาต์พุต */
    char reached[MAX_FUNC]; memset(reached, 0, sizeof(reached));
    int has_main = 0;
    for (int i = 0; i < g.nfuncs; i++) {
        Node *f = g.funcs[i];
        /* รากของการเข้าถึง: main และฟังก์ชันที่ export (ให้ C เรียก) */
        if ((f->ns == NULL || f->ns[0] == 0) && strcmp(f->name, "main") == 0 && !f->is_extern) {
            has_main = 1; reach_func(&g, i, reached);
        }
        if (f->is_export) reach_func(&g, i, reached);
    }
    for (int i = 0; i < program->nitems; i++) /* การเรียกในค่าเริ่มต้นของ global (รันใน main) */
        if (program->items[i]->kind == ND_VAR_DECL && program->items[i]->operand)
            reach_node(&g, program->items[i]->operand, "", reached);

    /* ส่วนหัวไฟล์แอสเซมบลี */
    fprintf(out, "; ===== MVS compiler output (x86-64 Windows, NASM syntax) =====\n");
    fprintf(out, "default rel\n");          /* ใช้การอ้างอิงแบบ RIP-relative */
    if (has_main) fprintf(out, "global main\n"); /* ส่งออก main ให้ C runtime เรียก (ถ้ามี) */
    /* ส่งออกฟังก์ชันที่ทำเครื่องหมาย export ไว้ให้ภาษา C เรียกได้ (ชื่อสัญลักษณ์ดิบ) */
    for (int i = 0; i < g.nfuncs; i++)
        if (g.funcs[i]->is_export) fprintf(out, "global %s\n", g.funcs[i]->name);

    /* ประกาศ foreign function (extern) เฉพาะตัวที่เข้าถึงได้ — dedup ตามชื่อ */
    for (int i = 0; i < g.nfuncs; i++) {
        if (!g.funcs[i]->is_extern || !reached[i]) continue;
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (g.funcs[j]->is_extern && reached[j] && strcmp(g.funcs[j]->name, g.funcs[i]->name) == 0) { dup = 1; break; }
        if (!dup) fprintf(out, "extern %s\n", g.funcs[i]->name);
    }
    fprintf(out, "\n");

    /* gen ทุกฟังก์ชันที่มี body และเข้าถึงได้ (ส่วน .text) — ข้าม extern และฟังก์ชันที่ไม่ถูกใช้ */
    fprintf(out, "section .text\n");
    for (int i = 0; i < program->nitems; i++) {
        Node *d = program->items[i];
        if (d->kind != ND_FUNC || d->is_extern) continue;
        if (d->ngen > 0) continue;                  /* ข้าม generic template (เหลือแต่ instance) */
        if (!reached[func_index(&g, d)]) continue; /* tree-shaking: ข้ามฟังก์ชันที่ไม่ถูกเรียก */
        gen_func(&g, d, program);
    }

    /* ส่วนข้อมูลอ่านอย่างเดียว: สตริงค่าคงที่ทั้งหมด (รวม format string ที่มาจาก std/io.mvs) */
    fprintf(out, "\nsection .data\n");
    for (int i = 0; i < g.nstrs; i++) {
        fprintf(out, "mvs_str_%d: db ", i);
        for (int j = 0; j < g.strs[i].len; j++) fprintf(out, "%d,", g.strs[i].data[j]);
        fprintf(out, "0\n"); /* ปิดท้ายด้วย 0 ให้เป็นสตริงแบบ C */
    }

    /* ส่วนตัวแปร global (จองพื้นที่ตามขนาดจริง ปัดขึ้นเป็นพหุคูณของ 8 เริ่มต้นเป็น 0) */
    fprintf(out, "\nsection .bss\n");
    for (int i = 0; i < g.nglobals; i++) {
        char lbl[LABEL_MAX]; global_label(g.globals[i].name, lbl);
        int slot = (g.globals[i].size + 7) / 8 * 8;
        if (slot < 8) slot = 8;
        fprintf(out, "%s: resb %d\n", lbl, slot);
    }

    return g.had_error ? 1 : 0;
}
