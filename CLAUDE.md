# CLAUDE.md

แนวทางสำหรับ Claude Code (และ AI session อื่น ๆ) เมื่อทำงานในโปรเจกต์นี้

> **อ่านก่อนเริ่มเสมอ:** [`Rules.md`](Rules.md) = กฎที่ห้ามละเมิด · [`Recap.md`](Recap.md) = สถานะงานและสิ่งที่ค้าง
> · [`GUIDE.md`](GUIDE.md) = คู่มือภาษาเชิงลึก (ไวยากรณ์ + กลไกหน่วยความจำ + assembly จริง)

## โปรเจกต์นี้คืออะไร

คอมไพเลอร์สำหรับภาษา **MVS** (ภาษาระดับล่างคล้าย C แต่ syntax อ่านง่ายกว่า)
เขียนด้วย **C ล้วน** สร้างแอสเซมบลี **x86-64 Windows (NASM)** โดยตรง
**ไม่ใช้ LLVM, ไม่ใช้ flex/bison** — lexer/parser เขียนมือทั้งหมด

## ข้อจำกัดสำคัญที่สุด (รายละเอียดเต็มใน Rules.md)

1. **ห้าม LLVM / flex / bison** — gen แอสเซมบลีเอง, lexer/parser เขียนมือ
2. **คอมเมนต์โค้ดเป็นภาษาไทยละเอียด** แต่ **output/error เป็นภาษาอังกฤษ**
3. Toolchain ที่มี: **clang + nasm เท่านั้น** (ไม่มี gcc/ld/flex/bison/make ของ GNU ครบชุด)
4. โครงสร้างต้องรองรับหลายสถาปัตยกรรม — front-end ห้ามผูกกับ x86

## คำสั่งหลัก

```powershell
# build คอมไพเลอร์
make
#   เทียบเท่า: clang -Wall -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc `
#              src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/codegen.c `
#              src/arch/common.c src/arch/x86_64/win.c -o mvs.exe

# คอมไพล์โปรแกรม MVS → .exe แล้วรัน
.\mvs.exe examples\demo.mvs
.\examples\demo.exe

# debug codegen: ดูแอสเซมบลีที่ gen ออกมา (ไม่เรียก nasm/clang)
.\mvs.exe examples\demo.mvs -S --keep
```

## สถาปัตยกรรมโค้ด (pipeline)

```
.mvs → [lexer] → tokens → [parser] → AST → [codegen driver] → [arch backend] → .asm
                                                                          ↓
                                        nasm -f win64 → .obj → clang → .exe
