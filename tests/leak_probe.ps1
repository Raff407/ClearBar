# Is explorer growth during ClearBar kill/restart cycles caused by ClearBar or is it noise?
# Phase A (control): same churn but launching a trivial process instead of ClearBar.
# Phase B: ClearBar kill/restart. Samples explorer handles/private bytes every 20 cycles.
param([string]$Stage = (Join-Path $PSScriptRoot '..\out'), [int]$Cycles = 100)
$Stage = [IO.Path]::GetFullPath($Stage); $exe = Join-Path $Stage 'ClearBar.exe'
$ex = Get-Process explorer | Sort-Object StartTime | Select-Object -Last 1
function S { $ex.Refresh(); '{0,6} handles  {1,8:N2} MB private  {2} threads' -f $ex.HandleCount, ($ex.PrivateMemorySize64/1MB), $ex.Threads.Count }
"control (cmd /c exit churn):"; S
for ($i = 1; $i -le $Cycles; $i++) { Start-Process cmd -ArgumentList '/c','exit' -WindowStyle Hidden; Start-Sleep -Milliseconds 800; if ($i % 20 -eq 0) { "  $i  $(S)" } }
"ClearBar kill/restart:"; S
for ($i = 1; $i -le $Cycles; $i++) {
  Get-Process ClearBar -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 300
  Start-Process $exe; Start-Sleep -Milliseconds 500
  if ($i % 20 -eq 0) { "  $i  $(S)" }
}
Start-Sleep 3; "final: $(S)"
