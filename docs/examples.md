# MVS example programs

Examples are grouped by topic; every file has a header with its build/run
command. Start with [demo.mvs](../examples/demo.mvs), then dig into whatever
group interests you.

```powershell
mvs.exe examples/<group>/<file>.mvs     # builds <file>.exe next to the source
examples\<group>\<file>.exe             # run it
mvs.exe examples/<group>/<file>.mvs -S  # look at the generated assembly instead
```

You need `clang` and `nasm` on PATH; mvs.exe warns if either is missing.

## demo.mvs

A grab-bag showcase: io.out, variables, functions, arithmetic, control flow.

## 01_language: language core

| File | Contents |
|------|----------|
| `hello.mvs` | the first program |
| `types.mvs` | every data type |
| `operators.mvs` | arithmetic/comparison/logic, `**` power, args > 4 |
| `bitwise.mvs` | `& \| ^ ~ << >>`, bitmask/flags |
| `casts.mvs` | `as` conversions + compile-time type checking |
| `control.mvs` | if/elseif/else, while, for, do-while, switch |
| `args.mvs` | command-line args via `main(argc, argv)` |
| `arrays.mvs` | `[T; N]`: literals, indexing, `.len`, decay to `*T` |
| `int128.mvs` | 128-bit arithmetic past 64 bits |
| `shadow.mvs` | scope shadowing |
| `compile_attr.mvs` | `@compile(target_os/target_arch)` |
| `hexbin.mvs` | hex and binary literals, `{:x}` output |
| `enums.mvs` | enums + match: payloads, `_`, exhaustiveness |

## 02_functions: functions / generics / overloading

| File | Contents |
|------|----------|
| `recursion.mvs` | self-recursion + mutual recursion |
| `generics.mvs` | generic functions (monomorphization) |
| `overload.mvs` | overloading by type, a generic calling an overload |
| `funcptr.mvs` | function pointers as values, indirect calls |
| `defaults.mvs` | default parameter values |

## 03_structs: struct / method / pointer

| File | Contents |
|------|----------|
| `structs.mvs` | structs, members, struct return, io.out on a struct |
| `methods.mvs` | methods, `Type::new`, chaining |
| `pointers.mvs` | `&`/`*`/`**`, pointer arithmetic, malloc'd arrays |
| `compound.mvs` | compound assignment evaluates its lvalue exactly once |
| `blob_fields.mvs` | i128/f64/f32 fields initialized from plain literals |
| `generic_structs.mvs` | `Pair<T, U>` + `impl` methods + nesting |

## 04_traits

| File | Contents |
|------|----------|
| `traits.mvs` | trait, `impl Trait for`, `<T: Trait>` static dispatch |
| `display.mvs` | trait `Display` + `fmt.println` |
| `dynamic.mvs` | `dyn Trait`: vtable dispatch, dyn arrays/params, `where` |

## 05_strings

| File | Contents |
|------|----------|
| `strings.mvs` | `String` on the heap: `from`/`push_str`/`as_str`/`drop` |

## 06_modules

| File | Contents |
|------|----------|
| `use_import.mvs` | the three import forms |
| `mathlib.mvs` | a user-written module, imported by the above |
| `shadow_std.mvs` | user functions sharing names with std resolve per namespace |

## 07_c_interop

| File | Contents |
|------|----------|
| `extern_c.mvs` | MVS calling C (strlen/atoi from the CRT) |
| `use_c.mvs` + `mathops.c` | MVS calling our own C file |
| `export_lib.mvs` + `caller.c` | C calling MVS (`export func`) |
| `freestanding.mvs` | `--nostd`: no std, no CRT, no OS |

## 08_stdlib

| File | Contents |
|------|----------|
| `io_demo.mvs` | io.out in every form |
| `floats.mvs` | floating point + C math |
| `files.mvs` | `fs.write`/`fs.read` + `io.in` |
| `net_client.mvs` / `net_server.mvs` | TCP client/server skeletons |
| `net_loop.mvs` | TCP loopback round trip in one process |
| `lib_out.mvs` | `fmt.outf`: io.out as a pure-MVS library |
| `lib_math.mvs` | `std/math`: libm wrappers + overloaded helpers |
| `lib_mem.mvs` | `std/mem`: alloc/copy/set/eq/swap/grow |
| `lib_rand.mvs` | `std/rand`: xorshift64, platform-identical sequences |
| `lib_sys.mvs` | `std/time` + `std/env` + `std/process` |
| `out_width.mvs` | io.out width/precision: `{:8.2}` `{:08}` `{:04x}` |
| `option_result.mvs` | `Option` / `Result`: unwrap and fallbacks |
| `lib_vec.mvs` | `Vec<T>`: push/get/set/pop over several element types |
| `lib_map.mvs` | `HashMap<K, V>`: i64 and str keys, growth, overwrite |
| `threads.mvs` | OS threads + Mutex: 4 workers, join |

## 09_no_std: freestanding (`--nostd`)

| File | Contents |
|------|----------|
| `kernel.mvs` | OS-style skeleton: VGA writer + exported `kmain` |
| `bump_alloc.mvs` | a freestanding bump allocator |
| `use_core.mvs` | the `core` package with zero undefined symbols |
| `intrinsics.mvs` | `core/arch` + `asm()`: barriers, hardware counter |

## The big ones

Three real programs, each with its own README:

- [10_json](../examples/10_json/README.md): a JSON library and CLI, 1084 lines.
  Tokenizer, arena value tree, recursive descent parser with `Result` errors,
  serializers, command line.
- [11_vm](../examples/11_vm/README.md): a whole language pipeline for a toy
  language, 1393 lines. Lexer, parser, arena AST, bytecode compiler, stack
  machine, disassembler.
- [12_http](../examples/12_http/README.md): a web application on `std/http`,
  the axum-shaped framework in the standard library. Routes with path
  parameters, query strings, JSON, redirects, a custom 404.

All three run byte-identically on the three targets and are clean under
AddressSanitizer with leak detection on.
