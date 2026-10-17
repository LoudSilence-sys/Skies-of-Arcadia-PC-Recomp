#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# build_win.sh: cross-compile the Windows executable with llvm-mingw
set -e
cd "$(dirname "$0")"
[ -f out/table.c ] || { echo "out/ not found. Generate it first from your own disc:
  python3 tools/ppc2c.py /path/to/game/sys/main.dol config/GEAP8P/symbols.txt out && python3 tools/mknames.py"; exit 1; }
# LLVM_MINGW: path to an llvm-mingw toolchain (https://github.com/mstorsjo/llvm-mingw)
T=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw toolchain directory}/bin
CC=$T/x86_64-w64-mingw32-clang
mkdir -p build_win
cp rt/rt.h out/
for f in out/gen_*.c out/table.c; do o=build_win/$(basename $f .c).o; [ $o -nt $f ] && [ $o -nt rt/rt.h ] || echo $f; done | xargs -r -P 4 -I{} sh -c "$CC -O2 -w -c {} -o build_win/\$(basename {} .c).o"
HTDEF="-DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DHAVE_STDINT_H -DDISABLE_SSF -DLSB_FIRST"   # Dreamcast (AICA) only: the Saturn part and its 68000 core are not built
for f in rt/ht/arm.c rt/ht/dcsound.c rt/ht/sega.c rt/ht/yam.c; do o=build_win/ht_$(basename $f .c).o; [ $o -nt $f ] || $CC -O2 -w $HTDEF -c $f -o $o; done
for f in rt/*.c; do $CC -O2 -Wall -Wno-unused -Iout -c $f -o build_win/$(basename $f .c).o; done
$CC -o SkiesOfArcadia.exe build_win/*.o -lopengl32 -lgdi32 -luser32 -lwinmm -lpthread -static
echo built-win
