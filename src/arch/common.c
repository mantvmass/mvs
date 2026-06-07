/*
 * common.c - ส่วนกลางของ backend (ไม่ขึ้นกับสถาปัตยกรรม)
 * ดูคำอธิบายภาพรวมใน common.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

/* ---------- ฟังก์ชันช่วยทั่วไป ---------- */

/* ออกหมายเลข label ใหม่ที่ไม่ซ้ำ */
int new_label(Gen *g) { return g->label_id++; }

/* เขียนชื่อ label ของฟังก์ชันลงบัฟเฟอร์ตามกฎ:
 *   - extern (foreign): ใช้ชื่อดิบ เช่น printf (เพื่อให้ตรงกับสัญลักษณ์ใน C runtime)
 *   - main (namespace ว่าง): คงชื่อ main ให้ C runtime เรียกได้
 *   - มี namespace: mvs_<ns>_<name>   เช่น io.out -> mvs_io_out
 *   - ไม่มี namespace: mvs_<name>      เช่น factorial -> mvs_factorial */
void func_label_of(Node *fn, char *buf) {
    /* extern และ export ใช้ชื่อสัญลักษณ์ดิบ เพื่อให้ตรงกับ/เรียกได้จากภาษา C (buf ต้องมีขนาด LABEL_MAX) */
    if (fn->is_extern || fn->is_export) { snprintf(buf, LABEL_MAX, "%s", fn->name); return; }
    const char *ns = fn->ns ? fn->ns : "";
    if (ns[0] == 0 && strcmp(fn->name, "main") == 0) { snprintf(buf, LABEL_MAX, "main"); return; }
    if (ns[0]) snprintf(buf, LABEL_MAX, "mvs_%s_%s", ns, fn->name);   /* method ใช้ ns = ชื่อ struct */
    else       snprintf(buf, LABEL_MAX, "mvs_%s", fn->name);
}

/* ค้นหาฟังก์ชันตาม (namespace, ชื่อ) — namespace ว่างหมายถึงระดับ global */
Node *find_func(Gen *g, const char *ns, const char *name) {
    for (int i = 0; i < g->nfuncs; i++) {
        Node *f = g->funcs[i];
        const char *fns = f->ns ? f->ns : "";
        if (strcmp(f->name, name) == 0 && strcmp(fns, ns) == 0) return f;
    }
    return NULL;
}

/* เขียนชื่อ label ของตัวแปร global (เติมนำหน้า mvs_gv_ กันชนกับสัญลักษณ์ใน C runtime
 * และกันชนกับ label ฟังก์ชัน/method ที่เป็น mvs_<ns>_<name> เช่น struct ชื่อ g) */
void global_label(const char *name, char *buf) {
    snprintf(buf, LABEL_MAX, "mvs_gv_%s", name);
}

/* ค้นหาสัญลักษณ์ตัวแปร: ดูจาก scope ที่มองเห็นได้ (ในออกนอก เพื่อ shadowing) แล้วค่อย global */
Sym *find_var(Gen *g, const char *name) {
    for (int i = g->nvisible - 1; i >= 0; i--) {
        Sym *s = &g->locals[g->visible[i]];
        if (strcmp(s->name, name) == 0) return s;
    }
    for (int i = 0; i < g->nglobals; i++)
        if (strcmp(g->globals[i].name, name) == 0) return &g->globals[i];
    return NULL;
}

/* ลงทะเบียนสตริงในคลัง คืนดัชนี (ใช้เป็นชื่อ label str_<idx>) */
int intern_string(Gen *g, const char *data, int len) {
    if (g->nstrs >= MAX_STR) { fprintf(stderr, "codegen error: too many string literals (max %d)\n", MAX_STR); g->had_error = 1; return 0; }
    int idx = g->nstrs++;
    g->strs[idx].data = (unsigned char *)malloc(len + 1);
    memcpy(g->strs[idx].data, data, len);
    g->strs[idx].data[len] = '\0';
    g->strs[idx].len = len;
    return idx;
}

/* ---------- ระบบชนิดข้อมูล: ขนาด, struct layout, การอนุมานชนิด ---------- */

/* หา layout ของ struct ตามชื่อ */
StructInfo *find_struct(Gen *g, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g->nstructs; i++)
        if (strcmp(g->structs[i].name, name) == 0) return &g->structs[i];
    return NULL;
}

