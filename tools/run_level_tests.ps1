# Each level in turn on the PC test build (OpenGL renderer), through test_level (src/game/test_level.cpp) and
# tools\scripts\mp2_level_test.txt: shots every 10 s, a contact sheet build\level_tests\lNN.png, and per level the
# log's level inits, crash lines and frame lines in build\level_tests\summary.txt. The settings file beside the exe
# is this script's own (test_level only) and is removed at the end.
#   powershell -ExecutionPolicy Bypass -File tools\run_level_tests.ps1 [-Levels "4-30" | "2,9,14"] [-Seconds 140]
# (-Levels is a string: powershell -File passes no arrays)
param(
    [string]$Levels = '2-30',
    [int]$Seconds = 140,
    # gl: the OpenGL renderer (the Switch's), vk: the kit's Vulkan renderer, for comparing the two; vk runs write
    # build\level_tests\vkNN and lNN_vk.png
    [string]$Renderer = 'gl'
)
$ErrorActionPreference = 'Stop'
$list = @()
foreach ($part in ($Levels -split '[,; ]+' | Where-Object { $_ })) {
    if ($part -match '^(\d+)-(\d+)$') { $list += [int]$Matches[1]..[int]$Matches[2] } else { $list += [int]$part }
}
$root = Resolve-Path "$PSScriptRoot\.."
Set-Location $root
if ($Renderer -eq 'vk') { Remove-Item Env:\MP2_GL -ErrorAction SilentlyContinue } else { $env:MP2_GL = '1' }
$tag = if ($Renderer -eq 'vk') { 'vk' } else { '' }
$out = Join-Path $root 'build\level_tests'
New-Item -ItemType Directory -Force $out | Out-Null
$summary = Join-Path $out 'summary.txt'
$toml = Join-Path $root 'build\mp2-pc\mp2-nx.toml'
foreach ($lv in $list) {
    Set-Content -Path $toml -Value "test_level = $lv" -Encoding ascii
    Remove-Item -Recurse -Force (Join-Path $root 'build\mp2-pc\shots') -ErrorAction SilentlyContinue
    $dir = Join-Path $out ('{0}{1}' -f $tag, $lv)
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $head = & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'tools\run_pc.ps1') -Seconds $Seconds -Shots 10 `
        -Script (Join-Path $root 'tools\scripts\mp2_level_test.txt') -Tail 0
    $log = (Get-ChildItem (Join-Path $root 'build\mp2-pc\logs\mp2_*.log') | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    Add-Content -Path $summary -Value ("== level {0} {3} ({1}) {2}" -f $lv, (Get-Date -Format 'HH:mm:ss'), ($head | Select-Object -First 1), $Renderer)
    Select-String -Path $log -Pattern 'game: level init|test:|crash: (exc|guest|in )|d3d: Swap (12|24|30|36|39)00|gl: present (24|36)01' |
        ForEach-Object { Add-Content -Path $summary -Value ('  ' + $_.Line) }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $shots = Join-Path $root 'build\mp2-pc\shots'
    if (Test-Path $shots) {
        Copy-Item "$shots\*" $dir -Force
        $sheet = if ($tag) { 'l{0:D2}_vk.png' -f $lv } else { 'l{0:D2}.png' -f $lv }
        & python (Join-Path $root 'tools\contact_sheet.py') (Join-Path $out $sheet) $dir 4 4 | Out-Null
    }
}
Remove-Item -Force $toml -ErrorAction SilentlyContinue
Add-Content -Path $summary -Value ("== done {0}" -f (Get-Date -Format 'HH:mm:ss'))
