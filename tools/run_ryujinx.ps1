# MP2-nx: one emulator run of the NRO in the user's Ryujinx with this port's own data folder (Ryujinx\MP2, made from
# the SSX port's system folders: bis firmware and system keys, no sdcard). Refuses to start while any Ryujinx runs or
# when the free commit memory is under -MinCommitGB; stops only the process it started; puts the SD's mp2-nx.toml back
# as it was after the run. Based on katamari-nx's tools/run_ryujinx.ps1.
#   powershell -File tools/run_ryujinx.ps1 -Seconds 240 -Name emu1 [-Toml <an mp2-nx.toml for this run>]
param([int]$Seconds = 240, [string]$Name = "emu", [string]$Toml = "", [double]$MinCommitGB = 8,
      [string]$Nro = "", [string]$Script = "",
      # the GL renderer's test switches for this run (gl_flags.txt beside the NRO, removed after), e.g. "noscissor"
      [string]$GlFlags = "",
      # the deko3d renderer's (dk_flags.txt), e.g. "selftest"
      [string]$DkFlags = "",
      # seconds into the run at which to save a picture of Ryujinx's own window (what the emulator displays, which
      # the port's captures cannot show: they read the frame back through the emulated GPU); the window is shown
      # normal instead of minimized for these
      [string]$WindowShots = "",  # "40,110": a string, since powershell -File passes no arrays
      # seconds between the port's captures (the "capture" file's text; empty: the renderer's 15)
      [string]$Capture = "",
      # the Ryujinx folder beside the port's parent folder (Desktop\Ryujinx next to Desktop\Max Payne 2)
      [string]$Ryujinx = "$PSScriptRoot\..\..\..\Ryujinx")
