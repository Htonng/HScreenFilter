# Build HScreenFilter v1.0.0 release artifacts (portable zip + installer)
# Usage: .\package.ps1
# Requires: Inno Setup 6 at user Programs dir, 7-Zip installed.

$ErrorActionPreference = 'Stop'
# 脚本位于 releases\package.ps1，项目根是它的上两级（releases 的父目录）
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ver = '1.2.4'
$releaseTag = "v$ver"

# Locate tools
$dotnet = Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet\dotnet.exe'
if (-not (Test-Path $dotnet)) { $dotnet = 'dotnet' }
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'

# 1) Publish Release build
Push-Location (Join-Path $root 'HScreenFilter')
try { & $dotnet build HScreenFilter.csproj -c Release } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'dotnet build failed.' }

$src = Join-Path $root "HScreenFilter\bin\Release\net8.0-windows10.0.26100.0\win-x64"
$releaseDir = Join-Path $root "releases\$releaseTag"

# 2) Prepare portable folder
$portable = Join-Path $releaseDir 'portable'
if (Test-Path $portable) { Remove-Item $portable -Recurse -Force }
New-Item -ItemType Directory -Force -Path $portable | Out-Null
Copy-Item -Path "$src\*" -Destination $portable -Recurse -Force
$verText = "HScreenFilter $releaseTag (Portable)`r`n`r`nPortable screen-filter tool.`r`n- Run HScreenFilter.exe directly, no install needed.`r`n- Config stored in %LocalAppData%\HScreenFilter\profiles.json"
[System.IO.File]::WriteAllText((Join-Path $portable 'VERSION.txt'), $verText, (New-Object System.Text.UTF8Encoding($false)))

# 3) Build portable zip
$zipOut = Join-Path $root "releases\HScreenFilter-$releaseTag-portable.zip"
if (Test-Path $zipOut) { Remove-Item $zipOut -Force }
& $sevenZip a -tzip -mx9 $zipOut (Join-Path $portable '*') | Out-Null
if ($LASTEXITCODE -ne 0) { throw '7z failed.' }

# 4) Build installer with Inno Setup
if (-not (Test-Path $iscc)) { throw "Inno Setup not found at $iscc" }
Push-Location $releaseDir
try { & $iscc (Join-Path $root 'releases\installer.iss') | Out-Null } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed.' }

# Inno OutputDir=.. puts setup at project root; move into releases
$setupName = "HScreenFilter-$releaseTag-setup.exe"
$projectRootSetup = Join-Path $root $setupName
$finalSetup = Join-Path $root "releases\$setupName"
if (Test-Path $projectRootSetup) {
    Move-Item $projectRootSetup $finalSetup -Force
}

Write-Host "`nPackage done (v$ver):" -ForegroundColor Green
Write-Host "  Portable zip: $zipOut"
Write-Host "  Installer   : $finalSetup"