/* ขนาดเป็นไบต์ของชนิด: pointer/str = 8, struct = ขนาดของ layout */
int type_size(Gen *g, DataType base, int ptr, const char *sname) {
    if (ptr > 0) return 8;
    switch (base) {
        case TYPE_I8: case TYPE_U8: case TYPE_BOOL: case TYPE_CHAR: return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        /* f32 เก็บ 4 ไบต์จริง (single-precision); คำนวณ/ส่งผ่านฟังก์ชันใช้ double แล้วแปลงตอน load/store */
        case TYPE_I32: case TYPE_U32: case TYPE_F32: return 4;
        case TYPE_I64: case TYPE_U64: case TYPE_ISIZE: case TYPE_USIZE: case TYPE_F64: return 8;
        case TYPE_I128: case TYPE_U128: return 16;
        case TYPE_STR: return 8;
        case TYPE_FUNC: return 8;   /* function pointer = ที่อยู่ 8 ไบต์ */
        case TYPE_STRUCT: { StructInfo *s = find_struct(g, sname); return s ? s->size : 0; }
        default: return 8;
    }
}

/* ชนิดทศนิยมหรือไม่ */
int is_float_type(DataType t) { return t == TYPE_F32 || t == TYPE_F64; }

/* ชนิดจำนวนเต็มแบบมีเครื่องหมายหรือไม่ (ใช้เลือก movsx/movzx ตอนโหลด) */
int is_signed_type(DataType t) {
    switch (t) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE: return 1;
        default: return 0;
    }
}

/* ลงทะเบียนชื่อ struct + ข้อมูลฟิลด์ (ยังไม่คำนวณ offset/size — ทำใน layout_structs) */
void register_struct(Gen *g, Node *decl) {
    if (g->nstructs >= MAX_FUNC) { fprintf(stderr, "codegen error: too many structs\n"); g->had_error = 1; return; }
    StructInfo *s = &g->structs[g->nstructs++];
    s->name = decl->name;
    s->nfields = 0;
    s->size = 0;
    for (int i = 0; i < decl->nitems; i++) {
        if (s->nfields >= 64) { fprintf(stderr, "codegen error: struct '%s' has too many fields (max 64)\n", decl->name); g->had_error = 1; break; }
        Node *f = decl->items[i];
        Field *fd = &s->fields[s->nfields++];
        fd->name = f->name; fd->type = f->type; fd->ptr = f->ptr; fd->sname = f->type_name;
        fd->sig = f->sig;   /* ลายเซ็น (เฉพาะฟิลด์ที่เป็น function pointer) */
        fd->offset = 0; fd->size = 0;
    }
}

/* DFS ตรวจ struct ที่อ้างถึงตัวเอง "แบบ by-value" (ptr==0) ซึ่งมีขนาดอนันต์ */
static int struct_has_cycle(Gen *g, int si, int *onpath) {
    if (onpath[si]) return 1;
    onpath[si] = 1;
    StructInfo *s = &g->structs[si];
    for (int i = 0; i < s->nfields; i++) {
        if (s->fields[i].type == TYPE_STRUCT && s->fields[i].ptr == 0) {
            StructInfo *fs = find_struct(g, s->fields[i].sname);
            if (fs && struct_has_cycle(g, (int)(fs - g->structs), onpath)) { onpath[si] = 0; return 1; }
        }
    }
    onpath[si] = 0;
    return 0;
}

