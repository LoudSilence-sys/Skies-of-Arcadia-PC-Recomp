#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
"""sigmatch.py OBJDIR... -> match reference functions (from MWCC .o files) against the DOL.
Relocated instruction fields are masked on both sides. Output: addr name (unique matches only)."""
import struct, sys, os, re, glob, collections

def elf_funcs(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF': return []
    shoff, = struct.unpack('>I', d[0x20:0x24]); shnum, shstrndx = struct.unpack('>HH', d[0x30:0x34])
    sh = [struct.unpack('>IIIIIIIIII', d[shoff + i * 40:shoff + i * 40 + 40]) for i in range(shnum)]
    def secdata(i): s = sh[i]; return d[s[4]:s[4] + s[5]]
    names = secdata(shstrndx)
    def nm(off, tab): e = tab.index(b'\0', off); return tab[off:e].decode()
    symtab = [i for i, s in enumerate(sh) if s[1] == 2]
    if not symtab: return []
    st = sh[symtab[0]]; strtab = secdata(st[6]); sd = secdata(symtab[0])
    syms = []
    for k in range(0, len(sd), 16):
        n, v, sz, info, oth, shn = struct.unpack('>IIIBBH', sd[k:k + 16])
        syms.append((nm(n, strtab), v, sz, info & 15, shn))
    out = []
    for i, s in enumerate(sh):
        if nm(s[0], names) != '.text': continue
        text = secdata(i)
        relmask = set()
        for j, r in enumerate(sh):
            if r[1] == 4 and r[7] == i:
                rd = secdata(j)
                for k in range(0, len(rd), 12):
                    off, info, add = struct.unpack('>IIi', rd[k:k + 12])
                    relmask.add(off & ~3)
        for name, v, sz, typ, shn in syms:
            if shn == i and typ == 2 and sz >= 8:
                words = [struct.unpack('>I', text[v + o:v + o + 4])[0] for o in range(0, sz, 4)]
                masks = [(v + o) in relmask for o in range(0, sz, 4)]
                out.append((name, words, masks, os.path.basename(path)))
    return out

def canon(w):
    """mask fields that typically differ by relocation in the target (we have no relocs there)."""
    op = w >> 26
    if op == 18: return w & 0xFC000003
    if op in (14, 15, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 50, 52, 54) and ((w >> 16) & 31) in (0, 2, 13):
        return w & 0xFFFF0000
    if op == 15: return w & 0xFFFF0000
    return w

def main():
    dol = sys.argv[1]; syms = sys.argv[2]; objdirs = sys.argv[3:]
    d = open(dol, 'rb').read()
    offs = struct.unpack('>18I', d[0:72]); addrs = struct.unpack('>18I', d[72:144]); sizes = struct.unpack('>18I', d[144:216])
    def rd(a, n):
        for i in range(18):
            if sizes[i] and addrs[i] <= a < addrs[i] + sizes[i]: return d[offs[i] + a - addrs[i]:offs[i] + a - addrs[i] + n]
    targets = collections.defaultdict(list)
    for l in open(syms):
        m = re.match(r'(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)', l)
        if m:
            a = int(m.group(3), 16); s = int(m.group(4), 16)
            b = rd(a, s); ws = [struct.unpack('>I', b[o:o + 4])[0] for o in range(0, s, 4)]
            targets[s].append((a, m.group(1), ws))
    refs = []
    for od in objdirs:
        for p in sorted(glob.glob(os.path.join(od, '*.o'))): refs += elf_funcs(p)
    found = collections.defaultdict(set)
    for name, words, masks, src in refs:
        n = len(words) * 4
        sig = [None if m else canon(w) for w, m in zip(words, masks)]
        for a, tname, ws in targets.get(n, []):
            ok = True
            for s_, w in zip(sig, ws):
                if s_ is not None and s_ != canon(w): ok = False; break
            if ok: found[a].add(name)
    nuniq = 0
    for a in sorted(found):
        ns = found[a]
        if len(ns) == 1:
            print('%08X %s' % (a, next(iter(ns)))); nuniq += 1
        else:
            print('%08X ?%s' % (a, ','.join(sorted(ns))))
    print('matched', nuniq, 'ambiguous', len(found) - nuniq, file=sys.stderr)

main()
