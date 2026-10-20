#Requires -Version 7

# 构建 TelemetryMon 并打包为自包含发布目录与压缩包（优先 7z 极限压缩，未安装 7z 时回退 zip）。
#
# 用法示例：
#   ./package.ps1                            # 默认：vs2022 预设，构建 + 打包
#   ./package.ps1 -Preset ninja              # 切换构建预设（Ninja 需 VS 开发者环境）
#   ./package.ps1 -SkipBuild                 # 跳过构建，仅用已有产物重新打包
#   ./package.ps1 -NoZip                     # 只产出发布目录，不压缩
#
# 产物结构（TelemetryMon-v<版本>-dist/）：
#   ├── TelemetryMon.exe
#   ├── PresentMonAPI2.dll
#   ├── MaaWin32Screencap.dll
#   ├── LICENSE-PresentMon.txt
#   ├── README.md
#   └── docs/USAGE.md

[CmdletBinding()]
param(
    # 构建预设（CMakePresets.json）：vs2022（默认）/ vs2026 / ninja。
    [ValidateSet('vs2022', 'vs2026', 'ninja')]
    [string]$Preset = 'vs2022',

    # 覆盖从 CMakeLists.txt 解析的版本号。
    [string]$Version,

    # 压缩包输出目录。
    [string]$OutDir = '..\dist',

    # 跳过构建步骤，仅用已有产物重新打包。
    [switch]$SkipBuild,

    # 只产出发布目录，不生成压缩包。
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = $PSScriptRoot
Set-Location $root

# 从 CMakeLists.txt 解析版本号（仅匹配行首 project 声明里的 VERSION）。
$cmakeLists = Get-Content 'CMakeLists.txt' -Raw
if ($cmakeLists -notmatch '(?m)^project\([^)]*?VERSION\s+([0-9][0-9.]*)') {
    throw '无法从 CMakeLists.txt 解析 VERSION'
}
if (-not $Version) { $Version = $Matches[1] }
Write-Host "版本: $Version" -ForegroundColor Cyan

# 构建步骤（预设的二进制目录：build/<预设名>；多配置预设嵌套 Release 目录）。
$buildDir = Join-Path $root "build/$Preset"
if (-not $SkipBuild) {
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-Host "==> cmake --preset $Preset" -ForegroundColor Green
        cmake --preset $Preset
    }
    Write-Host "==> cmake --build --preset $Preset" -ForegroundColor Green
    cmake --build --preset $Preset
}

# 压缩产物目录/文件（SourceSpec 传「目录/*」时压缩包不含目录层级，传目录本身时含）。
function Compress-Artifact {
    param([string]$SourceSpec, [string]$ArchivePath)
    if (Test-Path $ArchivePath) { Remove-Item $ArchivePath -Force }
    if (Get-Command 7z -ErrorAction SilentlyContinue) {
        Write-Host "==> 压缩 (7z -mx=9): $ArchivePath" -ForegroundColor Green
        & 7z a -t7z -mx=9 $ArchivePath $SourceSpec
        if ($LASTEXITCODE -ne 0) {
            throw "7z 压缩失败 (exit $LASTEXITCODE)"
        }
    } else {
        Write-Host '未检测到 7z，回退至 zip 压缩。' -ForegroundColor Yellow
        Write-Host "==> 压缩 (zip): $ArchivePath" -ForegroundColor Green
        Compress-Archive -Path $SourceSpec -DestinationPath $ArchivePath -CompressionLevel Optimal
    }
}

# 定位构建产物（多配置预设嵌套 Release 目录，单配置在预设目录根部）。
$exe = Get-ChildItem $buildDir -Recurse -Filter TelemetryMon.exe |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) {
    throw "未找到构建产物 TelemetryMon.exe（请去掉 -SkipBuild 重新构建）"
}
$payloadDir = $exe.DirectoryName

# 组装发布目录。
$distName = "TelemetryMon-v$Version-dist"
$distRoot = Join-Path $root $distName
Write-Host "==> 组装发布目录: $distName" -ForegroundColor Green
if (Test-Path $distRoot) { Remove-Item $distRoot -Recurse -Force }
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null

Copy-Item (Join-Path $payloadDir 'TelemetryMon.exe') $distRoot
Copy-Item (Join-Path $payloadDir 'PresentMonAPI2.dll') $distRoot
Copy-Item (Join-Path $payloadDir 'MaaWin32Screencap.dll') $distRoot

# 许可与文档随产物分发。
$license = Join-Path $root '..\PresentMon\LICENSE.txt'
if (Test-Path $license) {
    Copy-Item $license (Join-Path $distRoot 'LICENSE-PresentMon.txt')
}
Copy-Item (Join-Path $root 'README.md') $distRoot
Copy-Item (Join-Path $root 'docs') $distRoot -Recurse

# 打印发布目录结构。
Write-Host '发布目录结构:' -ForegroundColor DarkGray
Get-ChildItem $distRoot -Recurse -File | ForEach-Object {
    Write-Host ('  ' + $_.FullName.Substring($distRoot.Length + 1)) -ForegroundColor DarkGray
}

# 压缩发布目录（压缩包内保留目录层级，解压即得完整发布目录）。
if (-not $NoZip) {
    $use7z = [bool](Get-Command 7z -ErrorAction SilentlyContinue)
    $archivePath = Join-Path $OutDir ("TelemetryMon-v{0}-x64.{1}" -f $Version, ($use7z ? '7z' : 'zip'))
    Compress-Artifact -SourceSpec $distRoot -ArchivePath $archivePath
    Write-Host "  压缩包: $archivePath"
}

# 完成。
Write-Host ''
Write-Host '打包完成。' -ForegroundColor Green
Write-Host "  目录: $distRoot"
