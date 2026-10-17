#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 LoudSilence-sys and contributors
# names.txt -> out/names.h: '#define FN_<name> f_<addr>' for every unique, C-safe name
import re, collections, os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ns = [l.split() for l in open(os.path.join(root, 'names.txt'))]
cnt = collections.Counter(n for a, n in ns)
with open(os.path.join(root, 'out', 'names.h'), 'w') as f:
    f.write('#pragma once\n')
    for a, n in ns:
        if cnt[n] == 1 and re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', n) and not n.startswith('fn_'):
            f.write('#define FN_%s f_%s\n#define RN_%s r_%s\n#define ADDR_%s 0x%sU\n' % (n, a, n, a, n, a))
