/*
 * generic.c - monomorphization ของ generic function (ดูภาพรวมใน generic.h)
 *
 * อัลกอริทึม:
 *   1. รวบรวม generic template (ngen > 0) จากโปรแกรม
 *   2. สแกนทุกฟังก์ชัน concrete หาจุดเรียก generic
 *   3. อนุมานชนิดจริงจาก argument -> สร้าง instance (clone + แทนชนิด) ถ้ายังไม่มี
 *   4. เปลี่ยนชื่อที่จุดเรียกให้ชี้ instance นั้น
 *   5. instance ใหม่ก็ถูกสแกนต่อ (รองรับ generic เรียก generic) จนไม่มีของใหม่
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generic.h"

/* ชนิดจริงที่อนุมานได้ (base + ความลึก pointer + ชื่อ struct + ลายเซ็นถ้าเป็น function pointer) */
typedef struct { DataType base; int ptr; char *sname; Node *sig; } CType;

/* รายการแมป ชื่อ -> ชนิด (ใช้เป็น var-type map ต่อฟังก์ชัน และ generic-param map) */
typedef struct { char *name; CType t; } Bind;

/* ---------- ตัวช่วยค้นหาในโปรแกรม ---------- */

/* ตรวจว่า type (ชื่อ struct) impl trait ที่ระบุหรือไม่ (มีเครื่องหมาย ND_TRAIT_IMPL) */
static int type_impls_trait(Node *prog, const char *sname, const char *trait) {
    if (!sname || !trait) return 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_TRAIT_IMPL && d->name && d->type_name &&
            strcmp(d->name, trait) == 0 && strcmp(d->type_name, sname) == 0) return 1;
    }
    return 0;
}

static int mono_err; /* จำนวน error จาก trait bound ที่ละเมิด (รีเซ็ตใน monomorphize) */

/* หา trait declaration ตามชื่อ */
static Node *find_trait(Node *prog, const char *name) {
    for (int i = 0; i < prog->nitems; i++)
        if (prog->items[i]->kind == ND_TRAIT && strcmp(prog->items[i]->name, name) == 0) return prog->items[i];
    return NULL;
}

/* มีฟังก์ชัน (method/associated) ชื่อ mname ที่อยู่ใน namespace = type หรือไม่ */
static int type_has_method(Node *prog, const char *type, const char *mname) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->ns && strcmp(f->ns, type) == 0 && strcmp(f->name, mname) == 0) return 1;
    }
    return 0;
}

/* ตรวจทุก `impl Trait for Type`: trait ต้องมีจริง และ Type ต้องมีครบทุก method ของ trait */
static void check_trait_impls(Node *prog) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_TRAIT_IMPL) continue;
        Node *tr = find_trait(prog, d->name);
        if (!tr) {
            fprintf(stderr, "type error: unknown trait '%s' in impl for '%s'\n", d->name, d->type_name);
            mono_err++; continue;
        }
        for (int j = 0; j < tr->nitems; j++) {
            Node *sig = tr->items[j];
            if (sig->kind != ND_FUNC) continue;
            if (!type_has_method(prog, d->type_name, sig->name)) {
                fprintf(stderr, "type error: type '%s' is missing method '%s' required by trait '%s'\n",
                        d->type_name, sig->name, d->name);
                mono_err++;
            }
        }
    }
}

/* หา generic template ตามชื่อ (ngen > 0) */
static Node *find_template(Node *prog, const char *name) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_FUNC && d->ngen > 0 && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* หา ND_FUNC ตามชื่อ (ไม่ว่า generic หรือไม่) คืนชนิดที่คืนค่า */
static int func_ret_type(Node *prog, const char *name, CType *out) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_FUNC && strcmp(d->name, name) == 0) {
            out->base = d->type; out->ptr = d->ptr; out->sname = d->type_name; out->sig = NULL; return 1;
        }
    }
    return 0;
}

/* หาชนิดของฟิลด์ใน struct (ใช้อนุมาน a.b) */
static int struct_field_type(Node *prog, const char *sname, const char *field, CType *out) {
    if (!sname) return 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_STRUCT_DECL && strcmp(d->name, sname) == 0) {
            for (int j = 0; j < d->nitems; j++) {
                Node *f = d->items[j];
                if (strcmp(f->name, field) == 0) {
                    out->base = f->type; out->ptr = f->ptr; out->sname = f->type_name; out->sig = f->sig; return 1;
                }
            }
        }
    }
    return 0;
}

/* อันดับความกว้างของชนิดจำนวนเต็ม (ไบต์) ใช้เลื่อนชนิดผลลัพธ์ของเลขคณิตให้ order-independent */
static int int_rank(DataType t) {
    switch (t) {
        case TYPE_I8: case TYPE_U8: case TYPE_BOOL: case TYPE_CHAR: return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        case TYPE_I32: case TYPE_U32: return 4;
        case TYPE_I64: case TYPE_U64: case TYPE_ISIZE: case TYPE_USIZE: return 8;
        case TYPE_I128: case TYPE_U128: return 16;
        default: return 8;
    }
}

/* ---------- การอนุมานชนิดของนิพจน์ (ใช้ var-type map ที่สร้างจากชนิดที่ประกาศไว้) ---------- */

