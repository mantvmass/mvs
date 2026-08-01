# CLAUDE.md

Guidance for Claude Code (and any AI session) working in this project.

> **Read first:** [docs/rules.md](docs/rules.md), the rules you must not break.
> [docs/guide.md](docs/guide.md), the language reference and internals.

## What this project is

A compiler for **MVS**, a low-level language (C-level, but easier to read),
written in plain C, emitting NASM assembly directly for x86-64 Windows (win64,
default), x86-64 Linux (`--target elf64`), and AArch64 Linux (`--target arm64`).
No LLVM, no flex/bison: the lexer and parser are hand-written.

The constraints that matter most (full detail in docs/rules.md):

1. No LLVM / flex / bison. Generate assembly yourself.
2. Everything is English only: comments, program output, errors, docs.
3. Toolchain is clang + nasm on Windows (gcc + nasm on Linux/CI).
4. The front end must not bind to x86; new architectures reuse `arch/common.c`.

## Main commands

```powershell
# ONE command that runs every check there is (Linux/WSL). Use this instead of
# spot-checking; exit code 0 means everything passed.
#   sh scripts/audit.sh          full
#   sh scripts/audit.sh quick    skips arm64 and shortens the fuzz

make            # build the compiler (clang, see Makefile for the file list)
make test       # golden output + compile-only + compile-fail suite
.\mvs.exe test  # the same portable core without PowerShell

.\mvs.exe examples\demo.mvs           # compile, then run .\examples\demo.exe
.\mvs.exe examples\demo.mvs -S --keep # look at the assembly, no nasm/clang
```

## Code architecture

```
.mvs -> [lexer] -> tokens -> [parser] -> AST -> [codegen driver] -> [arch backend] -> .asm/.s
    win64 (default): nasm -f win64 -> .obj -> clang -> .exe
    elf64:           nasm -f elf64 -> .o   (link on Linux: gcc file.o -no-pie -lm)
    arm64:           aarch64-linux-gnu-gcc -c -> .o (link with the cross gcc, run under qemu)
```

| File | Role |
|------|------|
| `src/token.h`, `src/lexer.{h,c}` | token kinds, hand-written tokenizer |
| `src/ast.{h,c}`, `src/parser.{h,c}` | AST (one `Node` tagged by `kind`), recursive descent |
| `src/module.{h,c}` | resolve `import` across files + the std and core packages |
| `src/generic.c` | passes after parse: enum desugar, monomorphize, resolve_overloads, typecheck |
| `src/diag.{h,c}` | Rust-style diagnostics: excerpts, carets, help notes, warnings |
| `src/codegen.{h,c}` | driver: picks the backend by `TargetArch` |
| `src/arch/common.{h,c}` | shared backend logic: types, structs, symtab, tree-shaking |
| `src/arch/x86_64/win.c` | win64 backend (NASM, stack machine) |
| `src/arch/x86_64/sysv.c` | Linux/ELF backend (SysV ABI) |
| `src/arch/arm64/linux.c` | AArch64 backend (AAPCS64, GNU as syntax) |
| `src/main.c` | CLI driving the pipeline |
| `std/*.mvs`, `core/*.mvs` | standard library and freestanding package, written in MVS |

## Where to edit when adding a feature

- token/keyword: `token.h` + `lexer.c` (the `KEYWORDS` table)
- syntax: `parser.c` (+ `ND_*` in `ast.h`)
- type checking / trait bound / generic / overload: `generic.c`
- instruction emission: `arch/x86_64/win.c` (and the sysv/arm64 twins);
  shared logic goes in `arch/common.c`
- import behavior: `module.c`; stdlib functions: `std/*.mvs`
- a new arch: `arch/<arch>/<os>.c` (reuse `common.c`) + `TargetArch`
  (`codegen.h`) + a case in `codegen.c`

## Before calling it done

1. `make` passes with no warnings.
2. `make test` passes.
3. New feature: add an example under `examples/`, register it in
   `scripts/test.ps1` + the Makefile `EXAMPLES` list, regenerate goldens with
   `scripts/test.ps1 -Update`, and read the new golden file yourself.
4. New error: add a `tests/compile_fail/*.mvs` with a `//~ ERROR:` header.

## Status

The feature list is long and lives in [docs/guide.md](docs/guide.md): section 4
is the reference for what exists, section 8 for the limits, ROADMAP.md for what
is left. Assume a mainstream feature already works (generics, traits, enums +
match, Option/Result, Vec/HashMap, threads, http, testing, three backends) and
check the guide before implementing anything.

Worth knowing beyond the guide:

- **Performance.** `sh scripts/bench.sh` times the same program built by MVS
  and by C. Locals are register allocated (busiest scalars whose address is
  never taken go to rbx/r12/r13/r14, x19-x22 on arm64) and a binary op whose
  operands are both simple skips the temp stack. That took the benchmark from
  8.4x gcc -O2 to about 3x; the remaining gap is the stack machine used for
  everything else.
- **How the audit stays exhaustive.** `scripts/audit.sh` is the one command.
  Its enumerated axes: `matrix.sh` (every type through every context),
  `matrix_features.sh` (every feature through every context),
  `matrix_modules.sh` (every import form crossed with every kind of exported
  thing), `api_coverage.py` (every std/core function must be called by a test
  or example), and `tests/diff/ops_*` from `gen_ops_diff.py` (every operator on
  every numeric type with C as the oracle, 1924 checks). When a bug turns up,
  add the axis that would have caught it, not a single regression test.
- **Dogfood.** Two real programs, 2477 lines of MVS between them:
  `examples/10_json` and `examples/11_vm`. Run them before claiming a language
  change is ergonomic; both have caught gaps the suite missed.

## Project-specific cautions (full list in docs/rules.md)

- Freestanding by default (rules.md section 0): the core must not depend on
  the OS/CRT. Everything touching the OS lives in `std/*.mvs`.
- C interop: `export func` = raw symbol + `global`; `-c` produces a `.obj`;
  `--nostd` = freestanding object.
- Methods: `ns` = struct name (label), `mod` = module (resolves internal
  calls). Don't confuse them (rules.md section 5.6).
- Linking needs `-llegacy_stdio_definitions -lws2_32`; extern/export names
  must not clash with NASM reserved words (`abs`, etc.).
- Floats are stored as a bit-pattern in rax, entering xmm only for math;
  `io.out` is a compiler intrinsic.

## Documentation map

| File | Contents |
|------|----------|
| [README.md](README.md) | overview, install, how to build |
| [docs/guide.md](docs/guide.md) | language reference, internals, real assembly, status |
| [docs/rules.md](docs/rules.md) | rules, freestanding philosophy, ABI, developer cautions |
| [ROADMAP.md](ROADMAP.md) | what is planned |
| [docs/examples.md](docs/examples.md) | the example programs |
