# Skies of Arcadia Legends — PC Recompilation (work in progress)

A native PC port of **Skies of Arcadia Legends** (GameCube, PAL `GEAP8P`) made by
**static recompilation**: the game's PowerPC code is translated to C and compiled for x86-64,
running on a small runtime that stands in for the GameCube hardware. No emulator.

**This repository contains no game code or data.** You need your own copy of the game;
the recompiled code is generated locally from your own `main.dol`.

## Status

| Area | State |
|---|---|
| Boot, menus, gameplay | Running (Windows and Linux) |
| Graphics | OpenGL 3.3 renderer ported from Dolphin's pipeline (TEV, indirect texturing, fog, z-textures, EFB copies kept on the GPU). Remaining glitches are being compared frame-by-frame against Dolphin |
| Input | Keyboard + XInput controllers |
| Audio | GameCube DSP (AX) port; **optional Dreamcast sound** (see below) |
| Saves | Memory card emulation (raw card image, Dolphin GCI import via `tools/mkcard.py`) |
| PC features | Internal resolution up to 6x, borderless fullscreen, V-Sync, 60 fps by frame interpolation (the game steps at 30), 16:9 widescreen (experimental), in-game Options menu on the title screen (F1 anywhere) |
| Next | Rendering accuracy, battle performance, widescreen culling, then decompilation for modding |

### Frame interpolation
The game's logic is fixed-step (one step per two fields). For 60 fps the renderer records each frame's GX
command stream and replays it with vertex positions and transform matrices blended halfway towards the
previous frame, showing that in-between image one field before the real frame (`fps60` in `settings.ini`).

### Dreamcast sound
If you also own the **Dreamcast** version, the PC build can play all music and sequenced sound
effects through the Dreamcast's own sound driver (`BONITO.DRV`) running on an emulated AICA
(Highly Theoretical core), using the Dreamcast sound banks from your discs. Legends-only content
automatically stays on the GameCube audio. Point `dc_path.txt` at your Dreamcast disc folders.

## How it works
- `tools/ppc2c.py` — translates every function of the DOL (boundaries from `config/GEAP8P/symbols.txt`,
  a [decomp-toolkit](https://github.com/encounter/decomp-toolkit) config) into C. Each function is emitted
  as `r_XXXXXXXX` with a weak alias `f_XXXXXXXX`, so any function can be overridden in C while still
  calling the original — the hook point for fixes, enhancements and incremental decompilation.
- `rt/` — runtime: memory, interrupts and scheduling, disc/EXI/SI/memory card, GX FIFO and renderer,
  the AX DSP ucode (port of Dolphin's HLE), and `dcsnd.c` (Dreamcast sound bridge).
- `rt/ht/` — Highly Theoretical sound emulation core (Neill Corlett, GPL — see `rt/ht/COPYING.txt`).

## Building (outline)
1. Extract your PAL disc; point the scripts at `sys/main.dol`.
2. `python3 tools/ppc2c.py <main.dol> config/GEAP8P/symbols.txt out` then `python3 tools/mknames.py`.
3. `./build.sh` (Linux) or `LLVM_MINGW=/path/to/llvm-mingw ./build_win.sh` (cross-compile for Windows).
4. Put `game_path.txt` (path of the extracted disc) and `dsp_coef.bin` next to the executable; `settings.ini`
   is created on first run.

### Testing without a GPU
Headless Linux runs use Mesa's software rasteriser (`EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1`).
`SOA_INPUT="frame:button:len,..."` scripts controller input, `SOA_DUMP=N` writes every Nth frame to `frames/`,
and `tools/dolphin_movie.sh` + `tools/mkdtm.py` produce matching Dolphin reference frames from the same
save and input script for pixel comparison (`tools/framecmp.py`).

Personal, non-commercial preservation and modding project. Skies of Arcadia is © SEGA.