/* คำนวณ offset/ขนาดของทุก struct แบบ fixpoint (รองรับ struct ที่อ้างถึง struct อื่นซึ่งประกาศทีหลัง) */
void layout_structs(Gen *g) {
    /* ตรวจ struct ที่บรรจุตัวเองแบบ by-value ก่อน (ขนาดอนันต์ → ต้องใช้ pointer; กัน loop ตอน layout/print) */
    int onpath[MAX_FUNC];
    for (int si = 0; si < g->nstructs; si++) {
        memset(onpath, 0, sizeof(onpath));
        if (struct_has_cycle(g, si, onpath)) {
            fprintf(stderr, "codegen error: struct '%s' contains itself by value (infinite size) — use a pointer field (*%s)\n",
                    g->structs[si].name, g->structs[si].name);
            g->had_error = 1;
            return;   /* หยุดก่อน layout เพื่อเลี่ยง recursion ไม่รู้จบ */
        }
    }
    for (int round = 0; round <= g->nstructs; round++) {
        int changed = 0;
        for (int si = 0; si < g->nstructs; si++) {
            StructInfo *s = &g->structs[si];
            int off = 0;
            for (int i = 0; i < s->nfields; i++) {
                int fsize = type_size(g, s->fields[i].type, s->fields[i].ptr, s->fields[i].sname);
                s->fields[i].offset = off;
                s->fields[i].size = fsize;
                off += fsize; /* แบบ packed (ไม่ใส่ padding ระหว่างฟิลด์) */
            }
            int newsize = (off + 7) / 8 * 8; /* ปัดขึ้นเป็นพหุคูณของ 8 */
            if (newsize != s->size) { s->size = newsize; changed = 1; }
        }
        if (!changed) break;
    }
}

/* หาฟิลด์ภายใน struct ตามชื่อ */
Field *find_field(StructInfo *s, const char *name) {
    for (int i = 0; i < s->nfields; i++)
        if (strcmp(s->fields[i].name, name) == 0) return &s->fields[i];
    return NULL;
}

/* ถ้านิพจน์ callee เป็น "ค่า function pointer" (ตัวแปร/ฟิลด์ที่ชนิดเป็น TYPE_FUNC) คืนลายเซ็นของมัน
 * ถ้าเป็นการเรียกฟังก์ชัน/method โดยตรง (ชื่อฟังก์ชัน ไม่ใช่ค่า func-ptr) คืน NULL
 * ใช้แยกแยะ "การเรียกผ่าน function pointer (indirect)" ออกจากการเรียกตรงทั้งใน type_of และ gen_call */
Node *expr_func_sig(Gen *g, Node *callee) {
    if (!callee) return NULL;
    if (callee->kind == ND_IDENT) {
        Sym *s = find_var(g, callee->name);
        if (s && s->type == TYPE_FUNC && s->ptr == 0) return s->sig;  /* ตัวแปร/พารามิเตอร์ที่เป็น func-ptr */
        return NULL;                                                  /* ไม่ใช่ตัวแปร -> ชื่อฟังก์ชันเรียกตรง */
    }
    if (callee->kind == ND_MEMBER) {
        ExprType bt = type_of(g, callee->operand);
        StructInfo *s = find_struct(g, bt.sname);
        if (s) { Field *f = find_field(s, callee->name); if (f && f->type == TYPE_FUNC && f->ptr == 0) return f->sig; }
    }
    return NULL;
}