static CType infer(Node *prog, Node *n, Bind *map, int nmap) {
    CType r = { TYPE_I64, 0, NULL, NULL };
    if (!n) return r;
    switch (n->kind) {
        case ND_INT: r.base = TYPE_I64; break;
        case ND_FLOAT: r.base = TYPE_F64; break;
        case ND_CHAR: r.base = TYPE_CHAR; break;
        case ND_BOOL: r.base = TYPE_BOOL; break;
        case ND_STR: r.base = TYPE_STR; break;
        case ND_STRUCT_LIT: r.base = TYPE_STRUCT; r.sname = n->name; break;
        case ND_IDENT:
            for (int i = nmap - 1; i >= 0; i--)
                if (strcmp(map[i].name, n->name) == 0) return map[i].t;
            /* ไม่ใช่ตัวแปร: อาจเป็น "ชื่อฟังก์ชันใช้เป็นค่า" -> function pointer (ลายเซ็น = โหนดฟังก์ชัน) */
            for (int i = 0; i < prog->nitems; i++) {
                Node *d = prog->items[i];
                if (d->kind == ND_FUNC && !d->is_method && d->ngen == 0 && d->name && strcmp(d->name, n->name) == 0) {
                    r.base = TYPE_FUNC; r.sig = d; return r;
                }
            }
            break;
        case ND_MEMBER: {
            CType bt = infer(prog, n->operand, map, nmap);
            CType ft; if (struct_field_type(prog, bt.sname, n->name, &ft)) return ft;
            break;
        }
        case ND_UNARY:
            if (n->op == TK_AMP)       { r = infer(prog, n->operand, map, nmap); r.ptr++; }
            else if (n->op == TK_STAR) { r = infer(prog, n->operand, map, nmap); if (r.ptr > 0) r.ptr--; }
            else                       { r = infer(prog, n->operand, map, nmap); }
            break;
        case ND_BINARY:
            if (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||n->op==TK_LE||n->op==TK_GE||
                n->op==TK_AND||n->op==TK_OR) { r.base = TYPE_BOOL; break; }
            else {
                CType lt = infer(prog, n->lhs, map, nmap), rt = infer(prog, n->rhs, map, nmap);
                if (n->op==TK_PLUS || n->op==TK_MINUS) {
                    if (lt.ptr>0 && rt.ptr>0) { r.base=TYPE_ISIZE; break; }  /* ptr-ptr -> count */
                    if (lt.ptr>0) { r=lt; break; }
                    if (rt.ptr>0) { r=rt; break; }                          /* int+ptr -> ptr */
                }
                int lf = (lt.base==TYPE_F32||lt.base==TYPE_F64)&&lt.ptr==0;
                int rf = (rt.base==TYPE_F32||rt.base==TYPE_F64)&&rt.ptr==0;
                if (lf || rf) { r.base = (lt.base==TYPE_F64||rt.base==TYPE_F64)?TYPE_F64:TYPE_F32; break; }
                /* int-int: เลื่อนเป็นชนิดที่กว้างกว่า (order-independent) เพื่อให้เลือก overload เหมือนกันไม่ว่าลำดับ */
                r = (int_rank(rt.base) > int_rank(lt.base)) ? rt : lt;
            }
            break;
        case ND_ASSIGN: r = infer(prog, n->lhs, map, nmap); break;
        case ND_CAST: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_CALL: {
            Node *callee = n->operand;
            if (!callee) break;
            /* การเรียกผ่าน function pointer (indirect): callee เป็นค่าชนิด TYPE_FUNC
             * (ตัวแปร/พารามิเตอร์ หรือ ฟิลด์ struct) -> ชนิดผลลัพธ์ = ชนิดคืนค่าของลายเซ็น */
            {
                CType cc = infer(prog, callee, map, nmap);
                if (cc.base == TYPE_FUNC && cc.sig) {
                    CType ct = { cc.sig->type, cc.sig->ptr, cc.sig->type_name, NULL };
                    return ct;
                }
            }
            if (callee->kind == ND_IDENT) {              /* f(...) เรียกตรง */
                CType ct; if (func_ret_type(prog, callee->name, &ct)) return ct;
            } else if (callee->kind == ND_MEMBER) {      /* ns.f(...) หรือ obj.method(...) */
                /* หา "ขอบเขต" (scope): ถ้า base เป็น struct ใช้ชื่อ struct (method),
                 * ถ้า base เป็นชื่อเปล่าใช้เป็น namespace (เช่น net) — เพื่อเลือกฟังก์ชันที่ ns ตรง
                 * ก่อน (กันชนชื่อกับ extern C เช่น method accept กับ Winsock accept) */
                const char *scope = NULL;
                CType bt = infer(prog, callee->operand, map, nmap);
                if (bt.base == TYPE_STRUCT && bt.sname) scope = bt.sname;
                else if (callee->operand->kind == ND_IDENT) scope = callee->operand->name;
                Node *best = NULL;
                for (int i = 0; i < prog->nitems; i++) {
                    Node *f = prog->items[i];
                    if (f->kind != ND_FUNC || strcmp(f->name, callee->name) != 0) continue;
                    if (scope && f->ns && strcmp(f->ns, scope) == 0) { best = f; break; }
                    if (!best) best = f;
                }
                if (best) { CType ct = { best->type, best->ptr, best->type_name, NULL }; return ct; }
            }
            break;
        }
        default: break;
    }
    return r;
}

/* เพิ่มตัวแปร/พารามิเตอร์เข้า var-type map (scope stack) — มองเห็นได้หลังจุดประกาศ
 * ใช้ร่วมกันทั้ง monomorphize/overload/typecheck เพื่อให้อนุมานชนิดถูกเมื่อมี shadowing */
static void add_bind(Bind *map, int *nmap, Node *d) {
    if (*nmap >= 512 || !d->name) return;
    map[*nmap].name = d->name;
    map[*nmap].t.base = d->type; map[*nmap].t.ptr = d->ptr; map[*nmap].t.sname = d->type_name; map[*nmap].t.sig = d->sig;
    (*nmap)++;
}

/* ใส่ตัวแปร global (top-level let/const) ลง scope ฐาน เพื่อให้อนุมานชนิดของ global ถูก
 * (ต้องเรียกก่อนใส่ params — params/locals จะ shadow ทับได้ถูกต้อง) */
static void seed_globals(Node *prog, Bind *map, int *nmap) {
    for (int i = 0; i < prog->nitems; i++)
        if (prog->items[i]->kind == ND_VAR_DECL) add_bind(map, nmap, prog->items[i]);
}

/* ---------- การแทนชนิดและตั้งชื่อ instance ---------- */

/* เขียนรหัสชนิดสำหรับตั้งชื่อ instance (เช่น i32, pi32, Foo) */
static void type_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    const char *base = t.base == TYPE_STRUCT && t.sname ? t.sname : datatype_name(t.base);
    strcpy(p, base);
}

