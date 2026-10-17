#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# build.sh: regenerate (if needed) and build the native executable
set -e
cd "$(dirname "$0")"
[ -f out/table.c ] || { echo "out/ not found. Generate it first from your own disc:
  python3 tools/ppc2c.py /path/to/game/sys/main.dol config/GEAP8P/symbols.txt out && python3 tools/mknames.py"; exit 1; }
mkdir -p build
cp rt/rt.h out/
for f in out/gen_*.c out/table.c; do o=build/$(basename $f .c).o; [ $o -nt $f ] && [ $o -nt rt/rt.h ] || echo $f; done | xargs -r -P 2 -I{} sh -c 'gcc -O1 -w -c {} -o build/$(basename {} .c).o'
HTDEF="-DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DHAVE_STDINT_H -DDISABLE_SSF -DLSB_FIRST"   # Dreamcast (AICA) only: the Saturn part and its 68000 core are not built
for f in rt/ht/arm.c rt/ht/dcsound.c rt/ht/sega.c rt/ht/yam.c; do o=build/ht_$(basename $f .c).o; [ $o -nt $f ] || gcc -O2 -w $HTDEF -c $f -o $o; done
for f in rt/*.c; do [ -f $f ] && gcc -O2 -g -fno-pie -Wall -Wno-unused -Wno-misleading-indentation -Wno-format-truncation -Iout -I/usr/local/include -c $f -o build/$(basename $f .c).o; done
gcc -no-pie -o soa build/*.o -lm -lpthread -lEGL
echo built
