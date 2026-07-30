# MVS example programs

Examples are grouped by topic, and **every file has a header with its build/run command.**
Start with [demo.mvs](demo.mvs) for the overview, then dig into whatever group interests you.

## Building and running

```powershell
mvs.exe examples/<group>/<file>.mvs     # builds <file>.exe next to the source (calls nasm + clang)
examples\<group>\<file>.exe             # run it
mvs.exe examples/<group>/<file>.mvs -S  # inspect the generated assembly (no nasm/clang)
```

You need **clang** and **nasm** on your PATH. mvs.exe warns if either is missing and prints the
versions it uses.

---

## demo.mvs

A grab-bag showcase: io.out, variables/const, functions, arithmetic, if/for/while, hex.

## 01_language: language core

| File | Contents |
|------|----------|
| `hello.mvs` | the first program |
| `types.mvs` | every data type (i8..i128, u8..u128, isize/usize, bool, char, str, f32, f64, pointer) |
| `operators.mvs` | arithmetic/comparison/logic, `**` power, break/continue, args > 4 |
| `bitwise.mvs` | `& \| ^ ~ << >>` + bitmask/flags |
| `casts.mvs` | `as` conversions + compile-time type checking |
| `control.mvs` | if/elseif/else, while, for, do-while, switch/case |
| `args.mvs` | command-line args via `main(argc, argv)` |
| `arrays.mvs` | `[T; N]` arrays: literals, indexing, `.len`, structs, decay to `*T` |
| `int128.mvs` | full 128-bit arithmetic: mul/div/mod past 64 bits, u128 max, shifts |
| `shadow.mvs` | scope shadowing: blocks, type-changing shadows, loop variables |
| `compile_attr.mvs` | conditional compilation: `@compile(target_os/target_arch)` on funcs and globals |

## 02_functions: functions / generics / overloading

| File | Contents |
|------|----------|
| `recursion.mvs` | self-recursion + mutual recursion |
| `generics.mvs` | generic functions (monomorphization) |
| `overload.mvs` | overloading by type + a generic calling an overload |
| `funcptr.mvs` | function pointers: `func(...) -> T` as a value, pass/store/call (indirect) |
| `defaults.mvs` | default parameter values (functions + methods, filled at compile time) |

## 03_structs: struct / method / pointer

| File | Contents |
|------|----------|
| `structs.mvs` | structs, members, struct return, io.out on a struct |
| `methods.mvs` | methods + associated `Type::new` + chaining |
| `pointers.mvs` | `&`/`*`/`**`, pointer arithmetic, arrays via malloc |
| `compound.mvs` | compound assignment evaluates its lvalue (incl. calls) exactly once |

## 04_traits: trait / Display

| File | Contents |
|------|----------|
| `traits.mvs` | trait + `impl Trait for` + `<T: Trait>` (static dispatch) |
| `display.mvs` | trait `Display` + `fmt.println` (library-style formatting) |
| `dynamic.mvs` | `dyn Trait` objects: vtable dispatch, dyn arrays/params, `where T: A + B` |

## 05_strings: strings

| File | Contents |
|------|----------|
| `strings.mvs` | `String` (heap, owned): `from`/`from_int`/`push_str`/`as_str`/`drop` |

## 06_modules: module system

| File | Contents |
|------|----------|
| `use_import.mvs` | the three import forms (namespace / symbol / alias) |
| `mathlib.mvs` | a user-written module (imported by the above) |

## 07_c_interop: working with C

| File | Contents |
|------|----------|
| `extern_c.mvs` | MVS calling C (strlen/atoi from the CRT) |
| `use_c.mvs` + `mathops.c` | MVS calling functions from our own C file (link .obj + .c) |
| `export_lib.mvs` + `caller.c` | C calling MVS (`export func` + a C-side prototype) |
| `freestanding.mvs` | `--nostd` mode (no std/CRT/OS) for OS / bare-metal |

## 08_stdlib: the standard library

| File | Contents |
|------|----------|
| `io_demo.mvs` | io.out in every form (`{}`, `{:x}`, struct, many args) + io.print |
| `floats.mvs` | floating point + C math (`sqrt`) |
| `files.mvs` | `fs.write`/`fs.read` + `io.in` |
| `net_client.mvs` / `net_server.mvs` | TCP client/server skeletons (`net.TcpClient`/`TcpServer`) |
| `net_loop.mvs` | TCP loopback round trip in one process; cross-platform sockets via `@compile` |
| `lib_out.mvs` | `fmt.outf`: io.out as a pure-MVS library (variadic `...dyn Display`, impl-on-primitive) |
| `lib_math.mvs` | `std/math`: libm wrappers + overloaded `abs`/`min`/`max`/`clamp`, `gcd`/`lcm`/`ipow` |
| `lib_mem.mvs` | `std/mem`: alloc/copy/set/eq/swap/grow over raw `*u8` buffers |

## 09_no_std: freestanding (OS / bare-metal, `--nostd`)

| File | Contents |
|------|----------|
| `kernel.mvs` | OS-style skeleton: VGA text writer + exported `kmain` entry (links with GNU ld) |
| `bump_alloc.mvs` | a freestanding bump allocator (no malloc, state in a caller-owned struct) |