/* แทน generic type parameter ในทุกโหนดของ instance ด้วยชนิดจริง */
static void substitute(Node *n, Bind *gmap, int ngmap) {
    if (!n) return;
    if (n->type == TYPE_STRUCT && n->type_name) {
        for (int i = 0; i < ngmap; i++) {
            if (strcmp(n->type_name, gmap[i].name) == 0) {
                n->type = gmap[i].t.base;
                n->ptr += gmap[i].t.ptr;                 /* *T + (T=*i32) -> **i32 */
                free(n->type_name);
                n->type_name = gmap[i].t.sname ? strdup(gmap[i].t.sname) : NULL;
                /* ถ้า T ผูกกับ "ค่า function" (TYPE_FUNC) ต้องพาลายเซ็นมาด้วย ไม่งั้นเรียก f(...) ไม่ได้
                 * clone กันแก้ไขโครงสร้างที่ใช้ร่วม (ลายเซ็นของฟังก์ชันจริงไม่มี generic param อยู่แล้ว) */
                if (n->type == TYPE_FUNC) n->sig = gmap[i].t.sig ? node_clone(gmap[i].t.sig) : NULL;
                break;
            }
        }
    }
    substitute(n->lhs, gmap, ngmap);   substitute(n->rhs, gmap, ngmap);
    substitute(n->operand, gmap, ngmap); substitute(n->cond, gmap, ngmap);
    substitute(n->then_branch, gmap, ngmap); substitute(n->else_branch, gmap, ngmap);
    substitute(n->init, gmap, ngmap);  substitute(n->step, gmap, ngmap);
    substitute(n->body, gmap, ngmap);
    substitute(n->sig, gmap, ngmap);   /* generic param ที่อยู่ในลายเซ็น func-ptr เช่น f: func(T) -> T */
    for (int i = 0; i < n->nitems; i++) substitute(n->items[i], gmap, ngmap);
}

/* ---------- ตัวขับหลัก ---------- */

/* สแกนโหนดในฟังก์ชัน concrete หาการเรียก generic แล้ว instantiate + เปลี่ยนชื่อจุดเรียก
 * scope-aware (map/nmap โตตามการประกาศ) เพื่ออนุมานชนิด argument ถูกเมื่อมี shadowing
 * คืนจำนวน instance ใหม่ที่สร้าง (ใช้ตรวจ fixpoint) */
static int scan_calls(Node *prog, Node *n, Bind *map, int *nmap, int *made) {
    if (!n) return *made;
    switch (n->kind) {                          /* จัดการ scope ก่อน */
        case ND_BLOCK: { int s = *nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        case ND_VAR_DECL: { scan_calls(prog,n->operand,map,nmap,made); add_bind(map,nmap,n); return *made; }
        case ND_FOR: { int s=*nmap; scan_calls(prog,n->init,map,nmap,made); scan_calls(prog,n->cond,map,nmap,made); scan_calls(prog,n->step,map,nmap,made); scan_calls(prog,n->body,map,nmap,made); *nmap=s; return *made; }
        case ND_IF: { scan_calls(prog,n->cond,map,nmap,made); int s1=*nmap; scan_calls(prog,n->then_branch,map,nmap,made); *nmap=s1; int s2=*nmap; scan_calls(prog,n->else_branch,map,nmap,made); *nmap=s2; return *made; }
        case ND_WHILE: case ND_DOWHILE: { scan_calls(prog,n->cond,map,nmap,made); int s=*nmap; scan_calls(prog,n->body,map,nmap,made); *nmap=s; return *made; }
        case ND_SWITCH: { scan_calls(prog,n->cond,map,nmap,made); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        case ND_CASE: { scan_calls(prog,n->operand,map,nmap,made); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        default: break;
    }
    /* เดินลูกก่อน (children-first) — สำคัญสำหรับ generic ซ้อน เช่น f(g(x)):
     * inner call ต้องถูก instantiate+rename เป็น instance concrete ก่อน outer จะอนุมานชนิด argument */
    scan_calls(prog, n->lhs, map, nmap, made);   scan_calls(prog, n->rhs, map, nmap, made);
    scan_calls(prog, n->operand, map, nmap, made); scan_calls(prog, n->cond, map, nmap, made);
    scan_calls(prog, n->then_branch, map, nmap, made); scan_calls(prog, n->else_branch, map, nmap, made);
    scan_calls(prog, n->init, map, nmap, made);  scan_calls(prog, n->step, map, nmap, made);
    scan_calls(prog, n->body, map, nmap, made);
    for (int i = 0; i < n->nitems; i++) scan_calls(prog, n->items[i], map, nmap, made);

    /* การเรียก generic: ทั้ง f(...) (ND_IDENT) และ ns.f(...) (ND_MEMBER ที่ base เป็น namespace ไม่ใช่ struct)
     * — เพื่อให้ generic helper ใน std เรียกแบบ module.func ได้ (กันชนกับ method โดยเช็คว่า base ไม่ใช่ struct) */
    int is_gcall = 0;
    if (n->kind == ND_CALL && n->operand) {
        if (n->operand->kind == ND_IDENT) is_gcall = 1;
        else if (n->operand->kind == ND_MEMBER) {
            CType bt = infer(prog, n->operand->operand, map, *nmap);
            if (bt.base != TYPE_STRUCT) is_gcall = 1; /* ns.f(...) ไม่ใช่ obj.method() */
        }
    }
    if (is_gcall) {
        Node *tmpl = find_template(prog, n->operand->name);
        if (tmpl) {
            /* อนุมานชนิดของ generic parameter แต่ละตัวจาก argument */
            Bind gmap[4];
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                CType concrete = { TYPE_I64, 0, NULL, NULL };
                /* หา parameter ตัวแรกที่ชนิดอ้างถึง generic param นี้ */
                for (int pi = 0; pi < tmpl->nitems && pi < n->nitems; pi++) {
                    Node *pp = tmpl->items[pi];
                    if (pp->type == TYPE_STRUCT && pp->type_name &&
                        strcmp(pp->type_name, tmpl->gen[gi]) == 0) {
                        CType at = infer(prog, n->items[pi], map, *nmap);
                        concrete.base = at.base; concrete.sname = at.sname; concrete.sig = at.sig;
                        concrete.ptr = at.ptr - pp->ptr; if (concrete.ptr < 0) concrete.ptr = 0;
                        break;
                    }
                }
                gmap[gi].name = tmpl->gen[gi]; gmap[gi].t = concrete;
            }
            /* ตรวจ trait bound: ถ้า <T: Trait> ชนิดจริงต้อง impl trait นั้น (เป็น struct ที่มีเครื่องหมาย) */
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                if (!tmpl->gen_bound[gi]) continue;
                CType ct = gmap[gi].t;
                if (!(ct.base == TYPE_STRUCT && ct.sname && type_impls_trait(prog, ct.sname, tmpl->gen_bound[gi]))) {
                    const char *tn = (ct.base == TYPE_STRUCT && ct.sname) ? ct.sname : datatype_name(ct.base);
                    fprintf(stderr, "type error: type '%s' does not implement trait '%s' (required by %s<%s: %s>)\n",
                            tn, tmpl->gen_bound[gi], tmpl->name, tmpl->gen[gi], tmpl->gen_bound[gi]);
                    mono_err++;
                }
            }
            /* ตั้งชื่อ instance จากรหัสชนิด */
            char mangled[600]; size_t ml = 0;   /* ชื่อ instance (ชื่อ template + รหัสชนิด) — มีขอบเขต */
            ml = snprintf(mangled, sizeof(mangled), "%s", tmpl->name);
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                char code[128]; type_code(gmap[gi].t, code);
                ml += snprintf(mangled + ml, sizeof(mangled) - ml, "__%s", code);
                if (ml >= sizeof(mangled)) { ml = sizeof(mangled) - 1; break; }
            }
            /* สร้าง instance ถ้ายังไม่มี (dedup ด้วยการค้นชื่อ mangled ในโปรแกรม) */
            {
                int exists = 0;
                for (int i = 0; i < prog->nitems; i++)
                    if (prog->items[i]->kind == ND_FUNC && strcmp(prog->items[i]->name, mangled) == 0) { exists = 1; break; }
                if (!exists) {
                    Node *inst = node_clone(tmpl);
                    free(inst->name); inst->name = strdup(mangled);
                    inst->ngen = 0;                 /* concrete แล้ว */
                    substitute(inst, gmap, tmpl->ngen);
                    node_add_item(prog, inst);
                    (*made)++;
                }
            }
            /* เปลี่ยนจุดเรียกให้ชี้ instance */
            free(n->operand->name); n->operand->name = strdup(mangled);
        }
    }
    return *made;
}

