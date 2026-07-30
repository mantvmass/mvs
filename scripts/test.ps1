# ============================================================
#  MVS compiler test suite
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts/test.ps1            # run all tests
#    powershell -ExecutionPolicy Bypass -File scripts/test.ps1 -Update    # regenerate golden files
#
#  Three test groups:
#    1. run-pass:     compile + run an example, diff stdout against tests/expected/<name>.txt
#                     (exit code must be 0 unless tests/expected/<name>.exit says otherwise)
#    2. compile-only: examples that must compile but are not run (network / stdin / .obj output)
#    3. compile-fail: tests/compile_fail/*.mvs must FAIL to compile, and the compiler output
#                     must contain the substring declared on the file's first line:
#                     //~ ERROR: <substring>
# ============================================================

param([switch]$Update)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$mvs = Join-Path $root "mvs.exe"
if (-not (Test-Path $mvs)) {
    Write-Host "error: mvs.exe not found; build it first (make)" -ForegroundColor Red
    exit 1
}

$expectedDir = Join-Path $root "tests/expected"
if (-not (Test-Path $expectedDir)) { New-Item -ItemType Directory $expectedDir | Out-Null }

# examples that compile and run deterministically (no input, no network)
$runPass = @(
    "examples/demo",
    "examples/01_language/hello",
    "examples/01_language/types",
    "examples/01_language/operators",
    "examples/01_language/casts",
    "examples/01_language/control",
    "examples/01_language/bitwise",
    "examples/01_language/args",
    "examples/01_language/arrays",
    "examples/01_language/int128",
    "examples/01_language/shadow",
    "examples/01_language/compile_attr",
    "examples/01_language/hexbin",
    "examples/02_functions/generics",
    "examples/02_functions/overload",
    "examples/02_functions/recursion",
    "examples/02_functions/funcptr",
    "examples/02_functions/defaults",
    "examples/03_structs/structs",
    "examples/03_structs/methods",
    "examples/03_structs/pointers",
    "examples/03_structs/compound",
    "examples/03_structs/blob_fields",
    "examples/03_structs/generic_structs",
    "examples/04_traits/traits",
    "examples/04_traits/display",
    "examples/04_traits/dynamic",
    "examples/05_strings/strings",
    "examples/06_modules/use_import",
    "examples/06_modules/shadow_std",
    "examples/08_stdlib/io_demo",
    "examples/08_stdlib/floats",
    "examples/08_stdlib/lib_out",
    "examples/08_stdlib/lib_math",
    "examples/08_stdlib/lib_mem",
    "examples/08_stdlib/lib_rand",
    "examples/08_stdlib/lib_sys",
    "examples/08_stdlib/out_width",
    "examples/08_stdlib/option_result",
    "examples/08_stdlib/lib_vec",
    "examples/08_stdlib/threads",
    "examples/08_stdlib/net_loop"
)

# fixed command-line arguments for examples that read argv
$runArgs = @{ "examples/01_language/args" = "10 32" }

# examples that must compile but are not run, with the flags they need
$compileOnly = @(
    @{ src = "examples/07_c_interop/extern_c.mvs";     flags = "" },
    @{ src = "examples/07_c_interop/export_lib.mvs";   flags = "-c" },
    @{ src = "examples/07_c_interop/use_c.mvs";        flags = "-c" },
    @{ src = "examples/07_c_interop/freestanding.mvs"; flags = "--nostd" },
    @{ src = "examples/09_no_std/kernel.mvs";          flags = "--nostd" },
    @{ src = "examples/09_no_std/bump_alloc.mvs";      flags = "--nostd" },
    @{ src = "examples/09_no_std/use_core.mvs";        flags = "--nostd" },
    @{ src = "examples/08_stdlib/files.mvs";           flags = "" },
    @{ src = "examples/08_stdlib/net_client.mvs";      flags = "" },
    @{ src = "examples/08_stdlib/net_server.mvs";      flags = "" }
)

$pass = 0; $fail = 0; $updated = 0
$failures = @()

function Normalize([string]$s) {
    if ($null -eq $s) { return "" }
    return ($s -replace "`r`n", "`n").TrimEnd("`n", " ", "`t")
}

# flatten "examples/01_language/types" -> "01_language_types"
function FlatName([string]$ex) {
    return ($ex -replace "^examples/", "" -replace "[/\\]", "_")
}

