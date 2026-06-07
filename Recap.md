# Recap — สถานะงานและสิ่งที่ค้างไว้

เอกสารสรุปสำหรับ **AI session / ผู้พัฒนาคนถัดไป** ให้รับช่วงงานต่อได้ทันที
อ่านคู่กับ [`Rules.md`](Rules.md) (กฎที่ห้ามละเมิด)

อัปเดตล่าสุด: 2026-06-08

---

## สถานะปัจจุบัน: ✅ subset พื้นฐานทำงานครบ pipeline

คอมไพเลอร์รับ `.mvs` → สร้าง `.exe` ที่รันได้จริงบน Windows x86-64
ทดสอบแล้ว `examples/hello.mvs` และ `examples/demo.mvs` ให้ผลถูกต้อง

> **recheck รอบ shadowing:** pass วิเคราะห์ชนิด (`typecheck`/`scan_ov`/`scan_calls`) เดิมใช้ flat var-map
> ทำให้ **shadowing ต่างชนิดเลือก type/overload ผิด → segfault**; แก้เป็น **scope-aware** (`add_bind` + push/pop), ลบ `collect_vars`
>
> **recheck รอบ float/switch:** จับบั๊ก "ผลผิดเงียบ ๆ" 5 จุด — แก้ครบ:
> 1. `2.0 ** 3` คืน 0 (ตกไป int loop) → ทำ float power (คูณ double ใน xmm) · 2. `-3.5` (unary) ทำ `neg` บน double bits
> → พลิก sign bit · 3. float `%` → compile error (SSE ไม่มี) · 4. float `++`/`--` → compile error · 5. `switch` บน float → compile error
>
> **recheck รอบ call-arg:** เดิม **argument ของการเรียกฟังก์ชันไม่ถูกตรวจชนิด** (`f("x")` ที่ `f(i32)` ผ่านเงียบ ๆ →
> ส่ง pointer เป็น int) → เพิ่มตรวจชนิด argument + จำนวน argument ทั้ง **ฟังก์ชันตรง และ method** (`obj.m(...)`)
> ใน `typecheck` (เว้น extern/generic-template/namespaced)
>
> **งานจัดระเบียบ (2026-06-08):** จัดกลุ่ม `examples/` เป็นโฟลเดอร์ 01_language..08_stdlib (ทุกไฟล์มี build header) ·
> เพิ่ม `types.mvs` (ครบทุกชนิด), `pointers.mvs`, `recursion.mvs`, `io_demo.mvs`, `use_c.mvs`+`mathops.c` (MVS เรียกไฟล์ C) ·
> `mvs.exe` ตรวจ nasm/clang ก่อนใช้ (เตือนให้ติดตั้งถ้าไม่พบ) + แสดงเวอร์ชันที่ใช้ · GUIDE เพิ่มหัวข้อ `str` vs `String`
>
> **recheck รอบ 3-agent #3:** จับบั๊กจริง 5 (รวม memory-safety/crash) — แก้ครบ:
> 1. **int↔float ข้ามขอบเงียบ ๆ** (`let x: f64 = 5`, `takesF(5)`, `return 5` จากฟังก์ชัน f64 → ขยะ) → ใส่ implicit coerce ที่ var/assign/return/arg (`gen_coerce_num`)
> 2. **label ชน `mvs_g_*`** (global `count` ↔ func `g_count` / struct `g` method) → เปลี่ยน prefix global เป็น `mvs_gv_`
> 3. **`int as bool` ไม่ normalize** (`5 as bool`→5, `256 as bool`→false) → `cmp/setne` เป็น 0/1
> 4. **parser ไม่มี depth guard** → input ซ้อนลึก (`((((...`, `{{{...`, `!!!...`) ทำ **stack overflow** เงียบ → cap ที่ 300 + error
> 5. **struct บรรจุตัวเอง by-value** (ขนาดอนันต์) → layout/print loop ไม่รู้จบ **crash** → ตรวจ cycle ที่ `layout_structs` (error) + depth cap ใน `expand_struct`
> build สะอาด `-Wextra`, ไม่มี dead code, regression 23/23 + battery 28 + negatives 14
>
> **recheck รอบ 3-agent #2:** จับบั๊กจริง 9 — แก้ครบ (verify ด้วยการรันทุกตัว):
> 1. **int inference order-dependent** (`f(a+1)`→i32 แต่ `f(1+a)`→i64 → overload คนละตัว) → promote เป็นชนิด int ที่กว้างกว่า (`int_rank`)
> 2. **nested generic `f(g(x))`** (outer อนุมานก่อน inner instantiate → bogus `ident__T`) → scan_calls children-first
> 3. **deref non-pointer** `*x` (x: i32) คอมไพล์ผ่าน → **segfault** → typecheck เพิ่ม error
> 4. **identifier ยาวเกิน → ล้น label buffer** (`char[300]`) → cap ที่ lexer (100) + `LABEL_MAX` 720 + snprintf
> 5. **u64↔f64 cast ค่า ≥ 2^63 ผิด** (`cvtsi2sd` มองเป็น signed) → เส้นทาง unsigned conversion
> 6. **signature buffer overflow** (many params/ชื่อยาว → `[96]`/`code[64]`/`mangled[256]` ล้น) → `SIGCAP` 256 + bounded append + snprintf
> 7. **`~(u8)0` คืน 64-bit ขยะ** → mask ผลตามความกว้างชนิด
> 8. **import โมดูลเดียวกันด้วย alias ต่างกัน** (`import a` + `import b`) สัญลักษณ์ไม่ resolve → error ชัดเจน
> 9. **io.out struct ซ้อนลึกเกิน 256 ฟิลด์** format/arg ไม่ตรง → guard ใน expand_struct
> build สะอาด `-Wextra`, ไม่มี dead code, regression 23/23 + battery 22 + negatives 12
>
> **recheck รอบ 3-agent #1:** จับบั๊กจริง 6 + minor — แก้ครบ:
> 1. **type_of/infer ไม่ promote float** (`2 + 1.5` int อยู่ซ้าย → พิมพ์ bit-pattern ขยะ; mixed nested พัง) → promote เป็น float + pointer typing (ptr±int→ptr, ptr-ptr→isize)
> 2. **pointer overload false-duplicate** (`cat_code`/`width_code` ยุบ pointer เป็น "p" → `f(*i32)` ชน `f(*u8)`) → ใส่ depth+pointee (`pi32`/`pu8`)
> 3. **global ไม่อยู่ใน inference scope** (`f(GLOBAL)` อนุมานเป็น i64 → เลือก overload/instantiate ผิด) → `seed_globals` ในทุก pass
> 4. **global initializer ไม่ถูก monomorphize/typecheck** (`const X = max(1,2)` พัง) → สแกน global init ใน monomorphize+typecheck
> 5. **struct re-eval ใน io.out** (`io.out("{}", mk())` เรียก mk() ต่อฟิลด์ → side-effect ซ้ำ/ค่าผิด) → `ND_FRAMEREF` materialize ครั้งเดียว
> 6. **embedded-NUL string OOB** (`"a\0b"` strdup ตัด แต่ str_len เต็ม → memcpy เกิน) → คัดลอกตามความยาวจริง
> + bounds checks (nfuncs/nglobals/nloops), ลบ dead guard, scope ให้ ND_CASE
> build สะอาด `-Wextra`, ไม่มี dead code, ผ่าน regression 23/23 + battery + negatives

