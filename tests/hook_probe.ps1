# Isolates SetWindowsHookEx DLL loading into explorer for a given DLL path.
param([Parameter(Mandatory)][string]$Dll, [int]$Flags = 1)  # 1 = DONT_RESOLVE_DLL_REFERENCES, 0 = normal
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class H2 {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryEx(string p, IntPtr f, uint flags);
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SetWindowsHookEx(int id, IntPtr proc, IntPtr mod, uint tid);
  [DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string w);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$m = [H2]::LoadLibraryEx($Dll, [IntPtr]::Zero, [uint32]$Flags); $le = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
"LoadLibraryEx -> 0x{0:X} (err {1})" -f $m.ToInt64(), $le
$proc = [H2]::GetProcAddress($m, 'ClearBarHookProc'); "proc 0x{0:X}" -f $proc.ToInt64()
$tray = [H2]::FindWindow('Shell_TrayWnd', $null); $pid2 = [uint32]0; $tid = [H2]::GetWindowThreadProcessId($tray, [ref]$pid2)
$h = [H2]::SetWindowsHookEx(3, $proc, $m, $tid); $le = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
"SetWindowsHookEx(tid $tid) -> 0x{0:X} (err {1})" -f $h.ToInt64(), $le
[void][H2]::PostMessage($tray, 0, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep 2
"explorer has: " + ((Get-Process -Id $pid2).Modules | Where-Object { $_.FileName -like '*ClearBarTap*' } | ForEach-Object FileName)
[void][H2]::UnhookWindowsHookEx($h)
