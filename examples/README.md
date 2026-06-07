# ตัวอย่างโปรแกรม MVS

ตัวอย่างแบ่งเป็นกลุ่มตามหัวข้อ — **ทุกไฟล์มีส่วนหัวบอกคำสั่ง build/run**
เริ่มที่ [`demo.mvs`](demo.mvs) เพื่อดูภาพรวม แล้วเจาะกลุ่มที่สนใจ

## วิธี build/run ทั่วไป
```powershell
mvs.exe examples/<group>/<file>.mvs     # สร้าง <file>.exe ข้างไฟล์ (เรียก nasm + clang ให้)
examples\<group>\<file>.exe             # รัน
mvs.exe examples/<group>/<file>.mvs -S  # ดูแอสเซมบลีที่ gen (ไม่เรียก nasm/clang)
```
> ต้องมี **clang** และ **nasm** บน PATH — mvs.exe จะแจ้งเตือนถ้าไม่พบ และบอกเวอร์ชันที่ใช้ตอน build

---

## `demo.mvs`
โชว์เคสรวม: io.out, ตัวแปร/const, ฟังก์ชัน, เลขคณิต, if/for/while, hex

## 01_language — แกนภาษา
| ไฟล์ | เนื้อหา |
|------|---------|
| `hello.mvs` | โปรแกรมแรกสุด |
| `types.mvs` | **ชนิดข้อมูลทั้งหมด** (i8..i128, u8..u128, isize/usize, bool, char, str, f32, f64, pointer) |
| `operators.mvs` | เลขคณิต/เปรียบเทียบ/ตรรกะ, `**` ยกกำลัง, break/continue, args > 4 |
| `bitwise.mvs` | `& \| ^ ~ << >>` + bitmask/flags |
| `casts.mvs` | แปลงชนิดด้วย `as` + การตรวจชนิดตอนคอมไพล์ |
| `control.mvs` | if/elseif/else, while, for, do-while, switch/case |
| `args.mvs` | รับ command-line args ผ่าน `main(argc, argv)` |

## 02_functions — ฟังก์ชัน / generic / overload
| ไฟล์ | เนื้อหา |
|------|---------|
| `recursion.mvs` | เรียกตัวเอง + mutual recursion |
| `generics.mvs` | **generic function** (monomorphization) |
| `overload.mvs` | **overloading** ตามชนิด + generic เรียก overload |
| `funcptr.mvs` | **function pointer** — `func(...) -> T` เป็นค่า, ส่ง/เก็บ/เรียก (indirect call) |

## 03_structs — struct / method / pointer
| ไฟล์ | เนื้อหา |
|------|---------|
| `structs.mvs` | struct, สมาชิก, คืน struct, io.out struct |
| `methods.mvs` | method + associated `Type::new` + chaining |
| `pointers.mvs` | `&`/`*`/`**`, pointer arithmetic, อาเรย์ผ่าน malloc |

## 04_traits — trait / Display
| ไฟล์ | เนื้อหา |
|------|---------|
| `traits.mvs` | trait + `impl Trait for` + `<T: Trait>` (static dispatch) |
| `display.mvs` | trait `Display` + `fmt.println` (formatting แบบ library) |

## 05_strings — สตริง
| ไฟล์ | เนื้อหา |
|------|---------|
| `strings.mvs` | **`String`** (heap, owned): `from`/`from_int`/`push_str`/`as_str`/`drop` |

## 06_modules — ระบบโมดูล
| ไฟล์ | เนื้อหา |
|------|---------|
| `use_import.mvs` | import 3 รูปแบบ (namespace / symbol / alias) |
| `mathlib.mvs` | โมดูลที่ผู้ใช้เขียนเอง (ถูก import) |

## 07_c_interop — ทำงานร่วมกับภาษา C
| ไฟล์ | เนื้อหา |
|------|---------|
| `extern_c.mvs` | **MVS เรียก C** (strlen/atoi จาก CRT) |
| `use_c.mvs` + `mathops.c` | **MVS เรียกฟังก์ชันจากไฟล์ C ของเราเอง** (ลิงก์ .obj + .c) |
| `export_lib.mvs` + `caller.c` | **C เรียก MVS** (`export func` + prototype ฝั่ง C) |
| `freestanding.mvs` | โหมด `--nostd` (ไม่พึ่ง std/CRT/OS) เขียน OS/bare-metal |

## 08_stdlib — ไลบรารีมาตรฐาน
| ไฟล์ | เนื้อหา |
|------|---------|
| `io_demo.mvs` | io.out ทุกรูปแบบ (`{}`, `{:x}`, struct, หลาย arg) + io.print |
| `floats.mvs` | เลขทศนิยม + เรียก C math (`sqrt`) |
| `files.mvs` | `fs.write`/`fs.read` + `io.in` |
| `net_client.mvs` / `net_server.mvs` | TCP ผ่าน Winsock (`net.TcpClient`/`TcpServer`) |