### สิ่งที่ทำได้แล้ว
- lexer เขียนมือครบ (keyword, ตัวเลข, สตริง+escape, char, operator 1–2 ตัวอักษร, คอมเมนต์ `//` และ block)
- parser recursive-descent: ลำดับความสำคัญของตัวดำเนินการครบ
- ตัวแปร `let`/`const` (local + global), เลขคณิต `+ - * / % ^`, เปรียบเทียบ, ตรรกะ `&& || !`
- `if/elseif/else`, `while`, `for`, `break`, `continue`
- ฟังก์ชัน + พารามิเตอร์ (≤4) + return + recursion
- **ระบบโมดูล (import):** นำเข้าจากไฟล์ (`"./x.mvs"`) และจาก package (`"std"` เป็น namespace)
- **foreign function** `extern func` เรียก C runtime / Winsock ได้
- **standard library เขียนด้วย MVS** — `io` (out/print/in), `fs` (write/read), `net` (TCP, ทดลอง)
- **struct แบบ Rust** — `impl` method, method call (`obj.m()`), **chaining**, คืน struct (sret)
- **C interop** — `extern` (MVS→C), `export` (C→MVS), `-c`/`--emit-obj` ผลิต `.obj`
- **`--nostd` freestanding** — ไม่พึ่ง std/CRT/OS, ผลิต `.obj` (พิสูจน์แล้ว: poke memory, struct/method ทำงาน, ไม่มี CRT dep)
- pointer, ความกว้าง int จริง, f32/f64, switch/do-while, io.out แบบ Rust, args>4, tree-shaking
- stdlib `net` แบบ Rust (TcpServer/TcpClient + method) — **ทดสอบจริงแล้ว** (curl ต่อ server ได้)
- codegen x86-64 win64 (NASM Intel) ครบ: prologue/epilogue, จัดเรียง stack, sret, stack args, method self injection

