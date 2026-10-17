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
| Input | Keyboard + XInput controllers, fully remappable (Xbox or GameCube button layout) |
| Audio | GameCube DSP (AX) port; **optional Dreamcast sound** (see below) |
| Saves | Memory card emulation (raw card image, Dolphin GCI import via `tools/mkcard.py`) |
| PC features | Internal resolution up to 6x, MSAA, anisotropic filtering, borderless fullscreen, V-Sync, 60 fps by frame interpolation (the game steps at 30), 16:9 widescreen with optional edge-anchored minimap, Dolphin-format HD texture packs, save states (F5/F9, 10 slots), fast-forward, encounter rate and text speed options, separate music/effects volume, screenshots (F12), pause when unfocused; all in a five-page Options screen (title menu, or F1 anywhere). Everything defaults to the original behaviour |
| Next | Rendering accuracy, battle performance, then decompilation for modding |

### Frame interpolation
The game's logic is fixed-step (one step per two fields). For 60 fps the renderer records each frame's GX
command stream and replays it with vertex positions and transform matrices blended halfway towards the
previous frame, showing that in-between image one field before the real frame (`fps60` in `settings.ini`).

### Dreamcast sound
If you also own the **Dreamcast** version, the PC build can play all music and sequenced sound
effects through the Dreamcast's own sound driver (`BONITO.DRV`) running on an emulated AICA
(Highly Theoretical core), using the Dreamcast sound banks from your discs. Legends-only content
automatically stays on the GameCube audio. Point `dc_path.txt` at your Dreamcast disc folders.

### Save states
The host C stack mirrors the guest call stack, so a state is taken at one fixed point - the main loop's
end-of-frame wait - and stores the guest return-address chain there. `tools/ppc2c.py` emits every
unconditional call site as a resume point (`rs_take` at function entry): a state saved under a different
chain is loaded by unwinding to the bottom of the host stack, restoring memory and hardware state, and
calling back down through the saved chain into the save point. Each runtime module serialises its own state
(`*_state(SS*)`); the Dreamcast sound chips are included. Memory-card contents are not part of a state.

### HD textures
Textures are named exactly as Dolphin names them (`tex1_WxH[_m]_<xxh64>[_<tlut xxh64>]_<fmt>`, with `$`
wildcards), so Dolphin texture packs for GEAP8P/GEAE8P/GEAJ8P load unchanged from `Load/Textures/<id>`.

## How it works
- `tools/ppc2c.py` — translates every function of the DOL (boundaries from `config/GEAP8P/symbols.txt`,
  a [decomp-toolkit](https://github.com/encounter/decomp-toolkit) config) into C. Each function is emitted
  as `r_XXXXXXXX` with a weak alias `f_XXXXXXXX`, so any function can be overridden in C while still
  calling the original — the hook point for fixes, enhancements and incremental decompilation.
- `rt/` — runtime: memory, interrupts and scheduling, disc/EXI/SI/memory card, GX FIFO and renderer,
  the AX DSP ucode (port of Dolphin's HLE), and `dcsnd.c` (Dreamcast sound bridge).
- `rt/ht/` — Highly Theoretical sound emulation core (Neill Corlett, GPL — see `rt/ht/COPYING.txt`).
- `rt/ext/` — stb_image / stb_image_write (public domain) and xxHash (BSD-2-Clause, Yann Collet).

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
