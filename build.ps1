# Build HScreenFilter (WinUI 3)
# Usage: .\build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Prefer the .NET SDK installed under the user profile (used during development of this repo)
$dotnet = Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet\dotnet.exe'
if (-not (Test-Path $dotnet)) { $dotnet = 'dotnet' }

Push-Location (Join-Path $root 'HScreenFilter')
try {
    & $dotnet build HScreenFilter.csproj -c Release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    $outDir = Join-Path $root 'HScreenFilter\bin\Release\net8.0-windows10.0.26100.0\win-x64'
    Write-Host "`nBuild OK." -ForegroundColor Green
    Write-Host "Output: $outDir" -ForegroundColor Cyan
} finally {
    Pop-Location
}
