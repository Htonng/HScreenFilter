# build.ps1 - Build HScreenFilter (C++ port) with the portable llvm-mingw toolchain.
# Usage: .\build.ps1
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate the toolchain: $env:LLVM_MINGW first, then common portable paths.
$tc = $env:LLVM_MINGW
if (-not $tc -or -not (Test-Path $tc)) {
    $candidates = @(
        "$root\tools\llvm-mingw-beta\llvm-mingw-20260616-ucrt-x86_64",
        "F:\tools\llvm-mingw\llvm-mingw-20260616-ucrt-x86_64",
        "$env:LOCALAPPDATA\llvm-mingw\llvm-mingw-20260616-ucrt-x86_64",
        "C:\llvm-mingw"
    )
    foreach ($c in $candidates) { if (Test-Path "$c\bin\clang++.exe") { $tc = $c; break } }
}
if (-not $tc -or -not (Test-Path "$tc\bin\clang++.exe")) {
    throw "llvm-mingw toolchain not found. Set env LLVM_MINGW to the toolchain root."
}

$clang = "$tc\bin\clang++.exe"
$windres = "$tc\bin\windres.exe"
$out = "$root\build"
$exe = "$out\HScreenFilter.exe"

New-Item -ItemType Directory -Force -Path $out | Out-Null

$srcs = @(
    "src\common.cpp",
    "src\log.cpp",
    "src\json.cpp",
    "src\models.cpp",
    "src\store.cpp",
    "src\autostart.cpp",
    "src\monitors.cpp",
    "src\msgwindow.cpp",
    "src\hotkeys.cpp",
    "src\tray.cpp",
    "src\fgwatcher.cpp",
    "src\engines\hlsl.cpp",
    "src\engines\lut_engine.cpp",
    "src\engines\mag_engine.cpp",
    "src\engines\gamma_engine.cpp",
    "src\engines\filter_engine.cpp",
    "src\ui\dialogs.cpp",
    "src\ui\main_window.cpp",
    "src\main.cpp"
) | ForEach-Object { Join-Path $root $_ }

# Resource compilation (icon + version info); rc is UTF-8, set codepage explicitly
# so Chinese version strings are not mangled in the resources.
# Use Start-Process to get the real exit code ($LASTEXITCODE may be unset under -Command).
$resProc = Start-Process -FilePath $windres -ArgumentList @('--codepage=65001', '-I', "$root\src", "$root\src\app.rc", '-O', 'coff', '-o', "$out\app_res.o") -Wait -NoNewWindow -PassThru
if ($resProc.ExitCode -ne 0) { throw "windres failed (exit $($resProc.ExitCode))" }

# 用 Start-Process 调用原生程序并取真实退出码（$LASTEXITCODE 在 -Command 调用下可能不更新）
function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$LogPath
    )
    $errPath = "$LogPath.err"
    $p = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $LogPath -RedirectStandardError $errPath
    if (Test-Path $LogPath) { Get-Content -LiteralPath $LogPath -Raw | Write-Host }
    if (Test-Path $errPath) { Get-Content -LiteralPath $errPath -Raw | Write-Host }
    Remove-Item -LiteralPath $LogPath, $errPath -Force -ErrorAction SilentlyContinue
    return $p.ExitCode
}

# Compile flags (link-only flags like -mwindows are NOT used here)
$commonFlags = @(
    "-std=c++17",
    "-O2",
    "-DWIN32_LEAN_AND_MEAN",
    "-I$root\src",
    "-I$tc\include"
)

Write-Host "Compiling $($srcs.Count) sources..."
$objs = @()
$i = 0
foreach ($s in $srcs) {
    $obj = "$out\" + [IO.Path]::GetFileNameWithoutExtension($s) + ".o"
    $compileArgs = $commonFlags + @('-c', $s, '-o', $obj)
    $compileLog = "$out\compile-$i.log"
    $exit = Invoke-Native $clang $compileArgs $compileLog
    if ($exit -ne 0) { throw "compile failed: $s" }
    $objs += $obj
    $i++
}

Write-Host "Linking..."
$linkFlags = @(
    "$out\app_res.o",
    "-L$tc\x86_64-w64-mingw32\lib",
    "-ld2d1", "-ldwrite", "-ld3d11", "-ldxgi",
    "-lshell32", "-lole32", "-luuid", "-lshlwapi", "-lcomctl32", "-lcomdlg32",
    "-loleaut32", "-luser32", "-lgdi32", "-ladvapi32", "-ldwmapi",
    "-static", "-static-libgcc", "-mwindows"
)
$linkArgs = $objs + $linkFlags + @('-o', $exe)
$linkLog = "$out\link.log"
$exit = Invoke-Native $clang $linkArgs $linkLog
if ($exit -ne 0) { throw "link failed" }

# Ship the icon next to the exe (window/tray icon)
New-Item -ItemType Directory -Force -Path "$out\assets" | Out-Null
Copy-Item "$root\assets\icon.ico" "$out\assets\icon.ico" -Force

Write-Host ""
Write-Host "Build OK: $exe"
Write-Host "Run:      $exe"
Write-Host "SelfTest: $exe --selftest"
