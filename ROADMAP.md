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

- Pattern matching phase 3: shipped. Nested patterns (`Some(Ok(v))`), literal
  patterns in payloads, range patterns (`1..=10`), `if` guards, wildcard
  sub-patterns, value match on integers, match-as-expression. Phases 1-3 done.
- Concurrency phase 2: shipped. `__atomic` compiler intrinsic (load, store, add,
  sub, cas, xchg on all three backends), `core/atomic` wrapper, `CondVar` in
  `std/sync`, bounded MPMC `Channel` in `std/channel`. Scoped join helper is
  still open. Send/Sync-style race checking would need an ownership system and
  is not planned.
- Generic struct extensions: array type arguments, `impl Trait for Vec<T>`.
- More collections: `HashSet`, ordered maps. (`remove()` on `HashMap` shipped
  with tombstone deletion.)
- Arrays of arrays (`[[T; N]; M]`) and slice syntax (`&a[1..3]`).
- Macros are deliberately not planned. Decorators plus intrinsics cover the
  current needs without a second language layer.

## Codegen: an IR

The IR layer is implemented (`src/ir.h`, `src/ir.c`, `src/lower.h`,
`src/lower.c`): three-address code with virtual registers, typed
instructions, and high-level call nodes. The AST lowers to IR once; each
backend then translates IR to assembly. This eliminates the duplicated
`gen_expr`/`gen_stmt` logic across three backends.

What the IR enables next (none committed):

- Optimization passes (CSE, strength reduction, constant folding, real
  register allocation over every value). A plain IR + regalloc compiler
  lands around 1.2-1.5x gcc -O2; the current stack machine sits at about 3x.
- New features lower to IR once instead of being emitted three times.

The trade-off remains: with an optimizer in the middle, `-S` output no
longer maps line by line back to the source.

## Core library

`core` is the pure-MVS package that stays importable under `--nostd`. Shipped:
`mem`, `cmp`, `ptr`, `cstr`, `slice`, `bits`, `arch` (CPU instructions through
the `asm()` intrinsic), `panic` (freestanding abort with replaceable handler),
`fmt` (number-to-string formatting without CRT), `volatile` (MMIO access),
`option` (freestanding Option<T>), `result` (freestanding Result<T, E>),
`math` (pure integer math), `hash` (FNV-1a, splitmix64), `sort` (insertion
sort, binary search, min/max). Still open:

- `core/cell`: interior mutability.
- `core/ffi`: C-compatible type aliases.

`core/atomic` shipped (load, store, add, sub, cas, exchange via the
`__atomic` compiler intrinsic).

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
- Editor support: the tree-sitter grammar shipped (`editor/tree-sitter-mvs`,
  parses every `.mvs` in the repo at 100%), and `editor/vscode-mvs` carries a
  TextMate grammar for VS Code, which does not speak tree-sitter. An LSP
  server would be the next step, but it needs the compiler callable as a
  library first.

Memory management stays manual by design; see
[docs/rules.md](docs/rules.md) section 0.2.
