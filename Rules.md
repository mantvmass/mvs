# Rules — กฎและข้อควรระวังในการพัฒนา MVS Compiler

เอกสารนี้สำหรับ **ผู้พัฒนาและ AI session ถัดไป** อ่านก่อนแก้โค้ดทุกครั้ง
เพื่อรักษาความสอดคล้องของโปรเจกต์ ละเมิดกฎเหล่านี้ = งานจะพังหรือผิดทิศทาง

---

## 0. ปรัชญาการออกแบบ: freestanding by default (ห้ามทำลาย)

**ภาษา MVS ต้องไม่พึ่ง OS API / C runtime โดยปริยาย** เพื่อให้นำไปเขียน OS, bootloader,
firmware หรือ bare-metal ที่ไม่มี OS service ได้

- **แกนภาษา (core) ต้องไม่มี I/O หรือ OS dependency ฝังในคอมไพเลอร์** — ทุกอย่างที่แตะ OS/CRT
  (พิมพ์, ไฟล์, socket, รับ input) อยู่ใน `std/*.mvs` ที่ผู้ใช้ต้อง `import` เองเท่านั้น
- **`io.out` เป็น intrinsic ก็จริง แต่ถูก gate ด้วยการ import io** (ถ้าไม่ import std → ใช้ไม่ได้)
- ใช้ **calling convention ของ x86-64/win64** ในการ gen โค้ด (นี่คือ ABI ของ "การเรียกฟังก์ชน"
  ไม่ใช่ "OS API") — เขียน OS ด้วยสถาปัตยกรรมเดียวกันก็ใช้ convention นี้ได้
- โหมด **`--nostd`** = freestanding จริง: ห้าม import package (std), ไม่ลิงก์ CRT, ผลิต `.obj`
  ให้นำไปลิงก์/ฝังเอง(เช่นใส่ใน kernel image)
- เมื่อเพิ่มฟีเจอร์ใหม่ **ห้ามฝัง dependency กับ OS/CRT ลงในแกนภาษา** — ถ้าจำเป็นต้องเรียก OS
  ให้ทำผ่าน `extern` ใน `std/*.mvs` เท่านั้น
- ⚠️ **เรื่อง object format (สำหรับคนเขียน OS):** `--nostd` ผลิตโค้ด x86-64 ที่ self-contained
  (พิสูจน์ได้: `llvm-nm` ไม่มี undefined symbol) แต่ตอนนี้เป็น **COFF/PE + win64 calling convention**
  (`nasm -f win64`) — ลิงก์ด้วย **LLVM/lld** ได้เลย; ถ้าใช้ **GNU ld + ELF** (GRUB multiboot) ต้องรอ
  backend **ELF/SysV** (`arch/x86_64/sysv.c`) ที่เป็น roadmap ใน `Recap.md` — "win" = convention ไม่ใช่การพึ่ง OS

## 0.5 การทำงานร่วมกับภาษา C (interop)

- **MVS เรียก C:** ผ่าน `extern func name(...) -> T;` (ชื่อสัญลักษณ์ดิบ ตรงกับ C)
- **C เรียก MVS:** ใช้ `export func name(...) -> T { ... }` → ได้สัญลักษณ์ชื่อดิบ + `global`
- **ผลิต object file:** `mvs file.mvs -c` (หรือ `--emit-obj`) → `.obj` เอาไปลิงก์กับโปรแกรม C ได้
- ฟังก์ชันปกติของ MVS ใช้ label `mvs_<...>` (กันชน) — **เฉพาะ `export` เท่านั้นที่ใช้ชื่อดิบ**

## 1. ข้อห้ามเด็ดขาด (Hard constraints)

- ❌ **ห้ามใช้ LLVM** ไม่ว่ากรณีใด — ต้อง gen แอสเซมบลีเองทั้งหมด
- ❌ **ห้ามใช้ flex / bison** — lexer และ parser เขียนด้วยมือ (hand-written) เท่านั้น
- ❌ **ห้ามพึ่ง gcc** — เครื่องเป้าหมายมีแค่ `clang` กับ `nasm` (ไม่มี gcc/ld/as/make ของ GNU บางตัว)
- ❌ **ห้ามฝัง OS/CRT dependency ในแกนภาษา** (ดูข้อ 0)
- ✅ ใช้ **C ล้วน (C99/C11)** มาตรฐาน + libc เท่านั้น (สำหรับตัวคอมไพเลอร์)

