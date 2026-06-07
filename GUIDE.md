# GUIDE — คู่มือภาษา MVS แบบละเอียด (โครงสร้าง + กลไกภายใน + สื่อการเรียนรู้)

> เอกสารนี้คือสื่อการเรียนรู้เชิงลึกของภาษา **MVS** ตั้งแต่ไวยากรณ์ ไปจนถึงกลไกระดับล่าง
> (การจอง/คืนหน่วยความจำ, การจัดวาง stack frame, calling convention, การแปลงเป็นแอสเซมบลีจริง)
> เหมาะสำหรับผู้ที่อยากเข้าใจว่า "โค้ดที่เขียนกลายเป็น instruction อะไรบ้าง"
>
> เอกสารที่เกี่ยวข้อง: [`README.md`](README.md) (เริ่มต้นใช้งาน) · [`Rules.md`](Rules.md) (กฎผู้พัฒนา) · [`Recap.md`](Recap.md) (สถานะ/roadmap)

สารบัญ
1. [ภาษา MVS คืออะไร](#1-ภาษา-mvs-คืออะไร)
2. [เส้นทางจากซอร์สโค้ดสู่โปรแกรม (pipeline)](#2-เส้นทางจากซอร์สโค้ดสู่โปรแกรม)
3. [โครงสร้างคอมไพเลอร์](#3-โครงสร้างคอมไพเลอร์)
4. [ไวยากรณ์ภาษาแบบครบ](#4-ไวยากรณ์ภาษาแบบครบ)
5. [กลไกหน่วยความจำ: stack, heap, การจอง/คืน](#5-กลไกหน่วยความจำ)
6. [กลไกภายในแบบละเอียด (พร้อม assembly จริง)](#6-กลไกภายในแบบละเอียด)
7. [ข้อจำกัดปัจจุบัน](#7-ข้อจำกัดปัจจุบัน)
8. [ลำดับการเรียนรู้ที่แนะนำ](#8-ลำดับการเรียนรู้ที่แนะนำ)
9. [อภิธานศัพท์](#9-อภิธานศัพท์)

---

## 1. ภาษา MVS คืออะไร

MVS เป็นภาษาโปรแกรมมิ่งระดับล่าง (low-level) อยู่ระดับเดียวกับ C แต่ syntax อ่านง่ายกว่า
(คล้าย Rust) เป้าหมายหลัก:

- **จัดการหน่วยความจำเอง** เหมือน C — ไม่มี garbage collector, ไม่มี runtime ซ่อนอยู่
- **freestanding by default** — แกนภาษาไม่พึ่ง OS/CRT จึงนำไปเขียน OS / bare-metal ได้
- คอมไพล์เป็น **แอสเซมบลี x86-64 โดยตรง** (ไม่ใช้ LLVM) แล้วประกอบด้วย `nasm` ลิงก์ด้วย `clang`

ปรัชญาสำคัญ: ทุกอย่างที่แตะ OS (พิมพ์จอ, ไฟล์, เครือข่าย, จองหน่วยความจำ heap) อยู่ใน
**standard library (`std/*.mvs`) ที่ต้อง `import` เอง** ไม่ได้ฝังในคอมไพเลอร์

---

## 2. เส้นทางจากซอร์สโค้ดสู่โปรแกรม

```
ไฟล์ .mvs
   │  lexer   (src/lexer.c)      แปลงตัวอักษร -> token
   ▼
 tokens
   │  parser  (src/parser.c)     ประกอบ token -> ต้นไม้ไวยากรณ์ (AST)
   ▼
  AST
   │  module  (src/module.c)     รวมไฟล์ที่ import เข้าด้วยกัน
   ▼
 AST รวม
   │  codegen (src/arch/...)     เดิน AST -> แอสเซมบลี NASM (.asm)
   ▼
 .asm
   │  nasm -f win64              ประกอบ -> ไฟล์อ็อบเจกต์ (.obj)
   ▼
 .obj
   │  clang (linker)             ลิงก์ + ผูก C runtime -> ไฟล์ปฏิบัติการ
   ▼
 .exe
```

โหมดการสร้าง:
| คำสั่ง | ได้อะไร | ใช้เมื่อ |
|--------|---------|---------|
| `mvs file.mvs` | `.exe` | โปรแกรมปกติ |
| `mvs file.mvs -S` | `.asm` | อยากดูแอสเซมบลีที่ gen |
| `mvs file.mvs -c` | `.obj` | เอาไปลิงก์กับโปรแกรม C |
| `mvs file.mvs --nostd` | `.obj` (freestanding) | เขียน OS / bare-metal |

---

## 3. โครงสร้างคอมไพเลอร์

```
src/
  token.h            ชนิดของ token ทั้งหมด
  lexer.{h,c}        ตัวตัดคำ (เขียนมือ ไม่ใช้ flex)
  ast.{h,c}          นิยาม Node ของ AST (ใช้ Node เดียวแยกด้วย kind)
  parser.{h,c}       recursive-descent parser (ไม่ใช้ bison)
  module.{h,c}       ระบบโมดูล: resolve import ข้ามไฟล์ + package std
  codegen.{h,c}      ตัวขับ เลือก backend ตาม TargetArch
  arch/
    common.{h,c}     ★ ส่วนกลาง (ไม่ขึ้นกับ arch): ระบบชนิด, struct layout,
                        symbol table, จองตัวแปร, reachability, format string
    x86_64/win.c     ★ เฉพาะ x86-64 win64: ปล่อย instruction จริง + ABI
  main.c             CLI ขับ pipeline
std/                 standard library เขียนด้วย MVS (io, fs, net)
```

หัวใจอยู่ที่การแยก **"ตรรกะ" (common.c)** ออกจาก **"การปล่อย instruction" (win.c)**
เพิ่ม backend ใหม่ (เช่น `arch/x86_64/sysv.c` สำหรับ ELF/Linux) ทำได้โดย reuse `common.c`

---

## 4. ไวยากรณ์ภาษาแบบครบ

### 4.1 คอมเมนต์
```rust
// คอมเมนต์บรรทัดเดียว
/* คอมเมนต์
   หลายบรรทัด */
```

### 4.2 ชนิดข้อมูล (data types)

| ชนิด | ความหมาย | ขนาดจริง (ไบต์) | signed? |
|------|----------|-----------------|---------|
| `i8` `i16` `i32` `i64` | จำนวนเต็มมีเครื่องหมาย (signed: idiv, setl/setg) | 1, 2, 4, 8 | ✅ |
| `u8` `u16` `u32` `u64` | จำนวนเต็มไม่มีเครื่องหมาย (unsigned: div, setb/seta, wrap จริง) | 1, 2, 4, 8 | ❌ |
| `i128` `u128` | จำนวนเต็ม 128-bit (เก็บ/ส่ง 16 ไบต์ แต่คำนวณยังเป็น 64-bit) | 16 | — |
| `isize` `usize` | ขนาดเท่า pointer | 8 | i/u |
| `bool` | `true` / `false` | 1 | ❌ |
| `char` | อักขระ ใช้ `''` เท่านั้น เช่น `'A'` | 1 | ❌ |
| `f32` | ทศนิยม single-precision (เก็บ 4 ไบต์จริง, คำนวณเป็น double) | 4 | — |
| `f64` | ทศนิยม double-precision | 8 | — |
| `str` | สตริง ใช้ `""` เท่านั้น (ตัวชี้ไปยังไบต์ + 0) | 8 | — |
| `void` | ไม่มีค่า (ชนิดคืนของฟังก์ชัน) | 0 | — |
| `*T` | pointer ไปยังชนิด T | 8 | — |
| `struct` | โครงสร้างข้อมูล | ผลรวมของฟิลด์ | — |

> หมายเหตุ: คอมไพเลอร์เก็บค่า **คำนวณใน register 64-bit เสมอ** แต่เคารพความกว้างจริง
> ตอน **เก็บลงตัวแปร** (ตัดให้เหลือ N ไบต์) และตอน **โหลด** (ขยายกลับด้วย sign/zero-extend)
> ดังนั้น `u8` จึง wrap-around จริง เช่น `200 + 100 = 44` (ระหว่างคำนวณยังเป็น 64-bit)

#### `str` ต่างกับ `String` อย่างไร

| | `str` (ชนิดในตัว) | `String` (จาก `std/string`) |
|--|------------------|------------------------------|
| คืออะไร | **ตัวชี้** ไปยังข้อความคงที่ (null-terminated) | **บัฟเฟอร์บน heap** ที่เป็นเจ้าของข้อมูล |
| เปลี่ยน/ต่อได้ไหม | ❌ อ่านอย่างเดียว | ✅ `push_str`, สร้างจากเลขด้วย `from_int` |
| หน่วยความจำ | อยู่ใน `.data` (ไม่ต้องจัดการ) | จองด้วย malloc — ต้อง `drop()` เอง (ไม่มี GC) |
| สร้างอย่างไร | `let s: str = "hi";` | `let s: String = String::from("hi");` |
| ใช้กับ io.out | `io.out("{}", s)` ได้ตรง | ใช้ `s.as_str()` → คืน `str` |

```rust
import { io } from "std";
import { String } from "std/string";
let lit: str = "hello";                 // คงที่ อ่านอย่างเดียว
let owned: String = String::from("hi"); // บน heap
owned.push_str(", world");              // ต่อได้
io.out("{} / {}", lit, owned.as_str()); // hello / hi, world
owned.drop();                           // คืนหน่วยความจำ
```
> โดยทั่วไป: ใช้ `str` สำหรับข้อความคงที่/พารามิเตอร์ที่แค่อ่าน; ใช้ `String` เมื่อต้องสร้าง/ต่อข้อความตอน runtime

### 4.3 ตัวแปร
```rust
let name: type = value;     // เปลี่ยนค่าได้
const NAME: type = value;   // ค่าคงที่ (โดยตั้งใจให้คงที่)
let x: i32;                 // ประกาศโดยไม่กำหนดค่า (ค่าเริ่มต้นเป็น 0 บน stack ไม่รับประกัน)
```
ประกาศได้ทั้งระดับ **global** (นอกฟังก์ชัน) และ **local** (ในฟังก์ชัน)

### 4.4 ตัวดำเนินการ (operators)

| กลุ่ม | ตัวดำเนินการ | หมายเหตุ |
|-------|-------------|----------|
| เลขคณิต | `+ - * / % **` | `**` = ยกกำลัง (right-assoc), `%` = มอด |
| เปรียบเทียบ | `== != < > <= >=` | ผลเป็น bool (1/0); unsigned ใช้ setb/seta |
| ตรรกะ | `&& \|\| !` | short-circuit |
| ระดับบิต | `& \| ^ ~ << >>` | AND / OR / **XOR** / NOT / เลื่อนซ้าย / เลื่อนขวา |
| กำหนดค่า | `= += -= *= /=` | |
| เพิ่ม/ลด | `++ --` | แบบ postfix |
| pointer | `&x` `*p` `**p` | address-of / dereference (เลขคณิต pointer scale ตาม sizeof) |

ลำดับความสำคัญ (สูง→ต่ำ): `unary` → `as` → `**` → `* / %` → `+ -` → `<< >>` → เปรียบเทียบ →
`== !=` → `&`(bit) → `^`(bit) → `\|`(bit) → `&&` → `\|\|` → `=`  · ใช้วงเล็บ `()` จัดลำดับได้

> หมายเหตุ: `&` เป็นได้ทั้ง **address-of** (นำหน้า เช่น `&x`) และ **bitwise AND** (อยู่ระหว่างค่า เช่น `a & b`)
> แยกตามตำแหน่ง; `**` เป็นได้ทั้ง **ยกกำลัง** (`a ** b`) และ **deref สองชั้น** (`**ptr`) แยกตามตำแหน่งเช่นกัน

#### การตรวจชนิดเวลาคอมไพล์ (type checking)

ภาษานี้ **ไม่ยอมให้ชนิดมั่ว** — ตรวจตั้งแต่ตอนคอมไพล์ (สำคัญสำหรับงาน low-level/OS/embedded
ที่ความผิดพลาดชนิดอาจพังเงียบ ๆ ตอนรัน) ตัวอย่างที่จะเป็น **error ทันที**:
```rust
let x: i32 = 50 + "50";   // error: cannot apply '+'/'-' to 'i64' and 'str'
let y: u8  = "hello";     // error: cannot initialize variable: ... 'u8' and 'str'
let z: i32 = pt * 2;      // error (pt เป็น struct): cannot apply arithmetic to 'Point' and 'i64'
let w: i32 = f & 3;       // error (f เป็น float): bitwise/shift requires integer operands
f("hello");               // error: argument 1 to 'f': cannot pass 'str' where 'i32' is expected
f(1, 2, 3);               // error: function 'f' expects N argument(s) but got 3
```
กฎโดยสรุป: เลขคณิต/ยกกำลังต้องเป็น**ตัวเลข**, บิต/เลื่อนต้องเป็น**จำนวนเต็ม**, เทียบ/ตรรกะห้ามใช้กับ
**struct**, การกำหนดค่า/คืนค่า/ฟิลด์ต้อง**ชนิดเข้ากันได้**, และ **argument ของการเรียกฟังก์ชัน/method**
ต้องตรงชนิด+จำนวน — แต่ยัง**ผ่อนปรนกับงานระดับล่างที่ถูกต้อง**
(เช่น `ptr +/- int`, เทียบ pointer กับ `0` เพื่อเช็ค null, ผสมความกว้างของ int, ส่ง `str`↔`*u8`)

#### การแปลงชนิดด้วย `as` (explicit cast)

เมื่อต้องการแปลงชนิดจริง ๆ ใช้ `as` เพื่อบอกเจตนา (เหมือน Rust) — `as` ผูกแน่นกว่าตัวดำเนินการคณิต:
```rust
let a: i32 = 7;
a as f64 / 2.0     // 3.5   (int -> float; ถ้าไม่ cast: 7/2 = 3)
3.9 as i32         // 3     (float -> int: ตัดเศษทิ้ง ไม่ปัด)
300 as u8          // 44    (ลดความกว้าง: 300 mod 256)
(255 as u8) as i8  // -1    (ตีความบิตใหม่แบบมีเครื่องหมาย)
65 as char         // 'A'   (int -> char)
ptr as usize       // ที่อยู่เป็นจำนวนเต็ม (pointer <-> integer)
```
> ดูตัวอย่างเต็มที่ [`examples/01_language/casts.mvs`](examples/01_language/casts.mvs)

### 4.5 การควบคุมการทำงาน (control flow)
```rust
if (cond) { ... } elseif (cond) { ... } else { ... }

while (cond) { ... }

for (let i: i32 = 0; i < 10; i++) { ... }

do { ... } while (cond);          // ทำอย่างน้อย 1 ครั้ง

switch (x) {                       // fallthrough แบบ C (ต้อง break เอง)
    case 1: ...; break;
    case 2:
    case 3: ...; break;            // 2 และ 3 ใช้โค้ดเดียวกัน
    default: ...;
}

break;     // ออกจากลูป/switch
continue;  // ข้ามไปรอบถัดไป (ใน switch จะข้ามไป loop ที่ครอบอยู่)
```

### 4.6 ฟังก์ชัน
```rust
func add(a: i32, b: i32) -> i32 {
    return a + b;
}
func greet() -> void { ... }       // ไม่คืนค่า
```
- รองรับ **การเรียกซ้ำ (recursion)**
- รองรับ **พารามิเตอร์เกิน 4 ตัว** (ตัวที่ 5+ ส่งผ่าน stack)
- `func main() -> i8` คือจุดเริ่มโปรแกรม; ค่าที่ return เป็น exit code

#### Generic function (monomorphization)
```rust
func max<T>(a: T, b: T) -> T {   // T = ชนิดใดก็ได้
    if (a > b) { return a; }
    return b;
}
max(3, 7);       // คอมไพเลอร์สร้าง max__i64 อัตโนมัติ
max(2.5, 9.1);   // และ max__f64 (อนุมานชนิดจาก argument)
```
- คอมไพเลอร์อนุมานชนิดจริงจาก argument แล้ว **สร้างสำเนาเฉพาะชนิด** (เหมือน Rust/C++) — ดู `src/generic.c`
- รองรับ generic กับ pointer (`*T`), generic เรียก generic, และใช้ instance ซ้ำ

#### Function overloading (ชื่อซ้ำต่างชนิด)
```rust
func show(n: i32) -> void { io.out("int: {}", n); }
func show(s: str) -> void { io.out("str: {}", s); }
func show(p: Point) -> void { io.out("point: ({}, {})", p.x, p.y); }

show(42);       // เลือก show(i32)
show("hi");     // เลือก show(str)
```
- คอมไพเลอร์เลือก overload ตาม **หมวดชนิด** ของ argument (int/float/str/char/bool/pointer/struct)
  แล้วเปลี่ยนชื่อภายในตาม signature (เช่น `show__i`, `show__s`) — ดู `resolve_overloads` ใน `src/generic.c`

#### Generic + overload = constraint แบบ duck-typed (คล้าย trait)
```rust
func print_all<T>(a: T, b: T) -> void { show(a); show(b); }  // T ต้องมี show รองรับ
print_all(1, 2);      // ใช้ show(i32)
print_all("a", "b");  // ใช้ show(str)
```
generic ทำงานได้กับ "ชนิดใดก็ตามที่มี overload/operation ที่ใช้ในตัวมันรองรับ" — เป็น constraint โดยปริยาย
(แบบ duck-typed; ถ้าต้องการ constraint **ชัดเจนและตรวจสอบได้** ใช้ `trait` — ดูหัวข้อถัดไป)

#### Trait + associated function + generic ผูก trait (constraint ชัดเจน)
```rust
struct Point { x: i32; y: i32; }
struct Circle { r: i32; }

trait Area {
    func area(self: *Self) -> i32;       // signature เท่านั้น (ไม่มี body); Self = ชนิดที่ impl
}

impl Point {                              // inherent impl
    func new(x: i32, y: i32) -> Point { return Point { x: x, y: y }; }  // associated function
}
impl Area for Point  { func area(self: *Point)  -> i32 { return self.x * self.y; } }
impl Area for Circle { func area(self: *Circle) -> i32 { return 3 * self.r * self.r; } }

// generic ผูก trait: T ต้อง impl Area เท่านั้น ถึงเรียก .area() ได้
func describe<T: Area>(s: T) -> i32 { return s.area(); }

let p: Point = Point::new(3, 4);    // เรียก associated function ผ่าน ::
let c: Circle = Circle::new(5);
describe(p);                         // 12  — เลือก Point::area ตอนคอมไพล์ (static dispatch)
describe(c);                         // 75  — เลือก Circle::area
```
- **associated function** = ฟังก์ชันใน `impl` ที่**ไม่มี `self`** เรียกด้วย `Type::func(...)` (เหมือน constructor / `Type::from`)
- **trait** = สัญญา; **`impl Trait for Type`** ผูกชนิดเข้ากับ trait
- **`<T: Trait>`** บังคับชนิดให้ impl trait — ถ้าไม่ impl จะ **error ตอนคอมไพล์** (`type 'X' does not implement trait 'Area'`)
- dispatch เป็น **static** (ผ่าน monomorphization) — ไม่มี vtable/overhead ตอนรัน เหมาะกับงาน low-level
- คอมไพเลอร์ยังตรวจด้วยว่า `impl` ทำ method ครบตาม trait (ขาด → error) และ trait ที่อ้างถึงมีจริง
> ดูตัวอย่างเต็มที่ [`examples/04_traits/traits.mvs`](examples/04_traits/traits.mvs)

### 4.7 struct และ method (แบบ Rust)
```rust
struct Rect { w: i32; h: i32; }    // ฟิลด์คั่นด้วย ; หรือ ,

impl Rect {
    func area(self: *Rect) -> i32 { return self.w * self.h; }
    func scale(self: *Rect, k: i32) -> *Rect {   // คืน *self เพื่อ chain
        self.w *= k; self.h *= k; return self;
    }
}

let r: Rect = Rect { w: 3, h: 4 };  // struct literal
let a: i32 = r.area();              // method call (ฉีด &r เป็น self อัตโนมัติ)
let b: i32 = r.scale(2).scale(2).area();  // method chaining
r.w = 10;                          // เข้าถึง/แก้สมาชิก
```
- ฟังก์ชันคืนค่า struct ได้ (`func make() -> Rect { return Rect{...}; }`)
- เข้าถึง struct ซ้อน (`a.b.c`) และผ่าน pointer (`p.field` เมื่อ p เป็น `*Struct`)

### 4.8 pointer
```rust
let y: i32 = 42;
let p: *i32 = &y;     // p ชี้ไปที่ y
let v: i32 = *p;      // อ่านค่าที่ p ชี้ (= 42)
*p = 99;              // เขียนผ่าน p (y กลายเป็น 99)
```

### 4.8.1 function pointer
ชนิด `func(P1, P2, ...) -> R` ใช้เป็น **ค่า** ได้ — ชี้ไปยังฟังก์ชัน แล้วเรียกผ่านตัวแปร/ฟิลด์ (indirect call)
```rust
func add(a: i32, b: i32) -> i32 { return a + b; }
func mul(a: i32, b: i32) -> i32 { return a * b; }

// ค่าของ function pointer = "ชื่อฟังก์ชันเปล่า ๆ" (ไม่ต้องมี &)
let f: func(i32, i32) -> i32 = add;
io.out("{}", f(3, 4));        // 7   (เรียกผ่านตัวแปร)
f = mul;
io.out("{}", f(3, 4));        // 12

// ส่งเป็น argument (higher-order function)
func apply(op: func(i32, i32) -> i32, a: i32, b: i32) -> i32 { return op(a, b); }
io.out("{}", apply(add, 10, 20));   // 30

// เก็บเป็นฟิลด์ของ struct (ใช้ทำ dispatch table / callback)
struct Op { name: str; fn: func(i32, i32) -> i32; }
let plus: Op = Op { name: "plus", fn: add };
io.out("{}", plus.fn(5, 6));        // 11
```
- คืนค่าเป็น struct ผ่าน function pointer ได้ (ใช้ sret) · ใช้กับ generic ได้ (`f: func(T) -> T` หรือ `T` ผูกกับค่า function)
- โค้ดที่ gen: ค่า = `lea rax, [rel <label>]`; เรียก = `call rax` (ดู 6.2)
- ตัวอย่างเต็ม: [`examples/02_functions/funcptr.mvs`](examples/02_functions/funcptr.mvs)

### 4.9 ทศนิยม
```rust
let pi: f64 = 3.14159;
let area: f64 = pi * 2.0 * 2.0;
io.out("{}", 1.5 + 2);     // แปลง int -> float อัตโนมัติ -> 3.500000
```

### 4.10 การแสดงผล io.out (แบบ Rust)
```rust
import { io } from "std";
io.out("hello");                  // hello
io.out("x = {}", 42);             // x = 42      ({} = แทรกค่า, ขึ้นบรรทัดใหม่ให้)
io.out("{} + {} = {}", a, b, a+b);// หลายค่า
io.out("hex = {:x}", 255);        // hex = ff
io.out("pct {{}}");               // pct {}      ({{ }} = วงเล็บปีกกาจริง)
io.out("{}", p);                  // p เป็น struct -> Point { x: 3, y: 4 }  (แบบ {:?} ของ Rust)
```
> `io.out` เป็น **compiler intrinsic** (เหมือน `println!` ของ Rust) — แยกวิเคราะห์ `{}` ตอนคอมไพล์
> แล้วเลือกชนิดการพิมพ์ตาม argument; ต้อง `import { io }` ก่อนใช้
> รองรับ **struct** (ขยายเป็น `Name { field: ค่า, ... }` รวมถึง struct ซ้อน) และ **argument ไม่จำกัดจำนวน**
>
> **ทำไม io.out เป็น intrinsic ไม่ใช่ฟังก์ชันใน io.mvs?** เพราะรูป `{}` ที่รับ argument **จำนวนและชนิดไม่จำกัด**
> ต้องมี (ก) variadic ฝั่งรับ + (ข) การ dispatch ตามชนิดของแต่ละ argument ซึ่งต้องใช้ macro (ขยายตอนคอมไพล์)
> หรือ runtime type info (tagged value/dynamic dispatch) — ภาษานี้ยังไม่มี จึงทำเป็น intrinsic เหมือน `println!`
> **ทางเลือกที่เป็น library จริง:** ใช้ trait `Display` + `fmt.println(x)` (โมดูล `fmt`) สำหรับชนิดที่ผู้ใช้ impl เอง
> (static dispatch ผ่าน generic+trait) — ดู [`examples/04_traits/display.mvs`](examples/04_traits/display.mvs)

### 4.11 ระบบโมดูล (import) — 3 รูปแบบ ("path เป็นตัวกำหนด")

```rust
// A) namespace ของ submodule: path เป็น "package เปล่า" -> ชื่อใน {} คือ submodule -> เรียกแบบ io.xxx
import { io, fs, net } from "std";        // io.out(...), net.TcpServer(...)

// B) symbol import: path ชี้ "โมดูลเจาะจง" -> ชื่อใน {} คือสัญลักษณ์ -> ดึงเข้ามาตรง ๆ
import { String } from "std/string";      // String::from(...)
import { factorial } from "./mathlib.mvs"; // factorial(...)

// C) alias ทั้งโมดูลเป็น namespace: ไม่มี {} -> ใช้ชื่อ alias นำหน้า
import math from "./mathlib.mvs";          // math.factorial(...)
import str  from "std/string";             // str.<freefunc>(...)
```

| รูปแบบ path | ความหมายของ `{ }` | การเข้าถึง |
|-------------|-------------------|-----------|
| `"std"` (package เปล่า) | submodule → namespace | `io.out` |
| `"std/x"` หรือ `"./f.mvs"` + `{...}` | สัญลักษณ์ → ดึงตรง ๆ | `String::from` |
| `"std/x"` หรือ `"./f.mvs"` + alias | ทั้งโมดูล → namespace alias | `math.factorial` |

**กฎ:** อ้างทั้ง package/โมดูล → ใช้ namespace; เจาะจงสัญลักษณ์ → ดึงตรง ๆ (ไฟล์ relative ต้องลงท้าย `.mvs`)

**ตัวตรวจของระบบ import (error ทันที):**
- สัญลักษณ์ที่ import แบบ B ต้องมีจริงในโมดูล → ไม่งั้น `module 'x' has no exported symbol 'name'`
- ชื่อซ้ำ (struct/trait/ฟังก์ชัน ns+ชื่อ+ชนิดเดียวกัน) → `duplicate ...` (overload ต่างชนิดไม่นับ)
- namespace/alias ผูกซ้ำคนละโมดูล → `namespace 'x' is already bound to a different module`
- import วนกลับ (A→B→A) → `circular import detected`

```rust
extern func printf(fmt: str) -> i32;          // เรียกฟังก์ชัน C (MVS -> C)
export func mvs_add(a: i32, b: i32) -> i32 {  // ให้ C เรียก MVS (ชื่อสัญลักษณ์ดิบ)
    return a + b;
}
```

### 4.12 standard library
| โมดูล | ฟังก์ชันหลัก |
|-------|--------------|
| `io` | `io.out(fmt, ...)`, `io.print(s)`, `io.in(prompt) -> str` |
| `fs` | `fs.write(path, content)`, `fs.read(path) -> str` |
| `net` | `net.TcpClient(ip, port)`, `net.TcpServer(ip, port)` + method `accept`/`send`/`recv`/`close` |
| `string` | `String` (สตริงบน heap): `String::from(s)`, `String::from_int(n)`, `.push_str(s)` (chain), `.as_str() -> str`, `.len()`, `.drop()` |
| `fmt` | trait `Display { fmt(self) -> String }` + `fmt.println(x)` / `fmt.print(x)` — พิมพ์ชนิดที่ impl Display (library, static dispatch) |

```rust
import { io, string } from "std";
let s: String = String::from("hello");      // ชนิด String ใช้ชื่อตรง ๆ (struct เป็น global)
s.push_str(", world");                       // ต่อข้อความบน heap (จองใหม่ให้)
io.out("{} (len {})", s.as_str(), s.len());  // hello, world (len 12)
let n: String = String::from_int(42);        // "42"
s.drop(); n.drop();                          // คืนหน่วยความจำเอง (ไม่มี GC)
```
> `str` = ตัวชี้ข้อความคงที่ (อ่านอย่างเดียว) · `String` = บัฟเฟอร์บน heap ที่ต่อ/แก้ได้และต้อง `drop()` เอง

---

## 5. กลไกหน่วยความจำ

นี่คือหัวใจของภาษา low-level — MVS จัดการหน่วยความจำ **ด้วยมือเหมือน C ทุกประการ**
ไม่มี garbage collector, ไม่มี reference counting, ไม่มี destructor/RAII

หน่วยความจำมี 3 บริเวณ:

### 5.1 Stack — อัตโนมัติ (ตัวแปร local)

ตัวแปร local ทุกตัว "จอง" อยู่บน stack frame ของฟังก์ชัน **โดยอัตโนมัติ**:

- **ตอนเข้าฟังก์ชัน (prologue):** จองพื้นที่ทั้งก้อนด้วย `sub rsp, <ขนาดเฟรม>`
- **ตอนออกฟังก์ชัน (epilogue):** คืนทั้งก้อนด้วย `leave` (= `mov rsp, rbp; pop rbp`)

ขนาดเฟรมคำนวณล่วงหน้า (pre-pass `collect_locals`) โดยเดินดูการประกาศตัวแปรทั้งหมด
ตัวแปรแต่ละตัวจองพื้นที่ **อย่างน้อย 8 ไบต์** (ปัดขึ้นเป็นพหุคูณของ 8) อยู่ที่ `[rbp - offset]`

> **อายุ (lifetime):** ตัวแปร stack มีชีวิตตลอดการทำงานของฟังก์ชัน แล้วถูกคืนอัตโนมัติเมื่อ return
> — **ห้ามคืน pointer ไปยังตัวแปร local** (`return &local;`) เพราะหน่วยความจำถูกคืนแล้ว (dangling)

ดูตัวอย่าง assembly จริงในข้อ 6.1

### 5.2 Heap — ด้วยมือ (ผ่าน extern malloc/free)

หน่วยความจำที่ต้องอยู่นานกว่า scope ฟังก์ชัน หรือขนาดไม่รู้ล่วงหน้า ต้องจองบน heap
MVS **ไม่มี `new`/`malloc` ในตัวภาษา** — ใช้ผ่าน `extern` เรียก C runtime:

```rust
extern func malloc(n: usize) -> *u8;   // จองหน่วยความจำ n ไบต์ คืน pointer
extern func free(p: *u8) -> void;      // คืนหน่วยความจำที่จองไว้

func main() -> i8 {
    let buf: *u8 = malloc(64);   // จอง 64 ไบต์บน heap
    *buf = 65;                   // เขียนผ่าน pointer
    let v: u8 = *buf;            // อ่านกลับ (= 65)
    free(buf);                   // ★ ต้องคืนเอง ไม่งั้น memory leak
    return v;
}
```
(โปรแกรมนี้คืน exit code 65 — ทดสอบแล้วใช้งานได้จริง)

**กฎการจัดการ heap (เหมือน C):**
- ทุก `malloc` ต้องมี `free` คู่กัน 1 ครั้ง ไม่งั้น **memory leak**
- ห้ามใช้ pointer หลัง `free` แล้ว (**use-after-free**)
- ห้าม `free` ซ้ำสอง (**double-free**)
- ไม่มีการตรวจขอบเขต (no bounds checking) — เขียนเกินที่จองไว้ = พฤติกรรมไม่กำหนด

> std lib บางตัว (`io.in`, `fs.read`, `net.*recv`) เรียก `malloc` ภายในและ **ยังไม่ได้ free**
> (ยอมให้ leak เพื่อความง่ายของ subset) — โปรแกรมจริงควร wrap แล้ว free เอง

### 5.3 Heap ในโหมด `--nostd` (เขียน OS เอง)

ในโหมด freestanding **ไม่มี `malloc`** (ไม่มี C runtime) — ต้องเขียน allocator เอง
เหมือนตอนเขียน kernel จริง วิธีพื้นฐานคือ **bump allocator** บนพื้นที่หน่วยความจำที่เรารู้ที่อยู่:

```rust
// ตัวอย่างแนวคิด allocator ง่าย ๆ แบบ freestanding (ยังไม่มี global mut array จึงสาธิตผ่าน pointer)
// สมมติเรามี pointer ไปยังหน่วยความจำว่างที่ OS/bootloader เตรียมไว้

func bump_alloc(heap_ptr: *usize, current: usize, size: usize) -> usize {
    // คืน address เดิม แล้วเลื่อน current ไปข้างหน้า size ไบต์ (ผู้เรียกเก็บ current ใหม่)
    return current + size;
}

// เขียนค่าลงที่อยู่หน่วยความจำตรง ๆ (เช่น VGA buffer 0xB8000)
export func poke(addr: *u8, value: u8) -> void {
    *addr = value;
}
```
ใน OS จริง: คุณคุม physical/virtual memory เอง, จัดสรรหน้า (page) เอง, ไม่มีใครมา `free` ให้

### 5.4 หน่วยความจำคงที่ (static): global และ string

- **ตัวแปร global** อยู่ในส่วน `.bss` (จองด้วย `resb <size>` เริ่มต้นเป็น 0)
  ค่าเริ่มต้นถูกตั้ง **ตอนต้นฟังก์ชัน `main`** (ไม่ใช่ฝังในไฟล์)
- **สตริงค่าคงที่** อยู่ในส่วน `.data` (ประกาศเป็นไบต์ + ปิดท้าย 0); ตัวแปร `str` คือ pointer ไปยังไบต์เหล่านั้น

> **ไม่มีชนิด array** ในภาษา — บัฟเฟอร์ทำผ่าน `malloc` + pointer + เลขคณิต pointer (`*(buf + i)`)

---

## 6. กลไกภายในแบบละเอียด

ทุกตัวอย่าง assembly ด้านล่างเป็น **เอาต์พุตจริง** จากคอมไพเลอร์ (ดูเองได้ด้วย `mvs file.mvs -S`)

### 6.1 ตัวแปร + เลขคณิต

ซอร์ส:
```rust
func main() -> i8 {
    let a: i32 = 5;
    let b: i32 = a + 3;
    return b;
}
```
แอสเซมบลีที่ได้ (พร้อมคำอธิบาย):
```asm
main:
    push rbp                  ; เก็บ base pointer เดิม
    mov rbp, rsp              ; ตั้ง frame ใหม่
    sub rsp, 16               ; จองเฟรม 16 ไบต์ (a อยู่ [rbp-8], b อยู่ [rbp-16])
    ; --- let a: i32 = 5 ---
    mov rax, 5                ; ประเมินค่า 5 -> rax
    mov [rbp - 8], eax        ; เก็บลง a (eax = 32-bit เพราะ i32 ตัดเหลือ 4 ไบต์)
    ; --- let b: i32 = a + 3 ---
    lea rax, [rbp - 8]        ; หาที่อยู่ของ a
    movsxd rax, dword [rax]   ; โหลดค่า a ขยายเครื่องหมายจาก 32->64 bit (i32 = signed)
    sub rsp, 16               ; ดันค่า a เก็บใน stack ชั่วคราว (ใช้ 16 ไบต์รักษาการจัดเรียง)
    mov [rsp], rax
    mov rax, 3                ; ประเมิน 3 -> rax
    mov rcx, rax              ; rhs (3) -> rcx
    mov rax, [rsp]            ; ดึง a กลับ -> rax (lhs)
    add rsp, 16
    add rax, rcx              ; a + 3
    mov [rbp - 16], eax       ; เก็บลง b (ตัดเหลือ 32 บิต)
    ; --- return b ---
    lea rax, [rbp - 16]
    movsxd rax, dword [rax]   ; โหลด b
    leave                     ; คืนเฟรม (mov rsp,rbp; pop rbp)
    ret                       ; ค่าใน rax/al = exit code
```
**บทเรียน:**
- ทุกนิพจน์ประเมินลง `rax` (stack machine ง่าย ๆ)
- การดำเนินการสองตัว = ประเมินซ้าย ดันลง stack ชั่วคราว ประเมินขวา แล้วนำกลับมาคำนวณ
- `i32` เก็บด้วย `eax` (4 ไบต์) โหลดด้วย `movsxd` (sign-extend) — นี่คือ "ความกว้างชนิดจริง"
- temp stack ใช้ทีละ **16 ไบต์** เพื่อรักษา 16-byte alignment (สำคัญต่อการเรียกฟังก์ชัน)

### 6.2 การเรียกฟังก์ชัน (win64 ABI)

ซอร์ส:
```rust
func add(x: i32, y: i32) -> i32 { return x + y; }
func main() -> i8 { return add(7, 8); }
```
ฝั่ง callee (`add`):
```asm
mvs_add:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    mov [rbp - 8], rcx        ; พารามิเตอร์ตัวที่ 1 (x) มาจาก rcx
    mov [rbp - 16], rdx       ; พารามิเตอร์ตัวที่ 2 (y) มาจาก rdx
    ... (x + y) ...
    leave
    ret                       ; ผลลัพธ์อยู่ใน rax
```
ฝั่ง caller (`main`) เรียก `add(7, 8)`:
```asm
    mov rax, 7
    sub rsp, 16               ; ดันค่า arg ลง temp stack
    mov [rsp], rax
    mov rax, 8
    sub rsp, 16
    mov [rsp], rax
    sub rsp, 32               ; จอง "shadow space" 32 ไบต์ (กฎ win64)
    mov rax, [rsp + 48]       ; ดึง arg0 (7) -> rcx
    mov rcx, rax
    mov rax, [rsp + 32]       ; ดึง arg1 (8) -> rdx
    mov rdx, rax
    call mvs_add              ; เรียก (ผลลัพธ์กลับมาใน rax)
    add rsp, 32               ; คืน shadow space
    add rsp, 32               ; คืน temp stack ของ args
```
**กฎ win64 ABI ที่เห็นในโค้ด:**
- อาร์กิวเมนต์จำนวนเต็ม 4 ตัวแรกส่งผ่าน **rcx, rdx, r8, r9** (ตัวที่ 5+ วางบน stack)
- ต้องจอง **shadow space 32 ไบต์** ก่อน `call` ทุกครั้ง
- ค่าคืนอยู่ใน **rax**
- `rsp` ต้องจัดเรียง **16 ไบต์** ณ จุด `call`

### 6.3 struct: การจัดวางและการเข้าถึงสมาชิก

ซอร์ส:
```rust
struct P { x: i32; y: i32; }
func main() -> i8 {
    let p: P = P { x: 10, y: 20 };
    return p.x;
}
```
**Layout ของ `P`** (แบบ packed, ปัดขนาดรวมเป็นพหุคูณ 8):
| ฟิลด์ | offset | ขนาด |
|-------|--------|------|
| `x` (i32) | 0 | 4 |
| `y` (i32) | 4 | 4 |
| รวม | | 8 |

แอสเซมบลี (เขียนค่าลง struct literal):
```asm
    sub rsp, 16
    lea rax, [rbp - 8]        ; ที่อยู่ฐานของ p
    sub rsp, 16
    mov [rsp], rax            ; เก็บที่อยู่ฐานไว้ชั่วคราว
    mov rax, 10               ; ค่าของ x
    mov rcx, rax
    mov rax, [rsp]            ; ที่อยู่ฐาน
    mov [rax], ecx            ; เก็บ x ที่ offset 0
    mov rax, 20               ; ค่าของ y
    mov rcx, rax
    mov rax, [rsp]
    add rax, 4                ; เลื่อนไป offset 4 (ฟิลด์ y)
    mov [rax], ecx            ; เก็บ y
    ...
    lea rax, [rbp - 8]
    movsxd rax, dword [rax]   ; อ่าน p.x (offset 0)
```
**บทเรียน:**
- การเข้าถึงสมาชิก = หา **ที่อยู่ฐานของ struct** แล้ว **บวก offset ของฟิลด์**
- struct ที่ใหญ่กว่า 8 ไบต์จองหลาย slot บน stack
- **คัดลอก struct** (`a = b`) ใช้ `rep movsb` (คัดลอกทีละไบต์)
- **คืน struct จากฟังก์ชัน** ใช้เทคนิค *sret*: ผู้เรียกจองที่ว่างไว้ แล้วส่ง pointer ไปยังที่นั้น
  เป็นอาร์กิวเมนต์ซ่อน (rcx) ฟังก์ชันเขียนผลลงไปตรง ๆ

### 6.4 pointer

- `&x` → `lea rax, [rbp - offset]` (เอาที่อยู่ใส่ rax)
- `*p` (อ่าน) → โหลดค่าของ p (= ที่อยู่) แล้ว `mov rax, [rax]` ตามขนาดชนิดที่ชี้
- `*p = v` (เขียน) → คำนวณ v, เอาที่อยู่จาก p, แล้ว `mov [rax], <reg>`

### 6.5 ทศนิยม (float)

เก็บค่า double เป็น **bit-pattern ใน rax** เหมือนจำนวนเต็ม ย้ายเข้า `xmm` เฉพาะตอนคำนวณ:
```asm
    movq xmm1, rax           ; ตัวขวา -> xmm1
    ...
    movq xmm0, rax           ; ตัวซ้าย -> xmm0
    addsd xmm0, xmm1         ; บวกแบบ double
    movq rax, xmm0           ; ผลกลับมาเก็บใน rax
```
ถ้าฝั่งใดเป็น int จะแปลงด้วย `cvtsi2sd` ก่อน

### 6.6 io.out (compiler intrinsic)

ตอนคอมไพล์ `io.out("x = {}", n)` คอมไพเลอร์:
1. แยกวิเคราะห์ format string หา `{}` → นับ placeholder
2. เลือก specifier ตามชนิดของแต่ละ arg (`{}` + int → `%lld`, str → `%s`, char → `%c`, float → `%f`, `{:x}` → `%llx`)
3. สร้าง C format string `"x = %lld\n"` แล้วเรียก `printf`

### 6.7 การตั้งชื่อสัญลักษณ์ (symbol naming)

| สิ่ง | label | เหตุผล |
|------|-------|--------|
| `main` | `main` | ให้ C runtime เรียก |
| ฟังก์ชันทั่วไป | `mvs_<name>` | กันชนกับ libc |
| ฟังก์ชันในโมดูล | `mvs_<module>_<name>` | เช่น `mvs_io_print` |
| method | `mvs_<Struct>_<method>` | เช่น `mvs_Rect_area` |
| `extern` / `export` | `<name>` (ดิบ) | ตรงกับสัญลักษณ์ภาษา C |
| ตัวแปร global | `mvs_g_<name>` | |

### 6.8 tree-shaking
คอมไพเลอร์เริ่มจาก `main` (และฟังก์ชันที่ `export`) แล้วไล่ตามการเรียก (`reach_func`)
**ฟังก์ชันที่ไม่ถูกเรียกจะไม่ถูก gen** — ลดขนาดเอาต์พุต

---

## 6.9 How-to / สูตรสำเร็จ (recipes)

รวมวิธีทำงานที่พบบ่อย — คัดลอกไปปรับใช้ได้เลย

### จองและคืนหน่วยความจำ heap
```rust
extern func malloc(n: usize) -> *u8;
extern func free(p: *u8) -> void;
let buf: *u8 = malloc(256);
*buf = 65;
free(buf);                       // ★ ต้อง free เองเสมอ
```

### ทำ "อาเรย์" ด้วย pointer (ภาษาไม่มีชนิด array)
```rust
extern func malloc(n: usize) -> *u8;
let arr: *i32 = malloc(40);      // 10 ช่อง i32 (10 * 4 ไบต์)
let i: i32 = 0;
while (i < 10) {
    *(arr + i) = i * i;          // pointer arithmetic scale ตาม sizeof(i32) อัตโนมัติ
    i++;
}
io.out("arr[3] = {}", *(arr + 3));   // 9
```

### รับ command-line arguments (argc/argv)
```rust
extern func atoi(s: str) -> i32;
func main(argc: i32, argv: **u8) -> i32 {   // ประกาศ main แบบมีพารามิเตอร์ได้เลย
    let prog: *u8 = *(argv + 0);            // argv[0] = ชื่อโปรแกรม; argv[i] = *(argv + i)
    if (argc >= 2) { let n: i32 = atoi(*(argv + 1)); }
    return 0;
}
```
> CRT เรียก `main(argc, argv)` ตาม C ABI อยู่แล้ว (main เป็นสัญลักษณ์ดิบ) — `argv` เป็นอาเรย์ของ `*u8` (สตริง C)
> ดู [`examples/01_language/args.mvs`](examples/01_language/args.mvs)

### อาเรย์ของ struct (malloc + pointer)
```rust
extern func malloc(n: usize) -> *u8;
struct Pt { x: i32; y: i32; }
let pts: *Pt = malloc(80);           // 10 ช่อง (sizeof(Pt)=8)
let i: i32 = 0;
while (i < 10) {
    (*(pts + i)).x = i;              // เข้าถึงสมาชิกผ่าน pointer ที่เลื่อนแล้ว
    (*(pts + i)).y = i * 10;
    i++;
}
io.out("pts[3] = ({}, {})", (*(pts + 3)).x, (*(pts + 3)).y);   // 3 30
```

### โครงสร้างเชื่อมโยง (linked list / tree) ด้วย self-referential struct
```rust
struct Node { val: i32; next: *Node; }   // ชี้หาตัวเองได้ผ่าน pointer
let n2: Node;  n2.val = 20;  n2.next = 0;     // 0 = NULL
let n1: Node;  n1.val = 10;  n1.next = &n2;
io.out("{} -> {}", n1.val, (*n1.next).val);   // 10 -> 20
```
> โครงสร้าง struct อ้างถึงกันไปมาได้ (คอมไพเลอร์คำนวณ layout แบบ fixpoint) — ลำดับการประกาศไม่สำคัญ

### เปรียบเทียบสตริง (ห้ามใช้ ==)
```rust
extern func strcmp(a: str, b: str) -> i32;
// s == "x" เทียบ "ที่อยู่" ไม่ใช่เนื้อหา! ใช้ strcmp แทน
if (strcmp(name, "admin") == 0) { io.out("welcome admin"); }
```

### เรียกฟังก์ชันคณิตศาสตร์ของ C
```rust
extern func sqrt(x: f64) -> f64;     // double
extern func pow(b: f64, e: f64) -> f64;
io.out("sqrt(2)={}", sqrt(2.0));
```

### ฟังก์ชันทั่วไป (generic) + เลือกตามชนิด (overload)
```rust
func max<T>(a: T, b: T) -> T { if (a > b) { return a; } return b; }
func show(n: i32) -> void { io.out("int {}", n); }
func show(s: str) -> void { io.out("str {}", s); }
func dump<T>(v: T) -> void { show(v); }   // ทำงานกับชนิดที่มี show รองรับ
```

### ให้ภาษา C เรียกโค้ด MVS (ผลิต .obj)
```rust
// lib.mvs (ไม่ต้องมี main)
export func mvs_add(a: i32, b: i32) -> i32 { return a + b; }
```
```
mvs.exe lib.mvs -c          # ได้ lib.obj
clang main.c lib.obj -o app # ลิงก์เข้ากับโปรแกรม C
```

### เขียนโค้ด freestanding (ไม่มี OS — เขียน kernel/bare-metal)
```rust
// ห้าม import std / ห้ามใช้ io.out ในโหมดนี้
export func poke(addr: *u8, value: u8) -> void { *addr = value; }  // เขียน VGA buffer ฯลฯ
```
```
mvs.exe kernel.mvs --nostd  # ได้ kernel.obj ที่ไม่พึ่ง CRT (ลิงก์/ฝังเอง)
```

### อ่าน/เขียนไฟล์ และรับ input
```rust
import { io, fs } from "std";
fs.write("out.txt", "hello");
let text: str = fs.read("out.txt");
let name: str = io.in("your name: ");   // รับหนึ่งคำ
```

### TCP echo server
```rust
import { io, net } from "std";
let server: TcpServer = net.TcpServer("0.0.0.0", 8080);
let conn: TcpSocket = server.accept();
let req: str = conn.recv();
conn.send("HTTP/1.0 200 OK\r\n\r\nhi");
conn.close(); server.close();
```

### bitmask / flags ด้วย bitwise
```rust
let READ: u32 = 1;  let WRITE: u32 = 2;
let perm: u32 = READ | WRITE;
if ((perm & WRITE) != 0) { io.out("writable"); }
perm = perm & ~WRITE;            // ลบบิต WRITE
```

### bitmask / flags ด้วย bitwise (รวม XOR)
```rust
let a: u32 = 12;  let b: u32 = 10;
io.out("xor = {}", a ^ b);       // 6  (^ = XOR; ยกกำลังใช้ **)
io.out("toggle bit2: {}", a ^ 4);
```

### วนลูปผ่านบัฟเฟอร์ด้วย pointer (เหมือน C)
```rust
extern func malloc(n: usize) -> *u8;
let buf: *i32 = malloc(40);
let p: *i32 = buf;
let i: i32 = 0;
while (i < 10) { *p = i * i; p++; i++; }   // p++ เลื่อนทีละ sizeof(i32)=4 ไบต์
// นับจำนวน element ระหว่าง pointer สองตัว
io.out("count = {}", p - buf);   // 10  (ptr - ptr หารด้วย sizeof ให้อัตโนมัติ)
```

### คืนค่าหลายค่าด้วย out-pointer (ภาษาไม่มี tuple)
```rust
func divmod(a: i32, b: i32, q: *i32, r: *i32) -> void {
    *q = a / b;
    *r = a % b;
}
let q: i32 = 0;  let r: i32 = 0;
divmod(17, 5, &q, &r);
io.out("17/5 = {} เศษ {}", q, r);   // 3 เศษ 2
```

### overloading: กฎย่อ
- เลือกตาม **หมวดชนิด** ของ argument: `int / float / str / char / bool / pointer / struct`
- ต่าง**หมวด**ได้ (`show(i32)` vs `show(str)`); ต่างแค่**ความกว้าง**ไม่ได้ (`show(i32)` vs `show(i64)` → error ชัด)
- ผสมกับ generic ได้: `func dump<T>(v: T) { show(v); }` ใช้ได้กับชนิดที่มี `show` รองรับ

### ดูแอสเซมบลีที่ gen (debug)
```
mvs.exe prog.mvs -S --keep      # ได้ prog.asm อ่านได้
```

> หมายเหตุการออกแบบ: **ลำดับการประกาศ struct ไม่สำคัญ** — struct อ้างถึงกันไปมาได้ (คำนวณ layout แบบ fixpoint)

---

## 7. ข้อจำกัดปัจจุบัน

> ส่วนนี้สำคัญ — รู้ขอบเขตก่อนใช้งานจริง (ดู roadmap การแก้ใน `Recap.md`)

### ภาษา / ชนิดข้อมูล
- **ไม่มีชนิด array** — ต้องใช้ pointer + malloc แทน
- bitwise ครบ (`& | ^ ~ << >>`); ยกกำลังใช้ `**` (เช่น `2 ** 8`, `2.0 ** 10`) — ฐานทศนิยมได้ (คูณ double ซ้ำ
  ตามเลขชี้กำลังจำนวนเต็ม); เลขชี้กำลังติดลบยังไม่รองรับ (คืน 1.0)
- **generic + overloading + trait มีครบ** (monomorphization); overload แยก**ความกว้าง int** ได้ (i32 vs i64 เป็นคนละตัว
  โดย literal จำนวนเต็มจับคู่แบบหมวดถ้ามี int overload เดียว); `trait`/`<T: Trait>`/default method มีแล้ว — แต่ยังเป็น
  **static dispatch** เท่านั้น (ยังไม่มี `dyn Trait`/vtable, `where` หลายเงื่อนไข)
- `const` **ยังไม่บังคับใช้จริง** (เขียนทับได้ในระดับ codegen) — เป็นเพียงเจตนา
- **ค่าปริยายของพารามิเตอร์** (`age: u8 = 5`) parse ได้แต่ codegen ไม่ใช้

### ตัวเลข
- **คำนวณใน register 64-bit เสมอ** — ความกว้างชนิดเคารพตอน load/store (เก็บ/โหลด ตัด/ขยายถูก,
  unsigned ใช้ div/setb จริง, มี wrap-around) แต่ overflow กลางนิพจน์เป็นแบบ 64-bit
  (เช่น `let a: u8 = 200; io.out("{}", a + 100)` พิมพ์ 300 — แต่ `let c: u8 = a + 100` เก็บแล้วได้ 44;
  `~` ตัดให้พอดีความกว้างชนิดแล้ว เช่น `~(u8)0 = 255`). ชนิด**ผลลัพธ์**ของเลขคณิต int = ตัวที่กว้างกว่า (เช่น `i32 + i64` → i64)
- **`i128` / `u128`** เก็บ/ส่ง 16 ไบต์ แต่ยังคำนวณเป็น 64-bit (ยังไม่รองรับเลข 128-bit เต็ม)
- **ไม่ตรวจหารด้วยศูนย์** (จะ crash ตอน runtime)
- การแปลง `u64`↔`f64` ค่า ≥ 2^63 ใช้เส้นทาง unsigned แล้ว (ถูกต้อง); `dereference` ของค่าที่ไม่ใช่ pointer เป็น compile error
- **int↔float แปลงโดยปริยายที่ขอบ** (กำหนดค่า/คืนค่า/ส่ง argument) เช่น `let x: f64 = 5` ได้ 5.0; `int as bool` ได้ 0/1
- โครงสร้างที่บรรจุตัวเองแบบ by-value (`struct P { n: P; }`) เป็น compile error (ขนาดอนันต์) — ใช้ `*P` แทน
- generic ที่ `T` ปรากฏเฉพาะใน return/body (ไม่อยู่ใน parameter) จะ instantiate เป็น `i64` โดยปริยาย
- **ทศนิยม (float):** `+ - * /` `**` (ฐาน float, ชี้กำลังจำนวนเต็ม) เปรียบเทียบ และ unary `-` ใช้ได้;
  แต่ **`%` (modulo), `++`/`--`, และ `switch` ใช้กับ float ไม่ได้** → เป็น compile error (ใช้กับจำนวนเต็มเท่านั้น)
  · เลขชี้กำลัง `**` ติดลบยังไม่รองรับ (คืน 1.0)

### struct / ฟังก์ชัน
- struct literal ที่ฟิลด์เป็น struct ซ้อนรับเฉพาะ literal/lvalue
- struct **literal** ตรง ๆ เป็น argument ส่งไม่ได้ (ใช้ตัวแปรชั่วคราว) — แต่ **ผลลัพธ์ struct จากฟังก์ชัน**
  ใช้เป็น rvalue ได้แล้ว (`g(make())`, `make().field`, `make().method()` — คอมไพเลอร์ materialize ลงช่อง temp ให้)
- **generic method** (`impl` ที่มี `<T>`) ยังไม่รองรับ — ใช้ generic function แทน
- จำนวนฟิลด์ struct สูงสุด 64, ฟังก์ชัน/สัญลักษณ์มีลิมิต (ดู `MAX_*` ใน `common.h`)

### กับดักที่พบบ่อย (gotchas — ไม่ใช่บั๊ก แต่ควรรู้)
- **`==` บนสตริงเทียบ "ที่อยู่" ไม่ใช่เนื้อหา** — ใช้ `extern strcmp` (ดู How-to)
- **`char + int` ให้ผลเป็น `char`** (สืบชนิดจากตัวซ้าย) — io.out จะพิมพ์เป็นอักขระ; แปลงเป็น i32 ถ้าต้องการตัวเลข
- io.out: `%` ในข้อความพิมพ์ตรง ๆ (ไม่ต้อง escape); ใช้ `{{` `}}` แทนปีกกาจริง

### หน่วยความจำ
- **ไม่มีการจัดการหน่วยความจำอัตโนมัติ** — ไม่มี GC, ไม่มี RAII/destructor, ต้อง `free` เอง
- **ไม่มีการตรวจขอบเขต** (no bounds checking) — เขียนเกิน = undefined behavior
- std lib บางตัวยัง `malloc` โดยไม่ `free` (ยอม leak ในตัวอย่าง)

### C interop / float
- float ส่ง/รับผ่าน **xmm** ตาม ABI แล้ว — เรียก C math ได้ทั้ง `f64` (`sqrt`/`pow`) และ `f32` (`sqrtf`)
  - ⚠️ ฟังก์ชัน MVS ที่ `export` และมีพารามิเตอร์ `f32` ถูกเรียกจาก C จะอ่านบิตผิด (ใช้ `f64` แทนสำหรับ export)
- **ชื่อ extern/export ห้ามชนคำสงวน NASM** (`abs`, `rel`, `seg`, `wrt`) — assemble ไม่ผ่าน

### เป้าหมาย / รูปแบบเอาต์พุต
- รองรับเฉพาะ **x86-64 + win64 calling convention** + object format **COFF/PE** (`nasm -f win64`)
- **ยังไม่มี ELF / SysV ABI** — OS dev สาย GNU ld/ELF (GRUB multiboot) ต้องรอ backend ใหม่ (roadmap)
- ต้องมี `nasm` + `clang` ในเครื่อง (ลิงก์ผ่าน `-llegacy_stdio_definitions -lws2_32`)

### คอมไพเลอร์
- **หยุดที่ error แรก** (ยังไม่มี error recovery เต็มรูปแบบ)
- การ resolve method ระยะ reachability เป็นแบบ over-approximate (อาจเก็บ method ชื่อซ้ำเกินจริงเล็กน้อย)

---

## 8. ลำดับการเรียนรู้ที่แนะนำ

**ระดับใช้งานภาษา (ดู `examples/` แบ่งเป็นกลุ่ม 01..08):**
1. `examples/demo.mvs` → `01_language/` (hello, types, operators, casts, control, bitwise, args)
2. `02_functions/` (recursion, generics, overload) → `03_structs/` (structs, methods, pointers)
3. `04_traits/` (traits, display) → `05_strings/` (String) → `06_modules/` (import)
4. `07_c_interop/` (extern_c, use_c + .c, export_lib + .c, freestanding)
5. `08_stdlib/` (io_demo, floats, files, net_client/server)

**ระดับเข้าใจคอมไพเลอร์ (อ่านโค้ดตามลำดับ):**
1. `src/token.h` → `src/lexer.c` (token คืออะไร, ตัดคำยังไง)
2. `src/ast.h` (รูปร่างของ AST) → `src/parser.c` (recursive descent)
3. `src/arch/common.c` (ระบบชนิด, struct layout, symbol table)
4. `src/arch/x86_64/win.c` (`gen_expr`, `gen_stmt` — การปล่อย instruction)
5. `src/module.c`, `src/main.c` (โมดูล + pipeline)

**แบบฝึกหัดที่แนะนำ:**
- เขียนโปรแกรม แล้วรัน `mvs file.mvs -S` อ่าน assembly เทียบกับซอร์ส
- ลองทำ bump allocator บน buffer ที่ malloc มา (จองครั้งเดียว แจกจ่ายเอง)
- (ขั้นสูง) เพิ่ม operator ใหม่: แก้ `token.h` → `lexer.c` → `parser.c` → `gen_binop_apply` ใน win.c

---

## 9. อภิธานศัพท์

| คำ | ความหมาย |
|----|----------|
| **Token** | หน่วยเล็กสุดที่มีความหมาย (keyword, ตัวเลข, ตัวดำเนินการ) |
| **Lexer** | ตัวแปลงตัวอักษรเป็น token |
| **Parser** | ตัวประกอบ token เป็นต้นไม้ไวยากรณ์ |
| **AST** | Abstract Syntax Tree — ต้นไม้แทนโครงสร้างโปรแกรม |
| **Codegen** | ตัวแปลง AST เป็นแอสเซมบลี |
| **ABI** | Application Binary Interface — กติกาการเรียกฟังก์ชัน (รีจิสเตอร์, stack) |
| **Stack frame** | พื้นที่บน stack ของฟังก์ชันหนึ่งครั้งเรียก (ตัวแปร local อยู่ที่นี่) |
| **Prologue/Epilogue** | โค้ดต้น/ท้ายฟังก์ชันที่จอง/คืนเฟรม |
| **Shadow space** | พื้นที่ 32 ไบต์ที่ผู้เรียกต้องจองให้ callee (กฎ win64) |
| **sret** | structure return — คืน struct ผ่าน pointer ซ่อนที่ผู้เรียกเตรียมไว้ |
| **rbp / rsp** | base / stack pointer (รีจิสเตอร์ชี้ frame และยอด stack) |
| **RIP-relative** | อ้างที่อยู่สัมพัทธ์กับตัวนับคำสั่ง (`default rel` ใน NASM) |
| **Tree-shaking** | ตัดฟังก์ชันที่ไม่ถูกเรียกออกจากเอาต์พุต |
| **Intrinsic** | ฟีเจอร์ที่คอมไพเลอร์จัดการเอง (เช่น `io.out`) ไม่ใช่ฟังก์ชันธรรมดา |
| **Freestanding** | โค้ดที่ไม่พึ่ง OS/runtime — ใช้เขียน OS/bare-metal |

---

> หากพบว่าเอกสารนี้ไม่ตรงกับโค้ด ให้ยึดโค้ดเป็นหลักและอัปเดตเอกสาร — ดูกฎใน [`Rules.md`](Rules.md)

---

## 📚 เอกสารทั้งหมด (เชื่อมโยงถึงกัน)

> 📍 คุณกำลังอ่าน: **GUIDE.md**

| ไฟล์ | เนื้อหา |
|------|---------|
| [README.md](README.md) | ภาพรวม · ติดตั้ง · วิธีใช้ · ความสามารถ |
| [GUIDE.md](GUIDE.md) | คู่มือภาษาเชิงลึก · ไวยากรณ์ · กลไกหน่วยความจำ · assembly จริง |
| [Rules.md](Rules.md) | กฎ/ปรัชญา freestanding · ABI · ข้อควรระวังสำหรับผู้พัฒนา |
| [Recap.md](Recap.md) | สถานะงาน · roadmap · gotchas |
| [CLAUDE.md](CLAUDE.md) | ไฟล์นำทางสำหรับ AI/ผู้พัฒนา · คำสั่ง · จุดแก้ไข |
| [examples/README.md](examples/README.md) | รายการโปรแกรมตัวอย่างทั้งหมด |
