# MPayne2-nx — console test package

Max Payne 2: The Fall of Max Payne (Xbox, NTSC-U, `TT-012`) as a Switch homebrew. **Private:** the NRO holds the game's
recompiled code; do not publish this package.

## What this build is

- Renderer: **deko3d** (`PORT_RENDERER=dk`, `host-switch/dk_backend.cpp`), the Switch GPU's own API (libnx's): the
  OpenGL renderer's draws without Mesa's driver on the CPU (that was 3 ms of a heavy frame). It draws what the OpenGL
  renderer draws (the console's frames side by side, 2026-11-22). Its shaders are compiled on the console by uam (the
  deko3d shader compiler, built into the NRO) the first time each one is seen and kept in `shadercache/` (files
  `*_dk3.bin`): the first run has short hitches where new materials appear, later runs none.
- `fallback/MPayne2-nx-gl.nro`: the same build with the **OpenGL** renderer (`PORT_RENDERER=gl`, devkitPro's
  switch-mesa), the one shipped before deko3d; checked in Ryujinx only (2026-11-24: menus, Resume Game, play, the
  options panel opening on its tabs, its chords and the gyro row). Copy it over `MPayne2-nx.nro` if the deko3d one does
  not start.
- Output: 1280x720, the game told the TV is widescreen (it draws its own anamorphic 16:9). Docked, the system scales
  the 720p picture to the TV. The game's frame is drawn at the full 1280x720 (`render_wide = 1` in `MPayne2-nx.toml`,
  your choice of 2026-11-24: sharper than the original 960x720 stretched to 1280, which `render_wide = 0` gives).
  Measured on the console (2026-11-24, handheld, the GPU at 460.8 MHz from the Horizon-OC profile): 60 fps
  everywhere except level 19's GPU-heavy stretch (`19_Third_Address_Unknown`, about 10 s), where it gives 59.0-59.6
  fps (a few repeated frames; 60.0 with `render_wide = 0`); the GPU works about a quarter more with it on.
- **Frame rate (the shipped settings):** `fps_cap = 60` with `fps_adaptive = 1`: 60 fps where the console holds it,
  free just under 60 where it nearly does, locked to 30 where it falls well short (frames over `fps_adaptive_ms` = 20
  ms on average, below 50 fps), back to 60 once they take 1.5 ms less; vsync paces both (swap interval 1 or 2).
  `fps_adaptive_ms = 16` locks to 30 as soon as 60 slips (the behaviour before 2026-11-23). The game times itself by
  the clock, so it plays at the same speed at 30 and 60. `cpu_clock = 1581` with `mem_clock = 1600` asks for the CPU
  at 1581 MHz and the memory at its docked 1600 MHz at least (a clock manager's higher clocks are left as they are; the
  old clocks come back at exit): the lowest pair measured that holds 60 in every level tested (below). **`MPayne2-nx_30.toml`**: 30 fps locked at the system's 1020 MHz
  (cooler, longer on battery).
- **Measured on the console (2026-11-22, the CPU at 1020 MHz):** with deko3d chapter 1 holds **60 fps locked** (12-13
  ms of work a frame; the OpenGL build gave 57 fps); level 27, the heaviest, about 46 fps free (21 ms a frame; OpenGL
  40). Films, menus and the graphic novel run at 60.
- **The clock survey (2026-11-23, handheld, the deko3d build with its render thread, four of the heaviest levels):**
  at 1020 MHz the heaviest fights run 43-58 fps; 1428 MHz, 1581 MHz, and 1428 with the memory at 1600 each left them
  at 57-58.7 fps; **1581 MHz with the memory at 1600 held 60 in every window of play**. What is left under 60 is a
  single long frame (0.1-0.2 s) as a level starts, at any clock.
- **The clock on a console with a clock manager:** Horizon-OC / sys-clk (a governor and a thermal limit) can hold the
  clocks at the system's whatever the game asks: in the survey the CPU stayed at 1020 MHz for the first 1-3 minutes
  of each run and the memory went back to 1331 for the first 5 s (the log's `threads:` lines say the CPU clock measured
  every 10 s; `clock: ... was reset` lines say when the port set a clock again). To have it hold the pair itself, give
  hbmenu's title a profile (it applies to that title's own game too). Horizon-OC (the HATS 2026-11-09 pack's) reads
  profiles only as sections of `sdmc:/config/horizon-oc/config.ini`, sys-clk style, in MHz: `[010091D01597C000]`,
  `handheld_cpu=1581`, `docked_cpu=1581`, `handheld_mem=1600` (docked, the memory is at 1600 already), and
  `handheld_gpu=460` for the one GPU-bound stretch (19_Third_Address_Unknown, about 48-58 s into the level: 55.6-57.3
  fps at the handheld 307.2 MHz, 60.0 at 460.8; the port's own `gpu_clock` cannot hold it there, the system puts 307.2
  back every second); its overlay's Edit App Profile sets the same. It reads no `profiles/` folder. Without the folder `sdmc:/config/horizon-oc` it
  runs on its defaults and saves nothing; with the folder it writes no `config.ini` of its own: make it from its
  `config.ini.template` and add the section at the end. **On the test console (2026-11-23) with that profile:** the
  CPU at 1581 MHz and the memory at 1600 from the first second of the run, every 10 s reading 1570-1583 MHz for 8
  minutes, with the port asking no CPU clock (`cpu_clock = 0`); every play window of the four heavy levels at 60 fps.
- The kit's options panel: **Minus + R3**, or **both sticks pressed in together**, opens and closes it (the game takes
  no input while it is open); the FPS counter is one of its settings. Its **CONTROLLER** page has the gyro aiming
  (MOTION CONTROLS: GYROSCOPE off by default; ALWAYS, or WHILE FIRING (ZR): turning the console, or the right Joy-Con
  or Pro Controller, turns the view; sensitivity, vertical speed, drift correction and inversion below it) and the
  sticks' dead zones and sensitivities. Changing a setting there rewrites `MPayne2-nx.toml` without its comments (the same
  values; `MPayne2-nx_shipped.toml` in this package has them). Minus, L3 or R3 pressed alone reaches the game 80 ms late,
  so the chords never reach it (checked on the console, 2026-11-24: no crouch, no game menu behind the panel) while a
  quick tap of any of them still does.