## 2. รูปแบบคอมเมนต์ (สำคัญมาก)

- คอมเมนต์ในโค้ด compiler (`src/**`) ต้องเป็น **ภาษาไทยแบบละเอียด** อธิบายว่า
  "ทำอะไร / ทำไม / ทำงานอย่างไร" ไม่ใช่แค่แปลโค้ดตรงตัว
- ทุกฟังก์ชันควรมีคอมเมนต์หัวเรื่องอธิบายหน้าที่
- ทุกไฟล์ขึ้นต้นด้วย block comment อธิบายบทบาทของไฟล์ในภาพรวม
- ⚠️ **ข้อความที่โปรแกรมพิมพ์ออก (output / error message) ต้องเป็นภาษาอังกฤษปกติ**
  — ภาษาไทยใช้เฉพาะคอมเมนต์เท่านั้น
- ⚠️ ห้ามเขียน `/*` หรือ `*/` ซ้อนภายใน block comment (clang เตือน `-Wcomment`)
  ถ้าต้องพูดถึงสัญลักษณ์คอมเมนต์ ให้เลี่ยงคำหรือบรรยายแทน

## 3. Toolchain และ pipeline

```
.mvs → lexer → parser → AST → monomorphize → resolve_overloads → typecheck → codegen → .asm → nasm -f win64 → .obj → clang → .exe
```

- **ลำดับ pass สำคัญ:** `typecheck()` (ใน `generic.c`) ต้องรัน **หลัง** monomorphize + resolve_overloads
  เพราะตอนนั้นชนิดทุกอย่างเป็น concrete แล้ว (generic instance/overload ถูก resolve) — รันก่อนหน้าจะ false-positive
- `typecheck` เป็น **scope-aware** (สร้าง map ทีละขั้นด้วย `add_bind` + push/pop) ออกแบบให้ "เข้มเฉพาะที่ผิดชัดเจน"
  เพื่อไม่ false-positive กับโค้ดระดับล่างที่ถูก (ptr±int, เทียบ ptr กับ 0, ผสมความกว้าง int, `str`↔`*u8`) — แก้กฎต้องรัน sweep examples ทั้งหมด
- `typecheck` ตรวจ **argument ของการเรียก** (ชนิด+จำนวน) ทั้งฟังก์ชันตรงและ method (`argoff=1` ข้าม self)
  — เว้น extern (variadic C), generic template, และ namespaced free-func (`ns.f`)
- การแปลงชนิดทำผ่าน **`as` (ND_CAST)** เท่านั้น — ไม่มี implicit narrowing/float↔int นอกจากที่เลขคณิตผสมทำให้โดยตั้งใจ
- ⚠️ **`type_of`/`infer` ของ `ND_BINARY` ต้อง promote (order-independent)**: `int <op> float` → float (กว้างสุด),
  `int <op> int` → ชนิด int ที่**กว้างกว่า** (`int_rank`/`type_size`), `ptr±int` → ptr, `ptr-ptr` → isize
  (อย่าคืน `type_of(lhs)` ตรง ๆ — จะเลือก format/overload ผิดและขึ้นกับลำดับตัวถูกดำเนินการ)
- ⚠️ **scan_calls ต้อง children-first** (resolve inner generic call ก่อน) ไม่งั้น generic ซ้อน `f(g(x))` จะอนุมาน outer
  จาก template ที่ยังไม่ instantiate → ได้ชนิด `T` ดิบ
- ⚠️ **บัฟเฟอร์ที่สร้างจากชื่อ identifier**: identifier ถูก cap ที่ 100 ตัวอักษร (lexer); label ใช้ `LABEL_MAX` (720) + snprintf;
  signature ใช้ `SIGCAP` (256) + `sig_append` bounded; mangled instance ใช้ snprintf — อย่าใช้ `sprintf`/`strcpy` ดิบกับชื่อผู้ใช้
- ⚠️ **`~` ต้อง mask ผลตามความกว้างชนิด** (เช่น `~(u8)0` = 255); cast u64↔f64 ค่า ≥ 2^63 ต้องใช้เส้นทาง unsigned (cvtsi2sd เป็น signed)
- ⚠️ **int↔float ต้อง coerce ที่ "ขอบ"** (var init/assign/return/argument) ด้วย `gen_coerce_num` — backend แปลงเฉพาะในนิพจน์ผสม
  ไม่งั้น `let x: f64 = 5` / `takesF(5)` / `return 5` (จากฟังก์ชัน f64) จะได้ bit-pattern ขยะ; `int as bool` ต้อง normalize 0/1