---

## งานค้าง / Roadmap

### ✅ ลำดับ 1, 2, 3 — เสร็จแล้วทั้งหมด
- [x] **struct** — decl, literal, member access (อ่าน/เขียน), nested, คืน struct (sret) — `StructInfo`/`register_struct`/`gen_store_struct`
- [x] **pointer** — `*T`, `&x`, `*p` — `gen_addr`/`type_of` (มี ptr depth)
- [x] **ความกว้างชนิด int จริง** — `type_size`, load `movsx/movzx`, store ตัดขนาด (มี wrap-around)
- [x] **switch/case/default** (fallthrough แบบ C) + **do-while** — เก็บค่า switch ใน frame slot
- [x] **f32/f64** — bit-pattern ใน rax + xmm ตอนคำนวณ; แปลง int↔float; `%f` ใน io.out
- [x] **io.out แบบ Rust** — `{}`, `{:x}`, หลายค่า, เลือกชนิดอัตโนมัติ (`build_c_format`, intrinsic)
- [x] **import** (ไฟล์ + package std), **extern**, stdlib `io`/`fs`/`net`, **io.in**
- [x] **arg > 4 ตัว** (stack args), **tree-shaking** (`reach_func`)

### ✅ เพิ่มเติม — เสร็จแล้ว
- [x] **struct method (impl) + chaining** แบบ Rust — `obj.method()`, ฉีด self อัตโนมัติ
- [x] **C interop** — `extern`/`export` + `-c` ผลิต object file (พิสูจน์: C เรียก MVS ได้)
- [x] **`--nostd` freestanding** — สำหรับเขียน OS / bare-metal (ดูปรัชญาใน Rules.md ข้อ 0)

