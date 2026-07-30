<div align="center">

# MVS

**A small low-level language with Rust-flavored syntax and a compiler written by hand in plain C.**

No LLVM, no flex, no bison: the output is real assembly you can read.

[![CI](https://img.shields.io/github/actions/workflow/status/mantvmass/mvs/ci.yml?branch=main&style=flat-square&logo=githubactions&logoColor=white&label=CI)](https://github.com/mantvmass/mvs/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue?style=flat-square)](#license)
[![Platforms](https://img.shields.io/badge/platforms-win64%20%C2%B7%20elf64%20%C2%B7%20arm64-8250df?style=flat-square)](ROADMAP.md)
[![Written in C](https://img.shields.io/badge/written%20in-plain%20C-A8B9CC?style=flat-square&logo=c&logoColor=white)](src/)

[Install](#install) · [Build](#build-from-source) · [Docs](#documentation) · [Examples](docs/examples.md) · [Roadmap](ROADMAP.md)

</div>

> **For education.** This project exists to show how a real compiler works end to
> end (lexer, parser, type checker, and three code generators, all hand-written).
> It is a learning subset, not a production toolchain.
>
> **Memory is managed by hand, on purpose.** No GC, no reference counting, no
> destructors: you allocate, you free, exactly like C. See
> [docs/rules.md](docs/rules.md) section 0.2 for the reasoning.

```txt
import { io, math } from "std";

struct Point { x: f64; y: f64; }

impl Point {
    func len(self: *Point) -> f64 {
        return math.sqrt(self.x * self.x + self.y * self.y);
    }
}

func main() -> i8 {
    let p: Point = Point { x: 3.0, y: 4.0 };
    io.out("len = {}", p.len());   // len = 5.000000
    return 0;
}
```

The language has structs, methods, traits (static and `dyn` dispatch), generics
(functions AND structs: `Vec<i64>::new()`), Rust-style enums with exhaustive
`match`, `Option`/`Result`, threads + Mutex, overloading, real `[T; N]` arrays,
128-bit integers, conditional compilation (`@compile`), a std library plus a
freestanding `core` package, built-in testing (`mvs test` + `*.test.mvs`),
C interop in both directions, Rust-style diagnostics, and a `--nostd` mode for
OS work. Three backends (x86-64 Windows, x86-64 Linux/ELF, AArch64 Linux) share
one architecture-independent core and are CI-tested against the same golden
outputs.

## Install

Prebuilt binaries from the latest release:

```sh
# Linux
curl -fsSL https://raw.githubusercontent.com/mantvmass/mvs/main/scripts/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/mantvmass/mvs/main/scripts/install.ps1 | iex
```

Uninstall with `scripts/uninstall.sh` / `scripts/uninstall.ps1`.

## Build from source

Needs `clang` + `nasm` on PATH (Windows), or `gcc` + `nasm` (Linux):

```powershell
make                          # build the compiler
.\mvs.exe examples\demo.mvs   # compile a program, then run .\examples\demo.exe
make test                     # run the full test suite
```

Cross targets (`--target elf64` / `--target arm64`), the generated assembly
(`-S --keep`), and C interop are covered in the [guide](docs/guide.md).

## Documentation

| Where | What |
|-------|------|
| [docs/guide.md](docs/guide.md) | the language reference, memory model, real emitted assembly, internals |
| [docs/rules.md](docs/rules.md) | design rules, ABI details, and gotchas for working on the compiler |
| [docs/examples.md](docs/examples.md) | the full list of example programs (in [examples/](examples/)) |
| [ROADMAP.md](ROADMAP.md) | planned platforms and the core library plan |
| [CLAUDE.md](CLAUDE.md) | working notes for AI assistants and contributors |

## License

Dual-licensed under either the [Apache License 2.0](LICENSE-APACHE) or the
[MIT license](LICENSE-MIT), at your option.

Copyright (c) 2026 Phumin Maliwan
