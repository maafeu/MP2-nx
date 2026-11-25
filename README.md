# MP2-nx — Max Payne 2 (Xbox) for Nintendo Switch

An experimental static recompilation port of the original Xbox **Max Payne 2: The Fall of Max Payne**: the Xbox's
x86 executable is turned into C++ and compiled for the Switch's ARM64 (and, as a test build, for Windows). It is not
emulation: the game's code runs natively, and the Xbox libraries (Direct3D 8, DirectSound, the kernel) are rewritten
for the host.

> This repository does not contain the game. The disc files, the XBE and the C++ generated from it are not in it and
> must never be added. You need your own copy of Max Payne 2 (Xbox, NTSC-U, `TT-012`). **Do not share an NRO or an
> exe built from this repository**: each holds the game's recompiled code.

**Status (2026-11-24):** complete on both targets, played through by hand.

- **Nintendo Switch** (the deko3d renderer, 1280x720): every level plays at 60 fps on the console in handheld with the
  shipped settings (`package/mp2-nx.toml`: CPU 1581 MHz, memory 1600 MHz; a Horizon-OC profile holding them, with the
  handheld GPU at 460.8 MHz for level 19's GPU-heavy stretch: `package/TESTING.md`). Films, menus, saving and
  loading, chapters played through, the ending and the credits; the port's options panel (Minus + R3 or both sticks)
  with gyro aiming. The game's frame is drawn at the full 1280x720 (`render_wide = 1`).
- **PC** (Windows, Vulkan): every level at 60 fps, borderless or windowed (F11), mouse aiming, an Xbox-style
  controller, the same options panel (F10).

See [docs/MP2-notes.md](docs/MP2-notes.md) for every decision, fix and measurement, and
[docs/progress-log.md](docs/progress-log.md) for the history.

**Building:** `ferramentas/recompilar.ps1 exemplos/mp2` makes the C++ from your own `default.xbe`; then
`ferramentas\build_pc.bat exemplos\mp2` for the PC, and in devkitPro's msys2
`CMAKE_EXTRA='-DPORT_RENDERER=dk -DPYTHON_EXECUTABLE=<python.exe>' ./ferramentas/build_switch.sh exemplos/mp2` for the
Switch NRO (`-DPORT_RENDERER=gl` for the OpenGL fallback). `tools/make_package.ps1` and `tools/make_package_pc.ps1`
lay out a package for your own console or PC; `package/TESTING.md` and `package/README-PC.txt` say how to set it up.

## Layout

| Folder | What it is |
| --- | --- |
| `exemplos/mp2/` | The port: `port.cmake`, `config/game.toml`, the game's Direct3D 8 and DirectSound HLE and options (`src/`), its hooks (`src/game/`) |
| `sdk/XboxXBE-Recomp/` | The recompiler and runtime (TGP482's XboxXBE-Recomp, MIT) with the kit's Switch changes, and this port's XDK 5558 data (`xdk/5558`) |
| `host-switch/`, `host-pc/` | The Switch layer and the Windows test build |
| `ferramentas/` | The kit's scripts: recompile, build, package |
| `tools/` | This port's own scripts (disc extraction, XDK 5558 work, PC and Ryujinx runs, RenderDoc analysis, packaging, the GL helper sync) |
| `docs/` | The kit's documentation (Portuguese) and this port's notes |

The kit's layout is kept as the base has it (`COLD-FEAR-NX-PORT`'s `kit/`), so a fix here is a plain patch for the
base.

## Credits and licences

Code this port is built from:

- **XboxSwitchPortKit** and the Cold Fear Switch port (`COLD-FEAR-NX-PORT`) — the base of this repository: the kit's
  Switch host, the Vulkan renderer, the Direct3D 8 / DirectSound layer and its Switch fixes.
- **XboxXBE-Recomp** by TGP482 (MIT) — the x86 recompiler and runtime (`sdk/XboxXBE-Recomp/LICENSE`).
- **Black-Android** (MIT) — the base of the game layer in `exemplos/mp2/src` (`exemplos/mp2/LICENSE-Black-Android`).
- toml++, Dear ImGui and glslang under their own licences (`sdk/XboxXBE-Recomp/THIRD_PARTY.txt`).
- **SPIRV-Cross** (Khronos, Apache-2.0) — SPIR-V to GLSL for the OpenGL renderer (`third_party/spirv-cross/LICENSE`),
  and the Khronos OpenGL / KHR headers (`third_party/gl`, MIT-style Khronos licence in each header).
- **switch-mesa** (devkitPro's portlibs: Mesa's nouveau GL driver, EGL, libdrm_nouveau; MIT) — linked by the Switch
  build with `PORT_RENDERER=gl`.
- `host-switch/gl_backend.cpp` (this port) carries a verbatim copy of the kit's `vulkan.cpp` console-state helpers
  (XboxSwitchPortKit), kept in step by `tools/make_gl_backend.py`; `tools/zydis-lite` is written for this port and
  follows Zydis 4.1's public API (Zydis: MIT).

The Switch porting method this work follows comes from the Xbox 360 ports and their shared base: Renut's renderer
and the renut-nx ultimate base, renut-nx, sa2-nx, NetherSX2_nx, battd_nx and the bejeweled3-nx / zuma ports. Their
code is credited wherever any of it is used here; [docs/MP2-notes.md](docs/MP2-notes.md) says what is.

This repository's own code is under the MIT licence ([LICENSE](LICENSE)), as the base's is; the parts above keep
their own licences and notices. Max Payne 2 itself is not covered by any of them and is not included.