### ลำดับ 4 — งานที่เหลือ / ปรับปรุง
- [x] ~~float ผ่าน xmm (เรียก C `sqrt`/`pow` f64 ได้), return ผ่าน xmm0~~ — ทำแล้ว
- [x] ~~f32 เก็บ 4 ไบต์จริง (cvtss2sd/cvtsd2ss)~~ — ทำแล้ว
- [x] ~~pointer arithmetic scale ตาม sizeof(pointee)~~ — ทำแล้ว
- [x] ~~struct ส่ง by-value เป็น parameter~~ — ทำแล้ว (caller ส่ง &arg, callee copy)
- [x] ~~unsigned เต็มรูปแบบ (div/mod/เปรียบเทียบ unsigned, %llu, literal u64 ผ่าน strtoull)~~ — ทำแล้ว
- [x] ~~**bitwise operators** `& | ^ ~ << >>` (`^`=XOR, ยกกำลังใช้ `**`)~~ — ทำแล้ว
- [x] ~~**generics** (monomorphization, `src/generic.c`) — `max<T>`, generic+pointer, generic เรียก generic~~ — ทำแล้ว
- [x] ~~**scope shadowing** จริง (visibility stack แยกจาก slot allocation)~~ — ทำแล้ว
- [x] ~~รองรับ C `float` จริง (single) สำหรับ `sqrtf`~~ — ทำแล้ว
- [x] ~~**overloading ตามชนิด** (resolve+mangle ใน `src/generic.c`)~~ — ทำแล้ว (เลือกตามความกว้าง int แบบเป๊ะ + fallback หมวด)
- [x] ~~**constraint แบบ duck-typed**: generic เรียก overload ได้ (ใช้กับชนิดที่มี overload รองรับ)~~ — ทำแล้ว
- [x] ~~io.out รองรับ argument ไม่จำกัด (เกิน 3) ผ่าน stack args + xmm สำหรับ float~~ — ทำแล้ว
- [x] ~~io.out พิมพ์ struct ได้แบบ `{:?}` ของ Rust (`Point { x: 3, y: 4 }`, รวม struct ซ้อน)~~ — ทำแล้ว (ขยายใน `build_c_format`)
- [x] ~~**ตรวจชนิดเวลาคอมไพล์ (type checking)**: จับ `50 + "50"`, `u8 = str`, bitwise บน float~~ — ทำแล้ว (`typecheck()` ใน `generic.c` เรียกหลัง monomorphize/overload)
- [x] ~~**`as` cast**: แปลงชนิดชัดเจน int↔float, narrowing, sign-reinterpret, pointer↔int~~ — ทำแล้ว (ND_CAST: lexer→parser→type_of/infer→codegen)
- [x] ~~**associated function** `Type::new(..)` (impl func ไม่มี self, เรียกผ่าน `::`)~~ — ทำแล้ว (TK_COLONCOLON; resolve ผ่าน ns เดิม)
- [x] ~~**trait + `impl Trait for Type` + `<T: Trait>` constraint**~~ — ทำแล้ว: static dispatch ฟรีผ่าน monomorphization; ตรวจ bound + impl ครบ + trait มีจริง (ND_TRAIT/ND_TRAIT_IMPL, `gen_bound[]`)
- [x] ~~**trait default method**~~ — ทำแล้ว (`apply_trait_defaults`: clone body + แทน Self→type ให้ type ที่ไม่ override)
- [x] ~~**overload แยกตามความกว้าง int** (i32 vs i64 เป็นคนละตัว)~~ — ทำแล้ว (exact width + category fallback)
- [x] ~~**`String` (heap) + `String::from`/`from_int`/`push_str`/`as_str`/`drop`**~~ — ทำแล้ว (`std/string.mvs`)
- [x] ~~เข้าถึง/ส่งผ่านผลลัพธ์ struct จากฟังก์ชันเป็น rvalue โดยตรง (`f(x).field`, `g(make())`)~~ — ทำแล้ว (`collect_struct_temps` จองช่อง temp + sret)
- [x] ~~**f32 เก็บ 4 ไบต์จริง** + **float xmm กับฟังก์ชัน C สองทาง** (รวม f32 single ↔ double)~~ — ทำแล้ว
- [x] ~~**trait `Display` + `fmt.println`/`fmt.print` เป็น library จริง** (std/fmt.mvs) — formatting ที่ผู้ใช้ขยายได้ผ่าน trait+generic (static dispatch)~~ — ทำแล้ว + รองรับ **namespaced generic call** (`fmt.println(x)` monomorphize ได้)
- [x] ~~**ออกแบบระบบ import ใหม่ (3 รูปแบบ + ตัวตรวจ)**~~ — ทำแล้ว: A) `import { io } from "std"` namespace · B) `import { String } from "std/string"` symbol · C) `import m from "./x.mvs"` alias namespace · + module check (symbol มีจริง), **duplicate detection** (struct/trait/func), namespace/alias ผูกซ้ำคนละโมดูล, **circular import** detection
- [x] ~~**function pointer** (ชนิด `func(...) -> T` เป็นค่า)~~ — ทำแล้ว: ตัวแปร/พารามิเตอร์/ฟิลด์ struct ที่เป็น func-ptr,
  ส่งเป็น argument, เก็บในฟิลด์, **เรียกแบบ indirect (`call rax`)**, คืน struct ผ่าน func-ptr (sret) ได้, ใช้กับ generic ได้
  (`f: func(T)->T` และ T ผูกกับค่า function) — ดูตัวอย่าง `examples/02_functions/funcptr.mvs`
  - **ค่า = ชื่อฟังก์ชันเปล่า ๆ** (เช่น `let f: func(i32)->i32 = add;` หรือ `app.get("/", home)`) → codegen `lea` ที่อยู่ label
  - แตะ: `ast.h` (`TYPE_FUNC` + ฟิลด์ `sig`), `parser.c` (`parse_type` ส่วน `func(...)->T`), `generic.c` (infer/typecheck/substitute พา `sig`),
    `arch/common.c` (`expr_func_sig`/`type_of`/tree-shaking/`add_local`/struct field), `arch/x86_64/win.c` (`gen_call` indirect + func-as-value)
  - ใช้สาธิตใน `vmass/` (เฟรมเวิร์กทดสอบ net) เป็น router `app.get(path, handler)` + dispatch table

