#!/usr/bin/env python3
# names.txt -> out/names.h: '#define FN_<name> f_<addr>' for every unique, C-safe name
import re, collections
ns = [l.split() for l in open('/home/claude/recomp/names.txt')]
cnt = collections.Counter(n for a, n in ns)
with open('/home/claude/recomp/out/names.h', 'w') as f:
    f.write('#pragma once\n')
    for a, n in ns:
        if cnt[n] == 1 and re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', n) and not n.startswith('fn_'):
            f.write('#define FN_%s f_%s\n#define RN_%s r_%s\n#define ADDR_%s 0x%sU\n' % (n, a, n, a, n, a))
