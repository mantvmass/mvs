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

## Conditional compilation (design sketch)

Cross-platform code needs a way to select code per target at compile time. The
working idea is a decorator/attribute form; the syntax is NOT final:

```rust
@compile(target_os = "linux")
func page_size() -> usize { return 4096; }

@compile(target_os = "windows")
func page_size() -> usize { return dwPageSize(); }

@compile(target_arch = "aarch64")
func pause() -> void { /* wfe */ }
```

Open questions: attribute grammar (`@compile(...)` vs `#[cfg(...)]`), whether it
gates whole items only or also blocks, and how it interacts with imports and
tree-shaking. With three ABIs now live (win64, SysV, AAPCS64) there are real use
cases to design against; this is the next language feature up.

## Core library (`core`), modeled on Rust

Today `std/` is small (`io`, `fs`, `net`, `string`, `fmt`) and CRT-backed. The plan
is a `core` layer that works under `--nostd` too:

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

`Option`/`Result` need generic STRUCTS (today only functions are generic), and
`core::arch` needs the conditional-compilation story above; both are prerequisites
worth building first.

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