```

| ไฟล์                      | หน้าที่                                              |
|---------------------------|------------------------------------------------------|
| `src/token.h`             | นิยามชนิด token                                       |
| `src/lexer.{h,c}`         | ตัดคำ (hand-written tokenizer)                        |
| `src/ast.{h,c}`           | โครงสร้าง AST (`Node` เดียวแยกด้วย `kind`) + ตัวช่วย   |
| `src/parser.{h,c}`        | recursive-descent parser                             |
| `src/module.{h,c}`        | ระบบโมดูล: resolve `import` ข้ามไฟล์ + package std    |
| `src/codegen.{h,c}`       | ตัวขับ เลือก backend ตาม `TargetArch` (ไม่ผูก arch)   |
| `src/arch/common.{h,c}`   | ส่วนกลาง backend (ไม่ขึ้น arch): type, struct, symtab, tree-shaking |
| `src/arch/x86_64/win.c`   | backend เฉพาะ x86-64 win64 (ปล่อย NASM, stack-machine) |
| `src/main.c`              | CLI: ขับ pipeline ทั้งหมด                             |
| `std/*.mvs`               | standard library เขียนด้วย MVS (`io`/`string`/`fmt`/`fs`/`net`) |

## จะเพิ่มฟีเจอร์ต้องแตะไฟล์ไหน

- token/keyword → `token.h` + `lexer.c` (ตาราง `KEYWORDS`)
- ไวยากรณ์ → `parser.c` (+ `ND_*` ใน `ast.h`)
- type checking / trait bound / generic / overload → `generic.c` (รัน pass หลัง parse: monomorphize → resolve_overloads → typecheck)
- การ gen instruction → `arch/x86_64/win.c` · ตรรกะกลาง (type/struct/symtab/tree-shake/io.out format) → `arch/common.c`
- พฤติกรรม import → `module.c` · ฟังก์ชัน stdlib → `std/*.mvs`
- arch ใหม่ → สร้าง `arch/<arch>/<os>.c` (reuse `common.c`) + `TargetArch` (`codegen.h`) + case ใน `codegen.c`

## เช็คก่อนถือว่าเสร็จ

1. `make` ผ่านไม่มี warning
2. คอมไพล์ **และรัน** `examples/demo.mvs` + `examples/01_language/types.mvs` ได้ผลถูกต้อง
3. เพิ่มฟีเจอร์ → เพิ่มตัวอย่างใน `examples/` พิสูจน์ว่าใช้ได้จริง

## สถานะฟีเจอร์ (ดูรายละเอียดใน Recap.md)

**ทำแล้ว:** struct + **method/impl + chaining** · pointer · ความกว้าง int จริง · **f32 (4 ไบต์จริง)**/f64 (SSE) ·
switch/do-while · io.out แบบ Rust (`{}`/`{:x}` + **พิมพ์ struct** + arg ไม่จำกัด) · import + extern ·
stdlib `io`/`fs`/`net`/**`string`** + `io.in` · args > 4 · tree-shaking · sret · **C interop (`export`/`-c`)** ·
**`--nostd` freestanding** · **generics + overloading (แยกความกว้าง int)** · **scope shadowing** ·
struct by-value param + **struct-returning call เป็น rvalue** · **type checking** + **`as` cast** ·
**trait + associated function (`Type::new`) + `<T: Trait>` + default method** · **`String` (heap) + `String::from`/`from_int`** ·
**float xmm กับ C สองทาง (f32 single↔double)** · **function pointer (`func(...) -> T` เป็นค่า + indirect `call rax`)**

**ยังเหลือ:** i128/u128 คำนวณ 128-bit เต็ม · trait dynamic dispatch (`dyn`/vtable) + `where` หลายเงื่อนไข ·
ชนิด array จริง · io.out เป็น library (variadic+reflection) · backend ARM64/Linux

## ข้อควรระวังเฉพาะ (ดูเต็มใน Rules.md)

- **ปรัชญา freestanding by default** (Rules ข้อ 0): แกนภาษาห้ามพึ่ง OS/CRT — ทุกอย่างที่แตะ OS อยู่ใน `std/*.mvs` (opt-in)
- C interop: `export func` = ชื่อสัญลักษณ์ดิบ + `global`; `-c` ผลิต `.obj`; `--nostd` = freestanding obj
- method: `ns` = ชื่อ struct (label), `mod` = โมดูล (resolve การเรียกภายใน) — อย่าสับสน (ดู Rules 5.6)
- link ต้องมี `-llegacy_stdio_definitions -lws2_32`; ชื่อ extern/export ห้ามชนคำสงวน NASM (`abs` ฯลฯ)
- float เก็บเป็น bit-pattern ใน rax, เข้า xmm เฉพาะตอนคำนวณ; `io.out` เป็น compiler intrinsic

---

## 📚 เอกสารทั้งหมด (เชื่อมโยงถึงกัน)

> 📍 คุณกำลังอ่าน: **CLAUDE.md**

| ไฟล์ | เนื้อหา |
|------|---------|
| [README.md](README.md) | ภาพรวม · ติดตั้ง · วิธีใช้ · ความสามารถ |
| [GUIDE.md](GUIDE.md) | คู่มือภาษาเชิงลึก · ไวยากรณ์ · กลไกหน่วยความจำ · assembly จริง |
| [Rules.md](Rules.md) | กฎ/ปรัชญา freestanding · ABI · ข้อควรระวังสำหรับผู้พัฒนา |
| [Recap.md](Recap.md) | สถานะงาน · roadmap · gotchas |
| [CLAUDE.md](CLAUDE.md) | ไฟล์นำทางสำหรับ AI/ผู้พัฒนา · คำสั่ง · จุดแก้ไข |
| [examples/README.md](examples/README.md) | รายการโปรแกรมตัวอย่างทั้งหมด |
