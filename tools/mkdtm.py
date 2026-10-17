#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
"""mkdtm.py - write a Dolphin input movie (.dtm) from a press script, for reference captures.
usage: mkdtm.py out.dtm NINPUTS "idx:button:len,idx:button:len,..."
buttons: start a b x y z up down left right l r   (up/down/left/right = main stick)
One ControllerState per pad poll; the movie starts from boot with slot A memory card inserted."""
import struct, sys

BITS = {'start': 0, 'a': 1, 'b': 2, 'x': 3, 'y': 4, 'z': 5, 'dup': 6, 'ddown': 7, 'dleft': 8, 'dright': 9, 'l': 10, 'r': 11}


def main():
    out, n, script = sys.argv[1], int(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else ''
    ev = []
    for item in filter(None, script.split(',')):
        i, b, l = item.split(':'); ev.append((int(i), b, int(l)))
    h = bytearray(256)
    h[0:4] = b'DTM\x1a'; h[4:10] = b'GEAP8P'; h[10] = 0; h[11] = 1; h[12] = 0
    struct.pack_into('<QQQ', h, 0x0D, n, n, 0)
    # bSaveConfig=0 keeps Dolphin's own config; memcards: slot A; bClearSave=0
    off = 0x89
    h[off + 14] = 1      # memcards
    h[off + 15] = 0      # bClearSave
    struct.pack_into('<Q', h, 0xED, 1 << 62)   # tickCount: playback ends when the emulated clock passes this, so keep it far away
    body = bytearray()
    for k in range(n):
        bits = 1 << 14   # is_connected
        sx = sy = 0x80
        for i, b, l in ev:
            if i <= k < i + l:
                if b in BITS: bits |= 1 << BITS[b]
                elif b == 'up': sy = 0xE0
                elif b == 'down': sy = 0x20
                elif b == 'left': sx = 0x20
                elif b == 'right': sx = 0xE0
        body += struct.pack('<HBBBBBB', bits, 0, 0, sx, sy, 0x80, 0x80)
    open(out, 'wb').write(bytes(h) + bytes(body))


main()
