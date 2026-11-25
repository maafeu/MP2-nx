# The Switch build (host-switch) with the Windows CMake and Ninja and devkitPro's toolchain file, for a PC whose msys2
# has no ninja (ferramentas/build_switch.sh needs one; installing it would be a download).
#   powershell -ExecutionPolicy Bypass -File tools\build_switch.ps1 [-Game exemplos\mp2] [-Target host_check] [-Jobs 12]
#                                                                    [-Renderer soft|nvk] [-Extra "-DX=Y"]
param(
    [string]$Game = 'exemplos\mp2',
    [string]$Target = '',
    [int]$Jobs = 12,
    [string]$Renderer = '',
    [string]$Extra = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$gameDir = (Resolve-Path (Join-Path $root $Game)).Path
$name = Split-Path $gameDir -Leaf
$out = Join-Path $root "build\$name-switch"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$env:DEVKITPRO = 'C:/devkitPro'
$env:PATH = "$ninjaDir;C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;$env:PATH"
if (-not (Test-Path "$out\build.ninja")) {
    $cfg = @('-S', "$root\host-switch", '-B', $out, '-G', 'Ninja', "-DPORT_GAME_DIR=$gameDir",
             '-DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', "-DCMAKE_MAKE_PROGRAM=$ninjaDir\ninja.exe")
    if ($Renderer) { $cfg += "-DPORT_RENDERER=$Renderer" }
    if ($Extra) { $cfg += $Extra.Split(' ') }
    & $cmake @cfg
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
}
$b = @('--build', $out, '-j', "$Jobs")
if ($Target) { $b += @('--target', $Target) }
& $cmake @b
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Get-ChildItem "$out\*.nro" -ErrorAction SilentlyContinue | Select-Object Name, Length
