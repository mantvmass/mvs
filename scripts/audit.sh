#!/bin/sh
# THE audit: one command that runs every check this project has.
#
# It exists because sampling the compiler by hand finds a couple of bugs per
# session and never converges. Everything below is either exhaustive over some
# axis (the feature matrix, every example under -O) or an independent oracle
# (the C reference, the sanitizers, the fuzzer). Run this and the answer to
# "is anything broken" is a single exit code, not an opinion.
#
#   1  golden suites        every example, run and diffed, on elf64 and arm64
#   2  built-in runner      mvs test: goldens + unit tests + compile-fail
#   3  feature matrix       every type through every context, self-checking
#   3b feature matrix 2     traits, function pointers, generics, pointers, flow
#   4  differential tests   the same programs in MVS and C must agree
#   5  optimizer equality   every example must print the same thing under -O
#   6  freestanding         --nostd objects with zero undefined symbols
#   7  debug info           -g line tables reach back to .mvs
#   8  sanitizers           the whole suite under ASan and UBSan
#   9  fuzzing              mutations, token soup and nesting stress
#
# Usage: sh scripts/audit.sh [quick]   (quick skips arm64 and shortens the fuzz)
set -u
QUICK=${1:-full}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
fails=""
step() { printf '\n=== %s ===\n' "$1"; }
note() { fails="$fails\n  - $1"; }

CFLAGS_C="-Wall -Wextra -Werror -O2 -Isrc"
SRC="src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/generic.c src/diag.c src/codegen.c src/arch/common.c src/arch/x86_64/win.c src/arch/x86_64/sysv.c src/arch/arm64/linux.c"

step "build the compiler (warnings are errors)"
if gcc $CFLAGS_C $SRC -o mvs; then echo "ok"; else echo "FAILED"; note "compiler build"; exit 1; fi

step "1. golden suite: elf64"
if sh scripts/test_elf_ci.sh > /tmp/audit_elf.txt 2>&1; then tail -1 /tmp/audit_elf.txt; else tail -5 /tmp/audit_elf.txt; note "elf64 golden suite"; fi

if [ "$QUICK" != "quick" ]; then
    step "1b. golden suite: arm64 under qemu"
    if sh scripts/test_arm64_ci.sh > /tmp/audit_a64.txt 2>&1; then tail -1 /tmp/audit_a64.txt; else tail -5 /tmp/audit_a64.txt; note "arm64 golden suite"; fi
fi

step "2. built-in runner (mvs test)"
if ./mvs test > /tmp/audit_test.txt 2>&1; then tail -1 /tmp/audit_test.txt; else tail -8 /tmp/audit_test.txt; note "mvs test"; fi

step "3. feature matrix: every type through every context"
if sh scripts/matrix.sh elf64 > /tmp/audit_mx.txt 2>&1; then tail -1 /tmp/audit_mx.txt; else tail -10 /tmp/audit_mx.txt; note "feature matrix (elf64)"; fi
if [ "$QUICK" != "quick" ]; then
    if sh scripts/matrix.sh arm64 > /tmp/audit_mx64.txt 2>&1; then tail -1 /tmp/audit_mx64.txt; else tail -10 /tmp/audit_mx64.txt; note "feature matrix (arm64)"; fi
fi

step "3b. feature matrix: traits, function pointers, generics, pointers, flow"
if sh scripts/matrix_features.sh elf64 > /tmp/audit_mf.txt 2>&1; then tail -1 /tmp/audit_mf.txt; else tail -12 /tmp/audit_mf.txt; note "feature matrix 2 (elf64)"; fi
if [ "$QUICK" != "quick" ]; then
    if sh scripts/matrix_features.sh arm64 > /tmp/audit_mf64.txt 2>&1; then tail -1 /tmp/audit_mf64.txt; else tail -12 /tmp/audit_mf64.txt; note "feature matrix 2 (arm64)"; fi
fi

step "4. differential tests against the C reference"
if sh scripts/difftest.sh elf64 > /tmp/audit_diff.txt 2>&1; then tail -1 /tmp/audit_diff.txt; else tail -10 /tmp/audit_diff.txt; note "difftest (elf64)"; fi
if [ "$QUICK" != "quick" ]; then
    if sh scripts/difftest.sh arm64 > /tmp/audit_diff64.txt 2>&1; then tail -1 /tmp/audit_diff64.txt; else tail -10 /tmp/audit_diff64.txt; note "difftest (arm64)"; fi
