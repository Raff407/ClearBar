# ClearBar installer - per-user, no admin rights needed.
#
#   Files   -> %LOCALAPPDATA%\Programs\ClearBar\{ClearBar.exe, ClearBarTap.dll, config.ini}
#   Startup -> Task Scheduler task "ClearBar" (runs at log on, current user only)
#
# Normally started by double-clicking Install.cmd. Re-running it upgrades in place
# and keeps your config.ini.
#
# If explorer already has a different ClearBarTap.dll loaded (an older version),
# explorer is restarted once - the taskbar disappears for a few seconds - because
# only one copy can run inside explorer at a time.
#
# Why these choices:
#  * %LOCALAPPDATA%\Programs\ - on some Windows 11 builds explorer will not load
#    hook DLLs from folders sitting directly under %LOCALAPPDATA% or %APPDATA%.
#  * Task Scheduler instead of the HKCU Run key - Windows' startup launcher can
#    silently skip new Run entries; a logon task is reliable.
param([string]$From)
$ErrorActionPreference = 'Stop'

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }
# SHA-256 via .NET (no module dependency; Get-FileHash fails if PSModulePath is polluted).
function Hash($path) {
  $sha = [Security.Cryptography.SHA256]::Create(); $fs = [IO.File]::OpenRead($path)
  try { [BitConverter]::ToString($sha.ComputeHash($fs)) } finally { $fs.Dispose(); $sha.Dispose() }
}

# Release zip: binaries sit next to this script. Source checkout: they are in .\out.
if (-not $From) {
  $From = if (Test-Path (Join-Path $PSScriptRoot 'ClearBar.exe')) { $PSScriptRoot } else { Join-Path $PSScriptRoot 'out' }
}
$newExe = Join-Path $From 'ClearBar.exe'
$newDll = Join-Path $From 'ClearBarTap.dll'
foreach ($f in $newExe, $newDll) {
  if (-not (Test-Path $f)) { throw "Missing $f. Extract the whole zip first (or run build.cmd if you built from source)." }
}

# Windows 11 only (the XAML taskbar arrived with build 22000).
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild
if ($build -lt 22000) { throw "ClearBar needs Windows 11 (found build $build)." }
# x64 only: an x64 helper DLL cannot load into ARM64 Explorer.
$arch = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') { throw "ClearBar supports x64 Windows 11 only (this PC is $arch)." }

$dest = Join-Path $env:LOCALAPPDATA 'Programs\ClearBar'
$exe = Join-Path $dest 'ClearBar.exe'
$dll = Join-Path $dest 'ClearBarTap.dll'
$cfg = Join-Path $dest 'config.ini'
$session = (Get-Process -Id $PID).SessionId
function My-Explorer { Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session }

Say ''
Say 'Installing ClearBar...' Cyan

# Another taskbar tool fighting over the same XAML elements would undo our look.
if (Get-Process TranslucentTB -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session) {
  Say '  TranslucentTB is running. Both tools control the same taskbar, so close TranslucentTB' Yellow
  Say '  (tray icon > Exit) and turn off its startup entry (Task Manager > Startup apps).' Yellow
}

# Files downloaded from the internet carry a "mark of the web"; clear it on ours.
Get-ChildItem $From -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force $dest | Out-Null
$newHash = Hash $newDll

# Stop a running instance (the taskbar returns to normal while files are swapped).
if (Test-Path $exe) { & $exe --exit 2>$null; Start-Sleep -Milliseconds 500 }
Get-Process ClearBar -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session | Stop-Process -Force

