/*
 * generic.h - การทำ monomorphization ของ generic function
 *
 * แปลง `func max<T>(a: T, b: T) -> T` ให้กลายเป็นสำเนาเฉพาะชนิดที่ถูกเรียกจริง
 * (เช่น max__i32, max__f64) แล้วเปลี่ยนจุดเรียกให้ชี้ไปยังสำเนานั้น
 * ทำงานเป็น pass บน AST ก่อนส่งให้ codegen — codegen จึงเห็นแต่ฟังก์ชันชนิดเดียว (concrete)
 */
#ifndef MVS_GENERIC_H
#define MVS_GENERIC_H

#include "ast.h"

/* แทนที่การเรียก generic ด้วย instance เฉพาะชนิด แล้วเพิ่ม instance เข้าโปรแกรม
 * คืนจำนวน error จากการละเมิด trait bound (`<T: Trait>` ที่ชนิดจริงไม่ impl) — 0 = ผ่าน */
int monomorphize(Node *program);

/* แก้ฟังก์ชัน overload (ชื่อซ้ำต่างชนิดพารามิเตอร์): เปลี่ยนชื่อแต่ละตัวตาม signature
 * แล้ว resolve จุดเรียกให้ชี้ตัวที่ชนิด argument ตรงกัน */
void resolve_overloads(Node *program);

/* ตรวจชนิดเวลาคอมไพล์: จับการผสมชนิดที่ผิด (เช่น 50 + "50", u8 = str)
 * คืนจำนวน error ที่พบ (0 = ผ่าน). เรียกหลัง monomorphize + resolve_overloads */
int typecheck(Node *program);

/* ตรวจชื่อซ้ำระดับบน (struct/trait/ฟังก์ชันที่ ns+ชื่อ+signature เดียวกัน) — เรียกก่อน monomorphize
 * คืนจำนวน error (0 = ผ่าน) */
int check_duplicates(Node *program);

#endif /* MVS_GENERIC_H */