/* ---------- การแก้ฟังก์ชัน overload (ชื่อซ้ำต่างชนิดพารามิเตอร์) ---------- */

/* รหัส "หมวด" ของชนิด (int/float/str/char/bool/pointer/struct) — ใช้เป็น fallback ตอนจับคู่
 * pointer นำหน้าด้วย 'p' ตามความลึก แล้วตามด้วยหมวดของ pointee (กันชนระหว่าง *i32 กับ *Point) */
static void cat_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
            strcpy(p, "i"); break;
        case TYPE_F32: case TYPE_F64: strcpy(p, "f"); break;
        case TYPE_STR:  strcpy(p, "s"); break;
        case TYPE_CHAR: strcpy(p, "c"); break;
        case TYPE_BOOL: strcpy(p, "b"); break;
        case TYPE_STRUCT: strcpy(p, t.sname ? t.sname : "S"); break;
        default: strcpy(p, "x");
    }
}

/* รหัสชนิด "ละเอียด" แยกความกว้าง int/float และ "ความลึก+ชนิด pointee" — ใช้จับคู่ overload แบบเป๊ะ
 * เช่น *i32 -> "pi32", *u8 -> "pu8", **i32 -> "ppi32" (กันชน/false-duplicate ของ pointer overload) */
static void width_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    switch (t.base) {
        case TYPE_STR:    strcpy(p, "s"); break;
        case TYPE_CHAR:   strcpy(p, "c"); break;
        case TYPE_BOOL:   strcpy(p, "b"); break;
        case TYPE_STRUCT: strcpy(p, t.sname ? t.sname : "S"); break;
        default:          strcpy(p, datatype_name(t.base)); /* i8..i64, u8..u64, isize/usize, f32, f64 */
    }
}

#define SIGCAP 256  /* ขนาดบัฟเฟอร์ signature (กันล้นเมื่อมีพารามิเตอร์/ชื่อ struct จำนวนมาก) */

/* ต่อรหัสชนิดเข้า buf แบบมีขอบเขต (cap=SIGCAP) — คืนความยาวใหม่ */
static void sig_append(char *buf, size_t *len, int first, const char *code) {
    size_t add = (first ? 0 : 1) + strlen(code);
    if (*len + add + 1 >= SIGCAP) return;   /* เต็ม: หยุด (ไม่ล้น) */
    if (!first) buf[(*len)++] = '_';
    strcpy(buf + *len, code); *len += strlen(code);
}

/* สร้าง signature ของฟังก์ชันจากชนิดพารามิเตอร์ (exact=1 -> width_code, 0 -> cat_code) */
static void sig_func(Node *fn, char *buf, int exact) {
    buf[0] = '\0';
    if (fn->nitems == 0) { strcpy(buf, "void"); return; }
    size_t len = 0;
    for (int i = 0; i < fn->nitems; i++) {
        CType t = { fn->items[i]->type, fn->items[i]->ptr, fn->items[i]->type_name, NULL };
        char c[128]; if (exact) width_code(t, c); else cat_code(t, c);
        sig_append(buf, &len, i == 0, c);
    }
}

/* สร้าง signature ของจุดเรียกจากชนิด argument (อนุมานด้วย var-type map) */
static void sig_call(Node *prog, Node *call, Bind *map, int nmap, char *buf, int exact) {
    buf[0] = '\0';
    if (call->nitems == 0) { strcpy(buf, "void"); return; }
    size_t len = 0;
    for (int i = 0; i < call->nitems; i++) {
        CType t = infer(prog, call->items[i], map, nmap);
        char c[128]; if (exact) width_code(t, c); else cat_code(t, c);
        sig_append(buf, &len, i == 0, c);
    }
}

/* กลุ่มฟังก์ชันที่ชื่อซ้ำกัน (overload set): sig = ละเอียด (exact), catsig = หมวด (fallback) */
typedef struct { char *orig; Node *defs[16]; char sig[16][SIGCAP]; char catsig[16][SIGCAP]; int n; } OvSet;

/* ตรวจว่าฟังก์ชันนี้เข้าเกณฑ์ overload หรือไม่ (ฟังก์ชันธรรมดาระดับ global) */
static int ov_eligible(Node *f) {
    return f->kind == ND_FUNC && f->body && f->ngen == 0 &&
           !f->is_export && !f->is_method && !f->is_extern &&
           (f->ns == NULL || f->ns[0] == 0) && strcmp(f->name, "main") != 0;
}

