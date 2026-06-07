/*
 * ast.h - นิยามโครงสร้างต้นไม้ไวยากรณ์ (Abstract Syntax Tree)
 *
 * parser จะสร้างต้นไม้นี้จาก token แล้ว codegen จะเดินต้นไม้เพื่อสร้าง assembly
 * ใช้ Node เดียวแทนทุกชนิดของโหนด โดยแยกความหมายด้วยฟิลด์ kind
 * ฟิลด์ไหนใช้กับ kind ไหนดูได้จากคอมเมนต์ของแต่ละ NodeKind
 */
#ifndef MVS_AST_H
#define MVS_AST_H

#include "token.h"

/* ชนิดข้อมูลของภาษา MVS */
typedef enum {
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64, TYPE_I128, TYPE_ISIZE,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64, TYPE_U128, TYPE_USIZE,
    TYPE_BOOL, TYPE_VOID, TYPE_STR, TYPE_CHAR, TYPE_F32, TYPE_F64,
    TYPE_STRUCT, TYPE_FUNC, TYPE_UNKNOWN
} DataType;

/* ชนิดของโหนดใน AST */
typedef enum {
    /* --- นิพจน์ (expression) --- */
    ND_INT,        /* ค่าคงที่จำนวนเต็ม: ใช้ int_val */
    ND_FLOAT,      /* ค่าคงที่ทศนิยม: ใช้ int_val เก็บ bit-pattern ของ double (IEEE-754) */
    ND_STR,        /* ค่าคงที่สตริง: ใช้ str_val, str_len */
    ND_CHAR,       /* ค่าคงที่อักขระ: ใช้ int_val */
    ND_BOOL,       /* ค่าคงที่บูลีน: ใช้ int_val (0/1) */
    ND_IDENT,      /* อ้างถึงตัวแปร: ใช้ name */
    ND_BINARY,     /* การดำเนินการสองตัว: ใช้ op, lhs, rhs */
    ND_UNARY,      /* การดำเนินการตัวเดียว: ใช้ op, operand (เช่น -x, !x) */
    ND_CALL,       /* การเรียกฟังก์ชัน: ใช้ name (ชื่อฟังก์ชัน), items (อาร์กิวเมนต์) */
    ND_MEMBER,     /* การเข้าถึงสมาชิก a.b: ใช้ operand (ฐาน), name (ชื่อสมาชิก) */
    ND_ASSIGN,     /* การกำหนดค่า: ใช้ op, lhs (เป้าหมาย), rhs (ค่า) */
    ND_CAST,       /* แปลงชนิด x as T: ใช้ operand (ค่า), type/ptr/type_name (ชนิดปลายทาง) */
    ND_FRAMEREF,   /* อ้าง lvalue ที่ช่อง temp บน frame [rbp - int_val] (ใช้ภายใน codegen io.out) */

    /* --- คำสั่ง (statement) --- */
    ND_VAR_DECL,   /* ประกาศตัวแปร: ใช้ name, type, operand (ค่าเริ่มต้น), is_const */
    ND_EXPR_STMT,  /* คำสั่งที่เป็นนิพจน์: ใช้ operand */
    ND_RETURN,     /* return: ใช้ operand (ค่าที่คืน อาจเป็น NULL) */
    ND_IF,         /* if/elseif/else: ใช้ cond, then_branch, else_branch */
    ND_WHILE,      /* while: ใช้ cond, body */
    ND_FOR,        /* for: ใช้ init, cond, step, body */
    ND_BLOCK,      /* บล็อก { ... }: ใช้ items (รายการคำสั่ง) */
    ND_BREAK,      /* break */
    ND_CONTINUE,   /* continue */
    ND_DOWHILE,    /* do-while: ใช้ body, cond */
    ND_SWITCH,     /* switch: ใช้ cond (ค่าที่เทียบ), items (รายการ ND_CASE) */
    ND_CASE,       /* case/default: ใช้ operand (ค่าที่เทียบ, NULL = default), items (คำสั่งใน case) */

    /* --- struct --- */
    ND_STRUCT_DECL,/* ประกาศ struct: ใช้ name, items (ฟิลด์เป็น ND_PARAM) */
    ND_STRUCT_LIT, /* สร้างค่า struct: ใช้ name (ชื่อ struct), items (ฟิลด์ init เป็น ND_ASSIGN) */

    /* --- ระดับบนสุด (top level) --- */
    ND_PARAM,      /* พารามิเตอร์ฟังก์ชัน/ฟิลด์ struct: ใช้ name, type, ptr, type_name */
    ND_FUNC,       /* นิยามฟังก์ชัน: ใช้ name, type (ชนิดคืนค่า), items (พารามิเตอร์), body
                    *  ถ้า is_extern = 1 และ body = NULL คือการประกาศ foreign function */
    ND_IMPORT,     /* นำเข้าโมดูล: ใช้ str_val (path), items (รายชื่อที่นำเข้า เป็น ND_IDENT) */
    ND_TRAIT,      /* นิยาม trait: ใช้ name (ชื่อ trait), items (method signature เป็น ND_FUNC ไม่มี body) */
    ND_TRAIT_IMPL, /* เครื่องหมายว่า type หนึ่ง impl trait หนึ่ง: ใช้ name (ชื่อ trait), type_name (ชื่อ type) */
    ND_PROGRAM     /* ทั้งโปรแกรม: ใช้ items (รายการนิยามระดับบน) */
} NodeKind;

