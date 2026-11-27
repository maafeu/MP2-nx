# The PC build's window as the screen shows it (the port's options panel included, which the shots\NNNN.bmp of the
# guest frame leave out), saved at given seconds after the game's start: PrintWindow with PW_RENDERFULLCONTENT, so the
# window needs no focus and nothing on the PC's screen moves. Waits up to a minute for the game to start.
#   powershell -ExecutionPolicy Bypass -File tools\tests\window_shot.ps1 -At "58,61" [-Out build\mp2-pc\shots]
param([string]$At = "60", [string]$Out = "$PSScriptRoot\..\..\build\mp2-pc\shots")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
New-Item -ItemType Directory -Force $Out | Out-Null
$p = $null
foreach ($i in 1..120) {
    $p = Get-Process -Name 'MPayne2-nx_pc' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $p) { throw 'the game did not start' }
foreach ($s in ($At -split ',' | ForEach-Object { [double]$_ })) {
    while (((Get-Date) - $p.StartTime).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 100 }
    $p.Refresh()
    $h = $p.MainWindowHandle
    $r = New-Object W+RECT
    [void][W]::GetClientRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.r)), ([Math]::Max(1, $r.b))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    $ok = [W]::PrintWindow($h, $dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $file = Join-Path $Out ('window_{0:000}s.png' -f [int]$s)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "$file $($r.r)x$($r.b) printed: $ok"
}
