# Runs the PC test build for a while and prints the end of its log.
#   powershell -ExecutionPolicy Bypass -File tools\run_pc.ps1 [-Seconds 60] [-Shots 15] [-Script file] [-Tail 60]
# -Shots N: a screenshot (shots\NNNN.bmp beside the exe) every N seconds. -Script: lines "second BUTTONS [frames]",
# each written to the exe's cmd file at that second (host-pc/input.cpp presses them). Only this run's process is
# stopped, by its id.
param(
    [int]$Seconds = 60,
    [int]$Shots = 0,
    [string]$Script = '',
    [int]$Tail = 60,
    [string]$Game = "$PSScriptRoot\..\..\game",
    [switch]$Docked,
    [switch]$RenderDoc  # launched by renderdoccmd; script step RDC captures a frame into build\rdc
)
$ErrorActionPreference = 'Stop'
$exeDir = Resolve-Path "$PSScriptRoot\..\build\mp2-pc"
$exe = Join-Path $exeDir 'mp2-nx_pc.exe'
$game = (Resolve-Path $Game).Path
# a window, not the borderless screen the shipped settings ask for (2026-11-24): the test runs stay out of the way
$args2 = @("`"$game`"", '--windowed')
if ($Docked) { $args2 += '--docked' }
$steps = @()
if ($Script) {
    foreach ($l in Get-Content $Script) {
        if ($l -match '^\s*#' -or -not $l.Trim()) { continue }
        $parts = $l.Trim() -split '\s+', 2
        $steps += [pscustomobject]@{ At = [double]$parts[0]; Cmd = $parts[1] }
    }
}
if ($RenderDoc) {
    $rdcDir = Join-Path (Resolve-Path "$PSScriptRoot\..\build").Path 'rdc'
    New-Item -ItemType Directory -Force $rdcDir | Out-Null
    $rargs = @('capture', '-d', "`"$exeDir`"", '-c', "`"$rdcDir\mp2`"", "`"$exe`"") + $args2
    $launcher = Start-Process -FilePath 'C:\Program Files\RenderDoc\renderdoccmd.exe' -ArgumentList $rargs -PassThru
    $p = $null
    for ($i = 0; $i -lt 40 -and -not $p; $i++) {
        Start-Sleep -Milliseconds 250
        $c = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($launcher.Id)" | Where-Object { $_.Name -eq 'mp2-nx_pc.exe' }
        if ($c) { $p = Get-Process -Id $c.ProcessId }
    }
    if (-not $p) { $p = $launcher }
} else {
    $p = Start-Process -FilePath $exe -ArgumentList $args2 -WorkingDirectory $exeDir -PassThru
}
$start = Get-Date
$nextShot = $Shots
while (-not $p.HasExited) {
    $t = ((Get-Date) - $start).TotalSeconds
    if ($t -ge $Seconds) { break }
    foreach ($s in $steps) {
        if (-not $s.PSObject.Properties['Done'] -and $t -ge $s.At) {
            # TRACE: the D3D layer logs the next frame's calls; SHOT: one screenshot
            if ($s.Cmd -eq 'TRACE') { New-Item -ItemType File (Join-Path $exeDir 'trace') -Force | Out-Null }
            elseif ($s.Cmd -eq 'RDC') { New-Item -ItemType File (Join-Path $exeDir 'rdc') -Force | Out-Null }
            elseif ($s.Cmd -eq 'SHOT') { New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null }
            else { Set-Content -Path (Join-Path $exeDir 'cmd') -Value $s.Cmd -Encoding ascii }
            $s | Add-Member -NotePropertyName Done -NotePropertyValue $true
        }
    }
    if ($Shots -gt 0 -and $t -ge $nextShot) {
        New-Item -ItemType File (Join-Path $exeDir 'shot') -Force | Out-Null
        $nextShot += $Shots
    }
    Start-Sleep -Milliseconds 250
}
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
$log = Get-ChildItem (Join-Path $exeDir 'logs\mp2_*.log') | Sort-Object LastWriteTime | Select-Object -Last 1
"run: alive at the end = $alive, exit code = $($p.ExitCode), log = $($log.Name)"
Get-Content $log.FullName -Tail $Tail
