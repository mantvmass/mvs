# Roadmap

Where MVS is heading. Everything here is planned or sketched, not promised; the
current state of the language lives in [docs/guide.md](docs/guide.md).

## Platform support

| Target | ABI | Object format | Status |
|--------|-----|---------------|--------|
| x86-64 Windows | win64 | COFF/PE (`nasm -f win64`) | done, default target |
| x86-64 Linux | System V AMD64 | ELF64 (`nasm -f elf64`) | done (`--target elf64`), CI-tested |
| AArch64 Linux | AAPCS64 | ELF64 (GNU as syntax) | done (`--target arm64`), CI-tested under qemu |
| x86-64 macOS | System V + Mach-O quirks | Mach-O | planned |
| RISC-V Linux | RV64 psABI | ELF64 | under consideration |

The backend split (`src/arch/common.c` holds all logic, per-target files hold only
instruction emission) exists exactly so new rows in this table stay cheap. Targets
that cannot run locally are verified on GitHub CI (cross toolchains + qemu-user).

## Conditional compilation: done

`@compile(target_os = ...)` / `@compile(target_arch = ...)` is implemented (see
[docs/guide.md](docs/guide.md) section 4.13). It gates whole top-level items, is
resolved in the module loader before duplicate/type checks, and already carries
`std/net` (Winsock vs POSIX sockets in one file). Possible extensions, not
committed: gating blocks inside a function, a `not`/`any` combinator form, and
custom user-defined keys (feature flags).

## Core library (`core`), modeled on Rust

Today `std/` is CRT-backed: `io`, `fs`, `net` (cross-platform via `@compile`),
`string`, `fmt`, `math` (libm + overloaded integer/float helpers), and `mem`
(allocation + raw memory ops). The plan is a `core` layer that works under
`--nostd` too:

| Module | Purpose |
|--------|---------|
| `core::mem` | memory operations (`size_of`, `swap`, `copy`) |
| `core::ptr` | raw pointer utilities (`null`, `read`, `write`, offsets) |
| `core::slice` | slice utilities over `{ptr, len}` pairs |
| `core::str` | UTF-8 string operations |
| `core::cmp` | comparisons (`min`, `max`, `Ordering`, `Eq`/`Ord` traits) |
| `core::option` | `Option<T>` |
| `core::result` | `Result<T, E>` |
| `core::cell` | interior mutability (`Cell`, `RefCell`) |
| `core::sync::atomic` | atomic operations |
| `core::ffi` | C-compatible type aliases |
| `core::panic` | panic/abort support with a `--nostd` hook |
| `core::arch` | architecture intrinsics (inline `asm`, SIMD, `pause`/`wfe`) |

`std/mem` and `std/math` are the first (CRT-backed) slice of this plan.
`Option`/`Result` need generic STRUCTS (today only functions are generic); that is
the main prerequisite left now that conditional compilation exists.

## Language

- Generic structs (`struct Vec<T>`), the gateway to `Option`/`Result`/collections.
- Pattern matching (`match`) once `Option`/`Result` exist.
- Arrays of arrays (`[[T; N]; M]`) and slice syntax (`&a[1..3]`).
- `io.out` gaining width/precision format specs (`{:8.2}`).
- Macros are deliberately NOT planned; the decorator form above plus intrinsics
  cover the current needs without a second language layer.

## Tooling

- `mvs test` subcommand running the golden suite without PowerShell.
- Better `-O` story: peephole cleanup of the stack-machine output.
- Editor support: a tree-sitter grammar for highlighting.
