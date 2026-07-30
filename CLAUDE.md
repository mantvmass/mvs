# CLAUDE.md

Guidance for Claude Code (and any AI session) working in this project.

> **Read first:** [docs/rules.md](docs/rules.md), the rules you must not break.
> [docs/guide.md](docs/guide.md), the language reference, internals, project status, and roadmap.

## What this project is

A compiler for **MVS**, a low-level language (C-level, but easier to read). Written in **plain C**,
emitting NASM assembly directly for two targets: **x86-64 Windows (win64)** and **x86-64
Linux/ELF (SysV, `--target elf64`)**. **No LLVM, no flex/bison**: the lexer and parser are
hand-written.

## The most important constraints (full detail in docs/rules.md)

1. **No LLVM / flex / bison**: generate assembly yourself; hand-write the lexer/parser.
2. **Everything is English only**: source comments, program output, error messages, and docs.
3. Available toolchain: **clang + nasm** on Windows (gcc + nasm on Linux/CI); never flex/bison/LLVM.
4. The structure must support multiple architectures: the front end must not bind to x86.

## Main commands

```powershell
# build the compiler
make
#   equivalent to: clang -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc `
#                  src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/generic.c `
#                  src/diag.c src/codegen.c src/arch/common.c src/arch/x86_64/win.c `
#                  src/arch/x86_64/sysv.c src/arch/arm64/linux.c -o mvs.exe

# run the test suite (golden output + compile-only + compile-fail)
make test
# or without PowerShell (run-pass + compile-fail portable core, from the repo root):
.\mvs.exe test

# compile an MVS program to .exe, then run it
.\mvs.exe examples\demo.mvs
.\examples\demo.exe

# debug codegen: see the generated assembly (no nasm/clang)
.\mvs.exe examples\demo.mvs -S --keep
```

## Code architecture (pipeline)

```
.mvs → [lexer] → tokens → [parser] → AST → [codegen driver] → [arch backend] → .asm/.s
    win64 (default): nasm -f win64 → .obj → clang → .exe
    elf64:           nasm -f elf64 → .o   (link on Linux: gcc file.o -no-pie -lm)
    arm64:           aarch64-linux-gnu-gcc -c → .o (link with the cross gcc, run under qemu)
