# MVS Compiler

คอมไพเลอร์สำหรับภาษา **MVS** — ภาษาระดับล่าง (low-level) ที่ออกแบบให้อยู่ระดับเดียวกับ C
แต่มี syntax ที่อ่านง่ายกว่า เขียนด้วย **C ล้วน ไม่พึ่ง LLVM / flex / bison**
และสร้างแอสเซมบลี **x86-64 (Windows)** โดยตรง

> สถานะปัจจุบัน: **subset พื้นฐาน** ที่คอมไพล์และรันได้จริงครบ pipeline
> (ดูหัวข้อ [ความสามารถปัจจุบัน](#ความสามารถปัจจุบัน) และ [แผนพัฒนา](#แผนพัฒนาต่อ))

## หลักการทำงาน (Pipeline)

```
  .mvs ──> [lexer] ──> tokens ──> [parser] ──> AST ──> [codegen] ──> .asm
                                                                       │
                                              nasm -f win64 ◀──────────┘
                                                   │
                                                  .obj ──> clang (linker) ──> .exe
```

โครงสร้างถูกออกแบบให้ **รองรับหลายสถาปัตยกรรม**: ส่วน lexer/parser/AST ไม่ผูกกับ
สถาปัตยกรรมใด ๆ ส่วน codegen เลือก backend ผ่าน interface เดียว
การเพิ่ม ARM64 หรือ Linux ในอนาคตทำได้โดยเพิ่มไฟล์ backend ใน `src/arch/`
โดยไม่ต้องแก้ส่วนหน้า

## โครงสร้างโปรเจกต์

```
src/
  token.h            นิยามชนิดของ token ทั้งหมด
  lexer.h / lexer.c  ตัวตัดคำ (เขียนด้วยมือ)
  ast.h  / ast.c     โครงสร้างต้นไม้ไวยากรณ์ (AST) + ตัวช่วย
  parser.h / parser.c  parser แบบ recursive descent
  module.h / module.c  ระบบโมดูล (resolve import ข้ามไฟล์ + std)
  codegen.h / codegen.c  ตัวขับ codegen (เลือก backend ตามสถาปัตยกรรม)
  arch/
    common.h / common.c          ส่วนกลางของ backend (ไม่ขึ้นกับ arch): type, struct layout, symtab, tree-shaking
    x86_64/win.h / x86_64/win.c   backend เฉพาะ x86-64 Windows (ปล่อย NASM/win64)
    (อนาคต) x86_64/linux.c, arm64/linux.c — เพิ่มไฟล์ใหม่ reuse common ได้
  main.c             CLI: ขับ pipeline ทั้งหมด
std/                 standard library เขียนด้วย MVS (io, string, fmt, fs, net)
examples/            โปรแกรมตัวอย่าง (.mvs)
Makefile             สคริปต์ build
GUIDE.md             คู่มือภาษาแบบละเอียด (ไวยากรณ์ + กลไกหน่วยความจำ + assembly จริง)
Rules.md             กฎ/ข้อควรระวังสำหรับผู้พัฒนา (และ AI session ถัดไป)
Recap.md             สรุปสถานะงานและสิ่งที่ค้างไว้
```

## สิ่งที่ต้องมี (Prerequisites)

- **clang** — ใช้ build ตัวคอมไพเลอร์ และทำหน้าที่เป็น linker
- **nasm** — ประกอบไฟล์ `.asm` เป็น `.obj`

ตรวจสอบ:
```powershell
clang --version
nasm --version
```

## การ build และใช้งาน

```powershell
# 1. build ตัวคอมไพเลอร์เป็น mvs.exe
make
#   (หรือเรียก clang ตรง ๆ ตามคำสั่งใน Makefile)

# 2. คอมไพล์โปรแกรม MVS เป็น .exe (mvs.exe เรียก nasm + clang ให้ และบอกเวอร์ชันที่ใช้)
.\mvs.exe examples\01_language\hello.mvs

# 3. รัน
.\examples\01_language\hello.exe
```
> ต้องมี **clang** และ **nasm** บน PATH — ถ้าไม่พบ mvs.exe จะแจ้งให้ติดตั้งพร้อมลิงก์

ตัวเลือกของ `mvs.exe`:

| ตัวเลือก         | ความหมาย                                        |
|------------------|--------------------------------------------------|
| `-o <file>`      | กำหนดชื่อไฟล์ผลลัพธ์                              |
| `-S`             | สร้างเฉพาะไฟล์ `.asm` แล้วหยุด                    |
| `-c` / `--emit-obj` | สร้างไฟล์ `.obj` แล้วหยุด (สำหรับลิงก์กับภาษา C)  |
| `--nostd`        | โหมด freestanding: ไม่พึ่ง std/CRT/OS (สร้าง `.obj`) — ใช้เขียน OS |
| `--keep`         | เก็บไฟล์กลาง (`.asm`, `.obj`) ไว้ ไม่ลบทิ้ง        |

## ความสามารถปัจจุบัน

- ตัวแปร `let` / `const` (global + ในฟังก์ชัน) พร้อม**ความกว้างของชนิดจริง** (i8/u8/.../i64 — wrap-around)
  - **unsigned เต็มรูปแบบ**: หาร/มอด/เปรียบเทียบแบบ unsigned, รองรับ u64 เต็มช่วง (> 2^63)
- เลขคณิตครบ `+ - * / % ^` ตามลำดับความสำคัญ และวงเล็บ `()`
- **ตรวจชนิดเวลาคอมไพล์ (type checking):** `50 + "50"`, `u8 = str`, bitwise บน float ฯลฯ = error ทันที
- **แปลงชนิดด้วย `as`** (แบบ Rust): `x as i32`, `i as f64`, `300 as u8` (narrowing), `ptr as usize`
- ตัวเปรียบเทียบ `== != < > <= >=`, ตรรกะ `&& || !`, **ระดับบิต `& | ^ ~ << >>`** (`^`=XOR), ยกกำลัง `**`
- **generic function** `func max<T>(...)` (monomorphization) + **overloading ตามชนิด** (`show(i32)`/`show(str)`) — generic+overload = constraint แบบ duck-typed
- **trait + associated function** (แบบ Rust): `trait Area {...}` (+ **default method**), `impl Area for Point {...}`, `Point::new(..)`, generic ผูก trait `func f<T: Area>(..)` — **static dispatch** + ตรวจ bound ตอนคอมไพล์
- **`String`** (สตริงบน heap): `String::from("..")`, `String::from_int(42)`, `.push_str()` (chain), `.as_str()`, `.drop()` — `import { string } from "std"`
- โครงสร้างควบคุม: `if / elseif / else`, `while`, `for`, `do-while`, `switch / case / default`, `break`, `continue`
- ฟังก์ชัน: พารามิเตอร์ (รองรับเกิน 4 ตัวผ่าน stack), การคืนค่า, การเรียกซ้ำ (recursion)
- **struct แบบ Rust:** ประกาศ, struct literal, สมาชิก `p.x`, struct ซ้อน, คืน struct, **ส่ง by-value**, และ **method** ผ่าน `impl`
  - method: `impl Vec { func len(self: *Vec) -> i32 {...} }` เรียกด้วย `v.len()`
  - **method chaining:** `v.setX(3).setY(4).sum()` (method คืน `*self`)
- **pointer:** `*T`, `&x` (address-of), `*p` (dereference), เลขคณิต scale ตาม `sizeof(pointee)`
- **function pointer:** ชนิด `func(...) -> T` เป็นค่า — ชื่อฟังก์ชันเปล่า ๆ คือค่า, เก็บในตัวแปร/ฟิลด์ struct, ส่งเป็น argument (higher-order), เรียกแบบ indirect (ดู `examples/02_functions/funcptr.mvs`)
- **ทศนิยม `f32`/`f64`** ผ่าน SSE/xmm — เรียกฟังก์ชัน C math (`sqrt`, `pow`) ได้
- **ทำงานร่วมกับ C:** `extern` (MVS เรียก C), `export func` (C เรียก MVS), `-c` ผลิต `.obj`
- **freestanding (`--nostd`):** ไม่พึ่ง std/CRT/OS — เขียน OS/bare-metal ได้
- **ทศนิยม `f32` / `f64`:** literal, เลขคณิต, เปรียบเทียบ, แปลง int↔float อัตโนมัติ (ใช้ SSE)
- **io.out แบบ Rust:** `io.out("x = {}", x)` — `{}`, `{:x}` (hex), หลายค่า (ไม่จำกัด), เลือกชนิดอัตโนมัติ, **พิมพ์ struct ได้** (`Point { x: 3, y: 4 }` แบบ `{:?}`)
- **ระบบโมดูล (import):**
  - `import { f } from "./file.mvs";` — นำเข้าฟังก์ชันจากไฟล์อื่น (ใช้ชื่อตรง ๆ)
  - `import { io, fs, net } from "std";` — นำเข้าโมดูลจาก package เป็น namespace
- **foreign function:** `extern func printf(fmt: str) -> i32;` เรียกฟังก์ชันจาก C runtime
- **standard library เขียนด้วย MVS เอง** (ต้อง import ก่อนใช้):
  - `io` — `io.out` (format), `io.print`, `io.in` (รับ input)
  - `string` — `String` (สตริงบน heap): `String::from`/`from_int`, `push_str`, `as_str`, `drop`
  - `fmt` — trait `Display` + `fmt.println`/`fmt.print` (formatting แบบ library)
  - `fs` — `fs.write`, `fs.read`
  - `net` — TCP แบบ Rust: `net.TcpClient`/`net.TcpServer` + method (`accept`/`send`/`recv`/`close`) ผ่าน Winsock
- **tree-shaking:** gen เฉพาะฟังก์ชันที่ถูกเรียกจริง
- คอมเมนต์ `//` และ `/* ... */`

ดูตัวอย่างใน [`examples/`](examples/) ซึ่งแบ่งเป็นกลุ่ม (`01_language` .. `08_stdlib`) — ทุกไฟล์มีส่วนหัวบอกคำสั่ง build/run

### ทำงานร่วมกับ C และ freestanding

```rust
// mvslib.mvs — export ให้ C เรียก (ไม่มี main)
export func mvs_square(x: i32) -> i32 { return x * x; }
```
```c
// main.c — เรียกฟังก์ชันจาก MVS
int mvs_square(int);
int main(void) { return mvs_square(6); }  // 36
```
```powershell
mvs.exe mvslib.mvs -c           # ได้ mvslib.obj
clang main.c mvslib.obj -o app  # ลิงก์ MVS เข้ากับ C
mvs.exe kernel.mvs --nostd      # freestanding .obj (ไม่พึ่ง std/CRT) สำหรับเขียน OS
```

```rust
import { io } from "std";
import { factorial } from "./mathlib.mvs";

func main() -> i8 {
    io.out("factorial(5) = {}", factorial(5));   // factorial(5) = 120
    return 0;
}
```

## แผนพัฒนาต่อ

- [x] ~~struct (+ method/impl, chaining), pointer, ความกว้าง int จริง~~ — ทำแล้ว
- [x] ~~`f32` / `f64`, `switch/case`, `do-while`~~ — ทำแล้ว
- [x] ~~`import`, foreign function, stdlib `io`/`fs`/`net`, `io.in`, args > 4, tree-shaking~~ — ทำแล้ว
- [x] ~~C interop (`export`, `-c`), freestanding `--nostd`~~ — ทำแล้ว
- [x] ~~overloading ตามชนิด (+ แยกความกว้าง int), generics~~ — ทำแล้ว
- [x] ~~trait + associated function + `<T: Trait>` + trait default method~~ — ทำแล้ว
- [x] ~~type checking ตอนคอมไพล์ + `as` cast + scope shadowing~~ — ทำแล้ว
- [x] ~~`String` (heap) + `String::from`/`from_int` + struct-returning call เป็น rvalue~~ — ทำแล้ว
- [x] ~~float ส่งผ่าน xmm กับฟังก์ชัน C ภายนอก (สองทาง) + f32 เก็บ 4 ไบต์จริง~~ — ทำแล้ว
- [x] ~~function pointer (`func(...) -> T` เป็นค่า + indirect call + ใช้กับ generic)~~ — ทำแล้ว
- [ ] i128/u128 คำนวณ 128-bit เต็ม · trait แบบ dynamic dispatch / `where` · backend ARM64, Linux

## หมายเหตุ

- โค้ดในส่วน compiler มีคอมเมนต์ภาษาไทยอธิบายการทำงานอย่างละเอียด
- อยากเข้าใจภาษาเชิงลึก (กลไกหน่วยความจำ, assembly จริง) อ่าน [`GUIDE.md`](GUIDE.md)
- ก่อนพัฒนาต่อ โปรดอ่าน [`Rules.md`](Rules.md) (กฎ/ข้อควรระวัง) และ [`Recap.md`](Recap.md) (สถานะงาน)

---

## 📚 เอกสารทั้งหมด (เชื่อมโยงถึงกัน)

> 📍 คุณกำลังอ่าน: **README.md**

| ไฟล์ | เนื้อหา |
|------|---------|
| [README.md](README.md) | ภาพรวม · ติดตั้ง · วิธีใช้ · ความสามารถ |
| [GUIDE.md](GUIDE.md) | คู่มือภาษาเชิงลึก · ไวยากรณ์ · กลไกหน่วยความจำ · assembly จริง |
| [Rules.md](Rules.md) | กฎ/ปรัชญา freestanding · ABI · ข้อควรระวังสำหรับผู้พัฒนา |
| [Recap.md](Recap.md) | สถานะงาน · roadmap · gotchas |
| [CLAUDE.md](CLAUDE.md) | ไฟล์นำทางสำหรับ AI/ผู้พัฒนา · คำสั่ง · จุดแก้ไข |
| [examples/README.md](examples/README.md) | รายการโปรแกรมตัวอย่างทั้งหมด |
