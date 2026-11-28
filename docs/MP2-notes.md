# Max Payne 2 (Xbox) — port notes

Every decision and game-specific fix of this port, with how it was found and tested. Newest last in each section.

## The game

| Field | Value |
| --- | --- |
| Game | Max Payne 2: The Fall of Max Payne (Xbox, Remedy / Rockstar, 2003) |
| Disc image | `Max Payne 2 - The Fall of Max Payne (USA).iso`, 7,825,162,240 bytes, a full redump (XGD1: the XDVDFS game partition at 0x18300000) |
| Title ID | `0x5454000C` (`TT-012`), region 1 (North America), certificate version 1 |
| `default.xbe` | 3,592,192 bytes, SHA-256 `13a0ef7fb8cb0f18d7d371f6fe96903db817d8965206f78600bc1c2d6a4e2bfd`, built 2003-11-11 |
| Entry point | `0x000970EA` (retail key) |
| XDK | **5558** for every library: XAPILIB, D3D8, XGRAPHC, LIBCPMT, LIBCMT, D3DX8, DSOUND, XBOXKRNL |
| Code sections | `.text` 0x11000–0x289724, `D3D`, `XGRPH`, `D3DX`, `DSOUND`, `WMADEC`, `XPP`, Bink (`BINK*`), `DOLBY` |
| Disc contents | `default.xbe`, `DATA.RAS` (77.9 MB), `assets/` (levels, menu, sv), `movies/*.bik` (Bink), `dsstdfx.bin` (the DirectSound effects image), 429 files |

The task text gives the title ID as SLUS-20814; that is the PlayStation 2 release's serial. The Xbox disc here is
TT-012 (`0x5454000C`), and that is what this port checks.

## Decision 1: the base and the code generator (2026-11-21)

The task asks for a ReXGlue port from "the ultimate base (COLD-FEAR-NX-PORT)". What is on disk:

- The game is an **original Xbox** title: an x86 (Pentium III) `default.xbe` with the XDK's Direct3D 8 (NV2A) and
  DirectSound. It never had an Xbox 360 release (the 360 plays it through its backward-compatibility emulator).
- ReXGlue (and the renut-nx / renut-ultimate tree that the task's reading list describes, at
  `Desktop\renut-ultimate\base`) recompiles Xbox 360 XEX files: PowerPC code and the Xenos GPU. It cannot read an XBE,
  and Renut's renderer translates Xenos command streams, not NV2A / Direct3D 8.
- `COLD-FEAR-NX-PORT-main` is not that tree. It is gerisson1996's Cold Fear (original Xbox) port, built on the
  **XboxSwitchPortKit** (`kit/`): TGP482's XboxXBE-Recomp (x86 → C++, MIT) with the Switch changes, Direct3D 8 and
  DirectSound rewritten over Vulkan (NVK on the Switch) and the Switch's audio, a Windows test build (`host-pc`) with
  the same renderer. It is the only base on this PC that can generate code for this game.

So this port is built on the kit, not on ReXGlue. The parts of the task that are about ReXGlue's tree (katamari's
files, `tools/codegen.ps1`, `post_codegen.py`, the CG-13 and calling-convention censuses of PPC code, the deko3d
Renut renderer and its keys, `switch_remote.py`, the renut tomls) have no counterpart here; each step that has one is
done with the kit's tool. The kit is copied to the root of this repo (its own layout kept, so a fix here is a plain
patch against the base's `kit/`), with the game in `exemplos/mp2`. Cold Fear's game layer is left out: its hooks
(`src/game/*`, by address), `config/game.toml`, cover, icon and `HANDOFF.md`.

## Decision 2: XDK 5558

The kit ships signatures and data layouts for XDK 5849 only (`sdk/XboxXBE-Recomp/xdk/5849`). Max Payne 2 is 5558,
so `xdk/5558/signatures.toml` and `layout.toml` are made for it: the functions found by the 5849 signatures where the
code did not change, by the `.XTLID` section (the XBE carries one, 0x5B8 bytes), and by hand from the code otherwise;
each layout value is proven by an instruction of this XBE, written beside it as the 5849 file does.

### Result