/* อนุมานชนิด/ขนาด/ความลึก pointer ของนิพจน์ (ลึกพอสำหรับ struct/pointer) */
ExprType type_of(Gen *g, Node *n) {
    ExprType r = { TYPE_I64, 0, NULL, NULL };
    if (!n) return r;
    switch (n->kind) {
        case ND_INT: r.base = TYPE_I64; break;
        case ND_FLOAT: r.base = TYPE_F64; break;
        case ND_CHAR: r.base = TYPE_CHAR; break;
        case ND_BOOL: r.base = TYPE_BOOL; break;
        case ND_STR: r.base = TYPE_STR; break;
        case ND_STRUCT_LIT: r.base = TYPE_STRUCT; r.sname = n->name; break;
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (s) { r.base = s->type; r.ptr = s->ptr; r.sname = s->sname; r.sig = s->sig; }
            else {  /* ไม่ใช่ตัวแปร: อาจเป็น "ชื่อฟังก์ชันใช้เป็นค่า" -> function pointer */
                Node *f = find_func(g, g->cur_ns ? g->cur_ns : "", n->name);
                if (!f) f = find_func(g, "", n->name);
                if (f) { r.base = TYPE_FUNC; r.ptr = 0; r.sig = f; }
            }
            break;
        }
        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *s = find_struct(g, bt.sname);
            if (s) { Field *f = find_field(s, n->name); if (f) { r.base = f->type; r.ptr = f->ptr; r.sname = f->sname; r.sig = f->sig; } }
            break;
        }
        case ND_UNARY:
            if (n->op == TK_AMP)       { r = type_of(g, n->operand); r.ptr++; }
            else if (n->op == TK_STAR) { r = type_of(g, n->operand); if (r.ptr > 0) r.ptr--; }
            else                       { r = type_of(g, n->operand); }
            break;
        case ND_BINARY:
            if (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||
                n->op==TK_LE||n->op==TK_GE||n->op==TK_AND||n->op==TK_OR) { r.base = TYPE_BOOL; break; }
            else {
                ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);
                /* เลขคณิต pointer: ptr±int -> pointer, ptr-ptr -> จำนวน (isize) */
                if (n->op==TK_PLUS || n->op==TK_MINUS) {
                    if (lt.ptr>0 && rt.ptr>0) { r.base=TYPE_ISIZE; break; }
                    if (lt.ptr>0) { r=lt; break; }
                    if (rt.ptr>0) { r=rt; break; }
                }
                /* ผสมทศนิยม: ผลเป็น float (กว้างสุด) — สำคัญต่อการเลือก format/แปลงชนิด */
                int lf = is_float_type(lt.base)&&lt.ptr==0, rf = is_float_type(rt.base)&&rt.ptr==0;
                if (lf || rf) { r.base = (lt.base==TYPE_F64||rt.base==TYPE_F64)?TYPE_F64:TYPE_F32; break; }
                /* int-int: ชนิดผลลัพธ์ = ตัวที่กว้างกว่า (order-independent) */
                r = (type_size(g, rt.base, 0, NULL) > type_size(g, lt.base, 0, NULL)) ? rt : lt;
            }
            break;
        case ND_ASSIGN: r = type_of(g, n->lhs); break;
        case ND_CAST: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_FRAMEREF: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_CALL: {
            Node *callee = n->operand;
            /* การเรียกผ่าน function pointer (indirect): ชนิดผลลัพธ์ = ชนิดคืนค่าของลายเซ็น */
            Node *fsig = expr_func_sig(g, callee);
            if (fsig) { r.base = fsig->type; r.ptr = fsig->ptr; r.sname = fsig->type_name; break; }
            Node *f = NULL;
            if (callee->kind == ND_IDENT) {
                f = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
                if (!f) f = find_func(g, "", callee->name);
            } else if (callee->kind == ND_MEMBER) {
                ExprType bt = type_of(g, callee->operand);
                if (bt.base == TYPE_STRUCT && bt.sname) f = find_func(g, bt.sname, callee->name); /* method */
                if (!f && callee->operand->kind == ND_IDENT) f = find_func(g, callee->operand->name, callee->name);
            }
            if (f) { r.base = f->type; r.ptr = f->ptr; r.sname = f->type_name; }
            break;
        }
        default: break;
    }
    return r;
}

/* อนุมานชนิดพื้นฐาน (ใช้เลือก format specifier ของ io.out) */
DataType infer_type(Gen *g, Node *n) {
    return type_of(g, n).base;
}

/* ต่อสตริง s เข้าบัฟเฟอร์ที่ขยายได้ (อัปเดตค่าที่ out, len, cap ชี้อยู่) */
static void buf_append(char **out, size_t *len, size_t *cap, const char *s) {
    size_t sl = strlen(s);
    while (*len + sl + 2 > *cap) { *cap *= 2; *out = (char *)realloc(*out, *cap); }
    memcpy(*out + *len, s, sl);
    *len += sl;
}

/* เลือก printf specifier ตามชนิด (pointer=address, str=%s, char=%c, float=%f, unsigned=%llu, อื่น ๆ=%lld) */
static const char *spec_for(DataType base, int ptr, const char *user) {
    if (user && (strcmp(user, "x") == 0 || strcmp(user, ":x") == 0)) return "%llx";
    if (ptr > 0) return "%llu";                 /* pointer = ที่อยู่ */
    if (base == TYPE_STR) return "%s";
    if (base == TYPE_CHAR) return "%c";
    if (is_float_type(base)) return "%f";
    if (base==TYPE_U8||base==TYPE_U16||base==TYPE_U32||base==TYPE_U64||base==TYPE_U128||base==TYPE_USIZE) return "%llu";
    return "%lld";
}

/* ขยาย struct เป็นรูป "Name { f1: <spec>, f2: <spec> }" (แบบ {:?} ของ Rust)
 * พร้อมเก็บโหนดเข้าถึงสมาชิก (base.field) ลง vals เพื่อส่งให้ printf */