- ⚠️ **label ของ global ใช้ prefix `mvs_gv_`** (ไม่ใช่ `mvs_g_`) — กันชนกับ label ฟังก์ชัน/method `mvs_<ns>_<name>` (เช่น struct ชื่อ `g`)
- ⚠️ **parser มี depth guard** (`MAX_PARSE_DEPTH` ใน `parse_expr`/`parse_block`/`parse_unary`) กัน stack overflow จาก input ซ้อนลึก
- ⚠️ **struct บรรจุตัวเอง by-value = error** (`struct_has_cycle` ใน `layout_structs`) + `expand_struct` มี depth cap — กัน loop ไม่รู้จบ
- ⚠️ **signature ของ overload (`width_code`/`cat_code`) ต้องรวม "ความลึก pointer + pointee"** (`*i32`→`pi32`) ไม่งั้น
  `f(*i32)` กับ `f(*u8)` ชนกัน (false-duplicate / label ซ้ำ)
- ⚠️ ทุก pass วิเคราะห์ (`typecheck`/`scan_ov`/`scan_calls`) ต้อง `seed_globals()` ก่อนใส่ params — ไม่งั้น global ถูกอนุมานเป็น i64
  และต้องสแกน **global initializer** ด้วย (ไม่ใช่แค่ body ของฟังก์ชัน)
- io.out ที่พิมพ์ **struct ซึ่งคืนจากฟังก์ชัน** (`io.out("{}", mk())`) ต้อง materialize ครั้งเดียวผ่าน `ND_FRAMEREF`
  (อ้างช่อง temp ที่ `collect_struct_temps` จองไว้) — ไม่งั้นแต่ละฟิลด์ gen_call ซ้ำ → side-effect ซ้ำ/ค่าผิด

- **build คอมไพเลอร์:** `clang ... -o mvs.exe` (ดู Makefile)
- **assemble:** `nasm -f win64 x.asm -o x.obj`
- **link:** `clang x.obj -o x.exe -llegacy_stdio_definitions -lws2_32`
  - `legacy_stdio_definitions` จำเป็นเพราะ UCRT ทำ `printf`/`scanf` เป็น inline (ไม่งั้น `scanf` ลิงก์ไม่ผ่าน)
  - `ws2_32` สำหรับโมดูล net (ผูกไว้เสมอ ไม่กระทบโปรแกรมที่ไม่ใช้)
- รูปแบบแอสเซมบลี = **NASM Intel syntax** + `default rel` (RIP-relative)

## 4. win64 ABI — กฎที่พลาดแล้วพังเงียบ ๆ

- อาร์กิวเมนต์จำนวนเต็ม 4 ตัวแรก: **rcx, rdx, r8, r9**; ตัวที่ 5+ วางบน stack เหนือ shadow space (รองรับแล้ว)
- อาร์กิวเมนต์ทศนิยม: **xmm0–xmm3** ตามตำแหน่ง (variadic เช่น printf ใส่ทั้ง GPR และ xmm)
- ต้องจอง **shadow space 32 ไบต์** ก่อน `call` ทุกครั้ง (`sub rsp,32` / `add rsp,32`)
- **rsp ต้องจัดเรียง 16 ไบต์** ณ จุดที่ทำ `call`
  - ที่ entry ฟังก์ชัน rsp ≡ 8 (mod 16); `push rbp` ทำให้ ≡ 0
  - ขนาดเฟรมต้องเป็น **พหุคูณของ 16** เพื่อรักษาการจัดเรียง
  - stack ชั่วคราว (temp push) ใช้ **16 ไบต์ต่อครั้ง** (`sub rsp,16`) ไม่ใช่ 8
    เพื่อไม่ให้การจัดเรียงพังตอนมี call ซ้อนในนิพจน์ — **อย่าเปลี่ยนเป็น push/pop 8 ไบต์**