```

| File | Role |
|------|------|
| `src/token.h` | token kinds |
| `src/lexer.{h,c}` | tokenizer (hand-written) |
| `src/ast.{h,c}` | AST (one `Node` tagged by `kind`) + helpers |
| `src/parser.{h,c}` | recursive-descent parser |
| `src/module.{h,c}` | module system: resolve `import` across files + std package |
| `src/diag.{h,c}` | Rust-style diagnostics: source excerpts + carets + help notes + warnings |
| `src/codegen.{h,c}` | driver: picks the backend by `TargetArch` (not arch-bound) |
| `src/arch/common.{h,c}` | shared backend (arch-independent): types, structs, symtab, tree-shaking |
| `src/arch/x86_64/win.c` | x86-64 win64 backend (emits NASM, stack machine) |
| `src/arch/x86_64/sysv.c` | x86-64 Linux/ELF backend (SysV ABI, `--target elf64`) |
| `src/arch/arm64/linux.c` | AArch64 Linux backend (AAPCS64, `--target arm64`, GNU as syntax) |
| `src/main.c` | CLI driving the whole pipeline |
| `std/*.mvs` | standard library written in MVS (`io`/`string`/`fmt`/`fs`/`net`/`math`/`mem`) |

## Where to edit when adding a feature

- token/keyword → `token.h` + `lexer.c` (the `KEYWORDS` table)
- syntax → `parser.c` (+ `ND_*` in `ast.h`)
- type checking / trait bound / generic / overload → `generic.c` (passes after parse:
  monomorphize → resolve_overloads → typecheck)
- instruction emission → `arch/x86_64/win.c`; shared logic (type/struct/symtab/tree-shake/io.out format)
  → `arch/common.c`
- import behavior → `module.c`; stdlib functions → `std/*.mvs`
- a new arch → create `arch/<arch>/<os>.c` (reuse `common.c`) + `TargetArch` (`codegen.h`) + a case in `codegen.c`

## Before calling it done

1. `make` passes with no warnings.
2. `make test` passes: the golden output + compile-only + compile-fail suite in `scripts/test.ps1`.
3. New feature → add an example under `examples/`, register it in `scripts/test.ps1` + the Makefile
   `EXAMPLES` list, and regenerate goldens with `scripts/test.ps1 -Update`. New error → add a
   `tests/compile_fail/*.mvs` with a `//~ ERROR:` header.

## Feature status (details in docs/guide.md)

**Done:** struct + method/impl + chaining · pointer · real int width · f32 (real 4 bytes)/f64 (SSE) ·
switch/do-while · Rust-style io.out (`{}`/`{:x}` + struct printing + unlimited args) · import + extern ·
stdlib `io`/`fs`/`net`/`string` + `io.in` · args > 4 · tree-shaking · sret · C interop (`export`/`-c`) ·
`--nostd` freestanding · generics + overloading (split by int width) · scope shadowing ·
struct by-value params + struct-returning call as an rvalue · type checking + `as` cast ·
trait + associated function (`Type::new`) + `<T: Trait>` + default method · `String` (heap) +
`String::from`/`from_int` · float xmm across the C boundary both ways (f32 single↔double) ·
function pointer (`func(...) -> T` as a value + indirect `call rax`) · const enforcement ·
default parameter values · single-eval compound assignment · math-style `**` precedence ·
golden test suite (`make test`) · Rust-style diagnostics (multi-error recovery, source excerpt +
caret + help, missing-return check, unused/unreachable warnings) · real `[T; N]` array type
(literals, indexing, `a.len`, const-index bounds check, arrays in structs, decay to `*T`) ·
full 128-bit `i128`/`u128` arithmetic (software divmod helpers + decimal io.out) ·
`dyn Trait` trait objects (fat pointer + vtable dispatch) · multi-bound generics
(`<T: A + B>`, `where` clauses) · ELF/SysV backend (`--target elf64`, run-tested in WSL;
freestanding ELF links with GNU ld for GRUB OS dev) · impl-on-primitive
(`impl Display for i64`) · variadic `...dyn Trait` parameters (packed dyn slices) ·
`fmt.outf` = io.out as a pure-MVS library · native Linux build + Linux CI job ·
ARM64 backend (`--target arm64`, AAPCS64, GNU as syntax, full suite run under
qemu-aarch64 in CI with the same goldens as x86) · conditional compilation
`@compile(target_os/target_arch)` (filtered in the module loader) · cross-platform
`std/net` (Winsock vs POSIX selected with `@compile`) · `std/math` + `std/mem` ·
namespace-aware overloading (`math.abs` i64 vs f64) + `ns.func` reaching a
module's externs (`math.fmod` -> libm) · namespace-aware name resolution in EVERY
front-end pass (typecheck/defaults/monomorphize match codegen: user functions may
shadow std names) · argument checking for `ns.func(...)` calls (count, types,
variadic trait bounds) · hex/binary literals (`0xFF`, `0b1010`) + hard error on
literals past 64 bits · struct-literal stores fixed for 16-byte fields (i128/dyn
widen/copy) and int-to-float field init on all three backends · UTF-8 BOM
tolerated by the lexer · `std/time`/`std/env`/`std/process` (per-OS via @compile)
+ `std/rand` (pure-MVS xorshift64, platform-identical sequences) · io.out
width/precision (`{:8.2}`, `{:08}`, `{:04x}`) · `mvs test` built-in runner ·
`mvs --version` · `-O` peephole (safe NASM patterns, x86 targets).

**Remaining:** see [ROADMAP.md](ROADMAP.md) (generic structs -> `Option`/`Result`,
the freestanding `core` library, macOS target).

## Project-specific cautions (full list in docs/rules.md)

- **Freestanding by default** (docs/rules.md §0): the language core must not depend on the OS/CRT. Everything
  touching the OS lives in `std/*.mvs` (opt-in).
- C interop: `export func` = raw symbol name + `global`; `-c` produces a `.obj`; `--nostd` = freestanding obj.
- method: `ns` = struct name (label), `mod` = module (resolves internal calls). Don't confuse them (docs/rules.md §5.6).
- linking needs `-llegacy_stdio_definitions -lws2_32`; extern/export names must not clash with NASM reserved
  words (`abs`, etc.).
- floats are stored as a bit-pattern in rax, entering xmm only for math; `io.out` is a compiler intrinsic.

## Documentation

| File | Contents |
|------|----------|
| [README.md](README.md) | overview · install · how to compile · usage |
| [docs/guide.md](docs/guide.md) | language reference · internals · real assembly · status · roadmap |
| [docs/rules.md](docs/rules.md) | rules · freestanding philosophy · ABI · developer cautions |
| [ROADMAP.md](ROADMAP.md) | planned platforms · conditional compilation sketch · core library plan |
| [CLAUDE.md](CLAUDE.md) | this file: navigation for AI/developers · commands · where to edit |
| [docs/examples.md](docs/examples.md) | the full list of example programs |
