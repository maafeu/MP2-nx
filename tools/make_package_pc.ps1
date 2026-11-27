# The PC package (private, for the user's own PC; the exe holds the recompiled game, so it is not to be published):
# the exe, the shipped settings, the readme and the Visual C++ runtime beside it (app-local, from Visual Studio's own
# Redist folder, so no installer is needed), and under analysis\ the PDB for mapping a crash to source lines.
#   powershell -ExecutionPolicy Bypass -File tools\make_package_pc.ps1 [-Out ..\out\package\pc] [-Build build\mp2-pc]
param([string]$Out = "$PSScriptRoot\..\..\out\package\pc", [string]$Build = "$PSScriptRoot\..\build\mp2-pc")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$Build = (Resolve-Path $Build).Path
$exe = Join-Path $Build 'mp2-nx_pc.exe'
$pdb = Join-Path $Build 'mp2-nx_pc.pdb'
if (-not (Test-Path $exe)) { throw "no exe in ${Build}: build it first (ferramentas\build_pc.bat exemplos\mp2)" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Copy-Item $exe $Out -Force
Copy-Item (Join-Path $root 'package\mp2-nx_pc.toml') (Join-Path $Out 'mp2-nx.toml') -Force
Copy-Item (Join-Path $root 'package\README-PC.txt') (Join-Path $Out 'README.txt') -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'game') | Out-Null
Set-Content -Path (Join-Path $Out 'game\PUT THE GAME FILES HERE.txt') -Encoding ascii -Value @(
    'Copy the Max Payne 2 disc''s files here: default.xbe and its folders (README.txt beside the exe says more).')
# the Visual C++ runtime the exe imports (msvcp140, vcruntime140, vcruntime140_1), app-local
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem -Path (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crt) { throw "no Visual C++ runtime Redist folder under $vs" }
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item (Join-Path $crt.FullName $dll) $Out -Force }
$hash = (Get-FileHash $exe -Algorithm SHA256).Hash
New-Item -ItemType Directory -Force (Join-Path $Out 'analysis') | Out-Null
# only this exe's PDB: the ones an earlier run of this script put here match no exe in the package any more
$keep = "mp2-nx_pc-$($hash.Substring(0, 8)).pdb"
Get-ChildItem (Join-Path $Out 'analysis') -Filter 'mp2-nx_pc-*.pdb' | Where-Object { $_.Name -ne $keep } | Remove-Item
Copy-Item $pdb (Join-Path $Out "analysis\$keep") -Force
$rev = (& git -C $root rev-parse --short HEAD)
$dirty = (& git -C $root status --porcelain -- host-pc sdk exemplos/mp2/src | Measure-Object).Count
Set-Content -Path (Join-Path $Out 'analysis\BUILD.txt') -Encoding ascii -Value @(
    "EXE: $hash",
    "PDB: mp2-nx_pc-$($hash.Substring(0, 8)).pdb (the exe's own build)",
    "Sources: git $rev of MaxPayne2-nx$(if ($dirty) { " plus $dirty uncommitted files" })",
    "Visual C++ runtime: $($crt.FullName)")
Get-ChildItem $Out -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring($Out.Length + 1) } }, Length
