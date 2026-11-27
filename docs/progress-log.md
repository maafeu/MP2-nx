# Progress log

Times are the PC's local time (ABT).

## 2026-11-21

- 22:25 Start. Read the task; looked at the inputs.
- 22:30 The disc is an original Xbox (XGD1) image: `default.xbe`, x86. The base at the given path is the
  XboxSwitchPortKit (original Xbox), not a ReXGlue tree. Decision 1 in MP2-notes.md.
- 22:40 `default.xbe` read: TT-012, XDK 5558 throughout; the kit has 5849 only (Decision 2).
- 22:50 No Zydis on the PC and no downloads allowed (Decision 3); no LunarG Vulkan SDK; no Switch NVK build.
- 23:00 Disc extracted to `..\game` (429 files) with `tools/extract_xiso.py`. Repo created from the base's `kit/`.
  (Times below are the PC's clock; the early entries above were estimates and run ahead of it.)
- 22:46 `tools/zydis-lite` written; equal to LLVM's disassembler on all real code of the XBE.
- 22:52 Recompiler and `xbe_symbols` built without downloads. 152/238 XDK names found with the 5849 signatures.
- 22:57 5558 layout checked (same offsets as 5849), SetPalette and BlockOnResource found by hand; 5558 signatures
  signed from MP2. Code generated: 23,559 functions, 140 MB of C++.
- 23:03 PC build links (third-party compat fixes, Cold Fear's by-address hooks moved to MP2's functions).
- 23:06 First PC run: boots, CreateDevice, Bink intro; 16-bit `pusha` (CPUID probe) and the BinkWait guest-0 call
  fixed (BinkWait / BinkNextFrame found).
- 23:08 **PC build at the main menu** (Vulkan on the A2000, 30 fps).
- 23:10–23:28 Menu navigation: input reaches the game, the cursor does not move. Open (notes, "Open problems").
- 23:32 Switch NRO builds with `PORT_RENDERER=soft` (no NVK on the PC). Ryujinx data folder made.
- 23:36 Ryujinx: GCC 16 libgcc's GCS read stopped the JIT → `tools/patch_gcs.py`; set:sys 79 (OLED AUTO) → OLED off
  by default; crash in the CPU renderer → raw-vertex path fixed (found on the PC with `MP2_SOFT=1`).
- 23:52 **Switch build (soft renderer) at the main menu in Ryujinx**, correct picture, about 4 fps there.

## 2026-11-22

- 00:05 Menu "bug" was the test script (an extra A opened How to Play). New Game → game mode → the level load stopped
  on the game's disc error: Z: (utility drive, XBE init flag 1) and T:/U: were never mounted → mounted at boot.
- 00:15 **PC build in gameplay**: chapter 1 (hospital), HUD, 30 fps, picture correct in the screenshots
  (`tools/scripts/mp2_gameplay.txt`: R, the black button, skips the graphic novel).
- 00:20–00:35 Switch soft NRO in Ryujinx into chapter 1's load (2.4–4 fps there; 3D picture wrong in the CPU
  renderer). Widescreen checked: the game draws anamorphic 16:9 when told the TV is widescreen (notes, defaults).
- 00:40 SPIRV-Cross found on the PC; HLSL → SPIR-V → GLSL → DKSH proven for all 15 shader kinds. Decision 4b: an
  OpenGL renderer on switch-mesa, written and checked on the PC first.
- 00:46 Console package built (soft renderer): `out/package`.
- 01:10 `host-switch/gl_backend.cpp` + `gl_shader.cpp` written; PC build with `MP2_GL=1`: **chapter 1 at a locked
  30 fps through OpenGL**, no GL errors.
- 01:15–01:40 GL against Vulkan with RenderDoc on both: static scene pixel-identical; Max differs only because the game
  uploaded another animation pose (bone constants differ, everything else equal). Two Vulkan runs match each other.
- 01:45 Switch build with `PORT_RENDERER=gl` (switch-mesa: EGL, glapi, drm_nouveau; glslang needs the PC's Python
  for its build info).
- 01:50–01:55 Switch build fixes for the GL renderer: glslang's dependency files (msys2 make "multiple target
  patterns") → CMake's own scanner plus empty `compiler_depend.ts` stamps; glslang's OSDependent for a non-Unix
  target; switch-mesa from portlibs. **GL NRO built** (25.8 MB).
- 01:55 **Ryujinx: chapter 1 at 30 fps through OpenGL** (Mesa 20.1 nouveau, GL 4.3 core). The port's captures came back
  a quarter wide; a GL self-test showed every read-back is (even a clear), and Ryujinx's own window shows the full,
  correct frame (02:25, `-WindowShots`). PC GL build checked again: 30 fps in chapter 1.
- 02:25 Console package rebuilt with the GL NRO (soft NRO as fallback; `out/package_soft_0046` kept). Handoff patch
  regenerated (36 files, applies to the base). Installer prepared from renut-ultimate's (`mp2-nx-installer`, not
  published) and tested in the browser: the zip it makes from the game folder is right.
- 02:35–02:45 `render_wide` option (off): the GL renderer draws MP2's 16:9 frame at 1280x720. Bink films: not the
  video overlay; traced to the game's film loop (notes).
- 02:45–03:10 A header change did not rebuild its dependents on the Switch (the dependency change of 01:50 had left
  `depend.make` empty): a mixed NRO crashed Ryujinx (OLED "auto" read from a shifted field). GCC's dependency files
  are back for the port's own code (off only for glslang); all host objects rebuilt; a header touch now rebuilds its
  users. The packaged 02:12 NRO predates the header change and is consistent.
- 03:00 Ryujinx window pictures turned black for every build, the packaged one included: the PC's display is off
  overnight, so screen and window captures are black. The port's own captures still work (quarter wide under Ryujinx).
  New NRO checked that way: default and `render_wide` both draw chapter 1 at 29–30 fps.
- 03:10–03:18 GL renderer: unchanged state skipped (chapter 1: 385 state calls a frame made, 1412 skipped) and the
  ring-buffer split fixed; Ryujinx checked; package rebuilt.
- 03:20–03:30 **Bink films fixed**: the engine pushes each film frame's quad as an inline array; `Walk` now draws
  those and runs before each present (found with a new trace of every hooked call). Intro film shown on the PC; in
  Ryujinx film frames draw (1 draw each, 0 before); chapter 1 unchanged on both.
- 03:35–03:50 GL renderer draws the kit's options panel and FPS counter (ImGui): Minus + R3 had opened an invisible
  panel that swallowed input. Checked on the PC (panel opens, moves, closes) and in Ryujinx (FPS counter drawn);
  chapter 1 unchanged on both. Package rebuilt with this build.
