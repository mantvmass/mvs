# MVS Compiler

A compiler for **MVS**, a small low-level language at roughly C's level but with a friendlier,
Rust-flavored syntax. It's written in plain C, **no LLVM, no flex, no bison**, and emits
**x86-64 (Windows) assembly** directly, which is then assembled with `nasm` and linked with `clang`.

> **For education.** This project exists to show how a real compiler works end to end: a
> hand-written lexer and parser, a small type system, and a code generator that produces actual
> x86-64 assembly you can read. It is a learning subset, not a production toolchain.

```
file.mvs ──> lexer ──> parser ──> AST ──> codegen ──> .asm ──> nasm ──> .obj ──> clang ──> .exe
```

## What you need

- **clang**: builds the compiler and also acts as the linker
- **nasm**: assembles the generated `.asm` into a `.obj`

```powershell
clang --version
nasm --version
```

## Building the compiler

`make` builds `mvs.exe`. If you don't have `make`, run the compile command directly. It's a single
clang invocation over the sources:

```powershell
make
```

```powershell
# equivalent to (this is what the Makefile runs):
clang -Wall -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc `
      src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/codegen.c `
      src/arch/common.c src/arch/x86_64/win.c -o mvs.exe
```

## Compiling and running an MVS program

```powershell
.\mvs.exe examples\demo.mvs     # produces examples\demo.exe (mvs.exe calls nasm + clang for you)
.\examples\demo.exe             # run it
```

`mvs.exe` checks for `clang` and `nasm` on your PATH and tells you which versions it uses.

Useful flags:

| Flag | Meaning |
|------|---------|
| `-o <file>` | set the output filename |
| `-S` | emit only the `.asm` and stop (handy for reading the generated assembly) |
| `-c` / `--emit-obj` | emit a `.obj` and stop (to link against C) |
| `--nostd` | freestanding: no std/CRT/OS dependency (emits a `.obj`) |
| `--target elf64` | Linux/ELF (SysV ABI): emits a `.o` to link on Linux (`gcc file.o`) |
| `--keep` | keep intermediate files (`.asm`, `.obj`) |

## Where to look next

- [GUIDE.md](GUIDE.md): the language reference, the memory model, the real assembly the compiler
  emits, and the project's status and roadmap.
- [RULES.md](RULES.md): the design rules and gotchas for working on the compiler itself.
- [examples/](examples/): sample programs grouped by topic (`01_language` … `08_stdlib`); each file
  has a header with its build/run command.

## Project layout

```
src/
  token.h            token kinds
  lexer.{h,c}        hand-written tokenizer
  ast.{h,c}          AST (one Node tagged by kind) + helpers
  parser.{h,c}       recursive-descent parser
  module.{h,c}       module system (imports across files + std)
  codegen.{h,c}      codegen driver (picks a backend by architecture)
  arch/
    common.{h,c}     arch-independent backend: types, struct layout, symtab, tree-shaking
    x86_64/win.{h,c} x86-64 Windows backend (emits NASM/win64)
  main.c             CLI driving the whole pipeline
std/                 standard library written in MVS (io, string, fmt, fs, net)
examples/            sample programs
Makefile             build script
```

The front end (lexer/parser/AST) is architecture-independent; the back end picks a target through a
single interface, so a future ARM64 or Linux target is a new file under `src/arch/` rather than a
rewrite.