### ข้อจำกัดเล็ก ๆ ที่ audit เจอ (ยังไม่แก้ — edge/ความเสี่ยงต่ำ)
- **1 โมดูล = 1 namespace**: ถ้า import ไฟล์เดียวกันด้วยชื่อ/รูปแบบต่างกัน (เช่น `{io} from "std"` แล้ว `{print} from "std/io"`) ครั้งหลัง dedup เงียบ — สัญลักษณ์อาจไม่ถูก expose ตามที่คาด (โมดูลถูก tag ns ครั้งแรกที่โหลด)
- **extern C ที่รับ f32 ตำแหน่งที่ ≥4 (บน stack)** ยังไม่ย่อ double→single (ในรีจิสเตอร์ 0–3 ทำแล้ว) — เคสหายาก
- compound assign (`op=`) ที่ฐาน lvalue มี call (เช่น `getptr().x += 1`) อาจเรียก call ซ้ำ — เคสหายาก
- `[`/`]` ถูก lex แต่ไม่มี array type · generic param > 4 ตัวถูก clamp · `-2 ** 2 == 4` (unary ผูกแน่นกว่า `**`)
- **function pointer (v1)** ข้อจำกัดที่ยอมรับได้:
  - ชนิด `func(...)->T` ยังไม่รับ vararg (`...`) ในลายเซ็น · ยัง overload ตามชนิด func-ptr ไม่ได้ (ทุก func-ptr mangle เป็น `func` เดียว)
  - typecheck การกำหนดค่าให้ตัวแปร func-ptr ยัง lenient (ไม่เทียบ signature เป๊ะ + ไม่จับ `let f: func()->i32 = 5;`)
  - ถ้าเอา func-ptr ไปชี้ **ฟังก์ชัน C (`extern`)** ที่รับ/คืน **f32** จะไม่ย่อ double↔single ให้ (ค่าผิดเงียบ ๆ) — func-ptr เจตนาใช้กับฟังก์ชัน MVS
