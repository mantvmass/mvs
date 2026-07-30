#!/bin/sh
# Linux CI driver: build each example with --target elf64 using a NATIVE Linux build
# of the compiler, link with gcc, run, and diff stdout against the shared goldens
# in tests/expected/ (the same files the win64 suite uses).
# Usage: sh scripts/test_elf_ci.sh   (from the repo root; expects ./mvs built natively)
set -u
MVS=${MVS:-./mvs}
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
examples/01_language/compile_attr
examples/01_language/hexbin
examples/01_language/enums
examples/01_language/bounds
examples/02_functions/generics
examples/02_functions/overload
examples/02_functions/recursion
examples/02_functions/funcptr
examples/02_functions/defaults
examples/03_structs/structs
examples/03_structs/methods
examples/03_structs/pointers
examples/03_structs/compound
examples/03_structs/blob_fields
examples/03_structs/generic_structs
examples/04_traits/traits
examples/04_traits/display
examples/04_traits/dynamic
examples/05_strings/strings
examples/06_modules/use_import
examples/06_modules/shadow_std
examples/08_stdlib/io_demo
examples/08_stdlib/floats
examples/08_stdlib/lib_out
examples/08_stdlib/lib_math
examples/08_stdlib/lib_mem
examples/08_stdlib/lib_rand
examples/08_stdlib/lib_sys
examples/08_stdlib/out_width
examples/08_stdlib/option_result
examples/08_stdlib/lib_vec
examples/08_stdlib/lib_map
examples/08_stdlib/threads
examples/08_stdlib/net_loop"

for t in $tests; do
    name=$(echo "$t" | sed 's|^examples/||; s|/|_|g')
    if ! "$MVS" "$t.mvs" --target elf64 >/dev/null 2>&1; then
        echo "FAIL  $name (mvs compile)"; fail=$((fail+1)); continue
    fi
    if ! gcc "$t.o" -o /tmp/mvs_ci -no-pie -lm -lpthread 2>/tmp/mvs_ci_err.txt; then
        echo "FAIL  $name (link)"; cat /tmp/mvs_ci_err.txt; fail=$((fail+1)); continue
    fi
    /tmp/mvs_ci > /tmp/mvs_ci_out.txt 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL  $name (exit $rc)"; cat /tmp/mvs_ci_out.txt; fail=$((fail+1)); continue
    fi
    # goldens may carry CRLF from Windows checkouts; compare LF to LF
    tr -d '\r' < "tests/expected/$name.txt" > /tmp/mvs_ci_want.txt
    tr -d '\r' < /tmp/mvs_ci_out.txt > /tmp/mvs_ci_got.txt
    if ! diff -u /tmp/mvs_ci_want.txt /tmp/mvs_ci_got.txt > /tmp/mvs_ci_diff.txt; then
        echo "FAIL  $name (output mismatch)"; cat /tmp/mvs_ci_diff.txt; fail=$((fail+1)); continue
    fi
    rm -f "$t.o"
    pass=$((pass+1)); echo "ok    $name"
done
# freestanding (--nostd) ELF objects: must have ZERO undefined symbols and the
# kernel skeleton must link with plain GNU ld (the GRUB multiboot path)
for t in examples/07_c_interop/freestanding examples/09_no_std/kernel examples/09_no_std/bump_alloc examples/09_no_std/use_core examples/09_no_std/intrinsics; do
    name=$(echo "$t" | sed 's|^examples/||; s|/|_|g')
    if ! "$MVS" "$t.mvs" --nostd --target elf64 >/dev/null 2>&1; then
        echo "FAIL  nostd_$name (mvs compile)"; fail=$((fail+1)); continue
    fi
    undef=$(nm -u "$t.o" | wc -l)
    if [ "$undef" -ne 0 ]; then
        echo "FAIL  nostd_$name ($undef undefined symbol(s))"; nm -u "$t.o"; fail=$((fail+1)); continue
    fi
    pass=$((pass+1)); echo "ok    nostd_$name (freestanding, 0 undefined)"
    rm -f "$t.o"
done
if "$MVS" examples/09_no_std/kernel.mvs --nostd --target elf64 >/dev/null 2>&1 &&
   ld examples/09_no_std/kernel.o -o /tmp/mvs_kernel.elf --entry=kmain 2>/dev/null; then
    pass=$((pass+1)); echo "ok    nostd_kernel_ld (links with GNU ld)"
else
    fail=$((fail+1)); echo "FAIL  nostd_kernel_ld"
fi
rm -f examples/09_no_std/kernel.o

echo
if [ "$fail" -gt 0 ]; then echo "FAILED: $fail failure(s), $pass passed"; exit 1; fi
echo "ALL PASS: $pass test(s)"
