# build-webview2-demo.ps1 — 构建 WebView2 Flat Design 原型宿主
# 前置：便携 llvm-mingw 工具链（LLVM_MINGW 或默认路径）+ tools/webview2（SDK 头文件与 WebView2Loader.dll）
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

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
    throw "llvm-mingw toolchain not found. Set env LLVM_MINGW."
}

$clang = "$tc\bin\clang++.exe"
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$flags = @(
    "-std=c++17",
    "-O2",
    "-DUNICODE", "-D_UNICODE",
    "-DWIN32_LEAN_AND_MEAN",
    "-I$root\src",
    "-I$root\tools\webview2\include"
)
$links = @(
    "-mwindows",
    "-lole32", "-luuid", "-luser32", "-lgdi32", "-lshell32", "-ladvapi32", "-lshlwapi",
    "-static", "-static-libgcc"
)
Write-Host "Compiling webview2_demo.cpp ..."
& $clang @flags "$root\src\webview2_demo.cpp" @links -o "$out\webview2_demo.exe" 2>&1
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

# 部署：WebView2Loader.dll + webui
Copy-Item "$root\tools\webview2\x64\WebView2Loader.dll" "$out\WebView2Loader.dll" -Force
$dstWeb = Join-Path $out 'webui'
if (Test-Path $dstWeb) { Remove-Item $dstWeb -Recurse -Force }
Copy-Item "$root\webui" $dstWeb -Recurse -Force
Get-ChildItem "$out\webview2_demo.exe", "$out\WebView2Loader.dll", "$out\webui" | Select-Object FullName

Write-Host ""
Write-Host "Build OK: $out\webview2_demo.exe"
Write-Host "Run:      $out\webview2_demo.exe"