/* โหนดหนึ่งตัวใน AST */
typedef struct Node {
    NodeKind kind;        /* ชนิดของโหนด */
    int      line;        /* บรรทัดในซอร์ส ใช้รายงาน error */

    DataType type;        /* ชนิดข้อมูลของโหนด (สำหรับ expr) หรือชนิดคืนค่า (สำหรับ func) */
    int      ptr;         /* ความลึกของ pointer (0 = ไม่ใช่ pointer, 1 = *T, 2 = **T) */
    char    *type_name;   /* ชื่อ struct เมื่อ type == TYPE_STRUCT */
    struct Node *sig;     /* ลายเซ็นเมื่อ type == TYPE_FUNC (function pointer):
                           *   เป็นโหนดแบบ ND_FUNC — items = พารามิเตอร์ (ND_PARAM),
                           *   type/ptr/type_name = ชนิดที่คืนค่า */

    /* ค่าคงที่ */
    long long int_val;    /* ใช้กับ ND_INT, ND_CHAR, ND_BOOL */
    char     *str_val;    /* ใช้กับ ND_STR */
    int       str_len;    /* ความยาวสตริง */

    /* ชื่อ */
    char     *name;       /* ชื่อตัวแปร/ฟังก์ชัน/สมาชิก/พารามิเตอร์ */

    /* ตัวดำเนินการ */
    TokenType op;         /* ใช้กับ ND_BINARY, ND_UNARY, ND_ASSIGN */

    /* โหนดลูก (ความหมายขึ้นกับ kind ดูคอมเมนต์ด้านบน) */
    struct Node *lhs;     /* ตัวซ้าย (binary/assign) */
    struct Node *rhs;     /* ตัวขวา (binary/assign) */
    struct Node *operand; /* ตัวเดียว (unary/return/member base/var init/expr-stmt) */
    struct Node *cond;        /* เงื่อนไข (if/while/for) */
    struct Node *then_branch; /* กิ่ง then (if) */
    struct Node *else_branch; /* กิ่ง else (if) - อาจเป็น ND_IF ตัวถัดไปสำหรับ elseif */
    struct Node *init;        /* ส่วนเริ่มต้น (for) */
    struct Node *step;        /* ส่วนเปลี่ยนค่า (for) */
    struct Node *body;        /* ตัวลูป/ตัวฟังก์ชัน (while/for/func) */

    /* รายการโหนดลูกแบบไม่จำกัดจำนวน (block stmts / call args / func params / program decls) */
    struct Node **items;
    int           nitems;
    int           is_const;   /* ใช้กับ ND_VAR_DECL: 1 = const, 0 = let */
    int           is_extern;  /* ใช้กับ ND_FUNC: 1 = foreign function (ไม่มี body) */
    int           is_export;  /* ใช้กับ ND_FUNC: 1 = ส่งออกให้ C เรียกได้ (ชื่อสัญลักษณ์ดิบ) */
    int           is_method;  /* ใช้กับ ND_FUNC: 1 = method ของ struct (ns = ชื่อ struct) */
    char         *gen[4];     /* ใช้กับ ND_FUNC: ชื่อ generic type parameter (เช่น "T") */
    char         *gen_bound[4];/* ใช้กับ ND_FUNC: ชื่อ trait ที่ผูกกับ generic param แต่ละตัว (NULL = ไม่ผูก) เช่น <T: Display> */
    int           ngen;       /* จำนวน generic type parameter (0 = ฟังก์ชันปกติ) */
    char         *ns;         /* namespace สำหรับ "ป้ายชื่อสัญลักษณ์" (module สำหรับ func ปกติ, ชื่อ struct สำหรับ method) */
    char         *mod;        /* namespace ของ "โมดูล" ที่ฟังก์ชันสังกัด ใช้ resolve การเรียกแบบไม่ระบุชื่อภายใน method */
} Node;

/* --- ฟังก์ชันช่วยสร้างโหนด --- */
Node *node_new(NodeKind kind, int line);
/* เพิ่มโหนดลูกเข้า items[] (ขยายหน่วยความจำอัตโนมัติ) */
void  node_add_item(Node *parent, Node *child);
/* คัดลอกต้นไม้ AST แบบลึก (ใช้ตอน instantiate generic function) */
Node *node_clone(Node *n);

/* --- ฟังก์ชันช่วยเรื่องชนิดข้อมูล --- */
/* แปลง type token (เช่น TK_TYPE_I32) เป็น DataType */
DataType datatype_from_token(TokenType t);
/* ชื่อชนิดข้อมูลสำหรับแสดงผล/debug */
const char *datatype_name(DataType t);

#endif /* MVS_AST_H */
