#!/bin/sh
# ARM64 driver: build each example with --target arm64 using a native Linux build of
# the compiler, link with the AArch64 cross gcc, run under qemu-aarch64, and diff
# stdout against the shared goldens in tests/expected/ (same files as win64/elf64).
# Needs: gcc-aarch64-linux-gnu, qemu-user.
# Usage: sh scripts/test_arm64_ci.sh   (from the repo root; expects ./mvs built natively)
set -u
MVS=${MVS:-./mvs}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
QEMU=${QEMU:-qemu-aarch64}
fail=0
pass=0
tests="examples/demo
examples/01_language/hello
examples/01_language/types
examples/01_language/operators
examples/01_language/casts
examples/01_language/control
examples/01_language/bitwise
examples/01_language/arrays
examples/01_language/int128
examples/01_language/shadow
examples/02_functions/generics
examples/02_functions/overload
examples/02_functions/recursion
examples/02_functions/funcptr
examples/02_functions/defaults
examples/03_structs/structs
examples/03_structs/methods
examples/03_structs/pointers
examples/03_structs/compound
examples/04_traits/traits
examples/04_traits/display
examples/04_traits/dynamic
examples/05_strings/strings
examples/06_modules/use_import
examples/08_stdlib/io_demo
examples/08_stdlib/floats
examples/08_stdlib/lib_out"

for t in $tests; do
    name=$(echo "$t" | sed 's|^examples/||; s|/|_|g')
    if ! "$MVS" "$t.mvs" --target arm64 >/dev/null 2>&1; then
        echo "FAIL  $name (mvs compile)"; fail=$((fail+1)); continue
    fi
    if ! "$CROSS" "$t.o" -o /tmp/mvs_a64 -static -lm 2>/tmp/mvs_a64_err.txt; then
        echo "FAIL  $name (link)"; cat /tmp/mvs_a64_err.txt; fail=$((fail+1)); continue
    fi
    "$QEMU" /tmp/mvs_a64 > /tmp/mvs_a64_out.txt 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL  $name (exit $rc)"; cat /tmp/mvs_a64_out.txt; fail=$((fail+1)); continue
    fi
    tr -d '\r' < "tests/expected/$name.txt" > /tmp/mvs_a64_want.txt
    tr -d '\r' < /tmp/mvs_a64_out.txt > /tmp/mvs_a64_got.txt
    if ! diff -u /tmp/mvs_a64_want.txt /tmp/mvs_a64_got.txt > /tmp/mvs_a64_diff.txt; then
        echo "FAIL  $name (output mismatch)"; cat /tmp/mvs_a64_diff.txt; fail=$((fail+1)); continue
    fi
    rm -f "$t.o"
    pass=$((pass+1)); echo "ok    $name"
done

echo
if [ "$fail" -gt 0 ]; then echo "FAILED: $fail failure(s), $pass passed"; exit 1; fi
echo "ALL PASS: $pass test(s)"
