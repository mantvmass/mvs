/*
 * common.h - ส่วนกลางของ backend ที่ "ไม่ขึ้นกับสถาปัตยกรรม"
 *
 * รวมโครงสร้างสถานะ (Gen) และฟังก์ชันช่วยที่ใช้ร่วมกันได้ทุก backend:
 *   - ระบบชนิดข้อมูล (ขนาด, signed/float, struct layout, การอนุมานชนิด)
 *   - ตารางสัญลักษณ์ตัวแปร/ฟังก์ชัน + การจองพื้นที่ตัวแปร
 *   - การวิเคราะห์การเข้าถึงได้ (reachability / tree-shaking)
 *   - การแปลง format string ของ io.out
 *
 * แต่ละ backend (เช่น arch/x86_64/win.c) เพียง include ไฟล์นี้แล้วเขียนเฉพาะ
 * ส่วน "ปล่อย instruction" ของสถาปัตยกรรมตน — โค้ดส่วนกลางไม่ต้องเขียนซ้ำ
 *
 * หมายเหตุ: type_size สมมติ pointer ขนาด 8 ไบต์ (เป้าหมายปัจจุบันเป็น 64-bit ทั้งหมด)
 */
#ifndef MVS_ARCH_COMMON_H
#define MVS_ARCH_COMMON_H

#include <stdio.h>
#include "../ast.h"

#define MAX_SYM   512
#define MAX_FUNC  256
#define MAX_STR   256
#define MAX_LOOP  64
#define MAX_ARGS  4   /* รองรับอาร์กิวเมนต์ผ่านรีจิสเตอร์ 4 ตัว (ที่เกินวางบน stack) */
#define LABEL_MAX 720 /* ขนาดบัฟเฟอร์ชื่อ label (รองรับชื่อ instance generic ที่ยาว) — globals ใช้ prefix mvs_gv_ กันชน func */

/* ตารางสัญลักษณ์ (symbol) สำหรับตัวแปร */
typedef struct {
    char    *name;
    DataType type;
    int      ptr;       /* ความลึกของ pointer (0 = ไม่ใช่ pointer) */
    char    *sname;     /* ชื่อ struct เมื่อ type == TYPE_STRUCT */
    int      size;      /* ขนาดเป็นไบต์ของตัวแปร */
    int      is_global; /* 1 = ตัวแปร global, 0 = ตัวแปร local บน stack */
    int      offset;    /* สำหรับ local: ระยะจาก rbp (เป็นบวก ใช้ [rbp - offset]) */
    Node    *sig;       /* ลายเซ็นเมื่อ type == TYPE_FUNC (function pointer) ไม่งั้น NULL */
} Sym;

/* ฟิลด์หนึ่งตัวภายใน struct (พร้อม offset และขนาด ใช้ตอนเข้าถึงสมาชิก) */
typedef struct {
    char    *name;
    DataType type;
    int      ptr;
    char    *sname;     /* ชื่อ struct เมื่อฟิลด์เป็น struct ซ้อน */
    int      offset;    /* offset ของฟิลด์ภายใน struct (ไบต์) */
    int      size;      /* ขนาดของฟิลด์ (ไบต์) */
    Node    *sig;       /* ลายเซ็นเมื่อฟิลด์เป็น TYPE_FUNC (function pointer) ไม่งั้น NULL */
} Field;

/* ข้อมูล layout ของ struct หนึ่งชนิด */
typedef struct {
    char  *name;
    Field  fields[64];
    int    nfields;
    int    size;        /* ขนาดรวมของ struct (ไบต์) */
} StructInfo;

/* ชนิดของนิพจน์ (ใช้ติดตามชนิด/ขนาด/pointer ระหว่าง codegen) */
typedef struct {
    DataType base;
    int      ptr;
    char    *sname;
    Node    *sig;       /* ลายเซ็นเมื่อ base == TYPE_FUNC (function pointer) ไม่งั้น NULL */
} ExprType;