- ค่าคืนจากฟังก์ชันอยู่ใน **rax**; `main` ใช้ rax/al เป็น exit code
- ⚠️ **รีจิสเตอร์ callee-saved (nonvolatile) ของ win64: `rbx, rbp, rdi, rsi, rsp, r12-r15`**
  ฟังก์ชันที่เรา gen **ห้าม clobber** โดยไม่บันทึก/คืนค่า — โดยเฉพาะ **`rsi`/`rdi`**
  (ที่หลายคนพลาดเพราะ `rep movsb` ใช้มัน) ถ้าทำเสีย โค้ด C ที่เรียก MVS (export) จะพัง
  - การ copy หน่วยความจำ ให้ใช้ `gen_memcpy()` (byte loop ด้วย `r10/r11/rcx/rax` ซึ่งเป็น volatile) — **อย่าใช้ `rep movsb`**
  - รีจิสเตอร์ scratch ที่ใช้ได้อิสระ (volatile): `rax, rcx, rdx, r8, r9, r10, r11, xmm0-5`

## 5. หลักการตั้งชื่อ label (อย่าทำให้ชนกับ C runtime)

| สิ่ง                      | รูปแบบ label          | หมายเหตุ                          |
|---------------------------|-----------------------|----------------------------------|
| ฟังก์ชัน `main`           | `main`                | export ให้ CRT เรียก             |
| ฟังก์ชันผู้ใช้ (ไม่มี ns)  | `mvs_<name>`          | กันชนกับ libc                    |
| ฟังก์ชันในโมดูล (มี ns)   | `mvs_<ns>_<name>`     | เช่น io.out -> `mvs_io_out`       |
| foreign function (extern) | `<name>` (ดิบ)        | เช่น `printf` ต้องตรงกับ C runtime |
| ตัวแปร global             | `mvs_g_<name>`        |                                  |
| สตริงค่าคงที่             | `mvs_str_<idx>`       | ประกาศใน `.data`                 |
| label ภายใน (ลูป/if/switch)| `.L<prefix><id>`      | local label, `id` จาก `new_label()` |

> ดูฟังก์ชัน `func_label_of()` และ `find_func()` ใน `arch/common.c` เป็นต้นฉบับ

## 5.5 หลักการที่เพิ่มเข้ามา (struct / pointer / float / ABI)

- **ความกว้างชนิด:** ตัวแปรจองตามขนาดจริง (`type_size`); store ตัดตามขนาด (al/ax/eax/rax),
  load ขยายด้วย `movsx`/`movzx` ตามเครื่องหมาย (`is_signed_type`) — แต่ยังคำนวณใน register 64-bit
- **lvalue/address:** `gen_addr()` ใส่ "ที่อยู่" ลง rax ใช้ร่วมกับ member access, `&`, `*`, การกำหนดค่า
- **struct คืนค่า (sret):** ฟังก์ชันคืน struct ใช้ hidden pointer ใน rcx, พารามิเตอร์จริงเลื่อนไปหนึ่งช่อง;
  ค่า struct (literal/copy/ผลฟังก์ชัน) เขียนผ่าน `gen_store_struct()` (rep movsb สำหรับ copy)
- **arg เกิน 4 ตัว:** ตัวที่ 5+ วางบน stack เหนือ shadow space; callee อ่านจาก `[rbp + 48 + (pos-4)*8]`
- **float:** เก็บเป็น **bit-pattern ของ double ใน rax** เหมือน int; ย้ายเข้า `xmm` เฉพาะตอนคำนวณ
  (`movq`/`cvtsi2sd` → `addsd`/`ucomisd` → `movq` กลับ); **`f32` เก็บ 4 ไบต์จริง** (load `movd`+`cvtss2sd`, store `cvtsd2ss`+`movd`)
  - ⚠️ printf เป็น variadic: ค่า float ต้องใส่ทั้ง GPR และ `xmm<n>` (ดู io.out)
  - float กับฟังก์ชัน C ภายนอกใช้ได้สองทาง (arg ใน `xmm<pos>`, คืนทาง `xmm0`); **f32 ข้ามขอบ C**:
    export ที่รับ f32 ทำ `cvtss2sd` ตอนรับ, คืน f32 ทำ `cvtsd2ss` ก่อน ret (C ใช้ single, ฝั่ง MVS ใช้ double)
  - ⚠️ **ทุก operation บน float ต้องจัดการใน xmm เอง** — อย่าใช้ instruction จำนวนเต็มกับ bit-pattern:
    `**` = ลูป `mulsd`, unary `-` = `xor` sign-bit (ไม่ใช่ `neg`); `%`/`++`/`--`/`switch` บน float = **compile error** (typecheck/codegen กัน)

