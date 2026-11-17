# Windows 10 live tests. Run INSIDE a Windows 10 (19041-19045) session, e.g. a
# throwaway VM, never on a PC someone is using: it drives the keyboard, restarts
# Explorer and changes the theme.
#
#   powershell -ExecutionPolicy Bypass -File win10_live.ps1 -Stage <folder with ClearBar.exe>
#
# Checks by reading screen pixels of every taskbar over a cyan backdrop window
# placed under it (clear = cyan shows through). Prints PASS/FAIL lines and a
# SUMMARY line; also a "revert" table: how often and how long Explorer's own
# look came back during shell actions while ClearBar was set to clear.
param(
  [Parameter(Mandatory)][string]$Stage,
  [string[]]$Only
)
$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage)
$exe = Join-Path $Stage 'ClearBar.exe'
$cfg = Join-Path $Stage 'config.ini'
if (-not (Test-Path $exe)) { throw "ClearBar.exe not found in $Stage" }
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild
if ($build -lt 19041 -or $build -gt 19045) { throw "This suite needs Windows 10 2004-22H2, build 19041-19045 (found build $build)." }
# helper.ps1 lives in tests\ (one level up); a flat copy next to this script also works.
$helperPs = @((Join-Path $PSScriptRoot 'helper.ps1'), (Join-Path $PSScriptRoot '..\helper.ps1')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $helperPs) { throw 'helper.ps1 not found (expected in tests\ or next to this script).' }

Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
using System.Collections.Generic; using System.Diagnostics; using System.Threading;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, UIntPtr w, string l, uint f, uint t, out UIntPtr r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Taskbars() {
    var l = new List<IntPtr>(); var p = FindWindowEx(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null);
    if (p != IntPtr.Zero) l.Add(p);
    IntPtr h = IntPtr.Zero; while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) l.Add(h);
    return l;
  }
  // Fraction of pixels near (r,g,b) on the bar's middle row, over 55-80% of its
  // width (Windows 10: Start, search box and pinned icons sit on the left).
  public static double Frac(IntPtr bar, int r, int g, int b, int tol) {
    RECT rc; if (!GetWindowRect(bar, out rc)) return -1;
    int w = rc.R - rc.L, x0 = rc.L + (int)(w * 0.55), x1 = rc.L + (int)(w * 0.80), y = rc.T + (rc.B - rc.T) / 2;
    using (var bmp = new Bitmap(x1 - x0, 1, PixelFormat.Format32bppArgb))
    using (var gr = Graphics.FromImage(bmp)) {
      gr.CopyFromScreen(x0, y, 0, 0, bmp.Size, CopyPixelOperation.SourceCopy);
      int hit = 0;
      for (int x = 0; x < bmp.Width; x++) { var c = bmp.GetPixel(x, 0);
        if (Math.Abs(c.R - r) <= tol && Math.Abs(c.G - g) <= tol && Math.Abs(c.B - b) <= tol) hit++; }
      return (double)hit / bmp.Width;
    }
  }
  // Mean luminance of the same band (for blur / normal checks).
  public static double Lum(IntPtr bar) {
    RECT rc; if (!GetWindowRect(bar, out rc)) return -1;
    int w = rc.R - rc.L, x0 = rc.L + (int)(w * 0.55), x1 = rc.L + (int)(w * 0.80), y = rc.T + (rc.B - rc.T) / 2;
    using (var bmp = new Bitmap(x1 - x0, 1, PixelFormat.Format32bppArgb))
    using (var gr = Graphics.FromImage(bmp)) {
      gr.CopyFromScreen(x0, y, 0, 0, bmp.Size, CopyPixelOperation.SourceCopy);
      double s = 0; for (int x = 0; x < bmp.Width; x++) { var c = bmp.GetPixel(x, 0); s += 0.2126*c.R + 0.7152*c.G + 0.0722*c.B; }
      return s / bmp.Width;
    }
  }
  // Watches every bar for ms milliseconds; returns [samples, badSamples, longestBadMs]
  // where "bad" = any bar's cyan fraction below min.
  public static double[] Watch(int ms, double min) {
    var sw = Stopwatch.StartNew(); int n = 0, bad = 0; long badStart = -1, longest = 0;
    while (sw.ElapsedMilliseconds < ms) {
      bool ok = true;
      foreach (var b in Taskbars()) if (Frac(b, 0, 255, 255, 14) < min) ok = false;
      n++;
      if (!ok) { bad++; if (badStart < 0) badStart = sw.ElapsedMilliseconds; longest = Math.Max(longest, sw.ElapsedMilliseconds - badStart); }
      else badStart = -1;
      Thread.Sleep(8);
    }
    return new double[] { n, bad, longest };
  }
  public static void Combo(params byte[] keys) { foreach (var k in keys) keybd_event(k,0,0,UIntPtr.Zero); Array.Reverse(keys); foreach (var k in keys) keybd_event(k,0,2,UIntPtr.Zero); }
  public static void Broadcast(string what) { UIntPtr r; SendMessageTimeout((IntPtr)0xffff, 0x1A, UIntPtr.Zero, what, 2, 3000, out r); }
}
"@
[W]::SetProcessDPIAware() | Out-Null

