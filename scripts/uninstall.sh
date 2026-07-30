#!/bin/sh
# ============================================================
#  MVS uninstaller (Linux)
#  Usage: sh scripts/uninstall.sh
#  Removes ~/.mvs and the lines install.sh added to ~/.profile.
# ============================================================
set -e
dest="$HOME/.mvs"

if [ -d "$dest" ]; then
    rm -rf "$dest"
    echo "removed $dest"
else
    echo "nothing installed at $dest"
fi

profile="$HOME/.profile"
if [ -f "$profile" ] && grep -qs "# mvs-install" "$profile"; then
    tmp="$profile.mvs_tmp"
    grep -v "# mvs-install" "$profile" > "$tmp"
    mv "$tmp" "$profile"
    echo "cleaned $profile"
fi
echo "uninstalled"