$ok = $false
try {
  # Any ClearBarTap.dll inside our explorer that isn't byte-identical to the new one?
  $stale = @(My-Explorer | ForEach-Object { $_.Modules } | Where-Object ModuleName -eq 'ClearBarTap.dll' |
             Where-Object { -not (Test-Path $_.FileName) -or $_.FileName -ne $dll -or (Hash $_.FileName) -ne $newHash })
  $needCopy = -not (Test-Path $dll) -or (Hash $dll) -ne $newHash
  $copied = $false
  if ($needCopy -and $stale.Count -eq 0) {
    try { Copy-Item $newDll $dll -Force; $copied = $true } catch { }
  }
  if ($stale.Count -gt 0 -or ($needCopy -and -not $copied)) {
    Say '  Restarting Explorer once to load the new version (taskbar back in a few seconds)...' Yellow
    My-Explorer | Stop-Process -Force
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($needCopy -and -not $copied -and $sw.ElapsedMilliseconds -lt 15000) {
      try { Copy-Item $newDll $dll -Force -ErrorAction Stop; $copied = $true } catch { Start-Sleep -Milliseconds 100 }
    }
    Start-Sleep 2
    if (-not (My-Explorer)) { Start-Process explorer.exe }
  }
  if ((Hash $dll) -ne $newHash) { throw 'Could not replace ClearBarTap.dll; the previous version was left in place.' }

  # The exe only after the DLL is in place, so a failed upgrade keeps a matching old pair.
  Copy-Item $newExe $exe -Force
  # Keep the uninstaller next to the program (used by Settings > Installed apps),
  # so the downloaded zip isn't needed to remove it.
  $src = Join-Path $PSScriptRoot 'uninstall.ps1'
  if (Test-Path $src) { Copy-Item $src (Join-Path $dest 'uninstall.ps1') -Force }
  Get-ChildItem $dest -File | Unblock-File -ErrorAction SilentlyContinue

  # Entry in Settings > Apps > Installed apps (current user only).
  $version = (Get-Item $exe).VersionInfo.ProductVersion
  $size = [int](((Get-ChildItem $dest -File | Measure-Object Length -Sum).Sum) / 1KB)
  $u = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ClearBar'
  New-Item $u -Force | Out-Null
  $props = @{ DisplayName = 'ClearBar'; DisplayVersion = $version; Publisher = 'ClearBar (open source)'
              InstallLocation = $dest; DisplayIcon = $exe; URLInfoAbout = 'https://github.com/Raff407/ClearBar'
              UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$dest\uninstall.ps1`" -Yes" }
  foreach ($k in $props.Keys) { Set-ItemProperty $u $k $props[$k] }
  foreach ($k in 'NoModify', 'NoRepair') { New-ItemProperty $u $k -Value 1 -PropertyType DWord -Force | Out-Null }
  New-ItemProperty $u 'EstimatedSize' -Value $size -PropertyType DWord -Force | Out-Null

  # Config files from v1 used colour accents; back them up and start from the new defaults.
  if ((Test-Path $cfg) -and (Select-String -Path $cfg -Pattern '^\s*colou?r\s*=' -Quiet)) {
    Move-Item $cfg (Join-Path $dest 'config.v1.bak') -Force
  }

  # Start at every log on (current user only, normal rights, no time limit).
  Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'ClearBar' -ErrorAction SilentlyContinue
  $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
  $action = New-ScheduledTaskAction -Execute $exe
  $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
  $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -Priority 5
  $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
  Register-ScheduledTask -TaskName 'ClearBar' -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Description 'ClearBar: transparent / blurred Windows 11 taskbar (local only, no network).' -Force | Out-Null
  $ok = $true
}
finally {
  # Whatever happened above, leave ClearBar running - the same way it starts at log on.
  if (Get-ScheduledTask -TaskName 'ClearBar' -ErrorAction SilentlyContinue) { Start-ScheduledTask -TaskName 'ClearBar' }
  elseif (Test-Path $exe) { Start-Process $exe }
}

if ($ok) {
  $running = $null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not $running -and $sw.ElapsedMilliseconds -lt 5000) {
    Start-Sleep -Milliseconds 250
    $running = Get-Process ClearBar -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session
  }
  Say ''
  if ($running) {
    Say 'Done. ClearBar is running and will start automatically when you sign in.' Green
  } else {
    Say 'Installed, but ClearBar did not start. Smart App Control or antivirus may have blocked it (see the FAQ).' Yellow
  }
  Say "Settings: $cfg  (saving the file applies changes instantly)"
  Say 'To remove it: Settings > Apps > Installed apps > ClearBar, or Uninstall.cmd.'
  if (-not $running) { exit 1 }
}
