#!/bin/sh
# build_win.sh: cross-compile the Windows executable with llvm-mingw
set -e
cd /home/claude/recomp
T=/home/claude/decomp/tools/mingw/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64/bin
CC=$T/x86_64-w64-mingw32-clang
mkdir -p build_win
cp rt/rt.h out/
for f in out/gen_*.c out/table.c; do o=build_win/$(basename $f .c).o; [ $o -nt $f ] && [ $o -nt rt/rt.h ] || echo $f; done | xargs -r -P 2 -I{} sh -c "$CC -O1 -w -c {} -o build_win/\$(basename {} .c).o"
$CC -c rt/dolblob.S -o build_win/dolblob.o
HTDEF="-DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DHAVE_STDINT_H -DUSE_M68K -DLSB_FIRST"
for f in rt/ht/*.c rt/ht/m68k/*.c; do o=build_win/ht_$(basename $f .c).o; [ $o -nt $f ] || $CC -O2 -w $HTDEF -c $f -o $o; done
for f in rt/*.c; do $CC -O2 -Wall -Wno-unused -Iout -c $f -o build_win/$(basename $f .c).o; done
$CC -o SkiesOfArcadia.exe build_win/*.o -lopengl32 -lgdi32 -luser32 -lwinmm -static
echo built-win
