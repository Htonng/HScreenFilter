# 运行 HScreenFilter（C++ 版）
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'build\HScreenFilter.exe'
if (-not (Test-Path $exe)) { throw "未找到 $exe，请先运行 .\build.ps1" }
& $exe