## 5.6 Method ของ struct (impl) — แบบ Rust

- ประกาศด้วย `impl StructName { func method(self: *StructName, ...) -> T {...} }`
- method ถูกติด `ns = ชื่อ struct` (สำหรับ label `mvs_<Struct>_<method>`) และ `is_method = 1`
- เรียกด้วย `obj.method(args)` — คอมไพเลอร์ฉีด `self` (เป็น pointer ไปยัง obj) เป็นอาร์กิวเมนต์แรก
  - ถ้า `obj` เป็น struct value → ส่ง `&obj`; ถ้าเป็น pointer อยู่แล้ว → ส่งค่าตรง ๆ
- **chaining** ทำได้เมื่อ method คืน pointer (`*Struct`) เช่น builder pattern `v.setX(1).setY(2)`
- ⚠️ **ความต่างระหว่าง `ns` กับ `mod`** (ในโครงสร้าง `Node`) — ห้ามสับสน:
  - `ns` = namespace ของ **ป้ายชื่อสัญลักษณ์** (module สำหรับ func ปกติ, ชื่อ struct สำหรับ method)
  - `mod` = namespace ของ **โมดูลที่สังกัด** ใช้ resolve การเรียกแบบไม่ระบุชื่อ (`g->cur_ns = fn->mod`)
  - ถ้า method (ns = struct) ใช้ ns เป็น cur_ns จะเรียกตัวเองวน (เช่น method `send` เรียก extern `send`)
- การเรียกแบบไม่ระบุชื่อภายใน method resolve ในโมดูล แล้วค่อย global — เรียก method พี่น้องต้องใช้ `self.x()`

## 5.7 Trait + associated function + generic ผูก trait

- **associated function** = func ใน `impl` ที่**ไม่มี `self`** เรียกด้วย `Type::func(..)` (`::` = `TK_COLONCOLON`)
  - AST เหมือน method call ทุกอย่าง (`ND_MEMBER` base = ชื่อชนิด); gen_call แยกแยะจากชนิด base:
    base เป็นตัวแปร struct → ฉีด self (method), base เป็นชื่อชนิด/โมดูล → ไม่ฉีด self (associated/namespaced)
- **trait** (`ND_TRAIT`) เก็บแค่ signature; **`impl Trait for Type`** สร้าง method ปกติ (ns=Type)
  + เครื่องหมาย **`ND_TRAIT_IMPL`**(name=trait, type_name=type) ไว้ตรวจ
- **`<T: Trait>`** เก็บใน `Node.gen_bound[]`; ตรวจตอน **monomorphize** ว่าชนิดจริง impl trait (มี `ND_TRAIT_IMPL`)
  — ไม่ผ่าน → `monomorphize()` คืน error count > 0 → main หยุด
- **dispatch เป็น static ฟรี**: หลัง monomorphize ชนิด T เป็น concrete แล้ว `x.method()` resolve ตามชนิดเอง
  — ไม่มี vtable; trait จึงเป็น "syntax + การตรวจสอบ" เป็นหลัก (codegen ไม่ต้องรู้จัก trait)
- ⚠️ codegen/typecheck **ข้าม** `ND_TRAIT`/`ND_TRAIT_IMPL` (ไม่ใช่ ND_FUNC) — อย่าเผลอ gen เป็นฟังก์ชัน
- ตรวจเพิ่ม: `impl` ต้องทำ method ครบตาม trait + trait ที่อ้างถึงต้องมีจริง (`check_trait_impls` ใน generic.c)

## 6. ข้อจำกัดของ codegen ปัจจุบัน

- **scope shadowing มีแล้ว** (visibility stack `g->visible[]` push/pop ต่อ block; `find_var` ค้นล่าสุดก่อน
  ตัวแปรแต่ละตัวจอง slot แยกแม้ชื่อซ้ำ) — และ pass วิเคราะห์ชนิดทั้งหมด (`typecheck`/`scan_ov`/`scan_calls`)
  เป็น **scope-aware** แล้ว (สร้าง var-map ทีละขั้นด้วย `add_bind` + push/pop ต่อ block) จึงอนุมานชนิดถูกเมื่อมี shadowing
  ⚠️ เดิมใช้ flat map (`collect_vars`) ทำให้ overload/generic เลือกผิด → segfault เมื่อ shadow ต่างชนิด — แก้แล้ว
