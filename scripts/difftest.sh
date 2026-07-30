#!/bin/sh
# Differential testing: for every pair tests/diff/<name>.mvs + <name>.c, compile
# and run BOTH, then require byte-identical stdout.
#
# This is the strongest correctness signal we have: the C compiler is the
# reference implementation, so a mismatch means MVS generated wrong code, not
# just that a golden file drifted. Goldens prove "the output did not change";
# this proves "the output is right".
#
# Usage: sh scripts/difftest.sh [elf64|arm64]   (from the repo root)
#   elf64 (default): native gcc for both sides
#   arm64:           cross gcc for both sides, both run under qemu-aarch64
set -u
MVS=${MVS:-./mvs}
MODE=${1:-elf64}
if [ "$MODE" = "arm64" ]; then
    CC=${CROSS:-aarch64-linux-gnu-gcc}
    RUN=${QEMU:-qemu-aarch64}
    TARGET="--target arm64"
    LINK="-static -lm"
else
    CC=${CC:-gcc}
    RUN=""
    TARGET="--target elf64"
    LINK="-no-pie -lm"
fi
fail=0
pass=0

for m in tests/diff/*.mvs; do
    [ -e "$m" ] || continue
    name=$(basename "$m" .mvs)
    ref="tests/diff/$name.c"
    if [ ! -f "$ref" ]; then
        echo "FAIL  $name (no C reference)"; fail=$((fail+1)); continue
    fi

    if ! "$MVS" "$m" $TARGET >/tmp/diff_mvs_build.txt 2>&1; then
        echo "FAIL  $name (mvs compile)"; cat /tmp/diff_mvs_build.txt; fail=$((fail+1)); continue
    fi
    if ! $CC "tests/diff/$name.o" -o /tmp/diff_mvs $LINK 2>/tmp/diff_link.txt; then
        echo "FAIL  $name (link mvs)"; cat /tmp/diff_link.txt; fail=$((fail+1)); continue
    fi
    if ! $CC -O2 -o /tmp/diff_c "$ref" $LINK 2>/tmp/diff_cc.txt; then
        echo "FAIL  $name (cc reference)"; cat /tmp/diff_cc.txt; fail=$((fail+1)); continue
    fi

    $RUN /tmp/diff_mvs > /tmp/diff_out_mvs.txt 2>&1
    rc_m=$?
    $RUN /tmp/diff_c > /tmp/diff_out_c.txt 2>&1
    rc_c=$?
    rm -f "tests/diff/$name.o" "tests/diff/$name.asm" "tests/diff/$name.s"

    if [ $rc_m -ne $rc_c ]; then
        echo "FAIL  $name (exit codes differ: mvs=$rc_m c=$rc_c)"; fail=$((fail+1)); continue
    fi
    if ! diff -u /tmp/diff_out_c.txt /tmp/diff_out_mvs.txt > /tmp/diff_delta.txt; then
        echo "FAIL  $name (output differs from the C reference)"
        head -20 /tmp/diff_delta.txt
        fail=$((fail+1)); continue
    fi
    lines=$(wc -l < /tmp/diff_out_c.txt)
    pass=$((pass+1)); echo "ok    $name ($lines line(s) identical to C)"
done

echo
if [ "$fail" -gt 0 ]; then echo "DIFF FAILED: $fail mismatch(es), $pass matched"; exit 1; fi
echo "DIFF OK: $pass program(s) match the C reference exactly"
