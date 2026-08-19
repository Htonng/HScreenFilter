# build-release.ps1 — 构建发布版 HScreenFilter（WebView2 桥接版，链接真实数据层 + 滤镜引擎）
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

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
if (-not $tc -or -not (Test-Path "$tc\bin\clang++.exe")) { throw "llvm-mingw toolchain not found. Set env LLVM_MINGW." }

$clang = "$tc\bin\clang++.exe"
$windres = "$tc\bin\windres.exe"
$out = Join-Path $root 'dist\HScreenFilter-v2.0.0-beta'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$srcs = @(
    "src\app_main.cpp",
    "src\common.cpp", "src\log.cpp", "src\json.cpp", "src\models.cpp",
    "src\store.cpp", "src\monitors.cpp", "src\autostart.cpp",
    "src\msgwindow.cpp", "src\hotkeys.cpp", "src\fgwatcher.cpp",
    "src\engines\hlsl.cpp", "src\engines\lut_engine.cpp", "src\engines\mag_engine.cpp",
    "src\engines\gamma_engine.cpp", "src\engines\filter_engine.cpp"
) | ForEach-Object { Join-Path $root $_ }

$flags = @(
    "-std=c++17", "-O2",
    "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
    "-I$root\src",
    "-I$root\tools\webview2\include"
)
$links = @(
    "-mwindows",
    "-ld3d11", "-ldxgi",
    "-lshell32", "-lole32", "-luuid", "-lshlwapi",
    "-luser32", "-lgdi32", "-ladvapi32", "-ldwmapi", "-lcomdlg32",
    "-static", "-static-libgcc"
)

Write-Host "Compiling resource ..."
# rc 为 UTF-8 编码，显式指定代码页，否则中文版本信息在资源里乱码。
# 用 Start-Process 拿退出码（$LASTEXITCODE 在 -Command 调用脚本时可能不被设置）
$resProc = Start-Process -FilePath $windres -ArgumentList @('--codepage=65001', "$root\src\release.rc", '-O', 'coff', '-o', "$root\build\release_res.o") -Wait -NoNewWindow -PassThru
if ($resProc.ExitCode -ne 0) { throw "windres failed (exit $($resProc.ExitCode))" }

Write-Host "Compiling $($srcs.Count) sources ..."
& $clang "$root\build\release_res.o" @flags @srcs @links -o "$out\HScreenFilter.exe" 2>&1
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Copy-Item "$root\tools\webview2\x64\WebView2Loader.dll" "$out\WebView2Loader.dll" -Force
$dstWeb = Join-Path $out 'webui2'
if (Test-Path $dstWeb) { Remove-Item $dstWeb -Recurse -Force }
Copy-Item "$root\webui2" $dstWeb -Recurse -Force

# 清理旧产物（防止旧命名/日志/预览图混入发布包）
Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match 'webview2_demo2|\.log$|preview\.png$' } | Remove-Item -Force -ErrorAction SilentlyContinue

# 打包发布 zip
$zip = Join-Path $root 'dist\HScreenFilter-v2.0.0-beta.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$out\*" -DestinationPath $zip -Force

Write-Host ""
Write-Host "Build OK: $out\HScreenFilter.exe"
Write-Host "Run:      $out\HScreenFilter.exe"
Write-Host "Package:  $zip"
Write-Host "Version:  v2.0.0-beta"
