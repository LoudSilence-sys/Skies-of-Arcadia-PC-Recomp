# Development notes

## Layout

| Path | Contents |
|---|---|
| `tools/ppc2c.py` | Recompiler: DOL + symbols → `out/gen_*.c`, `out/funcs.h`, `out/table.c` |
| `tools/mknames.py` | `names.txt` → `out/names.h` (`FN_<name>` macros for hooks) |
| `config/GEAP8P/` | decomp-toolkit config (function boundaries, splits) |
| `rt/runtime.c` | Boot, DOL loading and SHA-1 check, settings, main entry |
| `rt/sched.c` | Time base, decrementer, interrupts, frame pacing |
| `rt/hw.c` | MMIO: VI, PI, DI (disc), EXI (memory card, SRAM, RTC), SI (controllers), AI, DSP mailbox, ARAM |
| `rt/fifo.c`, `rt/gx.c`, `rt/gl_backend.c` | GX command processor, vertex/state handling, OpenGL renderer |
| `rt/ax.c`, `rt/dcsnd.c` | GameCube AX audio, Dreamcast sound bridge |
| `rt/hle_os.c`, `rt/patches.c` | High-level replacements for a few OS functions; game hooks (text speed, encounters, HUD, battle detection) |
| `rt/savestate.c` | Save states, rewind, thumbnails |
| `rt/pcfeat.c`, `rt/options.c`, `rt/input.c`, `rt/win32.c` | PC features, options UI, input mapping, Windows platform layer |

## Overriding a game function

Every recompiled function `r_8010D8E4` has a weak alias `f_8010D8E4`, and all calls go through `f_`.
Define `f_8010D8E4` in a runtime file to replace it. Call `r_8010D8E4` from it to run the original.

## Verifying against Dolphin

Changes to rendering, audio or hardware behaviour are checked against Dolphin running the same game:

1. `tools/mkdtm.py` writes a Dolphin input movie from the same press script used for `SOA_INPUT`.
2. `tools/dolphin_movie.sh` / `dolphin_ref.sh` / `dolphin_texdump.sh` run headless Dolphin with that movie and
   a memory card, and dump frames (or textures).
3. `SOA_DUMP=N` dumps every Nth frame from the recompiled build. `tools/framecmp.py REF OURS` reports the
   frames that differ most.

These scripts need `DOLPHIN` (path to `dolphin-emu-nogui`), `DOLPHIN_USER` (a Dolphin user folder) and
`GAME_DIR` (the extracted game). `tools/dis.sh` needs `DECOMP_DIR`, a decomp-toolkit project for GEAP8P.

Headless Linux runs: `EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 ./soa "$GAME_DIR"`.

## Environment variables

These are for testing and debugging. None are needed to play.

**Run control**

| Variable | Meaning |
|---|---|
| `SOA_SECONDS=n` | Exit after n seconds of game time |
| `SOA_INPUT="frame:button:len,..."` | Scripted controller input. `SOA_INPUT_XFB=1` counts frames by XFB copies instead of fields |
| `SOA_CARD=path` | Memory card image (default `MemoryCardA.raw`) |
| `SOA_SOFT=1` | Software renderer instead of OpenGL |
| `SOA_SCALE`, `SOA_MSAA`, `SOA_WS`, `SOA_INTERP`, `SOA_FPS60` | Override the matching `settings.ini` values |

**Save states**

| Variable | Meaning |
|---|---|
| `SOA_QSAVE=f1,f2..` / `SOA_QLOAD=f1,f2..` | Save / load a state at the given frames |
| `SOA_QRESUME=1` | Always use the unwind-and-re-enter load path |
| `SOA_QCHAIN=1` | Log saved call chains |
| `SOA_IMPORT=dir` | Start from a RAM/ARAM/GPU register dump taken in a locally modified Dolphin (see `tools/fstfix.py`). Test use only |
| `SOA_REWIND=a,b` | Hold rewind from frame a to b |

**Output and dumps**

| Variable | Meaning |
|---|---|
| `SOA_DUMP=n` | Write every nth frame to `frames/` |
| `SOA_DUMPRANGE=a,b` | Limit frame dumps to a range |
| `SOA_SHOT=frame` | Take a screenshot at a frame |
| `SOA_RAMDUMP=a-b:step,...` | Dump guest RAM on the given frames |
| `SOA_WAV=path` | Record the audio output |
| `SOA_DCDUMP=path` | Record raw Dreamcast sound output |

**Logging and tests**

| Variable | Meaning |
|---|---|
| `SOA_PROF=1` | Sampling profiler, Linux only (`tools/prof.py` reads its output) |
| `SOA_TXTLOG`, `SOA_TXTLOG2` | Log message-window text |
| `SOA_ENCLOG` | Log encounter rolls |
| `SOA_COPYLOG`, `SOA_TEXLOG` | Log EFB copies, texture loads |
| `SOA_MCLOG`, `SOA_DSPLOG`, `SOA_DCLOG` | Log memory-card commands, DSP mailbox, Dreamcast sound |
| `SOA_DBGFRAME=n`, `SOA_DBGOBJ`, `SOA_DBGPIX=x,y`, `SOA_DBGRANGE=a,b`, `SOA_DBGTEX=addr,...`, `SOA_DBGTEXF`, `SOA_REDDBG`, `SOA_BT_FRAME` | Draw-call level GPU debugging for one frame |
| `SOA_OPTTEST=frame,page,row`, `SOA_POSTTEST=frame`, `SOA_CARDOPS=ops` | Drive the options screen, output filters and card manager in headless tests |
| `SOA_NODC`, `SOA_DCTEST`, `SOA_DCROOT=dir`, `SOA_DCGAIN=x`, `SOA_DCNOSKIP` | Dreamcast sound: disable, test, extra disc root, gain, no idle skipping |
| `SOA_INTERP_T=t`, `SOA_INTERPLOG`, `SOA_VIDEFER=n`, `SOA_DIFAST`, `SOA_EXP` | Interpolation factor and log, VI deferral, instant disc reads, experiments |

## Rules for the repository

- Never commit anything generated from the game (`out/`, `names.h`), game files, saves, audio, dumps,
  screenshots of game frames, or binaries. `.gitignore` covers the usual ones.
- New third-party code needs its licence added to `THIRD_PARTY_NOTICES.md` and `licenses/`, and a note in the
  file header when it is ported rather than copied.
- New options must default to the original game's behaviour.
