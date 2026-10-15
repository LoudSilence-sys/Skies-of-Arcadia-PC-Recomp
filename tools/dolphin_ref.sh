#!/bin/sh
# run Dolphin on the clean game and keep every 25th dumped frame
D=/home/claude/build/dolphin/b2/Binaries/dolphin-emu-nogui
OUT=/home/claude/refframes
FR=/home/claude/dolu/Dump/Frames
EGL_PLATFORM=surfaceless timeout ${1:-300} $D -u /home/claude/dolu -p headless -v OGL \
  -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend=Null -C Dolphin.Core.EmulationSpeed=0 \
  -C GFX.Settings.DumpFramesAsImages=True -C Dolphin.Movie.DumpFrames=True \
  -e "/home/claude/game/SOA Legends/sys/main.dol" >/tmp/dolphin_ref.log 2>&1 &
P=$!
while kill -0 $P 2>/dev/null; do
  sleep 5
  for f in $FR/framedump_*.png; do
    [ -e "$f" ] || continue
    n=$(basename $f .png | sed 's/framedump_//')
    if [ $((n % 25)) -eq 0 ]; then mv "$f" $OUT/ 2>/dev/null; else rm -f "$f"; fi
  done
done
echo done; ls $OUT | wc -l
