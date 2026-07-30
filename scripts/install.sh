#!/bin/sh
# ============================================================
#  MVS installer (Linux)
#  Usage: sh scripts/install.sh
#
#  Builds the compiler (if needed), copies mvs + std/ to ~/.mvs,
#  and appends PATH + MVS_STD exports to ~/.profile (idempotent).
# ============================================================
set -e
cd "$(dirname "$0")/.."

if [ ! -x ./mvs ]; then
    echo "building mvs ..."
    ${CC:-cc} -Wall -Wextra -Isrc \
        src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/generic.c \
        src/diag.c src/codegen.c src/arch/common.c src/arch/x86_64/win.c src/arch/x86_64/sysv.c \
        -o mvs
fi

dest="$HOME/.mvs"
mkdir -p "$dest"
cp ./mvs "$dest/"
rm -rf "$dest/std"
cp -r ./std "$dest/std"

profile="$HOME/.profile"
marker="# mvs-install"
if ! grep -qs "$marker" "$profile"; then
    {
        echo "$marker"
        echo "export PATH=\"\$HOME/.mvs:\$PATH\"  $marker"
        echo "export MVS_STD=\"\$HOME/.mvs/std\"  $marker"
    } >> "$profile"
    echo "added PATH + MVS_STD to $profile"
fi

echo "installed mvs to $dest"
echo "restart your shell (or 'source ~/.profile'), then: mvs yourfile.mvs"
