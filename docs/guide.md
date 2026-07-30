# GUIDE: the MVS language, its internals, and project status

This is the deep reference for **MVS**: syntax, the memory model, the calling convention, and
the actual assembly the compiler emits. The back half covers project status, roadmap, and the
gotchas worth knowing before you change anything. For the rules you must not break, see
[rules.md](rules.md). To get started, see [../README.md](../README.md).

Contents:
1. What MVS is
2. Source to executable (pipeline)
3. Compiler layout
4. Language reference
5. Memory model: stack, heap, static
6. Internals, with real assembly
7. Recipes
8. Current limitations
9. Status and roadmap
10. Gotchas
11. Glossary

---

## 1. What MVS is

MVS is a low-level language, roughly C's level but with a friendlier (Rust-ish) syntax. The goals:

- **Manual memory management like C**: no garbage collector, no hidden runtime.
- **Freestanding by default**: the core depends on no OS/CRT, so you can write an OS or bare-metal
  code with it.
- **Compiles straight to x86-64 assembly** (no LLVM), assembled by `nasm`, linked by `clang`.

The guiding rule: anything that touches the OS (printing, files, networking, heap allocation) lives
in the standard library (`std/*.mvs`) and must be imported. None of it is baked into the compiler.

## 2. Source to executable

```
file.mvs
   │  lexer   (src/lexer.c)      characters  -> tokens
   ▼
 tokens
   │  parser  (src/parser.c)     tokens      -> AST
   ▼
  AST
   │  module  (src/module.c)     merge imported files
   ▼
 merged AST
   │  codegen (src/arch/...)     walk AST    -> NASM assembly (.asm)
   ▼
 .asm
   │  nasm -f win64              assemble    -> object file (.obj)
   ▼
 .obj
   │  clang (linker)             link + C runtime -> executable
   ▼
 .exe
```

Build modes:

| Command | Output | When |
|---------|--------|------|
| `mvs file.mvs` | `.exe` | normal program |
| `mvs file.mvs -S` | `.asm` | inspect the generated assembly |
| `mvs file.mvs -c` | `.obj` | link against a C program |
| `mvs file.mvs --nostd` | `.obj` (freestanding) | OS / bare-metal |
| `mvs file.mvs --target elf64` | `.o` (ELF64, SysV ABI) | link and run on Linux (`gcc file.o`) |
| `mvs file.mvs --target arm64` | `.o` (AArch64) | link with the cross gcc, run under qemu |
| `mvs file.mvs --nostd --target elf64` | `.o` (freestanding ELF) | GNU ld / GRUB multiboot OS dev |
| `mvs file.mvs -O` | same, fewer instructions | peephole cleanup of the assembly (x86 targets) |
| `mvs test` | test report | run the golden suite from the repo root (no PowerShell needed) |
| `mvs --version` | version string | |

## 3. Compiler layout

```
src/
  token.h            all token kinds
  lexer.{h,c}        tokenizer (hand-written, no flex)
  ast.{h,c}          AST node (one Node tagged by kind)
  parser.{h,c}       recursive-descent parser (no bison)
  module.{h,c}       module system: resolve imports across files + std package
  diag.{h,c}         Rust-style diagnostics: source excerpts, carets, help notes
  codegen.{h,c}      driver: picks the backend by TargetArch
  arch/
    common.{h,c}     arch-independent: type system, struct layout, symbol table,
                     variable allocation, reachability, format strings
    x86_64/win.c     x86-64 Windows (win64 ABI): real instructions + calling convention
    x86_64/sysv.c    x86-64 Linux/ELF (SysV ABI): 6 GPR args + separate xmm class, no shadow space
  main.c             CLI driving the pipeline
std/                 standard library written in MVS (io, fs, net, string, fmt)
```

The whole design hinges on keeping "logic" (`common.c`) separate from "instruction emission" (`win.c`).
A new backend (say `arch/x86_64/sysv.c` for ELF/Linux) reuses `common.c` and only rewrites emission.

## 4. Language reference

### 4.1 Comments

```txt
// single line
/* multi
   line */
```

### 4.2 Data types

| Type | Meaning | Real size (bytes) | Signed? |
|------|---------|-------------------|---------|
| `i8` `i16` `i32` `i64` | signed integer (idiv, setl/setg) | 1, 2, 4, 8 | yes |
| `u8` `u16` `u32` `u64` | unsigned integer (div, setb/seta, real wrap) | 1, 2, 4, 8 | no |
| `i128` `u128` | full 128-bit integer (real 128-bit + - * / % shifts and compares) | 16 | i/u |
| `isize` `usize` | pointer-sized | 8 | i/u |
| `bool` | `true` / `false` | 1 | no |
| `char` | a character, `'A'` (single quotes only) | 1 | no |
| `f32` | single precision (stored in a real 4 bytes, computed as double) | 4 | - |
| `f64` | double precision | 8 | - |
| `str` | string literal `"..."` (pointer to bytes + 0) | 8 | - |
| `void` | no value (a function return type) | 0 | - |
| `*T` | pointer to T | 8 | - |
| `[T; N]` | fixed-size array of N elements | N * sizeof(T) | - |
| `struct` | a struct | sum of fields | - |

