# Upload HScreenFilter assets and create a GitHub Release
# Usage: .\upload.ps1
# Uses your existing git credential (GCM) to authenticate with GitHub - no token hardcoded.
# Requires the repository 'origin' remote to be set, and assets built by package.ps1.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = 'Htonng/HScreenFilter'
$ver = '1.2.6'
$releaseTag = "v$ver"

# --- Get token from git credential manager (never printed) ---
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill
$user = (($cred -split "`n" | Where-Object { $_ -match '^username=' }) -split '=')[1]
$token = (($cred -split "`n" | Where-Object { $_ -match '^password=' }) -split '=')[1]
if (-not $token) { throw 'Could not obtain GitHub token from git credential manager.' }
Write-Host "Authenticated as: $user"

$authHeaders = @{ 'User-Agent' = 'HScreenFilter'; 'Authorization' = "Bearer $token" }

# --- Create (or reuse) release ---
$releaseBody = @{
    tag_name = $releaseTag
    name     = "HScreenFilter $releaseTag"
    body     = "## HScreenFilter $releaseTag`n`nWinUI 3 screen-filter tool.`n`n### ✨ 新功能`n- 多显示器独立滤镜：每台显示器独立开关 + 独立激活配置（n 选 1 随显示器）+ 独立临时设置`n- 每配置记忆 DXGI 引擎开关（切换配置时自动恢复）`n- 显示器列表下拉框（主显示器置前），逐显示器管理滤镜`n- 打开全局总开关时若所有显示器均未启用则默认全部启用`n`n### Download`n- **Installer**: HScreenFilter-$releaseTag-setup.exe`n- **Portable**: HScreenFilter-$releaseTag-portable.zip"
    draft    = $false
    prerelease = $false
} | ConvertTo-Json

$release = $null
try {
    # Try existing first
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/$releaseTag" -Headers $authHeaders -UseBasicParsing
    Write-Host "Release exists: id=$($release.id)"
} catch {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases" -Method Post -Headers $authHeaders -ContentType 'application/json' -Body $releaseBody -UseBasicParsing
    Write-Host "Release created: id=$($release.id) tag=$($release.tag_name)"
}
$releaseId = $release.id

# --- Upload assets ---
$assets = @(
    @{ File = Join-Path $root "HScreenFilter-$releaseTag-setup.exe"; Name = "HScreenFilter-$releaseTag-setup.exe"; Type = 'application/octet-stream' },
    @{ File = Join-Path $root "HScreenFilter-$releaseTag-portable.zip"; Name = "HScreenFilter-$releaseTag-portable.zip"; Type = 'application/zip' }
)

foreach ($a in $assets) {
    if (-not (Test-Path $a.File)) { Write-Host "Skip missing: $($a.File)"; continue }
    $upload = Invoke-RestMethod -Uri "https://uploads.github.com/repos/$repo/releases/$releaseId/assets?name=$($a.Name)" `
        -Method Post -Headers @{ 'User-Agent'='HScreenFilter'; 'Authorization'="Bearer $token"; 'Content-Type'=$a.Type } `
        -InFile $a.File -UseBasicParsing
    Write-Host "Uploaded: $($upload.name) ($([math]::Round($upload.size/1MB,2)) MB)"
}

Write-Host "`nRelease ready: https://github.com/$repo/releases/tag/$releaseTag" -ForegroundColor Green
