<div align="center">

# The MVS Compiler

A small low-level language with Rust-flavored syntax, compiled by hand in plain C.
No LLVM, no flex, no bison: the output is real assembly you can read.

[![CI](https://img.shields.io/github/actions/workflow/status/mantvmass/mvs/ci.yml?branch=main&style=flat-square&logo=githubactions&logoColor=white&label=CI)](https://github.com/mantvmass/mvs/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue?style=flat-square)](#license)
[![Platforms](https://img.shields.io/badge/platforms-win64%20%C2%B7%20elf64%20%C2%B7%20arm64-8250df?style=flat-square)](ROADMAP.md)

[Install](#install) · [Build](#build-from-source) · [Docs](#documentation) · [Examples](docs/examples.md) · [Roadmap](ROADMAP.md)

</div>

For education. This project exists to show how a real compiler works end to end: lexer, parser,
type checker, and three code generators, all written by hand. It is a learning
subset, not a production toolchain. Memory is managed by hand, on purpose: no GC,
no reference counting, no destructors. You allocate, you free, exactly like C
(the reasoning is in [docs/rules.md](docs/rules.md) section 0.2).

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

Despite the size, the language covers a lot: structs and methods, traits with
both static and `dyn` dispatch, generics over functions and structs, Rust-style
enums with exhaustive `match`, `Option`/`Result`, threads, 128-bit integers,
real `[T; N]` arrays, C interop in both directions, a std library plus a
freestanding `core` package, built-in testing, and a `--nostd` mode for OS work.
Three backends (x86-64 Windows, x86-64 Linux, AArch64 Linux) share one
architecture-independent core and are CI-tested against the same golden outputs.

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
| [docs/guide.md](docs/guide.md) | language reference, memory model, real emitted assembly, internals |
| [docs/rules.md](docs/rules.md) | design rules, ABI details, gotchas for working on the compiler |
| [docs/examples.md](docs/examples.md) | the example programs (in [examples/](examples/)) |
| [ROADMAP.md](ROADMAP.md) | what is planned, what is under consideration |
| [CLAUDE.md](CLAUDE.md) | working notes for AI assistants and contributors |

## License

Dual-licensed under either the [Apache License 2.0](LICENSE-APACHE) or the
[MIT license](LICENSE-MIT), at your option.

Copyright (c) 2026 Phumin Maliwan
