# Roadmap

Where MVS is heading. Everything here is planned or sketched, not promised.
What already exists is documented in [docs/guide.md](docs/guide.md); this file
only lists what is left.

## Platforms

| Target | ABI | Object format | Status |
|--------|-----|---------------|--------|
| x86-64 Windows | win64 | COFF/PE (`nasm -f win64`) | done, default |
| x86-64 Linux | System V AMD64 | ELF64 (`nasm -f elf64`) | done (`--target elf64`) |
| AArch64 Linux | AAPCS64 | ELF64 (GNU as syntax) | done (`--target arm64`, qemu in CI) |
| x86-64 macOS | System V + Mach-O quirks | Mach-O | planned |
| RISC-V Linux | RV64 psABI | ELF64 | under consideration |

The backend split (`src/arch/common.c` holds the logic, per-target files hold
only instruction emission) exists exactly so new rows in this table stay cheap.
Targets that cannot run locally are verified on CI with cross toolchains and
qemu-user.

## Language

- Pattern matching phase 3: nested patterns (`Some(Ok(v))`), literal and range
  patterns, `if` guards. Phases 1 and 2 shipped (payload variants, exhaustive
  `match` as statement and expression, bare patterns, generic enums).
- Concurrency phase 2: channels, atomics as compiler intrinsics, a scoped join
  helper. Threads + Mutex shipped. Send/Sync-style race checking would need an
  ownership system and is not planned.
- Generic struct extensions: array type arguments, `impl Trait for Vec<T>`.
- More collections: `HashSet`, ordered maps, `remove()` on `HashMap`
  (needs tombstones).
- Arrays of arrays (`[[T; N]; M]`) and slice syntax (`&a[1..3]`).
- Macros are deliberately not planned. Decorators plus intrinsics cover the
  current needs without a second language layer.

## Codegen: an IR

The biggest open decision. Today the pipeline is AST straight to assembly:
`gen_expr` sees one node at a time, so it plays it safe with the temp stack,
and `-O` can never grow past peephole patterns. An intermediate representation
(three-address code between the AST and the backends) would change that:

- AST lowers to IR once, replacing `gen_expr`/`gen_stmt` in all three
  backend files; each backend then only translates IR to instructions.
- Optimizations (CSE, strength reduction, constant folding, real register
  allocation over every value, not just named locals) are IR-to-IR passes,
  written once for all targets. A plain IR + regalloc compiler lands around
  1.2-1.5x gcc -O2; the current stack machine sits at about 3x.
- New features lower to IR once instead of being emitted three times.

The price: rewriting three stable, golden-tested backends, and every golden
diff breaks in the transition (the differential tests are the safety net).
The deeper cost is the project's stated point: with an optimizer in the
middle, `-S` output no longer maps line by line back to the source. Either
the 3x gap is the price of readable output, or readability is the price of
closing it. Undecided, so this stays a sketch.

## Core library

`core` is the pure-MVS package that stays importable under `--nostd`. Shipped:
`mem`, `cmp`, `ptr`, `cstr`, `slice`, `bits`, `arch` (CPU instructions through
the `asm()` intrinsic). Still open:

- `core/option`: needs a story for the name clash with `std/option`.
- `core/cell`: interior mutability.
- `core/sync::atomic`: needs compare-exchange intrinsics on top of `asm`.
- `core/ffi`: C-compatible type aliases.
- `core/panic`: needs a freestanding abort hook.

## Conditional compilation

`@compile(target_os/target_arch)` works today (guide section 4.13). Possible
extensions, none committed: gating blocks inside a function, `not`/`any`
combinators, user-defined keys (feature flags).

## Hardening

Roughly in order of value:

- More real programs, and larger ones. Still the strongest bug finder we have:
  `examples/10_json` and `examples/11_vm` each surfaced compiler gaps no test
  suite caught.
- More differential pairs, ideally generated rather than hand-written.
- Integer overflow checks behind a flag (trap on signed overflow).
- DWARF variable info. `-g` gives line info today; a debugger cannot print
  locals yet.
- Performance: locals are register allocated and simple operands skip the temp
  stack (about 3x gcc -O2 on the benchmark, from 8.4x). Going further means an
  IR; see the codegen section above.
- Deterministic release builds and a signed-artifact story.

## Tooling

- Growing the `-O` peephole: today it is safe two/three-line patterns;
  register-liveness-aware rules are open.
- Editor support: a tree-sitter grammar for highlighting.

Memory management stays manual by design; see
[docs/rules.md](docs/rules.md) section 0.2.
