# mp2-nx web installer

**The page: https://maafeu.github.io/MP2-nx/**

This folder is published as that page by `.github/workflows/installer-pages.yml` whenever it changes. It keeps its own licence (GPL-3.0, `LICENSE` here); the rest of the repository is MIT.

A copy of renut_nx_ultimate_installer (GPL-3.0: `README_TEMPLATE.md`, `LICENSE`, `NOTICES.md`) set up for
[MP2-nx](https://github.com/maafeu/MP2-nx), the Nintendo Switch port of the original Xbox Max Payne 2.

**This page contains no game code and no game files.** It packs the player's own files into the SD card layout the
port expects:

1. **The game:** the player's NTSC-U disc (`TT-012`) as an `.iso` or an extracted folder. The page checks
   `default.xbe` against the supported version's SHA-256 (`release/manifest.json`) and that `DATA.RAS` is there.
2. **The port:** the player's own build of `mp2-nx.nro`, made from the MP2-nx source. The NRO holds the game's
   recompiled code, so it is never part of this page; the page only checks that the chosen file is a Switch NRO.
3. **The zip:** `switch/mp2-nx/mp2-nx.nro`, `switch/mp2-nx/mp2-nx.toml` (the port's shipped settings, from
   `release/`) and the game's files under `switch/mp2-nx/game/`, streamed to the browser's downloads while it is
   written. Nothing is uploaded: everything runs in the browser.

## What changed from the template

| File | Change |
| --- | --- |
| `game.json` | Max Payne 2; `exe: default.xbe`, `game_dir: game` (the port reads `switch/mp2-nx/game/`), no shaders, no LZ4, no DLC; needs `default.xbe` and `DATA.RAS`; own texts |
| `release/manifest.json` | one build: the NTSC-U disc's `default.xbe` (`exe_sha256`) and `mp2-nx.toml`; no `nro_sha256`, which tells the page to ask for the player's NRO |
| `index.html`, `app.js` | a step for the player's own NRO (shown when the release has none; checked for the NRO header), steps numbered as shown, the Switch launch the port was tested with (hbmenu with R held on an installed game) |
| `worker.js` | the player's NRO in place of a release one |
| `lib/install.js` | `findBuild` takes the executable's name; the game's files go to `game_dir` |
| `test/browser.html` | `?nro=<url>` gives the worker an NRO as the player's |
| `icon.png` | an original drawing (`tools/make_icon.py`: heavy rain over an invented night city, a red haze and a red streak across the sky; no game art, no real building, no lettering) |

The disc reader (`lib/xiso.js`) already knew the original Xbox's layout. The shader and LZ4 code stays, unused.

## Testing it locally

`python serve.py 8193 . --test-files <your extracted game> --test-packages <folder with your mp2-nx.nro> --test-out
<zip>` and `/test/browser.html?compile=0&nro=/_pkg/mp2-nx.nro`; `test/check_zip.py` checks the zip. The zip holds
game data: never commit or share it.

## Licence and credits

GPL-3.0 (`LICENSE`), as the installer it is made from; the components and their authors are in `NOTICES.md`, and the template's own guide for porters is `README_TEMPLATE.md`. Max Payne is a trademark of its owners; this is an unofficial fan project, not affiliated with or endorsed by the game's publishers, Microsoft or Nintendo.
