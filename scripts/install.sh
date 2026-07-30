#!/bin/sh
# ============================================================
#  MVS installer (Linux)
#  Usage: sh scripts/install.sh
#         curl -fsSL https://raw.githubusercontent.com/mantvmass/mvs/main/scripts/install.sh | sh
#
#  Downloads the prebuilt release binary (mvs-linux-x86_64.tar.gz from the
#  latest GitHub Release), unpacks it to ~/.mvs, and appends PATH + MVS_STD
#  exports to ~/.profile (idempotent).
#  (Releases are produced by .github/workflows/release.yml on v* tags.)
# ============================================================
set -e

repo="mantvmass/mvs"
asset="mvs-linux-x86_64.tar.gz"
url="https://github.com/$repo/releases/latest/download/$asset"
dest="$HOME/.mvs"
tmp="/tmp/$asset"

echo "downloading $url ..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$tmp" || { echo "error: download failed. Is there a published release yet?"; exit 1; }
else
    wget -q "$url" -O "$tmp" || { echo "error: download failed. Is there a published release yet?"; exit 1; }
fi

mkdir -p "$dest"
tar xzf "$tmp" -C "$dest"
rm -f "$tmp"
chmod +x "$dest/mvs"

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