$results = New-Object System.Collections.Generic.List[object]
function Record($name, $ok, $detail) {
  $results.Add([pscustomobject]@{ Test = $name; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Detail = $detail })
  Write-Output ("[{0}] {1}  {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}
function Want($n) { -not $Only -or ((($Only -join ',') -split ',') -contains $n) }
function Write-Config([string]$desk, [hashtable]$extra = @{}) {
  $sb = New-Object Text.StringBuilder
  [void]$sb.AppendLine('[general]'); [void]$sb.AppendLine('debug_log = true')
  [void]$sb.AppendLine('[desktop]'); [void]$sb.AppendLine("accent = $desk"); [void]$sb.AppendLine('show_line = false')
  foreach ($sec in $extra.Keys) { [void]$sb.AppendLine("[$sec]"); foreach ($k in $extra[$sec].Keys) { [void]$sb.AppendLine("$k = $($extra[$sec][$k])") } }
  [IO.File]::WriteAllText($cfg, $sb.ToString())
}
function Fracs { ([W]::Taskbars() | ForEach-Object { [math]::Round([W]::Frac($_, 0, 255, 255, 14), 2) }) -join ',' }
function All-Cyan([double]$min = 0.95) { foreach ($b in [W]::Taskbars()) { if ([W]::Frac($b, 0, 255, 255, 14) -lt $min) { return $false } }; $true }
function No-Cyan { foreach ($b in [W]::Taskbars()) { if ([W]::Frac($b, 0, 255, 255, 14) -gt 0.05) { return $false } }; $true }
function Wait-Until([scriptblock]$cond, [int]$ms = 3000) {
  $sw = [Diagnostics.Stopwatch]::StartNew(); do { if (& $cond) { return $sw.ElapsedMilliseconds }; Start-Sleep -Milliseconds 30 } while ($sw.ElapsedMilliseconds -lt $ms); -1
}
function Start-Helper($mode) {
  $p = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',$helperPs,'-Mode',$mode -PassThru
  if ($mode -eq 'backdrop' -or $mode -eq 'stripes') {
    # the backdrop must really be there, or "no cyan" checks would pass for the wrong reason
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ([W]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, [NullString]::Value, 'ClearBarTestBackdrop') -eq [IntPtr]::Zero -and $sw.ElapsedMilliseconds -lt 15000) { Start-Sleep -Milliseconds 100 }
    if ($p.HasExited -or $sw.ElapsedMilliseconds -ge 15000) { throw "helper backdrop window did not appear ($mode)" }
    Start-Sleep -Milliseconds 500
  }
  $p
}
function Stop-ClearBar { Get-Process ClearBar -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 300 }
function Start-ClearBar { Start-Process $exe | Out-Null }
function Explorer-Pid { $b = [W]::Taskbars(); if ($b.Count -eq 0) { return 0 }; $p = [uint32]0; [void][W]::GetWindowThreadProcessId($b[0], [ref]$p); [int]$p }
function Restart-Explorer {
  $old = Explorer-Pid; Stop-Process -Id $old -Force
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (((Explorer-Pid) -eq $old -or (Explorer-Pid) -eq 0) -and $sw.ElapsedMilliseconds -lt 40000) {
    if ($sw.ElapsedMilliseconds -gt 8000 -and -not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe; Start-Sleep 2 }
    Start-Sleep -Milliseconds 200
  }
}
$VK_LWIN = 0x5B; $VK_ESC = 0x1B; $VK_TAB = 0x09; $VK_MENU = 0x12

$helper = $null
$reverts = New-Object System.Collections.Generic.List[object]
# Saved so the finally block can put everything back, whatever happens.
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$savedTheme = Get-ItemProperty $personalize
$cfgBackup = "$cfg.testbak"
if (Test-Path $cfg) { Copy-Item $cfg $cfgBackup -Force }
$wasRunning = [bool](Get-Process ClearBar -ErrorAction SilentlyContinue)
function Revert-Check($name, [scriptblock]$action, [int]$watchMs = 2500, [scriptblock]$undo = $null) {
  [void](Wait-Until { All-Cyan } 3000)
  & $action
  $r = [W]::Watch($watchMs, 0.9)
  if ($undo) { & $undo; $r2 = [W]::Watch(1500, 0.9); $r = @(($r[0] + $r2[0]), ($r[1] + $r2[1]), ([math]::Max($r[2], $r2[2]))) }
  $settled = (Wait-Until { All-Cyan } 2000) -ge 0
  $reverts.Add([pscustomobject]@{ Action = $name; Samples = $r[0]; Reverted = $r[1]; LongestMs = $r[2]; ClearAfter = $settled })
  Record "stays clear: $name" ($settled -and $r[2] -le 400) "reverted samples $($r[1])/$($r[0]), longest $($r[2]) ms, clear afterwards=$settled"
}

try {
  Stop-ClearBar
  $helper = Start-Helper 'backdrop'; Start-Sleep 2
  "build $build, taskbars: $([W]::Taskbars().Count), explorer pid $(Explorer-Pid)"

  Record 'baseline: Windows look hides the backdrop' (No-Cyan) "cyan $(Fracs)"

  Write-Config 'clear'
  $sw = [Diagnostics.Stopwatch]::StartNew(); Start-ClearBar
  $t = Wait-Until { All-Cyan } 5000
  Record 'start with clear: every taskbar see-through' ($t -ge 0) "after $($sw.ElapsedMilliseconds) ms, cyan $(Fracs)"
  Record 'no helper DLL in explorer on Windows 10' (-not ((Get-Process -Id (Explorer-Pid)).Modules | Where-Object ModuleName -eq 'ClearBarTap.dll')) ''

  Write-Config 'normal'; $t = Wait-Until { No-Cyan } 3000
  Record 'normal: Windows look restored' ($t -ge 0) "after ${t} ms, cyan $(Fracs)"
  Write-Config 'clear'; $t = Wait-Until { All-Cyan } 3000
  Record 'clear again (hot reload)' ($t -ge 0) "after ${t} ms"

  if (Want 'blur') {
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
    $helper = Start-Helper 'stripes'
    # control first: with clear, the stripes must show through (also proves the backdrop is up)
    $stripes = { foreach ($b in [W]::Taskbars()) { if ([W]::Frac($b, 255, 255, 255, 30) -lt 0.3 -or [W]::Frac($b, 0, 0, 0, 30) -lt 0.3) { return $false } }; $true }
    $t = Wait-Until $stripes 8000
    Record 'clear: stripes visible through the taskbar (control)' ($t -ge 0) "after ${t} ms"
    Write-Config 'blur'; Start-Sleep -Milliseconds 800
    # stripes are black/white: clear shows both extremes; blur averages towards grey
    $ok = $true; $det = @()
    foreach ($b in [W]::Taskbars()) {
      $white = [W]::Frac($b, 255, 255, 255, 30); $black = [W]::Frac($b, 0, 0, 0, 30); $lum = [W]::Lum($b)
      $det += "white=$([math]::Round($white,2)) black=$([math]::Round($black,2)) lum=$([math]::Round($lum))"
      # blurred black/white stripes average to a mid grey (Windows' own look would be dark)
      if ($white -gt 0.1 -or $black -gt 0.1 -or $lum -lt 60 -or $lum -gt 200) { $ok = $false }
    }
    Record 'blur: stripes behind the taskbar are blurred' $ok ($det -join ' | ')
    Write-Config 'clear'; $t = Wait-Until $stripes 3000
    Record 'clear again: stripes visible' ($t -ge 0) "after ${t} ms"
    Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
    $helper = Start-Helper 'backdrop'; Start-Sleep 2
    [void](Wait-Until { All-Cyan } 3000)
  }

  if (Want 'reverts') {
    Revert-Check 'Start menu open/close' { [W]::Combo($VK_LWIN) } 2000 { [W]::Combo($VK_ESC) }
    Revert-Check 'Search open/close' { [W]::Combo($VK_LWIN, 0x53) } 2500 { [W]::Combo($VK_ESC) }
    Revert-Check 'Action Center open/close' { [W]::Combo($VK_LWIN, 0x41) } 2000 { [W]::Combo($VK_ESC) }
    Revert-Check 'Alt+Tab held' { [W]::keybd_event($VK_MENU,0,0,[UIntPtr]::Zero); [W]::keybd_event($VK_TAB,0,0,[UIntPtr]::Zero); [W]::keybd_event($VK_TAB,0,2,[UIntPtr]::Zero) } 1200 { [W]::keybd_event($VK_MENU,0,2,[UIntPtr]::Zero) }
    Revert-Check 'theme broadcast (ImmersiveColorSet)' { [W]::Broadcast('ImmersiveColorSet') } 2500
    $p = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    $oldLight = (Get-ItemProperty $p).SystemUsesLightTheme; $oldTr = (Get-ItemProperty $p).EnableTransparency
    Revert-Check 'light/dark taskbar theme switch' { Set-ItemProperty $p SystemUsesLightTheme ([int](-not $oldLight)); [W]::Broadcast('ImmersiveColorSet') } 3000 { Set-ItemProperty $p SystemUsesLightTheme $oldLight; [W]::Broadcast('ImmersiveColorSet') }
    Revert-Check 'transparency effects off/on' { Set-ItemProperty $p EnableTransparency ([int](-not $oldTr)); [W]::Broadcast('ImmersiveColorSet') } 3000 { Set-ItemProperty $p EnableTransparency $oldTr; [W]::Broadcast('ImmersiveColorSet') }
    Revert-Check 'window open, maximise, restore, close' {
      $script:w = Start-Helper 'plainwin'; Start-Sleep 1.5; [W]::Combo($VK_LWIN, 0x26); Start-Sleep 1; [W]::Combo($VK_LWIN, 0x28) } 2000 { Stop-Process -Id $script:w.Id -Force }
    # DWM announces a composition change to every top-level window (as after a display-driver reset).
    Revert-Check 'DWM composition-change broadcast' { [void][W]::PostMessage([IntPtr]0xffff, 0x031E, [IntPtr]1, [IntPtr]0) } 2000
    Revert-Check 'right-click taskbar menu' {
      $b = [W]::Taskbars()[0]; $rc = New-Object W+RECT; [void][W]::GetWindowRect($b, [ref]$rc)
      [void][W]::SetCursorPos($rc.L + [int](($rc.R - $rc.L) * 0.7), $rc.T + 10); [W]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [W]::mouse_event(16,0,0,0,[UIntPtr]::Zero) } 1500 { [W]::Combo($VK_ESC) }
    Write-Output ''
    Write-Output 'REVERTS (Explorer re-applying its own look while ClearBar = clear):'
    $reverts | Format-Table -AutoSize | Out-String -Width 200 | Write-Output
  }

  if (Want 'modes') {
    Write-Config 'clear' @{ task_view_opened = @{ enabled = 'true'; accent = 'normal' }; start_opened = @{ enabled = 'true'; accent = 'normal' };
                            search_opened = @{ enabled = 'true'; accent = 'normal' }; maximized_window = @{ enabled = 'true'; accent = 'normal' } }
    [void](Wait-Until { All-Cyan } 3000)
    [W]::Combo($VK_LWIN, $VK_TAB); $t = Wait-Until { No-Cyan } 3000
    Record 'Task View -> normal' ($t -ge 0) "after ${t} ms, cyan $(Fracs)"
    [W]::Combo($VK_ESC); $t = Wait-Until { All-Cyan } 3000
    Record 'Task View closed -> clear' ($t -ge 0) "after ${t} ms"
    [W]::Combo($VK_LWIN); $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 3000
    Record 'Start open -> normal (start_opened)' ($t -ge 0) "after ${t} ms"
    [W]::Combo($VK_ESC); $t = Wait-Until { All-Cyan } 3000
    Record 'Start closed -> clear' ($t -ge 0) "after ${t} ms"
    [W]::Combo($VK_LWIN, 0x53); $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 3000
    Record 'Search open -> normal (search_opened)' ($t -ge 0) "after ${t} ms"
    [W]::Combo($VK_ESC); [void](Wait-Until { All-Cyan } 3000)
    [W]::keybd_event($VK_MENU,0,0,[UIntPtr]::Zero); [W]::keybd_event($VK_TAB,0,0,[UIntPtr]::Zero); [W]::keybd_event($VK_TAB,0,2,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900; $alt = All-Cyan 0.9; [W]::keybd_event($VK_MENU,0,2,[UIntPtr]::Zero)
    Record 'Alt+Tab is not Task View (stays clear)' $alt "cyan $(Fracs)"
    $w = Start-Helper 'maxwin'; $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 4000
    Record 'maximised window -> normal (maximized_window)' ($t -ge 0) "after ${t} ms"
    Stop-Process -Id $w.Id -Force; $t = Wait-Until { All-Cyan } 3000
    Record 'window closed -> clear' ($t -ge 0) "after ${t} ms"
    # Start alone (Search mode off): Windows 10 keeps the focus in Search's window while Start is open.
    Write-Config 'clear' @{ start_opened = @{ enabled = 'true'; accent = 'normal' }; search_opened = @{ enabled = 'false' } }
    [void](Wait-Until { All-Cyan } 3000)
    [W]::Combo($VK_LWIN); $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 3000
    Start-Sleep -Milliseconds 700; $held = [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05
    Record 'Start alone -> normal and stays normal while open' ($t -ge 0 -and $held) "after ${t} ms, still normal 0.7 s later: $held"
    [W]::Combo($VK_ESC); $t = Wait-Until { All-Cyan } 3000
    Record 'Start closed -> clear (start_opened only)' ($t -ge 0) "after ${t} ms"
    # Search alone (Start mode off): Search applies, Start does not.
    Write-Config 'clear' @{ start_opened = @{ enabled = 'false' }; search_opened = @{ enabled = 'true'; accent = 'normal' } }
    [void](Wait-Until { All-Cyan } 3000)
    [W]::Combo($VK_LWIN, 0x53); $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 3000
    Record 'Search alone -> normal (search_opened only)' ($t -ge 0) "after ${t} ms"
    [W]::Combo($VK_ESC); [void](Wait-Until { All-Cyan } 3000)
    [W]::Combo($VK_LWIN); Start-Sleep -Milliseconds 1200; $startClear = All-Cyan 0.9; [W]::Combo($VK_ESC)
    Record 'Start does not trigger search_opened' $startClear "primary cyan while Start open: $(Fracs)"
    [void](Wait-Until { All-Cyan } 3000)
    if ([W]::Taskbars().Count -gt 1) {
      Write-Config 'clear' @{ maximized_window = @{ enabled = 'true'; accent = 'normal' } }
      [void](Wait-Until { All-Cyan } 3000)
      $w = Start-Helper 'maxwin'; $t = Wait-Until { [W]::Frac([W]::Taskbars()[0], 0, 255, 255, 14) -lt 0.05 } 6000
      $sec = [W]::Frac([W]::Taskbars()[1], 0, 255, 255, 14)
      Record 'maximised on primary: secondary taskbar stays clear' ($t -ge 0 -and $sec -gt 0.95) "secondary cyan $([math]::Round($sec,2))"
      Stop-Process -Id $w.Id -Force
    }
    Write-Config 'clear'; [void](Wait-Until { All-Cyan } 3000)
  }

  if (Want 'lifecycle') {
    & $exe --exit; $t = Wait-Until { No-Cyan } 3000
    Record '--exit restores the Windows look' ($t -ge 0 -and -not (Get-Process ClearBar -ErrorAction SilentlyContinue)) "after ${t} ms"
    Start-ClearBar; $t = Wait-Until { All-Cyan } 5000
    Record 'restart -> clear' ($t -ge 0) "after ${t} ms"
    Stop-ClearBar; Start-Sleep 1
    Write-Output "  info: killed ClearBar -> look still clear: $(All-Cyan 0.9) (expected on Windows 10, see README)"
    # A new ClearBar whose mode is 'normal' must restore a bar left clear by the killed one.
    Write-Config 'normal'; Start-ClearBar; $t = Wait-Until { No-Cyan } 5000
    Record 'restart with normal restores a bar left over by a killed ClearBar' ($t -ge 0) "after ${t} ms"
    Write-Config 'clear'; [void](Wait-Until { All-Cyan } 5000)
    $sw = [Diagnostics.Stopwatch]::StartNew(); Restart-Explorer
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
    $helper = Start-Helper 'backdrop'; Start-Sleep 2
    $t = Wait-Until { All-Cyan } 30000
    Record 'explorer restart -> clear again' ($t -ge 0) "clear $($sw.ElapsedMilliseconds) ms after the restart began"
  }
}
finally {
  foreach ($vk in $VK_MENU, $VK_LWIN, $VK_TAB) { [W]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }  # no stuck keys
  if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
  Get-Process powershell -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like 'ClearBarTest*' } | Stop-Process -Force -ErrorAction SilentlyContinue
  foreach ($n in 'SystemUsesLightTheme', 'EnableTransparency') {
    if ($null -ne $savedTheme.$n) { Set-ItemProperty $personalize $n $savedTheme.$n }
  }
  [W]::Broadcast('ImmersiveColorSet')
  if (Test-Path $cfgBackup) { Move-Item $cfgBackup $cfg -Force } else { Remove-Item $cfg -ErrorAction SilentlyContinue }
  if (-not $wasRunning) { & $exe --exit 2>$null }
}
$fails = @($results | Where-Object Result -eq 'FAIL').Count
Write-Output ''
Write-Output "SUMMARY: $($results.Count - $fails) passed, $fails failed"
if ($fails) { exit 1 }
