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
| Graphics | OpenGL 3.3 renderer — works, with known glitches being fixed |
| Input | Keyboard + XInput controllers |
| Audio | GameCube DSP (AX) port; **optional Dreamcast sound** (see below) |
| Saves | Memory card emulation (raw card image, Dolphin GCI import via `tools/mkcard.py`) |
| Next | Graphics accuracy, long-play stability, widescreen/60 fps, then decompilation for modding |

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
3. `./build.sh` (Linux) or `./build_win.sh` (cross-compile with llvm-mingw).
   Paths in the scripts are currently set for the author's machine.

Personal, non-commercial preservation and modding project. Skies of Arcadia is © SEGA.