/* สแกนหาจุดเรียกฟังก์ชัน overload แล้วเปลี่ยนชื่อให้ตรง signature ของ argument (scope-aware) */
static void scan_ov(Node *prog, Node *n, Bind *map, int *nmap, OvSet *sets, int nsets) {
    if (!n) return;
    switch (n->kind) {                          /* จัดการ scope ก่อน (รองรับ shadowing) */
        case ND_BLOCK: { int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        case ND_VAR_DECL: { scan_ov(prog,n->operand,map,nmap,sets,nsets); add_bind(map,nmap,n); return; }
        case ND_FOR: { int s=*nmap; scan_ov(prog,n->init,map,nmap,sets,nsets); scan_ov(prog,n->cond,map,nmap,sets,nsets); scan_ov(prog,n->step,map,nmap,sets,nsets); scan_ov(prog,n->body,map,nmap,sets,nsets); *nmap=s; return; }
        case ND_IF: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s1=*nmap; scan_ov(prog,n->then_branch,map,nmap,sets,nsets); *nmap=s1; int s2=*nmap; scan_ov(prog,n->else_branch,map,nmap,sets,nsets); *nmap=s2; return; }
        case ND_WHILE: case ND_DOWHILE: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s=*nmap; scan_ov(prog,n->body,map,nmap,sets,nsets); *nmap=s; return; }
        case ND_SWITCH: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        case ND_CASE: { scan_ov(prog,n->operand,map,nmap,sets,nsets); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        default: break;
    }
    /* แก้ลูกก่อน (resolve การเรียกซ้อนใน argument) เพื่อให้อนุมานชนิดของ argument ที่เป็น
     * การเรียก overload ได้ถูก — เช่น outer(inner(x)) ต้อง resolve inner ก่อน */
    scan_ov(prog, n->lhs, map, nmap, sets, nsets);   scan_ov(prog, n->rhs, map, nmap, sets, nsets);
    scan_ov(prog, n->operand, map, nmap, sets, nsets); scan_ov(prog, n->cond, map, nmap, sets, nsets);
    scan_ov(prog, n->then_branch, map, nmap, sets, nsets); scan_ov(prog, n->else_branch, map, nmap, sets, nsets);
    scan_ov(prog, n->init, map, nmap, sets, nsets);  scan_ov(prog, n->step, map, nmap, sets, nsets);
    scan_ov(prog, n->body, map, nmap, sets, nsets);
    for (int i = 0; i < n->nitems; i++) scan_ov(prog, n->items[i], map, nmap, sets, nsets);

    if (n->kind == ND_CALL && n->operand && n->operand->kind == ND_IDENT) {
        for (int s = 0; s < nsets; s++) {
            if (sets[s].n < 2 || strcmp(sets[s].orig, n->operand->name) != 0) continue;
            char wx[SIGCAP], wc[SIGCAP];
            sig_call(prog, n, map, *nmap, wx, 1);   /* signature ละเอียด */
            sig_call(prog, n, map, *nmap, wc, 0);   /* signature หมวด (fallback) */
            int found = -1;
            /* 1) จับคู่แบบเป๊ะตามความกว้างก่อน (เช่น i32 ตรง i32) */
            for (int d = 0; d < sets[s].n; d++)
                if (strcmp(sets[s].sig[d], wx) == 0) { found = d; break; }
            /* 2) ถ้าไม่เจอ ใช้หมวด — ต้องมีตัวเดียวเท่านั้น (เช่น literal i64 -> show(i32) ที่มีตัวเดียว) */
            if (found < 0) {
                int cnt = 0, cd = -1;
                for (int d = 0; d < sets[s].n; d++)
                    if (strcmp(sets[s].catsig[d], wc) == 0) { cnt++; cd = d; }
                if (cnt == 1) found = cd;
                else if (cnt > 1) {
                    fprintf(stderr, "codegen error: ambiguous call to '%s' with argument types (%s) — multiple width overloads match; use 'as' to disambiguate\n", sets[s].orig, wx);
                    break;
                }
            }
            if (found >= 0) { free(n->operand->name); n->operand->name = strdup(sets[s].defs[found]->name); }
            else fprintf(stderr, "codegen error: no overload of '%s' matches argument types (%s)\n", sets[s].orig, wx);
            break;
        }
    }
}

/* เทียบ namespace (NULL ถือเป็น "") */
static int ns_eq(const char *a, const char *b) {
    if (!a) a = ""; if (!b) b = "";
    return strcmp(a, b) == 0;
}

/* ตรวจชื่อซ้ำระดับบน: struct/trait ชื่อซ้ำ, ฟังก์ชัน (ns+ชื่อ+ชนิดพารามิเตอร์เป๊ะ) ซ้ำ
 * — overload (ชื่อเดียวต่างชนิด) ไม่ถือว่าซ้ำ; extern ซ้ำได้ (เป็นการประกาศ ถูก dedup ภายหลัง)
 * เรียกก่อน monomorphize เพื่อจับ error ทันที คืนจำนวน error */
int check_duplicates(Node *prog) {
    int errc = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *a = prog->items[i];
        if (a->kind != ND_STRUCT_DECL && a->kind != ND_TRAIT && a->kind != ND_FUNC) continue;
        for (int j = 0; j < i; j++) {
            Node *b = prog->items[j];
            if (a->kind != b->kind || !a->name || !b->name || strcmp(a->name, b->name) != 0) continue;
            if (a->kind == ND_STRUCT_DECL) {
                fprintf(stderr, "error: duplicate struct '%s'\n", a->name); errc++;
            } else if (a->kind == ND_TRAIT) {
                fprintf(stderr, "error: duplicate trait '%s'\n", a->name); errc++;
            } else { /* ND_FUNC */
                if (a->is_extern || b->is_extern || a->ngen > 0 || b->ngen > 0) continue;
                if (!ns_eq(a->ns, b->ns)) continue;
                char sa[SIGCAP], sb[SIGCAP]; sig_func(a, sa, 1); sig_func(b, sb, 1);
                if (strcmp(sa, sb) != 0) continue; /* คนละ signature = overload ไม่ใช่ซ้ำ */
                if (a->ns && a->ns[0])
                    fprintf(stderr, "error: duplicate function '%s.%s' with the same parameter types (%s)\n", a->ns, a->name, sa);
                else
                    fprintf(stderr, "error: duplicate function '%s' with the same parameter types (%s)\n", a->name, sa);
                errc++;
            }
        }
    }
    return errc;
}

