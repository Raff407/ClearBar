# Builds the release zip from .\out: dist\ClearBar-v<version>-x64.zip (+ .sha256)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$version = (Select-String -Path 'src\common\version.h' -Pattern 'CLEARBAR_VERSION_STR "([^"]+)"').Matches[0].Groups[1].Value
foreach ($f in 'out\ClearBar.exe', 'out\ClearBarTap.dll', 'LICENSE') { if (-not (Test-Path $f)) { throw "missing $f (run build.cmd)" } }

$name = "ClearBar-v$version-x64"
$stage = Join-Path 'dist' $name
Remove-Item 'dist' -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item 'out\ClearBar.exe', 'out\ClearBarTap.dll', 'Install.cmd', 'install.ps1', 'Uninstall.cmd', 'uninstall.ps1', 'LICENSE' $stage
@"
ClearBar $version - transparent / blurred Windows 10/11 taskbar
=============================================================

Install:    extract the zip, double-click Install.cmd   (no admin; starts with Windows)
Requires:   Windows 11, or Windows 10 2004-22H2; x64
Uninstall:  Settings > Apps > Installed apps (Windows 10: Apps & features) > ClearBar, or double-click Uninstall.cmd
Settings:   %LOCALAPPDATA%\Programs\ClearBar\config.ini  (saving applies instantly)

  [desktop]
  accent = clear      ; clear | blur | normal
  blur = 8            ; 1-50, used when accent = blur
  show_line = false   ; the thin line along the top of the taskbar

Docs, source and issues: https://github.com/Raff407/ClearBar
License: GPL-3.0-or-later (see LICENSE)
"@ | Set-Content (Join-Path $stage 'README.txt') -Encoding utf8

$zip = Join-Path 'dist' "$name.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content (Join-Path 'dist' "$name.zip.sha256") -Encoding ascii -NoNewline
Get-Item $zip | Select-Object Name, Length
"SHA256 $hash"