- 03:55–04:00 Package, installer (0.0.6) and handoff patch on the final build (git 6ab6bf9, NRO 759A9C00...).
  Exploratory PC check (walking, firing, bullet-time dive, back to the menu) on GL and Vulkan: the same picture
  wherever the game state matches (`tools/scripts/mp2_gameplay_move.txt`).
- 04:00–04:12 Ryujinx soak: 12 minutes, 20,400 frames at 29.7–30 fps in chapter 1, GL object counts flat, no crash.
- 04:15 The GL renderer logs its own CPU time on the title's thread: chapter 1, PC 1.63 ms of game 5.2 + present 0.2;
  Ryujinx 5.48 ms of game 10.7 + present 0.9. Final package: git 40254aa, NRO 1D7E7ABE..., checked in Ryujinx;
  installer 0.0.7 and the handoff patch (37 files) match it. Report: `out/REPORT.md`.

### 2026-11-22 morning: full testing (the user: "full testing, using all resources available")

- 06:25–06:40 Console (no other session running): sys-botbase answers, **sys-ftpd-light refuses every connection**.
  Its pause combos (from the SD backup's configs) did not bring it back. `tools/switch_remote.py` copied from the
  base with `mkdir`, `put-tree`, `netload` (hbmenu's nxlink protocol, the file always inside the port's folder) and
  the `r*` commands for a new receiver app (`tools/console_recv`, TCP 5005, the port's folder only, no delete).
  hbmenu (Arkanoid tile with R, title checked) took the netload and answered "No such file or directory": the
  port's folder does not exist on the card and hbmenu does not make folders. Nothing written; console left on the
  Home menu. **The console run waits for `sdmc:/switch/mp2-nx` to exist or the file server to come back.**
- 06:40–07:25 `test_level` (a test aid): New Game is a load from a stream made for 03_First_Hospital, so swapping
  the folder or the name crashed (levels 2, 13, 14). The game's own developer script command does it:
  `maxpayne_gamemode->GM_InitSingleLevel(<level>);` run on the game mode from its per-frame update
  (`src/game/test_level.cpp`). Level 2 (04_Warehouse) plays at 30 fps on the PC. Every level init is now logged.
- 07:05–08:15 PC batch of levels 2–30 (`tools/run_level_tests.ps1`); Switch NRO rebuilt with the hook (07:09).
- 07:10–08:00 Ryujinx, 11 levels (5, 8, 11, 13, 16, 19, 20, 22, 24, 27, 30) alongside the PC batch: each loaded,
  played and drew the right picture in Ryujinx's window (the display is on again), 28–30 fps, game 8–21 ms a frame
  in play (notes: the table). `run_ryujinx.ps1` now waits for the stopped emulator before copying its log.
- 08:05 Package rebuilt (`out/package`, git 5130649, NRO E197A773...; the 04:15 one kept as
  `out/package_gl_0415`); installer manifest 0.0.8. The handoff patch's files did not change.
- 08:13 PC batch done: all 29 levels loaded, played, 30 fps, no crash. 08:15 the emulator SD had kept a
  `test_level = 8` settings file (the level 8 run's restore failed while the stopped emulator held it): removed (it
  was absent before), and `run_ryujinx.ps1`'s restore now retries and warns.
- 08:20–08:30 Kit fix: the wait stall report logged with the dispatcher lock held, its "seconds" counted in wakes
  (notes). PC and Switch rebuilt, checked on the PC and in Ryujinx; handed off (row 18, patch 38 files, applies).
  GL vs Vulkan on levels 19 and 27: the same picture.
- 08:38 The user's cover picture as the NRO's icon (`exemplos/mp2/icon.jpg`, not in git); NRO rebuilt.
- 08:40 The user made `sdmc:/switch/mp2-nx` (DBI over USB; no presses while its MTP screen was up). Receiver
  netloaded through hbmenu (title checked), its LIST fixed (no `rewinddir` on the console) and loaded again; NRO and
  settings uploaded (2.4 MB/s); the game folder uploading (about 12 minutes). The user: work on without asking until
  the game is done.
- 08:56 The upload stopped at 389 of 429 files: the console had gone to sleep (nobody pressed anything for 10
  minutes) and does not answer until someone wakes it. Fixed for next time: the receiver and scripted runs keep
  auto-sleep off (`appletSetAutoSleepDisabled`, checked in Ryujinx). A background watch waits for the console.
- 09:00–09:27 Meanwhile: the ending (final graphic novel and closing scene) and three survival maps play on the PC
  (`test_script`); the frame lines now split long frames by game time and count read-backs: Ryujinx's one long
  frame a second is game time with no read-backs (most likely the emulator). Package (git a292878, NRO 2703D23F...)
  and installer 0.1.0 ready for the console.
- 11:57 The console woke by itself for about 40 s (background tasks) and slept again before anything could be sent;
  the watch now wants five answers in a row over a minute.
- 12:00–12:25 PC: the game itself has no once-a-second spike (longest game part 10–12 ms per 300 frames). A sampling
  profiler for the PC build (`host-pc/profiler.cpp`, `MP2_PROF`): the title's thread works about 7.3 ms a frame,
  69% of it the game's recompiled code spread thin (top function 5–8%), 20–25% the GL renderer (texture hashing,
  vertex assembly, driver). Nothing cheap and pixel-identical to take before the console's figures (notes).
- 12:25–12:50 Films: the profiler showed Bink's MMX colour conversion (`sub_002CC910`) at 83% of the title thread
  during a film. `src/game/native_bink.cpp` does it natively, bit-exact (shadow mode: 0 mismatches in 98,000 calls on
  the PC and 32,768 on the NRO in Ryujinx); the film texture's upload no longer re-decodes texel by texel nor copies
  into a zeroed buffer (kit `DecodeLevel` fast path, handed off as row 19; upload hashes identical). PC film frame
  11.3 -> 4.4 ms of game time; in Ryujinx the conversion is 33 ms per 8192 calls against the original's 365 ms (about
  1 ms a film frame instead of 10.7), the film frame 3.9 ms of game time against 32.9. Levels play as before.
