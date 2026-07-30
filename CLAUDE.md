# CLAUDE.md

Guidance for Claude Code (and any AI session) working in this project.

> **Read first:** [RULES.md](RULES.md), the rules you must not break.
> [GUIDE.md](GUIDE.md), the language reference, internals, project status, and roadmap.

## What this project is

A compiler for **MVS**, a low-level language (C-level, but easier to read). Written in **plain C**,
emitting **x86-64 Windows (NASM)** assembly directly. **No LLVM, no flex/bison**: the lexer and
parser are hand-written.

## The most important constraints (full detail in RULES.md)

1. **No LLVM / flex / bison**: generate assembly yourself; hand-write the lexer/parser.
2. **Everything is English only**: source comments, program output, error messages, and docs.
3. Available toolchain: **clang + nasm only** (no full GNU gcc/ld/flex/bison/make).
4. The structure must support multiple architectures: the front end must not bind to x86.

## Main commands

```powershell
# build the compiler
make
#   equivalent to: clang -Wall -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc `
#                  src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/codegen.c `
#                  src/arch/common.c src/arch/x86_64/win.c -o mvs.exe

# run the test suite (golden output + compile-only + compile-fail)
make test

# compile an MVS program to .exe, then run it
.\mvs.exe examples\demo.mvs
.\examples\demo.exe

# debug codegen: see the generated assembly (no nasm/clang)
.\mvs.exe examples\demo.mvs -S --keep
```

## Code architecture (pipeline)

```
.mvs → [lexer] → tokens → [parser] → AST → [codegen driver] → [arch backend] → .asm
                                                                          ↓
                                        nasm -f win64 → .obj → clang → .exe
```

| File | Role |
|------|------|
| `src/token.h` | token kinds |
| `src/lexer.{h,c}` | tokenizer (hand-written) |
| `src/ast.{h,c}` | AST (one `Node` tagged by `kind`) + helpers |
| `src/parser.{h,c}` | recursive-descent parser |
| `src/module.{h,c}` | module system: resolve `import` across files + std package |
| `src/codegen.{h,c}` | driver: picks the backend by `TargetArch` (not arch-bound) |
| `src/arch/common.{h,c}` | shared backend (arch-independent): types, structs, symtab, tree-shaking |
| `src/arch/x86_64/win.c` | x86-64 win64 backend (emits NASM, stack machine) |
| `src/main.c` | CLI driving the whole pipeline |
| `std/*.mvs` | standard library written in MVS (`io`/`string`/`fmt`/`fs`/`net`) |

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
2. `make test` passes: the golden output + compile-only + compile-fail suite in `tests/run.ps1`.
3. New feature → add an example under `examples/`, register it in `tests/run.ps1` + the Makefile
   `EXAMPLES` list, and regenerate goldens with `tests/run.ps1 -Update`. New error → add a
   `tests/compile_fail/*.mvs` with a `//~ ERROR:` header.

## Feature status (details in GUIDE.md)

**Done:** struct + method/impl + chaining · pointer · real int width · f32 (real 4 bytes)/f64 (SSE) ·
switch/do-while · Rust-style io.out (`{}`/`{:x}` + struct printing + unlimited args) · import + extern ·
stdlib `io`/`fs`/`net`/`string` + `io.in` · args > 4 · tree-shaking · sret · C interop (`export`/`-c`) ·
`--nostd` freestanding · generics + overloading (split by int width) · scope shadowing ·
struct by-value params + struct-returning call as an rvalue · type checking + `as` cast ·
trait + associated function (`Type::new`) + `<T: Trait>` + default method · `String` (heap) +
`String::from`/`from_int` · float xmm across the C boundary both ways (f32 single↔double) ·
function pointer (`func(...) -> T` as a value + indirect `call rax`) · const enforcement ·
default parameter values · single-eval compound assignment · math-style `**` precedence ·
golden test suite (`make test`).

**Remaining:** full 128-bit i128/u128 math · dynamic dispatch (`dyn`/vtable) + multi-condition `where` ·
a real array type · io.out as a library (variadic + reflection) · ARM64/Linux backends.

## Project-specific cautions (full list in RULES.md)

- **Freestanding by default** (RULES §0): the language core must not depend on the OS/CRT. Everything
  touching the OS lives in `std/*.mvs` (opt-in).
- C interop: `export func` = raw symbol name + `global`; `-c` produces a `.obj`; `--nostd` = freestanding obj.
- method: `ns` = struct name (label), `mod` = module (resolves internal calls). Don't confuse them (RULES §5.6).
- linking needs `-llegacy_stdio_definitions -lws2_32`; extern/export names must not clash with NASM reserved
  words (`abs`, etc.).
- floats are stored as a bit-pattern in rax, entering xmm only for math; `io.out` is a compiler intrinsic.

## Documentation

| File | Contents |
|------|----------|
| [README.md](README.md) | overview · install · how to compile · usage |
| [GUIDE.md](GUIDE.md) | language reference · internals · real assembly · status · roadmap |
| [RULES.md](RULES.md) | rules · freestanding philosophy · ABI · developer cautions |
| [CLAUDE.md](CLAUDE.md) | this file: navigation for AI/developers · commands · where to edit |
| [examples/README.md](examples/README.md) | the full list of example programs |
