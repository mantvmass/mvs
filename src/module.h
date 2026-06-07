/*
 * module.h - ระบบโมดูล (module system) สำหรับการ import ข้ามไฟล์
 *
 * รับไฟล์เริ่มต้น (entry) แล้วไล่ resolve คำสั่ง import ทั้งหมดแบบ recursive
 * รวมฟังก์ชัน/ตัวแปร/extern จากทุกโมดูลเข้าเป็น ND_PROGRAM เดียวเพื่อส่งให้ codegen
 *
 * รูปแบบ import ที่รองรับ:
 *   import { factorial } from "./mathlib.mvs";  // นำเข้าจากไฟล์ (ใช้ชื่อตรง ๆ)
 *   import { io } from "std";                    // นำเข้าจาก package เป็น namespace (io.out)
 */
#ifndef MVS_MODULE_H
#define MVS_MODULE_H

#include "ast.h"

/* โหลดไฟล์ entry และ resolve import ทั้งหมด คืน ND_PROGRAM ที่รวมทุกโมดูล
 *   entry_path - ไฟล์ .mvs เริ่มต้น
 *   stddir     - โฟลเดอร์ของ standard library (ใช้ตอน import จาก package เช่น "std")
 *   nostd      - 1 = โหมด freestanding: ห้าม import จาก package (เช่น "std")
 *   had_error  - ตั้งเป็น 1 หากเกิดข้อผิดพลาด
 */
Node *module_load(const char *entry_path, const char *stddir, int nostd, int *had_error);

#endif /* MVS_MODULE_H */
