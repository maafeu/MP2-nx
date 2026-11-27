# The console package (private, for the user's own Switch; it holds the recompiled game in the NRO, so it is not to be
# published): the NRO, the settings files, the ELF and a snapshot of the port's sources for mapping a crash or a
# profile to source lines, and the test notes. -Build: the build folder of the main NRO (the deko3d renderer's,
# build\mp2-dk); -Gl: the OpenGL renderer's build, put under fallback/ as mp2-nx-gl.nro (with its ELF); -Fallback: a
# third NRO under fallback/ as mp2-nx-soft.nro, e.g. the soft renderer's build, for when no GPU renderer starts.
#   powershell -ExecutionPolicy Bypass -File tools\make_package.ps1 [-Out ..\out\package] [-Build build\mp2-dk]
#                                                                    [-Gl build\mp2] [-Fallback build\mp2-soft]
#                                                                    [-GlRev 919d41a] [-FallbackRev cfe7f01]
param([string]$Out = "$PSScriptRoot\..\..\out\package", [string]$Build = "$PSScriptRoot\..\build\mp2-dk",
      [string]$Gl = "$PSScriptRoot\..\build\mp2", [string]$Fallback = "",
      # the git revisions the GL and the third fallback NROs were built from, when not the sources' (they are not
      # rebuilt every time)
      [string]$GlRev = "", [string]$FallbackRev = "")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Build = (Resolve-Path $Build).Path
$nro = Join-Path $Build 'mp2-nx.nro'
$elf = Join-Path $Build 'mp2-nx.elf'
if (-not (Test-Path $nro)) { throw "no NRO in ${Build}: build it first (ferramentas/build_switch.sh exemplos/mp2)" }
function Get-Renderer([string]$dir) {
    (Select-String -Path (Join-Path $dir 'CMakeCache.txt') -Pattern '^PORT_RENDERER:STRING=(.*)$').Matches[0].Groups[1].Value
}
$renderer = Get-Renderer $Build
$sd = Join-Path $Out 'sd\switch\mp2-nx'
New-Item -ItemType Directory -Force $sd | Out-Null
Copy-Item $nro $sd -Force
Copy-Item (Join-Path $root 'package\mp2-nx.toml') $sd -Force
Copy-Item (Join-Path $root 'package\mp2-nx.toml') (Join-Path $Out 'mp2-nx_shipped.toml') -Force
Copy-Item (Join-Path $root 'package\mp2-nx_play.toml') $Out -Force
Copy-Item (Join-Path $root 'package\mp2-nx_ab.toml') $Out -Force
Copy-Item (Join-Path $root 'package\mp2-nx_30.toml') $Out -Force
Copy-Item (Join-Path $root 'tools\log_summary.py') $Out -Force
Copy-Item (Join-Path $root 'package\TESTING.md') $Out -Force
Copy-Item (Join-Path $root 'tools\scripts\mp2_gameplay_switch.txt') (Join-Path $Out 'input_script.txt') -Force
Copy-Item (Join-Path $root 'tools\scripts\mp2_gameplay_switch_newgame2.txt') (Join-Path $Out 'input_script_newgame2.txt') -Force
# the renderers' one-run self-tests: dk_flags.txt for the deko3d NRO, gl_flags.txt for the GL fallback
Set-Content -Path (Join-Path $Out 'dk_flags.txt') -Value 'selftest' -NoNewline -Encoding ascii
Set-Content -Path (Join-Path $Out 'gl_flags.txt') -Value 'selftest' -NoNewline -Encoding ascii
$hash = (Get-FileHash $elf -Algorithm SHA256).Hash.Substring(0, 8)
New-Item -ItemType Directory -Force (Join-Path $Out 'analysis') | Out-Null
Copy-Item $elf (Join-Path $Out "analysis\mp2-nx-$hash.elf") -Force
$rev = (& git -C $root rev-parse --short HEAD)
& git -C $root archive --format=zip -o (Join-Path $Out "analysis\mp2-nx-src-$rev.zip") HEAD
$lines = @(
    "NRO: $((Get-FileHash $nro -Algorithm SHA256).Hash)",
    "ELF: mp2-nx-$hash.elf (the NRO's own build; tools\patch_gcs.py applied to both)",
    "Sources: mp2-nx-src-$rev.zip = git $rev of MaxPayne2-nx (the recompiled C++ is not in it: exemplos\mp2\generated, made",
    "  again from the player's default.xbe by ferramentas\recompilar.ps1 exemplos\mp2, the same for the same XBE; nor",
    "  third_party\, whose sources third_party\README.md names)",
    "Renderer: PORT_RENDERER=$renderer")
$fb = Join-Path $Out 'fallback'
function Get-From([string]$from) {
    if ($from -and $from -ne $rev) { "built from git $from, an earlier revision than the NRO's" } else { 'the same git revision' }
}
$glFrom = Get-From $GlRev
$fbFrom = Get-From $FallbackRev
if ($Gl -and (Test-Path (Join-Path $Gl 'mp2-nx.nro'))) {
    New-Item -ItemType Directory -Force $fb | Out-Null
    Copy-Item (Join-Path $Gl 'mp2-nx.nro') (Join-Path $fb 'mp2-nx-gl.nro') -Force
    $ghash = (Get-FileHash (Join-Path $Gl 'mp2-nx.elf') -Algorithm SHA256).Hash.Substring(0, 8)
    Copy-Item (Join-Path $Gl 'mp2-nx.elf') (Join-Path $Out "analysis\mp2-nx-gl-$ghash.elf") -Force
    $lines += "Fallback: fallback\mp2-nx-gl.nro, PORT_RENDERER=$(Get-Renderer $Gl), ELF analysis\mp2-nx-gl-$ghash.elf, NRO $((Get-FileHash (Join-Path $fb 'mp2-nx-gl.nro') -Algorithm SHA256).Hash) ($glFrom)"
}
if ($Fallback) {
    New-Item -ItemType Directory -Force $fb | Out-Null
    Copy-Item (Join-Path $Fallback 'mp2-nx.nro') (Join-Path $fb 'mp2-nx-soft.nro') -Force
    $fhash = (Get-FileHash (Join-Path $Fallback 'mp2-nx.elf') -Algorithm SHA256).Hash.Substring(0, 8)
    Copy-Item (Join-Path $Fallback 'mp2-nx.elf') (Join-Path $Out "analysis\mp2-nx-soft-$fhash.elf") -Force
    $lines += "Fallback: fallback\mp2-nx-soft.nro, PORT_RENDERER=soft, ELF analysis\mp2-nx-soft-$fhash.elf, NRO $((Get-FileHash (Join-Path $fb 'mp2-nx-soft.nro') -Algorithm SHA256).Hash) ($fbFrom; the last-resort CPU renderer, slow)"
}
Set-Content -Path (Join-Path $Out 'analysis\BUILD.txt') -Value $lines -Encoding ascii
Get-ChildItem $Out -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring($Out.Length + 1) } }, Length
