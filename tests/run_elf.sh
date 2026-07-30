#!/bin/sh
# Run inside WSL/Linux from the repo root: link each prebuilt ELF object with gcc
# and run it, printing "== <name> rc=<exit>" then the program output.
# tests/run.ps1 drives this and diffs the output against the same goldens as win64.
cd "$(dirname "$0")/.." || exit 1
for f in "$@"; do
    if gcc "$f.o" -o /tmp/mvs_elf_test -no-pie -lm 2>/tmp/mvs_elf_err; then
        /tmp/mvs_elf_test > /tmp/mvs_elf_out 2>&1
        echo "== $f rc=$?"
        cat /tmp/mvs_elf_out
    else
        echo "== $f rc=LINKFAIL"
        cat /tmp/mvs_elf_err
    fi
done