void resolve_overloads(Node *prog) {
    static OvSet sets[128]; int nsets = 0;
    /* 1) จัดกลุ่มฟังก์ชันตามชื่อเดิม */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (!ov_eligible(f)) continue;
        int si = -1;
        for (int s = 0; s < nsets; s++) if (strcmp(sets[s].orig, f->name) == 0) { si = s; break; }
        if (si < 0) {
            if (nsets >= 128) { fprintf(stderr, "codegen error: too many distinct function names for overloading\n"); break; }
            si = nsets++; sets[si].orig = strdup(f->name); sets[si].n = 0;
        }
        if (sets[si].n < 16) {
            sets[si].defs[sets[si].n] = f;
            sig_func(f, sets[si].sig[sets[si].n], 1);     /* ละเอียด */
            sig_func(f, sets[si].catsig[sets[si].n], 0);  /* หมวด */
            sets[si].n++;
        }
        else fprintf(stderr, "codegen error: too many overloads of '%s' (max 16)\n", f->name);
    }
    /* 2) เปลี่ยนชื่อสมาชิกของกลุ่มที่มีมากกว่า 1 (overloaded) ตาม signature ละเอียด
     *    (การนิยามซ้ำ signature เดียวกันถูกจับไปแล้วใน check_duplicates ก่อน monomorphize) */
    for (int s = 0; s < nsets; s++) {
        if (sets[s].n < 2) continue;
        for (int d = 0; d < sets[s].n; d++) {
            char mangled[600]; snprintf(mangled, sizeof(mangled), "%s__%s", sets[s].orig, sets[s].sig[d]);
            free(sets[s].defs[d]->name); sets[s].defs[d]->name = strdup(mangled);
        }
    }
    /* 3) resolve จุดเรียกทุกที่ (ในทุกฟังก์ชัน concrete + ค่าเริ่มต้นของ global)
     *    ข้าม generic template (ngen>0) เพราะชนิดยังไม่ระบุ — instance ของมันถูกสแกนแยก */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body && f->ngen == 0) {
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
            scan_ov(prog, f->body, map, &nmap, sets, nsets);
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            Bind map[512]; int nmap = 0;   /* ใช้ map จริง (กัน NULL-deref ถ้า init มี block/decl) */
            seed_globals(prog, map, &nmap);
            scan_ov(prog, f->operand, map, &nmap, sets, nsets);
        }
    }
}

/* ---------- ตัวตรวจชนิดเวลาคอมไพล์ (compile-time type checking) ----------
 *
 * จุดประสงค์: จับ "ชนิดมั่ว" ตั้งแต่ตอนคอมไพล์ (ไม่ปล่อยให้ไปพังตอนรัน) เช่น
 *   50 + "50"           -> error (บวกตัวเลขกับสตริงไม่ได้)
 *   let x: u8 = "hi"    -> error (กำหนดสตริงให้ตัวแปรตัวเลขไม่ได้)
 * ออกแบบให้ "เข้มเฉพาะที่ผิดชัดเจน" เพื่อไม่ false-positive กับโค้ดระดับล่างที่ถูกต้อง
 * (เช่น pointer +/- int, เทียบ pointer กับ 0 เพื่อเช็ค null, ผสมความกว้าง int) */

static int tc_numeric(CType t) {            /* ตัวเลข (int/float/char/bool) ที่ไม่ใช่ pointer */
    if (t.ptr) return 0;
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
        case TYPE_F32: case TYPE_F64: case TYPE_CHAR: case TYPE_BOOL: return 1;
        default: return 0;
    }
}
static int tc_integer(CType t) {            /* จำนวนเต็ม/char/bool (ใช้กับ bitwise/shift) */
    if (t.ptr) return 0;
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
        case TYPE_CHAR: case TYPE_BOOL: return 1;
        default: return 0;
    }
}
static int tc_isptr(CType t)    { return t.ptr > 0; }
static int tc_isstr(CType t)    { return t.ptr == 0 && t.base == TYPE_STR; }
static int tc_isstruct(CType t) { return t.ptr == 0 && t.base == TYPE_STRUCT; }
static int tc_isvoid(CType t)   { return t.ptr == 0 && t.base == TYPE_VOID; }

/* เขียนชื่อชนิดอ่านง่ายลง buf (เช่น "u8", "*i32", "str", "Point") */
static void tc_name(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = '*';
    const char *b = (t.base == TYPE_STRUCT && t.sname) ? t.sname : datatype_name(t.base);
    strcpy(p, b);
}

/* ตรวจว่า value (s) กำหนดให้ target (d) ได้ไหม */
static int tc_assignable(CType d, CType s) {
    if (tc_isstruct(d)) return tc_isstruct(s) && d.sname && s.sname && strcmp(d.sname, s.sname) == 0;
    if (tc_isstruct(s)) return 0;                       /* struct ให้ค่าที่ไม่ใช่ struct ชนิดเดียวกันไม่ได้ */
    if (tc_isptr(d) || tc_isstr(d))                     /* target เป็น pointer/str: รับ pointer/str/integer (address/null) */
        return tc_isptr(s) || tc_isstr(s) || tc_integer(s);
    if (tc_numeric(d)) return tc_numeric(s);            /* target เป็นตัวเลข: ห้าม str/pointer/struct */
    return 1;                                           /* void ฯลฯ: ไม่เข้มงวด */
}

/* บริบทของฟังก์ชันที่กำลังตรวจ — map/nmap เป็น scope stack ที่โตขึ้นตามการประกาศ (push/pop ต่อ block)
 * เพื่อให้อนุมานชนิดของตัวแปรถูกต้องเมื่อมี shadowing (ใช้ตัวที่ประกาศก่อนและอยู่ใน scope) */
typedef struct { Node *prog; Bind *map; int *nmap; CType ret; int *errc; } TcCtx;

static void tc_err(TcCtx *c, Node *n, const char *msg, CType a, CType b) {
    char an[128], bn[128]; tc_name(a, an); tc_name(b, bn);
    fprintf(stderr, "type error (line %d): %s '%s' and '%s'\n", n->line, msg, an, bn);
    (*c->errc)++;
}

/* เพิ่มตัวแปรเข้า scope ปัจจุบัน (มองเห็นได้หลังจุดประกาศ) */
static void tc_add(TcCtx *c, Node *d) {
    if (*c->nmap >= 512 || !d->name) return;
    c->map[*c->nmap].name = d->name;
    c->map[*c->nmap].t.base = d->type;
    c->map[*c->nmap].t.ptr = d->ptr;
    c->map[*c->nmap].t.sname = d->type_name;
    c->map[*c->nmap].t.sig = d->sig;
    (*c->nmap)++;
}

