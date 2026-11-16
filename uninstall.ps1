# ClearBar uninstaller. Normally started by double-clicking Uninstall.cmd.
# Removes the startup task, stops ClearBar (the taskbar returns to normal at once)
# and deletes %LOCALAPPDATA%\Programs\ClearBar including your config.ini.
#
# Explorer keeps ClearBarTap.dll loaded until it restarts, so that one file can
# only be deleted after an Explorer restart - you are offered one (or pass -Yes).
param([switch]$Yes)
$ErrorActionPreference = 'Continue'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\ClearBar'
$exe = Join-Path $dest 'ClearBar.exe'
$session = (Get-Process -Id $PID).SessionId

Write-Host ''
Write-Host 'Removing ClearBar...' -ForegroundColor Cyan
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'ClearBar' -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'ClearBar' -Confirm:$false -ErrorAction SilentlyContinue
Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ClearBar' -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $exe) { & $exe --exit 2>$null; Start-Sleep -Milliseconds 500 }
Get-Process ClearBar -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session | Stop-Process -Force

function Remove-Files {
  $left = @()
  Get-ChildItem $dest -ErrorAction SilentlyContinue | ForEach-Object {
    $item = $_
    try { Remove-Item $item.FullName -Force -ErrorAction Stop } catch { $left += $item.Name }
  }
  if ((Test-Path $dest) -and -not (Get-ChildItem $dest -Force -ErrorAction SilentlyContinue)) {
    Remove-Item $dest -Force -ErrorAction SilentlyContinue
  }
  return ,$left
}

$left = Remove-Files
if ($left.Count -gt 0) {
  $answer = if ($Yes) { 'y' } else { Read-Host 'Explorer still holds ClearBarTap.dll. Restart Explorer now to finish removing it? [Y/n]' }
  if ($answer -notmatch '^[nN]') {
    Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session | Stop-Process -Force
    Start-Sleep 2
    $left = Remove-Files
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)) { Start-Process explorer.exe }
  }
}

if ($left.Count -gt 0) {
  Write-Host "ClearBar is stopped and won't start again. Leftover file(s): $($left -join ', ') in $dest" -ForegroundColor Yellow
  Write-Host 'They can be deleted after you next sign out, or by running Uninstall.cmd again.'
} else {
  Write-Host 'ClearBar removed.' -ForegroundColor Green
}