/* สถานะของ backend ทั้งหมดรวมไว้ในโครงสร้างเดียว (คอมไพเลอร์ทำงานครั้งละไฟล์) */
typedef struct {
    FILE *out;

    Sym  locals[MAX_SYM];   int nlocals;   /* ตัวแปรในฟังก์ชันปัจจุบัน (จองทุกตัวไว้ใน pre-pass) */
    Sym  globals[MAX_SYM];  int nglobals;  /* ตัวแปร global ทั้งโปรแกรม */

    /* สแต็กของตัวแปรที่ "มองเห็นได้" ตามขอบเขต (scope) — ดัชนีชี้เข้า locals[]
     * ใช้ทำ scope shadowing: ตัวแปรใน block ในมองเห็นทับตัวนอก, ออก block แล้วหายไป */
    int  visible[MAX_SYM];  int nvisible;

    /* ตารางฟังก์ชัน เก็บตัวโหนด ND_FUNC ไว้ใช้ตอน gen การเรียก (รวม namespace/extern) */
    Node *funcs[MAX_FUNC]; int nfuncs;

    /* ตาราง layout ของ struct ทุกชนิด */
    StructInfo structs[MAX_FUNC]; int nstructs;

    int sret_off;           /* offset ของช่องเก็บ hidden pointer สำหรับฟังก์ชันที่คืน struct (0 = ไม่มี) */

    const char *cur_ns;     /* namespace ที่ใช้ resolve การเรียกแบบไม่ระบุชื่อภายในฟังก์ชันปัจจุบัน */
    int cur_ret_float;      /* 1 ถ้าฟังก์ชันที่กำลัง gen คืนค่าทศนิยม (ต้องคืนผ่าน xmm0) */
    int cur_ret_f32c;       /* 1 ถ้าฟังก์ชัน export คืน f32 (ต้องแปลง double -> single ให้ C) */
    int io_imported;        /* 1 ถ้ามีการ import โมดูล io (ใช้ gate ฟังก์ชันในตัว io.out) */

    /* คลังสตริง: เก็บไบต์ของสตริงเพื่อนำไปประกาศในส่วน .data */
    struct { unsigned char *data; int len; } strs[MAX_STR]; int nstrs;

    /* สแต็กของลูปสำหรับ break/continue (เก็บหมายเลข label) */
    struct { int brk; int cont; } loops[MAX_LOOP]; int nloops;

    int label_id;           /* ตัวนับ label ให้ไม่ซ้ำ */
    int had_error;
} Gen;

/* --- ตัวนับ label และการตั้งชื่อสัญลักษณ์ (กฎร่วมทุก backend) --- */
int   new_label(Gen *g);
void  func_label_of(Node *fn, char *buf);   /* ชื่อ label ของฟังก์ชัน (extern/export = ชื่อดิบ) */
void  global_label(const char *name, char *buf);

/* --- ตารางฟังก์ชัน / ตัวแปร / สตริง --- */
Node *find_func(Gen *g, const char *ns, const char *name);
Sym  *find_var(Gen *g, const char *name);
int   intern_string(Gen *g, const char *data, int len);

/* --- ระบบชนิดข้อมูลและ struct layout --- */
StructInfo *find_struct(Gen *g, const char *name);
int   type_size(Gen *g, DataType base, int ptr, const char *sname);
int   is_float_type(DataType t);
int   is_signed_type(DataType t);
void  register_struct(Gen *g, Node *decl);
void  layout_structs(Gen *g);   /* คำนวณ offset/size ของทุก struct (fixpoint, รองรับ forward reference) */
Field *find_field(StructInfo *s, const char *name);
ExprType type_of(Gen *g, Node *n);
DataType infer_type(Gen *g, Node *n);
/* คืนลายเซ็นถ้า callee เป็นค่า function pointer (ตัวแปร/ฟิลด์ TYPE_FUNC) ไม่งั้น NULL (= เรียกตรง) */
Node *expr_func_sig(Gen *g, Node *callee);

/* --- แปลง format string ของ io.out ({} แบบ Rust) เป็น format ของ C printf ---
 * เติม vals[] = ค่าที่ส่งให้ printf จริง (struct ถูกขยายเป็นรายฟิลด์), *out_nv = จำนวน */
char *build_c_format(Gen *g, Node *call, const char *fmt, int *out_len, int *out_nph,
                     Node **vals, int *out_nv, int vals_cap);

/* --- การจองพื้นที่ตัวแปรบน stack (frame layout) --- */
int   add_local(Gen *g, const char *name, DataType type, int ptr, char *sname, Node *sig, int *frame);
void  collect_locals(Gen *g, Node *n, int *frame);
void  collect_struct_temps(Gen *g, Node *n, int *frame);  /* จองช่อง temp ให้ struct ที่คืนจากฟังก์ชัน (ใช้เป็น rvalue) */

/* --- การวิเคราะห์การเข้าถึงได้ (tree-shaking) --- */
int   func_index(Gen *g, Node *f);
void  reach_func(Gen *g, int idx, char *reached);
void  reach_node(Gen *g, Node *n, const char *ns, char *reached);

#endif /* MVS_ARCH_COMMON_H */
