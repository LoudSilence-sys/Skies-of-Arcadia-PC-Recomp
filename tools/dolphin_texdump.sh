#!/bin/sh
# dolphin_texdump.sh MOVIE.dtm CARD.raw OUTDIR SECONDS
# Like dolphin_movie.sh, but also dumps every texture Dolphin loads (Dump/Textures/GEAP8P) and keeps every 100th frame.
D=/home/claude/build/dolphin/b2/Binaries/dolphin-emu-nogui
U=/home/claude/dolu
FR=$U/Dump/Frames
mkdir -p "$3"; rm -f $FR/framedump_*.png; rm -rf "$U/Dump/Textures/GEAP8P"
EGL_PLATFORM=surfaceless timeout $4 $D -u $U -p headless -v OGL \
  -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend=Null -C Dolphin.Core.EmulationSpeed=0 \
  -C Dolphin.Core.SlotA=1 -C Dolphin.Core.MemcardAPath="$2" \
  -C Graphics.Settings.DumpFramesAsImages=True -C Graphics.Settings.FrameDumpsResolutionType=2 -C Dolphin.Movie.DumpFrames=True \
  -C Graphics.Settings.DumpTextures=True -C Graphics.Settings.DumpBaseTextures=True -C Graphics.Settings.DumpMipmapTextures=False \
  -m "$1" -e "/home/claude/game/SOA Legends/sys/main.dol" >/tmp/dolphin_texdump.log 2>&1 &
P=$!
while kill -0 $P 2>/dev/null; do
  sleep 5
  for f in $FR/framedump_*.png; do
    [ -e "$f" ] || continue
    n=$(basename $f .png | sed 's/framedump_//')
    if [ $((n % 100)) -eq 0 ]; then mv "$f" "$3"/ 2>/dev/null; else rm -f "$f"; fi
  done
done
echo done; ls "$3" | wc -l; ls "$U/Dump/Textures/GEAP8P" | wc -l