Write-Host "=== run-pass (golden output) ===" -ForegroundColor Cyan
foreach ($ex in $runPass) {
    $name = FlatName $ex
    $src = "$ex.mvs" -replace "/", "\"
    $exe = "$ex.exe" -replace "/", "\"

    cmd /c "`"$mvs`" `"$src`" >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failures += "compile $ex (exit $LASTEXITCODE)"
        Write-Host "  FAIL  $name (compile error)" -ForegroundColor Red
        continue
    }

    $argstr = ""
    if ($runArgs.ContainsKey($ex)) { $argstr = " " + $runArgs[$ex] }
    $out = cmd /c "`"$exe`"$argstr 2>&1" | Out-String
    $code = $LASTEXITCODE

    $goldFile = Join-Path $expectedDir "$name.txt"
    $exitFile = Join-Path $expectedDir "$name.exit"
    $wantCode = 0
    if (Test-Path $exitFile) { $wantCode = [int](Get-Content $exitFile -Raw).Trim() }

    if ($Update) {
        [IO.File]::WriteAllText($goldFile, (Normalize $out) + "`n")
        if ($code -ne 0) { [IO.File]::WriteAllText($exitFile, "$code`n") }
        $updated++
        Write-Host "  GOLD  $name (exit $code)" -ForegroundColor Yellow
        continue
    }

    if (-not (Test-Path $goldFile)) {
        $fail++; $failures += "missing golden file for $ex (run with -Update)"
        Write-Host "  FAIL  $name (no golden file)" -ForegroundColor Red
        continue
    }
    $want = Normalize ([IO.File]::ReadAllText($goldFile))
    $got = Normalize $out
    if ($got -ne $want -or $code -ne $wantCode) {
        $fail++; $failures += "output mismatch in $ex (exit got=$code want=$wantCode)"
        Write-Host "  FAIL  $name" -ForegroundColor Red
        Write-Host "    --- expected ---"; Write-Host $want
        Write-Host "    --- got (exit $code) ---"; Write-Host $got
    } else {
        $pass++
        Write-Host "  ok    $name"
    }
}

Write-Host "=== compile-only ===" -ForegroundColor Cyan
foreach ($t in $compileOnly) {
    $src = $t.src -replace "/", "\"
    cmd /c "`"$mvs`" `"$src`" $($t.flags) >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failures += "compile-only failed: $($t.src) (exit $LASTEXITCODE)"
        Write-Host "  FAIL  $($t.src)" -ForegroundColor Red
    } else {
        $pass++
        Write-Host "  ok    $($t.src)"
    }
}

# C interop: produce the .obj, link the C side with clang, run, and diff golden output.
# This exercises the f32 single<->double conversions across the C boundary in both directions.
$interopTests = @(
    @{ name = "interop_use_c";
       mvs = "examples\07_c_interop\use_c.mvs";
       link = "examples\07_c_interop\use_c.obj examples\07_c_interop\mathops.c" },
    @{ name = "interop_export_lib";
       mvs = "examples\07_c_interop\export_lib.mvs";
       link = "examples\07_c_interop\caller.c examples\07_c_interop\export_lib.obj" }
)

Write-Host "=== c-interop (link with clang, golden output) ===" -ForegroundColor Cyan
foreach ($t in $interopTests) {
    $exe = "$($t.name).exe"
    cmd /c "`"$mvs`" `"$($t.mvs)`" -c >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failures += "$($t.name): mvs -c failed (exit $LASTEXITCODE)"
        Write-Host "  FAIL  $($t.name) (mvs -c)" -ForegroundColor Red
        continue
    }
    cmd /c "clang $($t.link) -o $exe -llegacy_stdio_definitions >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failures += "$($t.name): clang link failed (exit $LASTEXITCODE)"
        Write-Host "  FAIL  $($t.name) (link)" -ForegroundColor Red
        continue
    }
    $out = cmd /c ".\$exe 2>&1" | Out-String
    $code = $LASTEXITCODE
    Remove-Item $exe -Force -ErrorAction SilentlyContinue

    $goldFile = Join-Path $expectedDir "$($t.name).txt"
    if ($Update) {
        [IO.File]::WriteAllText($goldFile, (Normalize $out) + "`n")
        $updated++
        Write-Host "  GOLD  $($t.name) (exit $code)" -ForegroundColor Yellow
        continue
    }
    if (-not (Test-Path $goldFile)) {
        $fail++; $failures += "missing golden file for $($t.name) (run with -Update)"
        Write-Host "  FAIL  $($t.name) (no golden file)" -ForegroundColor Red
        continue
    }
    $want = Normalize ([IO.File]::ReadAllText($goldFile))
    $got = Normalize $out
    if ($got -ne $want -or $code -ne 0) {
        $fail++; $failures += "output mismatch in $($t.name) (exit $code)"
        Write-Host "  FAIL  $($t.name)" -ForegroundColor Red
        Write-Host "    --- expected ---"; Write-Host $want
        Write-Host "    --- got (exit $code) ---"; Write-Host $got
    } else {
        $pass++
        Write-Host "  ok    $($t.name)"
    }
}

