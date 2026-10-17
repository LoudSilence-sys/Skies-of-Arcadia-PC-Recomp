# Third-party code, data and credits

This project is licensed under the **GNU General Public License v3.0 or later** (see `LICENSE`).
It includes, ports or links the work of other people, listed below with their licences.
If you spot something missing or wrongly attributed, please open an issue. It will be fixed.

## Code included in this repository

| Component | Where | Author(s) | Licence | Notes |
|---|---|---|---|---|
| Highly Theoretical (SegaCore): Yamaha AICA and ARM7 emulation | `rt/ht/` | Neill Corlett; maintained by Christopher Snowhill (kode54) | GPL-3.0 (`rt/ht/COPYING.txt`) | Unmodified copies of `arm`, `dcsound`, `sega`, `yam` and `emuconfig` from [kode54/Highly_Theoretical](https://github.com/kode54/Highly_Theoretical) at commit `2998a4b`. Only the Dreamcast path is built (`-DDISABLE_SSF`). The Saturn/68000 parts are not included. |
| stb_image 2.30, stb_image_write 1.16 | `rt/ext/` | Sean Barrett and contributors | Public domain / MIT (dual) | Used for texture-pack PNG loading, screenshots and state thumbnails. |
| xxHash | `rt/ext/xxhash.h` | Yann Collet | BSD-2-Clause (`licenses/xxHash-BSD-2-Clause.txt`) | XXH64 is used to name textures the same way Dolphin does. |

## Code ported from or modelled on Dolphin

[Dolphin](https://github.com/dolphin-emu/dolphin) is licensed GPL-2.0-or-later (`licenses/GPL-2.0-or-later.txt`).
This combination is distributed under GPL-3.0-or-later. Much of this runtime would not exist without the
Dolphin team's years of hardware research. The parts that were ported or closely follow Dolphin are:

| File | What comes from Dolphin |
|---|---|
| `rt/ax.c` | Port of DSPHLE's AX microcode implementation (`AX.cpp`, `AXVoice.h`): voice mixing, ADPCM, polyphase resampling, aux buses. |
| `rt/gl_backend.c` | The TEV / indirect texturing / fog shader generator, blend-state derivation and EFB-copy encoders, after VideoCommon's `PixelShaderGen`, `RenderState` and `TextureConversionShader`. |
| `rt/gx.c` | Texture format decoding, lighting equations, EFB-copy semantics, and the Dolphin texture-dump naming scheme (`tex1_WxH_<hash>_<fmt>`) that makes Dolphin HD texture packs load unchanged. |
| `rt/fifo.c` | Command-processor opcode handling and CP register behaviour (`OpcodeDecoding`, `CPMemory`). |
| `rt/hw.c` | The memory card device (`CEXIMemoryCard`, GCI and raw card layout). Other hardware behaviour (EXI, SI, VI, DI, interrupts) was checked against Dolphin. |
| `rt/runtime.c` | Low-memory boot state (after `CBoot::SetupGCMemory`). |
| `dsp_coef.bin` (not in the repository) | The runtime expects Dolphin's **free replacement** DSP coefficient table (`Data/Sys/GC/dsp_coef.bin` from any Dolphin install, made by the Dolphin team) next to the executable. No Nintendo ROM is used or distributed. |

Dolphin was also the reference for every accuracy fix. Frames, RAM and GPU registers from Dolphin runs were
compared against the recompiled build (`tools/dolphin_*.sh`, `tools/framecmp.py`). Local debugging
changes to Dolphin used for that comparison are not part of this project.

## Fonts (embedded as images)

| Font | Where | Licence |
|---|---|---|
| DejaVu Sans Bold | `rt/ui_font.c`: options-screen glyph atlas | Bitstream Vera Fonts licence plus DejaVu public-domain changes (`licenses/DejaVu-fonts.txt`) |
| Almendra Bold, © 2011-2012 Ana Sanfelippo | `rt/options_label.c`: the "Options" title text | SIL Open Font License 1.1 (`licenses/OFL-1.1.txt`) |

## Used by Windows builds (not in this repository)

| Component | Licence |
|---|---|
| SDL2 2.30.9 (`SDL2.dll`, unmodified, loaded at run time for controllers and rumble), © Sam Lantinga | zlib (`licenses/SDL2-zlib.txt`) |

## Tools and references (not included, but this work depends on them)

- **[decomp-toolkit](https://github.com/encounter/decomp-toolkit)** (Luke Street, MIT): the function boundaries in
  `config/GEAP8P/` come from a dtk project for this game, and `tools/dis.sh` reads dtk's disassembly.
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** (Mr-Wiseguy) and the wider static-recompilation scene:
  the approach of translating every function to C with overridable symbols follows their lead.
- **[YAGCD](https://www.gc-forever.com/yagcd/)** (Yet Another GameCube Documentation): hardware register reference.
- **Dolphin's free DSP ROM** authors (LM, delroth and the Dolphin team).

## The game

*Skies of Arcadia* and *Skies of Arcadia Legends* are © SEGA. This project is not affiliated with or endorsed
by SEGA or Nintendo. **This repository contains no game code, data, audio, keys or BIOS/ROM images, and the project publishes
no binaries.** The recompiled C is generated on your machine from your own copy of the game, and the
game's executable is read from your disc folder at run time (checked by SHA-1). A build made this way
contains code translated from SEGA's game, so it is for your personal use and must not be shared.