- คำนวณยังทำใน register 64-bit (ความกว้างเคารพเฉพาะตอน load/store) — overflow กลางคันเป็นแบบ 64-bit
- struct ส่ง **by-value เป็น parameter** ทำได้แล้ว (caller ส่ง &arg, callee copy); และ **ผลลัพธ์ struct จากฟังก์ชัน
  ใช้เป็น rvalue ได้** (`g(make())`, `make().field`) ผ่านช่อง temp ที่จองโดย `collect_struct_temps`
- struct literal ที่ฟิลด์เป็น struct ซ้อนรับเฉพาะ literal/lvalue (ดู `gen_store_struct`)
- **ชื่อ extern/export ห้ามชนคำสงวนของ NASM** (เช่น `abs`, `rel`, `seg`, `wrt`) เพราะใช้ชื่อสัญลักษณ์ดิบ
  → จะเกิด nasm syntax error; เลี่ยงโดยตั้งชื่ออื่น (เช่น C `abs` ใช้ไม่ได้ ต้องห่อด้วยฟังก์ชันชื่ออื่น)

## 6.5 ระบบโมดูล (import) — กฎและพฤติกรรม

- การ resolve import อยู่ใน `src/module.c` (front-end เดิม parse ทีละไฟล์)
- **3 รูปแบบ — "path เป็นตัวกำหนด"** (ดู `handle_import`):
  - `import { io } from "std"` — path = **package เปล่า** → ชื่อใน `{}` = **submodule** โหลดเป็น **namespace** (`io.xxx`)
  - `import { S } from "std/x"` / `from "./f.mvs"` — path = **โมดูลเจาะจง** → ชื่อใน `{}` = **สัญลักษณ์** ดึงตรง ๆ (ns="")
  - `import alias from "std/x"` / `from "./f.mvs"` — **ไม่มี `{}`** → ทั้งโมดูลเป็น **namespace = alias** (เก็บใน `imp->name`)
  - แยก "ไฟล์ relative" จาก "package submodule": ลงท้าย `.mvs` = ไฟล์ (เทียบ base_dir), ไม่ลงท้าย = `<stddir>/<sub>.mvs`
- **ตัวตรวจของ import (ทั้งหมด error ทันที):**
  - symbol import: ชื่อต้องมีจริงในโมดูล (`collect_defs`/`defs_has` → "no exported symbol")
  - ชื่อซ้ำ struct/trait/func(ns+ชื่อ+sig) → `check_duplicates()` ใน `generic.c` (เรียกก่อน monomorphize; overload ต่างชนิดไม่นับ; extern ซ้ำได้)
  - namespace/alias ผูกซ้ำคนละโมดูล → `register_ns` (`Loader.ns_name/ns_canon`)
  - **circular import** → `Loader.loading[]` (stack ระหว่างโหลด); `loaded[]` ลงทะเบียนเมื่อโหลด**เสร็จ** (รองรับ diamond)
- โฟลเดอร์ std หาได้จาก env `MVS_STD` ก่อน ไม่งั้นใช้ `<โฟลเดอร์ของ mvs.exe>/std`
- **std เป็นไฟล์ MVS จริง** (`io`/`fs`/`net`/`string`/`fmt`) ต้อง `import` ก่อนใช้ ทำงานผ่าน `extern` เรียก C runtime / Winsock
- ⚠️ **struct/trait ยังไม่ถูก namespace** (เป็น global เสมอ) — ชนิดจึงควรนำเข้าด้วย symbol import (`import { String } from "std/string"`)
  ส่วน alias/namespace ใช้ได้ผลกับ **free function** เท่านั้น (method/associated ผูกกับชื่อ struct อยู่แล้ว)
- **`io.out` เป็น compiler intrinsic** (เหมือน `println!` ของ Rust): แยกวิเคราะห์ `{}`/`{:x}` ตอนคอมไพล์
  และเลือก format ตามชนิด arg — ดู `build_c_format()` ใน `arch/common.c`; gate ด้วยว่า import io แล้วหรือยัง
- **tree-shaking:** gen เฉพาะฟังก์ชันที่เข้าถึงได้จาก main (`reach_func`/`reach_node`) — ฟังก์ชันที่ไม่ถูกเรียกถูกตัด
- ชื่อซ้ำข้ามโมดูล (ns+ชื่อ+sig เดียวกัน) ถูกจับโดย `check_duplicates` แล้ว (extern ยกเว้น — dedup ภายหลัง)

