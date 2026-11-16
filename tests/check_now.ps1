# One-shot: is every taskbar currently see-through? (cyan backdrop under each bar)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class C {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern long GetWindowLongPtr(IntPtr h, int i);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Bars() { var l = new List<IntPtr>(); var p = FindWindowEx(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null); if (p != IntPtr.Zero) l.Add(p);
    IntPtr h = IntPtr.Zero; while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) l.Add(h); return l; }
  public static double Cyan(IntPtr bar) { RECT rc; GetWindowRect(bar, out rc); int x0 = rc.L + 12, x1 = rc.L + (int)((rc.R-rc.L)*0.18);
    using (var b = new Bitmap(x1-x0, 1)) using (var g = Graphics.FromImage(b)) { g.CopyFromScreen(x0, (rc.T+rc.B)/2, 0, 0, b.Size); int hit=0;
      for (int x=0;x<b.Width;x++){ var c=b.GetPixel(x,0); if (c.R<15 && c.G>240 && c.B>240) hit++; } return (double)hit/b.Width; } }
  public static bool Topmost(IntPtr h) { return (GetWindowLongPtr(h, -20) & 0x8) != 0; }
}
"@
[C]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$h = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',(Join-Path $PSScriptRoot 'helper.ps1'),'-Mode','backdrop' -PassThru
Start-Sleep -Milliseconds 2000
foreach ($b in [C]::Bars()) { "bar 0x{0:X} topmost={1} cyan={2:N2}" -f $b.ToInt64(), [C]::Topmost($b), [C]::Cyan($b) }
Stop-Process -Id $h.Id -Force
