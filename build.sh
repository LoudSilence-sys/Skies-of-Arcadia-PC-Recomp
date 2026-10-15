#!/bin/sh
# build.sh: regenerate (if needed) and build the native executable
set -e
cd /home/claude/recomp
mkdir -p build
cp rt/rt.h out/
for f in out/gen_*.c out/table.c; do o=build/$(basename $f .c).o; [ $o -nt $f ] && [ $o -nt rt/rt.h ] || echo $f; done | xargs -r -P 2 -I{} sh -c 'gcc -O1 -w -c {} -o build/$(basename {} .c).o'
gcc -c rt/dolblob.S -o build/dolblob.o
HTDEF="-DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DHAVE_STDINT_H -DUSE_M68K -DLSB_FIRST"
for f in rt/ht/*.c rt/ht/m68k/*.c; do o=build/ht_$(basename $f .c).o; [ $o -nt $f ] || gcc -O2 -w $HTDEF -c $f -o $o; done
for f in rt/*.c; do [ -f $f ] && gcc -O2 -g -fno-pie -Wall -Wno-unused -Iout -I/usr/local/include -c $f -o build/$(basename $f .c).o; done
gcc -no-pie -o soa build/*.o -lm -lpthread -lEGL
echo built