- The Bink films (the intro logos and the story films) are shown; their colour conversion is native (about 8.7 ms a
  film frame on the console at 1020 MHz), well inside a film's own rate.
- `fallback/MPayne2-nx-soft.nro` (when present): the same build with the CPU (software) renderer, for if no GPU one
  starts: 2–5 fps, 3D picture wrong, films and menus right (checked in Ryujinx, 2026-11-24: the intro films
  to the title).

## SD card

```text
sdmc:/switch/MPayne2-nx/
  MPayne2-nx.nro            (sd/switch/MPayne2-nx/ in this package)
  MPayne2-nx.toml           (the shipped settings; the port writes it if missing)
  game/                 the disc's files: default.xbe, DATA.RAS, assets/, movies/, dsstdfx.bin, assets.txt
                        (or the .iso; the full redump image works)
  logs/                 written by the port: mp2_NNN.log, stderr.log
  hdd/                  written by the port: E (saves, T:/U:), Z (the level cache, about 30 MB a level)
  shadercache/          written by the port: compiled shaders (SPIR-V), so the second run compiles less
```

Start it from hbmenu **while holding R on an installed game** (title mode, all the memory).

## What to test

1. Boot: the intro films (A skips each), then the title and the main menu (START at the title).
2. Main menu: DOWN to New Game, A; A on the game mode (Detective). The graphic novel plays (R skips it).
3. Chapter 1 loads (the level files are copied to `hdd/Z` first: the first load is slower). The first time each new
   material is seen, its shader compiles: a short hitch; fewer on the second run.
4. Look at the picture (lighting, Max, the HUD) and the frame rate; sound: menu music and the film sound.
5. Leave it running a few minutes in chapter 1; then quit with HOME.
6. Saving: START in play opens Game Paused; Quick Save writes the save ("Game saved") and closes the menu. START,
   Load Last Game, A (and A again to quit the current game) loads it back. The main menu's Load lists Auto Save,
   Quick Save and Previous Quick Save. Checked on the PC, in Ryujinx and on the console (2026-11-24, Part III
   Chapter 5: Resume Game, two Quick Saves, Load Last Game; 60 fps in play, a single long frame of 0.1-0.25 s at
   each save or load). The times in the save names are UTC, not your local time: the game takes them from the
   console's UTC clock (`GetSystemTime`), on a real Xbox too.
7. Gyro aiming (optional): open the panel, CONTROLLER, GYROSCOPE to ALWAYS, close it and turn the console. On the
   console (2026-11-24) the sensor started and stopped with the setting, and with the console lying still the view did
   not move for 30 s, at 60 fps; aiming by turning it was tried by hand the same evening: it works. With it on, the
   game's auto aiming (Options > Game) may pull against it.

To have the presses done for you, put `input_script.txt` (in this package) beside the NRO: it skips the films, picks
New Game and goes into chapter 1 by itself. Remove it to play by hand.

With a save on the card the main menu has Resume Game between How to Play and New Game: the script's one DOWN then
resumes the save. `input_script_newgame2.txt` (two DOWNs) goes to New Game in that case.

**One diagnostic run** (once): put `dk_flags.txt` (it says `selftest`) beside the NRO. At start the renderer uploads
a small texture, clears test targets (whole and inside a scissor), draws the texture into them, reads them back and
logs what came back (`dk: selftest ...` lines: full rows, red on top, green below). The file's text `none` turns it
off again without removing the file; `faceswap` turns the triangles' front face round (for checking). The OpenGL
fallback reads `gl_flags.txt` the same way (`selftest`; `nopersist` keeps its old orphaned upload buffers).

**Captures:** a file named `capture` beside the NRO saves the presented frame to `shots/` every 15 s; its text sets
the interval in seconds, `0` turns it off (each capture stalls the game while it is written: never leave it on for
a timed run).

