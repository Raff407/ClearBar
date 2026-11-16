# Test helper windows.
#  -Mode backdrop : solid cyan tool windows exactly under every taskbar (not topmost,
#                   never activated, not counted as "user windows").
#  -Mode maxwin   : a normal app window, maximised on the primary monitor.
#  -Mode plainwin : a normal app window, restored, on the primary monitor.
#  -Mode stripes  : like backdrop, but 8 px black/white vertical stripes (blur test pattern).
param([ValidateSet('backdrop','stripes','maxwin','plainwin')][string]$Mode = 'backdrop')
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Windows.Forms; using System.Runtime.InteropServices;
public class ToolForm : Form {
  protected override CreateParams CreateParams { get { var c = base.CreateParams; c.ExStyle |= 0x80 | 0x08000000; return c; } }
  protected override bool ShowWithoutActivation { get { return true; } }
}
public static class H {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
[H]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
[System.Windows.Forms.Application]::EnableVisualStyles()
$forms = @()
if ($Mode -eq 'backdrop' -or $Mode -eq 'stripes') {
  $bars = @([H]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, 'Shell_TrayWnd', $null))
  $h = [IntPtr]::Zero
  while (($h = [H]::FindWindowEx([IntPtr]::Zero, $h, 'Shell_SecondaryTrayWnd', $null)) -ne [IntPtr]::Zero) { $bars += $h }
  foreach ($b in $bars) {
    $r = New-Object H+RECT; [H]::GetWindowRect($b, [ref]$r) | Out-Null
    $f = New-Object ToolForm
    $f.FormBorderStyle = 'None'; $f.ShowInTaskbar = $false; $f.BackColor = [System.Drawing.Color]::FromArgb(0,255,255)
    $f.StartPosition = 'Manual'; $f.Text = 'ClearBarTestBackdrop'
    if ($Mode -eq 'stripes') {
      $bmp = New-Object System.Drawing.Bitmap 16, 4
      for ($x = 0; $x -lt 16; $x++) { for ($y = 0; $y -lt 4; $y++) {
        $bmp.SetPixel($x, $y, $(if ($x -lt 8) { [System.Drawing.Color]::Black } else { [System.Drawing.Color]::White })) } }
      $f.BackgroundImage = $bmp; $f.BackgroundImageLayout = 'Tile'
    }
    $f.Show()
    # place exactly under the taskbar, at the bottom of the z-order
    [H]::SetWindowPos($f.Handle, [IntPtr]1, $r.L, $r.T, $r.R - $r.L, $r.B - $r.T, 0x10) | Out-Null
    $forms += $f
  }
} else {
  $f = New-Object System.Windows.Forms.Form
  $f.Text = 'ClearBarTestWindow'; $f.StartPosition = 'Manual'; $f.Location = New-Object System.Drawing.Point(200,200)
  $f.Size = New-Object System.Drawing.Size(800,500); $f.BackColor = [System.Drawing.Color]::White
  if ($Mode -eq 'maxwin') { $f.WindowState = 'Maximized' }
  $f.Show(); $f.Activate()
  $forms += $f
}
[System.Windows.Forms.Application]::Run($forms[0])
