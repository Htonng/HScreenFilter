# build.ps1 - Build HScreenFilter (C++ port) with the portable llvm-mingw toolchain.
# Usage: .\build.ps1
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate the toolchain: $env:LLVM_MINGW first, then common portable paths.
$tc = $env:LLVM_MINGW
if (-not $tc -or -not (Test-Path $tc)) {
    $candidates = @(
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

# Resource compilation (icon + version info)
& $windres "$root\src\app.rc" -O coff -o "$out\app_res.o" 2>$null
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

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
foreach ($s in $srcs) {
    $obj = "$out\" + [IO.Path]::GetFileNameWithoutExtension($s) + ".o"
    $output = & $clang @commonFlags -c $s -o $obj 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host $output
        throw "compile failed: $s"
    }
    $objs += $obj
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
$output = & $clang @objs @linkFlags -o $exe 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host $output
    throw "link failed"
}

# Ship the icon next to the exe (window/tray icon)
New-Item -ItemType Directory -Force -Path "$out\assets" | Out-Null
Copy-Item "$root\assets\icon.ico" "$out\assets\icon.ico" -Force

Write-Host ""
Write-Host "Build OK: $exe"
Write-Host "Run:      $exe"
Write-Host "SelfTest: $exe --selftest"
