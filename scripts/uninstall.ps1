# ============================================================
#  MVS uninstaller (Windows)
#  Usage: powershell -ExecutionPolicy Bypass -File scripts/uninstall.ps1
#  Removes %LOCALAPPDATA%\Programs\mvs, the PATH entry, and MVS_STD.
# ============================================================
$ErrorActionPreference = "Stop"
$dest = Join-Path $env:LOCALAPPDATA "Programs\mvs"

if (Test-Path $dest) {
    try {
        Remove-Item $dest -Recurse -Force -ErrorAction Stop
    } catch {
        # A running mvs.exe cannot be deleted. Leave PATH and MVS_STD alone so
        # the install stays consistent, and let the user retry.
        Write-Host "error: could not remove $dest" -ForegroundColor Red
        Write-Host "       close any running mvs.exe and run this script again"
        exit 1
    }
    Write-Host "removed $dest"
} else {
    Write-Host "nothing installed at $dest"
}

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath) {
    $newPath = ($userPath -split ";" | Where-Object { $_ -and $_ -ne $dest }) -join ";"
    if ($newPath -ne $userPath) {
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "removed the PATH entry"
    }
}
[Environment]::SetEnvironmentVariable("MVS_STD", $null, "User")
Write-Host "uninstalled"
