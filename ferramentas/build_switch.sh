#!/usr/bin/env bash
# Compila o .nro de um jogo. Rode no bash do devkitPro (C:\devkitPro\msys2\usr\bin\bash.exe -l):
#
#   cd /c/XboxSwitchPortKit && ./ferramentas/build_switch.sh exemplos/black
#
# Variaveis opcionais:
#   JOBS=8              quantos arquivos compilar ao mesmo tempo (padrao 6; cada um usa 1-2 GB de RAM)
#   TARGET=host_check   so o host e o runtime, sem o codigo recompilado: acha erros de compilacao rapido
#   BUILD_DIR=...       pasta do build (padrao build/<nome da pasta do jogo>)
#   CMAKE_EXTRA=...     opcoes a mais para o cmake (MaxPayne2-nx: -DPORT_RENDERER=soft|nvk)
#
# MaxPayne2-nx: sem ninja no msys2 (instalar seria um download), usa o make do msys2 ("Unix Makefiles"). O caminho do kit
# pode ter espacos ("Max Payne 2"): tudo vai entre aspas.
#
# Saida: build/<jogo>/<PORT_NAME>.nro e o .elf (que ferramentas/endereco_crash.sh le).
set -euo pipefail
if [ $# -lt 1 ]; then echo "uso: $0 <pasta do jogo>"; exit 1; fi
KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$(cd "$1" && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
BUILD="${BUILD_DIR:-$KIT/build/$(basename "$GAME")}"
if command -v ninja >/dev/null 2>&1; then GEN=Ninja; else GEN="Unix Makefiles"; fi
cmake -S "$KIT/host-switch" -B "$BUILD" -G "$GEN" \
    -DPORT_GAME_DIR="$GAME" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${CMAKE_EXTRA:-}
# MaxPayne2-nx: host-switch/CMakeLists.txt turns GCC's dependency files off for make (CMAKE_DEPENDS_USE_COMPILER, their
# C:/ paths broke glslang's rules); CMake 4.5 still makes each object depend on its target's compiler_depend.ts
# stamp without writing it. Empty stamps: CMake's own scanner (depend.make) tracks the headers.
if [ "$GEN" = "Unix Makefiles" ]; then
    find "$BUILD" -name build.make -path '*/CMakeFiles/*' | while read -r f; do
        d="$(dirname "$f")"
        [ -f "$d/compiler_depend.ts" ] || : > "$d/compiler_depend.ts"
        [ -f "$d/compiler_depend.make" ] || : > "$d/compiler_depend.make"
    done
fi
if [ -n "${TARGET:-}" ]; then
    cmake --build "$BUILD" -j "${JOBS:-6}" --target "$TARGET"
else
    cmake --build "$BUILD" -j "${JOBS:-6}"
fi
# MaxPayne2-nx: the GCS/SME system-register reads of GCC 16's libgcc, which Ryujinx's JIT cannot translate (never run on
# the console): tools/patch_gcs.py rewrites them in the ELF and the NRO
PY="$(command -v python3 || command -v python || true)"
[ -z "$PY" ] && PY="$(ls /c/Users/*/AppData/Local/Python/*/python.exe /c/Users/*/AppData/Local/Programs/Python/*/python.exe 2>/dev/null | head -1 || true)"
[ -z "$PY" ] && PY="/c/Users/$USERNAME/AppData/Local/Microsoft/WindowsApps/python.exe"
if [ -z "${TARGET:-}" ]; then
    for f in "$BUILD"/*.elf "$BUILD"/*.nro; do [ -f "$f" ] && "$PY" "$KIT/tools/patch_gcs.py" "$f"; done
fi
ls -la "$BUILD"/*.nro 2>/dev/null || true