- [ ] **io.out รูป `{}` (variadic) ย้ายไป io.mvs ให้เป็น library 100%** — *งานใหญ่ ต้องทำ prerequisite 3 อย่างก่อน:*

  > เป้าหมาย: `func out(fmt: str, args: ...dyn Display)` เขียนใน io.mvs แทน intrinsic
  > โดย out สแกน `{}` ตอน runtime แล้วเรียก `args[i].fmt()` ผ่าน vtable
  >
  > **ทำไมตอนนี้ทำไม่ได้:** รูป `io.out("{} {}", a, b)` รับ arg จำนวน+ชนิดไม่จำกัด ฟังก์ชันธรรมดารับไม่ได้
  > (param ตายตัว) และค่าใน MVS ไม่มี type tag ตอน runtime จึง dispatch ตามชนิดไม่ได้
  > → ต้องมีครบทั้ง 3 (ทำตามลำดับ):

  - [ ] **1. trait object / `dyn Trait` (fat pointer)** — ค่าชนิด `dyn Display` = `{ data_ptr, vtable_ptr }` 16 ไบต์
    - codegen: สร้าง **vtable** ต่อคู่ (type, trait) = ตาราง pointer ของ method ตามลำดับใน trait decl
    - แปลง `&x` (x: T ที่ impl Trait) → trait object: ใส่ `&x` + ที่อยู่ vtable ของ (T, Trait)
    - เรียก method ผ่าน trait object: โหลด fn ptr จาก `vtable[slot]` แล้ว `call rax` (dynamic dispatch)
    - แตะ: `ast.h` (ชนิด `dyn`), `parser.c` (parse `dyn Trait`), `arch/common.c` (สร้าง/หา vtable),
      `arch/x86_64/win.c` (gen vtable section, indirect call), `generic.c` (typecheck ว่า T impl Trait)
  - [ ] **2. variadic parameter (ฝั่งรับ)** — `func f(a: T, rest: ...U)` รับจำนวนไม่จำกัด
    - ABI: ผู้เรียกรวบ vararg เป็น **slice/array** (`{ptr, len}`) แล้วส่ง 1 อาร์กิวเมนต์ (เลี่ยง va_list ของ C)
    - ผู้เรียกต้องจองอาเรย์ชั่วคราวบน stack + เก็บค่าแต่ละตัวลงไป ก่อนเรียก
    - แตะ: `parser.c` (`...` ในพารามิเตอร์ + จุดเรียก), `arch/x86_64/win.c` (marshal เป็น slice), typecheck
  - [ ] **3. รวมร่าง** — `func out(fmt: str, args: ...dyn Display)` ใน io.mvs:
    - จุดเรียก `io.out("{}", x, y)` → คอมไพเลอร์ห่อ x, y เป็น `dyn Display` (ต้อง impl Display) → ใส่ใน slice
    - body ของ out (MVS ล้วน): loop ตัว fmt หา `{}` → `args[i].fmt()` (vtable) → ต่อด้วย String → printf
    - ต้องมี `impl Display for {i32,i64,f64,str,bool,char,...}` (primitive) ใน std ด้วย — ปัจจุบัน impl ได้เฉพาะ struct
    - [ ] sub: รองรับ **impl trait ให้ชนิด primitive** (ตอนนี้ `impl` ผูกกับ struct เท่านั้น)

  > **ทางเลือกที่มีแล้ว (ไม่ต้องรอ 3 ข้อข้างบน):** `fmt.println(x)` / `fmt.print(x)` ใน `std/fmt.mvs`
  > เป็น library จริง (static dispatch) ครอบคลุมการพิมพ์ค่าเดี่ยวที่ผู้ใช้ `impl Display` — io.out จึงคงเป็น
  > intrinsic ไปก่อน (เหมือน `println!` ของ Rust ที่เป็น macro โดยเจตนา)
- [ ] `where` clause หลายเงื่อนไข + trait associated const (`dyn Trait`/vtable = prerequisite ข้อ 1 ของ io.out ด้านบน)
- [ ] **i128/u128 คำนวณ 128-bit เต็ม** — *งานใหญ่:* ต้องทำ value-model สองรีจิสเตอร์ (rdx:rax)
      ทั้ง codegen + ตัวหาร 128-bit แบบ software (x86 ไม่มี 128/128 divide) — ปัจจุบันเก็บ 16 ไบต์ แต่คำนวณ 64-bit

### ลำดับ 5 — รูปแบบ object / สถาปัตยกรรม / ABI อื่น

> **บริบท OS dev:** โหมด `--nostd` ปัจจุบันผลิตโค้ด x86-64 ที่ self-contained 100%
> (ไม่มี undefined symbol, ไม่พึ่ง CRT/OS) รันบน bare-metal ได้ **แต่** object เป็น **COFF/PE**
> (`nasm -f win64`) + ใช้ **win64 calling convention** — เหมาะกับ toolchain ฝั่ง **LLVM/lld**
> ส่วน toolchain ฝั่ง **GNU ld + ELF** (เช่น GRUB multiboot) จะลิงก์ COFF ไม่ได้

- [ ] **ELF + SysV ABI backend** (สำคัญสำหรับ OS dev สาย ELF/GNU)
  - สร้าง `src/arch/x86_64/sysv.c` (`#include "../common.h"`) — เปลี่ยนเฉพาะ:
    - calling convention: args → `rdi, rsi, rdx, rcx, r8, r9` (ไม่มี shadow space 32 ไบต์)
    - `main.c` เรียก `nasm -f elf64` แทน `-f win64`
    - เพิ่ม `ARCH_X86_64_SYSV` ใน `TargetArch` + flag `--target elf64`
  - **`common.c` reuse ได้ทั้งหมด** (type/struct/symtab/tree-shaking ไม่เปลี่ยน)