# ELF64 cross-target: compile with --target elf64 (assembles with nasm -f elf64), then
# link with gcc and RUN inside WSL when it is available; output diffs against the SAME
# goldens as win64, proving both backends behave identically.
$wslOk = $false
cmd /c "wsl -e true >nul 2>&1"
if ($LASTEXITCODE -eq 0) { $wslOk = $true }
$elfTests = @(
    "examples/demo",
    "examples/01_language/arrays",
    "examples/01_language/int128",
    "examples/01_language/compile_attr",
    "examples/01_language/hexbin",
    "examples/02_functions/defaults",
    "examples/03_structs/methods",
    "examples/03_structs/blob_fields",
    "examples/03_structs/generic_structs",
    "examples/06_modules/shadow_std",
    "examples/04_traits/dynamic",
    "examples/05_strings/strings",
    "examples/08_stdlib/floats",
    "examples/08_stdlib/lib_out",
    "examples/08_stdlib/lib_math",
    "examples/08_stdlib/lib_mem",
    "examples/08_stdlib/lib_rand",
    "examples/08_stdlib/lib_sys",
    "examples/08_stdlib/out_width",
    "examples/08_stdlib/option_result",
    "examples/08_stdlib/lib_vec",
    "examples/08_stdlib/threads",
    "examples/08_stdlib/net_loop"
)
Write-Host "=== elf64 (SysV backend$(if ($wslOk) { ', run in WSL' } else { ', compile only: WSL not found' })) ===" -ForegroundColor Cyan
foreach ($ex in $elfTests) {
    $name = FlatName $ex
    $src = "$ex.mvs" -replace "/", "\"
    cmd /c "`"$mvs`" `"$src`" --target elf64 >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        $fail++; $failures += "elf64 compile $ex (exit $LASTEXITCODE)"
        Write-Host "  FAIL  elf64 $name (compile)" -ForegroundColor Red
        continue
    }
    if (-not $wslOk) { $pass++; Write-Host "  ok    elf64 $name (assembled)"; continue }
    $out = cmd /c "wsl -e sh scripts/test_elf_wsl.sh $ex 2>&1" | Out-String
    $lines = ($out -replace "`r`n", "`n") -split "`n"
    $rc = "?"
    if ($lines.Count -gt 0 -and $lines[0] -match "rc=(\S+)") { $rc = $Matches[1] }
    $body = ($lines | Select-Object -Skip 1) -join "`n"
    $goldFile = Join-Path $expectedDir "$name.txt"
    $want = if (Test-Path $goldFile) { Normalize ([IO.File]::ReadAllText($goldFile)) } else { "" }
    if ($rc -ne "0" -or (Normalize $body) -ne $want) {
        $fail++; $failures += "elf64 run mismatch in $ex (rc=$rc)"
        Write-Host "  FAIL  elf64 $name (rc=$rc)" -ForegroundColor Red
        Write-Host "    --- expected ---"; Write-Host $want
        Write-Host "    --- got ---"; Write-Host (Normalize $body)
    } else {
        $pass++
        Write-Host "  ok    elf64 $name"
    }
    Remove-Item ("$ex.o" -replace "/", "\") -Force -ErrorAction SilentlyContinue
}

Write-Host "=== compile-fail (expected errors) ===" -ForegroundColor Cyan
$failDir = Join-Path $root "tests/compile_fail"
if (Test-Path $failDir) {
    foreach ($f in (Get-ChildItem $failDir -Filter *.mvs | Sort-Object Name)) {
        $first = (Get-Content $f.FullName -TotalCount 1)
        if ($first -notmatch "^//~ ERROR:\s*(.+)$") {
            $fail++; $failures += "$($f.Name): first line must be '//~ ERROR: <substring>'"
            Write-Host "  FAIL  $($f.Name) (no //~ ERROR header)" -ForegroundColor Red
            continue
        }
        $want = $Matches[1].Trim()
        $out = cmd /c "`"$mvs`" `"$($f.FullName)`" 2>&1" | Out-String
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            $fail++; $failures += "$($f.Name): compiled successfully but should have failed"
            Write-Host "  FAIL  $($f.Name) (compiled, expected error)" -ForegroundColor Red
        } elseif ($out -notmatch [regex]::Escape($want)) {
            $fail++; $failures += "$($f.Name): error output missing '$want'"
            Write-Host "  FAIL  $($f.Name)" -ForegroundColor Red
            Write-Host "    expected substring: $want"
            Write-Host "    got: $($out.Trim())"
        } else {
            $pass++
            Write-Host "  ok    $($f.Name)"
        }
        # remove any artifact a partially-successful compile may have left
        $base = [IO.Path]::ChangeExtension($f.FullName, $null).TrimEnd(".")
        foreach ($e in @(".asm", ".obj", ".exe")) {
            if (Test-Path "$base$e") { Remove-Item "$base$e" -Force }
        }
    }
}

Write-Host ""
if ($Update) {
    Write-Host "updated $updated golden file(s)" -ForegroundColor Yellow
}
if ($fail -gt 0) {
    Write-Host "FAILED: $fail failure(s), $pass passed" -ForegroundColor Red
    foreach ($m in $failures) { Write-Host "  - $m" -ForegroundColor Red }
    exit 1
}
Write-Host "ALL PASS: $pass test(s)" -ForegroundColor Green
exit 0
