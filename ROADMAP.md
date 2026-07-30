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

The `core` package EXISTS now and covers most of the table: `core/mem`,
`core/cmp`, `core/ptr`, `core/cstr` (str is a keyword), `core/slice`
(`Slice<T>`), `core/bits`, and `core/arch` (CPU instructions through the new
`asm()` intrinsic), all pure MVS importing under `--nostd` with zero undefined
symbols. std carries the CRT/OS-backed layers (`mem`, `math`, `option`,
`result`, `vec`, `map`, `thread`, `sync`). Still open: `core/option` (needs a
story for the name clash with std/option), `cell`, `sync::atomic` (needs
compare-exchange intrinsics on top of `asm`), `ffi`, `panic` (needs a
freestanding abort hook).

## Language

- Pattern matching phase 3: nested patterns (`Some(Ok(v))`), literal and range
  patterns, and `if` guards. Phases 1 and 2 are done: enums with payload
  variants, exhaustive `match` statements AND expressions, bare patterns, and
  generic enums (`Option`/`Result` are real enums now).
- Concurrency, phase 2: channels (Mutex + condvar), atomics (compiler
  intrinsics), and a `scoped`-style join helper. Phase 1 (threads + Mutex,
  std/thread + std/sync) is done; Send/Sync-style compile-time race checking
  would need an ownership system and is not planned.
- Generic struct extensions: pointer/array type arguments, `impl Trait for Vec<T>`.
- More collections: a `HashSet`, ordered maps, and `remove()` on `HashMap`
  (tombstones). `Vec<T>` and `HashMap<K, V>` are done.
- Arrays of arrays (`[[T; N]; M]`) and slice syntax (`&a[1..3]`).
- Macros are deliberately NOT planned; the decorator form above plus intrinsics
  cover the current needs without a second language layer.

Done from earlier revisions of this list: generic structs (`struct Pair<T, U>`,
monomorphized with their `impl` methods, nested arguments supported), explicit
generic call arguments (`none<i64>()`), `Option<T>`/`Result<T, E>` in std,
`Vec<T>` (growable, bounds-checked, any element type),
`io.out` width/precision specs (`{:8.2}`, `{:08}`, `{:04x}`).

## Tooling

- Growing the `-O` peephole (today: safe two/three-line patterns on all three
  backends; register-liveness-aware rules are open).
- Editor support: a tree-sitter grammar for highlighting.

Done from earlier revisions of this list: `mvs test` (the built-in runner for
the golden + compile-fail suite, no PowerShell needed), `mvs --version`.
