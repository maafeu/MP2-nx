# The PC build's raw mouse path with injected input (SendInput): the game window (a --windowed test run, in play from
# the Quick Save) brought to the front, a click into it (takes the mouse, not a shot), relative moves of known size
# between screenshots, then the focus handed back to the window that had it. About 12 s of the user's mouse.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class M {
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct IN { public uint type; public MI mi; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int size);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public static void Send(int dx, int dy, uint flags) {
    var a = new IN[1]; a[0].type = 0; a[0].mi.dx = dx; a[0].mi.dy = dy; a[0].mi.flags = flags;
    if (SendInput(1, a, Marshal.SizeOf(typeof(IN))) != 1) throw new Exception("SendInput refused, size " + Marshal.SizeOf(typeof(IN)));
  }
}
"@
$exeDir = (Resolve-Path "$PSScriptRoot\..\..\build\mp2-pc").Path
$p = Get-Process -Name 'mp2-nx_pc' | Select-Object -First 1
$h = $p.MainWindowHandle
$before = [M]::GetForegroundWindow()
[M]::keybd_event(0x12, 0, 0, [IntPtr]::Zero); [M]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)  # Alt, so the focus may move
[void][M]::BringWindowToTop($h); $ok = [M]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400
"front: $ok, foreground is game: $([M]::GetForegroundWindow() -eq $h)"
[void][M]::SetCursorPos(680, 400)
[M]::Send(0, 0, 0x0002); Start-Sleep -Milliseconds 60; [M]::Send(0, 0, 0x0004)   # left down, up: takes the mouse
Start-Sleep -Milliseconds 500
New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null; Start-Sleep -Milliseconds 700
foreach ($i in 1..10) { [M]::Send(20, 0, 0x0001); Start-Sleep -Milliseconds 20 }      # 200 counts right
Start-Sleep -Milliseconds 700
New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null; Start-Sleep -Milliseconds 700
foreach ($i in 1..20) { [M]::Send(-20, 0, 0x0001); Start-Sleep -Milliseconds 20 }     # 400 left
Start-Sleep -Milliseconds 700
New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null; Start-Sleep -Milliseconds 700
foreach ($i in 1..4) { [M]::Send(0, -20, 0x0001); Start-Sleep -Milliseconds 20 }      # 80 up
Start-Sleep -Milliseconds 700
New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null; Start-Sleep -Milliseconds 700
[void][M]::SetForegroundWindow($before)
"focus handed back: $([M]::GetForegroundWindow() -eq $before)"
