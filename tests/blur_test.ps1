# Quiet live tests (no keyboard input): clear / blur / normal / line, blur strength,
# kill-restore, restart-reapply and (optionally) one explorer restart.
# Backdrop = 8 px black/white stripes under every taskbar:
#   clear  -> stripes visible (pixels near pure black / white)
#   blur   -> stripes smeared to mid grey (~128)
#   normal -> Windows' dark taskbar (no stripes, dark)
param([string]$Stage = (Join-Path $PSScriptRoot '..\out'), [switch]$SkipExplorerRestart)
$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage)
$exe = Join-Path $Stage 'ClearBar.exe'; $cfg = Join-Path $Stage 'config.ini'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class B {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Bars() { var l = new List<IntPtr>(); var p = FindWindowEx(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null); if (p != IntPtr.Zero) l.Add(p);
    IntPtr h = IntPtr.Zero; while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) l.Add(h); return l; }
  // [fraction near black, fraction mid grey 70..190, mean luma, top-row vs row-4 diff, fraction near white]
  public static double[] Stats(IntPtr bar, int row) {
    RECT rc; GetWindowRect(bar, out rc); int x0 = rc.L + 40, x1 = rc.L + (int)((rc.R-rc.L)*0.18);
    using (var bmp = new Bitmap(x1-x0, rc.B-rc.T)) using (var g = Graphics.FromImage(bmp)) {
      g.CopyFromScreen(x0, rc.T, 0, 0, bmp.Size); int y = row < 0 ? bmp.Height/2 : row; int blk=0, wht=0, mid=0; double sum=0;
      for (int x=0;x<bmp.Width;x++){ var c=bmp.GetPixel(x,y); double l=(c.R+c.G+c.B)/3.0; sum+=l; if(l<20) blk++; if(l>235) wht++; if(l>=70&&l<=190) mid++; }
      double d=0; int y4=Math.Min(4,bmp.Height-1); for (int x=0;x<bmp.Width;x++){ var a=bmp.GetPixel(x,0); var c=bmp.GetPixel(x,y4); d+=(Math.Abs(a.R-c.R)+Math.Abs(a.G-c.G)+Math.Abs(a.B-c.B))/3.0; }
      return new double[]{ (double)blk/bmp.Width, (double)mid/bmp.Width, sum/bmp.Width, d/bmp.Width, (double)wht/bmp.Width };
    } }
}
"@
[B]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$results = New-Object System.Collections.Generic.List[object]
function Record($n, $ok, $d) { $results.Add([pscustomobject]@{Test=$n;Result=$(if($ok){'PASS'}else{'FAIL'})}); Write-Host ("[{0}] {1}  {2}" -f $(if($ok){'PASS'}else{'FAIL'}), $n, $d) }
function Cfg($accent, $blur = 8, $line = 'false') {
  [IO.File]::WriteAllText($cfg, "[general]`r`ndebug_log=true`r`n[desktop]`r`naccent=$accent`r`nblur=$blur`r`nshow_line=$line`r`n")
}
function Explorer-Pid { $b=[B]::Bars(); if ($b.Count -eq 0) { return 0 }; $p=[uint32]0; [void][B]::GetWindowThreadProcessId($b[0],[ref]$p); [int]$p }
function Wait-Ready([int]$ms = 60000) { $sw=[Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt $ms) { try { $e=[Threading.EventWaitHandle]::OpenExisting("Local\ClearBar.TapReady.v2.$(Explorer-Pid)"); if ($e.WaitOne(200)) { return $sw.ElapsedMilliseconds } } catch { Start-Sleep -Milliseconds 200 } }; -1 }
# Poll until every bar satisfies the predicate on its stats; returns ms or -1.
function Wait-All([scriptblock]$pred, [int]$ms = 4000) {
  $sw=[Diagnostics.Stopwatch]::StartNew()
  do { $ok=$true; foreach ($b in [B]::Bars()) { $s=[B]::Stats($b,-1); if (-not (& $pred $s)) { $ok=$false } }; if ($ok) { return $sw.ElapsedMilliseconds }; Start-Sleep -Milliseconds 50 } while ($sw.ElapsedMilliseconds -lt $ms)
  -1
}
function Show { ([B]::Bars() | ForEach-Object { $s=[B]::Stats($_,-1); "blk={0:N2} wht={3:N2} mid={1:N2} mean={2:N0}" -f $s[0],$s[1],$s[2],$s[4] }) -join ' | ' }
$isClear  = { param($s) $s[0] -ge 0.35 -and $s[4] -ge 0.35 }               # black AND white stripes
$isBlur   = { param($s) $s[1] -ge 0.9 -and $s[4] -le 0.02 }               # all mid grey
$isNormal = { param($s) $s[4] -le 0.02 -and $s[1] -le 0.1 -and $s[2] -lt 70 } # uniform dark
function Start-Helper { Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',(Join-Path $PSScriptRoot 'helper.ps1'),'-Mode','stripes' -PassThru }

$helper = $null
try {
  if (-not (Get-Process ClearBar -ErrorAction SilentlyContinue)) { Start-Process $exe; [void](Wait-Ready) }
  $helper = Start-Helper; Start-Sleep -Milliseconds 1800
  "taskbars: $([B]::Bars().Count)  explorer $(Explorer-Pid)"

  Cfg 'clear';  $t = Wait-All $isClear;  Record 'clear: stripes visible through every bar' ($t -ge 0) "$t ms  $(Show)"
  Cfg 'blur';   $t = Wait-All $isBlur;   Record 'blur 8: stripes smeared to mid grey on every bar' ($t -ge 0) "$t ms  $(Show)"
  Cfg 'normal'; $t = Wait-All $isNormal; Record 'normal: Windows background back' ($t -ge 0) "$t ms  $(Show)"

  # blur strength is monotonic: blur 1 keeps more stripe contrast than blur 20
  Cfg 'blur' 1;  Start-Sleep -Milliseconds 700; $lo = [B]::Stats([B]::Bars()[0], -1)
  Cfg 'blur' 20; Start-Sleep -Milliseconds 700; $hi = [B]::Stats([B]::Bars()[0], -1)
  Record 'blur strength: 1 sharper than 20' ($lo[1] -lt $hi[1]) ("blur1 mid={0:N2} | blur20 mid={1:N2}" -f $lo[1],$hi[1])

  # line on/off while blurred
  Cfg 'blur' 8 'true';  Start-Sleep -Milliseconds 700; $on  = ([B]::Bars() | ForEach-Object { [B]::Stats($_,0)[3] })
  Cfg 'blur' 8 'false'; Start-Sleep -Milliseconds 700; $off = ([B]::Bars() | ForEach-Object { [B]::Stats($_,0)[3] })
  Record 'blur + show_line toggles the top line' (($on | Where-Object { $_ -lt 2 }).Count -eq 0 -and ($off | Where-Object { $_ -gt 1 }).Count -eq 0) ("top-vs-row4 on={0} off={1}" -f (($on | ForEach-Object { '{0:N1}' -f $_ }) -join ','), (($off | ForEach-Object { '{0:N1}' -f $_ }) -join ','))

  # clear <-> blur flips repeatedly (brush rebuild path, no leaks/glitches)
  $bad = 0; for ($i=0; $i -lt 20; $i++) { Cfg $(if ($i % 2) {'clear'} else {'blur'}); if ((Wait-All $(if ($i % 2) {$isClear} else {$isBlur}) 3000) -lt 0) { $bad++ } }
  Record '20 clear<->blur flips all land' ($bad -eq 0) "misses=$bad"

  Cfg 'blur'; [void](Wait-All $isBlur)
  Get-Process ClearBar | Stop-Process -Force; $t = Wait-All $isNormal 3000
  Record 'killed exe -> restored to Windows default' ($t -ge 0) "$t ms"
  $sw=[Diagnostics.Stopwatch]::StartNew(); Start-Process $exe; $t = Wait-All $isBlur 5000
  Record 'restarted exe -> blur back' ($t -ge 0) "$($sw.ElapsedMilliseconds) ms"

  if (-not $SkipExplorerRestart) {
    $old = Explorer-Pid; Stop-Process -Id $old -Force; $sw=[Diagnostics.Stopwatch]::StartNew()
    while (((Explorer-Pid) -eq $old -or (Explorer-Pid) -eq 0) -and $sw.ElapsedMilliseconds -lt 40000) {
      if ($sw.ElapsedMilliseconds -gt 8000 -and -not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe; Start-Sleep 2 }
      Start-Sleep -Milliseconds 100 }
    $r = Wait-Ready 60000
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }; $helper = Start-Helper; Start-Sleep -Milliseconds 1800
    Cfg 'normal'; $n = Wait-All $isNormal 5000; Cfg 'blur'; $b = Wait-All $isBlur 5000
    Record 'explorer restarted -> re-injected, normal->blur control' ($r -ge 0 -and $n -ge 0 -and $b -ge 0) "ready at $($sw.ElapsedMilliseconds) ms (wait $r), normal=$n blur=$b"
  }
} finally {
  if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
}
$f = @($results | Where-Object Result -eq 'FAIL').Count
"SUMMARY: $($results.Count - $f) passed, $f failed"