Values are always computed in 64-bit registers, but the real width is respected when **storing**
(truncate to N bytes) and **loading** (sign/zero-extend back). So `u8` really wraps: `200 + 100 = 44`
when stored (mid-computation it's still 64-bit).

#### `str` vs `String`

| | `str` (built-in) | `String` (from `std/string`) |
|--|------------------|------------------------------|
| What | a **pointer** to fixed text (null-terminated) | a **heap buffer** that owns its data |
| Mutable? | no, read-only | yes (`push_str`, `from_int`) |
| Memory | in `.data`, nothing to manage | malloc'd; you must `drop()` it (no GC) |
| Create | `let s: str = "hi";` | `let s: String = String::from("hi");` |
| With io.out | `io.out("{}", s)` directly | `s.as_str()` → returns `str` |

```txt
import { io } from "std";
import { String } from "std/string";
let lit: str = "hello";                 // fixed, read-only
let owned: String = String::from("hi"); // on the heap
owned.push_str(", world");              // can append
io.out("{} / {}", lit, owned.as_str()); // hello / hi, world
owned.drop();                           // free it yourself
```

Rule of thumb: `str` for fixed text and read-only parameters; `String` when you build or append at runtime.

#### Arrays `[T; N]`

```txt
let a: [i32; 5] = [10, 20, 30, 40, 50];  // literal must have exactly N elements
a[0] = 99;  a[1] += 5;                   // element read/write (any expression as index)
io.out("{}", a);                         // [99, 25, 30, 40, 50]  (expands like Rust's {:?})
io.out("{}", a.len);                     // 5 (compile-time constant)

struct Grid { cells: [i32; 4]; }         // arrays work as struct fields (and print inline)
let pts: [Point; 2] = [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }];  // arrays of structs

func sum(p: *i32, n: i32) -> i32 { ... }
sum(a, 5);                               // an array decays to a pointer at a call site
```

Rules: a `[T; N]` lives on the stack (globals in `.bss`); a constant index out of range is a
compile error; the literal length must match N exactly; parameters cannot be arrays (pass `*T`,
the array decays); whole-array assignment (`a = b`) is rejected, copy element by element.
For dynamically-sized buffers use `malloc` + pointer arithmetic as before.

Integer literals may be written in decimal, hex, or binary; all are plain `TK_INT`
tokens with the same 64-bit value:

```txt
let color: u32 = 0xFF8800;    // hex
let mask: u8 = 0b10100101;    // binary
io.out("{:x}", color);        // prints back as hex: ff8800
```

A literal that does not fit in 64 bits is a compile error (128-bit values are built
with arithmetic, e.g. `(1 as i128) << 100`).

### 4.3 Variables

```txt
let name: type = value;     // mutable
const NAME: type = value;   // constant: initializer required, any later write is a compile error
let x: i32;                 // declared, uninitialized (stack value not guaranteed)
```

Declarable at **global** scope (outside functions) and **local** scope (inside functions).

### 4.4 Operators

| Group | Operators | Notes |
|-------|-----------|-------|
| arithmetic | `+ - * / % **` | `**` = power (right-assoc), `%` = modulo |
| comparison | `== != < > <= >=` | result is bool (1/0); unsigned uses setb/seta |
| logical | `&& \|\| !` | short-circuit |
| bitwise | `& \| ^ ~ << >>` | AND / OR / XOR / NOT / shifts |
| assignment | `= += -= *= /=` | |
| inc/dec | `++ --` | postfix |
| pointer | `&x` `*p` `**p` | address-of / dereference (pointer math scales by sizeof) |

Precedence (high → low): `unary` → `as` → `**` → `* / %` → `+ -` → `<< >>` → comparison →
`== !=` → `&` → `^` → `|` → `&&` → `||` → `=`. Use `()` to override.

`&` is both address-of (prefix `&x`) and bitwise AND (infix `a & b`), told apart by position; `**`
is both power (`a ** b`) and double-deref (`**ptr`), likewise by position.

`**` binds tighter than prefix `-` and `~` (math convention): `-2 ** 2` is `-(2 ** 2)` = -4;
write `(-2) ** 2` to square the negative value.

#### Compile-time type checking

The language won't let types slide: it catches mismatches at compile time, which matters for
low-level/OS/embedded work where a type bug can fail silently at runtime. Immediate errors:

```txt
let x: i32 = 50 + "50";   // error: cannot apply '+'/'-' to 'i64' and 'str'
let y: u8  = "hello";     // error: cannot initialize variable: ... 'u8' and 'str'
let z: i32 = pt * 2;      // error (pt is a struct): cannot apply arithmetic to 'Point' and 'i64'
let w: i32 = f & 3;       // error (f is float): bitwise/shift requires integer operands
f("hello");               // error: argument 1 to 'f': cannot pass 'str' where 'i32' is expected
f(1, 2, 3);               // error: function 'f' expects N argument(s) but got 3
```

In short: arithmetic/power needs **numbers**, bitwise/shift needs **integers**, comparison/logic can't
take **structs**, assignment/return/field must be **compatible**, and call arguments (functions and
methods) must match in type and count. It stays lenient about correct low-level work, though:
`ptr +/- int`, comparing a pointer to `0` for null, mixing int widths, passing `str`↔`*u8`.

#### Explicit cast with `as`

To convert for real, use `as` (binds tighter than arithmetic):

```txt
let a: i32 = 7;
a as f64 / 2.0     // 3.5   (int -> float; without the cast: 7/2 = 3)
3.9 as i32         // 3     (float -> int: truncates, no rounding)
300 as u8          // 44    (narrow: 300 mod 256)
(255 as u8) as i8  // -1    (reinterpret bits as signed)
65 as char         // 'A'   (int -> char)
ptr as usize       // address as integer (pointer <-> integer)
```

### 4.5 Control flow

```txt
if (cond) { ... } elseif (cond) { ... } else { ... }
while (cond) { ... }
for (let i: i32 = 0; i < 10; i++) { ... }
do { ... } while (cond);            // runs at least once

switch (x) {                        // C-style fallthrough (break it yourself)
    case 1: ...; break;
    case 2:
    case 3: ...; break;             // 2 and 3 share code
    default: ...;
}

break;     // leave loop/switch
continue;  // next iteration (inside switch, jumps to the enclosing loop)
```

### 4.6 Functions

```txt
func add(a: i32, b: i32) -> i32 { return a + b; }
func greet() -> void { ... }        // no return value
```

- Recursion is supported.
- More than 4 parameters supported (arg 5+ passed on the stack).
- `func main() -> i8` is the entry point; its return is the exit code.

#### Default parameter values

```txt
func area(w: i32, h: i32 = 10, scale: i32 = 1) -> i32 { return w * h * scale; }
area(3);        // 30 (h=10, scale=1 filled in at compile time)
area(3, 4, 2);  // 24
```

Defaults must be trailing (a plain parameter cannot follow a defaulted one). They work for
methods too (`p.shifted()`), but not for overloaded names, generic templates, or extern C.

#### Generic functions (monomorphization)

```txt
func max<T>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}
max(3, 7);       // compiler emits max__i64
max(2.5, 9.1);   // and max__f64 (type inferred from the argument)
```

The compiler infers the real type from arguments and emits a per-type copy (like Rust/C++); see
`src/generic.c`. Works with pointers (`*T`), generics calling generics, and reused instances.

#### Function overloading

```txt
func show(n: i32) -> void { io.out("int: {}", n); }
func show(s: str) -> void { io.out("str: {}", s); }
func show(p: Point) -> void { io.out("point: ({}, {})", p.x, p.y); }

show(42);       // picks show(i32)
show("hi");     // picks show(str)
```

The overload is chosen by the **type category** of the argument (int/float/str/char/bool/pointer/struct),
then renamed internally by signature (`show__i`, `show__s`); see `resolve_overloads`.

#### Generic + overload = a duck-typed constraint

```txt
func print_all<T>(a: T, b: T) -> void { show(a); show(b); }  // T must have a matching show
print_all(1, 2);      // uses show(i32)
print_all("a", "b");  // uses show(str)
```

A generic works with "any type that supports the operations it uses", an implicit constraint. For an
explicit, checked constraint, use `trait`.

#### Trait + associated function + generic bound

```txt
struct Point { x: i32; y: i32; }
struct Circle { r: i32; }

trait Area {
    func area(self: *Self) -> i32;       // signature only; Self = the implementing type
}

impl Point {                              // inherent impl
    func new(x: i32, y: i32) -> Point { return Point { x: x, y: y }; }  // associated function
}
impl Area for Point  { func area(self: *Point)  -> i32 { return self.x * self.y; } }
impl Area for Circle { func area(self: *Circle) -> i32 { return 3 * self.r * self.r; } }

func describe<T: Area>(s: T) -> i32 { return s.area(); }   // T must impl Area

let p: Point = Point::new(3, 4);    // call the associated function via ::
let c: Circle = Circle::new(5);
describe(p);                         // 12, Point::area chosen at compile time (static dispatch)
describe(c);                         // 75, Circle::area
```

- **Associated function** = a func in `impl` with no `self`, called as `Type::func(...)` (like a constructor).
- **trait** is a contract; **`impl Trait for Type`** binds a type to it.
- **`<T: Trait>`** requires the type to implement the trait, otherwise a compile error
  (`type 'X' does not implement trait 'Area'`).
- Dispatch is **static** (via monomorphization): no vtable, no runtime overhead. The compiler also checks
  that an `impl` provides every trait method and that the trait exists.
- Trait default methods are supported (the body is cloned and `Self` replaced for types that don't override).

#### Trait objects: `dyn Trait` (dynamic dispatch)

```txt
let s: dyn Shape = &rect;            // fat pointer {data, vtable}; &circle works too
io.out("{}", s.area());              // dispatches through the vtable at run time
s = &circle;                          // same variable, different concrete type
let all: [dyn Shape; 2] = [&rect, &circle];   // heterogeneous lists work

func describe(s: dyn Shape) -> i32 { return s.area(); }   // dyn parameters and returns
```

A `dyn Trait` is 16 bytes: the data pointer plus a pointer to a per-(Type, Trait) vtable
(`mvs_vt_<Trait>_<Type>` in `.data`, one slot per trait method in declaration order). Storing
`&value` of a non-implementing struct is a compile error. Trait objects cannot be compared,
used as conditions, or passed to extern C.

#### Multiple bounds: `<T: A + B>` and `where`

```txt
func both<T: Shape + Named>(v: T) -> i32 { ... }          // inline form
func both<T>(v: T) -> i32 where T: Shape + Named { ... }  // where clause (same meaning)
```

Every listed trait is checked at instantiation; the error names the first missing one.

### 4.7 struct and methods (Rust-style)

```txt
struct Rect { w: i32; h: i32; }    // fields separated by ; or ,

impl Rect {
    func area(self: *Rect) -> i32 { return self.w * self.h; }
    func scale(self: *Rect, k: i32) -> *Rect {   // return *self to chain
        self.w *= k; self.h *= k; return self;
    }
}

let r: Rect = Rect { w: 3, h: 4 };  // struct literal
let a: i32 = r.area();              // method call (&r injected as self)
let b: i32 = r.scale(2).scale(2).area();  // chaining
r.w = 10;                          // member access
```

Functions can return structs, and you can reach nested structs (`a.b.c`) and go through a pointer
(`p.field` when p is `*Struct`).

### 4.8 Pointers

```txt
let y: i32 = 42;
let p: *i32 = &y;     // p points at y
let v: i32 = *p;      // read what p points to (= 42)
*p = 99;              // write through p (y becomes 99)
```

#### Function pointers

The type `func(P1, P2, ...) -> R` is a value: it points at a function you call through a variable or field.

```txt
func add(a: i32, b: i32) -> i32 { return a + b; }
func mul(a: i32, b: i32) -> i32 { return a * b; }

let f: func(i32, i32) -> i32 = add;   // value = the bare function name (no &)
io.out("{}", f(3, 4));        // 7
f = mul;
io.out("{}", f(3, 4));        // 12

func apply(op: func(i32, i32) -> i32, a: i32, b: i32) -> i32 { return op(a, b); }
io.out("{}", apply(add, 10, 20));   // 30   (higher-order function)

struct Op { name: str; fn: func(i32, i32) -> i32; }   // store as a field (dispatch table / callback)
let plus: Op = Op { name: "plus", fn: add };
io.out("{}", plus.fn(5, 6));        // 11
```

Returning a struct through a function pointer works (uses sret), and it works with generics. The emitted
code is `lea rax, [rel <label>]` for the value and `call rax` for the call (see 6.2).

### 4.9 Floats

```txt
let pi: f64 = 3.14159;
let area: f64 = pi * 2.0 * 2.0;
io.out("{}", 1.5 + 2);     // int -> float automatically -> 3.500000
```

### 4.10 Printing with io.out (Rust-style)

```txt
import { io } from "std";
io.out("hello");                  // hello
io.out("x = {}", 42);             // x = 42      ({} = a value, newline appended)
io.out("{} + {} = {}", a, b, a+b);// several values
io.out("hex = {:x}", 255);        // hex = ff
io.out("pct {{}}");               // pct {}      ({{ }} = literal braces)
io.out("{}", p);                  // p is a struct -> Point { x: 3, y: 4 }  (like Rust's {:?})

// width / precision (the "0W.P" part goes to printf verbatim, so C semantics)
io.out("[{:8.2}]", 3.14159);      // [    3.14]
io.out("[{:08}]", 42);            // [00000042]
io.out("[{:.3}]", 2.71828);       // [2.718]
io.out("[{:04x}]", 255);          // [00ff]
```

`io.out` is a **compiler intrinsic** (like Rust's `println!`): it parses `{}` at compile time and picks the
print format per argument. It needs `import { io }`, and it handles structs (expanded to
`Name { field: value, ... }`, including nested) and any number of arguments.

Why keep it an intrinsic at all? Printing an arbitrary STRUCT with `{}` uses compile-time
reflection (the compiler expands the fields), which a library cannot express. For everything
else the library route exists: `fmt.outf(f, args...)` is io.out written in pure MVS. It takes a
variadic `...dyn Display` slice, walks the `{}` placeholders at run time, and dispatches each
value through its `Display` impl (every primitive ships one; user structs join by writing
`impl Display for T`). See `examples/08_stdlib/lib_out.mvs`.

### 4.11 Module system: three forms (the path decides)

```txt
// A) submodule namespace: path is a bare package -> names in {} are submodules -> call as io.xxx
import { io, fs, net } from "std";        // io.out(...), net.TcpServer(...)

// B) symbol import: path is a specific module -> names in {} are symbols -> pulled in directly
import { String } from "std/string";      // String::from(...)
import { factorial } from "./mathlib.mvs"; // factorial(...)

// C) whole-module alias as a namespace: no {} -> use the alias prefix
import math from "./mathlib.mvs";          // math.factorial(...)
import str  from "std/string";             // str.<freefunc>(...)
```

| Path form | Meaning of `{ }` | Access |
|-----------|------------------|--------|
| `"std"` (bare package) | submodule → namespace | `io.out` |
| `"std/x"` or `"./f.mvs"` + `{...}` | symbol → pulled in directly | `String::from` |
| `"std/x"` or `"./f.mvs"` + alias | whole module → namespace alias | `math.factorial` |

Rule: referring to a whole package/module → use a namespace; naming a specific symbol → pull it in directly
(a relative file must end in `.mvs`).

Import checks (immediate errors):

- a symbol imported with form B must exist → otherwise `module 'x' has no exported symbol 'name'`.
- duplicate name (same ns+name+type for struct/trait/func) → `duplicate ...` (different-type overloads don't count).
- a namespace/alias bound to two different modules → `namespace 'x' is already bound to a different module`.
- one module imported under two different namespaces → error; mixing a namespace import with a
  symbol import of the same module → warning (free functions stay under the first form's namespace;
  structs/traits are global and work either way).
- a cycle (A→B→A) → `circular import detected`.

```txt
extern func printf(fmt: str) -> i32;          // call C (MVS -> C)
export func mvs_add(a: i32, b: i32) -> i32 {  // let C call MVS (raw symbol name)
    return a + b;
}
```

### 4.12 Standard library

| Module | Main functions |
|--------|----------------|
| `io` | `io.out(fmt, ...)`, `io.print(s)`, `io.in(prompt) -> str` |
| `fs` | `fs.write(path, content)`, `fs.read(path) -> str` |
| `net` | `net.TcpClient(ip, port)`, `net.TcpServer(ip, port)` + `accept`/`send`/`recv`/`close`; cross-platform (Winsock on Windows, POSIX sockets on Linux, selected with `@compile`) |
| `string` | `String` (heap string): `String::from(s)`, `from_int`/`from_uint`/`from_float`/`from_char`, `.push_str(s)` (chain), `.as_str() -> str`, `.len()`, `.drop()` |
| `fmt` | trait `Display { fmt(self) -> String }` (impl'd for every primitive) + `fmt.println(x)` / `fmt.print(x)` (static dispatch) + `fmt.outf(f, args...)`, a pure-MVS io.out with run-time `{}` handling |
| `math` | `sqrt`/`pow`/`floor`/`ceil`/`round`/`fmod` (libm), `pi()`/`e()`, and overloaded `abs`/`min`/`max`/`clamp` (i64 + f64) plus `sign`/`gcd`/`lcm`/`ipow` |
| `mem` | `alloc`/`alloc_zeroed`/`grow`/`dealloc`, `copy` (overlap-safe), `set`/`zero`, `eq`, `swap` |
| `time` | `now()` (epoch seconds), `millis()` (duration measurement), `sleep_ms(ms)`; per-OS via `@compile` |
| `env` | `get(name)` (missing reads as `""`), `set(name, value)`; per-OS via `@compile` |
| `process` | `run(cmd)` (shell command), `pid()`, `exit(code)` (the extern, reached through the namespace) |
| `rand` | xorshift64 in pure MVS: `seed`/`next`/`range(lo, hi)`/`unit()`; the same seed gives the SAME sequence on every platform |
| `test` | assertions for `*.test.mvs` files: `ok(cond)`, `eq(got, want)` (i64/f64/str overloads), `near(got, want, eps)`, `fail(msg)`; see section 4.14 |

Two resolution rules make modules pleasant to use:

- `ns.func(...)` also reaches a foreign function the module DECLARED: `math.fmod(x, y)`
  calls libm's `fmod` because `std/math.mvs` declares `extern func fmod(...)`. Externs keep
  their raw C symbol name; the namespace only scopes the lookup.
- Function overloading works through namespaces: `math.abs(-4)` picks `abs(i64)` while
  `math.abs(-2.5)` picks `abs(f64)`. Each module has its own overload sets, so a user
  function named `abs` never collides with `math.abs`.

```txt
import { io, string } from "std";
let s: String = String::from("hello");
s.push_str(", world");                       // append on the heap (reallocates)
io.out("{} (len {})", s.as_str(), s.len());  // hello, world (len 12)
let n: String = String::from_int(42);        // "42"
s.drop(); n.drop();                          // free it yourself (no GC)
```

### 4.13 Conditional compilation (@compile)

An `@compile(...)` attribute before any top-level item (function, extern, struct, global,
impl block, even an import) keeps that item only when the current target matches. This is
how one source file supports several platforms:

```txt
@compile(target_os = "windows")
extern func closesocket(s: usize) -> i32;
@compile(target_os = "linux")
extern func close(s: usize) -> i32;

@compile(target_os = "windows")
func close_fd(s: usize) -> i32 { return closesocket(s); }
@compile(target_os = "linux")
func close_fd(s: usize) -> i32 { return close(s); }

// both keys on one item = AND
@compile(target_os = "linux", target_arch = "aarch64")
func pause() -> void { /* ... */ }
```

- Keys: `target_os` (`"windows"`, `"linux"`) and `target_arch` (`"x86_64"`, `"aarch64"`).
  The values follow the `--target` flag: `win64` = windows/x86_64, `elf64` = linux/x86_64,
  `arm64` = linux/aarch64.
- Filtering happens in the module loader, BEFORE duplicate checks and type checking, so two
  gated definitions of the same function are fine as long as only one survives per target.
- An unknown key is a compile error; an unknown value only warns (it can never match).
- `std/net.mvs` is the reference user: Winsock vs POSIX sockets in one file.

### 4.14 Testing (`mvs test` + `*.test.mvs`)

Tests live in files named `somefile.test.mvs`. A test file defines test functions
(no `main` needed) and asserts through `std/test`. A test is either marked with
the `@test` decorator (any name) or simply named `test_*`:

```txt
// math.test.mvs
import { test, math } from "std";

@test
func gcd_of_common_factors() -> void {
    test.eq(math.gcd(48, 18), 6);
}

func test_sqrt() -> void {              // the naming convention works too
    test.near(math.sqrt(2.0), 1.4142135, 0.0001);
}
```

```
$ mvs test              # every *.test.mvs under the current directory (recursive)
$ mvs test src/         # ... under a specific directory
$ mvs test math.test.mvs
=== test files (*.test.mvs) ===
--- math.test.mvs
  ok    gcd_of_common_factors
  ok    test_sqrt
ALL PASS: 1 test file(s)
```

How it works: `mvs test` compiles each file with `--test-main`, which appends a
generated `main()` calling every test function in the ENTRY file in order and
printing `ok <name>` after each returns. A failed assertion prints
`FAIL <test name>: expected ... but got ...` and exits with 1, so the remaining
tests in that file are skipped and the file counts as failed (a per-test restart
needs unwinding, which waits for `Result`; see ROADMAP). A test file that defines
its own `main` is run as is.

Assertions: `test.ok(cond)` · `test.eq(got, want)` (overloaded for i64/f64/str;
str compares CONTENT via strcmp) · `test.near(got, want, eps)` for floats ·
`test.fail(msg)`. In the MVS repository itself, `mvs test` additionally runs the
golden example suite when it finds `tests/expected/`.

---

## 5. Memory model

This is the heart of a low-level language: MVS manages memory by hand, exactly like C. No garbage
collector, no reference counting, no destructors/RAII. Memory has three regions.

### 5.1 Stack: automatic (locals)

Every local lives on the function's stack frame, automatically:

- **On entry (prologue):** reserve the whole frame with `sub rsp, <frame size>`.
- **On exit (epilogue):** release it with `leave` (= `mov rsp, rbp; pop rbp`).

Frame size is computed ahead of time (a `collect_locals` pre-pass). Each variable gets at least 8 bytes
(rounded up to a multiple of 8) at `[rbp - offset]`.

Lifetime: a stack variable lives for the whole function call, then is released on return. So never return
a pointer to a local (`return &local;`): that memory is gone (dangling).

### 5.2 Heap: by hand (extern malloc/free)

For memory that must outlive a function or whose size is unknown, use the heap. MVS has no built-in
`new`/`malloc`; call the C runtime via `extern`:

```txt
extern func malloc(n: usize) -> *u8;
extern func free(p: *u8) -> void;

func main() -> i8 {
    let buf: *u8 = malloc(64);
    *buf = 65;
    let v: u8 = *buf;
    free(buf);                   // free it yourself, or you leak
    return v;                    // exit code 65
}
```

Heap rules (same as C): every `malloc` needs one matching `free` (else a leak); no use-after-free; no
double-free; no bounds checking (writing past your allocation is undefined behavior).

Some std functions (`io.in`, `fs.read`, `net.*recv`) malloc internally and don't free yet (leaking for
simplicity in this subset); real programs should wrap and free.

### 5.3 Heap under `--nostd`

In freestanding mode there is no `malloc` (no C runtime); write your own allocator, like in a real
kernel. The basic approach is a bump allocator over memory whose address you know:

```txt
func bump_alloc(heap_ptr: *usize, current: usize, size: usize) -> usize {
    return current + size;   // hand back the old address, advance current by size
}

export func poke(addr: *u8, value: u8) -> void {   // write to a raw address, e.g. VGA at 0xB8000
    *addr = value;
}
```

In a real OS you own physical/virtual memory and page allocation; nobody frees for you.

### 5.4 Static memory: globals and strings

- **Globals** live in `.bss` (`resb <size>`, zero-initialized). Their initial values are set at the start of
  `main` (not baked into the file).
- **String constants** live in `.data` (bytes + trailing 0); a `str` variable is a pointer to those bytes.

Fixed-size buffers use the `[T; N]` array type (stack/.bss); dynamically-sized buffers are made
with `malloc` + pointer + pointer arithmetic (`*(buf + i)` or `buf[i]`).

---

## 6. Internals, with real assembly

Every snippet below is **real compiler output**; reproduce it with `mvs file.mvs -S`.

### 6.1 Variables + arithmetic

```txt
func main() -> i8 {
    let a: i32 = 5;
    let b: i32 = a + 3;
    return b;
}
```

```asm
main:
    push rbp                  ; save old base pointer
    mov rbp, rsp              ; set up the new frame
    sub rsp, 16               ; reserve 16 bytes (a at [rbp-8], b at [rbp-16])
    ; --- let a: i32 = 5 ---
    mov rax, 5
    mov [rbp - 8], eax        ; store a (eax = 32-bit, i32 truncated to 4 bytes)
    ; --- let b: i32 = a + 3 ---
    lea rax, [rbp - 8]        ; address of a
    movsxd rax, dword [rax]   ; load a, sign-extend 32->64 (i32 is signed)
    sub rsp, 16               ; push a to the temp stack (16 bytes to keep alignment)
    mov [rsp], rax
    mov rax, 3
    mov rcx, rax              ; rhs (3) -> rcx
    mov rax, [rsp]            ; bring a back -> rax (lhs)
    add rsp, 16
    add rax, rcx              ; a + 3
    mov [rbp - 16], eax       ; store b (truncated to 32 bits)
    ; --- return b ---
    lea rax, [rbp - 16]
    movsxd rax, dword [rax]
    leave
    ret                       ; rax/al = exit code
```

Takeaways: every expression evaluates into `rax` (a simple stack machine); a binary op evaluates the left,
pushes it to a temp, evaluates the right, then combines; `i32` stores via `eax` and loads via `movsxd`
(that's "real type width"); temp pushes use 16 bytes to keep 16-byte alignment.

### 6.2 Calling a function (win64 ABI)

```txt
func add(x: i32, y: i32) -> i32 { return x + y; }
func main() -> i8 { return add(7, 8); }
```

Callee (`add`):

```asm
mvs_add:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    mov [rbp - 8], rcx        ; param 1 (x) from rcx
    mov [rbp - 16], rdx       ; param 2 (y) from rdx
    ... (x + y) ...
    leave
    ret                       ; result in rax
```

Caller (`main`) calling `add(7, 8)`:

```asm
    mov rax, 7
    sub rsp, 16               ; push args to the temp stack
    mov [rsp], rax
    mov rax, 8
    sub rsp, 16
    mov [rsp], rax
    sub rsp, 32               ; reserve 32 bytes of shadow space (win64 rule)
    mov rax, [rsp + 48]       ; arg0 (7) -> rcx
    mov rcx, rax
    mov rax, [rsp + 32]       ; arg1 (8) -> rdx
    mov rdx, rax
    call mvs_add              ; result comes back in rax
    add rsp, 32               ; release shadow space
    add rsp, 32               ; release the arg temp stack
```

win64 ABI as seen here: the first four integer args go in **rcx, rdx, r8, r9** (5+ on the stack); reserve
**32 bytes of shadow space** before every `call`; the return is in **rax**; `rsp` must be 16-byte aligned
at the `call`.

### 6.3 struct layout and member access

```txt
struct P { x: i32; y: i32; }
func main() -> i8 {
    let p: P = P { x: 10, y: 20 };
    return p.x;
}
```

Layout of `P` (packed, total rounded to a multiple of 8): `x` (i32) at offset 0, `y` (i32) at offset 4,
total 8.

```asm
    sub rsp, 16
    lea rax, [rbp - 8]        ; base address of p
    sub rsp, 16
    mov [rsp], rax            ; stash the base
    mov rax, 10               ; x
    mov rcx, rax
    mov rax, [rsp]
    mov [rax], ecx            ; store x at offset 0
    mov rax, 20               ; y
    mov rcx, rax
    mov rax, [rsp]
    add rax, 4                ; offset 4 (field y)
    mov [rax], ecx            ; store y
    ...
    lea rax, [rbp - 8]
    movsxd rax, dword [rax]   ; read p.x (offset 0)
```

Takeaways: member access = base address + field offset; a struct larger than 8 bytes uses several stack
slots; copying a struct (`a = b`) uses a byte loop; returning a struct uses *sret*: the caller reserves
space and passes a hidden pointer (rcx) the function writes through.

### 6.4 Pointers

- `&x` → `lea rax, [rbp - offset]`.
- `*p` (read) → load p (its address), then `mov rax, [rax]` at the pointee's size.
- `*p = v` (write) → evaluate v, take the address from p, then `mov [rax], <reg>`.

### 6.5 Floats

The double's bit-pattern lives in rax like an integer, moving into xmm only for math:

```asm
    movq xmm1, rax           ; rhs -> xmm1
    ...
    movq xmm0, rax           ; lhs -> xmm0
    addsd xmm0, xmm1         ; add as double
    movq rax, xmm0           ; result back to rax
```

If a side is an int it's converted first with `cvtsi2sd`.

### 6.6 io.out (compiler intrinsic)

Compiling `io.out("x = {}", n)`, the compiler: (1) scans the format string for `{}` and counts placeholders;
(2) picks a specifier per arg (`{}` + int → `%lld`, str → `%s`, char → `%c`, float → `%f`, `{:x}` → `%llx`);
(3) builds the C format `"x = %lld\n"` and calls `printf`.

### 6.7 Symbol naming

| Thing | Label | Why |
|-------|-------|-----|
| `main` | `main` | so the CRT calls it |
| ordinary function | `mvs_<name>` | avoid clashing with libc |
| function in a module | `mvs_<module>_<name>` | e.g. `mvs_io_print` |
| method | `mvs_<Struct>_<method>` | e.g. `mvs_Rect_area` |
| `extern` / `export` | `<name>` (raw) | matches the C symbol |
| global variable | `mvs_gv_<name>` | |

### 6.8 Tree-shaking

Starting from `main` (and `export`ed functions), the compiler follows calls (`reach_func`). Functions never
reached are never emitted, shrinking the output.

---

## 7. Recipes

Common tasks, copy-and-adjust.

### Allocate and free heap memory

```txt
extern func malloc(n: usize) -> *u8;
extern func free(p: *u8) -> void;
let buf: *u8 = malloc(256);
*buf = 65;
free(buf);                       // always free yourself
```

### Fixed-size arrays (stack) and dynamic buffers (malloc)

```txt
let a: [i32; 10] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];   // fixed size: a real array, no malloc
for (let i: i32 = 0; i < 10; i++) { a[i] = i * i; }
io.out("a[3] = {}", a[3]);           // 9

extern func malloc(n: usize) -> *u8;
let arr: *i32 = malloc(40);          // dynamic size: 10 i32 slots (10 * 4 bytes)
let i: i32 = 0;
while (i < 10) {
    arr[i] = i * i;                  // pointer indexing scales by sizeof(i32) automatically
    i++;
}
io.out("arr[3] = {}", arr[3]);       // 9
```

### Command-line arguments (argc/argv)

```txt
extern func atoi(s: str) -> i32;
func main(argc: i32, argv: **u8) -> i32 {   // declare main with parameters
    let prog: *u8 = *(argv + 0);            // argv[0] = program name; argv[i] = *(argv + i)
    if (argc >= 2) { let n: i32 = atoi(*(argv + 1)); }
    return 0;
}
```

The CRT already calls `main(argc, argv)` per the C ABI; `argv` is an array of `*u8` (C strings).

### Array of structs (malloc + pointer)

```txt
extern func malloc(n: usize) -> *u8;
struct Pt { x: i32; y: i32; }
let pts: *Pt = malloc(80);           // 10 slots (sizeof(Pt) = 8)
let i: i32 = 0;
while (i < 10) {
    (*(pts + i)).x = i;
    (*(pts + i)).y = i * 10;
    i++;
}
io.out("pts[3] = ({}, {})", (*(pts + 3)).x, (*(pts + 3)).y);   // 3 30
```

### Linked list / tree via a self-referential struct

```txt
struct Node { val: i32; next: *Node; }   // points to itself through a pointer
let n2: Node;  n2.val = 20;  n2.next = 0;     // 0 = NULL
let n1: Node;  n1.val = 10;  n1.next = &n2;
io.out("{} -> {}", n1.val, (*n1.next).val);   // 10 -> 20
```

Structs can reference each other freely (layout is computed by fixpoint); declaration order doesn't matter.

### Compare strings (not with ==)

```txt
extern func strcmp(a: str, b: str) -> i32;
// s == "x" compares addresses, not contents! use strcmp.
if (strcmp(name, "admin") == 0) { io.out("welcome admin"); }
```

### Call C math functions

```txt
extern func sqrt(x: f64) -> f64;
extern func pow(b: f64, e: f64) -> f64;
io.out("sqrt(2)={}", sqrt(2.0));
```

### Let C call MVS code (produce a .obj)

```txt
// lib.mvs (no main needed)
export func mvs_add(a: i32, b: i32) -> i32 { return a + b; }
```

```
mvs.exe lib.mvs -c          # produces lib.obj
clang main.c lib.obj -o app # link into a C program
```

### Freestanding code (no OS: kernel/bare-metal)

```txt
// no std import / no io.out in this mode
export func poke(addr: *u8, value: u8) -> void { *addr = value; }  // write VGA buffer, etc.
```

```
mvs.exe kernel.mvs --nostd  # kernel.obj with no CRT dependency (link/embed yourself)
```

### Files and input

```txt
import { io, fs } from "std";
fs.write("out.txt", "hello");
let text: str = fs.read("out.txt");
let name: str = io.in("your name: ");   // reads one word
```

### TCP echo server

```txt
import { io, net } from "std";
let server: TcpServer = net.TcpServer("0.0.0.0", 8080);
let conn: TcpSocket = server.accept();
let req: str = conn.recv();
conn.send("HTTP/1.0 200 OK\r\n\r\nhi");
conn.close(); server.close();
```

### Bitmask / flags (with XOR)

```txt
let READ: u32 = 1;  let WRITE: u32 = 2;
let perm: u32 = READ | WRITE;
if ((perm & WRITE) != 0) { io.out("writable"); }
perm = perm & ~WRITE;            // clear the WRITE bit

let a: u32 = 12;  let b: u32 = 10;
io.out("xor = {}", a ^ b);       // 6   (^ = XOR; power is **)
```

### Walk a buffer with a pointer (like C)

```txt
extern func malloc(n: usize) -> *u8;
let buf: *i32 = malloc(40);
let p: *i32 = buf;
let i: i32 = 0;
while (i < 10) { *p = i * i; p++; i++; }   // p++ advances by sizeof(i32) = 4
io.out("count = {}", p - buf);   // 10   (ptr - ptr divides by sizeof automatically)
```

### Return multiple values via out-pointers (no tuples)

```txt
func divmod(a: i32, b: i32, q: *i32, r: *i32) -> void {
    *q = a / b;
    *r = a % b;
}
let q: i32 = 0;  let r: i32 = 0;
divmod(17, 5, &q, &r);
io.out("17/5 = {} rem {}", q, r);   // 3 rem 2
```

### Inspect the generated assembly

```
mvs.exe prog.mvs -S --keep      # produces a readable prog.asm
```

---

## 8. Current limitations

Know the boundaries before relying on it (the roadmap for fixing these is in section 9).

### Language / types

- Full bitwise (`& | ^ ~ << >>`); power is `**` (e.g. `2 ** 8`, `2.0 ** 10`); a float base works (repeated
  `mulsd` by an integer exponent); a negative exponent isn't supported (returns 1.0).
- generics + overloading + traits are all present (monomorphization); overloads distinguish int **width**
  (i32 vs i64 are separate), with an integer literal matched by category if there's a single int overload;
  `trait`/`<T: A + B>`/`where`/default methods exist, and `dyn Trait` gives real vtable-based dynamic
  dispatch (trait objects cannot be compared, used as conditions, or passed to extern C).

### Numbers

- **Math always runs in 64-bit registers**: width is respected at load/store (correct truncate/extend, real
  unsigned div/setb, real wrap), but mid-expression overflow is 64-bit. (`let a: u8 = 200; io.out("{}", a + 100)`
  prints 300, but `let c: u8 = a + 100` stores 44; `~` masks to width, e.g. `~(u8)0 = 255`.) The **result type**
  of integer arithmetic is the wider operand (`i32 + i64` → i64).
- `i128`/`u128` compute with full 128-bit precision (`+ - * / % << >> & | ^ ~` and comparisons; division
  is a software shift-subtract routine). Known edges: no compound assignment or `++`/`--` (write
  `x = x + y`), no direct float casts (go through `i64`), not usable in `switch` or as a bare condition
  (compare `!= 0`), not supported across the extern C boundary, `{:x}` prints decimal like `{}`, and one
  `io.out` call can print at most 4 separate 128-bit values (conversion buffer ring).
- **No divide-by-zero check** (crashes at runtime).
- `u64`↔`f64` conversion of values ≥ 2^63 uses the unsigned path (correct); dereferencing a non-pointer is a
  compile error.
- int↔float converts implicitly at edges (assign/return/argument), e.g. `let x: f64 = 5` gives 5.0; `int as bool`
  gives 0/1.
- A struct containing itself by value (`struct P { n: P; }`) is a compile error (infinite size); use `*P`.
- A generic where `T` appears only in the return/body (not a parameter) instantiates as `i64` by default.
- For floats: `+ - * /`, `**` (float base, integer exponent), comparison, and unary `-` work; but `%`, `++`/`--`,
  and `switch` on a float are compile errors; a negative `**` exponent isn't supported (returns 1.0).

### struct / functions

- A struct literal with a nested struct field accepts only a literal/lvalue.
- A bare struct **literal** can't be passed as an argument (use a temp), but a struct **result from a function**
  works as an rvalue (`g(make())`, `make().field`, `make().method()`, materialized into a temp slot).
- **Generic methods** (`impl` with `<T>`) aren't supported; use a generic function.
- Max 64 struct fields; functions/symbols have limits (see `MAX_*` in `common.h`).

### Common gotchas (not bugs, but worth knowing)

- **`==` on strings compares addresses, not contents**; use `extern strcmp`.
- **`char + int` yields `char`** (inherits from the left operand); io.out prints it as a character; cast to i32
  for a number.
- io.out: `%` in text prints literally (no escaping); use `{{` `}}` for literal braces.

### Memory

- **No automatic memory management**: no GC, no RAII/destructor, you `free` yourself.
- **No bounds checking**: writing past an allocation is undefined behavior.
- Some std functions still malloc without freeing (deliberate leak in the examples).

### C interop / float

- Floats pass/return through **xmm** per the ABI; C math works for both `f64` (`sqrt`/`pow`) and `f32` (`sqrtf`),
  in every ABI position (registers and stack slots 5+). Export functions use the C single-precision convention
  for `f32` consistently, whether the caller is C or MVS code.
- **extern/export names must not collide with NASM reserved words** (`abs`, `rel`, `seg`, `wrt`), or they won't assemble.

### Target / output format

- Three targets: **win64** (default, COFF/PE via `nasm -f win64`, linked with clang),
  **elf64** (`--target elf64`, SysV ABI, `nasm -f elf64`), and **arm64**
  (`--target arm64`, AAPCS64, GNU as syntax, assembled with the AArch64 cross gcc/clang).
- For elf64/arm64 the compiler stops at the `.o`; linking happens on the Linux side
  (`gcc file.o -o file`, add `-lm` for C math, `-no-pie` for the absolute vtable relocs;
  arm64 links with `aarch64-linux-gnu-gcc` and runs under `qemu-aarch64`).
- On Windows: needs `nasm` + `clang` (linked via `-llegacy_stdio_definitions -lws2_32`).
- macOS (Mach-O) is not supported yet; see [../ROADMAP.md](../ROADMAP.md).

### Compiler

- Compilation stops between phases (all syntax errors report together, then all type errors), like Rust.
- Method reachability is over-approximate (may keep a few same-named methods beyond what's strictly used).

---

## 9. Status and roadmap

Current state: the language works end to end on three targets (x86-64 Windows, x86-64
Linux/ELF, AArch64 Linux), all CI-tested against the same golden outputs. The forward
plan lives in [../ROADMAP.md](../ROADMAP.md).

### Done

- Hand-written lexer (keywords, numbers, strings + escapes, chars, 1-2 char operators, `//` and block comments).
- Recursive-descent parser with full operator precedence.
- `let`/`const` (local + global), arithmetic `+ - * / % **`, comparison, logic `&& || !`, bitwise `& | ^ ~ << >>`.
- `if/elseif/else`, `while`, `for`, `do-while`, `switch/case/default`, `break`, `continue`.
- Functions + parameters (5+ via stack) + return + recursion.
- Real integer width (i8..i64, u8..u64, real unsigned div/mod/compare, real wrap), `i128`/`u128` stored as 16 bytes.
- `f32` (real 4 bytes) / `f64` via SSE; int↔float conversion; floats across the C boundary both ways.
- Pointers, pointer arithmetic scaled by sizeof, function pointers (value + indirect `call rax`).
- structs: literals, member access (read/write), nested, struct return (sret), by-value parameters,
  struct-result-as-rvalue; methods (`impl`) + associated functions (`Type::new`) + chaining.
- Generics (monomorphization) + overloading (by category, splitting int width) + traits (`impl Trait for`,
  `<T: Trait>`, default methods) with static dispatch and compile-time bound checking.
- `as` cast; compile-time type checking (incl. call argument type/count); scope shadowing (scope-aware in every
  analysis pass).
- Module system: three import forms + checks (symbol exists, duplicates, namespace binding, circular import).
- `io.out` Rust-style (`{}`, `{:x}`, structs, unlimited args); tree-shaking.
- stdlib in MVS: `io` (out/print/in), `fs` (write/read), `net` (TcpServer/TcpClient,
  cross-platform via `@compile`), `string` (`String` on the heap), `fmt` (`Display` +
  `println`/`print` + `outf`), `math` (libm + overloaded helpers), `mem` (alloc + mem ops).
- C interop (`extern`/`export` + `-c`); `--nostd` freestanding (proven self-contained via `llvm-nm`).
- `const` enforcement (initializer required, writes are compile errors); default parameter values
  (functions + methods, compile-time filled); compound assignment evaluates its lvalue exactly once;
  `**` binds tighter than unary minus; import-form mixing is reported instead of silently deduped.
- `f32` complete across the C boundary: narrowed/widened in every ABI position including stack
  slots 5+, for extern C calls, C callers of exports, and MVS callers of its own exports.
- Golden test suite (`make test`): run-pass output diffs, compile-only, and compile-fail
  (expected-error) tests in `tests/`, run by CI.
- ELF/SysV backend (`--target elf64`): `src/arch/x86_64/sysv.c` reusing `common.c`, with the
  System V calling convention (6 GPR args + a separate xmm class, no shadow space, AL for
  variadic calls), `nasm -f elf64` output, and a `.note.GNU-stack` section. The test suite
  links and RUNS the ELF binaries inside WSL, diffing against the same goldens as win64;
  `--nostd --target elf64` yields a freestanding ELF that GNU ld links directly (GRUB path).
- `dyn Trait` trait objects: 16-byte fat pointers {data, vtable}, per-(Type, Trait) vtables
  emitted in `.data`, run-time dispatch through the vtable, dyn variables/parameters/returns/
  array elements, plus multi-bound generics (`<T: A + B>` and `where` clauses).
- Full 128-bit `i128`/`u128` arithmetic: address-as-value convention (like structs), pair-wise
  qword add/sub/mul/bitwise/shift/compare, software shift-subtract division emitted as helper
  routines (`mvs_u128_divmod` and friends), decimal printing via `io.out`, and hidden-pointer
  parameter/return passing.
- Real `[T; N]` array type: stack/.bss storage, literals with exact-length checking, indexing
  (arrays and pointers, `p[i]` = `*(p + i)`), `a.len`, compile-time bounds check for constant
  indices, arrays in structs and structs in arrays, io.out expansion (`[1, 2, 3]`), and decay
  to `*T` at call sites.
- Rust-style diagnostics (`src/diag.c`): every error shows `file:line:col`, the offending
  source line with a caret, and a `help:` note; the parser recovers at `;`/`}` and reports
  many errors per run (capped at 20); non-void functions must return on every path
  (conservative control-flow analysis incl. `while (true)` without `break`); warnings for
  unused variables/parameters (prefix `_` to silence) and unreachable code, emitted for the
  entry file only so imported modules stay quiet.
- ARM64 backend (`--target arm64`): `src/arch/arm64/linux.c`, AAPCS64 (x0-x7 + d0-d7,
  sret in x8), GNU as syntax, assembled with the AArch64 cross gcc; the full example
  suite runs under qemu-aarch64 in CI against the same goldens as both x86 targets.
- Conditional compilation: `@compile(target_os = ...)` / `@compile(target_arch = ...)`
  on any top-level item, filtered in the module loader (section 4.13); `std/net` uses it
  to select Winsock vs POSIX sockets in one file.
- Hex/binary literals (`0xFF`, `0b1010`) with a hard error on literals past 64 bits.
- Namespace-consistent resolution everywhere: typecheck/defaults/monomorphize use the
  same module-first lookup as codegen, `ns.func(...)` reaches module externs, overload
  sets are per-namespace, and namespaced calls get full argument checking.

Verified by running, not just compiling: the net loopback example round-trips real TCP on
all three targets; C calls `mvs_square`/`mvs_sum_to` through a `.obj`; the freestanding
`.obj` has no undefined symbols.

### Remaining

See [../ROADMAP.md](../ROADMAP.md) for the full plan. Next up:

- **Generic structs** (`struct Vec<T>`): the prerequisite for `Option`/`Result`, real
  collections, and the freestanding `core` library.
- macOS target (Mach-O + its SysV quirks).

Note on io.out: the library path is COMPLETE. `fmt.outf(f, args...)` is a pure-MVS formatted
print built on impl-on-primitive Display impls, variadic `...dyn Display` slices, and run-time
dispatch. `io.out` itself remains an intrinsic because compile-time struct reflection (printing
any struct without an impl) cannot be expressed as a library.

### Known minor limits (low risk, not yet fixed)

- Generic params beyond 4 are clamped; arrays of arrays (`[[T; N]; M]`) are not supported yet.
- Function pointers (v1): the type doesn't take varargs in its signature; can't overload by function-pointer type
  (all mangle to `func`); assignment to a func-ptr variable is type-checked leniently; pointing a func-ptr at a C
  `extern` taking/returning `f32` won't narrow double↔single (func pointers are meant for MVS functions).

About `--nostd` for OS dev: it produces self-contained x86-64 with no undefined symbols and no CRT/OS
dependency (runnable on bare metal). Combine it with `--target elf64` to get a freestanding ELF object
that plain GNU ld links directly (verified: `ld file.o` produces a static ELF executable), which is the
GRUB multiboot path; without `--target` the object is COFF/PE for the LLVM/lld side.

### Where to edit when adding a feature

| To add... | Edit |
|-----------|------|
| a token/keyword | `src/token.h`, `src/lexer.c` (the `KEYWORDS` table) |
| new syntax | `src/parser.c` (+ new `ND_*` in `src/ast.h`) |
| instruction emission | `src/arch/x86_64/win.c` (`gen_expr` / `gen_stmt`) |
| shared logic (type/struct/symtab/tree-shake) | `src/arch/common.c` (+ `common.h`) |
| import behavior | `src/module.c` (`handle_import` / `load_module`) |
| a stdlib function | `std/*.mvs` (MVS + `extern` into libc) |
| a new architecture | `src/arch/<arch>/<os>.c` (reuse `common.c`) + `TargetArch` + a case in `codegen.c` |

---

## 10. Gotchas (learned the hard way)

- **16-byte stack alignment:** temp pushes use 16 bytes, not 8 (see `push_tmp`). Switching to 8 crashes `printf`
  in expressions with nested calls.
- **clang warnings:** you need `-D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations` or MSVC headers flood you
  with warnings (fopen/strdup/strcpy).
- **Link flags:** `-llegacy_stdio_definitions` (else `scanf` won't link, UCRT inlines it) and `-lws2_32` (net);
  set in `src/main.c`.
- **float in printf:** it's variadic, so a float must be placed in both the GPR and `xmm<n>` (see io.out), or it
  prints wrong.
- **struct return:** a struct-returning function uses a hidden pointer (rcx); only call it storing the result
  into a variable (otherwise error).
- **method `ns` vs `mod`:** a method's `ns` is the struct name (for the label); `mod` is the module (for resolving
  internal calls). Using ns as cur_ns makes a method call itself in a loop (e.g. `send` calling extern `send`);
  use `fn->mod`.
- **NASM keywords:** an extern/export name equal to a nasm reserved word (`abs`, `rel`, `seg`, `wrt`) won't
  assemble; avoid those names.
- **--nostd:** no package imports, no CRT linkage, emits a `.obj`; reachability roots = main + exports.
- **Don't nest `/*` inside a block comment:** clang warns `-Wcomment`.
- **scope shadowing:** fully supported (codegen uses the visible stack; every type-analysis pass is scope-aware);
  same-named variables of different types/scopes work correctly.
- **global init:** globals are initialized at the **start of main**, not in `.data`. See the loop in `gen_func`
  that checks `strcmp(fn->name, "main")`; with no `main`, global init doesn't run.
- **modules:** io needs `import { io } from "std";` first, else "undefined function 'io.out'".
- **std dir:** found via `MVS_STD` or `<dir of mvs.exe>/std`; set `MVS_STD` if you run mvs.exe from elsewhere.
- **printf variadic:** you can pass more arguments than the `extern` declares (codegen doesn't check arg count
  against the signature).
- **callee-saved registers (win64):** `rbx, rbp, rdi, rsi, rsp, r12-r15` must not be clobbered without restoring.
  Memory copy uses `gen_memcpy()` (r10/r11 volatile), never `rep movsb` (it uses rsi/rdi). Proven: C sets rsi/rdi,
  calls MVS (struct copy), and rsi/rdi survive.
- **pointer arithmetic must scale everywhere:** `p+1`, `p+=1`, `p++`, and `p - q` (divide by sizeof) all the same
  (there used to be a bug where compound/`++`/ptr-ptr didn't scale; fixed).
- **shift/div/mod signedness follows the left operand only** (not an OR of both sides); otherwise `signed >> n`
  becomes a logical shift. Comparison treats "either side unsigned = unsigned" (so a big u64 isn't seen as negative).
- **struct declaration order is free:** layout is computed by fixpoint (`layout_structs`), supporting fields that
  are structs declared later.
- **overload:** resolution counts nested-call arguments first (recurse children first); a same-category signature
  clash (i32 vs i64) reports a clear error.
- **malformed input** (trailing escape, unclosed `{`) must not read past the buffer: guards exist; internal tables
  have bounds checks (`MAX_*`).
- **No gcc/ld/flex/bison on the machine**: don't write scripts/Makefiles that call them.

---

## 11. Glossary

| Term | Meaning |
|------|---------|
| Token | the smallest meaningful unit (keyword, number, operator) |
| Lexer | turns characters into tokens |
| Parser | assembles tokens into a syntax tree |
| AST | Abstract Syntax Tree: the tree representing the program |
| Codegen | turns the AST into assembly |
| ABI | Application Binary Interface: the function-call contract (registers, stack) |
| Stack frame | one function call's stack region (where locals live) |
| Prologue/Epilogue | the function's entry/exit code that reserves/releases the frame |
| Shadow space | the 32 bytes the caller reserves for the callee (win64 rule) |
| sret | structure return: returning a struct through a hidden caller-provided pointer |
| rbp / rsp | base / stack pointer |
| RIP-relative | addressing relative to the instruction pointer (`default rel` in NASM) |
| Tree-shaking | dropping unreferenced functions from the output |
| Intrinsic | a feature the compiler handles itself (e.g. `io.out`), not an ordinary function |
| Freestanding | code with no OS/runtime dependency, for writing an OS / bare-metal |

If this doc and the code disagree, trust the code and update the doc.
