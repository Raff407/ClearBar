# Resource + stress measurements for ClearBar (run with ClearBar from .\out).
param([string]$Stage = (Join-Path $PSScriptRoot '..\out'), [int]$Toggles = 300, [int]$IdleSeconds = 60)
$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage)
$exe = Join-Path $Stage 'ClearBar.exe'; $cfg = Join-Path $Stage 'config.ini'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class M { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); }
"@
function Snap($p) {
  $p.Refresh()
  [pscustomobject]@{ WS_MB = [math]::Round($p.WorkingSet64/1MB,2); Private_MB = [math]::Round($p.PrivateMemorySize64/1MB,2)
    Handles = $p.HandleCount; Threads = $p.Threads.Count; CPU_s = [math]::Round($p.TotalProcessorTime.TotalSeconds,3) }
}
function Cfg($body) { [IO.File]::WriteAllText($cfg, $body) }
$clear = "[general]`r`ndebug_log=false`r`n[desktop]`r`naccent=clear`r`nshow_line=false`r`n"
$blurred = "[general]`r`ndebug_log=false`r`n[desktop]`r`naccent=blur`r`nblur=8`r`nshow_line=true`r`n"
$tracking = $clear + "[maximized_window]`r`nenabled=true`r`naccent=blur`r`n[visible_window]`r`nenabled=true`r`n"

Get-Process ClearBar -ErrorAction SilentlyContinue | Stop-Process -Force
Cfg $clear
Start-Process $exe; Start-Sleep 3
$cb = Get-Process ClearBar
$ex = Get-Process explorer | Sort-Object StartTime | Select-Object -Last 1

"== ClearBar after start"; Snap $cb | Format-Table -AutoSize | Out-String
$c0 = (Snap $cb).CPU_s; $e0 = (Snap $ex)
Start-Sleep $IdleSeconds
$c1 = (Snap $cb).CPU_s
"== idle ${IdleSeconds}s, default config: ClearBar CPU {0} s  (= {1:P4} of one core)" -f ($c1-$c0), (($c1-$c0)/$IdleSeconds)

# mouse churn with default config (no window tracking): cursor events must not wake us
$c0 = (Snap $cb).CPU_s
for ($i=0; $i -lt 3000; $i++) { [void][M]::SetCursorPos(400 + ($i % 800), 300 + ($i % 300)); if ($i % 100 -eq 0) { Start-Sleep -Milliseconds 10 } }
$c1 = (Snap $cb).CPU_s
"== 3000 cursor moves, default config: ClearBar CPU {0} s" -f ($c1-$c0)

Cfg $tracking; Start-Sleep 1
$c0 = (Snap $cb).CPU_s
for ($i=0; $i -lt 3000; $i++) { [void][M]::SetCursorPos(400 + ($i % 800), 300 + ($i % 300)); if ($i % 100 -eq 0) { Start-Sleep -Milliseconds 10 } }
$c1 = (Snap $cb).CPU_s
"== 3000 cursor moves, window tracking ON: ClearBar CPU {0} s" -f ($c1-$c0)
$c0 = (Snap $cb).CPU_s; Start-Sleep 20; $c1 = (Snap $cb).CPU_s
"== idle 20s, window tracking ON: ClearBar CPU {0} s" -f ($c1-$c0)
Cfg $clear; Start-Sleep 1

"== explorer before toggles"; $eb = Snap $ex; $eb | Format-Table -AutoSize | Out-String
for ($i=0; $i -lt $Toggles; $i++) { Cfg $(if ($i % 2) { $clear } else { $blurred }); Start-Sleep -Milliseconds 220 }
Cfg $clear; Start-Sleep 2
"== explorer after $Toggles appearance changes"; $ea = Snap $ex; $ea | Format-Table -AutoSize | Out-String
"explorer private delta: {0:N2} MB, handles delta: {1}" -f ($ea.Private_MB - $eb.Private_MB), ($ea.Handles - $eb.Handles)
"== ClearBar after stress"; Snap $cb | Format-Table -AutoSize | Out-String

# kill/restart churn: TAP must restore + re-apply every time without leaking
$eb = Snap $ex
for ($i=0; $i -lt 40; $i++) { Get-Process ClearBar | Stop-Process -Force; Start-Sleep -Milliseconds 300; Start-Process $exe; Start-Sleep -Milliseconds 500 }
Start-Sleep 2
$ea = Snap $ex
"== 40 kill/restart cycles: explorer private delta {0:N2} MB, handles delta {1}, threads {2}->{3}" -f ($ea.Private_MB - $eb.Private_MB), ($ea.Handles - $eb.Handles), $eb.Threads, $ea.Threads
"ClearBar instances running: $(@(Get-Process ClearBar).Count)"