- [ ] **ARM64** — เพิ่ม `src/arch/arm64/<os>.c` + `ARCH_ARM64` (register/instruction ชุดใหม่)
- [ ] เลือก object format / entry point ได้ (สำหรับ flat binary / multiboot kernel image)
- วิธีเพิ่ม: ดู `Rules.md` ข้อ 7 — front-end + `common.c` ใช้ซ้ำได้ แตะแค่ไฟล์ backend ใหม่ + `codegen.c`

### ✅ ทดสอบแล้ว (ไม่ใช่งานค้าง)
- [x] **net** — รัน `net_server.exe` แล้ว `curl http://127.0.0.1:8080/` ได้ผลจริง (echo HTTP)
- [x] **C interop** — C เรียก `mvs_square`/`mvs_sum_to` ผ่าน `.obj` ได้จริง
- [x] **freestanding** — `.obj` ไม่มี undefined symbol (self-contained, พิสูจน์ด้วย `llvm-nm`)

---

## จุดที่ต้องระวัง (gotchas ที่เจอมาแล้ว)

- **การจัดเรียง stack 16 ไบต์**: temp push ใช้ 16 ไบต์ ไม่ใช่ 8 (ดู `push_tmp`) — ถ้าแก้เป็น 8 จะ crash ตอนเรียก `printf` ในนิพจน์ที่มี call ซ้อน
- **`clang` กับ warning**: ต้องมี `-D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations` ไม่งั้นเตือนเพียบจาก MSVC headers (fopen/strdup/strcpy)
- **link flags**: ต้องมี `-llegacy_stdio_definitions` (ไม่งั้น `scanf` ลิงก์ไม่ผ่านเพราะ UCRT inline) และ `-lws2_32` (net) — อยู่ใน `src/main.c`
- **float ใน printf**: เป็น variadic ต้องใส่ค่าทั้ง GPR และ `xmm<n>` (ดู io.out) ไม่งั้นพิมพ์ผิด
- **struct return**: ฟังก์ชันคืน struct ใช้ hidden pointer (rcx) — เรียกแบบเก็บผลใส่ตัวแปรเท่านั้น (ไม่งั้น error)
- **method `ns` vs `mod`**: method ติด `ns` = ชื่อ struct (สำหรับ label) แต่ `mod` = โมดูล (สำหรับ resolve การเรียกภายใน)
  ถ้าใช้ ns เป็น cur_ns จะเรียกตัวเองวน (เช่น method `send` เรียก extern `send`) — ใช้ `fn->mod`
- **NASM keyword**: ชื่อ extern/export ที่ตรงคำสงวน nasm (`abs`,`rel`,`seg`,`wrt`) ทำให้ assemble ไม่ผ่าน — เลี่ยงชื่อนั้น
- **--nostd**: ห้าม import package, ไม่ลิงก์ CRT, ผลิต `.obj`; reachability roots = main + export
- **ห้าม `/*` ซ้อนในคอมเมนต์ไทย**: clang เตือน `-Wcomment`
- **scope shadowing**: รองรับเต็ม (codegen ใช้ visible stack, ทุก pass วิเคราะห์ชนิดเป็น scope-aware) — ตัวแปรชื่อซ้ำต่างชนิด/ต่าง scope ทำงานถูก
- **global init**: ตัวแปร global ถูกตั้งค่าเริ่มต้น **ตอนต้นฟังก์ชัน main** (ไม่ใช่ใน `.data`)
  ดู loop ใน `gen_func` ที่เช็ค `strcmp(fn->name,"main")` — ถ้าโปรแกรมไม่มี `main` global init จะไม่ทำงาน
- **module**: io ต้อง `import { io } from "std";` ก่อนใช้ ไม่งั้น error "undefined function 'io.out'"
- **std dir**: หาจาก env `MVS_STD` หรือ `<dir ของ mvs.exe>/std` — ถ้ารัน mvs.exe จากที่อื่นต้องตั้ง `MVS_STD`
- **printf variadic**: ส่ง argument เกินจำนวน param ที่ประกาศ `extern` ได้ (codegen ไม่เช็คจำนวน arg กับ signature)
- **callee-saved registers (win64)**: `rbx,rbp,rdi,rsi,rsp,r12-r15` ห้าม clobber โดยไม่คืนค่า
  → การ copy memory ใช้ `gen_memcpy()` (r10/r11 volatile) **ห้าม `rep movsb`** (มันใช้ rsi/rdi)
  พิสูจน์แล้ว: C ตั้ง rsi/rdi แล้วเรียก MVS (copy struct) — rsi/rdi ไม่เสีย
