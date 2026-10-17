#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# run Dolphin on the clean game and keep every 25th dumped frame
# DOLPHIN: dolphin-emu-nogui binary; DOLPHIN_USER: a Dolphin user folder; GAME_DIR: the extracted PAL game folder
: "${DOLPHIN:?set DOLPHIN to dolphin-emu-nogui}"; : "${DOLPHIN_USER:?set DOLPHIN_USER to a Dolphin user directory}"; : "${GAME_DIR:?set GAME_DIR to the extracted game}"
D=$DOLPHIN
OUT=${OUT:-refframes}; mkdir -p "$OUT"
FR=$DOLPHIN_USER/Dump/Frames
EGL_PLATFORM=surfaceless timeout ${1:-300} $D -u "$DOLPHIN_USER" -p headless -v OGL \
  -C Dolphin.Interface.UsePanicHandlers=False -C Dolphin.DSP.Backend=Null -C Dolphin.Core.EmulationSpeed=0 \
  -C Graphics.Settings.DumpFramesAsImages=True -C Dolphin.Movie.DumpFrames=True \
  -e "$GAME_DIR/sys/main.dol" >/tmp/dolphin_ref.log 2>&1 &
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
