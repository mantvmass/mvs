# MVS

[![CI](https://github.com/mantvmass/mvs/actions/workflows/ci.yml/badge.svg)](https://github.com/mantvmass/mvs/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg)](#license)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20x64%20%7C%20Linux%20x64%20%7C%20Linux%20ARM64-informational.svg)](ROADMAP.md)
[![Written in](https://img.shields.io/badge/written%20in-plain%20C-555.svg)](src/)
[![Dependencies](https://img.shields.io/badge/deps-no%20LLVM%2C%20no%20flex%2Fbison-success.svg)](docs/rules.md)

A compiler for **MVS**, a small low-level language at roughly C's level with a
friendlier, Rust-flavored syntax. The lexer, parser, type checker, and code
generators are all hand-written in plain C, and the output is real assembly you
can read.

> **For education.** This project exists to show how a real compiler works end to
> end. It is a learning subset, not a production toolchain.

```rust
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

The language has structs, methods, traits (static and `dyn` dispatch), generics,
overloading, real `[T; N]` arrays, 128-bit integers, conditional compilation
(`@compile`), a small std library, C interop in both directions, Rust-style
diagnostics, and a freestanding `--nostd` mode for OS work. Three backends
(x86-64 Windows, x86-64 Linux/ELF, AArch64 Linux) share one
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
| [docs/guide.md](docs/guide.md) | the language reference, memory model, real emitted assembly, internals |
| [docs/rules.md](docs/rules.md) | design rules, ABI details, and gotchas for working on the compiler |
| [docs/examples.md](docs/examples.md) | the full list of example programs (in [examples/](examples/)) |
| [ROADMAP.md](ROADMAP.md) | planned platforms and the core library plan |
| [CLAUDE.md](CLAUDE.md) | working notes for AI assistants and contributors |

## License

Dual-licensed under either the [Apache License 2.0](LICENSE-APACHE) or the
[MIT license](LICENSE-MIT), at your option.

Copyright (c) 2026 Phumin Maliwan
