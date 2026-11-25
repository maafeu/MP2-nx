Max Payne 2: The Fall of Max Payne (Xbox) on the PC - MP2-nx
==============================================================

The Xbox game (NTSC-U, TT-012), recompiled to run natively on Windows. Private: the exe holds the recompiled game,
so keep it to your own PC and do not share it.

What you need
-------------
- Windows 10 (1703 or later) or 11, 64-bit.
- A graphics card with a Vulkan driver (any current NVIDIA, AMD or Intel driver has one).
- The game's files from your own Xbox disc: default.xbe and its folders.
- The Visual C++ runtime is in this folder (msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll); nothing to install.

Setting up
----------
1. Copy the disc's files into the "game" folder beside mp2-nx_pc.exe, so that game\default.xbe exists.
   (Or start the exe with the folder as its argument: mp2-nx_pc.exe "D:\MP2 disc", or set XBE_GAME_DIR.)
2. Start mp2-nx_pc.exe.

The game runs at 60 frames a second at 1280x720 and fills the screen without a border. F11 or Alt+Enter switches
between that and a window you can size. The picture keeps the game's 16:9 shape, with black bars on screens of
another shape.

Controls
--------
An Xbox-style controller (XInput: Xbox 360, Xbox One, Xbox Series pads and most others) works as the Xbox pad.

Mouse and keyboard:
  mouse .............. aim (up looks up; mouse_sensitivity in the settings, 1.0 is 0.05 degrees a count)
  left button ........ shoot (right trigger)
  right button ....... shootdodge (left trigger)
  middle button ...... bullet time (B)
  wheel .............. weapons (D-pad up / down)
  W A S D ............ move (left stick)
  Space .............. jump (A)          Enter ........ A (menus)
  R .................. reload (X)        E ............ use / zoom (Y)
  C .................. crouch (left stick click)
  Q .................. X                 Backspace .... B
  1 .................. painkillers (white)
  2 .................. secondary attack (black)
  Ctrl / Shift ....... left / right trigger
  I J K L ............ right stick (look without the mouse)
  arrow keys ......... D-pad
  F / G .............. left / right stick click
  Tab ................ Start (pause)     Esc .......... Back
  F10 ................ the port's options panel (also Back + right stick click on a pad)
  F11, Alt+Enter ..... borderless screen / window

The game takes the mouse while it is in front: at once on the borderless screen, after a click in a window. The
pointer is hidden and kept inside the game until you switch away (Alt+Tab). The mouse aims at the same speed in
bullet time. The right stick follows the game's own "Invert Aiming" (Options > Game; on by default: pushed up, Max
looks down); the mouse always looks up when moved up. With the mouse you may want the game's "Enable Auto Aiming" and
"Enable Auto Lock" (Options > Game, both on by default) off, so the aim goes where you point it.

Settings
--------
mp2-nx.toml beside the exe; each line says what it does. The shipped values keep the original picture. "width" and
"height" (both set, e.g. 1920 and 1080) draw the game at that size instead of 1280x720: sharper, but not the
original picture.

Files the game makes beside the exe
-----------------------------------
- hdd\     the Xbox hard disk: your saves (hdd\E\UDATA) and the game's settings (hdd\E\TDATA)
- logs\    a log of each run (mp2_NNN.log); send the newest one with any problem
- shadercache\   compiled shaders, made again when deleted

Saves are named with the time in UTC, as the original game names them on a real Xbox.

analysis\ holds BUILD.txt (which build this is) and the debug symbols for reading a crash in the log.