- 14:00 The console stayed awake (the watch: five answers over a minute). The old receiver (08:56) no longer listened;
  PLUS closed it. Starting hbmenu with R held failed three times (the real Arkanoid started each time: closed at once
  from the Home menu, HOME, X, A, nothing in it touched). What works: R pressed, 3 s, R pressed again, then A, R kept
  held about 8 s (sys-botbase's virtual controller needs time to attach after a reconnect). New receiver netloaded
  (auto-sleep off while it runs); the rest of the game folder uploading.
- 14:22 Console run 1 (NRO A66A5C80, chapter 1 script, `capture` and the GL self-test on): boot, legal screen, films,
  loading screen and the main menu drawn right on the console; the self-test read back full rows (256/256, 960/960,
  1280/1280: the game's read-backs will work; Ryujinx's quarter rows were the emulator). But each capture (every 15 s)
  took 10-20 s to write row by row to the SD and stalled the game: 3 frames in the first 55 s, the script's presses
  lost, the game left on the menu. Clean exit (HOME, X, A: `Exit (0)`). Fixed: the BMP in one write, and the
  `capture` file's text sets the interval ("0" off), so the file can stay on the card (nothing deleted there).
- 14:30 The user: the target is now 60 fps locked. Decision: `fps_cap = 60` as the route (the port's own pacer; the
  game presents immediately and times itself by `rdtsc`, which the runtime returns as real time at 733 MHz, so it
  plays at the same speed at 60 as at 30, as the PC version of the game does at any rate). PC, level 27 (the
  heaviest), `fps_cap = 60`: 60.0 fps held, game 3.5 ms, renderer 1.1 ms a frame.
- 14:32 Console run 2 (NRO 1F887E42: the capture fix; `capture` = 0, `gl_flags.txt` = none): the script went New
  Game -> graphic novel -> chapter 1 by itself; chapter 1 drawn right on the console (screenshots).
- 14:52 Console run 3 (NRO 5E7144E2: clocks, renderer parts, slow-read log; 60 fps cap, `cpu_clock = 1785`): chapter
  1 at 47.9 fps, game 19.4 ms (renderer 9.3: assembly 1.2, textures 1.3, GL calls 2.9, the rest 3.8). The
  once-a-second long frame found: every ~39 frames the renderer's "rest" took 15-40 ms, the 32 MB stream buffer
  orphaned (`glBufferData`) and reallocated by Mesa. The films' native conversion took 260 ms per 8192 calls as at
  1020 MHz: the clock had not sped the title's work up.
- 15:07 Persistent mapped upload rings (4 fenced segments; `glBufferSubData` gone from the draws). PC: the same
  picture, 0 fence waits, GPU lag 0.4 ms. Committed 005c375.
- 15:10 Console run 4 (NRO 6B25131F, the same settings): the long frames gone (0-2 over 40 ms per 300 frames, was ~8);
  chapter 1 at 52.2 fps, game 18.4 ms, renderer 8.1 ms. The CPU measured 1020 MHz before the change and 1785 after
  (a chain of adds timed on the main thread), yet the films' conversion on the title's thread stayed at 260 ms. Next:
  a thread census (each guest thread's CPU and core every 10 s, the clock measured on the keeper's own ticks) and
  `thread_cores` (libnx starts every pthread on the process's one core): runs 5 (off) and 6 (on), the same NRO.
- 15:27-15:45 Console runs 5 and 6 (thread census): run 5 logged no census (the keeper at priority 0x3F never ran);
  run 6 with the keeper at 0x20: the system put the CPU back to 1020 MHz every second for 8 s and once at 20 s, the
  keeper restored it, the census measured 1783 MHz, chapter 1 at 59.6-59.7 fps, game 12.0 ms. The title's thread is
  81% of a core; guest threads already have cores 0-2. Committed 4cf0ce9 (with the buffered log).
- 15:50 Console run 7 (the log fix): no frame over 40 ms in play (longest 22-24 ms), but the CPU stayed at 1017-1020
  MHz the whole run (Horizon-OC's governor on the console; its config is outside the port's folder): 54.6 fps,
  game 17.0 ms. Decision: 60 fps at the system's clock, no reliance on the overclock (notes).
- 16:00-16:20 At 1020 MHz: four-lane texture signatures, index lists written in place (PC profile: hashing 8.1 ->
  6.2%, assembly 8.6 -> 4.7% of the title's thread; the same picture). The recompiler's x87 block cache written and the
  game's code regenerated (62 files; the old code in build/generated_nocache).
- 16:38-17:10 Console runs 9-11: the console profiler (COLD-FEAR's, ported: host-switch/prof.cpp) found the renderer's
  draw path a third of the title's thread; fixed from it: x86_icall cache, CRC32C texture signatures, vsc_gen,
  fixed draw-constant buffer, vectorised index scan. Chapter 1 at 1020 MHz: 55.8 -> 57.3 fps.
- 17:20 Console run 12, level 27 (the heaviest) at 60: about 40 fps at 1020 MHz (game 24.3 ms). Decision: adaptive 60
  (fps_adaptive: 60 where the console holds it, locked to 30 where it cannot), cpu_clock 1785 in the shipped settings,
  mp2-nx_30.toml for 30 at 1020 MHz (notes, "Level 27 and the decision").
- 17:31-17:58 Console runs 13-15 with the shipped settings: menus, novel and films at 60; level 27 and chapter 1 (the
  CPU held at 1020 MHz by the console's Horizon-OC) locked to 30.0 fps; the swap interval paces the lock (2 at 30, 1 at
  60) instead of a timer on top of vsync; the adaptive measure is the title's thread's CPU time (wall time could not
  come back to 60 once the swap waited in it).
- 18:05 Package `out\package` (git 30c0510, NRO 930680FC, ELF 6CB1AB23; the 12:50 one kept as out\package_gl_1250),
  installer manifest 0.1.2, handoff patch 54 files (applies to the base; HANDOFF rows 21-28), REPORT updated. The
  console's folder: the package's NRO and shipped settings, the run files inert (capture 0, gl_flags none,
  input_script comments only); the console left on the Home menu, nothing running.
- 18:20 The user: "Try deko3d". deko3d 0.5.0 and uam 1.1.0 already in devkitPro (no download); the ultimate base
  carries uam as a library that compiles on the console (`renut-ultimate/base/app/thirdparty/uam`): copied into
  `third_party/uam`. Decision: `PORT_RENDERER=dk`, a deko3d copy of the GL renderer, shaders compiled by uam on the
  console and cached (notes, "The deko3d renderer").
- 19:10 `host-switch/dk_backend.cpp` written (3270 lines: the GL renderer's helpers verbatim, the rest new), CMake's
  `dk` renderer (uamlib, libdeko3d); host_check compiles clean; the full build (build/mp2-dk) started.
- 19:50-21:00 Ryujinx: the renderer runs; culling, then the speckled railings (deko3d 0.5.0's tile height bug: the
  ultimate base's fix taken); chapter 1 and level 27 as the GL renderer draws them, 60 fps in the emulator.
- 21:15 Console run dk1 (the deko3d NRO as mp2-nx-dk.nro beside the package's, netloaded): the game ran, the screen
  black: the negative viewport height emptied deko3d's viewport clip rectangle. deko3d's origin is GL's
  UPPER_LEFT clip control: GL's positive viewport and front face (checked in Ryujinx: the self-test's texture red on
  top, the frame upright).
- 21:27 Console run dk2: menus, novel, chapter 1 drawn right; **chapter 1 60.0 fps locked at 1020 MHz** (game 12.9
  ms, renderer 4.1; GL 57 fps). 21:37 run dk3: level 27 46 fps (GL 40), renderer 6.5 ms (GL 9.7).
- 21:47 Run dk4, the profiler at level 27: the game's own code 65.5% of the title's thread, the renderer 27.5%.
- 22:00 Run dk5: cached rings and a sampler cache (level 27 the same); no texture ever changed without a lock: the
  signatures every 4th frame of a texture. The adaptive cap's way back at 14.5 ms (deko3d's chapter 1 is ~13 ms).
- 22:11 Run dk6, the shipped settings: chapter 1 at 60.0 fps throughout (CPU held at 1020), no drop to 30.
- 22:20 Decision: deko3d is the renderer (the Switch default; package NRO B5AE806A, git 56bca4f, the GL build as
  fallback/mp2-nx-gl.nro); installer manifest 0.1.3; handoff patch 56 files (row 29: the deko3d renderer). The
  console: the package's NRO and shipped settings, the run files inert (capture 0, dk_flags none, input_script
  comments only); the console on the Home menu, nothing running.
- 22:30 The user: "Go ahead" (level 27 at 60 on 1020 MHz by speeding up the game's own code), then "use all the
  processing resources you can find": builds at 30 jobs on the PC's 32 threads, the console for every measurement.
- 22:49 Run B0: the console's Horizon-OC let the port's 1785 MHz stand in level 27: 60.0 fps, game 13.5 ms. A/B runs
  from here use `cpu_clock = 0` and `tools/ab_compare.py` (windows compared at the same clock).
- 22:50-23:59 PGO (GCC 16.1, `PORT_PGO`, `host-switch/pgo.cpp`, `kernel::OnExit`): the instrumented NRO's TLS crash
  fixed with `-fno-profile-values`; three training runs; A/B abA/abB: neutral (chapter 1 12.70 against 12.50 ms,
  level 27 20.35 against 20.60). Not shipped. `test_level_after` and `test_levels` added for these runs.
- 00:19 Run survey1 (`test_levels`, 9 levels at 1020 MHz): all at 60 but the Crackhouse (57.4), Gognitti's Lot (54.0)
  and the Manor (about 46).
- 00:24-01:20 Vertex conversion per element and draw, `NoInitVector`, conversion tables, the converted path's scratch
  without TLS; deko3d colour table, target and shader last-hit caches, vertex counters. Bit-exact:
  `tools/tests/convert_exact.sh` (all type bytes, both index paths, every packed field; -O0/-O2/-O3).
- 00:49-01:36 Console runs A0 / B / prof2 / cut2 (same session, 1020 MHz): with about the same draws in level 27, the
  renderer 6.13 -> 5.14 ms, game 21.2 -> 20.2 ms, 45.2 -> 47.6 fps; assembly 1.73 -> 1.22 ms for the same 36
  converted draws; chapter 1 60 locked.
- 01:25 The port's log rotation (30 kept) removed `logs/mp2_001.log` on the SD at the 31st run (a copy is
  `out/console/run1/mp2_001.log`); 100 kept from the cut3 build on.
- 01:48 Run survey2 (cut3): the Crackhouse 58.7 / 49.6 fps (its two parts), Gognitti's Lot 54.7, the renderer 2.7-3.5
  ms there: the game's own code alone fills the frame at 1020 MHz. Decision: the cuts ship; 60 in the heaviest
  levels needs the 1785 MHz the shipped settings ask for (notes, "Level 27 at 1020 MHz").
- 02:05 Commit b09fb50; package `out\package` (NRO 5897A967, ELF 1BE98EE4; GL fallback A1D256A4 checked in Ryujinx at
  level 27; the soft fallback as before); installer manifest 0.1.4; handoff patch 63 files (row 30), applies to the
  base. The console: the package's NRO and shipped settings, the run files inert (capture 0, dk_flags and gl_flags
  none, input_script comments only, all read back); netload's copies of the test NROs (mp2-nx_survey, _cut1-3) and the
  earlier test files left in place (nothing deleted); the console on the Home menu, nothing running.
- 02:10 The user asked whether 60 locked is possible on the stock CPU settings; the answer: not with the renderer on
  the title's thread (its own code 13-17 ms at 1020 MHz); the user: "Yes, try both" (the renderer on its own core,
  and compiler work on the game's code).
- 02:20 Render thread for deko3d: the kit's threaded wrapper, `render_thread` in the settings, `Backend::Adopt`,
  `WaitResource` in BlockOnResource for textures and surfaces, the adaptive cap on the busier thread. Ryujinx: level
  27, films, menus drawn as before.
- 02:28-02:48 Console A/B (the same NRO, `render_thread` 0 then 1): level 27 45.3 / 46.7 fps against 55.2 / 51.8.
- 02:51-03:01 Profile with the render thread: the title's thread 1.37 ms a frame of recording (`SendState`), no waits.
- 03:05-03:36 The vertex constants skipped while `vsc_gen` holds; LTO (`PORT_LTO`): built in 3 min, runs; console:
  level 27 rows at 60.0 / 58.1 fps (rt3) and LTO no better at the same draw counts. LTO stays an option (off).
  Ryujinx's window captures turned black from 03:15 for every build (the PC's display asleep); the port's own captures
  and the console's screenshots were used from then.
- 03:41-03:55 Only the vertex constant registers written since the last send compared (`VscWritten`, all eight writes
  to `vsc`): the recording a draw about 5% lower on the console.
- 04:06 Run survey3 with the render thread at 1020 MHz: the Crackhouse 60.0 / 56.6 fps, Gognitti's Lot 57.0, the
  Manor 58.1 (before: 58.7 / 49.6 / 54.7 / about 46). The GL fallback rebuilt (the state layout changed) and checked.
- 04:20 Commit 9a8a5f4; package `out\package` (NRO B3C4D849, ELF E411D45C; GL fallback B752D183; the soft fallback as
  before; the shipped settings gain `render_thread = 1`); installer manifest 0.1.5; handoff patch 65 files (row 31),
  applies to the base. The console: the package's NRO and shipped settings, the run files inert (all read back); the
  test NROs netload saved (mp2-nx_rt1, _rt2, _rt3, _rt4, _lto1) left in place; on the Home menu, nothing running.
- 06:45 The user: "Let them run free" (the near-60 scenes at 1020 MHz free instead of locked to 30). Decision:
  `fps_adaptive_ms` (20 by default; 30 only past 20 ms a frame on average, back under 18.5; 16 is the old behaviour)
  in `app.h`, `host-switch/main.cpp`, the four package settings files and `package/TESTING.md`.
- 06:50 The user asked whether those scenes could be locked to 55. `fps_cap = 55` already works (the pacer's timer),
  but on the 60 Hz screen 55 fps means 5 repeated frames a second either way; measured on the console (run cap55).
- 06:54-07:01 Builds: this session's tool shell is now Git Bash, whose environment did not reach devkitPro's msys2
  (GCC had no TMP and tried `C:\WINDOWS`; a WinGet Ninja on PATH asked an existing Makefiles folder for Ninja). Builds
  now go through devkitPro's own login shell with a clean environment; the dk cache's `CMAKE_CACHEFILE_DIR`, which the
  failed configure had rewritten to the `/home` alias of the same folder, put back. deko3d NRO 8B8D138F (a full
  rebuild, 187 s), GL fallback 12F95E57; every object that includes `app.h` rebuilt in both.
- 07:02 Run ad20 (first try): the console still had the inert input script from 04:20, so the game sat in the main
  menu (log mp2_040 kept as `mp2_040_menu.log`: the setting read, the intro and attract films locked to 30 as before).
- 07:13 Run ad20 (survey3's settings with `fps_adaptive = 1`, `fps_adaptive_ms = 20`, survey3's Resume Game script):
  the fights the 16 ms line locked to 30 now run free at 54-58 fps (the Manor 57.0 for 100 s, the Crackhouse's second
  part 54-60 throughout); 30 only past 20 ms: three level starts (8-15 s each; 49-53 fps free in survey3) and the
  Manor's heaviest fight (19 s and 7 s; 43-50 fps free). About 55 s of 430 s of play at 30.
- 07:24 Run cap55 (the same NRO and course, `fps_cap = 55`): 55.0 fps where the others hold 60, 52-54.5 in the fights
  where running free gives 54-58, no frame over 40 ms in either. 55 not shipped (`fps_cap = 55` stays available). Its
  Manor stretch was a heavier state of the scene (274 draws a frame against 168, game 24 ms, 39-42 fps).
- 07:40 Commit 6f0be78; package `out\package` (NRO 8B8D138F, ELF 25C4FD9B; GL fallback 12F95E57; the soft fallback as
  before; the shipped settings gain `fps_adaptive_ms = 20`); installer manifest 0.1.6 (its README's release line
  brought up to date); handoff patch 65 files (row 32), applies to the base. The console: the package's NRO and
  shipped settings, the run files inert (capture 0, dk_flags and gl_flags none, input_script comments only, all read
  back); netload's copy of the test NRO (mp2-nx_ad1) left in place with the earlier ones (nothing deleted); on the Home
  menu, nothing running.
- 07:55 The user asked what to do about the levels that do not hold 60 at 1020 MHz; suggested: a clock for this game
  (a Horizon-OC per-title profile, which the user places: the console's clock manager, outside the port's folder)
  and a survey for the lowest clock that holds 60. The user: "lets go with that". The survey: the package's NRO
  (8B8D138F, netloaded as mp2-nx_ad1), survey3's course and settings (free frame rate) with `cpu_clock` (and
  `mem_clock`) per run.
- 08:05 Run clk1428: Horizon-OC held the CPU at 1020 for the first 73 s (the port set it again each time), then
  1419-1431 MHz to the end. Crackhouse 2 60 throughout; the heaviest fights CPU-bound at 57.2-58.7 fps (game 16.7-17.4
  ms); single long frames at level starts (45-178 ms) at Gognitti's Lot and the Manor.
- 08:16 Run clk1581: held at 1020 until about 165 s (the Crackhouse's first part ran at 1020), then 1572-1583 MHz.
  Crackhouse 2 60 throughout; the Manor 95% of its windows at 60, one dip at 58.6 fps with game 17.0 ms against 17.4
  at 1428 on the same scene (163-165 draws a frame): 11% more clock, 2% less time; the title's thread 91-95% busy,
  the GPU 10.8 ms. Memory-bound, it seemed: the handheld memory clock is 1331 MHz.
- 08:28 Run clk1581m (`mem_clock = 1600`, the docked rate; the system put it back to 1331 four times in the first 5
  s, the keeper restored it, then it held): every window in play at 60 but single long frames at level starts
  (105-179 ms). The Manor's fight at the same draws 15.2-15.7 ms (17.0 without), every level about 1 ms faster.
- 08:39 Run clk1428m (1428 MHz, memory 1600): the heaviest fights still 58.1-59.4 fps (the Manor's game 17.1 ms on
  the same scene). 1581 with the memory at 1600 is the lowest pair that held 60 in every window of play.
- 08:50 Decision: the shipped settings (and `_ab`, `_play`) and the NRO's defaults ask for `cpu_clock = 1581`,
  `mem_clock = 1600` (1785 and the system's memory before); `TESTING.md` gives the survey and the profile. The
  Horizon-OC profile for the user, `out\horizon-oc\010091D01597C000.ini` (`handheld_cpu=1581`, `docked_cpu=1581`,
  `handheld_mem=1600`): the user places it (the console's clock manager, outside the port's folder; the user offered
  access, declined: a system setting). NROs rebuilt: deko3d 0004B3B0, GL fallback 4901F24D (only `main.cpp`).
- 08:55 Commit d1f9a16; package `out\package` (NRO 0004B3B0, ELF 606CE7A7; GL fallback 4901F24D; the soft fallback as
  before; the shipped settings ask for 1581 / 1600); installer manifest 0.1.7; handoff patch 65 files, applies to the
  base. The console: the package's NRO (netloaded under its own name, so the card's `mp2-nx.nro` is the package's,
  read back), the shipped settings and the run files inert (all read back); a boot to the main menu with them (log
  mp2_047: `cpu_clock = 1581`, `mem_clock = 1600` read, both clocks set, the renderer on its own thread; the same 13
  boot wait-stall lines every log has; Horizon-OC held the CPU at 1020 for the 70 s it ran, as in the survey); on the
  Home menu, nothing running. No new test NRO on the card (the survey netloaded mp2-nx_ad1 again, the same file).
- 17:00 The user: "Can you please do that for me" (place the Horizon-OC profile). The receiver (tools/console_recv)
  gained one exception, Horizon-OC's config only: HOC_LIST, HOC_GET, HOC_APPEND (append-only, refused if the file
  changed since it was read or already has the title's section, the old file kept in the port's folder), then the
  read-only CFG_LIST (sdmc:/config, one level) and SVC_HAS (sm's HasService for hoc:clk and sys:clk); switch_remote.py
  hoc-ls / hoc-get / hoc-append / cfg-ls / svc-has, no path arguments. Found: the console's Horizon-OC sysmodule
  (HATS 2026-11-09) reads only `config.ini` `[TID]` sections, no `profiles/` folder (the 09:00 path was wrong);
  on the console `sdmc:/config/horizon-oc` does not exist while `hoc:clk` runs (defaults, nothing saved). Not done:
  making Horizon-OC's folder and config (its first configuration here; it writes its KIP from that file; its source
  could not be read to check): the user's call. `out\horizon-oc\mp2-nx_section.ini` replaces the wrong profile file.
  Nothing written outside the port's folder; the card's receiver is the new build (4dde... with SVC_HAS); console on
  Home, nothing running.
- 17:25 The user copied the HATS pack's `config/horizon-oc` folder to the card and restarted ("back up"). Horizon-OC
  wrote no `config.ini`; the receiver's new HOC_INIT made it from the folder's own `config.ini.template` (byte for
  byte, only because none existed; equal to the pack's template), HOC_APPEND added the `[010091D01597C000]` section
  (`handheld_cpu=1581`, `docked_cpu=1581`, `handheld_mem=1600`, CRLF; read back equal; the pre-append file kept as
  `hoc_config_before.ini` in the port's folder).
- 17:40 Check run, log mp2_048 (package NRO, survey3's course, `cpu_clock = 0`, `mem_clock = 1600`): CPU 1581 MHz at
  0.4 s before any change, memory `1600 -> 1600`, every census 1570-1583 MHz to 480 s, no reset; every play window of
  the four heavy levels 60 fps (longest frame 17-23 ms; the Manor's heavy stretch game 15.5-15.9 ms). Horizon-OC did
  not rewrite `config.ini`. Run files back to shipped (toml 9b572151, inert input script, read back); console on
  Home, nothing running.
- 17:45-18:25 Console sweeps of every level (runs A, B: logs mp2_049, mp2_050; package NRO, shipped clocks held by
  the profile, free frame rate, 65 s a level): 59-60 fps in play everywhere except level 19's GPU-bound stretch
  (55.6-57.3, GPU 14.5-16.4 ms) and level-start frames; two hangs at a level change (to 16 and to 29: level init, then
  the title's thread at 100% with no frame and no loader). PC: the default cap now 60 (host-pc/main.cpp), built,
  levels 22/24/27 at 60.0; 15 -> 16 on PC GL and 27 changes 15 <-> 16 on PC Vulkan with the render thread: no hang.
- 18:24 Console stress (15 <-> 16 every 20 s, profiler on): a crash dialog at about 10 min ("Do Not Share" chosen);
  the console then slept and stopped answering (a push notification to the user was not delivered: Remote Control
  off). Log mp2_051 and prof_051 stay on the card.
- 18:55-19:00 Built (not run): the stall report in the clock keeper and the guest_quantum_ms test aid (NRO
  17674013..., out/console/hoc/mp2-nx_stall.nro, run files mp2-nx_stall1516 / mp2-nx_quantum1516). Notes: "Every level
  on the console, and a level-change hang".
- 19:11 The console awake (the user: "It's awake"), on Home. The session's tool refused the presses that start hbmenu
  (twice, "destructive or unwanted"): no console run, nothing on the card touched.
- 19:15-20:20 The level-change hang found in the code (notes: "The level-change hang: ended guest threads kept their
  64 MB stacks"): the level init waits in a SwitchToThread loop for its loader thread, which never started because
  every ended guest thread had kept its 64 MB host stack (libnx frees it only on a join; detach is a stub), so after
  about 20 level changes the heap could not hold another. Fix in the runtime (kernel/threads.cpp): ended guest threads
  are joined by a reaper thread; a failed thread start is logged and retried with 8 MB; the thread-start line logs the
  heap in use. Switch NRO 27B7DD09 and the PC build made.
- 20:25-20:50 Ryujinx: run reap2 (fixed NRO, 30 changes 15 <-> 16 every 20 s): all 31 loaders started, heap 768-790
  MB, 60 fps after the last change; one false `stall:` line (fixed: a frame newer than the keeper's clock reading).
  Control run leak_old (package NRO 0004B3B0, same list): hung at the 21st guest thread (level init, no loader, no
  frame), as console run B did. Notes: the level-change hang section.
- 20:55 Commit 1bc0382; package `out\package` (NRO 997D1557, ELF 1094C0C1; GL fallback 1A4630D7, ELF 4498CC59; the
  soft fallback as before, BUILD.txt says it predates the fix); installer release 0.1.8; handoff patch 66 files (row
  33), applies to the base. Not on the console yet: the card still has the stress settings of the 18:24 run.
- 20:52-21:45 Console, the user opening hbmenu (three times; the session pressed Y, and closed the game from Home):
  logs mp2_051 (the profiled stress crash: the 11th code memory object refused, 0xCE01) fetched; run c1 (997D1557,
  the reaper only, log mp2_052): heap flat at 772-773 MB but the same crash at the 24th thread; guest stacks freed at
  thread exit and the system pool and stacks backed 8 MB at a time (PC: 28 changes at 60 fps); run c2 (AC48BB7E, log
  mp2_053): 31 level inits and loaders, heap 779-781 MB, four code memory objects, no error, 14 minutes in play. The
  card back to the shipped settings and inert input script (read back), its NRO AC48BB7E (read back); Home, nothing
  running.
- 21:50 Commit cfe7f01; package `out\package` (NRO AC48BB7E, ELF 28F326A6; GL fallback AC5A1D4C, ELF 808E0343; the
  soft fallback as before, BUILD.txt says it predates the fixes); installer release 0.1.9; handoff patch 67 files
  (row 33: all three fixes), applies to the base.
- 21:55-22:10 Level 19 (the user: "Yes, level 19", work on without asking): the sweep log's stretch is the GPU's
  pixel work (14.5-16.4 ms a frame, ~190 draws, no read-backs, native BC textures). Starting hbmenu from Home refused
  a third time ("undermining oversight"): no console run. Built instead: `test_gpu_clocks` (one run, a GPU step per
  level change; NRO 59565E4D as out/console/hoc/mp2-nx_gpusteps.nro; run file mp2-nx_l19_gpusteps.toml); the soft
  fallback rebuilt with the fixes (31C7458C) and put in the package. Commit 53e8fd4; handoff patch 67 files, applies.
- 22:55-23:10 Console (the user opened hbmenu): the GPU-step run (log mp2_054) ran test level 19 = 14_Second_Dream,
  not 19_Third_Address_Unknown (25): that level held 60.0 fps at the stock 307.2 MHz. The port's GPU rate was reset to
  307 every second, so `gpu_clock` cannot raise it here: the shipped value stays 0; `handheld_gpu=460` in Horizon-OC's
  profile offered to the user (out/horizon-oc/mp2-nx_section_with_gpu.ini), not written. FTP worked again: the log
  fetched and the shipped settings and inert input script put back over it (read back). Console on Home.
- 23:12-23:20 The user added `handheld_gpu=460` to the Horizon-OC profile and opened hbmenu: the line read back;
  run (log mp2_055): 19_Third_Address_Unknown's stretch at 60.0 fps (GPU 9.4-11.4 ms a frame at 460.8 MHz, against
  14.5-16.4 at 307.2 in the sweep). Every level at 60 in play on the console. TESTING.md and the profile section file
  updated; the card back to the shipped settings (FTP, read back); console on Home.
- 11-24 00:20-00:55 "Please continue": saving and loading, never run on purpose before, checked on the PC and in
  Ryujinx: Resume Game, Quick Save, Load Last Game, the main menu's Load (scripts `mp2_*` in tools/scripts). All
  work, 60 fps after each load. The save names' times are UTC: the game builds them from `GetSystemTime` (000F88D0),
  on a real Xbox too: left as is. Fixed on the way: XC_MAX_OS (0xFF) returned zeros, so the guest's time zone was
  UTC; it now answers the user section with the host's offset (HANDOFF row 34; base patch 68 files, applies).
  0xFFFF (the save signature key's source) unchanged. Package unchanged (AC48BB7E); TESTING.md gains a saving step.
- 11-24 07:05-07:20 Console (the user: "Yes, hbmenu is open"; title checked, Y pressed, the package NRO netloaded):
  run save1, log mp2_056, the shipped settings with a save-test input script. Resume Game loaded Part III, Chapter 5;
  Quick Save, Load Last Game and a second Quick Save all worked (new save folders, "Game saved", Mona back at the save
  spot); 60 fps in play around each, one long frame per save or load (66-235 ms); clean exit. The card back to the
  inert input script, settings and NRO read back (9b572151, AC48BB7E); the run's saves stay. Notes: "Saving and
  loading", "On the console". TESTING.md step 6 says console-checked.
- 11-24 07:30-09:05 "Keep working throughout the day ... until game is completed": the PC made a release. PC sweep
  of levels 2-29 (45 s each after Resume Game): 60 fps in play everywhere. PC window modes (borderless default,
  sizable window, F11 / Alt+Enter, DPI aware) and the PC package `out\package\pc` (exe, settings, readme, VC++
  runtime app-local, PDB): commit 446f5c1. Mouse look: the yaw found by guest memory scans and a hardware write
  watch (PC test aids SCAN, WATCH), hooked in `sub_00045B36` (src/game/mouse.cpp; the base's gyro aiming on the
  Switch turns through it too); raw mouse, buttons, wheel and keys on the PC: commit 8ad659e; PC package rebuilt
  (exe BCE43992). Handoff patch 69 files (host-pc/stubs.cpp added), applies. Notes: "The PC as a release".
- 11-24 16:26-17:15 Console (the user: "Yes, hbmenu is open"; title checked, Y pressed): run next1, log mp2_057, the
  next build 0905 (76285327) netloaded under its own name with the save script and the shipped settings. Resume Game,
  two Quick Saves and Load Last Game as with the shipped build, 60.0 fps in play, one long frame per save or load;
  the time zone answered as the console's own (bias 420 minutes); the gyro turned on and off through the port's panel
  (sensors on at 200 Hz, the view still with the console resting). The panel's change rewrote mp2-nx.toml without
  comments (same values): the shipped one put back. Card: settings, inert script and AC48BB7E read back; the run's
  saves, the test NRO and gyro.toml (off) stay. Found and fixed: the gyro's ZL mode (Max Payne 2's shootdodge) now
  ALWAYS / WHILE FIRING (ZR); the TOUCH CONTROLS page (stubs here) now CONTROLLER; opening the panel with both sticks
  crouched Max; on the PC F10 / Back + R3 never closed the panel. `input::PanelChords` (core/panel_chords.h, both
  hosts; HANDOFF row 36), scripted presses reach the Switch panel (test aid), run_ryujinx -Capture, window_shot.ps1.
  Checked in Ryujinx (NRO 913F5CDD) and on the PC (exe 2080EC42). PC package rebuilt; handoff patch 70 files,
  applies. The new Switch NRO waits for a console run (out/console/next/mp2-nx_1714.nro); package keeps AC48BB7E.
- 11-24 17:21-17:50 Console again ("Yes, hbmenu is open"; title checked, Y): run next2, log mp2_058, the 17:14 build
  (913F5CDD). Both sticks open and close the panel with no crouch; GYROSCOPE ALWAYS / WHILE FIRING (ZR) / OFF with the
  sensors on and off; the gyro always on with the console still: no drift in 30 s, 60 fps; an L3 tap crouches. Back +
  R3 worked but Back alone (sys-botbase presses it first) opened the game's menu behind the panel: `PanelChords` now
  holds Back back like the sticks (Ryujinx NRO D2C9C0DA, PC exe 74EC2B65 checked). Card: shipped settings, inert
  script, gyro.toml (off), AC48BB7E read back; the console then went to sleep, so the new package NRO is not on it.
  Package: the console-checked 913F5CDD (sources 170ae0a), fallbacks unchanged (cfe7f01, now labelled so),
  TESTING.md (panel, gyro step 7). The Back fix waits in out/console/next/mp2-nx_1741.nro.
- 11-24 17:44-17:55 Console ("Yes, hbmenu is open"; title checked, Y): run next3, log mp2_059, the 17:41 build
  (D2C9C0DA). Back + R3 pressed Back first opens and closes the panel with play behind it; both sticks no crouch; a
  Back tap still opens the game's menu; the L3 tap crouches; 60 fps; clean exit. The card's `mp2-nx.nro` is now
  D2C9C0DA (read back; AC48BB7E kept locally), settings, inert script and gyro.toml read back. Package rebuilt with
  D2C9C0DA (sources 919d41a), the card's file; TESTING.md updated.
- 11-24 17:58-18:25 "Keep working until the game is completed": installer release 0.1.10 (D2C9C0DA); the GL fallback
  rebuilt from the current sources (FC6A4942) and checked in Ryujinx with the panel script; package rebuilt with it
  (`make_package.ps1 -GlRev / -FallbackRev`); TESTING.md's fallback line; the report's standing sections rewritten to
  the port's current state (they still described the first night).
- 11-24 18:30 Dead Man Walking on the Switch build in Ryujinx (01_Survival via test_script after Resume Game): loads,
  plays at 60.
- 11-24 18:40-18:55 The ending (GM_SendEndOfGameMessages: the final graphic novel to its last panels) and the credits
  film from the main menu on the Switch build in Ryujinx: both play, no crash. No level-complete script command
  exists (exits are triggers), so a played-through exit stays for the user's play.
- 11-24 18:28-18:45 PC played by hand (the user): pc-test (the PC package with the test saves, the disc folder as its
  argument); Part III Chapter 8 in the manor at 60 fps for 4.5 minutes, Alt+Tab fine, no crash; the mouse aiming
  "felt good".
- 11-24 18:45-19:05 The console play test: the user did not get to play (no new log, the card unchanged); it stays
  open. The CPU fallback rebuilt from the current sources (A38AF499), checked in Ryujinx (films to the title), and the
  package rebuilt: all three NROs one revision (sources 48ddb39 = 919d41a's code). TESTING.md's soft line. Final
  report entry.
- 11-24 19:10-19:30 render_wide: the user asked whether to turn it on. Supported on the Switch renderers only (the
  PC's D3D layer has no horizontal scale). Ryujinx wide1 (deko3d, wide on): correct framing and HUD, sharper, 60 fps.
  Never measured on the console; recommended on, with a console check of the GPU-heavy levels.
- 11-24 19:30-19:45 "Turn it on": render_wide = 1 in the shipped Switch settings (and play / 30 fps; the A/B file now
  the original picture); TESTING.md; package (settings c3d3c6e6, git c8aa0fc) and installer 0.1.11; the card not
  reached (asleep). Level 19 A/B run files prepared for the next console session.
- 11-24 19:13-19:40 Console ("Yes, hbmenu is open"): render_wide in level 19's GPU stretch, one run (mp2_060). Off:
  60.0 fps, GPU up to 11.7-12.6 ms a 5 s window; on (panel, then Load Last Game): 59.6 / 59.0 fps for about 10 s, GPU
  up to 14.6 ms; elsewhere 60 either way (GPU +25%). Kept on (the user's choice). The card has the new shipped
  settings (wide on), read back. The game replaced its own auto save during the run (same chapter).
- 11-24 19:45 The user first asked to raise the handheld GPU clock, then: "Actually, keep it as is". Nothing written
  to the Horizon-OC config; wide rendering stays on with level 19's brief 59.0-59.6 fps stretch.
- 11-24 20:17-20:30 The console played by hand (the user, mp2_061): level 19 for about 8 minutes with wide rendering
  on, two reloads after dying, 60 fps except the GPU-heavy part's brief 58.4-58.6 windows; no crash, clean exit; the
  card unchanged. A chapter's exit and the gyro aim still untried.
- 11-24 20:27 (console time) The user played a chapter through on the console: "looks great". The log (mp2_062) stays
  open while the game is suspended; read it after the user closes the game.
- 11-24 20:35 The user: "Gyroscope works" (gyro.toml back to off afterwards). Every hand test done: PC mouse, a long
  console session, a chapter played through, the gyro. TESTING.md step 7 updated.
- 11-24 20:33-20:45 The chapter session's log (mp2_062): level 19 played to its exit, Part III Chapter 6
  (20_Third_Dream) loaded clean; 60 fps in level 19 apart from reloads; the gyro on and off and left on (gyro.toml
  gyro = 1); the panel's RENDER RESOLUTION stepped to 125% by accident on the way to the gyro (Third Dream then 43-50
  fps, GPU 20-23 ms): the card's settings put back to the shipped ones (100%), read back.
- 11-24 20:50-21:10 "Yes, fix it": the panel opens with the focus on its page's tab (SetItemDefaultFocus), so a stray
  left / right no longer changes RENDER RESOLUTION. Checked in Ryujinx (3132159B) and on the PC (82A1AEBA); waits for
  a console check before the package and the card.
- 11-24 21:13-21:30 Console (the user opened hbmenu): the panel's tab focus checked (mp2_063): a stray RIGHT after
  opening changes nothing, Third Dream at 60 at 100%. The card's game file 3132159B; package, installer 0.1.12, PC
  package 82A1AEBA and pc-test updated.
- 11-24 21:35-21:55 "Continue working": the GL (5990607F) and soft (D1E63760) fallbacks rebuilt with the panel's tab
  focus, checked in Ryujinx; the package one revision again (0a4afcc). HANDOFF row 37: the tab focus described for
  the base's own Cold Fear menu.cpp.
- 11-24 22:00-22:20 "Continue on and publish": the source prepared for publishing (no game material tracked or ever
  tracked; no personal details in the tree; LICENSE (MIT) and a current README; a single-commit `publish` branch,
  since the history holds an old user path). The NRO, exe and installer release are not published (recompiled game
  code). Creating the GitHub repository waits for the user's sign-in in the browser pane.
- 11-24 22:25 Published: https://github.com/maafeu/MP2-nx (public; `main` = 9db517d, the source as one clean commit,
  the MIT licence, the README). The game files (NRO, exe, installer release) not published.
- 11-24 22:35 Release v1.0.0 on GitHub: source only (0 assets), the tag on 38d1b69, release notes; Latest.
- 11-24 23:00-23:20 The web installer published: bring your own NRO (the page asks for the player's build when the
  release has none), tested end to end in Chromium (429 files byte for byte), a separate GPL-3.0 repository with
  GitHub Pages: https://maafeu.github.io/MP2-nx-installer/. The README links it. The cover art stays off the public
  page (the Switch build's own icon already uses it, privately).
- 11-24 23:35-23:55 The installer merged into this repository as `installer/`, published by a GitHub Actions Pages
  workflow: https://maafeu.github.io/MP2-nx/ (live, checked). The old repository redirects there (its root and any
  old path); both v1.0.0 release notes link the new address.
- 11-24 ~00:00 The old repository maafeu/MP2-nx-installer deleted (the user's word); the installer lives only in
  `installer/` here, at https://maafeu.github.io/MP2-nx/.
- 11-26 18:05-18:20 Renamed MaxPayne2-nx: the GitHub repository, the installer page
  (https://maafeu.github.io/MaxPayne2-nx/), every MP2-nx in the files and the v1.0.0 release; the file names
  (`mp2-nx.nro`, `switch/mp2-nx/`) kept.
