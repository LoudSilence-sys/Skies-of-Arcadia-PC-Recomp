#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
"""mkcard.py - build a formatted GameCube memory card image (.raw) and optionally import .gci saves.
usage: mkcard.py out.raw [save1.gci ...]
Card: 251 blocks (16 Mbit), ANSI encoding, flash ID all zero (the runtime derives the SRAM flash ID from the header)."""
import struct, sys, time

BLOCK = 0x2000
SIZE_MB = 0x10
NBLOCKS = SIZE_MB * 1024 * 1024 // 8 // BLOCK   # 256 incl. 5 system blocks


def csum(data):
    a = b = 0
    for i in range(0, len(data), 2):
        w = struct.unpack_from(">H", data, i)[0]
        a = (a + w) & 0xFFFF
        b = (b + (w ^ 0xFFFF)) & 0xFFFF
    if a == 0xFFFF: a = 0
    if b == 0xFFFF: b = 0
    return a, b


def header(fmt_time):
    h = bytearray(b"\xff" * BLOCK)
    h[0:0x200] = b"\x00" * 0x200
    rnd = fmt_time
    for i in range(12):   # serial = flashID(0) + rand, as CARDFormat does
        rnd = ((rnd * 0x41C64E6D + 0x3039) >> 16) & 0xFFFFFFFFFFFFFFFF
        h[i] = rnd & 0xFF
        rnd = ((rnd * 0x41C64E6D + 0x3039) >> 16) & 0x7FFF
    struct.pack_into(">QIIIHHH", h, 12, fmt_time, 0, 0, 0, 0, SIZE_MB, 0)
    h[0x26:0x1FC] = b"\xff" * (0x1FC - 0x26)
    struct.pack_into(">HH", h, 0x1FC, *csum(h[0:0x1FC]))
    return h


def main():
    out = sys.argv[1]
    gcis = [open(p, "rb").read() for p in sys.argv[2:]]
    fmt_time = int(time.time() * 40500000) & 0xFFFFFFFFFFFF
    entries = []
    bat_map = [0] * (0xFFB)
    data_blocks = {}
    nxt = 5
    for g in gcis:
        ent = bytearray(g[:0x40])
        n = struct.unpack_from(">H", ent, 0x38)[0]
        if len(g) < 0x40 + n * BLOCK: sys.exit("truncated gci")
        struct.pack_into(">H", ent, 0x36, nxt)
        for k in range(n):
            blk = nxt + k
            data_blocks[blk] = g[0x40 + k * BLOCK: 0x40 + (k + 1) * BLOCK]
            bat_map[blk - 5] = blk + 1 if k < n - 1 else 0xFFFF
        nxt += n
        entries.append(ent)
        print("imported %s %s, %d blocks" % (ent[0:6].decode("ascii", "replace"), ent[8:0x28].split(b"\0")[0].decode("ascii", "replace"), n))
    if nxt > NBLOCKS: sys.exit("saves do not fit")
    card = bytearray(b"\xff" * (NBLOCKS * BLOCK))   # erased flash
    card[0:BLOCK] = header(fmt_time)
    for i, upd in ((1, 0), (2, 1)):
        d = bytearray(b"\xff" * BLOCK)
        for k, e in enumerate(entries): d[k * 0x40:(k + 1) * 0x40] = e
        struct.pack_into(">H", d, 0x1FFA, upd)
        struct.pack_into(">HH", d, 0x1FFC, *csum(d[0:0x1FFC]))
        card[i * BLOCK:(i + 1) * BLOCK] = d
    for i, upd in ((3, 0), (4, 1)):
        b = bytearray(b"\x00" * BLOCK)
        struct.pack_into(">HHH", b, 4, upd, NBLOCKS - nxt, nxt - 1)
        for k, v in enumerate(bat_map): struct.pack_into(">H", b, 0xA + 2 * k, v)
        struct.pack_into(">HH", b, 0, *csum(b[4:BLOCK]))
        card[i * BLOCK:(i + 1) * BLOCK] = b
    for blk, dat in data_blocks.items(): card[blk * BLOCK:(blk + 1) * BLOCK] = dat
    open(out, "wb").write(card)
    print("wrote", out, len(card), "bytes")


main()
