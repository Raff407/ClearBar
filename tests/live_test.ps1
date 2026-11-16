# Live end-to-end tests against the real taskbar(s).
# Verifies by reading composited screen pixels with a cyan backdrop window under each taskbar.
param(
  [string]$Stage = (Join-Path $PSScriptRoot '..\out'),
  [switch]$SkipExplorerRestart,
  [switch]$SkipShell,
  [string[]]$Only
)
$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class T {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Taskbars() {
    var l = new List<IntPtr>(); var p = FindWindowEx(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null);
    if (p != IntPtr.Zero) l.Add(p);
    IntPtr h = IntPtr.Zero; while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) l.Add(h);
    return l;
  }
  // Returns [fraction of row pixels near (r,g,b), mean abs diff between row y0 and row y1]
  public static double[] Sample(IntPtr bar, int row, int r, int g, int b, int tol) {
    RECT rc; GetWindowRect(bar, out rc);
    int w = rc.R - rc.L, x0 = rc.L + 12, x1 = rc.L + (int)(w * 0.18);
    using (var bmp = new Bitmap(x1 - x0, rc.B - rc.T, PixelFormat.Format32bppArgb))
    using (var gr = Graphics.FromImage(bmp)) {
      gr.CopyFromScreen(x0, rc.T, 0, 0, bmp.Size, CopyPixelOperation.SourceCopy);
      int y = row < 0 ? bmp.Height / 2 : row; int hit = 0;
      for (int x = 0; x < bmp.Width; x++) { var c = bmp.GetPixel(x, y);
        if (Math.Abs(c.R - r) <= tol && Math.Abs(c.G - g) <= tol && Math.Abs(c.B - b) <= tol) hit++; }
      double diff = 0; int y4 = Math.Min(4, bmp.Height - 1);
      for (int x = 0; x < bmp.Width; x++) { var a = bmp.GetPixel(x, 0); var c = bmp.GetPixel(x, y4);
        diff += (Math.Abs(a.R - c.R) + Math.Abs(a.G - c.G) + Math.Abs(a.B - c.B)) / 3.0; }
      return new double[] { (double)hit / bmp.Width, diff / bmp.Width };
    }
  }
  public static void Combo(byte[] keys) { foreach (var k in keys) keybd_event(k,0,0,UIntPtr.Zero); Array.Reverse(keys); foreach (var k in keys) keybd_event(k,0,2,UIntPtr.Zero); }
}
"@
[T]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$results = New-Object System.Collections.Generic.List[object]
function Record($name, $ok, $detail) {
  $results.Add([pscustomobject]@{ Test = $name; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Detail = $detail })
  Write-Host ("[{0}] {1}  {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}
function Want($name) { -not $Only -or ($Only -contains $name) }

$exe = Join-Path $Stage 'ClearBar.exe'
$cfg = Join-Path $Stage 'config.ini'
function Write-Config([hashtable]$desk, [hashtable]$extra = @{}, [bool]$debug = $true) {
  $sb = New-Object Text.StringBuilder
  [void]$sb.AppendLine('[general]'); [void]$sb.AppendLine("debug_log = $($debug.ToString().ToLower())")
  [void]$sb.AppendLine('[desktop]')
  foreach ($k in $desk.Keys) { [void]$sb.AppendLine("$k = $($desk[$k])") }
  foreach ($sec in $extra.Keys) {
    [void]$sb.AppendLine("[$sec]")
    foreach ($k in $extra[$sec].Keys) { [void]$sb.AppendLine("$k = $($extra[$sec][$k])") }
  }
  # task view stays default (enabled/normal) unless overridden
  [IO.File]::WriteAllText($cfg, $sb.ToString())
}
function Explorer-Pid {
  $bars = [T]::Taskbars(); if ($bars.Count -eq 0) { return 0 }
  $pid2 = [uint32]0; [void][T]::GetWindowThreadProcessId($bars[0], [ref]$pid2); [int]$pid2
}
function Wait-Ready([int]$timeoutMs = 40000) {
  $name = "Local\ClearBar.TapReady.v2.$(Explorer-Pid)"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
    try { $e = [Threading.EventWaitHandle]::OpenExisting($name); if ($e.WaitOne(250)) { $e.Dispose(); return $sw.ElapsedMilliseconds } ; $e.Dispose() } catch { Start-Sleep -Milliseconds 250 }
  }
  return -1
}
# Poll until every taskbar's mid-row matches (or not) the colour; returns ms or -1.
function Wait-Color([int[]]$rgb, [bool]$expect = $true, [int]$timeoutMs = 3000, [double]$min = 0.95) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  do {
    $ok = $true
    foreach ($b in [T]::Taskbars()) {
      $f = [T]::Sample($b, -1, $rgb[0], $rgb[1], $rgb[2], 14)[0]
      if ($expect) { if ($f -lt $min) { $ok = $false } } else { if ($f -gt 0.05) { $ok = $false } }
    }
    if ($ok) { return $sw.ElapsedMilliseconds }
    Start-Sleep -Milliseconds 40
  } while ($sw.ElapsedMilliseconds -lt $timeoutMs)
  return -1
}
function Bar-Fractions([int[]]$rgb, [int]$row = -1) {
  ([T]::Taskbars() | ForEach-Object { [math]::Round([T]::Sample($_, $row, $rgb[0], $rgb[1], $rgb[2], 14)[0], 2) }) -join ','
}
function Line-Diffs { ([T]::Taskbars() | ForEach-Object { [math]::Round([T]::Sample($_, 0, 0, 0, 0, 0)[1], 1) }) -join ',' }
$CYAN = 0,255,255   # backdrop colour; accents are clear / blur / normal only

