# Run HScreenFilter (build first with .\build.ps1)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$exe = Get-ChildItem -Path (Join-Path $root 'HScreenFilter\bin') -Recurse -Filter 'HScreenFilter.exe' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $exe) {
    Write-Host 'HScreenFilter.exe not found. Run .\build.ps1 first.' -ForegroundColor Yellow
    exit 1
}

Start-Process $exe.FullName
Write-Host "Started: $($exe.FullName)" -ForegroundColor Green
