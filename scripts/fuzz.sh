#!/bin/sh
# Compiler robustness fuzzer.
#
# The contract being tested: for ANY input, mvs must exit cleanly with 0
# (compiled) or 1 (diagnosed) and must never crash, hang, or corrupt memory.
# A signal (segfault, abort, illegal instruction) or an ASan/UBSan report is a
# bug in the COMPILER, not in the input.
#
# Inputs come from three sources:
#   1. byte mutations of the real examples and tests (bit flips, truncation,
#      deletions, duplications)
#   2. token soup: random sequences of MVS keywords, operators, and literals
#   3. structural stress: deep nesting, long chains, huge literals
#
# Usage: sh scripts/fuzz.sh [iterations]   (default 300; expects ./mvs built)
set -u
MVS=${MVS:-./mvs}
N=${1:-300}
TMP=$(mktemp -d)
CORPUS=$TMP/corpus
mkdir -p "$CORPUS"
fail=0
run=0

# collect the seed corpus
find examples tests core std -name '*.mvs' 2>/dev/null | while read -r f; do
    cp "$f" "$CORPUS/$(echo "$f" | tr '/' '_')" 2>/dev/null || true
done
seeds=$(ls "$CORPUS" | wc -l)
echo "fuzzing with $seeds seed file(s), $N iteration(s)"

# deterministic pseudo-random stream (no external tools, reproducible)
SEED=${FUZZ_SEED:-12345}
rnd() {
    SEED=$(( (SEED * 1103515245 + 12345) % 2147483648 ))
    echo $(( SEED % $1 ))
}

KEYWORDS='let const func return struct if elseif else while do for switch case default break continue import from extern export impl trait dyn where enum match true false as i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize bool void str char f32 f64'
OPERATORS='+ - * / % ** ++ -- = += -= *= /= == != < > <= >= && || ! & | ~ ^ << >> -> => ( ) { } [ ] ; : :: , . ... @ "text" 42 3.14 0xFF 0b1010 name'

check() {
    # $1 = file to compile; any exit code other than 0/1 is a compiler bug
    run=$((run + 1))
    out=$("$MVS" "$1" -S --keep 2>&1)
    rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 1 ]; then
        echo "FUZZ FAIL (exit $rc) on $1"
        echo "$out" | head -5
        cp "$1" "fuzz-crash-$run.mvs" 2>/dev/null || true
        fail=$((fail + 1))
    fi
    case "$out" in
        *AddressSanitizer*|*runtime\ error:*|*LeakSanitizer*)
            echo "SANITIZER REPORT on $1"
            echo "$out" | head -20
            cp "$1" "fuzz-san-$run.mvs" 2>/dev/null || true
            fail=$((fail + 1))
            ;;
    esac
    rm -f "${1%.mvs}.asm" "${1%.mvs}.s" "${1%.mvs}.o" "${1%.mvs}.obj" "${1%.mvs}.exe"
}

i=0
while [ $i -lt "$N" ]; do
    i=$((i + 1))
    f=$TMP/case.mvs
    mode=$(rnd 3)
    if [ "$mode" -eq 0 ] && [ "$seeds" -gt 0 ]; then
        # --- mutate a real file ---
        pick=$(( $(rnd 1000) % seeds + 1 ))
        src=$(ls "$CORPUS" | sed -n "${pick}p")
        src="$CORPUS/$src"
        bytes=$(wc -c < "$src")
        [ "$bytes" -lt 8 ] && continue
        op=$(rnd 4)
        cut=$(( $(rnd 100000) % bytes ))
        case $op in
            0) head -c "$cut" "$src" > "$f" ;;                                  # truncate
            1) { head -c "$cut" "$src"; printf '}'; tail -c +"$cut" "$src"; } > "$f" ;;   # insert a brace
            2) { head -c "$cut" "$src"; tail -c +$((cut + 20)) "$src"; } > "$f" ;;        # delete a run
            3) { cat "$src"; cat "$src"; } > "$f" ;;                            # duplicate everything
        esac
    elif [ "$mode" -eq 1 ]; then
        # --- token soup ---
        : > "$f"
        toks=$(( $(rnd 200) + 5 ))
        j=0
        while [ $j -lt "$toks" ]; do
            j=$((j + 1))
            which=$(rnd 2)
            if [ "$which" -eq 0 ]; then
                n=$(( $(rnd 1000) % 45 + 1 ))
                printf '%s ' "$(echo $KEYWORDS | cut -d' ' -f$n)" >> "$f"
            else
                n=$(( $(rnd 1000) % 44 + 1 ))
                printf '%s ' "$(echo $OPERATORS | cut -d' ' -f$n)" >> "$f"
            fi
        done
    else
        # --- structural stress ---
        kind=$(rnd 4)
        depth=$(( $(rnd 400) + 50 ))
        case $kind in
            0) { printf 'func main() -> i8 { let x: i64 = '; j=0; while [ $j -lt "$depth" ]; do printf '('; j=$((j+1)); done; printf '1'; j=0; while [ $j -lt "$depth" ]; do printf ')'; j=$((j+1)); done; printf '; return 0; }\n'; } > "$f" ;;
            1) { printf 'func main() -> i8 {\n'; j=0; while [ $j -lt "$depth" ]; do printf 'if (1) {\n'; j=$((j+1)); done; j=0; while [ $j -lt "$depth" ]; do printf '}\n'; j=$((j+1)); done; printf 'return 0; }\n'; } > "$f" ;;
            2) { printf 'func main() -> i8 { let x: i64 = 1'; j=0; while [ $j -lt "$depth" ]; do printf ' + 1'; j=$((j+1)); done; printf '; return 0; }\n'; } > "$f" ;;
            3) { printf 'struct S {\n'; j=0; while [ $j -lt "$depth" ]; do printf '  f%d: i64;\n' "$j"; j=$((j+1)); done; printf '}\nfunc main() -> i8 { return 0; }\n'; } > "$f" ;;
        esac
    fi
    check "$f"
done

rm -rf "$TMP"
echo
if [ "$fail" -gt 0 ]; then echo "FUZZ FAILED: $fail problem(s) in $run case(s)"; exit 1; fi
echo "FUZZ OK: $run case(s), no crashes and no sanitizer reports"
