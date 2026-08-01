# ============================================================
#  MVS installer (Windows)
#  Usage: powershell -ExecutionPolicy Bypass -File scripts/install.ps1
#
#  Downloads the prebuilt release binary (mvs-windows-x86_64.zip from the
#  latest GitHub Release), unpacks it to %LOCALAPPDATA%\Programs\mvs, adds
#  that folder to the user PATH, and sets MVS_STD.
#  (Releases are produced by .github/workflows/release.yml on v* tags.)
# ============================================================
$ErrorActionPreference = "Stop"

$repo = "mantvmass/mvs"
$asset = "mvs-windows-x86_64.zip"
$url = "https://github.com/$repo/releases/latest/download/$asset"
$dest = Join-Path $env:LOCALAPPDATA "Programs\mvs"
$zip = Join-Path $env:TEMP $asset

Write-Host "downloading $url ..."
try {
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
} catch {
    Write-Host "error: download failed. Is there a published release yet?" -ForegroundColor Red
    Write-Host "       (tag a version to create one, or build from source with 'make')"
    exit 1
}

# Replace a previous install wholesale (only after the download succeeded),
# so a file dropped from a newer release cannot linger and shadow the new std.
if (Test-Path $dest) {
    $old = ""
    try { $old = & (Join-Path $dest "mvs.exe") --version } catch {}
    try {
        Remove-Item -Recurse -Force $dest -ErrorAction Stop
    } catch {
        # A running mvs.exe cannot be deleted, but Windows does allow renaming
        # a running executable: move it aside and clear everything else.
        $exe = Join-Path $dest "mvs.exe"
        if (Test-Path $exe) { Move-Item $exe "$exe.old" -Force }
        Get-ChildItem $dest -Exclude "mvs.exe.old" | Remove-Item -Recurse -Force
    }
    if ($old) { Write-Host "replacing $old" }
}

New-Item -ItemType Directory -Force $dest | Out-Null
Expand-Archive -Path $zip -DestinationPath $dest -Force
Remove-Item $zip -Force
# Drop the renamed-aside binary from a previous locked upgrade, if it is free now.
Remove-Item (Join-Path $dest "mvs.exe.old") -Force -ErrorAction SilentlyContinue

# user PATH (only append once)
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$dest*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$dest", "User")
    Write-Host "added $dest to the user PATH"
}
[Environment]::SetEnvironmentVariable("MVS_STD", (Join-Path $dest "std"), "User")

Write-Host "installed mvs to $dest"
Write-Host "open a NEW terminal, then: mvs yourfile.mvs"