function Start-ClearBar { Start-Process -FilePath $exe | Out-Null }
function Stop-ClearBar { Get-Process ClearBar -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 300 }
function Start-Helper($mode) { Start-Process -FilePath powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',(Join-Path $PSScriptRoot 'helper.ps1'),'-Mode',$mode -PassThru }

$helper = $null
try {
  Stop-ClearBar
  $helper = Start-Helper 'backdrop'
  Start-Sleep -Milliseconds 1500
  "taskbars: $([T]::Taskbars().Count)   explorer pid: $(Explorer-Pid)"

  # ---------- baseline: nothing running -> Windows default (backdrop hidden)
  if (Want 'baseline') {
    $f = Bar-Fractions $CYAN
    Record 'baseline: default taskbar hides backdrop' (($f -split ',' | ForEach-Object { [double]$_ }) -notcontains 1) "cyan fractions $f"
  }

  # ---------- inject with accent=normal (proves state flips both ways below)
  Write-Config @{ accent = 'normal'; show_line = 'true' }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Start-ClearBar
  $ready = Wait-Ready
  Record 'inject: TAP ready' ($ready -ge 0) "ready after $ready ms"
  $t = Wait-Color $CYAN $false 8000
  Record 'inject with accent=normal keeps Windows look' ($t -ge 0) "t=$($sw.ElapsedMilliseconds)ms cyan=$(Bar-Fractions $CYAN)"

  # ---------- clear -> backdrop visible
  Write-Config @{ accent = 'clear'; show_line = 'false' }
  $t = Wait-Color $CYAN $true 3000
  Record 'clear: backdrop visible through all taskbars' ($t -ge 0) "hot-reload ${t}ms fractions=$(Bar-Fractions $CYAN)"
  $top = Bar-Fractions $CYAN 0
  Record 'clear + show_line=false: top row is backdrop' ((($top -split ',') | Where-Object { [double]$_ -lt 0.95 }).Count -eq 0) "top-row cyan fractions $top"

  # ---------- clear + line
  Write-Config @{ accent = 'clear'; show_line = 'true' }
  Start-Sleep -Milliseconds 600
  $top = Bar-Fractions $CYAN 0; $mid = Bar-Fractions $CYAN
  Record 'clear + show_line=true: line drawn, body clear' ((($top -split ',') | Where-Object { [double]$_ -gt 0.5 }).Count -eq 0 -and (($mid -split ',') | Where-Object { [double]$_ -lt 0.95 }).Count -eq 0) "top=$top mid=$mid"

  # ---------- normal + line toggle
  Write-Config @{ accent = 'normal'; show_line = 'true' }
  Start-Sleep -Milliseconds 600
  $shown = Line-Diffs; $f = Bar-Fractions $CYAN
  Write-Config @{ accent = 'normal'; show_line = 'false' }
  Start-Sleep -Milliseconds 600
  $hidden = Line-Diffs
  $okShown = (($shown -split ',') | Where-Object { [double]$_ -lt 4 }).Count -eq 0
  $okHidden = (($hidden -split ',') | Where-Object { [double]$_ -gt 3 }).Count -eq 0
  Record 'normal: Windows background restored' ((($f -split ',') | Where-Object { [double]$_ -gt 0.05 }).Count -eq 0) "cyan=$f"
  Record 'normal: show_line toggles the top line' ($okShown -and $okHidden) "top-vs-row4 diff shown=$shown hidden=$hidden"

  # ---------- maximised / visible window modes
  if (Want 'windows') {
    # Run on a brand-new virtual desktop so the user's own windows (cloaked
    # there) don't count; then switch back and close it.
    Write-Config @{ accent = 'clear'; show_line = 'false' } @{
      maximized_window = @{ enabled = 'true'; accent = 'normal'; show_line = 'false' }
      visible_window   = @{ enabled = 'true'; accent = 'normal'; show_line = 'false' } }
    Start-Sleep -Milliseconds 800
    $own = Bar-Fractions $CYAN
    [T]::Combo([byte[]](0x5B,0x11,0x44)); Start-Sleep -Milliseconds 1500    # Win+Ctrl+D
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
    $helper = Start-Helper 'backdrop'; Start-Sleep -Milliseconds 1500
    $t = Wait-Color $CYAN $true 3000
    Record 'new virtual desktop: user windows cloaked -> desktop mode' ($t -ge 0) "cyan=$(Bar-Fractions $CYAN) (own desktop cyan=$own)"
    $w = Start-Helper 'maxwin'; Start-Sleep -Milliseconds 2500
    $p = [T]::Sample([T]::Taskbars()[0], -1, 0, 255, 255, 14)[0]
    $s = if ([T]::Taskbars().Count -gt 1) { [T]::Sample([T]::Taskbars()[1], -1, 0, 255, 255, 14)[0] } else { 1 }
    Record 'maximized_window: primary normal, secondary still clear' ($p -lt 0.05 -and $s -gt 0.95) "primary cyan=$([math]::Round($p,2)) secondary cyan=$([math]::Round($s,2))"
    Stop-Process -Id $w.Id -Force; Start-Sleep -Milliseconds 800
    $w = Start-Helper 'plainwin'; Start-Sleep -Milliseconds 2500
    $p = [T]::Sample([T]::Taskbars()[0], -1, 0, 255, 255, 14)[0]
    Record 'visible_window: primary normal with a restored window' ($p -lt 0.05) "primary cyan=$([math]::Round($p,2))"
    $wh = (Get-Process -Id $w.Id).MainWindowHandle
    [T]::Combo([byte[]](0x5B,0x28)); Start-Sleep -Milliseconds 300; [T]::Combo([byte[]](0x5B,0x28))  # Win+Down x2 -> minimise
    $t = Wait-Color $CYAN $true 3000
    Record 'window minimised -> back to desktop mode' ($t -ge 0) "cyan after ${t}ms"
    Stop-Process -Id $w.Id -Force
    [T]::Combo([byte[]](0x5B,0x11,0x73)); Start-Sleep -Milliseconds 1500    # Win+Ctrl+F4 closes the temp desktop
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
    $helper = Start-Helper 'backdrop'; Start-Sleep -Milliseconds 1500
    $back = Bar-Fractions $CYAN
    Record 'back on own desktop: same mode as before' ($back -eq $own) "cyan=$back (expected $own)"
  }

  # ---------- Start / Search / Task View
  if (-not $SkipShell) {
    Write-Config @{ accent = 'clear'; show_line = 'false' } @{
      search_opened = @{ enabled = 'true'; accent = 'normal'; show_line = 'true' }
      start_opened  = @{ enabled = 'true'; accent = 'normal'; show_line = 'true' } }
    Start-Sleep -Milliseconds 800
    [T]::Combo([byte[]](0x5B,0x53)); Start-Sleep -Milliseconds 1500   # Win+S
    $g = [T]::Sample([T]::Taskbars()[0], -1, 0, 255, 255, 14)[0]
    [T]::Combo([byte[]](0x1B)); Start-Sleep -Milliseconds 300
    $t = Wait-Color $CYAN $true 3000
    Record 'search_opened (Win+S) applies, reverts on close' ($g -lt 0.05 -and $t -ge 0) "primary cyan during=$([math]::Round($g,2)) revert=${t}ms"

    [T]::Combo([byte[]](0x5B,0x09)); Start-Sleep -Milliseconds 1500   # Win+Tab
    $f = Bar-Fractions $CYAN
    [T]::Combo([byte[]](0x1B)); Start-Sleep -Milliseconds 300
    $t = Wait-Color $CYAN $true 3000
    Record 'task_view_opened -> normal taskbar, reverts on close' (((($f -split ',') | Where-Object { [double]$_ -gt 0.05 }).Count -eq 0) -and $t -ge 0) "cyan during=$f revert=${t}ms"
  }

  # ---------- process death restores; restart re-applies without re-injection
  Write-Config @{ accent = 'clear'; show_line = 'false' }
  [void](Wait-Color $CYAN $true 3000)
  Get-Process ClearBar | Stop-Process -Force
  $t = Wait-Color $CYAN $false 3000
  Record 'killed exe -> taskbars restored by TAP' ($t -ge 0) "restored after ${t}ms"
  $sw = [Diagnostics.Stopwatch]::StartNew(); Start-ClearBar
  $t = Wait-Color $CYAN $true 5000
  Record 'restarted exe -> re-applied (TAP reused)' ($t -ge 0) "applied after $($sw.ElapsedMilliseconds)ms"
  & $exe --exit; $t = Wait-Color $CYAN $false 3000
  Record 'ClearBar --exit -> clean restore' ($t -ge 0 -and -not (Get-Process ClearBar -ErrorAction SilentlyContinue)) "restored after ${t}ms"
  Start-ClearBar; [void](Wait-Color $CYAN $true 5000)

  # ---------- explorer restart
  if (-not $SkipExplorerRestart) {
    # Restart explorer, ClearBar running. A pass requires: the TAP-ready event of
    # the NEW explorer, then proof of control (magenta) and clear again. Pixel
    # checks alone can be fooled while explorer has not painted its taskbar yet.
    function Restart-Explorer {
      $old = Explorer-Pid
      Stop-Process -Id $old -Force
      $sw = [Diagnostics.Stopwatch]::StartNew()
      while ((Explorer-Pid) -eq $old -or (Explorer-Pid) -eq 0) {
        if ($sw.ElapsedMilliseconds -gt 8000 -and -not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe; Start-Sleep 2 }
        if ($sw.ElapsedMilliseconds -gt 40000) { break }
        Start-Sleep -Milliseconds 100
      }
      return $sw
    }
    function Prove-Control($label, $sw) {
      $ready = Wait-Ready 60000
      if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
      $script:helper = Start-Helper 'backdrop'; Start-Sleep -Milliseconds 1500
      Write-Config @{ accent = 'normal'; show_line = 'false' }
      $m = Wait-Color $CYAN $false 5000
      Write-Config @{ accent = 'clear'; show_line = 'false' }
      $c = Wait-Color $CYAN $true 5000
      Record $label ($ready -ge 0 -and $m -ge 0 -and $c -ge 0) "ready at $($sw.ElapsedMilliseconds)ms (wait $ready), normal=$m clear=$c, explorer $(Explorer-Pid)"
    }
    $sw = Restart-Explorer
    Prove-Control 'explorer restarted -> re-injected, controllable, clear' $sw

    # ClearBar started the moment the new taskbar window exists (XAML not ready yet)
    Stop-ClearBar
    $old = Explorer-Pid; Stop-Process -Id $old -Force
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (((Explorer-Pid) -eq $old -or [T]::Taskbars().Count -lt 1) -and $sw.ElapsedMilliseconds -lt 40000) {
      if ($sw.ElapsedMilliseconds -gt 8000 -and -not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe; Start-Sleep 2 }
      Start-Sleep -Milliseconds 50
    }
    Start-ClearBar
    Prove-Control 'ClearBar started before taskbar XAML exists -> still injects' $sw
  }
}
finally {
  if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
  Get-Process powershell -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like 'ClearBarTest*' } | Stop-Process -Force -ErrorAction SilentlyContinue
}
""
$fails = @($results | Where-Object Result -eq 'FAIL').Count
"SUMMARY: $($results.Count - $fails) passed, $fails failed"