static void expand_struct(Gen *g, Node *base, const char *sname,
                          char **out, size_t *len, size_t *cap,
                          Node **vals, int *nv, int vals_cap, int depth) {
    StructInfo *s = find_struct(g, sname);
    if (!s) { buf_append(out, len, cap, "?"); return; }
    if (depth > 32) { buf_append(out, len, cap, "..."); return; }  /* กัน recursion ลึกผิดปกติ */
    /* ถ้า base เป็นผลลัพธ์ struct จากฟังก์ชัน (ND_CALL) ให้ชี้ไปที่ "ช่อง temp" ที่จองไว้แทน
     * (อ่านสมาชิกจาก temp เดียวกัน) ไม่งั้นแต่ละฟิลด์จะ gen_call ซ้ำ -> เรียกฟังก์ชันหลายรอบ/ค่าผิด */
    if (base->kind == ND_CALL && base->int_val) {
        Node *fr = node_new(ND_FRAMEREF, base->line);
        fr->int_val = base->int_val;
        fr->type = TYPE_STRUCT; fr->type_name = strdup(sname);
        base = fr;
    }
    buf_append(out, len, cap, sname);
    buf_append(out, len, cap, " { ");
    for (int i = 0; i < s->nfields; i++) {
        if (i) buf_append(out, len, cap, ", ");
        buf_append(out, len, cap, s->fields[i].name);
        buf_append(out, len, cap, ": ");
        Node *m = node_new(ND_MEMBER, base->line);  /* โหนด base.field */
        m->operand = base; m->name = strdup(s->fields[i].name);
        if (s->fields[i].type == TYPE_STRUCT && s->fields[i].ptr == 0) {
            expand_struct(g, m, s->fields[i].sname, out, len, cap, vals, nv, vals_cap, depth + 1); /* ซ้อน */
        } else if (*nv < vals_cap) {
            buf_append(out, len, cap, spec_for(s->fields[i].type, s->fields[i].ptr, NULL));
            vals[(*nv)++] = m;
        } else {
            buf_append(out, len, cap, "?"); /* เกินขีดจำกัด vals — ใส่ literal เพื่อให้ specifier ตรงกับจำนวน arg */
        }
    }
    buf_append(out, len, cap, " }");
}

/* แปลง format string ของ MVS (รูปแบบ {} แบบ Rust) เป็น format ของ C printf
 *   {} / {:?}  -> เลือก specifier ตามชนิด argument; ถ้าเป็น struct จะขยายเป็น "Name { ... }"
 *   {:x}       -> เลขฐานสิบหก ; {{ }} -> { } ; % -> %% ; ต่อท้าย \n อัตโนมัติ
 * คืนบัฟเฟอร์ใหม่ (malloc); ตั้ง *out_len, *out_nph (จำนวน placeholder ระดับ MVS),
 * และเติม vals[] (ค่าที่ต้องส่งให้ printf จริง — struct ถูกขยายเป็นหลายฟิลด์) พร้อม *out_nv */