fi

step "5. optimizer equality: every example must be identical under -O"
opt_fail=0
opt_run=0
for g in tests/expected/*.txt; do
    name=$(basename "$g" .txt)
    case "$name" in interop_*|*_args) continue ;; esac       # need a C toolchain / argv
    # golden name -> example path (groups are NN_word, so the second _ splits)
    rel=$(echo "$name" | sed 's/^\([0-9][0-9]*_[a-z_]*\)_/\1\//')
    src="examples/$rel.mvs"
    [ -f "$src" ] || continue
    opt_run=$((opt_run+1))
    ./mvs "$src" -O --target elf64 >/dev/null 2>&1 || { note "-O compile $rel"; opt_fail=$((opt_fail+1)); continue; }
    gcc "examples/$rel.o" -o /tmp/audit_opt -no-pie -lm -lpthread 2>/dev/null || { note "-O link $rel"; opt_fail=$((opt_fail+1)); continue; }
    /tmp/audit_opt > /tmp/audit_opt_out.txt 2>&1
    tr -d '\r' < "$g" > /tmp/audit_opt_want.txt
    if ! diff -q /tmp/audit_opt_want.txt /tmp/audit_opt_out.txt >/dev/null 2>&1; then
        echo "  DIFFERS under -O: $rel"; note "-O output $rel"; opt_fail=$((opt_fail+1))
    fi
    rm -f "examples/$rel.o"
done
if [ $opt_fail -eq 0 ]; then echo "ok: $opt_run example(s) byte-identical with and without -O"; fi

step "6. freestanding: --nostd objects must have no undefined symbols"
nostd_fail=0
for t in examples/07_c_interop/freestanding examples/09_no_std/kernel examples/09_no_std/bump_alloc examples/09_no_std/use_core examples/09_no_std/intrinsics; do
    ./mvs "$t.mvs" --nostd --target elf64 >/dev/null 2>&1 || { note "nostd compile $t"; nostd_fail=1; continue; }
    u=$(nm -u "$t.o" 2>/dev/null | wc -l)
    [ "$u" -eq 0 ] || { echo "  $t has $u undefined symbol(s)"; note "nostd undefined $t"; nostd_fail=1; }
    rm -f "$t.o"
done
[ $nostd_fail -eq 0 ] && echo "ok: 5 freestanding objects, 0 undefined symbols"

step "7. debug info: -g must map back to .mvs"
./mvs examples/03_structs/methods.mvs -g --target elf64 >/dev/null 2>&1
gcc examples/03_structs/methods.o -o /tmp/audit_dbg -no-pie -lm 2>/dev/null
if [ "$(objdump --dwarf=decodedline /tmp/audit_dbg 2>/dev/null | grep -c 'methods\.mvs')" -gt 0 ]; then
    echo "ok: line table names the .mvs source"
else
    echo "FAILED"; note "debug line info"
fi
rm -f examples/03_structs/methods.o examples/03_structs/methods.asm

step "8. sanitizers: the whole suite under ASan + UBSan"
if gcc -g -O1 -fsanitize=address,undefined -Isrc $SRC -o mvs_asan 2>/dev/null; then
    if ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./mvs_asan test > /tmp/audit_san.txt 2>&1; then
        tail -1 /tmp/audit_san.txt
    else
        tail -8 /tmp/audit_san.txt; note "sanitized suite"
    fi
else
    echo "skipped (no sanitizer support)"
fi

step "9. fuzzing the compiler"
N=$([ "$QUICK" = "quick" ] && echo 60 || echo 300)
if [ -x ./mvs_asan ]; then
    if ASAN_OPTIONS=detect_leaks=0 MVS=./mvs_asan sh scripts/fuzz.sh "$N" > /tmp/audit_fuzz.txt 2>&1; then
        tail -1 /tmp/audit_fuzz.txt
    else
        tail -10 /tmp/audit_fuzz.txt; note "fuzzing"
    fi
fi
rm -f mvs_asan

printf '\n===============================\n'
if [ -z "$fails" ]; then
    echo "AUDIT CLEAN: every check passed"
    exit 0
fi
printf 'AUDIT FOUND PROBLEMS:%b\n' "$fails"
exit 1
