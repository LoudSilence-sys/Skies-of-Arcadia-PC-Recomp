#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# dolphin_movie.sh MOVIE.dtm CARD.raw OUTDIR SECONDS [EVERY]
# Run headless Dolphin on the clean game with an input movie and a raw memory card in slot A,
# keeping every EVERY-th dumped frame (default 25) in OUTDIR.
# DOLPHIN: dolphin-emu-nogui binary; DOLPHIN_USER: a Dolphin user folder; GAME_DIR: the extracted PAL game folder
: "${DOLPHIN:?set DOLPHIN to dolphin-emu-nogui}"; : "${DOLPHIN_USER:?set DOLPHIN_USER to a Dolphin user directory}"; : "${GAME_DIR:?set GAME_DIR to the extracted game}"
D=$DOLPHIN
U=$DOLPHIN_USER
FR=$U/Dump/Frames
EVERY=${5:-25}
mkdir -p "$3"; rm -f $FR/framedump_*.png
EGL_PLATFORM=surfaceless timeout $4 $D -u $U -p headless -v OGL \
  -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend=Null -C Dolphin.Core.EmulationSpeed=0 \
  -C Dolphin.Core.SlotA=1 -C Dolphin.Core.MemcardAPath="$2" \
  -C Graphics.Settings.DumpFramesAsImages=True -C Graphics.Settings.FrameDumpsResolutionType=2 -C Dolphin.Movie.DumpFrames=True \
  -m "$1" -e "$GAME_DIR/sys/main.dol" >/tmp/dolphin_movie.log 2>&1 &
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