char *build_c_format(Gen *g, Node *call, const char *fmt, int *out_len, int *out_nph,
                     Node **vals, int *out_nv, int vals_cap) {
    size_t cap = strlen(fmt) * 2 + 32, len = 0;
    char *out = (char *)malloc(cap);
    int argi = 1, ph = 0, nv = 0;
    for (const char *p = fmt; *p; p++) {
        if (len + 8 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        if (p[0] == '{' && p[1] == '{') { out[len++] = '{'; p++; continue; }
        if (p[0] == '}' && p[1] == '}') { out[len++] = '}'; p++; continue; }
        if (p[0] == '{') {
            char spec[16]; int si = 0;
            p++;
            while (*p && *p != '}') { if (si < 15) spec[si++] = *p; p++; }
            spec[si] = '\0';
            if (*p == '\0') break;   /* '{' ไม่ปิด: กันอ่านเกิน buffer */
            Node *arg = (argi < call->nitems) ? call->items[argi] : NULL;
            ExprType at = { TYPE_I64, 0, NULL, NULL };
            if (arg) at = type_of(g, arg);
            int is_hex = (strcmp(spec, "x") == 0 || strcmp(spec, ":x") == 0);
            if (arg && at.base == TYPE_STRUCT && at.ptr == 0 && !is_hex) {
                expand_struct(g, arg, at.sname, &out, &len, &cap, vals, &nv, vals_cap, 0);
            } else {
                buf_append(&out, &len, &cap, spec_for(at.base, at.ptr, spec));
                if (arg && nv < vals_cap) vals[nv++] = arg;
            }
            argi++; ph++;
            continue;
        }
        if (p[0] == '%') { out[len++] = '%'; out[len++] = '%'; continue; }
        out[len++] = p[0];
    }
    out[len++] = '\n'; /* ขึ้นบรรทัดใหม่อัตโนมัติ */
    *out_len = (int)len;
    *out_nph = ph;
    *out_nv = nv;
    return out;
}

/* ---------- การจองพื้นที่ตัวแปรบน stack ---------- */

/* เพิ่มตัวแปร local พร้อมจัดสรร offset (จองเป็นพหุคูณของ 8 ไบต์ ตามขนาดจริง) คืน offset */
int add_local(Gen *g, const char *name, DataType type, int ptr, char *sname, Node *sig, int *frame) {
    if (g->nlocals >= MAX_SYM) { fprintf(stderr, "codegen error: too many local variables (max %d)\n", MAX_SYM); g->had_error = 1; return 0; }
    int size = type_size(g, type, ptr, sname);
    int slot = (size + 7) / 8 * 8;       /* จองพื้นที่อย่างน้อย 8 ไบต์ และเป็นพหุคูณของ 8 */
    *frame += slot;
    Sym *s = &g->locals[g->nlocals++];
    s->name = (char *)name;
    s->type = type;
    s->ptr = ptr;
    s->sname = sname;
    s->size = size;
    s->is_global = 0;
    s->sig = sig;                        /* ลายเซ็น (เฉพาะ function pointer) */
    s->offset = *frame;                  /* ตัวแปรอยู่ที่ [rbp - offset .. rbp - offset + size) */
    return s->offset;
}

/* เดินสำรวจ body ของฟังก์ชันเพื่อจองพื้นที่ให้ทุกการประกาศตัวแปร (pre-pass)
 * ทำให้รู้ขนาดเฟรมทั้งหมดก่อนเริ่ม gen โค้ดจริง */
void collect_locals(Gen *g, Node *n, int *frame) {
    if (!n) return;
    switch (n->kind) {
        case ND_VAR_DECL:
            n->int_val = g->nlocals;   /* จดดัชนี slot ของตัวแปรนี้ ใช้ตอน gen + scope */
            add_local(g, n->name, n->type, n->ptr, n->type_name, n->sig, frame);
            break;
        case ND_BLOCK:
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        case ND_IF:
            collect_locals(g, n->then_branch, frame);
            collect_locals(g, n->else_branch, frame);
            break;
        case ND_WHILE:
            collect_locals(g, n->body, frame);
            break;
        case ND_FOR:
            collect_locals(g, n->init, frame); /* ตัวแปรใน for-init */
            collect_locals(g, n->body, frame);
            break;
        case ND_DOWHILE:
            collect_locals(g, n->body, frame);
            break;
        case ND_SWITCH:
            /* จองช่องเก็บค่าที่ใช้เทียบของ switch ไว้บน frame (เก็บ offset ไว้ใน int_val)
             * เพื่อไม่ต้องคงค่าไว้บน temp stack — ทำให้ออกจาก switch ทุกทาง (break/continue) ปลอดภัย */
            n->int_val = add_local(g, "$switch", TYPE_I64, 0, NULL, NULL, frame);
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        case ND_CASE:
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        default: break;
    }
}

/* เดินทั้งต้นไม้ (รวมนิพจน์) จองช่อง temp ให้ทุกการเรียกฟังก์ชันที่ "คืน struct"
 * เก็บ offset ไว้ใน int_val ของโหนด ND_CALL — ใช้ตอนผลลัพธ์ struct ถูกใช้เป็น rvalue
 * (เช่น เป็นอาร์กิวเมนต์ หรือฐานของ .field) ที่ต้องมีที่อยู่จริงให้ผลลัพธ์ */
void collect_struct_temps(Gen *g, Node *n, int *frame) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        ExprType rt = type_of(g, n);
        if (rt.base == TYPE_STRUCT && rt.ptr == 0)
            n->int_val = add_local(g, "$tmp", TYPE_STRUCT, 0, rt.sname, NULL, frame);
    }
    collect_struct_temps(g, n->lhs, frame);   collect_struct_temps(g, n->rhs, frame);
    collect_struct_temps(g, n->operand, frame); collect_struct_temps(g, n->cond, frame);
    collect_struct_temps(g, n->then_branch, frame); collect_struct_temps(g, n->else_branch, frame);
    collect_struct_temps(g, n->init, frame);  collect_struct_temps(g, n->step, frame);
    collect_struct_temps(g, n->body, frame);
    for (int i = 0; i < n->nitems; i++) collect_struct_temps(g, n->items[i], frame);
}

