#!/usr/bin/env python3
import subprocess, bisect, collections, sys
syms = []
for l in subprocess.run(['nm', '-n', '--defined-only', sys.argv[1] if len(sys.argv) > 1 else 'soa'], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
c = collections.Counter()
samples = [int(x, 16) for x in open('prof.txt')]
for pc in samples:
    i = bisect.bisect_right(addrs, pc) - 1
    c[syms[i][1] if i >= 0 else '?'] += 1
names = {}
try:
    for l in open('names.txt'): a, n = l.split(); names['r_' + a] = n
except Exception: pass
for n, k in c.most_common(40): print('%6.2f%% %s %s' % (100.0 * k / len(samples), n, names.get(n, '')))