- **pointer arithmetic ต้อง scale ทุกที่**: `p+1` (binary), `p+=1` (compound), `p++`, และ `p - q` (หารด้วย sizeof)
  เหมือนกันหมด (เคยมีบั๊ก compound/`++`/ptr-ptr ไม่ scale — แก้แล้ว)
- **signedness ของ shift/div/mod อิงตัวซ้ายเท่านั้น** (ไม่ใช่ OR ของสองฝั่ง) — ไม่งั้น `signed >> n` กลายเป็น logical shift
  ส่วนการเปรียบเทียบใช้ "ถ้าฝั่งใด unsigned ถือเป็น unsigned" (กัน u64 ใหญ่ถูกมองเป็นลบ)
- **struct ประกาศก่อน-หลังได้อิสระ**: คำนวณ layout แบบ fixpoint (`layout_structs`) รองรับฟิลด์ที่เป็น struct ซึ่งประกาศทีหลัง
- **overload**: resolve นับ argument ที่เป็น call ซ้อนก่อน (recurse children first); ถ้า signature ชนหมวด (i32 vs i64) แจ้ง error ชัด
- **input ที่ผิดรูป (escape ท้ายไฟล์, `{` ไม่ปิด) ต้องไม่อ่านเกิน buffer** — มี guard แล้ว; ตารางภายในมี bounds check (MAX_*)
- **gcc/ld/flex/bison ไม่มีในเครื่อง** — อย่าเขียนสคริปต์/Makefile ที่เรียกมัน

---

## ไฟล์สำคัญและจุดที่ต้องไปแก้เมื่อเพิ่มฟีเจอร์

| จะเพิ่ม...            | แก้ไฟล์                                              |
|----------------------|------------------------------------------------------|
| token/keyword ใหม่   | `src/token.h`, `src/lexer.c` (ตาราง `KEYWORDS`)      |
| ไวยากรณ์ใหม่         | `src/parser.c` (+ `ND_*` ใหม่ใน `src/ast.h`)         |
| การ gen instruction  | `src/arch/x86_64/win.c` (`gen_expr` / `gen_stmt`)    |
| ตรรกะกลาง (type/struct/symtab/tree-shake) | `src/arch/common.c` (+ `common.h`) |
| พฤติกรรม import/โมดูล | `src/module.c` (`handle_import` / `load_module`)     |
| ฟังก์ชัน stdlib ใหม่ | `std/*.mvs` (เขียนด้วย MVS + `extern` เรียก libc)    |
| สถาปัตยกรรมใหม่      | สร้าง `src/arch/<arch>/<os>.c` (reuse `common.c`) + `TargetArch` + case ใน `codegen.c` |

## คำสั่งที่ใช้บ่อย
```powershell
make                              # build mvs.exe
.\mvs.exe examples\demo.mvs       # คอมไพล์เป็น .exe
.\mvs.exe examples\demo.mvs -S --keep   # ดูแอสเซมบลีที่ gen (debug codegen)
.\examples\demo.exe               # รันผลลัพธ์
```

---

## 📚 เอกสารทั้งหมด (เชื่อมโยงถึงกัน)

> 📍 คุณกำลังอ่าน: **Recap.md**

| ไฟล์ | เนื้อหา |
|------|---------|
| [README.md](README.md) | ภาพรวม · ติดตั้ง · วิธีใช้ · ความสามารถ |
| [GUIDE.md](GUIDE.md) | คู่มือภาษาเชิงลึก · ไวยากรณ์ · กลไกหน่วยความจำ · assembly จริง |
| [Rules.md](Rules.md) | กฎ/ปรัชญา freestanding · ABI · ข้อควรระวังสำหรับผู้พัฒนา |
| [Recap.md](Recap.md) | สถานะงาน · roadmap · gotchas |
| [CLAUDE.md](CLAUDE.md) | ไฟล์นำทางสำหรับ AI/ผู้พัฒนา · คำสั่ง · จุดแก้ไข |
| [examples/README.md](examples/README.md) | รายการโปรแกรมตัวอย่างทั้งหมด |
