# MVS

A compiler for **MVS**, a small low-level language at roughly C's level with a
friendlier, Rust-flavored syntax. Written in plain C, **no LLVM, no flex, no
bison**: the lexer, parser, type checker, and code generator are all hand-written,
and the output is real x86-64 assembly you can read.

> **For education.** This project exists to show how a real compiler works end to
> end. It is a learning subset, not a production toolchain.

```
file.mvs -> lexer -> parser -> typecheck -> codegen -> .asm -> nasm -> .obj -> link -> run
```

## Install

Prebuilt binaries from the latest release (macOS is on the [roadmap](ROADMAP.md)):

Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/mantvmass/mvs/main/scripts/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/mantvmass/mvs/main/scripts/install.ps1 | iex
```

Uninstall with `scripts/uninstall.sh` / `scripts/uninstall.ps1`.

## Build from source

Needs `clang` and `nasm` on PATH (Windows), or `gcc` and `nasm` (Linux).

```powershell
make                          # build the compiler (mvs.exe)
.\mvs.exe examples\demo.mvs   # compile an MVS program to .exe
.\examples\demo.exe           # run it
make test                     # run the full test suite
```

On Linux, or targeting Linux from Windows:

```sh
./mvs examples/demo.mvs --target elf64   # SysV ABI, ELF64 object
gcc examples/demo.o -o demo -no-pie -lm && ./demo
```

## Highlights

Structs + methods + traits (static AND dynamic dispatch via `dyn Trait`) · generics
with bounds and `where` clauses · function overloading · real `[T; N]` arrays ·
full 128-bit integers · function pointers · heap `String` · conditional compilation
(`@compile(target_os/target_arch)`) · modules + a std library
(`io`/`fs`/`net`/`string`/`fmt`/`math`/`mem`, with cross-platform TCP networking) ·
C interop in both directions · Rust-style compiler diagnostics · freestanding
`--nostd` mode for OS/bare-metal work · three backends (x86-64 win64, x86-64
SysV/ELF, AArch64 AAPCS64) sharing one architecture-independent core, all CI-tested
against the same golden outputs.

## Documentation

| Where | What |
|-------|------|
| [docs/guide.md](docs/guide.md) | the language reference, memory model, real emitted assembly, internals |
| [docs/rules.md](docs/rules.md) | design rules, ABI details, and gotchas for working on the compiler |
| [docs/examples.md](docs/examples.md) | the full list of example programs (in [examples/](examples/)) |
| [ROADMAP.md](ROADMAP.md) | planned platforms, conditional compilation, the core library plan |
| [CLAUDE.md](CLAUDE.md) | working notes for AI assistants and contributors |

## License

Dual-licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT license ([LICENSE-MIT](LICENSE-MIT))

at your option.

Copyright (c) 2026 Phumin Maliwan
