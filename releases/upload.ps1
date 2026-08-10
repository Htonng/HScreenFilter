# Upload HScreenFilter assets and create a GitHub Release
# Usage: .\upload.ps1
# Uses your existing git credential (GCM) to authenticate with GitHub - no token hardcoded.
# Requires the repository 'origin' remote to be set, and assets built by package.ps1.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = 'Htonng/HScreenFilter'
$ver = '1.2.5'
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
    body     = "## HScreenFilter $releaseTag`n`nWinUI 3 screen-filter tool.`n`n### ✨ 新功能`n- 配置与快捷键交互改版：配置应用开关（n 选 1）+ 底部「是否保存」保存条`n- 托盘菜单显示配置列表并在当前生效配置前打勾`n- 配置导入 / 导出、重命名、列表拖拽排序`n- 全局开关快捷键入口移至头部，提示 10 秒自动消失`n- 主题复选框不再随滚轮误切换`n`n### 🔧 修复`n- 修复覆盖层重建后可被捕获状态丢失（WGC 录不到滤镜）`n- 修复可捕获时 DXGI 截屏看不到 UI`n- 修复 HSL 分色系调色失效（恢复不透明渲染）`n`n### Download`n- **Installer**: HScreenFilter-$releaseTag-setup.exe`n- **Portable**: HScreenFilter-$releaseTag-portable.zip"
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
