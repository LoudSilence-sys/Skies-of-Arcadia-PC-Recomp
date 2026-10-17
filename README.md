# Skies of Arcadia Legends: PC static recompilation

A native Windows port of **Skies of Arcadia Legends** (GameCube, PAL `GEAP8P`), made by **static
recompilation**. The game's PowerPC code is translated to C ahead of time and compiled for x86-64. It runs on
a small runtime that stands in for the GameCube hardware (GPU, DSP audio, memory card, controllers).
There is no CPU emulator, and no Dolphin code runs at play time.

> **Work in progress.** It is playable, but it has not been played start to finish. See [Status](#status).

**Bring your own game.** This repository contains **no game code, data, audio, encryption keys or ROM
images**, and the project publishes **no binaries**. Everything game-specific is generated on your machine
from your own disc. A build you make contains code translated from SEGA's game, so keep it to yourself.

---

## Status

Tested on one Windows 11 PC (RTX 5080) and in headless Linux runs (Mesa software rendering).
"Verified" means a scripted run reached the point and the result was checked against Dolphin
(frames, RAM or GPU registers), or by the tester playing it.

**Works**
- Boot, title, new game and loading a memory-card save; field, dungeons, airship travel, menus, event scenes
  and dialogue.
- Battles, including the battle transition and the boss fight reached from the test save.
- Memory card saving and loading (raw card image, byte-identical GCI export, GCI import).
- OpenGL 3.3 renderer: TEV, indirect texturing, fog, z-textures, EFB copies on the GPU.
- GameCube DSP audio, and optional Dreamcast music/sound effects (see below).
- Keyboard, XInput, and SDL2 controllers (PlayStation, Switch Pro, GameCube adapter) with rumble.

**Known issues**
- Some slowdown in battles on the test PC (being profiled).
- Some shadows and lighting differ from Dolphin in places.
- Cutscene audio can drift out of sync with the 60 fps option on.
- Rendering is not pixel-exact. Remaining differences are found by frame comparison with Dolphin.

**Not yet verified**
- The full game from start to finish. Testing so far covers the early game.
- The encounter-rate option inside a random-battle area.
- SDL controller types other than Xbox on real hardware, and key/pad rebinding on Windows.
- Save states loaded across very different game situations on Windows (they are verified headless).
- Linux as a playable target: the Linux build is a **headless test build** (offscreen EGL, no window or input).

Only the **PAL** release is supported. The USA (`GEAE8P`) and Japanese (`GEAJ8P`) executables are
different programs and would need their own symbol maps.

## PC features

Everything defaults to the original behaviour. Settings are in the in-game Options screen (title menu, or
**F1** at any time) and in `settings.ini`.

- **Display.** Internal resolution up to 6×, MSAA, anisotropic filtering, borderless fullscreen (F11 /
  Alt+Enter), V-Sync, integer scaling, FXAA / sharpen / CRT output filters.
- **60 fps.** The game logic runs at 30 steps per second. The renderer replays each frame's GPU commands
  with vertices and matrices blended towards the previous frame, and shows that in-between image.
- **Widescreen.** 16:9 with the minimap and battle HUD anchored to the screen edges.
- **HD textures.** Dolphin-format texture packs load unchanged from `Load/Textures/GEAP8P` (same naming and hashes).
- **Save states.** 10 slots with thumbnails: F5 save, F9 load, F6/F7 change slot. Rewind: hold Backspace or Back.
- **Speed.** Fast-forward (Tab / right stick click), battle speed.
- **Gameplay.** Encounter rate, text speed.
- **Audio.** Separate music and effects volume.
- **Other.** Screenshots (F12), performance overlay (F3), pause when unfocused.
- **Memory cards.** Manager page: export, import and delete GCI saves; back up the card.
- **Button names in game text** follow the controller you are using (GameCube, Xbox, PlayStation,
  Switch or keyboard).
- **"Uncensored text."** Restores the original alcohol names (the western releases changed "rum" to
  "loqua"). The other changes made for the western releases were made to the game's models and event data,
  which the GameCube discs do not contain, so they cannot be restored from them.

### Dreamcast sound (optional)

If you also own the **Dreamcast** version, music and sequenced sound effects can be played by the
Dreamcast's own sound driver. It runs on an emulated Yamaha AICA (the Highly Theoretical core), using the
sound banks from your Dreamcast discs. Music that exists only in Legends stays on the GameCube audio. Put
the paths of your extracted Dreamcast disc folders in `dc_path.txt`, one per line.

## Building

You need:

- your own **PAL disc**, extracted with Dolphin (right-click the game → *Properties* → *Filesystem* →
  *Extract Entire Disc*). The folder must contain `sys/main.dol` with SHA-1
  `295a9fc982072bce60734eb6bdb60aec4182b702`, which the tools and the program both check;
- Python 3;
- GCC (Linux) or [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) to cross-compile for Windows;
- for Windows controller support, `SDL2.dll` (2.30.x) from [libsdl.org](https://github.com/libsdl-org/SDL/releases).
  Without it, XInput is used.

```sh
python3 tools/ppc2c.py "/path/to/game/sys/main.dol" config/GEAP8P/symbols.txt out   # generate C into out/ (about 55 MB, stays local)
python3 tools/mknames.py                                                           # readable names for hooks
LLVM_MINGW=/path/to/llvm-mingw ./build_win.sh                                       # -> SkiesOfArcadia.exe
# or ./build.sh for the headless Linux test build
```

Next to the executable, put:

- `game_path.txt`: one line, the extracted game folder (the one containing `sys` and `files`).
- `dsp_coef.bin`: Dolphin's free DSP coefficient table, from `Data/Sys/GC/` (Windows: `Sys/GC/`) in any
  Dolphin install. Without it, audio resampling falls back to linear interpolation.
- optionally `SDL2.dll` and `dc_path.txt`.

`settings.ini` and `MemoryCardA.raw` are created on first run. To bring over a Dolphin save, use
`tools/mkcard.py` or the in-game card manager.

## How it works

- `tools/ppc2c.py` translates every function in the DOL into C. Function boundaries come from
  `config/GEAP8P/symbols.txt`, a [decomp-toolkit](https://github.com/encounter/decomp-toolkit) config. Each
  function becomes `r_XXXXXXXX` with a weak alias `f_XXXXXXXX`, so any function can be replaced in C while still
  calling the original. That is the hook point for fixes, PC features and, later, decompilation for modding.
- `rt/` is the runtime: memory, interrupts and scheduling, disc / EXI / SI / memory card, the GX FIFO and
  OpenGL renderer, the AX DSP microcode, save states, PC features and options.
- **Save states.** The host C stack mirrors the guest call stack. A state is taken at one point (the main
  loop's end-of-frame wait) and records the guest return-address chain. The recompiler emits resume labels
  at call sites, so a state taken under a different chain is restored by unwinding the host stack and
  calling back down that chain.

Development notes, debug variables and the verification tools are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## How this was made

This project was written with substantial help from an AI model (Anthropic's Claude). Commits it
co-wrote carry a `Co-Authored-By: Claude` trailer. Every change was built and run before it was
committed. Rendering, audio and memory-card behaviour were compared against Dolphin running the same game
from the same save and inputs. Things that could not be tested are listed as such above, not claimed to
work. Bug reports with a save file and steps to reproduce are the most useful thing you can send.

## Credits and licence

Licensed under the **GNU GPL v3.0 or later** (`LICENSE`).

This builds directly on other people's work: most of all **Dolphin** (the hardware research, and the
GPU, DSP and memory-card logic this runtime ports), **Highly Theoretical** by Neill Corlett,
**decomp-toolkit** by Luke Street, the **N64Recomp** project that showed the way for static
recompilation, stb, xxHash, SDL, and the DejaVu and Almendra fonts. Full details, licences and the files
each one covers are in **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)** and `licenses/`.

*Skies of Arcadia* is © SEGA. This is an unofficial, non-commercial fan project, not affiliated with or
endorsed by SEGA or Nintendo. Please buy the game.