**Any level** (a test aid, not a setting): `test_level = NN` added to `MPayne2-nx.toml` (2 to 30, `game/assets.txt`'s
order: 2 `04_Warehouse` ... 30 `21_The_Manor_B_split2`) changes to level NN about 3 seconds after New Game's (or
the loaded save's) level starts, through the game's own `GM_InitSingleLevel` script command. Remove the line to play
normally; the port never writes it. The log names every level it loads (`game: level init ...`).

**Native code A/B** (test aids as well, never written by the port): the films' colour conversion runs as native code
(`src/game/native_bink.cpp`, bit-exact with the game's own). `test_native = false` runs the game's recompiled
original instead, to time a film both ways in one session; `test_shadow = true` runs both on every call, compares
everything they write and logs `native: ... mismatch` if they ever differ (slower: for checking only).

## What to send back

- `logs/mp2_NNN.log` (the newest) and `logs/stderr.log`.
- A photo or capture of the screen in chapter 1 if anything looks wrong.
- A crash names an address (`crash: ... pc ...`): `analysis/MPayne2-nx-<hash>.elf` is this NRO's own ELF, and
  `ferramentas/endereco_crash.sh` in the source snapshot turns the address into a function.

## Performance data ("console profile")

What the port records, always on (`log_summary.py` in this package makes one table of a log:
`python log_summary.py mp2_NNN.log`):

- every 300 frames: `d3d: Swap N: <fps>, longest <ms>, <n> over 40 ms; game <ms> present <ms> gpu wait <ms> pace <ms>`
  (game = the recompiled game's own time per frame with the renderer's draws, present = the renderer's present,
  pace = the wait for the cap). 60 fps locked shows as `60.0 fps`, longest about 17-20 ms.
- every 300 frames: `dk: renderer parts a frame: assembly, textures, commands, the rest` (the renderer's time split),
  with the barriers a frame, the GPU's time a frame (its busy time, without the wait for a swapchain image), the waits
  in the present, the shaders compiled or loaded from the cache, and the textures that changed without a lock (the
  OpenGL fallback writes `gl:` lines with its GL calls in place of the commands).
- every 10 s: `threads: ...; CPU at N MHz` (each busy guest thread's share of a core, and the CPU clock measured).
- `app: frames take N ms on average (over L): 30 fps until they take under M` / `back to 60 fps`: the adaptive cap's
  switches (L is `fps_adaptive_ms`, M 1.5 ms less).
- **A function-level profile** (a test aid): `profile = 1` (and `profile_after = 100`, the seconds to wait) in
  `MPayne2-nx.toml` samples the game's busy threads at 1 kHz into `logs/prof_NNN.bin` beside the log; `tools/prof_report.py
  prof_NNN.bin analysis/MPayne2-nx-<hash>.elf` (in the source snapshot) makes the tables. The port never writes the key.
- every 300 frames: `dk: present N, <draws> draws, <textures> textures, <targets> targets, <programs> programs; a frame:
  <KB> uploaded, <n> state calls, <n> skipped as unchanged, renderer <ms> on the title's thread`. The renderer draws on
  the game's own thread: `renderer` is its draws, clears, copies and read-backs (all inside `game` above) plus its
  present without the swap (the `present` figure), so `game` + `present` − `renderer` is about the recompiled game itself.
- `app: alive` every 5 seconds (a log that ends with these and no `crash:` is a hang, not a crash).

## Files in this package

| File | What |
| --- | --- |
| `sd/switch/MPayne2-nx/MPayne2-nx.nro` | the port (deko3d renderer) |
| `fallback/MPayne2-nx-gl.nro` | the same build with the OpenGL renderer (the one shipped before deko3d) |
| `dk_flags.txt` | the deko3d renderer's self-test, for one diagnostic run |
| `sd/switch/MPayne2-nx/MPayne2-nx.toml`, `MPayne2-nx_shipped.toml` | the shipped settings (the defaults, written out) |
| `MPayne2-nx_ab.toml` | A/B: the shipped settings with `render_wide = 0` (the original picture) |
| `MPayne2-nx_30.toml` | 30 fps locked at the system's clock (`fps_cap = 30`, `cpu_clock = 0`) |
| `log_summary.py` | one table from a log (fps, frame times, the renderer's parts, notes) |
| `MPayne2-nx_play.toml` | for playing: the same settings; the diagnostics in this kit are files (`input_script.txt`, `gl_flags.txt`, `capture`), none of them in the SD folder by default |
| `input_script.txt` | scripted presses to chapter 1 |
| `gl_flags.txt` | the OpenGL fallback's self-test, for one diagnostic run with it |
| `fallback/MPayne2-nx-soft.nro` | (when present) the CPU-renderer build, if no GPU one starts |
| `analysis/` | the ELFs, the source snapshot and `BUILD.txt` (hashes) |

A/B (the same NRO, one session): `MPayne2-nx_ab.toml` is the shipped settings with `render_wide = 0`; run with
`MPayne2-nx.toml` (A), quit, copy `MPayne2-nx_ab.toml` over it (B), run again, and compare the `d3d: Swap` lines. The
options panel's WIDE RENDERING row flips the same setting during a run.