$root = Split-Path -Parent $PSScriptRoot
$ryuDir = (Resolve-Path $Ryujinx).Path
$ryu = Join-Path $ryuDir "publish\Ryujinx.exe"
$data = Join-Path $ryuDir "MP2"
$sd = Join-Path $data "sdcard\switch\mp2-nx"
$sdNro = Join-Path $sd "mp2-nx.nro"
$out = Join-Path $root "build\emu_runs\$Name"
if (Get-Process -Name Ryujinx -ErrorAction SilentlyContinue) { Write-Output "a Ryujinx is running (another port's?): not starting"; exit 2 }
$os = Get-CimInstance Win32_OperatingSystem
$freeCommit = $os.FreeVirtualMemory / 1MB
if ($freeCommit -lt $MinCommitGB) { Write-Output ("free commit memory {0:N1} GB < {1} GB: not starting" -f $freeCommit, $MinCommitGB); exit 3 }
if (-not $Nro) { $Nro = Join-Path $root "build\mp2\mp2-nx.nro" }
if (-not (Test-Path $Nro)) { Write-Output "no NRO at $Nro"; exit 4 }
New-Item -ItemType Directory -Force $sd | Out-Null
# the disc files: a junction to the extracted game, not a 4 GB copy
$gameLink = Join-Path $sd "game"
if (-not (Test-Path $gameLink)) {
    New-Item -ItemType Junction -Path $gameLink -Target (Resolve-Path "$root\..\game").Path | Out-Null
}
Copy-Item $Nro $sdNro -Force
# the renderer's captures: a frame every 15 s into shots, or every $Capture seconds
Set-Content -Path (Join-Path $sd "capture") -Value $Capture -NoNewline
# the game's scripted presses for this run (host-switch/input.cpp reads input_script.txt beside the NRO)
$sdScript = Join-Path $sd "input_script.txt"
if ($Script) { Copy-Item $Script $sdScript -Force } elseif (Test-Path $sdScript) { Remove-Item $sdScript }
$sdFlags = Join-Path $sd "gl_flags.txt"
if ($GlFlags) { Set-Content -Path $sdFlags -Value $GlFlags -NoNewline } elseif (Test-Path $sdFlags) { Remove-Item $sdFlags }
$sdDkFlags = Join-Path $sd "dk_flags.txt"
if ($DkFlags) { Set-Content -Path $sdDkFlags -Value $DkFlags -NoNewline } elseif (Test-Path $sdDkFlags) { Remove-Item $sdDkFlags }
$sdToml = Join-Path $sd "mp2-nx.toml"
$savedToml = $null
if (Test-Path $sdToml) { $savedToml = Get-Content $sdToml -Raw }
if ($Toml) { Copy-Item $Toml $sdToml -Force }
New-Item -ItemType Directory -Force $out | Out-Null
$logs = Join-Path $sd "logs"
$started = Get-Date
$shotTimes = @($WindowShots -split '[,; ]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })
$style = if ($shotTimes.Count) { 'Normal' } else { 'Minimized' }
$p = Start-Process -FilePath $ryu -ArgumentList @("-r", "`"$data`"", "`"$sdNro`"") -PassThru -WindowStyle $style `
     -RedirectStandardOutput (Join-Path $out "ryujinx_stdout.txt") -RedirectStandardError (Join-Path $out "ryujinx_stderr.txt")
Write-Output ("started Ryujinx pid {0} (free commit {1:N1} GB)" -f $p.Id, $freeCommit)
if ($shotTimes.Count) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class RyuWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  // the process's biggest visible top-level window (Process.MainWindowHandle stays 0 for Ryujinx's)
  public static IntPtr Biggest(uint want) {
    IntPtr best = IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid); RECT r;
      if (pid == want && IsWindowVisible(h) && GetWindowRect(h, out r)) {
        long a = (long)(r.R - r.L) * (r.B - r.T); if (a > area) { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best;
  }
}
"@
}
$deadline = (Get-Date).AddSeconds($Seconds)
$pending = [System.Collections.Generic.List[int]]::new(); $shotTimes | ForEach-Object { $pending.Add($_) }
# a window left in the background for minutes showed only black when raised for a shot (gl9-gl12; gl8, raised at
# 40 s, was fine): Ryujinx's window is raised once it exists and kept in front, and each shot waits 2 s after raising
$raised = $false
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Seconds 1
    $el = ((Get-Date) - $started).TotalSeconds
    if ($shotTimes.Count -and -not $raised -and $el -ge 5) {
        $p.Refresh()
        $h0 = $p.MainWindowHandle
        if ($h0 -eq [IntPtr]::Zero) { $h0 = [RyuWin]::Biggest([uint32]$p.Id) }
        if ($h0 -ne [IntPtr]::Zero) { [RyuWin]::SetForegroundWindow($h0) | Out-Null; $raised = $true }
    }
    foreach ($s in @($pending)) {
        if ($el -lt $s) { continue }
        $pending.Remove($s) | Out-Null
        $p.Refresh()
        $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { $h = [RyuWin]::Biggest([uint32]$p.Id) }
        $r = New-Object RyuWin+RECT
        if ($h -eq [IntPtr]::Zero) { Write-Output "no Ryujinx window at $s s" }
        elseif ([RyuWin]::GetWindowRect($h, [ref]$r)) {
            [RyuWin]::SetForegroundWindow($h) | Out-Null
            Start-Sleep -Seconds 2
            $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            # PrintWindow with PW_RENDERFULLCONTENT (2): a screen copy reads a window on a direct flip plane as black
            $hdc = $g.GetHdc()
            $okp = [RyuWin]::PrintWindow($h, $hdc, 2)
            $g.ReleaseHdc($hdc)
            if (-not $okp) { $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
            $bmp.Save((Join-Path $out ("window_{0:D3}s.png" -f $s)), [System.Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
        }
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id) after $Seconds s" }
else { Write-Output "Ryujinx exited by itself (code $($p.ExitCode))" }
# 2026-11-22: copying while the killed process still held the log copied nothing, and the next run saw it running
if (-not $p.WaitForExit(30000)) { Write-Output "pid $($p.Id) still exiting after 30 s" }
Start-Sleep -Seconds 2
# this run's logs only
if (Test-Path $logs) {
    New-Item -ItemType Directory -Force (Join-Path $out "logs") | Out-Null
    Get-ChildItem $logs -File | Where-Object { $_.LastWriteTime -ge $started } | Copy-Item -Destination (Join-Path $out "logs") -Force
}
Get-ChildItem $sd -Include "*.png","*.bmp" -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started } |
    Move-Item -Destination $out -Force
if ($GlFlags -and (Test-Path $sdFlags)) { Remove-Item $sdFlags }
if ($DkFlags -and (Test-Path $sdDkFlags)) { Remove-Item $sdDkFlags }
# the SD's toml as it was before the run
# (2026-11-22: once the removal failed silently while the stopped emulator still held the file, and every later run
# then "restored" that run's test settings: so retried, and a failure said out loud)
for ($try = 0; $try -lt 10; $try++) {
    try {
        if ($null -ne $savedToml) { Set-Content -Path $sdToml -Value $savedToml -NoNewline -ErrorAction Stop }
        elseif ($Toml -and (Test-Path $sdToml)) { Remove-Item $sdToml -ErrorAction Stop }
        break
    } catch { Start-Sleep -Seconds 1 }
}
if ($try -ge 10) { Write-Output "WARNING: the SD's mp2-nx.toml could not be put back as it was" }
$rlog = Get-ChildItem (Join-Path $data "Logs") -Filter "*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $rlog) { $rlog = Get-ChildItem (Split-Path $ryu) -Recurse -Filter "*.log" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime | Select-Object -Last 1 }
if ($rlog) { Copy-Item $rlog.FullName (Join-Path $out "ryujinx.log") -Force }
Get-ChildItem $out -Recurse | Select-Object FullName, Length
