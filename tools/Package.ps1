$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$packageRoot = Join-Path $repoRoot 'package'
$artifactRoot = Join-Path $repoRoot 'artifacts'
[void](New-Item -ItemType Directory -Path $packageRoot -Force)
[void](New-Item -ItemType Directory -Path (Join-Path $packageRoot 'licenses') -Force)
[void](New-Item -ItemType Directory -Path $artifactRoot -Force)
# Explicit allowlist: never archive a workspace, an existing package directory,
# logs, PDBs or a user's edited configuration.
$files = [ordered]@{
    'build/Release/CSSXYSpeed.dll' = 'CSSXYSpeed.dll'
    'build/Release/xy_speed_loader.exe' = 'xy_speed_loader.exe'
    'packaging/Start-XYSpeed.ps1' = 'Start-XYSpeed.ps1'
    'packaging/Start-XYSpeed.cmd' = 'Start-XYSpeed.cmd'
    'packaging/xy-speed.ini' = 'xy-speed.ini'
    'packaging/使用说明.txt' = '使用说明.txt'
    'LICENSE' = 'LICENSE.txt'
    'third_party/imgui/LICENSE.txt' = 'licenses/ImGui-LICENSE.txt'
    'third_party/minhook/LICENSE.txt' = 'licenses/MinHook-LICENSE.txt'
}
foreach ($entry in $files.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $entry.Key) -Destination (Join-Path $packageRoot $entry.Value)
}
Copy-Item -LiteralPath (Join-Path $packageRoot 'Start-XYSpeed.cmd') -Destination (Join-Path $packageRoot '启动速度显示.cmd')
$archivePath = Join-Path $artifactRoot 'CSSXYSpeed-v0.1.1-win64.zip'
$temporary = Join-Path $artifactRoot ('package-'+[guid]::NewGuid().ToString('N')+'.zip')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($temporary,[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($name in @($files.Values) + @('启动速度显示.cmd')) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $packageRoot $name),('CSSXYSpeed-v0.1.1/'+$name),[IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }
Move-Item -LiteralPath $temporary -Destination $archivePath -Force
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $artifactRoot 'SHA256SUMS.txt'),($hash+'  CSSXYSpeed-v0.1.1-win64.zip'+"`n"),[Text.UTF8Encoding]::new($false))
Write-Host 'Created artifacts/CSSXYSpeed-v0.1.1-win64.zip and SHA256SUMS.txt'
