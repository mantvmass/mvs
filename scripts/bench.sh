#!/bin/sh
# Time the same program built by MVS and by C, and print the ratio.
#
# The point is not to beat gcc -O2. The point is that a language that calls
# itself low-level should be within a small factor of it, and that the number
# moves in the right direction when the compiler improves.
#
# Usage: sh scripts/bench.sh [elf64]   (from the repo root)
set -u
MVS=${MVS:-./mvs}
CC=${CC:-gcc}
runs=${RUNS:-3}

best() {   # run $@ a few times, print the best wall time in milliseconds
    b=999999999
    i=0
    while [ $i -lt "$runs" ]; do
        s=$(date +%s%N)
        "$@" > /dev/null 2>&1
        e=$(date +%s%N)
        ms=$(( (e - s) / 1000000 ))
        [ "$ms" -lt "$b" ] && b=$ms
        i=$((i+1))
    done
    echo "$b"
}

echo "building"
$MVS bench/loops.mvs --target elf64 > /dev/null || exit 1
$CC bench/loops.o -o /tmp/bench_mvs -no-pie -lm || exit 1
$MVS bench/loops.mvs --target elf64 -O > /dev/null || exit 1
$CC bench/loops.o -o /tmp/bench_mvs_opt -no-pie -lm || exit 1
$CC -O2 bench/loops.c -o /tmp/bench_c || exit 1
$CC -O0 bench/loops.c -o /tmp/bench_c0 || exit 1
rm -f bench/loops.o bench/loops.asm

# the outputs must agree before any timing means anything
/tmp/bench_mvs > /tmp/bench_out_mvs.txt
/tmp/bench_c > /tmp/bench_out_c.txt
if ! diff -q /tmp/bench_out_mvs.txt /tmp/bench_out_c.txt > /dev/null; then
    echo "MISMATCH: mvs and C do not agree, timings would be meaningless"
    diff /tmp/bench_out_mvs.txt /tmp/bench_out_c.txt
    exit 1
fi

m=$(best /tmp/bench_mvs)
mo=$(best /tmp/bench_mvs_opt)
c=$(best /tmp/bench_c)
c0=$(best /tmp/bench_c0)

echo
echo "best of $runs runs, milliseconds"
echo "  mvs            $m"
echo "  mvs -O         $mo"
echo "  gcc -O0        $c0"
echo "  gcc -O2        $c"
echo
awk -v m="$m" -v mo="$mo" -v c="$c" -v c0="$c0" 'BEGIN {
    printf "  mvs    is %.2fx gcc -O2 and %.2fx gcc -O0\n", m/c, m/c0;
    printf "  mvs -O is %.2fx gcc -O2 and %.2fx gcc -O0\n", mo/c, mo/c0;
}'