static void tc_check(TcCtx *c, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK: {                       /* เปิด scope ใหม่: ตัวแปรในบล็อกหายไปเมื่อจบ */
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_VAR_DECL: {                    /* ตรวจค่าเริ่มต้นด้วย scope ปัจจุบันก่อน แล้วค่อยเพิ่มตัวแปร */
            if (n->operand) {
                tc_check(c, n->operand);
                CType dt = { n->type, n->ptr, n->type_name, NULL };
                CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
                if (!tc_assignable(dt, vt)) tc_err(c, n, "cannot initialize variable: type mismatch between", dt, vt);
            }
            tc_add(c, n);
            return;
        }
        case ND_FOR: {                         /* ตัวแปรใน for-init มี scope แค่ในลูป */
            int save = *c->nmap;
            tc_check(c, n->init); tc_check(c, n->cond); tc_check(c, n->step); tc_check(c, n->body);
            *c->nmap = save;
            return;
        }
        case ND_IF: {
            tc_check(c, n->cond);
            int s1 = *c->nmap; tc_check(c, n->then_branch); *c->nmap = s1;
            int s2 = *c->nmap; tc_check(c, n->else_branch); *c->nmap = s2;
            return;
        }
        case ND_WHILE: case ND_DOWHILE: {
            tc_check(c, n->cond);
            int save = *c->nmap; tc_check(c, n->body); *c->nmap = save;
            return;
        }
        case ND_SWITCH: {
            tc_check(c, n->cond);
            /* switch เทียบด้วยจำนวนเต็ม (je) — ค่าที่เทียบต้องเป็น integer/char/bool ไม่งั้นผลผิดเงียบ ๆ */
            CType ct = infer(c->prog, n->cond, c->map, *c->nmap);
            if (!tc_integer(ct)) {
                char tn[128]; tc_name(ct, tn);
                fprintf(stderr, "type error (line %d): switch requires an integer/char/bool value, got '%s'\n", n->line, tn);
                (*c->errc)++;
            }
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_CASE: {
            tc_check(c, n->operand);
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_STRUCT_LIT: {
            /* รายการ item เป็น ND_ASSIGN (field = value) — ตรวจชนิดค่าเทียบชนิดฟิลด์ที่ประกาศ */
            for (int i = 0; i < n->nitems; i++) {
                Node *fi = n->items[i];
                if (!fi || !fi->rhs) continue;
                tc_check(c, fi->rhs);
                CType ft;
                if (fi->lhs && fi->lhs->kind == ND_IDENT &&
                    struct_field_type(c->prog, n->name, fi->lhs->name, &ft)) {
                    CType vt = infer(c->prog, fi->rhs, c->map, *c->nmap);
                    if (!tc_assignable(ft, vt)) tc_err(c, fi, "struct field type mismatch between", ft, vt);
                }
            }
            return;
        }
        default: break;
    }

    /* นิพจน์ทั่วไป: เดินลูกก่อน แล้วตรวจตัวดำเนินการ */
    tc_check(c, n->lhs); tc_check(c, n->rhs); tc_check(c, n->operand);
    tc_check(c, n->cond); tc_check(c, n->then_branch); tc_check(c, n->else_branch);
    tc_check(c, n->init); tc_check(c, n->step); tc_check(c, n->body);
    for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);

    if (n->kind == ND_BINARY) {
        CType lt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType rt = infer(c->prog, n->rhs, c->map, *c->nmap);
        TokenType op = n->op;
        if (op == TK_PLUS || op == TK_MINUS) {
            int lp = tc_isptr(lt), rp = tc_isptr(rt);
            if (lp && rp) {                                  /* ptr - ptr ได้, ptr + ptr ไม่ได้ */
                if (op == TK_PLUS) tc_err(c, n, "cannot add two pointers:", lt, rt);
            } else if (lp || rp) {                           /* ptr +/- int เท่านั้น */
                if (!((lp && tc_numeric(rt)) || (rp && tc_numeric(lt) && op == TK_PLUS)))
                    tc_err(c, n, "invalid pointer arithmetic between", lt, rt);
            } else if (!(tc_numeric(lt) && tc_numeric(rt))) {
                tc_err(c, n, "cannot apply '+'/'-' to", lt, rt);
            }
        } else if (op == TK_STAR || op == TK_SLASH || op == TK_STARSTAR) {
            if (!(tc_numeric(lt) && tc_numeric(rt))) tc_err(c, n, "cannot apply arithmetic to", lt, rt);
        } else if (op == TK_PERCENT) {
            /* modulo ใช้กับจำนวนเต็มเท่านั้น (float ต้องใช้ fmod) — กันผลผิดเงียบ ๆ */
            if (!(tc_integer(lt) && tc_integer(rt))) tc_err(c, n, "modulo '%' requires integer operands, got", lt, rt);
        } else if (op == TK_AMP || op == TK_PIPE || op == TK_CARET || op == TK_SHL || op == TK_SHR) {
            if (!(tc_integer(lt) && tc_integer(rt))) tc_err(c, n, "bitwise/shift requires integer operands, got", lt, rt);
        } else if (op == TK_EQ || op == TK_NEQ || op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt))
                tc_err(c, n, "cannot compare", lt, rt);
        } else if (op == TK_AND || op == TK_OR) {
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt))
                tc_err(c, n, "logical operator requires scalar operands, got", lt, rt);
        }
    } else if (n->kind == ND_ASSIGN) {
        CType lt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType rt = infer(c->prog, n->rhs, c->map, *c->nmap);
        if (!tc_assignable(lt, rt)) tc_err(c, n, "cannot assign value of type", rt, lt);   /* "... rt to lt" */
    } else if (n->kind == ND_RETURN && n->operand) {
        CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
        if (!tc_assignable(c->ret, vt)) tc_err(c, n, "return type mismatch between", c->ret, vt);
    } else if (n->kind == ND_UNARY && n->op == TK_STAR) {
        /* dereference `*x` ต้องใช้กับ pointer เท่านั้น (กัน segfault เงียบ ๆ) */
        CType ot = infer(c->prog, n->operand, c->map, *c->nmap);
        if (ot.ptr == 0) {
            char on[128]; tc_name(ot, on);
            fprintf(stderr, "type error (line %d): cannot dereference non-pointer value of type '%s'\n", n->line, on);
            (*c->errc)++;
        }
    } else if (n->kind == ND_CALL && n->operand) {
        /* ตรวจชนิด argument ของการเรียก: ฟังก์ชันตรง f(...) และ method obj.m(...) ของผู้ใช้
         * (เว้น extern/generic/namespaced — ดูชนิดไม่ครบ/variadic). argoff=1 = ข้าม self ของ method */
        Node *callee = n->operand, *fn = NULL;
        int argoff = 0;
        if (callee->kind == ND_IDENT) {
            for (int i = 0; i < c->prog->nitems; i++) {
                Node *d = c->prog->items[i];
                if (d->kind == ND_FUNC && !d->is_extern && d->ngen == 0 && !d->is_method &&
                    d->name && strcmp(d->name, callee->name) == 0) { fn = d; break; }
            }
        } else if (callee->kind == ND_MEMBER) {
            CType bt = infer(c->prog, callee->operand, c->map, *c->nmap);
            if (bt.base == TYPE_STRUCT && bt.sname) {       /* method call obj.m(...) */
                for (int i = 0; i < c->prog->nitems; i++) {
                    Node *d = c->prog->items[i];
                    if (d->kind == ND_FUNC && d->is_method && d->ngen == 0 && d->ns && callee->name &&
                        strcmp(d->ns, bt.sname) == 0 && strcmp(d->name, callee->name) == 0) { fn = d; argoff = 1; break; }
                }
            }
        }
        if (fn) {
            if (n->nitems != fn->nitems - argoff) {
                fprintf(stderr, "type error (line %d): %s '%s' expects %d argument(s) but got %d\n",
                        n->line, argoff ? "method" : "function", fn->name, fn->nitems - argoff, n->nitems);
                (*c->errc)++;
            } else {
                for (int i = 0; i < n->nitems; i++) {
                    Node *pp = fn->items[i + argoff];
                    CType pt = { pp->type, pp->ptr, pp->type_name, pp->sig };
                    CType at = infer(c->prog, n->items[i], c->map, *c->nmap);
                    if (!tc_assignable(pt, at)) {
                        char an[128], pn[128]; tc_name(at, an); tc_name(pt, pn);
                        fprintf(stderr, "type error (line %d): argument %d to '%s': cannot pass '%s' where '%s' is expected\n",
                                n->line, i + 1, fn->name, an, pn);
                        (*c->errc)++;
                    }
                }
            }
        }
    }
    /* หมายเหตุ: ND_VAR_DECL ถูกจัดการใน switch ด้านบนแล้ว (ตรวจ init + เพิ่มตัวแปรเข้า scope) */
}