/* ---------- การวิเคราะห์การเข้าถึงได้ (reachability) สำหรับตัดฟังก์ชันที่ไม่ถูกใช้ ---------- */

/* หาดัชนีของฟังก์ชันในตาราง funcs (คืน -1 ถ้าไม่พบ) */
int func_index(Gen *g, Node *f) {
    if (!f) return -1;
    for (int i = 0; i < g->nfuncs; i++) if (g->funcs[i] == f) return i;
    return -1;
}

void reach_node(Gen *g, Node *n, const char *ns, char *reached);

/* มาร์กฟังก์ชันว่าเข้าถึงได้ แล้วไล่สแกน body หาการเรียกต่อ (DFS, กันวนด้วย reached) */
void reach_func(Gen *g, int idx, char *reached) {
    if (idx < 0 || reached[idx]) return;
    reached[idx] = 1;
    Node *f = g->funcs[idx];
    reach_node(g, f->body, f->mod ? f->mod : "", reached);
}

/* สแกนโหนดหาการเรียกฟังก์ชัน แล้วมาร์กเป้าหมายว่าเข้าถึงได้ (เดินลูกทุกตัวแบบ recursive) */
void reach_node(Gen *g, Node *n, const char *ns, char *reached) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        Node *callee = n->operand;
        if (callee->kind == ND_MEMBER && callee->operand->kind == ND_IDENT &&
            strcmp(callee->operand->name, "io") == 0 && strcmp(callee->name, "out") == 0) {
            reach_func(g, func_index(g, find_func(g, "", "printf")), reached); /* io.out ใช้ printf */
        } else if (callee->kind == ND_IDENT) {
            Node *t = find_func(g, ns, callee->name);
            if (!t) t = find_func(g, "", callee->name);
            reach_func(g, func_index(g, t), reached);
        } else if (callee->kind == ND_MEMBER) {
            /* การเรียกผ่าน . อาจเป็น namespace ของโมดูล หรือ method ของ struct
             * (ระยะ reachability ยังไม่มีข้อมูลชนิดของตัวแปร จึง over-approximate:
             *  มาร์ก method ทุกตัวที่ชื่อตรงกัน รวมถึงฟังก์ชันใน namespace ของ base) */
            if (callee->operand->kind == ND_IDENT)
                reach_func(g, func_index(g, find_func(g, callee->operand->name, callee->name)), reached);
            for (int k = 0; k < g->nfuncs; k++)
                if (g->funcs[k]->is_method && strcmp(g->funcs[k]->name, callee->name) == 0)
                    reach_func(g, k, reached);
        }
    }
    /* ชื่อฟังก์ชันที่ถูกใช้เป็น "ค่า" (function pointer) เช่น `let h = handler;` หรือส่งเป็น argument
     * ต้องถือว่าเข้าถึงได้ด้วย ไม่งั้น tree-shaking ตัดทิ้งแล้ว lea อ้าง label ที่ไม่มี */
    if (n->kind == ND_IDENT) {
        Node *t = find_func(g, ns, n->name);
        if (!t) t = find_func(g, "", n->name);
        if (t) reach_func(g, func_index(g, t), reached);
    }
    reach_node(g, n->lhs, ns, reached);
    reach_node(g, n->rhs, ns, reached);
    reach_node(g, n->operand, ns, reached);
    reach_node(g, n->cond, ns, reached);
    reach_node(g, n->then_branch, ns, reached);
    reach_node(g, n->else_branch, ns, reached);
    reach_node(g, n->init, ns, reached);
    reach_node(g, n->step, ns, reached);
    reach_node(g, n->body, ns, reached);
    for (int i = 0; i < n->nitems; i++) reach_node(g, n->items[i], ns, reached);
}