- `xbe_symbols` with the 5849 signatures alone found 152 of the 238 names (bytes, constants, `.XTLID`); the misses that
  matter were found by hand from the code:
  - `D3DDevice_SetPalette` 0x28A9C0: stores `[device + stage*4 + 0xF98]`, pushes NV2A method 0x41B20 + stage*64.
  - `D3D_BlockOnResource` 0x28F990: the 0x50000 surface check, then BlockOnTime 0x28F660 (Lock2DSurface 0x292791 calls it).
  - `BinkWait` 0x2C0470 (the game's `if (!BinkWait(b)) BinkDoFrame(b)` at 0xFDF79; its ×1000 at 0x2C053B) and
    `BinkNextFrame` 0x2C1120 (the 5849 signature's field offsets 0x5C, 0x138, 0x154, 0x158, 0x20C, 0x280, 0x300).
- Left at 0 on purpose: `D3DDevice_SetIndices` / `D3D_IndexData` (MP2 passes index pointers straight to
  `DrawIndexedVertices`; nothing writes the device's +0x7A0 but the teardown), the fence wrappers (no game code calls
  SetFence or BlockOnTime), the overlay functions, DirectSound streams, `DirectSoundCreateBuffer` and the listed
  `IDirectSoundBuffer_*` (their `.XTLID` ids are absent: not linked), all networking (XNet, sockets, XOnline: MP2 links
  none).
- Every CDevice, stream-table, DSOUND effects-image and XPP offset the runtime uses was checked in MP2's code: the same
  as 5849 (`xdk/5558/layout.toml` names the instruction for each).
- `xdk/5558/signatures.toml` was then signed from MP2's completed config (`xbe_symbols --sign`): a fresh scan
  reproduces the config exactly (154 functions, 8 data).

### The kit's by-address hooks (stale addresses)

The base's `src/d3d/d3d8.cpp` had Cold Fear hooks by address (`sub_002709E0`, `sub_00263C30`, ...). In MP2 those
addresses are unrelated game code. Each was moved to MP2's own function or unbound:

| Cold Fear | What | Max Payne 2 |
| --- | --- | --- |
| 0x263C30 | the D3D's GPU-busy test | 0x28AA50 (pushbuffer get/put at [device+0x934]+0x3240/0x3244, PGRAPH 0x400700) |
| 0x265310 | LoadVertexShaderProgram(function, address) | 0x28BF30 (+0x10A8 + address*16, method 0x41E9C) |
| 0x26D8A0 | vertex buffer lock (resource, flags) | 0x28E940 (flush 0x41710, wait 0x28FA10, returns data \| 0x80000000) |
| 0x267F10 | the lock's wait | 0x28FA10 |
| 0x2709E0, 0x2709C0, 0x263C60 | UpdateOverlay, GetOverlayUpdateStatus, GetDisplayFieldStatus | none linked: unbound |

## Decision 3: no downloads — the x86 decoder

The recompiler (`xbe_recomp`, `xbe_symbols`) decodes x86 with Zydis v4.1.1, which its CMake fetches from GitHub on the
first build. Downloads are not allowed for this work, and no copy of Zydis is on this PC (C:, D:, E: searched). toml++
is on the PC (renut-nx's `sdk/thirdparty/tomlplusplus`).

So `tools/zydis-lite` was written: the part of Zydis 4.1's API the two tools use (decoder, operands, raw immediate and
displacement offsets for the signature masking, `cpu_flags` for the cmp/test peephole, a small formatter), for 32-bit
x86 with x87, MMX, SSE (and SSE2 for lengths). The x87 operand order follows what the code generator reads
(`fadd st0, st(i)` / `fadd st(i), st0` / `fcom st(i)` / `fcomi st0, st(i)`). Checked against LLVM's disassembler
(`llvm-objdump`) on every instruction of the game's code sections (`tools/zydis-lite/test/compare.py`): .text 890,891
instructions identical; every remaining difference is in data islands (jump tables, Bink and WMA tables) or LLVM's
spelling. `cpu_flags` errs towards "modifies" so the peephole can only be lost, never wrong. `-DXBE_RECOMP_FETCH=ON`
brings Zydis back when downloads are allowed.

## Decision 4: the Switch renderer — no NVK on this PC

The kit's Switch renderer is `host-switch/vulkan.cpp` on a static NVK (Mesa) build in `nvk/`, which the base does not
ship and which is nowhere on this PC; making one needs Mesa's sources (a download). The task's deko3d Renut renderer
translates Xbox 360 (Xenos) commands, not Direct3D 8, so it does not apply either.

What was done instead: `PORT_RENDERER=soft` (the default when `nvk/` is absent) — the SDK's CPU reference renderer
(`harness/reference_renderer.cpp`) behind `gpu::Backend`, presenting through libnx's framebuffer at 1280x720 (the 4:3
frame 960x720 in the middle). No GPU is used. It runs the game on the Switch target and in Ryujinx to the main menu,
with a correct picture, but slowly (the menu at about 4 fps under Ryujinx): a bring-up path, not a shipping renderer.

The ways to a GPU renderer on the Switch:
1. NVK for the Switch (the base's `nvk/`): the Vulkan renderer already draws this game correctly on the PC and would
   carry over as the kit's other ports do. Needs Mesa's sources (a download): the user's decision.
2. A deko3d back end for `gpu::Backend`.
3. switch-mesa OpenGL (devkitPro's portlibs, on the PC: `libEGL`, `libglapi`, `libdrm_nouveau`).

### Decision 4b (2026-11-22 00:40): the OpenGL renderer, `PORT_RENDERER=gl`

The shader problem behind 2 and 3 went away: SPIRV-Cross's sources were already on the PC (in another project's
download folder; copied into `third_party/spirv-cross`, Apache-2.0), so the kit's own chain — the SDK's HLSL → glslang
→ SPIR-V (`vk_shader.cpp`, with its disk cache) — continues to GLSL. Probe: all 15 of the Vulkan renderer's shader
kinds went HLSL → SPIR-V → GLSL → deko3d DKSH with uam (`tools/deko3d_shader_probe.py`), so deko3d would work too.

GL was picked over deko3d because it is a direct translation of `vulkan.cpp` (same objects: textures, framebuffers,
samplers, buffers), can be written and checked on the PC first (WGL, the same GPU as the Vulkan renderer, RenderDoc on
both), and switch-mesa is already built. deko3d stays the faster option on the console later (no Mesa CPU overhead).

`host-switch/gl_backend.cpp` + `gl_shader.cpp`:
- Draw for draw what `vulkan.cpp` does: the same targets (render scale, CPU writes latched and blitted in, read-backs at
  the console's size), textures (S3TC native, palettes, cubes, volumes with DXT decoded on the CPU), samplers (the same
  border colour choice, shadow compare with the swapped test), state, clears, copies (`glCopyImageSubData`), the gamma
  ramp and video overlay at present. The console-state helpers (constants, keys, palettes) are `vulkan.cpp`'s own,
  copied between markers by `tools/make_gl_backend.py` (run it after `vulkan.cpp` changes).
- Clip space is Vulkan's: `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`, so a target's row 0 is the console's top row
  in GL as in Vulkan and guest memory, and nothing is flipped but the present (drawn with the lower-left origin).
  Front face as in `vulkan.cpp` (ARB_clip_control keeps facing as before the flip); `MP2_GL_FACE=1` swaps it to check a
  driver. Flat shading takes the first vertex (`glProvokingVertex`), as the console and Vulkan do.
- SPIRV-Cross turns each texture/sampler pair a shader uses into a combined sampler on its own unit; `Pair` records
  which of t0-t3 and s0-s3 the unit takes, and units sampled with a comparison get a compare sampler.
- Draws run on the title's thread (`XBE_RENDER_THREAD=0`), where the context is current.
- The kit's options panel and FPS counter (ImGui) are drawn by the renderer itself (03:40): ImGui's legacy font
  atlas as a GL texture, the draw lists through one small shader on the same chain. Minus + R3 opens the panel; its
  Display section has WIDE RENDERING (`render_wide`, 03:50).
- `render_wide` (off by default, a picture change): the targets at the output's full width too, so MP2's anamorphic
  16:9 frame is drawn at 1280x720 instead of 960x720 and stretched. 29–30 fps in Ryujinx either way.
- Unchanged GL state is skipped and each draw's uploads are reserved in one ring storage (03:10, see below).
- Switch: EGL window surface 1280x720 on the default NWindow (720p, the user's target; the system scales it when
  docked), desktop GL 4.3 core, vsync. PC: `MP2_GL=1` (WGL 4.3 core on the game window).

Checked on the PC (RTX A2000, 2026-11-22 01:10–01:40): boot → menus → New Game → chapter 1 at a locked 30 fps, no GL
errors. Against the Vulkan renderer, same script, same seconds: the static scene is pixel-identical; Max and his shadow
on the table differ. RenderDoc on both (`build/rdc/{vk,gl}_gameplay.rdc`, `tools/rdc_state.py`, `rdc_save.py`,
`rdc_vsout.py`): the draw state of all 145 draws matches, the shadow texture is identical, the first draw that differs
is Max's head (draw 39), and in it the camera, view and light constants are equal but the bone matrices differ — the
game uploaded another animation pose (GL presents on the title thread, so the game's clock steps differently). Two
Vulkan runs match each other (265 small pixel differences). So the GL renderer draws the same input the same way.

Checked in Ryujinx 1.3.3 (2026-11-22 01:55–02:30, `build/emu_runs/gl1`–`gl8`): Mesa 20.1.0-rc3, GL 4.3 core on
nouveau NV120. Intro, menus, New Game, chapter 1, the game's own timing line at 29.5–30 fps throughout gameplay
(`game` 10.5–11 ms, `present` about 1 ms a frame); Ryujinx's status bar "30.00 FPS (33.34 ms)"; the picture in
Ryujinx's window (`gl8/window_125s.png`) is the full 16:9 chapter 1 frame, as on the PC. The soft renderer gave 2–4 fps
there.
- Emulator quirk, not the port: every read of a rendered frame back into guest memory (the `capture` shots,
  `glReadPixels`) comes back a quarter wide under Ryujinx — even a plain `glClear` of an offscreen target reads back
  64 of 256 pixels (`gl_flags.txt` `selftest`, `gl4`), with the scissor off and either clip origin — while Ryujinx
  displays the full frame. The port's own captures are therefore useless in Ryujinx with this renderer:
  `tools/run_ryujinx.ps1 -WindowShots "40,110"` saves Ryujinx's window instead. On the console the read-backs need
  checking (the game's own Sync read-backs use the same path); a console run is the user's call.
- 03:10 Removed work, no picture change: the GL renderer skips texture, sampler, viewport, scissor, uniform range,
  vertex format, blend colour and stencil calls whose value has not changed, and reuses the game's vertex and draw
  constants while equal. Chapter 1, counted per frame (`gl: present` log line): 385 state calls made, 1412 skipped,
  1.1 MB uploaded (PC and Ryujinx alike; the static scene identical to before on the PC). Fixed with it: a draw
  whose uploads straddled a ring buffer's orphaning read the earlier ones from the new storage (a garbage draw
  about once a ring wrap); each draw now reserves its uploads first.
- Never pipe `run_ryujinx.ps1` into `Select-Object -First N`: it stops the script before it copies the logs and puts
  the SD back (what happened to an earlier run's logs, and to `gl3`'s until it was tidied by hand).

## Fixes made to the kit and SDK (also handed off to the base)

| Fix | Where | Found by |
| --- | --- | --- |
| 16-bit `pusha`/`popa` | `tools/recomp/codegen_int.cpp` | MP2's CPUID probe 0x1D33C6 trapped |
| BinkDoFrame's hook called guest 0 when `BinkWait` is not in the config | `runtime/xdk/bink.cpp` | crash at the first film |
| CPU renderer read empty vertices on the raw vertex path | `runtime/gpu/vertex.*`, `harness/reference_renderer.cpp` | null read on the menu's first indexed draw (PC with `MP2_SOFT=1`, then Ryujinx) |
| ImGui 1.92 init struct, glslang < 12 resource limits / SPIRV library | `host-switch/vulkan.cpp`, `vk_shader.cpp`, CMake | the local third-party copies |
| No ninja in msys2: `make` fallback; quoted paths with spaces | `ferramentas/build_switch.sh` | the Switch build |
| GCC 16 libgcc's GCS/SME sysreg reads stop Ryujinx's JIT | `tools/patch_gcs.py` (run by `build_switch.sh`) | "Unknown MRS 0xD53B2521" |
| VS 18 / standalone LLVM, no FetchContent | `ferramentas/recompilar.ps1`, `recompilador/CMakeLists.txt`, `build_pc.bat` | this PC |

## Defaults that change the picture (off; configs for the user)

- `oled_screen`: the base defaulted to AUTO (lifts the darks on an OLED console in handheld). MP2-nx: 1 (off). AUTO also
  asks `set:sys` for the product model, which Ryujinx does not implement (it stopped the emulator).
- `screen.toml` `handheld_gamma`: the base lifted the handheld shadows by an exponent of 0.8. MP2-nx: 1.0 (off).
- `brightness` 5 is the identity ramp (kept).
- Resolution and aspect (the user's target: 1280x720, 30 fps locked): the guest frame is 640x480 (the game's own
  back buffer); the output is 1280x720 on both builds with a 30 fps cap (`fps_cap = 30`). The kit tells the game the
  TV is widescreen whenever the output is 3:2 or wider (`app::Widescreen()`, Xbox video flag 0x10000), and Max Payne 2
  supports it: told widescreen it draws anamorphic 16:9 frames (tested on the PC: `MP2_43=1` makes it 4:3 and the
  controls page then fills the 640 width instead of the middle; `tools/scripts/mp2_aspect.txt`). The present stretches
  that frame to the full 1280x720 — the picture an Xbox set to widescreen sends to a 16:9 TV, i.e. one of the game's
  own modes, kept as the default because it is the base's behaviour and the user's target. 4:3 (pillarboxed) is the
  other faithful mode: a config for the user (today only the PC test switch `MP2_43`).
  The Vulkan renderer draws the guest targets at the output height (render scale 1.5: 960x720 for the 640x480
  frame, stretched to 1280x720); the soft renderer draws at 640x480 and scales.

## Open problems

- ~~Main menu navigation~~ (solved 00:05): the test script's extra A had opened "How to Play"; the menu works.
- ~~Intro films black~~ **solved 03:30** (see the last bullet). Was: the Bink films (`star_n.bik`, the chapter films) play — sound, BinkDoFrame,
  BinkCopyToBuffer — but the picture stays black. RenderDoc (`build/rdc/mp2_frame84.rdc`, a film frame) shows only a
  clear and the present: the film is never drawn. Bink copies each frame into surface 0x004D6040, whose memory overlaps
  the depth buffer's (0x03C6C000 physical); the game's own display path for that surface (a texture draw or the video
  overlay) is not reached. Findings (02:45–03:00):
  - Not the video overlay: no PVIDEO register offset (0x8900–0x8958, 0x8700) is written anywhere in the recompiled
    code (`EnableOverlay = 0` in the config is simply unused).
  - The film loop is game function 0xFDF70: BinkWait, BinkDoFrame, 0x72310 (lock, via the engine's virtual
    renderer), 0x2C0AD0 (the copy into the frame), 0x71DE0, BinkNextFrame, then engine calls 0x72830, 0x71FC0,
    0x8D9D0, 0x72080, 0x72860 — mostly indirect (virtual) calls, so the display step is not visible statically.
  - The only write to the display's scan-out start (PCRTC_START, `[reg+0x600800]`) is D3D's own `sub_00290657`,
    called by D3D internals `sub_00292D90` / `sub_00292EB0` (Swap / v-blank flip). If the engine hands the film
    surface to D3D's flip (as the front buffer) instead of drawing it, the console scans it out directly and the
    kit's present, which shows its own back buffer, never sees it.
  - Solved (03:20–03:30): a trace of every hooked call (`runtime/xdk/hle.h`, on during the PC build's one-frame
    `trace`) showed no draw between the frame's decode and its Swap; a dump of the frame's pushed commands showed
    the engine's inlined DrawVerticesUP: an inline pixel shader (combiner methods), SET_BEGIN_END quads, the four
    vertices as an INLINE_ARRAY (0x1818) and END, pushed straight into the push buffer. The kit's `Walk` read
    begin/end and per-vertex data but not inline arrays, and ran only at the next push buffer request (after the
    present). `Walk` now draws inline arrays (as `EndPush` does) and runs at the start of each Swap. The intro film
    plays on the PC; in Ryujinx each film frame now draws its quad (before: 0 draws); chapter 1 unchanged. Film
    frames cost about 33 ms of game time in Ryujinx (the frame uploaded as a texture each time): to watch on the
    console.
- **Soft renderer picture** (Switch bring-up path only): lightmaps white and no near clipping in the 3D world;
  menus, graphic novel and HUD right. Not worth fixing now that the GL renderer exists.

## Full testing (2026-11-22 morning, the user: "full testing, using all resources available")

### The console: blocked by its file server

- No other port session was running (ListAgents), so the console was free. sys-botbase answered (version 2.5, the
  Home menu, title `010091D01597C000` = "Arkanoid - Eternal Battle", the full-memory hbmenu tile SSX's notes name).
  **sys-ftpd-light refused every connection on TCP 5000** (also 21 and 28280, probed for a minute). Its pause combos
  from the SD backup on this PC (`PLUS+MINUS+X` for sys-ftpd, `ZR+R+B` for sys-ftpd-10k), each pressed once on the
  Home menu, did not bring it back. A reboot was not tried: it can stop at the boot loader's menu, which only the
  owner can pass.
- Second route, through the same tool: hbmenu's netloader (hbmenu started from the Arkanoid tile with R held, the
  title checked, Y). `switch_remote.py netload` speaks nxlink's protocol and always names the file
  `mp2-nx/<file>` under hbmenu's `/switch`. hbmenu answered `open: err=2 No such file or directory`: **the port's
  folder `sdmc:/switch/mp2-nx` does not exist and hbmenu does not make folders**, so nothing was written. hbmenu was
  closed (HOME, X, A) and the console left on the Home menu as found.
- Ready for when the folder exists (the owner makes it, or sys-ftpd-light is back after a restart):
  `tools/console_recv` (`mp2-nx-recv.nro`, built from libnx's template), a receiver that runs in the foreground,
  listens on TCP 5005 and reads and writes only under `sdmc:/switch/mp2-nx` (no delete); `switch_remote.py netload`
  starts it, and `rput-tree` / `rput` / `rget` / `rls` / `rquit` drive it (tested against a loopback stand-in:
  the four films uploaded and read back byte for byte, paths outside the folder refused). With sys-ftpd-light back,
  the plain `put-tree` (new) uploads the game folder (1.7 GB, 429 files).

### The console, once the folder existed (08:40–)

- The user made `sdmc:/switch/mp2-nx` (with DBI over USB from this PC; DBI's screen showed its MTP session, so no
  button was pressed while it was open: the Home menu came back when the user closed it). sys-ftpd-light was still
  down, so the receiver route: hbmenu (Arkanoid tile + R, title `010091D01597C000` checked), Y, `netload`. hbmenu saved
  it as `sdmc:/switch/mp2-nx/mp2-nx-recv.nro` (the receiver prints the path it started from) and started it.
- The receiver's first LIST sent the count and no lines: `rewinddir` does not rewind on the console's SD. Fixed (one
  pass into a buffer); `rquit`, Y, `netload` again. Uploads run at about 2.4 MB/s over Wi-Fi (the NRO, 25.9 MB, in
  11 s); the game folder (1.7 GB, 429 files) in about 12 minutes.
- Decision (the user: work on without asking until the game is done): the console runs go in this order: (1) New
  Game into chapter 1 with the port's captures and the GL self-test; (2) `render_wide` A/B on the save that run
  leaves (same NRO, same session, A then B); (3) the heaviest levels from Ryujinx through `test_level`. Logs are
  read through the receiver after each run.

### Reaching every level: `test_level` (a test aid, never in the shipped settings)

- `game\assets.txt` names the 30 levels and their folders (`03_First_Hospital` = `levels\01` ...
  `21_The_Manor_B_split2` = `levels\30`). Swapping only the folder (a file-layer remap) crashed at the load: the
  level's data follows its name. Swapping the name at the level init (`sub_00023B05`, a game-mode method) loads the
  level but crashes on the rest of New Game's stream: New Game is a load from a stream (`sub_000254AE`: the level's
  name, then its state, `MP_GM_LOAD1..6`) made for 03_First_Hospital (levels 2, 13 and 14: the same memmove of
  0x3F3504F0 bytes, a float read as a string length).
- What works: the game's own developer script commands. `sub_0001B917` runs one script line on the game mode (its
  cheat routine `0001B952` runs `player->C_SetInfiniteAmmo(...)` lines through it), and the game mode's update,
  `sub_0001EFE2`, runs once a frame. `src/game/test_level.cpp` hooks the update: 90 updates after the first level
  started it runs `maxpayne_gamemode->GM_InitSingleLevel(<level NN>);` once (`test_level_command` names another
  command). Level 2 (04_Warehouse): its graphic novel, its in-engine film and play, 30 fps on the PC. The same file
  logs every level init (`game: level init <name>`), in every build.
- `tools/run_level_tests.ps1` runs levels in turn on the PC build with `tools/scripts/mp2_level_test.txt` and writes
  `build/level_tests/summary.txt` and a contact sheet per level (`tools/contact_sheet.py`).

- **The ending** (PC, `test_script = "maxpayne_gamemode->GM_SendEndOfGameMessages();"`, a second test key: one
  raw script line run at the same moment; `tools/scripts/mp2_ending.txt` presses R through it): the game plays the
  final graphic novel and the closing in-engine scene, then (the game not really at its end) returns to play in the
  manor. The credits film (`cred_n.bik`) is not reached that way; it plays through the same Bink path as the intro
  films, which play.

- **Dead Man Walking** (the survival maps, `assets\sv`): 01, 03 and 05 through `test_script =
  "maxpayne_gamemode->GM_InitSingleLevel(0N_Survival);"` on the PC: each loads (its own loading screen) and plays at
  30 fps, no crash.

### Levels in Ryujinx (the Switch NRO, GL on switch-mesa; 07:05–08:00)

Eleven levels, each 230 s through `tools/scripts/mp2_level_test_switch.txt` (New Game, the change to the level, R
through its graphic novel, then walking, firing and bullet time until Max is shot). These ran while the PC batch ran
on the same PC, so the times are rough; the emulator is a proxy anyway, and no run is compared with another for speed.
Every one loaded its level, played, and showed the right picture in Ryujinx's own window (the display was on again);
no crash. In play (the `d3d: Swap` lines from 120 s on):

| Level | fps | game ms a frame |
| --- | --- | --- |
| 5 07_Maxs_Apartment_split1 | 28.9–30 | 11–16 |
| 8 08_First_Address_Unknown | 29.2–29.6 | 7.6–10.3 |
| 11 10_Police_Station | 29.5–30 | 8.1–15.9 |
| 13 12_Condemned_Building_SP1 | 29.2–29.9 | 9.3–14.7 |
| 16 13_Construction_Site (Mona) | 28.1–29.8 | 14.2–15.2 |
| 19 14_Second_Dream (its blur) | 29.0–29.9 | 11.5–14.2 |
| 20 15_Second_Hospital | 29.4–30.1 | 9.7–13.0 |
| 22 17_Crackhouse_split1 | 28.5–30 | 10.1–16.6 |
| 24 18_Gognittis_Lot | 29.0–29.8 | 13.1–15.4 |
| 27 21_The_Manor_Split1 | 28.8–29.7 | 10.8–20.9 |
| 30 21_The_Manor_B_split2 | 29.3–30.1 | 8.1–8.6 |

Chapter 1 in Ryujinx (04:15, the PC otherwise idle) was game 10.7 ms with the renderer 5.5 ms of it. Later levels
cost more of the game's own time; the console's figures decide whether any of them misses 30 fps.

- PC, all 29 levels (2–30, OpenGL renderer, 140 s each, 07:05–08:13): each loaded, played and held 30 fps with
  0–1 frames over 40 ms per 300; no crash; only the two start-up notices every run logs (an empty launch request
  refused at 0.06 s, DirectSound's original-code exports). Contact sheets `build/level_tests/lNN.png`.
- The OpenGL renderer against the kit's Vulkan one on levels 19 and 27 (PC): the same picture wherever the game
  state matches (the dream's blur included); where the scripted camera had turned differently, different views.
- In Ryujinx about one frame a second is over 40 ms in play (9–13 per 300), in chapter 1 at 04:15 as in the later
  levels; the PC shows none. The `d3d: Swap` line now says how many of those had the game's part alone over 30 ms
  and the longest game part, and the `gl: present` line counts the game's read-backs: in Ryujinx 9 of 10 long
  frames are game time (38–52 ms), with 0 read-backs, steady uploads (1.2 MB a frame) and the stall logging gone.
  Not the port's read-backs or logging; most likely the emulator (a .NET pause, say). The console's figures decide.
  The PC build with the same line (12:00, chapter 1): game 6.0–7.4 ms, the longest game part 10–12 ms per 300
  frames, none over 30 ms after the load. So the game itself has no once-a-second spike.

### Where the frame's CPU time goes: a sampling profiler (PC, 12:00–12:20)

`host-pc/profiler.cpp` (PC build only, `MP2_PROF=START:SECONDS`): about every 2 ms it stops each thread that used
the CPU since its last sample, walks its stack (the recompiled code's host frames follow the guest's calls) and lets
it go. A thread stopped inside a wait or sleep call, or one that used no CPU, does not count. At the end it writes
`logs/prof.txt`: per thread how often it was working, functions by self and inclusive time, and the commonest call
chains of the top functions (names from the exe's PDB through DbgHelp). Nothing is allocated while a thread is
stopped.

Chapter 1 and level 27 (OpenGL, 50–60 s of play each, `build/prof_ch1.txt`, `build/prof_l27.txt`, not in git):
- The title's thread works 22% of the time (about 7.3 ms of each 33 ms frame); every other thread is under 7%.
- The game's recompiled code is about 69% of that, spread thin. The hottest function, `sub_000AE7B0` (a loop of
  3x4 matrix products in x87 code, under the render walk `sub_00106600`), is 5–8%; then a long tail of 1–2% each.
  No single hook would change much. The generated code already drops unused flags (they are never stored before a
  call) and declares `ctx` `__restrict` (guest stores do not force register reloads), so there is no cheap
  across-the-board gain in the generated code either.
- The OpenGL renderer is about 20–25%. Texture signatures (`gpu::Hash`, each texture hashed the first time a frame
  uses it, whole up to 64 KB, sampled above) 4–5%; vertex assembly (`gpu::Assemble`) 3–8%; the rest is the PC's GL
  driver. Content hashing is the renderer's only way to see the game's CPU writes to textures (`MarkDirty` covers
  copies only), so hashing less would no longer be pixel-identical.
- PC-only noise: the test-command file checked on every input poll, the trace and shot trigger files on every
  present (3–4%). The Switch build has none of these per frame (its capture check runs once).
- Ryujinx (chapter 1): 87 draws a frame, 1.18 MB uploaded, renderer 5.0–5.6 ms of game 10.1 ms. Mesa runs there as
  JIT-ed ARM code, so its share is more like the console's than the PC driver's is.

### Films: the colour conversion native and bit-exact, the film texture's upload trimmed (12:25–13:10)

The profiler on the intro film (PC): one function, `sub_002CC910` in the Bink library, was **83% of the title
thread** (about 10 ms of each film frame; the film frame cost 11.3 ms of game time against chapter 1's 6.6). It
converts two rows of 4:2:0 YUV to 32-bit pixels with MMX, which the recompiler turns into per-lane loops through
`memcpy`. In Ryujinx a film frame was about 33 ms of game time, so on the console the films could fall behind.

- `src/game/native_bink.cpp` replaces it: the same 16-bit arithmetic (psubusw, psllw, pmulhw, paddw, paddsw, the
  unpacks and shifts), four lanes at a time in GCC vector types (SSE on the PC, NEON on the Switch). It leaves
  everything as the original does: the six globals (the rows' destinations left at each row's end, the Y, U and V
  pointers), both destination rows, the four pushes below the return address, the registers (eax, edx, the kept
  ones, esp past the argument) and mm0–mm7 as its last step leaves them.
- Checked with `test_shadow = true` (a hidden settings key, never saved): every call runs both versions from the
  same state, compares all of the above and keeps the original's result. 65,536 calls of the intro film and 32,768
  in a film-plus-chapter-1 run: **0 mismatches**. `test_native = false` runs only the originals (for a timed A/B on
  the console in one session).
- What was left of a film frame was the renderer re-uploading the film texture (640x480 linear A8R8G8B8) each frame:
  decoded through `Texel`'s switch per texel into a new buffer, a second buffer zeroed and the rows copied into it,
  then `glTexSubImage2D`. Now the kit's `DecodeLevel` copies 32-bit colour formats a row at a time (alpha 255 for
  the X8 formats, the same bytes; handed off, row 19), and `UploadTexture` keeps its buffers and uploads straight
  from the decoded rows when they are already packed (every uncompressed 2D level; volumes still start from zeroed
  buffers, as their linear, depth and YUY2 paths fill only the first slice). Checked by hashing every upload: the
  first 345 uploads (logos and films) are identical with and without these changes.
- PC intro film, game time a frame: **11.3 ms -> 5.4 ms** (conversion native) **-> 4.4 ms** (upload trimmed); the
  title thread is busy 15% of the frame instead of 38%. Chapter 1 unchanged (30 fps, the same picture).

What kind of code the game's time is in (level 27's profile, `build/prof_l27.txt`): the top 40 game functions make
31% of the self time; about a third of their instructions are x87 (most of the hot ones are 40–80% x87: matrix,
vector and animation code), MMX/SSE 4%. Each x87 instruction is recompiled as a load and store through the 8-entry
stack array at a computed index plus the precision rounding, several host instructions for one. So if the console
shows the game's own code as the limit, the lever is the recompiler: an x87 stack tracked statically within a
function (locals instead of `ctx.st[(top + i) & 7]`, flushed around calls), not more single-function hooks. That
means regenerating and rebuilding all the game's code and checking it widely, so it waits for the console's figures.

For comparison, the base's Cold Fear port logged game 15–29 ms a frame on the console (its HANDOFF, 0.1.36). MP2's
6–7 ms on the PC may land near the 33 ms budget there. The console run decides what to work on: the game's code
(then native versions of its hottest functions, checked bit for bit against the recompiled ones), the renderer, or
the GPU.

### The wait stall report held the dispatcher lock (kit fix, handed off)

The kit's stall report (`runtime/kernel/objects.cpp`) counted its "seconds" in wakes of the shared condition
variable, which every signal of any object notifies: MP2's idle worker (it waits about 2 s at a time) logged
"blocked 2/4/8/16 s" and a 32-entry stack trace within a second, every wait: about 1,250 lines in a 4-minute Ryujinx
run, all written with `g_dispatch` held. On the Switch each log line is a write and a flush to the card, twice (the
log and stderr), so every such burst could stall any thread that waits or signals, the title's included. Now real
time, the first report at 4 s and then doubling, the stack trace once per thread, and nothing logged with the lock
held: PC 1,507 -> 25 lines in 140 s; Ryujinx 1,256 -> 31 lines in 240 s; the levels play as before. Its effect on the
frame rate is not measured (the emulator's one long frame a second stayed); the console will show it.

### The NRO's icon

The user's picture (the game's cover art, 2026-11-22) as `exemplos/mp2/icon.jpg` (256x256 JPEG, made from their
1254x1254 PNG): the Switch build embeds it as the NRO's icon (hbmenu's tile). It is the user's file for their own
build, so it stays out of git (`.gitignore`), like the game's files.

## The console (2026-11-22 afternoon) and 60 fps

### First real figures (run 2, 14:32, NRO 1F887E42, the shipped settings, 30 fps cap)

- Boot to the legal screen about 1 minute on this first run (shaders compiled, files first read), menus, New Game, the
  graphic novel and chapter 1 by the input script: every picture right (screenshots `out/console/r2_*.jpg`). The GL
  self-test read back full rows (Ryujinx's quarter rows were the emulator). Exit through HOME, X, A: `Exit (0)`.
- Chapter 1 steady (44 log rows): 30.0 fps; `game` 19.2 ms a frame, of it the GL renderer 9.3 ms; `present` 1.4 ms;
  pace 12.6 ms. So the recompiled game itself is about 11.3 ms and the frame's CPU on the title's thread about
  20.6 ms. About 8 frames in 300 go over 40 ms, each with the game part alone over 30 ms (longest 52-67 ms): the
  once-a-second long frame Ryujinx showed is the console's too, not the emulator's (the PC shows none).
- Against Ryujinx on this PC (chapter 1: game 10.7, renderer 5.5 ms) the console is about 1.8 times slower; the
  films' native conversion 260 ms per 8192 calls (Ryujinx 33): about 8.7 ms a film frame, well inside a film's own
  rate.
- Run 1 (14:22) with `capture` on showed why a capture file must not be on the card for a timed run: each shot took
  10-20 s to write row by row and the game made 3 frames in 55 s. Fixed (one write; the file's text "0" turns it off).

### 60 fps (the user's target from 14:30: "60 fps locked")

- How the frame rate is set: the game presents with `D3DPRESENT_INTERVAL_IMMEDIATE` (CreateDevice's interval
  80000000) and times itself by `rdtsc` (read in eight of the generated files; the runtime returns real time at
  733.33 MHz, `cpu/dispatch.cpp`) and `KeQuerySystemTime`. So the 30 fps is the port's own pacer (`fps_cap`,
  `app::Pace`), and at 60 the game runs at the same speed, as the PC version of the game does at any rate.
  PC, level 27 (the heaviest) with `fps_cap = 60`: 60.0 fps held, game 3.5 ms, renderer 1.1 ms.
- The console's budget at 60 is 16.7 ms a frame; chapter 1 needs about 20.6 ms of CPU at the system's 1020 MHz and the
  later levels more (Ryujinx: level 27 up to twice chapter 1's game time). The levers, largest first:
  1. The CPU clock: `cpu_clock = 1785` (`host-switch/clocks.cpp`, the COLD-FEAR base's clock code ported: clkrst or
     pcv, old rates back at exit, a keeper thread re-applies every second in focus). Not a system setting: the clock
     for this run only. Decision: on in the 60 fps settings, the settings file says so; more heat and battery.
  2. The renderer's 9.3 ms (Mesa's nouveau driver per draw, texture hashing, vertex assembly): the `gl: renderer
     parts` line splits it (assembly, textures, GL calls, the rest) and `gl: long frame` lines show long frames.
  3. The once-a-second long frame (game part over 30 ms): a visible hitch at 60.
  4. The recompiled game's own 11.3 ms: x87 emulation (about a third of the hot functions' instructions), the
     recompiler's static x87 stack tracking (a full regeneration).

### What the console runs found (runs 3-6, 14:52-15:45)

| Run | NRO | Change | Chapter 1 |
| --- | --- | --- | --- |
| 2 | 1F887E42 | 30 fps cap, stock clocks | 30.0 fps, game 19.2 ms, renderer 9.3, ~8 frames over 40 ms per 300 |
| 3 | 5E7144E2 | 60 cap, `cpu_clock = 1785` | 47.9 fps, game 19.4 ms: no faster |
| 4 | 6B25131F | persistent upload rings | 52.2 fps, game 18.4, renderer 8.1, 0-2 over 40 ms per 300 |
| 6 | DE4CE4D9 | the clock keeper able to run | **59.6-59.7 fps, game 12.0 ms**, renderer 5.1, about 1 over 40 ms per 300 |

- The once-a-second long frame was Mesa reallocating the orphaned 32 MB stream buffer every 30-40 frames (15-40 ms in
  the renderer's "rest"). Persistent mapped rings with four fenced segments end it; no fence wait was ever logged.
- The clock: clkrst accepted 1785 MHz (the boot check measured it), but the system put the CPU back to 1020 MHz
  every second for the first ~8 s and again at ~20 s, and the base's keeper thread at priority 0x3F never ran while the
  game played. At 0x20 it restores the clock each time; the census then measures 1781-1786 MHz for the rest of the run
  (the clock timed by the keeper's own CPU ticks, so a busy core cannot skew it; when it reads under 90% of the asked
  rate it sets it again).
- The thread census (every 10 s): the title's thread takes 81% of a core in chapter 1 at 60 fps, every other guest
  thread under 5%. Guest threads already have cores 0-2 in their mask (preferred core -1), so `thread_cores` changes
  nothing and stays 0.
- The remaining long frame every 300 frames was the log: on the Switch every line was flushed and written again to the
  unbuffered `stderr.log`, two SD writes a line, and the title's thread logs a few lines every 300 frames. Now the log
  gathers in 64 KB, warnings and errors flush at once (a crash logs at error level), and the host's 5 s heartbeat
  flushes the rest.

### The clock is not the port's to keep (run 7, 15:50)

Run 7 (the log fix; the same settings as run 6) never got its 1785 MHz: the census measured 1017-1020 MHz every 10 s
for 7 minutes although the keeper set it again each time. The console runs Horizon-OC (a sys-clk derivative:
`atmosphere/contents/00FF0000636C6BFF`, `config/horizon-oc` in the user's package on this PC), which has a CPU governor
and a thermal throttle limit and applies its own per-title profiles; in run 6 it let the clock stand, in run 7 (the
console warmer, 42 C on hbmenu's bar) it did not. Its config lives outside `sdmc:/switch/mp2-nx`, so it is the user's.
**Decision:** 60 fps must not depend on the overclock. `cpu_clock` stays a setting (0 by default); the target is the
system's 1020 MHz, where chapter 1 needed about 17.7 ms of the title's thread a frame (run 7: the game's own code about
9.6 ms, the GL renderer 7.35: assembly 1.0, texture signatures 1.25, Mesa's GL calls 2.7, the rest 2.4; present 0.7).
For the user, optionally: a Horizon-OC profile `sdmc:/config/horizon-oc/profiles/010091D01597C000.ini` with
`[values]`, `handheld_cpu=1785`, `docked_cpu=1785` lets the clock manager itself hold the clock for hbmenu's title (it
applies to that title's own game as well).

### Removing work at 1020 MHz (16:00-)

- Texture signatures: a four-lane version of the hash (`SigHash`, the same 64 x 64 -> 128 mix, independent lanes
  folded at the end); signatures are compared within a run only, never stored. PC profile: the hashing 8.1% -> 6.2% of
  the title's thread.
- Index lists (`gpu/vertex.cpp` `ListIndices`): sized once and written in place instead of an `insert` of an
  initialiser list per primitive; the same indices in the same order. PC: vertex assembly 8.6% -> 4.7% inclusive.
- **x87 in the recompiler: a block cache** (`tools/recomp/codegen_fp.cpp`, `codegen_emit.cpp`). From a block's first
  x87 instruction `x87t` holds `ctx.top` and the slots `x87t + 0..7` live in locals `x87r0..7`, loaded on first read;
  the generator tracks the depth, so every `st(i)` is a fixed local the compiler keeps in a register, and `x87f` holds
  the precision bit. Labels, jumps, calls, returns, hooks and the instructions that read the stack another way (fxam,
  fsincos, fptan, fxtract, fninit, fincstp/fdecstp, fnsave/frstor, fnstenv/fldenv) write the dirty slots and
  `ctx.top` back first; fnstsw computes TOP from the tracked depth, fldcw updates `x87f`. Each operation computes what
  it did through `ctx.st` (the same double arithmetic and the same rounding to float when the precision bits ask);
  only where the values wait between operations changes. `RECOMP_X87_CACHE=0` when regenerating turns it off. The
  previous generated code is kept in `build/generated_nocache` for an A/B.
- On the console the x87 cache changed little (run 8: chapter 1 55.8 fps, game 17.2 ms against 54.6 / 17.0 in run 7):
  GCC on the A57 had kept much of the old code's stack traffic cheap. A console profile was needed.

### The console profile (run 9, 16:40; `host-switch/prof.cpp` from the COLD-FEAR base's profiler)

292,000 samples of chapter 1 at 1020 MHz, all on the title's thread. `GlBackend::Draw` and below: 32.7% of it (Mesa's
draw path about 9%, its validation 5%, vertex assembly 5.4%, texture signatures 3.6%, the copies into the ring 3.6%);
the game's own code spread thin (the top functions about 1.4% each); the present's window buffer IPC about 1.9%.
Fixed from it (exact): `x86_icall` cached per thread (1.7%), texture signatures in CRC32C lanes (3.6%), the 3 KB
vertex-constant comparison skipped while `State::vsc_gen` has not moved (1.3%), the last draw constants in a fixed
buffer (0.9%), the index range scan vectorised. Console run 11: chapter 1 at 57.3 fps, game 16.8 ms (the trend at
1020 MHz: 54.6 -> 55.8 -> 56.1 -> 57.3 fps).

### Level 27 and the decision: adaptive 60 (17:20-18:00)

- Run 12 (level 27, the heaviest, 60 cap, the clock held at 1020 by the console's Horizon-OC): about 40 fps, game 24.3
  ms a frame (renderer 9.7 ms, 157 draws, 2.7 MB uploaded a frame); the picture right. At 1785 MHz it would be about
  17 ms: still not a lock. So 60 fps locked everywhere is not reachable on this console by removing work at this
  scale (the renderer would have to go to the GPU's own API, deko3d, to drop Mesa's share; a project of its own).
- **Decision (the user's "60 fps locked"):** `fps_cap = 60` with **`fps_adaptive = 1`** (`host-switch/main.cpp`
  `app::Pace`): 60 where the console holds it, locked to 30 where it cannot (the title's thread's CPU time a frame
  over 16 ms on average for half a second), back to 60 when it has stayed under 13 ms for 3 s. Never the uneven
  40-57 fps vsync would show. With vsync the swap interval paces (1 at 60, 2 at 30; `gl_backend.cpp` `SwapWindow`)
  and the pacer adds no timer: a timer on top of vsync at 30 made frames of 40-50 ms (run 13). The game times itself
  by the clock, so its speed does not change between the two. `cpu_clock = 1785` in the shipped settings (the
  user: "use all resources necessary"; the clock for this run only, the old one back at exit; Horizon-OC's thermal
  limit still wins when it throttles); `mp2-nx_30.toml` for 30 at 1020 MHz (cooler, longer on battery).
- Runs 13/14 (level 27 through Resume Game, the shipped settings): menus at 60, the level locked to 30 within half a
  second; with the swap interval, 30.0 fps and no frame over 40 ms once the heavy fight was over (5-7 per 300 frames
  during it, frames over 33 ms of work). Run 14 also showed the first adaptive measure (wall time between Pace calls)
  could not come back to 60 once the swap waited inside it: now the thread's CPU time.

## The deko3d renderer (2026-11-22 evening, the user: "Try deko3d")

Why: at level 27 the GL renderer took 9.7 ms of the title's thread a frame (run 12), 3.2 ms of it inside Mesa's
driver ("GL calls") for about 160 draws; in chapter 1, 2.9 of 7.2 ms. deko3d, the Switch GPU's own API in libnx, records
a draw as a few words in a command buffer: that share should mostly go.

### Decisions

- **`PORT_RENDERER=dk`, `host-switch/dk_backend.cpp`**: `gl_backend.cpp` draw for draw, its GL calls replaced by deko3d
  commands. The console-state helpers (constants, keys, signatures, the CPU write latch) are the GL renderer's own
  block, copied verbatim; everything below it (memory, shaders, state, barriers, present) is new. The GL renderer stays
  the shipped one until the deko3d one has been checked on the console (A/B, same session).
- **Shaders on the console, by uam**: the same HLSL → glslang → SPIR-V (`vk_shader.cpp`) → SPIRV-Cross → GLSL 4.30
  (`gl_shader.cpp`, each stage's textures numbered from 0) as the GL renderer; then uam, devkitPro's deko3d shader
  compiler, as a library: `third_party/uam`, copied from the ultimate base (`renut-ultimate/base/app/thirdparty/uam`,
  uam 1.1.0 with the SwitchWakerHD / banjo-nx fixes: deterministic scheduling, no dual issue, the stall before a new
  block, no exit() on errors). None of banjo-nx's optional code generation passes (`setBanjoFlags(0)`): the plain
  compiler, for the picture. uam runs on a thread of its own with 8 MB of stack (Mesa's GLSL front end recurses
  deeply); the title's thread waits for each compile, as it waited for Mesa's. Results kept in
  `shadercache/<hash>_dk1.bin` (the texture pairs and the DKSH): a second run compiles nothing. The probe of the
  morning (`tools/deko3d_shader_probe.py`, Decision 4b) had already put all 15 shader kinds through uam.
- **Conventions**: deko3d's device defaults are Vulkan's (depth 0 to 1, origin upper left), so the viewport and the
  front face are `vulkan.cpp`'s: a negative viewport height turns clip space the D3D way up, `frontFace = ccw ? CCW :
  CW`. `dk_flags.txt` `faceswap` flips the winding for a check.
- **Formats**: colour targets BGRA8 (the guest's byte order: a read-back is guest memory as it is, an upload is
  `DecodeLevel`'s output as it is), depth Z24S8; DXT1/3/5 native (BC1-3), volume DXT decoded on the CPU as the GL
  renderer does (the same texels), R32F native.
- **Depth bias**: `dkCmdBufSetDepthBias` doubles its constant (disassembled: `fadd s8, s8, s8`), as nouveau doubles
  GL's polygon offset units, so the GL renderer's values go in unchanged.
- **Memory**: rings in CPU-written memory for vertices, indices and texture uploads (64 MB) and constants (16 MB),
  4 fenced segments each as the GL renderer's persistent rings; an image heap (64 MB chunks, first fit, frees deferred
  until the frame that freed has finished); 32 MB of shader code; 3 frame slots of command memory with a fence each.
- **Barriers** (what Mesa did for the GL renderer): an image drawn and then sampled waits for the fragments and drops
  the texture cache; a copy (the copy engine: uploads, CopyRects, the feedback copy, read-backs) waits for everything
  before and after; a target sampled and then drawn into waits for the fragments. Tracked per image with stamps (the
  hazard barriers so far); one barrier before a draw however many of its images need one.
- **Present**: three 1280x720 swapchain images; the game's commands are sent before a swapchain image is waited for,
  so the GPU works on the frame meanwhile; the swap interval follows the frame cap (2 at 30, 1 at 60), as the GL
  renderer's.
- **Log lines** (`dk:`): the GL renderer's figures (`log_summary.py` reads both), "commands" in place of "GL calls",
  plus the barriers a frame, the GPU's busy time a frame (timestamps around the game's commands and the present's,
  without the wait for a swapchain image), the waits in the present, the shaders compiled or loaded from the cache,
  and the textures changed without a lock.

### What Ryujinx and the console showed (19:50-22:20)

- Ryujinx (dk2-dk8): the renderer ran first time (clears, scissored clears, read-backs); the first picture had the
  floors, far walls and the HUD culled and Max seen from inside, then the railings speckled: deko3d 0.5.0's **tile
  height bug** (SwitchWakerHD's fix, in the ultimate base's plume layer too: a custom tile size where the layout's
  shrunk tile and the engines' differ; uncompressed images with the 2D engine flag). After it, chapter 1 and level 27
  drew as the GL renderer does, at 60 fps (GL: 29 in the emulator), and the port's own captures worked there (GL's
  read-backs had come back a quarter wide in Ryujinx).
- Console run dk1: the game ran (2,100 frames by 52 s) but the screen stayed black; the self-test showed clears and
  read-backs right and **no draw at all**. `vulkan.cpp`'s negative viewport height: deko3d derives the GPU's viewport
  clip rectangle from the viewport, and a negative height empties it on the hardware (Ryujinx ignores it). The
  self-test's orientation check then showed what was really going on: **deko3d's `OriginUpperLeft` is GL's
  `glClipControl(GL_UPPER_LEFT)`**, the GL renderer's setting (clip space +1 at row 0), not Vulkan's; with the
  negative height the frame had been drawn upside down and turned back by the present's blit. Now: a positive
  viewport and GL's front face (the winding found in Ryujinx belonged to the mirrored picture).
- **Console run dk2 (chapter 1, 1020 MHz, free rate): 60.0 fps locked** from the level's start to the end of the run
  (longest frame 21 ms but for the 45 s captures), game 12.7-13.1 ms a frame, renderer 4.1 ms (GL: 57 fps, game 16.8,
  renderer 7.2, run 11); GPU 5.3 ms a frame. Menus, the novel and chapter 1 match the GL console frames.
- Console run dk3 (level 27, free rate): about **46 fps** (GL 40, run 12), game 21.0 ms, renderer 6.5 ms (GL 9.7):
  commands 0.40 ms against GL calls 3.16. GPU 10.7 ms a frame.
- Console run dk4 (the profiler, level 27): the renderer's draw path 27.5% of the title's thread (assembly 7.5%, the
  ring copies 3.8%, texture signatures 4.5%, small hashes and lookups ~1.5%, the swapchain's IPC ~2%); **the game's
  own recompiled code 65.5%** (about 14 ms of the 21.7 ms frame).
- Run dk5 (cached rings, sampler cache): the same level 27 figures (the cached rings neither help nor hurt; kept, as
  the ultimate base's layer does); and **no texture changed without a lock** through the films, menus, novel,
  chapter 1 and level 27: texture signatures are now taken every 4th frame of a texture (a lock re-uploads at once).
- Run dk6 (**the shipped settings**: 60 cap, adaptive, `cpu_clock = 1785`, the CPU held at 1020 by the console's
  Horizon-OC): chapter 1 at 60.0 fps throughout, game 12.3 ms, renderer 3.7 ms, the cap never dropped to 30.
- The adaptive cap's way back to 60 moved from 13 to 14.5 ms (`host-switch/main.cpp`): chapter 1 with deko3d takes
  about 13 ms of the title's thread, which would not have met 13 after a dip.

### Decision (22:20): deko3d is the port's renderer

`PORT_RENDERER=dk` is the Switch build's default (CMake, when `third_party/uam` is there); the package's NRO is the
deko3d build and the OpenGL build is `fallback/mp2-nx-gl.nro`. What 60 fps looks like now, at the console's 1020 MHz:
menus, films, the novel and chapter 1 at 60 locked; the heaviest level (27) about 46 fps free, locked to 30 by the
adaptive cap; at 1785 MHz (the Horizon-OC profile in TESTING.md) it would be about 12.4 ms a frame, 60. Level 27 at
60 on 1020 MHz needs about 5 ms more off a 21.7 ms frame, and the renderer has about 6 left in all: the rest is the
game's own code (the recompiler's output), the next lever.

## Level 27 at 1020 MHz (2026-11-22 22:30 to 11-23 02:00, the user: "Go ahead", "use all the processing resources")

The aim: level 27 (the Manor, the heaviest level) at 60 on the console's 1020 MHz, by removing work, with the picture
kept exactly as it is. Console runs compare at the same clock: `cpu_clock = 0` in their settings and
`tools/ab_compare.py` (per level, at the clock each window ran at), because the console's Horizon-OC holds the CPU at
1020 MHz most of the time but sometimes lets the port's 1785 stand: run B0 (22:49) got 1785 in level 27 and played it
at **60.0 fps, game 13.5 ms** (the shipped settings ask for 1785, which the stock firmware grants).

### PGO: neutral, kept as an option

GCC 16.1's profile-guided optimisation over the game's recompiled code and the port's own: `PORT_PGO=generate` (an
instrumented NRO; `-fprofile-update=single -fno-profile-values`, since the indirect-call profiler's TLS crashed at
start; the counters are written at exit or after `pgo.txt`'s seconds, `host-switch/pgo.cpp`, through the new
`kernel::OnExit` hooks, the Switch ending with svcExitProcess and no atexit) and `PORT_PGO=use` with the profiles
`tools/pgo_collect.py` takes off the SD. Three console training runs (chapter 1, level 27), then the A/B: chapter 1
12.70 ms (PGO) against 12.50, level 27 20.35 against 20.60: within the noise. Not shipped; the CMake option stays.

### The survey at 1020 MHz (run survey1, 00:19, `test_levels`)

`test_levels = "2,5,8,11,16,19,22,24,30"` (each level 85 s after the last; `test_level_after` gives chapter 1 that many
seconds first): every level holds 60 at 1020 MHz except the Crackhouse (57.4 fps, game 16.7 ms), Gognitti's Lot
(54.0, 17.5 ms) and the Manor (about 46).

### Renderer and runtime cuts (all exact: the same floats, the same commands)

- **Vertex conversion** (`sdk/.../gpu/vertex.cpp`): Max Payne 2's skinned characters have a second stream with a 9 byte
  stride (three bytes, `v1 type 34`), which the input assembler cannot read, so the whole draw (positions, packed
  normals, short texcoords) is converted on the CPU. The conversion's type switch now runs once per element and draw
  (`ConvertRun`) instead of per vertex; the divisions are tables (`kUnits`: each entry Convert's own expression,
  worked out at compile time); a FLOAT element moves a fixed size instead of calling memcpy per vertex; the scratch and
  output vectors no longer zero fill on resize (`NoInitVector`); the converted path's draw-order scratch is a member of
  `Assembled`, not a `thread_local` (under `-mtp=soft` every thread_local access is a call to `__aarch64_read_tp`, and
  the index loop made one per index). Proof: `tools/tests/convert_exact.sh` runs ConvertRun against the old Convert
  (kept as the reference) on all 256 type bytes, both index paths, random and extreme inputs and every packed field:
  0 mismatching at -O0, -O2 and -O3.
- Routing those streams through the GPU's decoders instead would have been cheaper still, but the GPU's division
  is not IEEE exact: a picture change, so not done (the user's decision, if ever wanted).
- **deko3d renderer**: colours through a 256-entry table, last-hit caches in front of the render target and shader
  lookups, and a per-frame line of how vertices were taken (`dk: vertices a frame: N draws in place, gathered,
  converted`).

What the console showed (same session, `cpu_clock = 0`, all at 1020 MHz):

| Run | Build | Level 27 steady (draws a frame) | Assembly | Renderer | Game | fps |
|---|---|---|---|---|---|---|
| A0 (00:49) | survey build | 160 | 2.05 ms | 6.13 ms | 21.2 ms | 45.2 |
| B (00:59) | + vertex loop, caches | 210 (another scene) | 1.73 ms | 6.30 ms | 23.1 ms | 41.8 |
| cut2 (01:33) | + tables, no TLS | 154 | 1.22 ms | 5.14 ms | 20.2 ms | 47.6 |

The scripted runs reach level 27 the same way but the fights diverge, so the steady scenes differ: B's had 31% more
draws (its renderer cost a draw fell from 38 to 30 µs); B and cut2 both converted the same 36 draws (765 KB) a frame,
and assembly fell from 1.73 to 1.22 ms. A0 and cut2 had about the same draw count: **the renderer about 1 ms less,
level 27 from 45.2 to 47.6 fps at 1020 MHz**. Chapter 1: 60.0 locked throughout (game 12.4-12.9 ms, noise).

The profile at 1020 MHz (run prof2, 01:13, cut1 build): the converted path's index loop and conversions most of
`Assemble` (6.1%), memcpy 5.6% (three quarters the uploads into the rings), the swapchain's binder calls 1.7% and the
GPU submissions 0.6% (both needed), `x86_icall` 1.6%; the rest is the game's code, spread thin. The recompiler's output
already takes the guest context as `__restrict` and keeps the flags in locals (dead ones compiled away; nothing stored
at calls), so the generator has no cheap win left.

### The survey again (run survey2, 01:48, cut3 build)

| Level (1020 MHz) | survey1 | survey2 | renderer |
|---|---|---|---|
| 17 Crackhouse 1 | 57.4 fps, game 16.7 ms | 58.7 fps, game 16.3 ms | 3.1 ms |
| 17 Crackhouse 2 | (not in survey1) | 49.6 fps, game 19.4 ms | 2.7 ms |
| 18 Gognitti's Lot | 54.0 fps, game 17.5 ms | 54.7 fps, game 16.4 ms | 3.5 ms |

### Decision (02:00)

In the heaviest levels the game's own code takes 13-16 ms of the title's thread at 1020 MHz, more than a 60 fps frame
leaves with any renderer: 60 there needs the CPU at 1785 MHz, which the shipped settings ask for (and get on a console
without a clock manager holding it at 1020; this console's Horizon-OC is the user's setting). Where the clock stays at
1020, the adaptive cap holds those scenes at a steady 30. The cuts above ship (package of 02:00); the rest of the
renderer's CPU time is uploads, binder calls and per-draw state spread thin. Further levers would be native versions
of hot game routines (each would need its own proof of the same results) or a clock policy, both the user's call.

The port's log rotation kept 30 runs: at the console's 31st run (01:25) it removed the first test log
(`logs/mp2_001.log`, whose copy is `out/console/run1/mp2_001.log`). It keeps 100 now (`core/log.cpp`), about 15 MB.

## 60 at 1020 MHz: the render thread and LTO (2026-11-23 02:10-04:20, the user: "Yes, try both")

The user lifted "remove work, don't move it to another core" for this: the renderer on a core of its own, and compiler
work on the game's code.

### The render thread (shipped, `render_thread = 1`)

- The kit's threaded wrapper (`exemplos/mp2/src/d3d/render_thread.cpp`, written for the Vulkan renderer) records each
  call on the title's thread (the state that changed, resource headers, indices) and replays it on a worker; vertex,
  texture and surface memory are read when the draw runs, as the console's GPU reads them after the call returns, and
  the XDK's locks, busy checks and fences wait for the worker. The deko3d build now has `XBE_RENDER_THREAD=1`;
  `render_thread` in the settings (1 by default) turns it on (the GL build ignores it: its context is the title
  thread's).
- deko3d's objects are not tied to a thread, but the renderer drops calls from any thread but its own: the worker now
  takes it over at its start (`Backend::Adopt`, `gpu/backend.h`; Init still runs on the title's thread).
- `D3D_BlockOnResource` on a texture or surface also waits until the draws that read it have run
  (`WaitResource`, a no-op without the worker), as the console's does: the title writes it next.
- The adaptive 30/60 cap weighs the busier of the title's thread and the render thread (`app::Pace`, the render
  thread's tick count through `app::SetRenderThread`).
- The render thread joins the thread census and the sampling profiler (`kernel::AddHostThread`); the log gives the
  hand-over's cost a frame (`dk: the title's thread a frame: recording N ms, waiting N ms (read-back, resource,
  fence, flush, copy, the frame before)`).
- The picture: the same commands in the same order (Ryujinx: films, menus, novel, level 27; the console's screenshots
  of level 27).

Console, level 27 at 1020 MHz, the same NRO and session, `render_thread` 0 then 1 (runs abRT, 02:28 and 02:39): 45.3 /
46.7 fps (game 21.2 / 20.5 ms) against **55.2 / 51.8 fps** (18.1 / 19.3 ms). The title's thread kept 1.4 ms a frame
of recording (`SendState`'s comparison of about 6 KB of state a draw, memory bound) and never waited for the worker.

The recording: the vertex shader constants (3 KB of it) are now skipped while `vsc_gen` has not moved, and compared
only over the registers written since the last send (`State::vsc_lo..vsc_hi`, widened by the D3D layer's
`VscWritten`, which all eight writes to `vsc` now call; a write without a span falls back to the whole block). On the
console the cost a draw fell about 5% (the game writes low and high registers every draw); the render and texture
stage states cannot be tracked the same way (the D3D layer refreshes them from the game's own memory before draws).

### LTO (`PORT_LTO`, off: no measurable gain)

The generated code calls every function through its weak alias (`sub_X`, which a hand-written replacement may take
over), and GCC never inlines a call to a weak symbol; with link-time optimisation the linker plugin says which
definitions prevail, so small game functions could inline across all 65 files. `PORT_LTO=ON` (`PORT_LTO_JOBS`
parallel link-time compiles) builds in about 3 minutes here and runs (Ryujinx, the console). Console, level 27, rows
of about the same draws: 16.6 ms of the title's thread at 120+36 draws against 15.9 at 116+36 and 17.2 at 129+36
without: no difference beyond the noise of the scenes. Kept as an option, not shipped.

### The survey with the render thread (run survey3, 04:06, 1020 MHz, free frame rate)

| Level | before (survey2) | render thread |
|---|---|---|
| 17 Crackhouse 1 | 58.7 fps, game 16.3 ms | **60.0 fps**, game 14.8 ms |
| 17 Crackhouse 2 | 49.6 fps, game 19.4 ms | 56.6 fps, game 17.6 ms |
| 18 Gognitti's Lot | 54.7 fps | 57.0 fps (its window mostly the level's start) |
| 21 The Manor (27) | about 45-47 fps | 58.1 fps, game 17.2 ms (49-60 by scene in the A/B runs) |

### Where it stands

At 1020 MHz the title's thread now holds the game's own code plus about 1.4 ms of recording: 15-18 ms in the heaviest
levels' fights, 20 ms in the heaviest scene seen (215 draws). Chapter 1, most levels and the Crackhouse's first part
hold 60 locked; the Crackhouse's second part, Gognitti's Lot and the Manor run 56-60 free, which the adaptive cap
(over 16 ms on average) still locks to 30. At 1785 MHz (the shipped request) everything holds 60 with room to spare.

## Near-60 scenes run free (2026-11-23 06:40-, the user: "Let them run free")

### Decision: the line at 20 ms (`fps_adaptive_ms`)

- The adaptive cap's line is now a setting, `fps_adaptive_ms` (20 by default, `app.h`; read and written with the
  others, `host-switch/main.cpp`): 30 once frames take over it on average for half a second, back to 60 once they
  take 1.5 ms less for 3 s. `fps_adaptive_ms = 16` is exactly the old behaviour (16 / 14.5).
- Why 20: survey3's free-running windows at 1020 MHz were game 17-18 ms (55-58 fps) in the fights, 20 ms (49-50 fps)
  in the heaviest and 23 ms (43.5 fps) once. Under 20 ms the screen repeats a frame now and then (at 55 fps 5 of 60
  refreshes a second); past it the repeats come every few frames and a steady 30 reads better. The line sits between
  the two, with the 1.5 ms margin so a scene at the line does not flip back and forth.
- The shipped settings (and `_30`, `_ab`, `_play`) gain the line with its comment; 1785 MHz is still the shipped
  request, where none of this comes into play.

### The user's question: a 55 fps lock for those scenes

`fps_cap = 55` already works (the pacer's timer, swap interval 1). It cannot look locked on the Switch: the screen
refreshes at 60 Hz and every frame stays up 1 or 2 refreshes, so 55 fps always shows 5 repeated frames a second, the
same stutter as running free at 55-58, only spaced evenly, and it holds back scenes that reach 58-60. A timer on top
of vsync also slipped against the refresh at 30 (frames of 40-50 ms, run 13). An even 55 would need the screen at
55 Hz, which the port cannot set. Measured on the console below (run cap55).

### On the console (runs ad20 07:13 and cap55 07:24, NRO 8B8D138F, 1020 MHz, survey3's course)

Both runs: survey3's settings and Resume Game script (the save opens in the Manor, then `test_levels` 22, 23, 24, 27
every 85 s); ad20 with `fps_adaptive = 1`, `fps_adaptive_ms = 20`; cap55 with `fps_cap = 55`. A first ad20 try sat in
the main menu (the console still had the inert input script from 04:20); its log shows the intro and attract films
locking to 30 (22-23 ms of work a frame), as they did under the 16 ms line.

| Stretch | free (survey3) | ad20: 20 ms line | cap55: 55 timer |
|---|---|---|---|
| Crackhouse 1, steady (game 14.5-15 ms) | 60.0 fps | 60.0 fps | 55.0 fps |
| Crackhouse 2, fights (game 17-19 ms) | 54.7-56.7 fps | 54.4-58.4 fps | 52.3-54.5 fps |
| Gognitti's Lot (game 13-18 ms) | 53-60 fps | 54.7-60 fps | 53.2-55.0 fps |
| The Manor, 100 s (game 17.5 ms, 168 draws) | 58.1 fps | 57.0 fps | (another scene: 274 draws, game 24 ms, 39-42 fps) |
| frames over 40 ms in play | 0 | 0 | 0 |

- ad20: the scenes the 16 ms line locked to 30 now run free at 54-60; 30 only past 20 ms, at three level starts
  (8-15 s each, 49-53 fps free in survey3) and in the Manor's heaviest fight (19 s and 7 s, 43-50 fps free): about
  55 s of 430 s of play at 30, against most of it under the old line.
- cap55: no frame over 40 ms (the timer did not slip against vsync the way it did at 30), but 55.0 where the scene
  holds 60 and 1-4 fps under the free rate where it does not (a frame ready just after the timer's release waits for
  the next refresh). It buys nothing over running free and costs frames, so 55 is not shipped; `fps_cap = 55` stays
  available to anyone who prefers it (the adaptive cap works only at `fps_cap = 60`).
- cap55's Manor stretch was a heavier state of the scene than the other runs' (274 draws a frame against 168; the
  recording 2.0 ms against 1.46): game 24 ms whatever the cap. With the shipped settings that stretch locks to 30.

## The clocks for 60 everywhere (2026-11-23 07:55-08:50, the user: "1 plus the clock survey")

The user asked what to do about the levels that do not hold 60 at 1020 MHz. Suggested, and chosen: a clock for this
game, held by the console's clock manager (a Horizon-OC profile the user places: it is the console's system
configuration, outside the port's folder; where it goes: below), at the lowest clock a survey finds to hold 60.

Runs: the package's NRO (8B8D138F), handheld, survey3's course and settings (Resume Game, then the Crackhouse's two
parts, Gognitti's Lot and the Manor, 85 s each; free frame rate), `cpu_clock` / `mem_clock` per run. Horizon-OC held
the CPU at 1020 MHz for the first 73-165 s of each run whatever the port set (the port set it again every 10 s; from
then on the census measured the asked rate to the end) and put the memory back to 1331 four times in the first 5 s;
only the windows at the asked clocks count.

| Clocks | Crackhouse 1 | Crackhouse 2 | Gognitti's Lot | The Manor's heaviest fight (160-166 draws) |
|---|---|---|---|---|
| 1020 (run ad20, free) | 49-60 fps | 54-60 | 53-60 | 57.0 fps for 100 s; 43-50 in its worst |
| 1428 | 57.4 fps, game 16.7 ms | 60 | 60 | 57.2 fps, game 17.4 ms |
| 1581 | (ran at 1020) | 60 | 60 | 58.6 fps, game 17.0 ms |
| 1428, memory 1600 | 58.5 fps, game 15.8 | 59.4-60 | 60 | 58.1 fps, game 17.1 ms |
| **1581, memory 1600** | **60** | **60** | **60** | **59.9-60.1 fps, game 15.2-15.7 ms** |

(At every clock: a single 0.1-0.2 s frame as a level starts, which pulls its 5 s window to 57-59 fps.)

- The heaviest fight barely moved with either the CPU (1428 -> 1581: 17.4 -> 17.0 ms) or the memory alone (1428:
  17.4 -> 17.1 ms) but did with both (15.2-15.7): the title's thread 91-95% busy, the GPU 10.8 ms, the render thread 3.7
  ms. Every level was about 1 ms faster with the memory at 1600.
- **Decision:** the shipped settings and the NRO's defaults ask for `cpu_clock = 1581`, `mem_clock = 1600` (1785 and
  the system's memory before): the lowest pair measured to hold 60 in every level tested, below the 1785 MHz boost
  clock; 1600 is the memory's docked rate. A Horizon-OC profile with the same pair (`handheld_cpu=1581`,
  `docked_cpu=1581`, `handheld_mem=1600`) would have the clock manager hold it from the start, with no fight with the
  port's keeper (where it goes: below).

### Where a Horizon-OC profile goes (2026-11-23 17:00, the user: "Can you please do that for me")

The 09:00 note said `sdmc:/config/horizon-oc/profiles/010091D01597C000.ini`, from the `profiles/README.ini` in the
user's HATS package on this PC. That was wrong for this build, so nothing was written there:

- The console's Horizon-OC sysmodule is the HATS 2026-11-09 pack's (`atmosphere/contents/00FF0000636C6BFF`, the same
  file in the user's OLED backup and MegaPackage, SHA-256 9b5ae2fd...). Its strings (the NSO unpacked) name only
  `/config/horizon-oc/config.ini` ("Skipping key '%s' in section '%s'": `[TID]` sections are read), `log.flag`,
  `log.txt`, `context.csv` and `sdmc:/atmosphere/kips/hoc.kip` (opened `r+b`: it writes its KIP); no `profiles`. Its
  `config.ini.template` shows the per-title form: `[01007EF00011E000]`, `docked_cpu=1224`, `handheld_mem=1600` (MHz).
  The overlay edits the same over IPC (`hocclkIpcSetProfiles`, "Edit App Profile").
- On the console (the receiver's new read-only `CFG_LIST`): `sdmc:/config` has no `horizon-oc` folder (nor
  `sys-clk`). `SVC_HAS` (Atmosphere's sm HasService, 65100): `hoc:clk=1 sys:clk=0`. So Horizon-OC runs on its
  defaults with nowhere to save; its governor's defaults are the likely cause of the CPU staying at 1020 at the start of
  the survey's runs.
- Not done: making `sdmc:/config/horizon-oc` and a `config.ini`. That is Horizon-OC's first configuration on this
  console, and the sysmodule writes its KIP (CPU and memory tables, voltages) from that file; whether a new file
  makes it write the KIP could not be checked (its source on GitHub was not read). The user decides.
  `out\horizon-oc\mp2-nx_section.ini` holds the section for when the folder exists. The receiver gained
  `HOC_APPEND` for that case: append-only, the file unchanged since it was read, no section for the title yet, the old
  file kept as `hoc_config_before.ini` in the port's folder.

### The profile placed and checked (2026-11-23 17:25-17:40, the user: "back up")

- The user copied the pack's `config/horizon-oc` folder (templates, `kip`, `lang`, `profiles`) to the card and
  restarted the console. Horizon-OC wrote no `config.ini` (`HOC_GET`: missing; `hoc:clk` running), like sys-clk,
  whose users copy the template. The receiver's new `HOC_INIT` made `config.ini` from the folder's own
  `config.ini.template`, byte for byte, only because there was none (513 bytes, equal to the pack's template on the
  PC); `HOC_APPEND` then added `\r\n\r\n[010091D01597C000]`, `handheld_cpu=1581`, `docked_cpu=1581`,
  `handheld_mem=1600` (CRLF like the template; 592 bytes, read back equal to the template plus the section; the
  pre-append file kept as `switch/mp2-nx/hoc_config_before.ini`). No restart after it.
- Check run (log mp2_048, the package NRO, survey3's course, `cpu_clock = 0` so the census shows Horizon-OC's CPU,
  `mem_clock = 1600` so the boot line shows the memory before the port sets it): `CPU measured at 1581 MHz before
  any change` at 0.4 s, `memory 1600 -> 1600`; every census 1570-1583 MHz from 20 s to 480 s; no `was reset` line.
  Every play window of the Crackhouse (both parts), Gognitti's Lot and the Manor at 60 fps, the longest frame 17-23
  ms; the Manor's heavy stretch game 15.5-15.9 ms at 60. Under 60 only the test's level loads (2-3.6 s) and single
  long frames as a level starts. Horizon-OC did not rewrite `config.ini` (still 592 bytes after the run).
- So Horizon-OC itself now holds the pair for hbmenu's title from the start of every run (the 1-3 minutes at 1020 of
  the survey are gone); the shipped settings keep asking for the same pair, harmless with the profile and the fallback
  without it. The card's run files went back to the shipped state (the toml and the inert input script, read back).
- Not reached by any clock: cap55's heavier state of the Manor (274 draws a frame, 24 ms at 1020) would take about
  18 ms at 1581 with the memory at 1600; the adaptive cap keeps it free just under 60 there.

## Environment

- Recompiler host compiler: LLVM 23.1.5 (`C:\Program Files\LLVM`, clang-cl) with the MSVC headers and libraries of
  Visual Studio 18 Build Tools (`VsDevCmd.bat`); VS 2022 (the kit's default) is not installed.
- No LunarG Vulkan SDK; Vulkan headers and volk are on the PC in renut-nx's plume (`app/thirdparty/plume/contrib`).
- No NVK build for the Switch (`nvk/`, the kit's Switch renderer driver) anywhere on the PC. libnx has deko3d
  (`C:\devkitPro\libnx\lib\libdeko3d.a`), and devkitPro's portlibs have switch-mesa (EGL / OpenGL).
- PC test build: `ferramentas\build_pc.bat exemplos\mp2` (LLVM clang, `third_party\vulkan` = Vulkan headers from
  renut-nx plus an import library made from `C:\Windows\System32\vulkan-1.dll` with `llvm-dlltool`), run with
  `tools\run_pc.ps1` (timed runs, `-Shots`, `-Script` with `TRACE` / `SHOT` steps; scripts in `tools\scripts`).
- Switch build: `ferramentas/build_switch.sh` in devkitPro's msys2 (make, since msys2 has no ninja here); GCC 16.1.
  `tools/patch_gcs.py` runs after it. GL renderer: `CMAKE_EXTRA='-DPORT_RENDERER=gl -DPYTHON_EXECUTABLE=<the PC's
  python.exe>'` (glslang's configure needs a Python). GCC's dependency files are off for glslang's directory only;
  with them off everywhere, a header change did not rebuild its users (02:45: a mixed NRO crashed Ryujinx). After
  touching a header, `make -n mp2-nx` in `build/mp2` lists what will rebuild.
- From a Git Bash shell (2026-11-23: the agent's shell changed from devkitPro's msys2 to Git Bash), devkitPro's msys2
  programs did not get the environment: no TMP/TEMP/USERPROFILE (GCC then tried `C:\WINDOWS` for its temporary files,
  "Permission denied"), and a Ninja on the Windows PATH (WinGet) made `build_switch.sh` ask an existing Makefiles
  folder for Ninja. Builds now go through devkitPro's own login shell with a clean environment: `env -i
  SYSTEMROOT=... TMP=... TEMP=... USERPROFILE=... PATH='C:\WINDOWS\system32;...' /c/devkitPro/msys2/usr/bin/bash.exe
  -l <script>`, the script exporting `DEVKITPRO`, `BUILD_DIR`, `CMAKE_EXTRA` and calling `build_switch.sh`. A
  configure from the wrong shell can rewrite `CMAKE_CACHEFILE_DIR` to the `/home/<user>` alias of the same folder;
  putting the line back is enough.
- Ryujinx: `Desktop\Ryujinx\publish\Ryujinx.exe` (beside the port's folder), this port's data folder `Ryujinx\MP2` made from the
  SSX port's `bis` (firmware 21.2.0) and `system` (keys) — the katamari data folder the task names is not on this PC.
  `tools\run_ryujinx.ps1` (one run, refuses while any Ryujinx runs or under 8 GB free commit, stops only its own
  process, the game folder linked into the SD by a junction, captures every 15 s, the SD's toml put back).

## Every level on the console, and a level-change hang (2026-11-23 17:40-19:00, the user: "Continue working until the game is completed")

### The sweep (package NRO 0004B3B0, the shipped clocks 1581 / 1600 held by the Horizon-OC profile, free frame rate)

Runs A (log mp2_049: levels 2-16) and B (mp2_050: 17-30), each level 65 s through `test_levels`, no input (Max stands
where the level starts; levels that open with a long scene showed that scene, at 60). The CPU census read 1573-1585 MHz
in every window of both runs.

- Levels 2-15 and 17-28: every 5 s window after a level's first 12 s at 59-60 fps, except single long frames as a level
  or a scene starts (0.1-0.4 s; 2.4-2.6 s where a level's load ran past 12 s: levels 20 and 27).
- Level 19 (19_Third_Address_Unknown), 53-59 s in: 55.6-57.3 fps with no long frame. The GPU: 14.5-16.4 ms a frame
  (longest 22-24 ms) with the title's thread waiting; at the system's handheld GPU clock (`gpu_clock = 0`). The one
  GPU-bound scene seen; a handheld GPU clock step is the candidate (to be measured).
- **Two hangs.** Level 16 (from 15, run A, change 15 of 15) and level 29 (from 28, run B, change 13 of 14): `game:
  level init <name>` and then nothing: no file of the new level opened, its loader thread (`thread N starts at
  00098C74`, 50-350 ms after every other change) never started, the title's thread at 100% in every census, no frame,
  the two idle workers waiting on their semaphores, for 100-120 s until the run was closed. Both came while the level
  before had played 65 s.
- The same changes on the PC: 15 -> 16 with the OpenGL renderer, then 27 changes 15 <-> 16 with the Vulkan renderer
  on the render thread (the console's wrapper): never a hang. Not the render thread's wrapper as such.
- Guest yields and zero sleeps go to `std::this_thread::yield()` (libnx: a yield to threads of the same priority on
  the same core); the guest threads all have one host priority and, with the shipped `thread_cores = 0`, one core.
  Horizon does not share a core between threads of one priority (a thread keeps it until it waits or yields); the
  Xbox's kernel and Windows do, by quantum. A guest thread spinning for another guest thread on its core would spin
  for good on the Switch only, and only when the timing puts the other one behind it: the working theory.
- A stress run (15 <-> 16 every 20 s with the profiler on, log mp2_051 / prof_051 on the card) ended in a crash dialog
  at about 10 minutes ("This software needed to close due to an error"; "Do Not Share" chosen); the console then went
  to sleep and stopped answering, so its log and profile are still on the card.

### Ready for the next console session (built, not yet run: NRO 17674013..., `out/console/hoc/mp2-nx_stall.nro`)

- A stall report in the clock keeper (`host-switch/clocks.cpp` `StallCheck`): when no frame has been paced for 10 s it
  logs each busy guest thread's pc, lr and five return addresses as NRO offsets (`stall:` lines; the ELF names them),
  five reports 2 s apart, then one a minute. Each report pauses the thread for a moment: if the hang is the starvation
  above, that alone lets the waiting thread run, and the game would recover about 10 s into a hang (the test).
- `guest_quantum_ms` (a test aid, 0 off): every N ms a guest thread that used its whole core is taken off it for a
  moment, as the Xbox's quantum does. `out/console/run_files/mp2-nx_stall1516.toml` (the stress list) and
  `mp2-nx_quantum1516.toml` (the same with 10 ms).
- The PC build's default frame cap is 60 (it was 30, the first target): levels 22 / 24 / 27 at 60.0 fps on the PC,
  level 22's play at 3.5-3.7 ms of game time a frame.

## The level-change hang: ended guest threads kept their 64 MB stacks (2026-11-23 19:15-20:30)

The console woke at 19:11, but the session's tool refused the button presses that start hbmenu (twice), so the stall
report could not be run. The cause came from the code instead.

- **What the title waits on.** The level init (`sub_00023B05`) builds the level's loader (`sub_000FC8D0`), starts it on
  a thread of its own at 0x23D82 (`sub_000FCBC0`: `CreateThread(0x24000 stack, sub_000FC980)`, the loader's progress
  counter `[obj+0x28]` set to 0), then at 0x24051 waits for the counter in `sub_000FC240`:
  `while (counter <= n) SwitchToThread();`. In every good change the loader thread logs `thread N starts at 00098C74
  (000FC980, ...)` 50-350 ms after `game: level init`; that line is written by the new thread itself
  (kernel/threads.cpp, `StartThread`), so in the two hangs the loader never ran, and the title spun in that loop.
- **Why the loader never ran.** Every guest thread gets a 64 MB host stack (`Spawn`, for the recompiled code's deep host
  frames). libnx allocates it from the heap and frees it only in `threadClose`, which newlib calls on a join; this
  port's `__syscall_thread_detach` was a stub returning 0 (host-switch/stubs.cpp), whose own comment said a thread that
  ends "keeps its stack and handle until the process exits". So each loader (one per level change) left 64 MB behind.
  Run A hung at its 23rd guest thread (22 started, 18 ended), run B at its 21st (20, 16): about 1.1-1.2 GB of dead
  stacks plus the live threads' 64 MB each, with the game's own memory, the 3.1 GB heap was full. `pthread_create`
  then failed, and `Spawn` did not check: no thread, no log, a title waiting for good. The stress run's crash after
  about 30 changes fits the same exhaustion (another allocation failing first).
- **Why only the console.** On Windows the 64 MB is a reservation (`STACK_SIZE_PARAM_IS_A_RESERVATION`), committed as
  used, and an ended thread's stack goes with it. The scheduler theory above was wrong: libnx gives every pthread the
  process's core mask with no preferred core (`svcSetThreadCoreMask(h, -1, mask)` right after `threadCreate`, read from
  libnx.a), so a ready thread is free to run on an idle core; `guest_quantum_ms` stays a test aid, off.
- **The fix (runtime, Switch only).** A guest thread that ends (returning, or through `ExitThread` /
  `PsTerminateSystemThread`) leaves the census list and hands its pthread to a reaper thread, which joins it once it
  has exited; libnx then unmaps and frees the stack. A failed `pthread_create` is logged (`thread: no 64 MB stack`)
  and retried with 8 MB rather than leaving a guest thread unstarted. The `thread N starts at` line now ends with the
  heap in use (`heap N MB used`, mallinfo), so a run's log shows whether memory stays flat across level changes.
- **Checked in Ryujinx (run reap2, NRO 27B7DD09, log mp2_075, the stress list with the clocks at 0 since Ryujinx has
  no clkrst set):** 30 changes between levels 15 and 16 every 20 s plus the first level, 31 level inits and 31 loader
  starts, the heap at 768 MB at the first loader and 790 MB at the last (the old build would have held about 1.9 GB
  more by then); 60 fps in play after the last change. One false `stall:` line (960 billion seconds): a frame paced
  after the keeper read the clock made the difference negative; the check now treats a newer frame as no stall
  (NRO 997D1557).
- **The control (run leak_old, the package's NRO 0004B3B0 before the fix, log mp2_076, the same list and settings):**
  11 changes fine, then at 357 s `game: level init 13_Construction_Site` and the backing line, and no loader thread,
  no frame and both workers waiting on their semaphores for the 60 s it was watched: the console's hang, at the 21st
  guest thread the process made (console run B hung at its 21st too). Stopped by hand at 420 s. Under Ryujinx the
  title's thread did not show as busy in the census (the emulator's tick count of a yielding thread), but the stop
  itself is the same.
- **The stress run's crash (log mp2_051, fetched 20:54 when the user opened hbmenu):** at its 17th level init
  (`13_Construction_Site`, 478 s, guest thread 70: the process's 24th) the guest stack's backing could not be made,
  `memory: backing D0600000+100000 failed at create (0xCE01)`, the kernel out of memory; the loader thread started on
  an uncommitted stack and faulted at its first push (guest address D062FF4C). The same exhaustion as the hangs, met
  first by a different allocation (the profiler's buffers were on in that run).
- **Guest stacks leaked too** (all platforms): `mem::FreeStack` had no caller, so each ended thread also kept its guest
  stack (the loader's: 192 KB committed, 256 KB of the 256 MB stack range), the `memory: backing D0x00000+100000` lines
  every few level changes. About a thousand loads to the range's end; on the Switch the backing is real memory. Ended
  threads now give their guest stack back (`FreeGuestStack`, after the routine returned or in `ExitThread`); the
  KPCR and ETHREAD stay, since the thread's handle can outlive it.

### The console checks (2026-11-23 20:52-21:30; the user opened hbmenu, the session pressed Y)

- **Run c1 (NRO 997D1557: the reaper only; log mp2_052; the stress list, no profiler):** the heap stayed at 772-773 MB
  through every loader (the 64 MB a change is gone), and threads 21-23, where both hangs were, started. It crashed at
  the 17th level init (476 s, the 24th thread), exactly where the profiled stress run had: `memory: backing
  D0600000+100000 failed at create (0xCE01)`, then the loader on an unbacked stack.
- **Code memory objects run out at 11.** Guest memory on the Switch is heap shown through code memory
  (`svcCreateCodeMemory`, `kernel/memory.cpp` `NxBack`), one object per backing. Both crash logs failed at the 11th
  object, both sweeps had made exactly 10: the kernel has room for about ten. MP2 made six at its start (the two
  64 MB blocks, and 1 MB granules: two of the system pool at B0000000, two of the stacks at D0000000), so four granules
  of growth were all a run had, and the leaked guest stacks (256 KB a level load) took them in about 23 threads.
- **Fixes:** guest stacks freed at thread exit (above), and the system pool and stack range backed 8 MB at a time
  (`kNxBlock`): four objects at the start, six spare, and the backing line names the object's number
  (`(code memory object N)`) so a log shows the headroom. Run c2: NRO AC48BB7E, the same list.
- **Run c2 (NRO AC48BB7E: the reaper, guest stacks freed, 8 MB blocks; log mp2_053; the same stress list):** 31 level
  inits and 31 loader starts, every change through; the heap at 779-781 MB from the first loader to the last; four code
  memory objects in all (the two 64 MB blocks, D0000000+8 MB, B0000000+8 MB), none added in the run; no error line;
  the CPU at 1574-1584 MHz; still in play at 14 minutes (both runs before it crashed at 476 s). Closed from Home.
  The card then got the shipped settings and the inert input script back (both read back identical), and keeps
  AC48BB7E as its `mp2-nx.nro` (read back identical to the build).

## Level 19's GPU-bound stretch (2026-11-23 21:55, the user: "Yes, level 19")

- **What the sweep log shows (mp2_050, 19_Third_Address_Unknown):** the opening scene at 3.6 ms of GPU a frame; when
  play starts (48-60 s in) 7.4, then 16.4 and 14.5 ms a frame (longest 22-24 ms), then 9.2. About 190 draws and
  1.5-2.4 MB of uploads a frame, the renderer 3.0-3.4 ms on its thread, no read-backs, 1.6-2.0 fragment barriers a
  frame: the GPU's own pixel work, not the renderer's overhead. Textures stay BC1-3 on the GPU (only volume textures
  are decoded), so it is not a decoded-texture bandwidth cost. The frame is drawn at 960x720 (`render_percent = 100`:
  the output's height), 2.25x the Xbox's 640x480; a lower share would be a picture change (the user's call), so not
  that.
- **Decision:** the candidate is a handheld GPU step through the port's own `gpu_clock` (Nintendo's handheld rates:
  307.2 / 384 / 460.8 MHz; the system's default for this title unmeasured, the log says `GPU 0`, not opened), the
  shipped value stays 0 until a console run measures it. Run files ready: `out/console/run_files/mp2-nx_l19_gpu461.toml`
  and `mp2-nx_l19_gpu384.toml` (level 19 three times, 70 s each, the free frame rate, the shipped CPU and memory
  clocks); the input script `input_script.txt` (New Game) beside them. If 384 holds 60 through the stretch it ships,
  else 461 if it does.
- **Blocked:** starting hbmenu from the Home menu (R held over a title) was refused by the session's safety check a
  third time ("undermining oversight"), so the run waits for the user to open hbmenu; everything after that (Y, the
  receiver, netload, closing from Home) has worked from here.
- **One run for all three steps (built 22:05, not yet run):** `test_gpu_clocks` (a test aid, never saved;
  `src/game/test_level.cpp` and `host-switch/clocks.cpp` `TestGpu`) sets the GPU clock for each `test_levels` change in
  turn, 0 meaning the system's own rate (read back and logged, so the default gets measured too). Run file
  `out/console/run_files/mp2-nx_l19_gpusteps.toml` (`test_levels = "19,19,19"`, `test_gpu_clocks = "0,384,461"`, 70 s
  each) with the test NRO `out/console/hoc/mp2-nx_gpusteps.nro` (59565E4D), netloaded under its own name so the card's
  `mp2-nx.nro` stays the package's AC48BB7E. Ryujinx cannot run it (its clkrst lacks the rate list, command 10).
- **The soft fallback rebuilt** with the thread and memory fixes (build/mp2-soft, NRO 31C7458C, git cfe7f01) and put
  in the package beside the deko3d NRO it was built with.
- **The run (22:56-23:02, the user opened hbmenu; log mp2_054, test NRO 59565E4D, `mp2-nx_l19_gpusteps.toml`):**
  - **The wrong level.** `test_levels` counts in `kLevels`' order, where 19 is `14_Second_Dream`; the sweep's slow
    stretch is in `19_Third_Address_Unknown`, which is 25 (sweep B changed to it at 585 s, its 9th change). The notes
    above call it "level 19" after the game's name; a run for it needs `test_levels = "25,25,25"`.
  - **What it did show:** the console's handheld GPU clock is 307.2 MHz (`test_gpu_clocks` step 0, read back). When
    the port set 384 or 460.8 MHz the clock was back at 307 within the second, every second (`clock: GPU was reset to
    307 MHz, back to 384 MHz`, the keeper's eight logged lines), so the steps barely held: 14_Second_Dream's play at
    11.75-12.96 ms of GPU a frame at 307.2, 11.43-12.50 "at 384", 11.22-12.28 "at 460.8" (at a held 460.8 it would be
    about 8.3). All three passes at 60.0 fps, longest frames 18-20 ms: that level holds 60 at the stock GPU clock.
- **Decision:** the shipped `gpu_clock` stays 0. The port cannot hold a GPU rate on this console (the clock manager or
  the system puts 307.2 back each second), and the clock is the clock manager's to keep (the CPU decision, "The clock
  is not the port's to keep"). Horizon-OC takes a title's GPU clock in its profile (`handheld_gpu=`, in its template's
  own example). For 19_Third_Address_Unknown's stretch (GPU 14.5-16.4 ms, longest 22-24, at 307.2), 460.8 MHz would
  bring it to about 10-11 ms (longest about 16): one line, `handheld_gpu=460`, under `[010091D01597C000]` in
  `sdmc:/config/horizon-oc/config.ini`. That file is the user's: the session's exception only appends a missing
  section, and the section exists, so the line is offered, not written. 460.8 MHz is Nintendo's own handheld top
  rate (not an overclock); the cost is battery.
- The card after the run: the shipped settings and the inert input script put back over FTP (sys-ftpd-light answered
  this time) and read back identical; `mp2-nx.nro` still AC48BB7E; the test NRO stays as `mp2-nx_gpusteps.nro` beside
  it (nothing on the card is deleted). Console on Home, nothing running.
- **With the user's `handheld_gpu=460` (23:13-23:18; log mp2_055; `mp2-nx_l25_check.toml`: `test_levels = "25,25"`,
  `test_gpu_clocks = "0,0"`, the clock read back, not set):** the profile read back with the line (receiver, read
  only); the GPU read 460.8 MHz at both changes and no reset line (the port set nothing). 19_Third_Address_Unknown,
  second pass, 48-58 s in (the sweep's stretch): GPU 11.39 and 9.40 ms a frame (longest 14.3-14.9) at 60.0 fps,
  against 16.37 and 14.51 (longest 22-24) at 55.6-57.3 fps at 307.2 MHz: the GPU time fell by the clock's ratio. The
  rest of both passes at 60.0 fps, 2.5-6.5 ms of GPU; no error line. **Every level now holds 60 in play on the console
  with the shipped settings and the profile** (cpu 1581, memory 1600, handheld GPU 460.8). `package/TESTING.md` and
  `out/horizon-oc/mp2-nx_section.ini` carry the fifth line; the card back to the shipped settings over FTP (read back).

## Saving and loading (2026-11-24 00:20-00:55; "Please continue")

No run had saved or loaded on purpose before; every console run played New Game or test levels, which only make the
game's own auto saves. So the save paths were run on the PC build and in Ryujinx (the deko3d NRO), with scripts in
`tools/scripts/`: `mp2_menu_look.txt`, `mp2_resume.txt`, `mp2_pause_look.txt`, `mp2_quicksave.txt`,
`mp2_load_menu.txt` (PC) and `mp2_quicksave_switch.txt` (Ryujinx timing).

- **The menus:** with a save on the disk the main menu reads How to Play, Resume Game, New Game, Load, Save (greyed
  out), Previously, Options, Credits; the cursor starts on How to Play. In play, START opens Game Paused: Quick Save,
  Load Last Game, "Press START to return". Quick Save closes the menu itself and shows "Game saved". Load Last Game
  asks "Press A to quit the current game or B to cancel". The main menu's Load lists Auto Save, Quick Save, Previous
  Quick Save and three empty Saved Game Slots.
- **What works (PC):** Resume Game loads the newest save (12_Condemned_Building_B, its cutscene, then play at 60 fps).
  Quick Save writes a new `UDATA\5454000c\<id>\` folder (`savegame.sav` 153620 bytes and `SaveMeta.xbx`) and the old
  quick save becomes the Previous Quick Save (the game deletes the older folder itself). Load Last Game puts Max back
  where the quick save was made; Load from the main menu loads a chosen save. 60 fps after every load.
- **The same in Ryujinx (Switch build, run save1, log mp2_077):** Resume Game loaded the old auto save at 61 s,
  Quick Save wrote `88F4AA6E1731` on the virtual SD at 126 s, and Load Last Game reopened it and `DATA.RAS` at
  159.9 s; Ryujinx's window showed Max back at the save spot, 60 fps after.
- **The save names' times are UTC.** The quick save made at 00:30 (UTC-6 here) is named "6:30". The name's builder,
  000F88D0, calls `GetSystemTime` (0027C5F9: `KeQuerySystemTime` and `RtlTimeToTimeFields`, UTC) and joins the
  fields with ":" (0x326A4C). A real Xbox's system time is UTC too, so the original game names its saves in UTC.
  Left as the game does it: changing the text is the user's call.
- **A kernel answer fixed on the way:** XAPI's `GetTimeZoneInformation` (0027C94F) reads XC_MAX_OS (0xFF, the
  EEPROM's user section) at 0027C54F: bias at +4, the zone names at +8 and +0xC, the change dates at +0x18 and +0x1C,
  the standard and daylight biases at +0x28 and +0x2C. The kernel answered zeros, so the C library's time zone
  (00226345, `_timezone` = bias × 60) was UTC. `ExQueryNonVolatileSetting` now answers 0xFF with the user section:
  the host's offset from UTC now (libnx `timeToCalendarTimeWithMyRule` on the Switch, whose C library keeps local
  time as UTC; `localtime`/`gmtime` on the PC), no daylight dates, and the language, video, audio and DVD region of
  the single settings. The PC log shows bias 360, Ryujinx 0 (its clock is UTC). Nothing Max Payne 2 shows changes
  (the names use `GetSystemTime`); it is what a console with a zone set answers (HANDOFF row 34).
- **Left byte for byte:** XC_MAX_ALL (0xFFFF), which the game asks for at 0009CB12 to build its save signature key
  (an HMAC: the 0x36363636 pad at 0009CA6A) when the kernel's HD key is all zeros. Changing that block would make
  existing saves fail their check. Its first 8 calls are now logged with the caller.
- **A PC test-tool quirk, not the game:** the PC build's main log is buffered and never flushed on a timer, so a run
  stopped by `run_pc.ps1` loses its last few KB (one run's log ended mid-line at 76 s of 100). `logs/stderr.log`
  gets every line unbuffered and showed the game running to the end. The Switch log flushes every 5 s.
- **Not in the package:** the shipped NRO stays the console-verified AC48BB7E. The kernel change shows nothing in
  this game, so it waits for the next package build; it is in git and the base patch.

### On the console (2026-11-24 06:09-06:14 console time; the user opened hbmenu, the session pressed Y)

Run save1 (log mp2_056; the package NRO AC48BB7E netloaded under its own name, the shipped settings, CPU 1575-1582
MHz, memory 1600; input script `tools/scripts/mp2_quicksave_console.txt`, out/console/run_files/input_script_save.txt).
The card had one save, the auto save of Part III, Chapter 5 (`DDE21F9A2D17`, backed up to out/console/save1).

- **Resume Game** (56 s) loaded Part III, Chapter 5 (Mona in the back room), in play at 60 fps by 66 s.
- **Quick Save** (113.3 s) wrote `U:\26236D323B5E\` (SaveMeta.xbx, savegame.sav); "Game saved" on the screen.
- **Load Last Game** (144.1 s; Mona had strafed into the burning room) reopened `26236D323B5E\savegame.sav` and
  `DATA.RAS`; the level's streams reopened at 144.5 s and she stood at the save spot again by 146 s.
- **A second Quick Save** (197.2 s) wrote the old quick save into `195967CBFF5F` ("Previous Quick Save - 13:10") and
  the new one into `04838A59467B` ("Quick Save - 13:12"); the game removed `26236D323B5E` itself, as on the PC. The
  auto save is byte for byte as before.
- **Frame rate:** 60.0 fps in every 5 s window of play before and after each save and load (game 8.6-14.5 ms). Each
  save or load costs one long frame: 66-82 ms at the first quick save, 146 ms at the reload, 235 ms at the second
  quick save (it writes two saves). Clean exit (`Exit (0)`, HOME, X, A).
- **The names' times:** 13:10 and 13:12 are UTC (the PC's 07:10-07:12, UTC-6); the console's own clock showed
  06:13, an hour behind the PC (its time zone is set an hour west of the PC's). The game names saves by
  `GetSystemTime`, so neither zone shows in them. This NRO predates the XC_MAX_OS fix (its log has
  `ExQueryNonVolatileSetting FF -> 0`); nothing differs in the game because of it.
- **The card afterwards:** the shipped `mp2-nx.toml` (9b572151), the inert `input_script.txt` (as before, read back),
  `mp2-nx.nro` read back as AC48BB7E. The saves the run made stay (the Quick Save and Previous Quick Save of Part III,
  Chapter 5, beside the auto save). Console on Home, nothing running.
- **A script note:** the Switch build's input names have no right stick (LS LSD LSL LSR only), so `RSR` in
  `mp2_quicksave_switch.txt` (the Ryujinx run) did nothing; the turn there came from LSR and LSD. The file now says
  so; the console script leaves it out.

## The PC as a release: every level, window modes, a package, mouse look (2026-11-24 07:30-09:05)

The user: "Keep working throughout the day uninterrupted until game is completed". The Switch meets its targets on
the console (with the Horizon-OC profile); the PC build was still a test host. Decisions, in order:

- **Every level on the PC at 60** (sweep, `tools/scripts/mp2_sweep_pc.txt`: Resume Game, then `test_levels` 2-29
  every 45 s, the default 60 fps cap, Vulkan). Every 5 s window of play at 60.0 fps, the game 0.2-2.5 ms a frame;
  single long frames (96-306 ms) only where a level's graphic novel hands over to play (its `music.rws` opens). Level
  30 was not reached in the run's 1370 s (some changes wait for a film). `test_levels` needs a level running before
  it changes (test_level.cpp counts updates after a level init), so a sweep starts with Resume Game or New Game.
- **Window modes** (host-pc/main.cpp): `display_mode` is honoured: borderless over the window's screen (the default;
  "fullscreen" is taken as borderless, no exclusive mode) or a sizable window; F11 and Alt+Enter switch (Enter held
  with Alt is not A). `host::WindowPixels` is the client size, so both renderers' last pass letterboxes the frame,
  still drawn at 1280x720 (`width`/`height` both set draw it at that size). DPI aware (per monitor v2). Checked:
  borderless 1920x1080 and back to the 1280x720 window three times, the swap chain following each. `--windowed`
  (tools/run_pc.ps1 passes it) keeps scripted runs in a window that does not take the focus.
- **The PC package** `out\package\pc` (tools/make_package_pc.ps1): the exe, the shipped settings
  (package/mp2-nx_pc.toml), the readme (package/README-PC.txt), the Visual C++ runtime app-local (msvcp140,
  vcruntime140, vcruntime140_1 from Visual Studio's Redist folder), the PDB and BUILD.txt. Run from a clean copy:
  shipped settings read, game found by argument, drawn.
- **Mouse look.** The game's own controls page (How to Play): RT shoot, LT shootdodge, B bullet time, A jump, X
  reload, Y use/zoom, white painkillers, black secondary attack, D-pad weapons, right stick aim (click: aim fast).
  The right stick's turn, measured from screenshots (tools/stick_shift.py, PIL only): a dead zone to about 0.62 of
  the travel, then steep, about 10 px a frame of the 640 px frame at full travel; on-screen measures varied up to 40%
  with the scene's depth, so the angle was found instead:
  - guest memory dumps between known turns (PC test aid `SCAN`, tools/scan_diff.py; mind `test_levels` in the PC
    settings, which changed level mid-run once): the yaw in radians at 00C430B8 (0 -> 0.4984 -> 0.7915 -> 0.2052
    over 16, 10 and -20 frames at full travel: 0.03 a frame), beside its rotation matrix, in the character object
    00C42EFC (vtable 0032AC88, just after the name "player");
  - a hardware write watch (PC test aid `WATCH=addr`: debug register 0 set from a helper thread on every thread,
    the game's own included, the writer named from the PDB): `sub_0014B900` (store at +0x6A), called from
    00148A44, where the yaw object (character +0x152) gets `+= sub_0014EF16()` (the controller's +0x7A) and is
    kept in 0..2 pi; the controller (`sub_0015003E`) fills +0x7A and +0x7E from its input source's vtable
    +0x78 / +0x7C;
  - the player's input source (vtable 0031C8F0) keeps the frame's turn at +0xD (yaw) and +0x11 (pitch), written by
    `sub_00045B36`: the stick's value (vtable +0x60 / +0x5C) times 100.8 (0031C9D0) or 63 (0031C9CC) degrees a
    second, times pi/180, times the frame's time.
  `src/game/mouse.cpp` hooks `sub_00045B36` and, for the player's input source only, adds `input::TakeMouse`'s
  counts (0.05 degrees each, gyro.cpp's unit) to +0xD and +0x11, taking back its own addition if the game did not
  write the field again that frame. Measured with scripted counts (`MX=`/`MY=`): 200 counts turn 47-51 px (10
  degrees; the stick's 10 degrees 58 px), -400 back 94-100 px. The pitch: positive tilts the view down, and so does
  the stick pushed up (the game's look is inverted as set here), so the mouse adds `+dy`: the mouse up looks up.
- **The PC mouse** (host-pc): raw input (WM_INPUT) while the window has the mouse (borderless and in front, or a
  window clicked into; the pointer hidden and clipped; Alt+Tab gives it back; the click that takes it, or brings the
  borderless window back, is not a shot); buttons left = right trigger, right = left trigger, middle = B, the wheel
  a D-pad press up or down per notch; keys added: Space A, R X, C left stick click. `TakeMouse` drops counts piled
  up over a pause (0.25 s without a call). The raw input path with injected input (tools/tests/real_mouse_run.ps1:
  the window brought to the front, a click, SendInput moves; about 12 s of the PC's mouse): the click took the mouse
  ("pc: mouse taken for the game"), 200 counts right turned 48 px, 400 left 95 px back, 80 up looked up 10 px; the
  same as the scripted counts. (A SendInput structure of the wrong size, 48 bytes for 40, is refused without a word:
  the script now checks.)
- **The main menu's other items on the PC:** Credits (six DOWNs: the cursor skips the greyed-out Save) plays
  `cred_n.bik`, Previously (four) `prev_n.bik`, both drawn right at the films' 30 fps; Options opens Controls
  (Movement, Combat), Select Crosshair, Game (Enable Auto Aiming, Enable Auto Lock, Invert Aiming, Enable Vibration,
  Auto Weapon Change, Enable Bullet Time Aid ticked; Enable Subtitles not) and Audio. "Invert Aiming" ticked is the
  stick's inverted look measured above. Scripts `mp2_credits.txt`, `mp2_previously.txt`, `mp2_options_menu.txt`.
- **Level 30 on the PC** (`test_level = 30` alone): 21_The_Manor_B_split2 at 60 fps for 70 s.
- **Dead Man Walking on the PC at 60** (`test_script` GM_InitSingleLevel(01_Survival) after Resume Game): 60.0 fps,
  the game 2-3 ms a frame.
- **The PC exe as a release** (host-pc/CMakeLists.txt): a window program (`/subsystem:windows`, entry
  `mainCRTStartup`, so `main` stays), no console window beside the game; the log as before in `logs/`. The port's
  icon (icon.jpg, the NRO's) made into an .ico at configure time with Pillow and compiled in (`icon.rc`), the window
  class's icon too.
- **The Switch:** the same hook gives the base's gyro aiming a place to turn the view (`PORT_GYRO_MODE` stays 0:
  off until the player picks it); not tried, no motion in Ryujinx.

## The next build on the console, and the panel's controller page (2026-11-24 afternoon; "Yes, hbmenu is open")

### On the console (15:26-15:40 console time; the user opened hbmenu, the session pressed Y)

Run next1 (log mp2_057; the next build `out/console/next/mp2-nx_0905.nro`, 76285327, netloaded under its own name so
the card's `mp2-nx.nro` stayed the package's AC48BB7E; the shipped settings, CPU 1581, memory 1600; input script
`tools/scripts/mp2_quicksave_console.txt`). The card's three saves were copied off first (out/console/next1/card_before).

- **Play, save and load as the shipped build:** Resume Game loaded the newest Quick Save (Part III, Chapter 5) and the
  script's Quick Save, Load Last Game and second Quick Save all went through ("Saving", the quit question, "Game
  saved" on the screenshots). 60.0 fps in every 5 s window of play (game 10.4 ms, renderer 2.7 ms, GPU 5.3 ms), one
  long frame at each save or load (152-292 ms), as the shipped build. Clean exit (`Exit (0)`).
- **The time zone:** `ExQueryNonVolatileSetting FF: the user section, time zone bias 420 minutes`, asked at the first
  Quick Save: the console's own zone (UTC-7, an hour west of the PC). The saves are still named in UTC ("Quick Save -
  Tue Nov 24 22:33" at 15:33 on the console's clock), as the original game names them (`GetSystemTime`, above).
- **The gyro through the port's panel** (both sticks pressed in, the TOUCH CONTROLS page, GYROSCOPE stepped to WHILE
  AIMING (ZL)): `gyro: AIM`, `gyro: sensors on`, three samples of 0.0 0.0 0.0 degrees a second with gravity 0.00 0.00
  -1.00 g at 5 ms (200 Hz): the readings of a console lying flat and still. Whether they were the handheld's own sensor
  or sys-botbase's virtual controller (player 1 while it is attached) the log cannot tell. With the gyro on and ZL not
  held the view held still for 10 s (two screenshots identical). Stepped back to OFF (`gyro: sensors off`). The turn
  itself needs a hand.
- **Found on the way, fixed below:** (1) the panel's gyro choices were the base's OFF / WHILE AIMING (ZL), but ZL is
  Max Payne 2's shootdodge; (2) the page was TOUCH CONTROLS with touch rows that do nothing here (CONTROLS LAYOUT "-",
  OPACITY 0%: touch:: is a stub on both hosts); (3) opening or closing the panel with both sticks also crouched or
  stood Max: sys-botbase presses the two one after the other, and the first one alone reached the game.
- **The panel saves the settings file:** closing the panel after a change rewrote `mp2-nx.toml` without its comments
  (539 bytes for 2405; every value as shipped, the rest written at their defaults). The game also wrote `gyro.toml`
  (gyro = 0 and the defaults, as with no file).
- **The card afterwards:** the shipped `mp2-nx.toml` put back (9b572151, read back), the inert `input_script.txt`
  (read back), `mp2-nx.nro` read back as AC48BB7E. New on the card: `mp2-nx_0905.nro` (the test NRO), `gyro.toml`
  (off), log mp2_057 and the run's saves (Quick Save and Previous Quick Save of Part III, Chapter 5, beside the auto
  save; copies in out/console/next1/after). Console on Home, nothing running.

### The panel's controller page and chords (src/common/menu.cpp, core/panel_chords.h, both hosts' input.cpp)

- **GYROSCOPE:** OFF, ALWAYS or WHILE FIRING (ZR), the PC's list. A gyro.toml saved with the old ZL mode shows as
  WHILE SHOOTDODGING (ZL) and the next step leaves it. Still off until the player picks it.
- **The page:** CONTROLLER, not TOUCH CONTROLS. On the Switch: MOTION CONTROLS (the gyro's eight rows), then the
  sticks' dead zones and sensitivities as before; on the PC (no gyro) the sticks' alone.
- **The chords:** `input::PanelChords` (sdk runtime `core/panel_chords.h`, header only, so the base gets it with the
  hosts), called by both hosts' `Poll` for player 1 before the pad is hidden:
  - a stick pressed in alone reaches the game 80 ms late; the other one pressed within that time makes it the chord,
    which the game never sees (the pad stays hidden until both are let go); a tap shorter than the wait reaches the
    game at its release, for 50 ms; a stick pressed with Back held is not held back;
  - **Back + R3 opens and closes the panel.** It was checked in `common/game_info.cpp` on the pad `Poll` had already
    emptied while the panel was open, so on the PC F10 and Back + R3 opened the panel but never closed it (found
    here: the PC run's L3 tap after the second Back + R3 did not crouch Max, the pad was still hidden). Now toggled
    before the pad is emptied; game_info.cpp no longer checks it (a base game that still does sees an empty pad).
- **Scripted runs reach the panel** (host-switch/input.cpp `NavButtons` adds the input script's held buttons): the
  panel, the gyro's trigger and the both-sticks chord read the controller itself, which a script did not reach.
  Nothing changes without an input_script.txt.
- **Test aids:** `tools/run_ryujinx.ps1 -Capture N` (a capture every N seconds; the `_screen` capture is taken after
  the panel is drawn); `tools/tests/window_shot.ps1` (the PC window as shown, the panel included, by PrintWindow, with
  no focus change); scripts `mp2_panel_switch.txt`, `mp2_panel_pc.txt`.

**Checked** (Ryujinx run panel5, NRO 913F5CDD; PC run with exe 2080EC42):
- Switch: both sticks 50 ms apart opened the panel and the closing pair (the other stick first) closed it, Max
  standing throughout; RB to CONTROLLER (MOTION CONTROLS, then CONTROLLER); GYROSCOPE stepped ALWAYS, WHILE FIRING
  (ZR), OFF (`gyro: ON`, `FIRE`, `OFF`; the sensors on and off; Ryujinx's own sensor reads zeros, gravity included,
  every 50 ms, so the console's 0, 0, -1 g at 5 ms was most likely the handheld's real sensor); a 50 ms L3 tap
  crouched Max, L3 held half a second stood him up; Back + R3 opened the panel and closed it again, Max standing.
  60.0 fps median in play.
- PC: Back + R3 opened the panel, RB showed the CONTROLLER page with the sticks' four rows, Back + R3 closed it, the
  L3 tap crouched Max and the held L3 stood him up (window captures at 58-70 s).
- **For the console:** `out/console/next/mp2-nx_1714.nro` (913F5CDD, with its ELF): the 0905 build checked above plus
  the panel changes.

### On the console again (16:21-16:28 console time; "Yes, hbmenu is open")

Run next2 (log mp2_058; `mp2-nx_1714.nro`, 913F5CDD, netloaded under its own name; the shipped settings; input script
`tools/scripts/mp2_resume_console.txt`, Resume Game only; the panel pressed through sys-botbase, as a player would):
- **Both sticks** (sys-botbase presses LSTICK, then RSTICK): the panel opened and closed with Mona standing; the frame
  after closing was identical to the one before opening. The CONTROLLER page as in Ryujinx.
- **GYROSCOPE** stepped ALWAYS (`gyro: ON`, sensors on, 0, 0, -1 g at 5 ms), then, after 30 s, WHILE FIRING (ZR) and
  OFF (`gyro: FIRE`, `OFF`, sensors off). **With the gyro always on and the console lying still the view did not
  move for 30 s** (four screenshots, no shift; drift correction on): no drift, and 60.0 fps with the game at 10.4 ms a
  frame, as with it off.
- **Back + R3** opened the panel and closed it, but **behind it the game had gone to its main menu**: sys-botbase
  presses MINUS before RSTICK, and Back alone reached the game (Back in play opens Max Payne 2's main menu). Resume
  Game returned to play. Same class as the sticks; fixed below.
- **An L3 tap** (sys-botbase's click) crouched Mona and a second one stood her up: the tap path works on the console.
- 60.0 fps in every 5 s window of play; clean exit (`Exit (0)`).
- **The card afterwards:** the shipped `mp2-nx.toml` put back (the panel had rewritten it, 539 bytes, the same
  values), the inert `input_script.txt`, `gyro.toml` as before (off), `mp2-nx.nro` AC48BB7E: all read back. New:
  `mp2-nx_1714.nro`, log mp2_058. The console then stopped answering (asleep after the game closed: sys-botbase and
  FTP timed out at connect), so the packaged NRO was not put on the card: it still has AC48BB7E (a copy in
  out/console/backup_AC48BB7E).

### Back held back too (core/panel_chords.h)

`input::PanelChords` now treats Back, L3 and R3 alike: any of them pressed alone reaches the game 80 ms late, and its
chord's other button within that time makes the chord (Back + R3 toggles the panel; L3 + R3 hides the pad), which the
game never sees; a tap shorter than the wait reaches the game at its release, for 50 ms. Checked in Ryujinx (run
panel6, NRO D2C9C0DA: Back first and R3 50 ms later opened and closed the panel with play behind it, a 50 ms Back tap
opened the game's menu, A resumed; the earlier steps as before) and on the PC (exe 74EC2B65: Back + R3, the CONTROLLER
page, the L3 tap and hold, a Back tap opening the game's menu). On the PC Escape is Back, so it too is 80 ms late.

**The package** (`out\package`): the console-checked `mp2-nx_1714.nro` (913F5CDD, sources git 170ae0a); the
fallbacks unchanged (GL AC5A1D4C and soft 31C7458C, git cfe7f01; `make_package.ps1 -FallbackRev` now says so in
BUILD.txt); TESTING.md describes the panel's chords, the CONTROLLER page and the gyro (step 7). The Back fix
(`out/console/next/mp2-nx_1741.nro`, D2C9C0DA) waited for a console run (below).

### On the console, the Back fix (16:43-16:48 console time; "Yes, hbmenu is open")

Run next3 (log mp2_059; `mp2-nx_1741.nro`, D2C9C0DA, netloaded under its own name; the shipped settings; input script
`mp2_resume_console.txt`; the chords pressed through sys-botbase, which presses a chord's buttons one after the other):
- **Back + R3** (MINUS first): the panel opened with play behind it, not the game's menu, and closed back to play.
- **Both sticks:** opened and closed it; the frames after each close identical to the one before (no crouch).
- **A Back tap** opened the game's main menu (it still reaches the game); A on Resume Game returned to play.
- **An L3 tap** crouched Mona, a second one stood her up.
- 60.0 fps in every window of play; clean exit (`Exit (0)`).
- **The card afterwards:** `mp2-nx.nro` is now this build (D2C9C0DA, read back; AC48BB7E kept in
  out/console/backup_AC48BB7E), the shipped `mp2-nx.toml` (not rewritten: no setting changed), the inert
  `input_script.txt` and `gyro.toml` (off), all read back. New: `mp2-nx_1741.nro`, log mp2_059.
- **The package** (`out\package`) holds D2C9C0DA (sources git 919d41a), the same file as the card's; TESTING.md says
  the chords are held back and checked on the console.

### The OpenGL fallback rebuilt, the installer release 0.1.10 (2026-11-24 evening; "Keep working until the game is completed")

- **The GL fallback** (`build/mp2`, NRO FC6A4942, the same sources as D2C9C0DA): rebuilt so the fallback has the
  gyro hook, the time zone answer and the panel changes too (it was git cfe7f01). Ryujinx run gl_panel1 with
  `mp2_panel_switch.txt`: the level loaded, GYROSCOPE stepped ON / FIRE / OFF (so the panel opened and was navigated),
  the L3 tap crouched and the hold stood Max up, the Back tap opened the game's menu and A resumed; 59.6 fps median
  (the GL renderer in the emulator, a proxy). The port's GL captures read back a quarter wide in Ryujinx (the known
  emulator quirk), so the centred panel itself is not in them. Not run on the console (a fallback for when deko3d does
  not start). The soft fallback stays git cfe7f01 (31C7458C). `make_package.ps1` takes `-GlRev` and `-FallbackRev` for
  each fallback's revision.
- **The installer** (`mp2-nx-installer`, not a git repo): release 0.1.10, `release/mp2-nx.nro` = D2C9C0DA (the
  package's and the card's), `mp2-nx.toml` unchanged (9b572151), manifest hashes from the copied files, README_MP2.md's
  release line. Not published.
- **Dead Man Walking on the Switch build** (Ryujinx run dmw1, NRO D2C9C0DA; `out/console/run_files/mp2-nx_ryu_dmw.toml`
  = the Ryujinx save settings plus `test_script = "maxpayne_gamemode->GM_InitSingleLevel(01_Survival);"`; input
  script `mp2_resume_console.txt`): after Resume Game the script line ran at 70 s, `game: level init 01_Survival`, the
  survival map's street (Max by the van, enemies arriving) at 60.0 fps from 79 s to the end of the run (a proxy).
- **The ending on the Switch build** (Ryujinx run ending1, NRO D2C9C0DA; `mp2-nx_ryu_ending.toml` = the Ryujinx save
  settings plus `test_script = "maxpayne_gamemode->GM_SendEndOfGameMessages();"`; script `mp2_ending_switch.txt`:
  Resume Game, then R every 2.5 s): the script line ran at 70 s and the final graphic novel played page by page to its
  last panels (captures every 5 s to 223 s); 60 fps, no crash. As on the PC.
- **The credits film on the Switch build** (run credits1, script `mp2_credits_switch.txt`: the main menu's Credits,
  six DOWNs): `open d:\movies\cred_n.bik` at 65.7 s, the credits scrolling at the film's 30 fps to the end of the run.
- **A level's own end:** the game's script has no level-complete command (its 75 `GM_` names in default.xbe; a
  chapter ends when Max reaches the level's exit trigger), so the chapters' starts with their graphic novels, which
  the console sweeps went through (`test_levels`), are what was checked; a played-through exit is the user's play.

## The PC played by hand (2026-11-24 18:31-18:38; the user: "Lets start with the PC Version testing")

- **Set-up:** `Max Payne 2\pc-test`, a copy of `out\package\pc` (exe 74EC2B65, the shipped settings, the runtime
  DLLs; not the game folder) with the PC test runs' three saves in `hdd\E\UDATA`, started with the extracted disc
  folder as its argument (`pc-test\Play Max Payne 2.lnk` does the same): no game data copied, the saves and logs stay
  in pc-test.
- **The session** (log `pc-test\logs\mp2_001.log`): borderless over the 1920x1080 screen, the mouse taken at once;
  the intro film, the main menu, Load twice, the Part III, Chapter 8 save (`21_The_Manor_B_split2` at 117 s), about
  4.5 minutes of play at **60.0 fps in every 5 s window** (the game 1.2-2.6 ms a frame; one 55 fps window at 119 s,
  the level's load); Alt+Tab gave the mouse back and took it again on return. No crash, only the usual idle-wait
  warnings. Not done by hand: a Quick Save, F11.
- **The user: the mouse aiming "felt good"** (`mouse_sensitivity = 1.0`, the game's auto aim as shipped).
- **The CPU fallback rebuilt** (`build/mp2-soft`, NRO A38AF499, the same sources): Ryujinx run soft1 with no script,
  the shipped Ryujinx settings: the soft renderer started (640x480 guest, 1280x720 framebuffer), the intro films
  played through the logos and the opening film to the title, 4.6 fps (this renderer's speed). The package's three
  NROs (deko3d D2C9C0DA, GL FC6A4942, soft A38AF499) are now one revision; BUILD.txt says "the same git revision" for
  both fallbacks.
- **The console play test by hand** (2026-11-24 evening): the user did not get to play; nothing on the card changed
  (no log after mp2_059, the saves and settings as left). It stays the one open item: a long session, a chapter
  played to its exit, the gyro aim.

### Wide rendering on the deko3d build (2026-11-24 evening; the user asked whether to turn `render_wide` on)

- Supported by the Switch's GPU renderers (deko3d and GL: `WantScale`, the targets `scale_x_` wide); the PC's D3D
  layer has one scale (`app::RenderScale`, 960x720 stretched) and no `render_wide`: adding it means separate
  horizontal scales through that shared layer's targets, viewports and scissors, so the PC is left as it is.
- Never measured on the console (the 11-22 plan's A/B was not run). The cost: a third more pixels for the GPU. Chapter
  1's GPU time is about 5 ms a frame; level 19's GPU-bound stretch is 9.4-11.4 ms at Horizon-OC's 460 MHz, so about
  12.5-15 ms with it on (estimate): under the 16.7 ms budget, unmeasured.
- Ryujinx run wide1 (NRO D2C9C0DA, `mp2-nx_ryu_wide.toml` = the Ryujinx save settings with `render_wide = 1`,
  `mp2_panel_switch.txt`): `render scale 2.000 x 1.500`, 60.0 fps median in play, no crash. Against run panel6 (the
  same NRO and script, wide off) at the same moments: the same framing, HUD, panel and main menu; at full size the
  shelves, the body bag and the wall texture sharper side to side. The game state differed between the runs (a weapon
  change before the script's first press in play, the panel's sensitivity stepped down, the gyro row not stepped):
  input from outside the script reached Ryujinx (a controller on this PC, most likely); nothing of it is the
  renderer's.
- **Decision (the user: "Turn it on"):** `render_wide = 1` in the shipped settings (`package/mp2-nx.toml`, and the
  play and 30 fps files); `mp2-nx_ab.toml` is now the original picture (`render_wide = 0`) as the A/B's other half.
  The port's own default (`options.h`) stays 0, so the console-checked NRO D2C9C0DA is not rebuilt for one constant;
  the files say the shipped value differs from the default (the panel's Defaults button on the Graphics page gives
  0). The PC is unchanged (no `render_wide` in its D3D layer). Package rebuilt (settings c3d3c6e6, git c8aa0fc), the
  installer release 0.1.11 (the same NRO, the new settings). The card still has the earlier settings (the console
  asleep at 19:40): the next console session puts them there first.
- **For the next console session (prepared, not run):** level 19's GPU stretch with wide rendering, an A/B in one
  session with the same NRO (`mp2-nx_1741.nro`, D2C9C0DA): `out/console/run_files/mp2-nx_l25_wide0.toml` then
  `mp2-nx_l25_wide1.toml` (`mp2-nx_l25_check.toml` of mp2_055 with `render_wide` 0 / 1: the shipped clocks, a free
  frame rate, `test_levels = "25,25"`, 75 s each; the stretch is 48-60 s into each pass), input script
  `input_script_resume.txt` (Resume Game first). Read the dk GPU ms and fps of the stretch in each; mp2_055 had GPU
  9.4-11.4 ms at 460.8 MHz with it off. Then the shipped settings (wide on) and the inert input script back.

### Wide rendering on the console: level 19's stretch (2026-11-24 18:13-18:28 console time; "Yes, hbmenu is open")

Run wide1 (log mp2_060; `mp2-nx_1741.nro`, D2C9C0DA; `out/console/run_files/mp2-nx_l25_wide_inrun.toml`: the shipped
clocks 1581 / 1600, a free frame rate, `render_wide = 0`, `test_levels = "25,25"` 90 s each; handheld, the GPU at
460.8 MHz from the user's Horizon-OC profile, read back at each change; input script `input_script_resume.txt`).
- **The plan's live log read failed:** sys-ftpd-light cannot open a file the game is writing ("450 failed to open
  file"), so the driver could not find the first pass's start and stopped before the panel. Both passes ran with wide
  off. Then, in the same run: the panel (Back + R3, Graphics, WIDE RENDERING ON: `dk: render scale 2.000 x 1.500` at
  556 s) and Load Last Game (570.5 s), which reloaded the game's own auto save, a checkpoint about 40 s into level 19
  written during the second pass, so the stretch came 11-21 s after the reload.
- **Wide off** (passes 1 and 2): the stretch's 5 s windows GPU 8.6 / 11.7 and 9.3 / 12.6 ms a frame, 60.0 fps; the rest
  of the level 6.2-6.5 ms of GPU at 60.0.
- **Wide on** (after the reload): the stretch 8.7 / 14.6 / 12.0 ms of GPU, **59.6 and 59.0 fps** (longest frames 21 and
  24 ms: a few repeated frames over about 10 s); the rest of the level 7.5-8.2 ms of GPU at 60.0 (about a quarter
  more than off). The game's own time unchanged (13.9-14.5 ms). Docked the GPU runs faster (768 MHz).
- **Decision:** wide rendering stays on (the user's choice); the cost is that stretch's few repeated frames in
  handheld. A clean 60 there with it on would need a higher handheld GPU clock in the user's Horizon-OC profile (their
  file and their call: more heat and battery), or wide off.
- **The auto save:** during the run the game replaced its one auto save (the Part III, Chapter 5 one of 4:15 UTC,
  `DDE21F9A2D17`, copied off earlier in out/console/next1/card_before) with a new one of the same chapter
  (`7766F2DF370D`, 1:18 UTC). The game does this itself; nothing was deleted by the session.
- **The card afterwards:** the new shipped `mp2-nx.toml` (c3d3c6e6, `render_wide = 1`), the inert `input_script.txt`,
  `gyro.toml` unchanged, `mp2-nx.nro` D2C9C0DA: all read back. Console on Home after the close.
- **The user's decision ("Actually, keep it as is"):** the Horizon-OC profile stays as it is (`handheld_gpu=460`);
  wide rendering stays on with the stretch's few repeated frames in handheld. Nothing was written to the console's
  config. (For a later change: the session may only append to `config.ini`, and the profile's `handheld_gpu=460` line
  would then appear twice in the section; how Horizon-OC's reader treats a repeated key was not checked, so a raise
  is best done by the user in the overlay's "Edit App Profile" or by editing that line.)

## The console played by hand (2026-11-24 19:17-19:26 console time; "Alright lets launch the long play session")

Log mp2_061 (the user started mp2-nx from hbmenu: `sdmc:/switch/mp2-nx/mp2-nx.nro`, D2C9C0DA; the shipped settings,
wide rendering on, `render scale 2.000 x 1.500`; the gyro off). The game was left on the death screen ("Press A to
load the last saved game") and closed by the session (HOME, X, A) to complete the log.
- **What was played:** the main menu, then level 19 (`19_Third_Address_Unknown`, Part III, Chapter 5) from 67 s, about
  8 minutes of play; the saves read at 124 and 192 s and the auto save loaded at 359 and 442 s (reloads after dying),
  each a single long frame of 160-300 ms. No level change (the chapter's exit not reached), no Quick Save, no settings
  changed, the gyro not turned on.
- **Frame rate:** 60.0 fps in nearly every 5 s window, the game 8-15 ms and the GPU 5-9 ms a frame; the GPU-heavy part
  of the level (GPU 9-11.6 ms windows, wide on) gave 58.4-58.6 fps in two windows and 59.7 in one, as the A/B
  measured; the rest were the reloads.
- **Stability:** no crash, no error lines (only the usual idle-wait warnings); `stall: no frame for 23 s` at the close
  is the HOME menu's suspension; clean `Exit (0)`.
- **The card afterwards:** unchanged (the three saves, the shipped settings c3d3c6e6, gyro.toml, the inert script).
- **Still not tried by hand:** a chapter played through to its exit, aiming by turning the console.
- **A chapter played through by hand on the console** (2026-11-24 evening, the user: "Just played a chapter, looks
  great"): the session's log (mp2_062) is still open, the game suspended on the Home menu at 20:27 console time; its
  details (the chapter change, frame rate) wait for the game to be closed.
- **Gyro aiming tried by hand on the console** (the user: "Gyroscope works"), in the same open session (details from
  its log below).
- **With that, everything is tried by hand:** the PC with a mouse, a long console session, a chapter played through,
  the gyro. Left for the user only: publishing (not done) and reading mp2_062 once the game is closed.

### The chapter session's log (mp2_062; 20:07-20:33 console time; the user closed the game)

- **The chapter played through:** level 19 (`19_Third_Address_Unknown`, Part III, Chapter 5) from 23 s to its exit;
  `game: level init 20_Third_Dream` at 1146 s (Part III, Chapter 6: the game's auto save "Part III, Chapter 6",
  `E7C1134259D8`, replaced the Chapter 5 one). The chapter change ran clean (one 1.2 s long frame for the load).
- **Frame rate in level 19** (wide rendering on, about 19 minutes): 60.0 fps in nearly every 5 s window; the dips
  (50.3-59.3 in single windows) are the reloads after dying (the saves read 13 times) and the known heavy stretch.
- **The Home menu:** the game suspended about 160 s (`stall: no frame for 160 s`, ending at 1334 s; the session's
  read-only screenshot at 20:27 showed the Home menu with the game open). No hang.
- **The gyro:** `gyro: ON` at 1357 s (stepped ON, FIRE, ON), the sensors on at 1379 s (0, 0, -1 g at 5 ms: the
  handheld's sensor), `OFF` at 1407 s, `ON` again at 1473 s and left on: `gyro.toml` after the session has `gyro = 1`
  (the earlier read of `gyro = 0` fell between 1407 and 1473 s). Left as the user set it.
- **Render resolution raised by accident:** `dk: render scale` 2.100 x 1.575 ... 2.500 x 1.875 at 1346-1348 s: five
  steps of the panel's RENDER RESOLUTION (100% to 125%) 0.4 s apart, right as the panel opened on its first row
  (where the focus starts) on the way to the gyro. With it, Third Dream's play ran at **43-50 fps, the GPU at 20-23 ms
  a frame** (1.5625 times the pixels of 100% wide; about 13 ms at the shipped 100%). The panel had rewritten
  `mp2-nx.toml` with `render_percent = 125`; the session put the shipped settings back (c3d3c6e6, `render_percent =
  100`, `render_wide = 1`; read back), the gyro's own file untouched.
- **Clean exit** (`Exit (0)`), no error lines (the usual idle-wait warnings only).
- **A panel hazard this shows:** the panel opens with the focus on RENDER RESOLUTION, so a D-pad left or right meant
  for moving around changes it at once.

### The panel opens on its tabs (2026-11-24 evening; the user: "Yes, fix it")

- `src/common/menu.cpp` `DrawTabs`: `ImGui::SetItemDefaultFocus()` on the current page's tab, so the panel opens with
  the focus there (ImGui gives tab items no default focus, so it used to start on the first row, RENDER RESOLUTION,
  where a D-pad left or right stepped the value at once). From the tab, left and right move between the tabs (a page
  is chosen with A, or LB / RB as before) and DOWN enters the rows at the first one.
- **Ryujinx** (run focus1, NRO 3132159B, `mp2_panel_switch.txt` updated: no UP after the first DOWN; a RIGHT straight
  after the second opening): the first opening with no row highlighted; RB, DOWN to GYROSCOPE and three RIGHTs gave
  `gyro: ON`, `FIRE`, `OFF` at the script's 101, 104, 107 s; the second opening's outline on the CONTROLLER tab, the
  RIGHT moved it to GENERAL and changed nothing (no `render scale` or `gyro:` line); 60 fps median.
- **PC** (exe 82A1AEBA, `mp2_panel_pc.txt` with a RIGHT after the opening; window captures): the panel opened on the
  GRAPHICS tab with no row highlighted; the RIGHT moved the outline to CONTROLLER, RENDER RESOLUTION still 100%.
- For the console: `out/console/next/mp2-nx_2153.nro` (3132159B); the package and the card keep D2C9C0DA until it
  has run there.
- **On the console** (21:13-21:18 console time; "Yes, hbmenu is open"; log mp2_063; `mp2-nx_2153.nro`, 3132159B, the
  shipped settings, the user's gyro on; input script `input_script_resume.txt`, which resumed the user's Part III,
  Chapter 6 auto save in `20_Third_Dream`): the panel opened by both sticks and by Back + R3 (sys-botbase) on the
  GRAPHICS tab with no row highlighted; a RIGHT straight after each moved the outline to CONTROLLER only. No
  `render scale` line after the first (2.000 x 1.500 throughout), and `mp2-nx.toml` still c3d3c6e6 afterwards (the
  panel saves only on a change): nothing changed. Third Dream at 60.0 fps in every window at the shipped 100%
  (the 43-50 fps of the session before was the accidental 125%). Clean exit.
- **Shipped:** the card's `mp2-nx.nro` is now 3132159B (read back; the inert script and shipped settings read back);
  the package (sources e9ddc5e; the fallbacks labelled git 919d41a, `make_package.ps1 -GlRev / -FallbackRev`), the
  installer release 0.1.12, the PC package (exe 82A1AEBA) and the user's `pc-test` copy (the exe and its PDB).
- **The fallbacks rebuilt with it** (the same sources as 3132159B): GL 5990607F (Ryujinx run gl_focus1 with
  `mp2_panel_switch.txt`: `gyro: ON`, `FIRE`, `OFF` at the script's 101, 104, 107 s, nothing at the stray RIGHT of
  130 s; 59.4 fps median, the GL renderer in the emulator) and soft D1E63760 (run soft2: the intro films to the
  title). The package's three NROs one revision again (sources 0a4afcc); TESTING.md says so.
- **For the base:** HANDOFF row 37 (not in the patch, the base's own `exemplos/coldfear/src/common/menu.cpp`: its
  panel's first row is RENDER RESOLUTION too, and its tabs have no default focus).

## Publishing the source (2026-11-24 evening; the user: "Continue on and publish")

- **What is published, and what is not.** The port's source only: the tools, the SDK and host code, the hooks, the
  docs. Not the NRO, the PC exe or the installer release that carries the NRO: each holds the game's recompiled code,
  and the kit's own README says no game file may be distributed, nor an NRO ("que contém o código do jogo
  recompilado"). That is also what keeps the game's copyrighted code off the internet.
- **Checked before publishing:** the repository tracks no game material (the `.gitignore` keeps out the assets, the
  generated C++, the game folder, build output, `out/`, logs, NROs, ELFs, the icon; no binary or image is tracked, nor
  ever was). No personal details in the current tree (the Windows user name, home-network address, email address,
  user paths: none; two generic `/c/Users/*/AppData` search paths in `build_switch.sh`). The history has six old
  lines with the Windows user path (since removed), so the published branch `publish` is one parentless commit of
  `master`'s tree (`git commit-tree`); the local history stays.
- **Added:** `LICENSE` (MIT, as the base's, with the base's copyright kept; third-party parts keep their own) and a
  current README (status, the deko3d build line, packaging, a warning never to share an NRO or exe).
- **Where:** a new repository `MP2-nx` under the user's GitHub account (Git Credential Manager has the account
  stored for pushing). No GitHub CLI on this PC (installing it would be a download), and the browser pane was not
  signed in to GitHub, so creating the repository waits for the user's sign-in there.
- **Published** (the user signed in to GitHub in the browser pane: "Signed in, continue"): the public repository
  https://github.com/maafeu/MP2-nx, created empty (no README, .gitignore or licence from GitHub) with the description
  "Max Payne 2 (Xbox) static recompilation port for Nintendo Switch and PC. The game is not included: build from
  your own disc." `git push origin publish:main` through Git Credential Manager's stored account: `main` is
  9db517d, one parentless commit of `master`'s tree (278 files; no personal details, no binaries; checked again just
  before the push). `git ls-remote` shows only `main` at 9db517d; GitHub shows the README and the MIT licence.
  `master` (the full local history, with the old user path in it) is not pushed.
- **Later updates:** commit on `master` as before, then put the new tree on GitHub as a commit on top of `publish`
  (`git commit-tree master^{tree} -p publish`, `git branch -f publish`, `git push origin publish:main`), so the old
  history never goes out. The NRO, exe and installer release stay private.
- **Release v1.0.0** (the user: "Can we create a release now?"): a source release, no binaries attached (the NRO and
  exe hold the game's recompiled code). The annotated tag `v1.0.0` on 38d1b69 (the published `main`), pushed alone;
  the release "MP2-nx 1.0.0" made on GitHub's page with the notes (what works, building from your own disc, known
  limits, credits and licence; the text kept in the session's scratchpad as release_notes_v1.0.0.md). GitHub's API:
  not a draft, not a pre-release, 0 assets (only the automatic source zip and tarball); marked Latest.
  https://github.com/maafeu/MP2-nx/releases/tag/v1.0.0

## The web installer, published (2026-11-24 night; the user: "can we create a web installer page?")

- **Bring your own NRO.** The installer (renut_nx_ultimate_installer, GPL-3.0) bundled `release/mp2-nx.nro`, which
  holds the game's recompiled code, so it could not go online as it was. Now a release without an NRO (no
  `nro_sha256` in `release/manifest.json`) shows a step "Choose your build of the port": the player's own
  `mp2-nx.nro` (checked for "NRO0" at 0x10), handed to the worker in place of a release NRO (`worker.js`). The disc is
  checked as before (`default.xbe`'s SHA-256, `DATA.RAS`); the zip holds the player's NRO, the shipped
  `mp2-nx.toml` and the game under `switch/mp2-nx/game/`. The steps number themselves; step 4 says to start the
  game from hbmenu with R held on an installed game (the tested way), not the template's Sphaira forwarder.
- **Tested end to end** (Chromium in the browser pane, the template's `serve.py` with `--test-files` the extracted
  game, `--test-packages` the package's NRO folder; `test/browser.html?compile=0&nro=/_pkg/mp2-nx.nro`, new): 429
  files, 1.79 GB, 132 s; a checker of the session's (the template's `check_zip.py` needs pip's lz4, not installed)
  found the NRO (3132159B) and settings (c3d3c6e6) byte for byte and all 429 game files byte for byte, none missing
  or extra, every CRC good. The test zip (game data) deleted. On the page: a non-NRO rejected, the NRO accepted,
  Create held off until both are chosen.
- **Published:** a separate repository (its GPL-3.0 kept apart from this repository's MIT),
  https://github.com/maafeu/MP2-nx-installer, one commit (8aa6a8e; 84 files, 36 MB; no NRO, zip, disc image, XBE or
  save; no personal details: the only scan hits are Python's own docstring and author credits inside Pyodide), with
  GitHub Pages from `main` (`.nojekyll`): **https://maafeu.github.io/MP2-nx-installer/**. Live: the own-NRO step
  shown, `release/mp2-nx.nro` 404, the download service worker registered.
- **The icon:** the page keeps its plain generated pattern. The user offered the game's cover art for it; that art
  and the logo are the publishers' and the page is public, so not there. The Switch build already uses that image
  as its NRO icon (`exemplos/mp2/icon.jpg`, git-ignored, so private) with the title "Max Payne 2".
- The local `mp2-nx-installer` (with the bundled NRO, private) has the same code; the public copy is
  `mp2-nx-installer-public`.
- **The page's icon** (the user: "Yes, make one"): an original drawing, `tools/make_icon.py` in the installer
  repository (Pillow, drawn at 2048 px and scaled to 512; a rainy night, a bullet crossing it in slow motion with its
  wake and a red streak; no characters, no game art, no lettering). Installer commit bea753c; live on the page.
- **The page's icon, second version** (the user: "Original skyline icon"): heavy rain over an invented night city,
  a red haze on the horizon and a red streak across the sky (`tools/make_icon.py` rewritten; no game art, no real
  building, no lettering). Installer commit b949b27; live. The user's other picture (the game's hero and logo, from
  a fan image) was not used on the public page, for the same reason as the cover art. The Switch build keeps its own
  icon and title as they are (the user: "Leave the originals on my switch").
- **The installer's release** (the user: "Can you create a release page please"): the annotated tag `v1.0.0` on the
  installer's b949b27, the release "mp2-nx web installer 1.0.0" (notes: the page's link, the four steps, no game
  code or files, tested in Chromium, credits and GPL-3.0); GitHub's API: not a draft, not a pre-release, 0 assets.
  https://github.com/maafeu/MP2-nx-installer/releases/tag/v1.0.0. The port's own v1.0.0 release notes gained a
  "Packing it for your Switch" section pointing to the installer (the user asked whether the two repositories
  duplicate each other: the port builds `mp2-nx.nro`, the installer only packs a build and the player's disc).
- **The installer merged into this repository** (the user: "Can we somehow merge the two repos together? keeping the
  web installer?"): the installer's files are `installer/` here (85 files, unchanged; GPL-3.0, its own `LICENSE` and
  `NOTICES.md`, which `LICENSE` and the README now say). `.github/workflows/installer-pages.yml` publishes the folder
  as it is to GitHub Pages (Pages source: GitHub Actions) on a push to `main` that touches it, or by hand. The page
  is now **https://maafeu.github.io/MP2-nx/**. The first run failed (deploy-pages v4 found no artifact from
  upload-pages-artifact v3); with checkout v7, configure-pages v6, upload-pages-artifact v5 and deploy-pages v5 it
  deploys (run 45555085255, main 27f08ed). Checked live: the own-NRO step shown, `release/mp2-nx.nro` 404 (none
  published), the icon, the service worker registered at `/MP2-nx/`, no console errors.
- The old repository (maafeu/MP2-nx-installer, main 5e60c21) now holds only a "moved" README, its `LICENSE` and
  `NOTICES.md`, the icon, and a page that sends the browser to the new address: `index.html` for the root, and
  `404.html` for any old path, which keeps the rest of the path. Its `sw.js` replaces the old download worker and
  unregisters itself. Checked in the browser: the old root and an old deep link land on the new page, and only the
  new page's worker is left. The installer's history stays there. Both v1.0.0 release notes now point to the new
  address (the old one with a "Moved" line).
- **The old repository deleted** (the user: "then we can remove the MP2-nx-installer", after being told it can't be
  undone and that the old link would then 404): deleted in GitHub's settings on 11-24 about 00:00. GitHub's API
  answers 404 for it; MP2-nx and https://maafeu.github.io/MP2-nx/ are untouched. The links above to the old repository,
  its release and its page are history now and lead nowhere. The local clone `mp2-nx-installer-public` (its remote
  is gone) and the private `mp2-nx-installer` are kept on the PC.
- **Renamed MaxPayne2-nx** (the user, 11-26: "can we change it everywhere to have it named MaxPayne2-nx"): the GitHub
  repository is now https://github.com/maafeu/MaxPayne2-nx (GitHub sends the old repository address there, for the
  web and for git) and the web installer https://maafeu.github.io/MaxPayne2-nx/ (Pages does not redirect: the old
  page address is a 404 now). In the files, every `MP2-nx` (161 places in 71 files: the source's comments, the
  window title, `PORT_AUTHOR`, the scripts, README, LICENSE, the installer) became `MaxPayne2-nx`; the installer
  page shows game.json's new `name` (heading, title, "Create MaxPayne2-nx.zip"). **Kept, deliberately:** the file
  and folder names `mp2-nx.nro`, `mp2-nx.toml` and the SD card folder `switch/mp2-nx/` (the player's settings and
  saves are there, and the card's layout and every guide use it), `exemplos/mp2`, this local folder's name `MP2-nx`
  (the tools' paths), and these history notes, which keep the name each step had. The v1.0.0 release is "MaxPayne2-nx
  1.0.0", its link updated. Checked live: the page's heading, button, links and service worker under the new name,
  the zip's folder still `switch/mp2-nx/`, no console errors.
- **Renamed MPayne2-nx, files and folders too** (the user, 11-26: another game's files share the SD card folder
  `switch/mp2-nx`, "so can we change all the entries to that of MPayne2-nx", and the repository from MaxPayne2-nx
  too). In the source (d2887a0): `PORT_NAME MPayne2-nx` in `exemplos/mp2/port.cmake`, which names the NRO
  (`MPayne2-nx.nro`), its folder (`sdmc:/switch/MPayne2-nx`) and the PC exe (`MPayne2-nx_pc.exe`); the settings file
  `MPayne2-nx.toml` (`game_info.cpp`) and the package's variants (`package/MPayne2-nx*.toml`); the console receiver's
  root; the scripts; the installer (game.json `name`, `package` and `nro`; the manifest's names and the settings
  file's SHA-256); every `MaxPayne2-nx` and `mp2-nx` in the text (301 places in 84 files, line endings kept).
  `exemplos/mp2` and these notes' file names stay. The checksum slip of the MaxPayne2-nx rename (it changed a comment
  in `release/mp2-nx.toml`, so the live page refused that file and could make no package) was fixed first (e1e7e23).
- The local folders: `MP2-nx` -> `MPayne2-nx`, `mp2-nx-installer` -> `MPayne2-nx-installer` (private: its release now
  carries the new NRO and settings, 0.1.13), `mp2-nx-installer-public` -> `MPayne2-nx-installer-public` (the leftover
  clone of the deleted repository); Ryujinx's SD folder `switch/MPayne2-nx`; `.claude/launch.json`'s installer test
  serves `MPayne2-nx/installer`. The build folders were made again from nothing (their caches held the old path):
  deko3d 5 min, OpenGL 3 min (its configure needs `-DPYTHON_EXECUTABLE` on a fresh folder), CPU 1 min, PC 1 min.
  Every NRO names only `sdmc:/switch/MPayne2-nx`; nothing in any build says mp2-nx. The PC build must be started from
  PowerShell (Git Bash drops `ProgramFiles(x86)`); its `vswhere` message is the kit script's quoting and harmless (the
  build uses `C:\Program Files\LLVM` 23.1.5 as before; Visual Studio's own LLVM is not installed, and the user was
  advised against installing it: it would quietly switch the PC compiler).
- Checked: Ryujinx (level 27 from `sdmc:/switch/MPayne2-nx/game`, 60 fps; the 13 "wait stall" lines every run has),
  the PC (menu and music after 60 s), the installer end to end (429 game files, the NRO and the settings byte for byte
  under `switch/MPayne2-nx/`, 1.79 GB in 137 s; the test zip deleted), the package made again (`out/package`; the old
  one kept as `out/package_1124`).
- **The console** (the owner's word, 11-26, the console on Home): `tools/switch_remote.py` gained `rename` and
  `move-root` (the target must not exist, the state read back, no delete). `/switch/mp2-nx` became
  `/switch/MPayne2-nx`, `mp2-nx.toml` -> `MPayne2-nx.toml` (the owner's settings kept), the new NRO in place (read
  back: cef3fcfd, the build's), the receiver rebuilt for the new root (e6f01e07); `out/switch_sd_root.txt` is now
  `/switch/MPayne2-nx`. Then, on the owner's word ("Go ahead and delete the old test builds"), the 17 old test NROs
  (`mp2-nx_*.nro`, `mp2-nx-dk.nro`, `mp2-nx-pgo.nro`; they looked for the old folder) were deleted by a one-off
  script that matched only those names; everything else on the card stayed.
- `pc-test` (the owner's PC folder): `mp2-nx.toml` -> `MPayne2-nx.toml` (kept), the new `MPayne2-nx_pc.exe` in place of
  `mp2-nx_pc.exe`, the README and the shortcut's target updated; the saves (`hdd`) untouched.
- **GitHub:** the repository is https://github.com/maafeu/MPayne2-nx (GitHub forwards MP2-nx and MaxPayne2-nx there),
  the page https://maafeu.github.io/MPayne2-nx/ (the MaxPayne2-nx page address is a 404). Release **v1.0.1** ("MPayne2-nx
  1.0.1", Latest, tag on 15ed1f8): v1.0.0's notes with the new names and how to move a 1.0.0 install; v1.0.0 is
  "MPayne2-nx 1.0.0" with a line saying 1.0.1 supersedes it (its source still builds `mp2-nx.nro`).

## The guest space on 2 MB boundaries (2026-11-28; the user: "Yes please")

- From the Max Payne port (MPayne-nx f1cbb90), which shares this kit's `kernel/memory.cpp`: the kernel maps memory
  with the largest block (2 MB, or 64 KB) that the virtual address, the physical address and the length allow, else
  4 KB pages. The 4 GB guest space sat at any 4 KB boundary `virtmemFindAslr` gave and its backing heap blocks at
  4 KB, so the guest's memory was mapped 4 KB at a time and the A57's translation cache (1024 entries: 4 MB that way)
  can miss on the game's scattered reads. There, 2 MB placement cut 2.8 % off the frame in Docks B-1 at 1020 MHz,
  repeatably.
- The same change here: `guest_align` (`app.h`, read by `host-switch/main.cpp`, boot only, never written back,
  default 1) reserves the space plus 2 MB and starts it at the first 2 MB boundary; each backing block is
  `memalign`ed at 2 MB when it is 2 MB long and on a 2 MB guest boundary, else 64 KB, falling back to 4 KB rather
  than failing. 0 keeps the earlier placement. Only where the memory sits changes, never what the game computes.
- Built: deko3d NRO 8e8de9df (v1.0.1's cef3fcfd kept in the scratchpad as the baseline). Ryujinx `align_dk1`
  (level 27, 180 s): `memory: guest_align 1`, the four blocks at 0x200a00000, 0x204c00000, 0x20a600000 and
  0x20b000000 (all 2 MB-aligned), menu and level drawn right, 60 fps, the same 13 stall-report error lines as
  v1.0.1's `rename_dk2`.
- **The console** (series align1, 2026-11-28 07:34-07:57 console time; the owner: "Yes, measure it"; the Max Payne
  session held its runs meanwhile): one NRO (8e8de9df, netloaded as `MPayne2-nx_align.nro` from hbmenu on the
  Arkanoid tile, title `010091D01597C000` checked each time), the owner's settings with a free frame rate
  (`fps_adaptive = 0`) and `guest_align` 0 or 1, ABBA; input script `mp2_resume_console.txt` (Resume Game, then no
  presses): the owner's newest save, Max standing before a police officer in 20_Third_Dream, the same 118 draws and
  1236 KB a frame in every run. Handheld, CPU 1581 MHz, memory 1600 (Horizon-OC's profile and the settings agree).
  The measure: the title thread's work a frame, 16.667 ms less its wait for the frame before (the `dk:` row, to
  0.01 ms), median of the 5 s windows from 105 s to the HOME press (22-23 windows a run):

  | Run (log) | guest_align | Work a frame | Spread in the run | GPU |
  |---|---|---|---|---|
  | 1 (mp2_064) | 0 | 9.007 ms | 8.99-9.03 | 11.02 ms |
  | 2 (mp2_065) | 1 | 8.707 ms | 8.70-8.79 | 10.98 ms |
  | 3 (mp2_066) | 1 | 8.732 ms | 8.72-8.76 | 11.02 ms |
  | 4 (mp2_067) | 0 | 8.962 ms | 8.95-8.98 | 11.02 ms |

  **0.27 ms less a frame with the space aligned (8.720 against 8.985, -2.9 %)**, every aligned run under every
  unaligned one, as Max Payne measured (-2.8 %). The aligned runs' heap blocks were at 2 MB boundaries
  (0x3f7ce00000 ...), the unaligned at any 4 KB one (0x30b7253000 ...). No crash; clean `Exit (0)`; the GPU unchanged.
- **The card afterwards:** the owner's `MPayne2-nx.toml` (c3d3c6e6) and the inert `input_script.txt` (fd3005cd) put
  back and read back; the three saves and `Max2Options.cfg` untouched (copied off first to
  `out/console/align1/card_before`, their dates unchanged, no new save); the installed `MPayne2-nx.nro` (v1.0.1,
  cef3fcfd) untouched. New: `MPayne2-nx_align.nro` (the test NRO; the console tool cannot delete) and logs
  mp2_064-067.
- The README gained "Reporting a crash": the log is `sdmc:/switch/MPayne2-nx/logs/mp2_NNN.log` on the Switch
  (numbered per run, the last 100 kept; `crash:` lines; `stderr.log` beside it for the driver, latest run only) and
  `logs\mp2_NNN.log` beside the exe on the PC.

## A player's logs: the clocks fought a clock manager (2026-11-28; the user: "inspect these log files")

Ten logs from another console (`mp2_001`-`010`, 2026-11-25 to 11-28; `sdmc:/switch/mp2-nx`, so a v1.0.0-era build,
`build switch-0.0.1`), read in an isolated folder, nothing run from them:
- **The OpenGL renderer** (`gl: 4.3 Mesa 20.1.0-rc3 ... nouveau`, the renderer on the title's thread), not the deko3d one
  every release is tested with: a build made without devkitPro's `deko3d` package (the CMake default then quietly
  became GL) or the package's `fallback/MPayne2-nx-gl.nro`, which was only ever checked in Ryujinx. Their settings:
  the shipped clocks (1581 / 1600), `vsync = false`, `fps_counter = true`, `oled_screen = 2`, `brightness = 6`.
- **A clock manager on that console** (CPU 1963 MHz, memory 1866 or 2133: an overclock). The port's keeper set the asked
  1581 / 1600 back every second whenever the clock differed, higher included (`clocks.cpp`; logged only 8 times a
  run, so the logs show it only in their first 4 s): for whole sessions the memory clock went 2133 -> 1600 -> 2133
  about once a second, each switch a RAM retraining. In mp2_006 the opposite: the CPU held at 1030 MHz under the 1581
  asked, set again every 10 s.
- **Four runs ended without `Exit (0)` and without a `crash:` line**, mid-play: mp2_001 (~161 s, 03_first_hospital),
  mp2_003 (~346 s, 05_First_Vodka), mp2_004 (~502 s, 05_First_Vodka), mp2_009 (~1744 s, four seconds after loading a
  save, after a 24 minute sleep). mp2_003, 004 and 009 end in binary garbage: the file system was not closed, which a
  killed game does not do (the system closes its files) but a console that froze or rebooted does. Frames were still
  drawn within the last flushed seconds (no 10 s stall report), so it was sudden. Not proven: the clock fight at an
  overclocked memory rate is the likeliest cause these logs show; Atmosphere's `crash_reports` / `fatal_reports` from
  those times would tell.
- Everything else matches this console's healthy runs: the 13 stall-report error lines, `xapi: launch of refused`,
  the dsound exports line, the idle-wait warnings; six runs ended cleanly, one of 6.5 hours with sleeps.

The fixes (1.0.2):
- **`host-switch/clocks.cpp`: the settings are a floor.** A clock already at or above the setting is left as it is
  (`clock: CPU stays at N MHz`); the keeper only raises a clock that fell below it (dock, undock, sleep); a clock
  pulled under it more than three times in a minute is left to whatever holds it (logged once). `test_gpu_clocks`
  still sets exact rates. The shipped settings' comments say "MHz at least" (the installer's `release/MPayne2-nx.toml`
  with them, its SHA-256 in the manifest updated: d2ec2848...).
- **No quiet OpenGL build:** without deko3d the configure stops with what to install, unless `-DPORT_RENDERER=gl` is
  given (checked in a minimal project: dk goes on, gl or soft without the option stop, gl with it goes on). The README
  says so.
- **The log names the build:** `PORT_VERSION` 1.0.2 and the renderer in the first line (`build switch-1.0.2-dk`); every
  build said `switch-0.0.1` before.
- **Checked on the console** (series clk1, 2026-11-28 11:30-11:39 console time; the owner: "Yes, check it"; the Max
  Payne session held its runs): NRO b3d08272 (`switch-1.0.2-dk`, netloaded over the earlier test file
  `MPayne2-nx_align.nro`), the owner's settings with only the clocks changed, Resume Game; Horizon-OC's profile holds
  1581 / 1600 for this title.
  - mp2_068, asked **1020 / 1331** (under the profile, the player's case): `clock: CPU stays at 1581 MHz (asked at
    least 1020)`, `memory stays at 1600 MHz (asked at least 1331)`; no "was reset" line; the CPU 1571-1582 MHz in
    every census for 150 s; `Exit (0)`. The old keeper would have set 1020 / 1331 and fought the profile every second.
  - mp2_069, asked **1785** (over the profile): `CPU 1581 -> 1785`; Horizon-OC put 1581 back at 1.3, 2.3 and 3.3 s
    (raised each time), and at 4.3 s `CPU pulled under 1785 MHz 4 times in a minute (now 1581 MHz): left to the clock
    manager holding it`; 1576-1583 MHz for the rest of the run, nothing switched again; `Exit (0)`.
  - The card afterwards: the owner's settings (c3d3c6e6) and the inert script (fd3005cd) put back and read back, the
    three saves and `Max2Options.cfg` untouched (dates unchanged); logs mp2_068-069.