int typecheck(Node *prog) {
    int errc = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body && f->ngen == 0) {
            Bind map[512]; int nmap = 0;
            CType ret = { f->type, f->ptr, f->type_name, NULL };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);            /* global อยู่ใน scope ฐาน */
            for (int j = 0; j < f->nitems; j++)        /* พารามิเตอร์ shadow ทับ global ได้ */
                if (f->items[j]->kind == ND_PARAM) tc_add(&c, f->items[j]);
            tc_check(&c, f->body);
        } else if (f->kind == ND_VAR_DECL && f->operand) {  /* ตรวจค่าเริ่มต้นของ global ด้วย */
            Bind map[512]; int nmap = 0;
            CType ret = { TYPE_VOID, 0, NULL, NULL };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);
            tc_check(&c, f->operand);
            CType dt = { f->type, f->ptr, f->type_name, NULL };
            CType vt = infer(prog, f->operand, map, nmap);
            if (!tc_assignable(dt, vt)) tc_err(&c, f, "cannot initialize global: type mismatch between", dt, vt);
        }
    }
    return errc;
}

/* เติม default method ของ trait ให้ type ที่ impl แต่ไม่ได้เขียน method นั้นเอง
 * (clone body จาก trait แล้วแทน Self -> ชื่อ type) — ทำก่อนตรวจความครบ */
static void apply_trait_defaults(Node *prog) {
    int n = prog->nitems; /* เฉพาะ marker ที่มีตอนเริ่ม (method ใหม่ที่ append ไม่ต้องวนซ้ำ) */
    for (int i = 0; i < n; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_TRAIT_IMPL) continue;
        Node *tr = find_trait(prog, d->name);
        if (!tr) continue;
        for (int j = 0; j < tr->nitems; j++) {
            Node *sig = tr->items[j];
            if (sig->kind != ND_FUNC || !sig->body) continue;           /* ไม่มี default */
            if (type_has_method(prog, d->type_name, sig->name)) continue;/* type เขียนเองแล้ว */
            Node *m = node_clone(sig);
            m->is_method = 1;
            free(m->ns); m->ns = strdup(d->type_name);
            Bind self_map[1];
            self_map[0].name = "Self";
            self_map[0].t.base = TYPE_STRUCT; self_map[0].t.ptr = 0; self_map[0].t.sname = d->type_name;
            substitute(m, self_map, 1);                                  /* Self -> type จริง */
            node_add_item(prog, m);
        }
    }
}

int monomorphize(Node *prog) {
    mono_err = 0;
    apply_trait_defaults(prog); /* เติม default method ก่อน แล้วค่อยตรวจความครบ */
    check_trait_impls(prog);   /* impl Trait for Type ต้องครบและ trait ต้องมีจริง */
    /* วนจนไม่มี instance ใหม่ (รองรับ generic เรียก generic) */
    for (int round = 0; round < 64; round++) {
        int made = 0;
        int nfuncs = prog->nitems; /* สแกนเฉพาะที่มีอยู่ตอนเริ่มรอบ (instance ใหม่รอรอบถัดไป) */
        for (int i = 0; i < nfuncs; i++) {
            Node *f = prog->items[i];
            if (f->kind == ND_FUNC && f->ngen == 0 && f->body) {
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
                scan_calls(prog, f->body, map, &nmap, &made);
            } else if (f->kind == ND_VAR_DECL && f->operand) {   /* generic call ในค่าเริ่มต้น global */
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                scan_calls(prog, f->operand, map, &nmap, &made);
            }
        }
        if (made == 0) break;
    }
    return mono_err;
}