## 7. การออกแบบให้รองรับหลายสถาปัตยกรรม (ห้ามทำลาย)

โครงสร้าง backend แยกเป็น **ส่วนกลาง** กับ **ส่วนเฉพาะสถาปัตยกรรม**:

```
src/arch/
  common.h / common.c       ← ส่วนกลาง (ไม่ขึ้นกับ arch): ระบบชนิด, struct layout,
                               symbol table, การจองตัวแปร, reachability, build_c_format
  x86_64/win.c              ← เฉพาะ x86-64 Windows: ปล่อย instruction + win64 ABI
  (อนาคต) x86_64/linux.c    ← ABI ต่าง (args rdi,rsi,rdx,rcx,r8,r9, ไม่มี shadow space)
  (อนาคต) arm64/linux.c     ← register/instruction ชุดใหม่
```

- `src/lexer`, `src/parser`, `src/ast`, `src/module` **ต้องไม่ผูกกับสถาปัตยกรรมใด ๆ**
- **ส่วนกลาง (`common.c`) ห้ามปล่อย instruction** — มีแต่ตรรกะ (type/struct/symtab/reachability)
  ฟังก์ชันที่ปล่อย asm (gen_expr/gen_stmt/gen_call/gen_func ฯลฯ) อยู่ในไฟล์ backend เท่านั้น
- เพิ่ม arch/os ใหม่ = สร้าง `src/arch/<arch>/<os>.c` ที่ `#include "../common.h"` แล้วเขียน
  เฉพาะส่วนปล่อยโค้ด + เพิ่มค่าใน `TargetArch` (codegen.h) + เพิ่ม case ใน `codegen_generate()`
  — **ห้ามแก้ front-end และพยายาม reuse `common.c` ให้มากที่สุด**
- `type_size` ใน common สมมติ pointer 8 ไบต์ (เป้าหมาย 64-bit) — ถ้าทำ 32-bit ต้อง parameterize

## 8. ขั้นตอนก่อนถือว่า "เสร็จ"

1. `make` ต้องผ่านโดย**ไม่มี warning** (เปิด `-Wall -Wextra` แล้ว; deprecation ปิดด้วย flag) — ห้ามทิ้ง dead code
2. คอมไพล์และ**รัน** `examples/hello.mvs` และ `examples/demo.mvs` ให้ผลถูกต้อง
3. ถ้าเพิ่มฟีเจอร์ ให้เพิ่มตัวอย่างใน `examples/` ที่พิสูจน์ว่าใช้ได้จริง
4. ตรวจ output ด้วยตา — codegen ผิดมักผ่าน assemble/link แต่ได้ผลรันผิด

## 9. สไตล์โค้ด

- ตามสไตล์ไฟล์รอบข้าง: indent 4 spaces, วงเล็บปีกกาเปิดบรรทัดเดียวกัน
- โครงสร้างข้อมูลของ backend รวมไว้ใน `Gen` struct เดียว (คอมไพเลอร์ทำงานครั้งละไฟล์)
- รายงาน error ผ่าน `fprintf(stderr, ...)` เป็นภาษาอังกฤษ พร้อมตำแหน่ง (ไฟล์:บรรทัด:คอลัมน์)

---

## 📚 เอกสารทั้งหมด (เชื่อมโยงถึงกัน)

> 📍 คุณกำลังอ่าน: **Rules.md**

| ไฟล์ | เนื้อหา |
|------|---------|
| [README.md](README.md) | ภาพรวม · ติดตั้ง · วิธีใช้ · ความสามารถ |
| [GUIDE.md](GUIDE.md) | คู่มือภาษาเชิงลึก · ไวยากรณ์ · กลไกหน่วยความจำ · assembly จริง |
| [Rules.md](Rules.md) | กฎ/ปรัชญา freestanding · ABI · ข้อควรระวังสำหรับผู้พัฒนา |
| [Recap.md](Recap.md) | สถานะงาน · roadmap · gotchas |
| [CLAUDE.md](CLAUDE.md) | ไฟล์นำทางสำหรับ AI/ผู้พัฒนา · คำสั่ง · จุดแก้ไข |
| [examples/README.md](examples/README.md) | รายการโปรแกรมตัวอย่างทั้งหมด |
