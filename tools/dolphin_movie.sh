#!/bin/sh
# dolphin_movie.sh MOVIE.dtm CARD.raw OUTDIR SECONDS [EVERY]
# Run headless Dolphin on the clean game with an input movie and a raw memory card in slot A,
# keeping every EVERY-th dumped frame (default 25) in OUTDIR.
D=/home/claude/build/dolphin/b2/Binaries/dolphin-emu-nogui
U=/home/claude/dolu
FR=$U/Dump/Frames
EVERY=${5:-25}
mkdir -p "$3"; rm -f $FR/framedump_*.png
EGL_PLATFORM=surfaceless timeout $4 $D -u $U -p headless -v OGL \
  -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend=Null -C Dolphin.Core.EmulationSpeed=0 \
  -C Dolphin.Core.SlotA=1 -C Dolphin.Core.MemcardAPath="$2" \
  -C GFX.Settings.DumpFramesAsImages=True -C Dolphin.Movie.DumpFrames=True \
  -m "$1" -e "/home/claude/game/SOA Legends/sys/main.dol" >/tmp/dolphin_movie.log 2>&1 &
P=$!
while kill -0 $P 2>/dev/null; do
  sleep 5
  for f in $FR/framedump_*.png; do
    [ -e "$f" ] || continue
    n=$(basename $f .png | sed 's/framedump_//')
    if [ $((n % EVERY)) -eq 0 ]; then mv "$f" "$3"/ 2>/dev/null; else rm -f "$f"; fi
  done
done
echo done; ls "$3" | wc -l
