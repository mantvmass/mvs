# ============================================================
#  MVS installer (Windows)
#  Usage: powershell -ExecutionPolicy Bypass -File scripts/install.ps1
#
#  Builds the compiler (if needed), copies mvs.exe + std/ to
#  %LOCALAPPDATA%\Programs\mvs, adds that folder to the user PATH,
#  and sets MVS_STD so the standard library is found from anywhere.
# ============================================================
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path "$root\mvs.exe")) {
    Write-Host "building mvs.exe ..."
    clang -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc `
        src/main.c src/lexer.c src/ast.c src/parser.c src/module.c src/generic.c `
        src/diag.c src/codegen.c src/arch/common.c src/arch/x86_64/win.c src/arch/x86_64/sysv.c `
        -o mvs.exe
    if ($LASTEXITCODE -ne 0) { Write-Host "error: build failed" -ForegroundColor Red; exit 1 }
}

$dest = Join-Path $env:LOCALAPPDATA "Programs\mvs"
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$root\mvs.exe" $dest -Force
Copy-Item "$root\std" $dest -Recurse -Force

# user PATH (only append once)
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$dest*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$dest", "User")
    Write-Host "added $dest to the user PATH"
}
[Environment]::SetEnvironmentVariable("MVS_STD", (Join-Path $dest "std"), "User")

Write-Host "installed mvs to $dest"
Write-Host "open a NEW terminal, then: mvs yourfile.mvs"
