#!/usr/bin/env python3
"""
ppc2c.py - static recompiler: Gekko/PowerPC DOL -> C.

Inputs : main.dol, dtk symbols.txt (function boundaries)
Outputs: out/gen_*.c (functions), out/funcs.h (prototypes), out/table.c (address -> function table)

Model
-----
* Guest RAM is a big-endian byte array; loads/stores go through mem helpers (rt.h).
* CPU state lives in a Ctx struct (r, f[ps0,ps1], cr, lr, ctr, xer, gqr...).
* Every guest function becomes `void f_XXXXXXXX(Ctx* c)`. `blr` -> return.
  `bl X` -> call f_X. `b X` outside the function -> tail call (call + return).
* `bctr` inside a function uses a switch over jump-table targets found in data.
* Calls into the middle of a function create an extra entry point function.
* Functions are emitted weak so the runtime can override any of them (HLE).
"""
import struct, re, sys, os, collections

def s16(x): return x - 0x10000 if x & 0x8000 else x
def sext(x, bits): return x - (1 << bits) if x & (1 << (bits - 1)) else x
def mask(mb, me):
    m = 0
    if mb <= me:
        for i in range(mb, me + 1): m |= 1 << (31 - i)
    else:
        for i in range(0, me + 1): m |= 1 << (31 - i)
        for i in range(mb, 32): m |= 1 << (31 - i)
    return m

class Dol:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        self.offs = struct.unpack('>18I', d[0:72]); self.addrs = struct.unpack('>18I', d[72:144]); self.sizes = struct.unpack('>18I', d[144:216])
        self.bss = struct.unpack('>II', d[0xD8:0xE0]); self.entry = struct.unpack('>I', d[0xE0:0xE4])[0]
    def secs(self):
        return [(i, self.offs[i], self.addrs[i], self.sizes[i]) for i in range(18) if self.sizes[i]]
    def word(self, a):
        for i, o, b, s in self.secs():
            if b <= a < b + s: return struct.unpack('>I', self.d[o + a - b:o + a - b + 4])[0]
        return None

class Gen:
    """Translates one entry (start..end) into C."""
    def __init__(self, R, start, end, fstart, fend):
        self.entry = start; start = fstart
        self.R = R; self.start = start; self.end = end; self.fstart = fstart; self.fend = fend
        self.out = []; self.labels = set(); self.need_dispatch = False
        self.rs = set()   # unconditional call sites: resume points for save-state loading (rs_take)

    def emit(self, s): self.out.append(s)

    # --- helpers for operand text ---
    @staticmethod
    def r(n): return 'c->r[%d]' % n
    @staticmethod
    def ra0(n): return '0' if n == 0 else 'c->r[%d]' % n
    @staticmethod
    def f0(n): return 'c->f[%d][0]' % n
    @staticmethod
    def f1(n): return 'c->f[%d][1]' % n

    def ea_d(self, ra, d):
        return '(u32)(%s + %d)' % (self.ra0(ra), d) if ra else '(u32)%d' % (d & 0xFFFFFFFF)
    def ea_x(self, ra, rb):
        return '(u32)(%s + c->r[%d])' % (self.ra0(ra), rb) if ra else 'c->r[%d]' % rb

    def cr0(self, val):
        self.emit('  SET_CR0(c, (s32)(%s));' % val)

    def target_label(self, t):
        return 'L_%08X' % t

    def branch_to(self, t, link=False, pc=0):
        """Unconditional transfer to t (already decided taken)."""
        if link:
            return 'c->lr = 0x%08X; %s; ' % (pc + 4, self.R.call_expr(t))
        if self.start <= t < self.end:
            self.labels.add(t)
            if t <= pc: return 'IRQ_CHECK(c); goto %s; ' % self.target_label(t)
            return 'goto %s; ' % self.target_label(t)
        if self.fstart <= t < self.fend:
            # jump backwards before this entry inside same function: tail-call that entry
            return '%s; return; ' % self.R.call_expr(t)
        return '%s; return; ' % self.R.call_expr(t)

    def cond_expr(self, bo, bi, use_ctr=True):
        parts = []
        if not (bo & 4) and use_ctr:
            self.emit('  c->ctr--;')
            parts.append('c->ctr != 0' if not (bo & 2) else 'c->ctr == 0')
        if not (bo & 16):
            bit = 'CRBIT(c, %d)' % bi
            parts.append(bit if (bo & 8) else '!' + bit)
        return ' && '.join(parts) if parts else '1'

    def gen(self):
        R = self.R
        pc = self.start
        body = []
        while pc < self.end:
            w = R.dol.word(pc)
            self.out = []
            try:
                self.ins(pc, w)
            except NotImplementedError as e:
                self.out = ['  UNIMPL(c, 0x%08X, 0x%08X); /* %s */' % (pc, w, e)]
                R.unimpl[str(e)] += 1
            body.append((pc, self.out))
            pc += 4
        lines = []
        if self.rs:   # resuming a saved call stack: jump straight back into the call that was in progress
            lines.append('  if (g_rs_n) switch (rs_take(c)) {')
            for pc in sorted(self.rs): lines.append('  case 0x%08X: goto RS_%08X;' % (pc + 4, pc))
            lines.append('  default: return;')
            lines.append('  }')
        if self.entry != self.start:
            self.labels.add(self.entry); lines.append('  goto %s;' % self.target_label(self.entry))
        if self.need_dispatch:
            for t in R.jt_targets.get(self.fstart, []):
                if self.start <= t < self.end: self.labels.add(t)
        for pc, out in body:
            if pc in self.labels: lines.append('%s:;' % self.target_label(pc))
            if pc in self.rs: lines.append('RS_%08X:;' % pc)
            if pc in R.idle_pcs: lines.append('  rt_idle(c);')
            lines.extend(out)
        nxt = self.end
        if nxt in R.entries: lines.append('  f_%08X(c); return; /* fell off end */' % nxt)
        else: lines.append('  return; /* fell off end */')
        if self.need_dispatch:
            lines.append('dispatch:')
            lines.append('  switch (c->ctr) {')
            for t in sorted(self.labels):
                lines.append('  case 0x%08X: goto %s;' % (t, self.target_label(t)))
            lines.append('  default: call_indirect(c, c->ctr); return;')
            lines.append('  }')
        return lines

    def ins(self, pc, w):
        R = self.R; e = self.emit; r = self.r; ra0 = self.ra0
        op = w >> 26
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31; rC = (w >> 6) & 31
        uimm = w & 0xFFFF; simm = s16(uimm); rc = w & 1
        if w == 0:
            e('  UNIMPL(c, 0x%08X, 0);' % pc); return
        # ---------------- branches ----------------
        if op == 18:
            li = sext(w & 0x03FFFFFC, 26); aa = (w >> 1) & 1; lk = w & 1
            t = (li if aa else pc + li) & 0xFFFFFFFF
            if lk: self.rs.add(pc)
            e('  ' + self.branch_to(t, lk, pc)); return
        if op == 16:
            bo = rD; bi = rA; bd = sext(w & 0xFFFC, 16); aa = (w >> 1) & 1; lk = w & 1
            t = (bd if aa else pc + bd) & 0xFFFFFFFF
            cond = self.cond_expr(bo, bi)
            e('  if (%s) { %s}' % (cond, self.branch_to(t, lk, pc))); return
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:  # bclr
                bo = rD; bi = rA; lk = w & 1
                cond = self.cond_expr(bo, bi)
                if lk:
                    e('  if (%s) { u32 t_ = c->lr; c->lr = 0x%08X; call_indirect(c, t_); }' % (cond, pc + 4))
                else:
                    e('  if (%s) return;' % cond)
                return
            if xo == 528:  # bcctr
                bo = rD; bi = rA; lk = w & 1
                cond = self.cond_expr(bo, bi, use_ctr=False)
                if lk:
                    if (bo & 0x14) == 0x14: self.rs.add(pc)
                    e('  if (%s) { c->lr = 0x%08X; call_indirect(c, c->ctr); }' % (cond, pc + 4))
                else:
                    self.need_dispatch = True
                    e('  if (%s) { if (c->ctr >= 0x%08X && c->ctr < 0x%08X) goto dispatch; call_indirect(c, c->ctr); return; }' % (cond, self.fstart, self.fend))
                return
            crd, cra, crb = rD, rA, rB
            ops = {257: 'a & b', 449: 'a | b', 193: 'a ^ b', 225: '!(a & b)', 33: '!(a | b)', 289: 'a == b', 129: 'a & !b', 417: 'a | !b'}
            if xo in ops:
                e('  { int a = CRBIT(c, %d), b = CRBIT(c, %d); SETCRBIT(c, %d, %s); }' % (cra, crb, crd, ops[xo])); return
            if xo == 0:  # mcrf
                e('  SETCRF(c, %d, GETCRF(c, %d));' % (rD >> 2, rA >> 2)); return
            if xo in (150,):  # isync
                return
            if xo == 50:  # rfi
                e('  RFI(c); return;'); return
            raise NotImplementedError('op19_%d' % xo)
        if op == 17:
            e('  SYSCALL(c, 0x%08X);' % pc); return
        if op == 3:
            e('  TRAP(c, 0x%08X);' % pc); return
        # ---------------- integer immediate ----------------
        if op == 14: e('  %s = %s;' % (r(rD), ('(u32)%d' % simm) if rA == 0 else '%s + %d' % (r(rA), simm))); return
        if op == 15: e('  %s = %s;' % (r(rD), ('0x%08Xu' % ((uimm << 16) & 0xFFFFFFFF)) if rA == 0 else '%s + 0x%08Xu' % (r(rA), (uimm << 16) & 0xFFFFFFFF))); return
        if op == 12 or op == 13:
            e('  { u32 a = %s, v = a + %d; SETCA(c, v < a); %s = v; }' % (r(rA), simm, r(rD)))
            if op == 13: self.cr0(r(rD))
            return
        if op == 8:
            e('  { u32 b = %s; u32 v = (u32)%d - b; SETCA(c, b <= (u32)%d); %s = v; }' % (r(rA), simm, simm, r(rD))); return
        if op == 7: e('  %s = (u32)((s32)%s * %d);' % (r(rD), r(rA), simm)); return
        if op == 24: e('  %s = %s | 0x%Xu;' % (r(rA), r(rD), uimm)); return
        if op == 25: e('  %s = %s | 0x%08Xu;' % (r(rA), r(rD), uimm << 16)); return
        if op == 26: e('  %s = %s ^ 0x%Xu;' % (r(rA), r(rD), uimm)); return
        if op == 27: e('  %s = %s ^ 0x%08Xu;' % (r(rA), r(rD), uimm << 16)); return
        if op == 28: e('  %s = %s & 0x%Xu;' % (r(rA), r(rD), uimm)); self.cr0(r(rA)); return
        if op == 29: e('  %s = %s & 0x%08Xu;' % (r(rA), r(rD), uimm << 16)); self.cr0(r(rA)); return
        if op == 11:
            crf = rD >> 2
            e('  CMPS(c, %d, (s32)%s, %d);' % (crf, r(rA), simm)); return
        if op == 10:
            crf = rD >> 2
            e('  CMPU(c, %d, %s, 0x%Xu);' % (crf, r(rA), uimm)); return
        # ---------------- rotates ----------------
        if op in (20, 21, 23):
            sh = rB; mb = (w >> 6) & 31; me = (w >> 1) & 31; m = mask(mb, me)
            rot = ('ROTL(%s, %d)' % (r(rD), sh)) if op != 23 else ('ROTL(%s, %s & 31)' % (r(rD), r(rB)))
            if op == 20:
                e('  %s = (%s & 0x%08Xu) | (%s & 0x%08Xu);' % (r(rA), rot, m, r(rA), (~m) & 0xFFFFFFFF))
            else:
                e('  %s = %s & 0x%08Xu;' % (r(rA), rot, m))
            if rc: self.cr0(r(rA))
            return
        # ---------------- loads/stores D-form ----------------
        ld = {32: ('R32', 'u32'), 34: ('R8', 'u8'), 40: ('R16', 'u16'), 42: ('R16S', 's16')}
        ldu = {33: 32, 35: 34, 41: 40, 43: 42}
        st = {36: 'W32', 38: 'W8', 44: 'W16'}
        stu = {37: 36, 39: 38, 45: 44}
        if op in ld:
            fn, _ = ld[op]
            e('  %s = (u32)%s(%s);' % (r(rD), fn, self.ea_d(rA, simm))); return
        if op in ldu:
            fn, _ = ld[ldu[op]]
            e('  { u32 ea = %s + %d; %s = (u32)%s(ea); %s = ea; }' % (r(rA), simm, r(rD), fn, r(rA)) if rD != rA else
              '  { u32 ea = %s + %d; %s = (u32)%s(ea); }' % (r(rA), simm, r(rD), fn)); return
        if op in st:
            e('  %s(%s, %s);' % (st[op], self.ea_d(rA, simm), r(rD))); return
        if op in stu:
            e('  { u32 ea = %s + %d; %s(ea, %s); %s = ea; }' % (r(rA), simm, st[stu[op]], r(rD), r(rA))); return
        if op == 46:
            e('  { u32 ea = %s; for (int i = %d; i < 32; i++, ea += 4) c->r[i] = R32(ea); }' % (self.ea_d(rA, simm), rD)); return
        if op == 47:
            e('  { u32 ea = %s; for (int i = %d; i < 32; i++, ea += 4) W32(ea, c->r[i]); }' % (self.ea_d(rA, simm), rD)); return
        # floating loads/stores
        if op in (48, 49):
            ea = self.ea_d(rA, simm) if op == 48 else '%s + %d' % (r(rA), simm)
            e('  { u32 ea = %s; double v = (double)RF32(ea); c->f[%d][0] = v; c->f[%d][1] = v;%s }' % (ea, rD, rD, (' %s = ea;' % r(rA)) if op == 49 else '')); return
        if op in (50, 51):
            ea = self.ea_d(rA, simm) if op == 50 else '%s + %d' % (r(rA), simm)
            e('  { u32 ea = %s; c->f[%d][0] = RF64(ea);%s }' % (ea, rD, (' %s = ea;' % r(rA)) if op == 51 else '')); return
        if op in (52, 53):
            ea = self.ea_d(rA, simm) if op == 52 else '%s + %d' % (r(rA), simm)
            e('  { u32 ea = %s; WF32(ea, (float)c->f[%d][0]);%s }' % (ea, rD, (' %s = ea;' % r(rA)) if op == 53 else '')); return
        if op in (54, 55):
            ea = self.ea_d(rA, simm) if op == 54 else '%s + %d' % (r(rA), simm)
            e('  { u32 ea = %s; WF64(ea, c->f[%d][0]);%s }' % (ea, rD, (' %s = ea;' % r(rA)) if op == 55 else '')); return
        # paired single quantized load/store
        if op in (56, 57, 60, 61):
            d12 = sext(w & 0xFFF, 12); W = (w >> 15) & 1; I = (w >> 12) & 7
            upd = op in (57, 61)
            ea = ('%s + %d' % (r(rA), d12)) if upd else self.ea_d(rA, d12)
            if op in (56, 57):
                e('  { u32 ea = %s; PSQ_L(c, %d, ea, %d, %d);%s }' % (ea, rD, W, I, (' %s = ea;' % r(rA)) if upd else ''))
            else:
                e('  { u32 ea = %s; PSQ_ST(c, %d, ea, %d, %d);%s }' % (ea, rD, W, I, (' %s = ea;' % r(rA)) if upd else ''))
            return
        if op == 31: return self.op31(pc, w)
        if op == 59: return self.op59(pc, w)
        if op == 63: return self.op63(pc, w)
        if op == 4: return self.op4(pc, w)
        raise NotImplementedError('op%d' % op)

    def op31(self, pc, w):
        e = self.emit; r = self.r
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31; rc = w & 1
        xo = (w >> 1) & 0x3FF; xo9 = xo & 0x1FF; oe = (w >> 10) & 1
        S = rD
        # XO-form arithmetic (rD = f(rA, rB)), OE ignored except we treat it like plain
        arith = {
            266: '%s + %s', 40: '%s - %s',  # subf: rB - rA (handled below)
            235: '(u32)((s32)%s * (s32)%s)',
            75: '(u32)(((s64)(s32)%s * (s64)(s32)%s) >> 32)',
            11: '(u32)(((u64)%s * (u64)%s) >> 32)',
        }
        if xo9 == 266: e('  %s = %s + %s;' % (r(rD), r(rA), r(rB)))
        elif xo9 == 40: e('  %s = %s - %s;' % (r(rD), r(rB), r(rA)))
        elif xo9 == 235: e('  %s = (u32)((s32)%s * (s32)%s);' % (r(rD), r(rA), r(rB)))
        elif xo9 == 75: e('  %s = (u32)(((s64)(s32)%s * (s64)(s32)%s) >> 32);' % (r(rD), r(rA), r(rB)))
        elif xo9 == 11: e('  %s = (u32)(((u64)%s * (u64)%s) >> 32);' % (r(rD), r(rA), r(rB)))
        elif xo9 == 491: e('  %s = DIVW(%s, %s);' % (r(rD), r(rA), r(rB)))
        elif xo9 == 459: e('  %s = DIVWU(%s, %s);' % (r(rD), r(rA), r(rB)))
        elif xo9 == 104: e('  %s = (u32)(-(s32)%s);' % (r(rD), r(rA)))
        elif xo9 == 10: e('  { u32 a = %s, b = %s, v = a + b; SETCA(c, v < a); %s = v; }' % (r(rA), r(rB), r(rD)))
        elif xo9 == 8: e('  { u32 a = %s, b = %s; SETCA(c, a <= b); %s = b - a; }' % (r(rA), r(rB), r(rD)))
        elif xo9 == 138: e('  { u64 v = (u64)%s + %s + GETCA(c); SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rB), r(rD)))
        elif xo9 == 136: e('  { u64 v = (u64)(~%s) + %s + GETCA(c); SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rB), r(rD)))
        elif xo9 == 202: e('  { u64 v = (u64)%s + GETCA(c); SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rD)))
        elif xo9 == 200: e('  { u64 v = (u64)(~%s) + GETCA(c); SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rD)))
        elif xo9 == 234: e('  { u64 v = (u64)%s + GETCA(c) + 0xFFFFFFFFull; SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rD)))
        elif xo9 == 232: e('  { u64 v = (u64)(~%s) + GETCA(c) + 0xFFFFFFFFull; SETCA(c, (v >> 32) != 0); %s = (u32)v; }' % (r(rA), r(rD)))
        else:
            xo9 = None
        if xo9 is not None:
            if rc: self.cr0(r(rD))
            return
        # X-form logical: rA = f(rS, rB)
        logic = {28: '%s & %s', 444: '%s | %s', 316: '%s ^ %s', 476: '~(%s & %s)', 124: '~(%s | %s)', 284: '~(%s ^ %s)', 60: '%s & ~%s', 412: '%s | ~%s'}
        if xo in logic:
            e('  %s = %s;' % (r(rA), logic[xo] % (r(S), r(rB))))
            if rc: self.cr0(r(rA))
            return
        if xo == 24: e('  { u32 n = %s & 63; %s = n > 31 ? 0 : %s << n; }' % (r(rB), r(rA), r(S)))
        elif xo == 536: e('  { u32 n = %s & 63; %s = n > 31 ? 0 : %s >> n; }' % (r(rB), r(rA), r(S)))
        elif xo == 792: e('  { u32 n = %s & 63; s32 s = (s32)%s; if (n > 31) { %s = (u32)(s >> 31); SETCA(c, s < 0); } else { %s = (u32)(s >> n); SETCA(c, s < 0 && (s & ((1u << n) - 1)) != 0); } }' % (r(rB), r(S), r(rA), r(rA)))
        elif xo == 824:
            sh = rB
            e('  { s32 s = (s32)%s; %s = (u32)(s >> %d); SETCA(c, s < 0 && (s & 0x%Xu) != 0); }' % (r(S), r(rA), sh, (1 << sh) - 1))
        elif xo == 26: e('  %s = %s ? (u32)__builtin_clz(%s) : 32;' % (r(rA), r(S), r(S)))
        elif xo == 922: e('  %s = (u32)(s32)(s16)%s;' % (r(rA), r(S)))
        elif xo == 954: e('  %s = (u32)(s32)(s8)%s;' % (r(rA), r(S)))
        else:
            xo_done = False
            if xo == 0: e('  CMPS(c, %d, (s32)%s, (s32)%s);' % (rD >> 2, r(rA), r(rB))); return
            if xo == 32: e('  CMPU(c, %d, %s, %s);' % (rD >> 2, r(rA), r(rB))); return
            ea = self.ea_x(rA, rB)
            lx = {23: 'R32', 87: 'R8', 279: 'R16', 343: 'R16S', 534: 'R32BR', 790: 'R16BR', 20: 'R32'}
            lux = {55: 'R32', 119: 'R8', 311: 'R16', 375: 'R16S'}
            sx = {151: 'W32', 215: 'W8', 407: 'W16', 662: 'W32BR', 918: 'W16BR', 150: 'W32'}
            sux = {183: 'W32', 247: 'W8', 439: 'W16'}
            if xo in lx: e('  %s = (u32)%s(%s);' % (r(rD), lx[xo], ea));
            elif xo in lux: e('  { u32 ea = %s + %s; %s = (u32)%s(ea); %s = ea; }' % (r(rA), r(rB), r(rD), lux[xo], r(rA)))
            elif xo in sx:
                e('  %s(%s, %s);' % (sx[xo], ea, r(S)))
                if xo == 150: e('  SETCRF(c, 0, 2);')  # stwcx. always succeeds
            elif xo in sux: e('  { u32 ea = %s + %s; %s(ea, %s); %s = ea; }' % (r(rA), r(rB), sux[xo], r(S), r(rA)))
            elif xo == 535: e('  { double v = (double)RF32(%s); c->f[%d][0] = v; c->f[%d][1] = v; }' % (ea, rD, rD))
            elif xo == 567: e('  { u32 ea = %s + %s; double v = (double)RF32(ea); c->f[%d][0] = v; c->f[%d][1] = v; %s = ea; }' % (r(rA), r(rB), rD, rD, r(rA)))
            elif xo == 599: e('  c->f[%d][0] = RF64(%s);' % (rD, ea))
            elif xo == 631: e('  { u32 ea = %s + %s; c->f[%d][0] = RF64(ea); %s = ea; }' % (r(rA), r(rB), rD, r(rA)))
            elif xo == 663: e('  WF32(%s, (float)c->f[%d][0]);' % (ea, S))
            elif xo == 695: e('  { u32 ea = %s + %s; WF32(ea, (float)c->f[%d][0]); %s = ea; }' % (r(rA), r(rB), S, r(rA)))
            elif xo == 727: e('  WF64(%s, c->f[%d][0]);' % (ea, S))
            elif xo == 759: e('  { u32 ea = %s + %s; WF64(ea, c->f[%d][0]); %s = ea; }' % (r(rA), r(rB), S, r(rA)))
            elif xo == 983: e('  { double v = c->f[%d][0]; u64 b; memcpy(&b, &v, 8); W32(%s, (u32)b); }' % (S, ea))
            elif xo == 339 or xo == 371:
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                e('  %s = MFSPR(c, %d);' % (r(rD), spr))
            elif xo == 467:
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                e('  MTSPR(c, %d, %s);' % (spr, r(S)))
            elif xo == 19: e('  %s = c->cr;' % r(rD))
            elif xo == 144:
                crm = (w >> 12) & 0xFF; m = 0
                for i in range(8):
                    if crm & (0x80 >> i): m |= 0xF << (28 - 4 * i)
                e('  c->cr = (c->cr & 0x%08Xu) | (%s & 0x%08Xu);' % ((~m) & 0xFFFFFFFF, r(S), m))
            elif xo == 83: e('  %s = c->msr;' % r(rD))
            elif xo == 146: e('  MTMSR(c, %s);' % r(S))
            elif xo == 595: e('  %s = c->sr[%d];' % (r(rD), rA & 15))
            elif xo == 210: e('  c->sr[%d] = %s;' % (rA & 15, r(S)))
            elif xo == 512: e('  SETCRF(c, %d, (c->xer >> 28) & 0xF); c->xer &= 0x0FFFFFFFu;' % (rD >> 2))
            elif xo == 1014: e('  DCBZ(%s);' % ea)
            elif xo in (54, 86, 470, 278, 246, 982, 598, 854, 566, 306):  # cache/sync no-ops
                pass
            elif xo == 4: e('  TRAP(c, 0x%08X);' % pc)
            else:
                raise NotImplementedError('op31_%d' % xo)
            return
        if rc: self.cr0(r(rA))

    def fres(self, expr, single):
        return ('(double)(float)(%s)' % expr) if single else expr

    def fset(self, d, expr, single):
        if single:
            self.emit('  { double v_ = (double)(float)(%s); c->f[%d][0] = v_; c->f[%d][1] = v_; }' % (expr, d, d))
        else:
            self.emit('  c->f[%d][0] = %s;' % (d, expr))

    def op59(self, pc, w):
        fD = (w >> 21) & 31; fA = (w >> 16) & 31; fB = (w >> 11) & 31; fC = (w >> 6) & 31; xo = (w >> 1) & 31
        A, B, C = 'c->f[%d][0]' % fA, 'c->f[%d][0]' % fB, 'c->f[%d][0]' % fC
        m = {18: '%s / %s' % (A, B), 20: '%s - %s' % (A, B), 21: '%s + %s' % (A, B), 25: '%s * %s' % (A, C),
             28: 'FMA_S(%s, %s, -(%s))' % (A, C, B), 29: 'FMA_S(%s, %s, %s)' % (A, C, B),
             30: '-FMA_S(%s, %s, -(%s))' % (A, C, B), 31: '-FMA_S(%s, %s, %s)' % (A, C, B),
             24: '1.0 / %s' % B}
        if xo not in m: raise NotImplementedError('op59_%d' % xo)
        self.fset(fD, m[xo], True)
        if w & 1: self.emit('  SET_CR1(c);')

    def op63(self, pc, w):
        e = self.emit
        fD = (w >> 21) & 31; fA = (w >> 16) & 31; fB = (w >> 11) & 31; fC = (w >> 6) & 31
        xo5 = (w >> 1) & 31; xo = (w >> 1) & 0x3FF
        A, B, C = 'c->f[%d][0]' % fA, 'c->f[%d][0]' % fB, 'c->f[%d][0]' % fC
        a5 = {18: '%s / %s' % (A, B), 20: '%s - %s' % (A, B), 21: '%s + %s' % (A, B), 25: '%s * %s' % (A, C),
              28: 'fma(%s, %s, -(%s))' % (A, C, B), 29: 'fma(%s, %s, %s)' % (A, C, B),
              30: '-fma(%s, %s, -(%s))' % (A, C, B), 31: '-fma(%s, %s, %s)' % (A, C, B),
              26: 'FRSQRTE(%s)' % B, 23: '(%s >= 0.0 ? %s : %s)' % (A, C, B)}
        if xo5 in a5:
            self.fset(fD, a5[xo5], False)
        elif xo in (0, 32):
            e('  FCMP(c, %d, %s, %s);' % (fD >> 2, A, B)); return
        elif xo == 12: e('  c->f[%d][0] = (double)(float)%s;' % (fD, B))
        elif xo == 14: e('  FCTIW(c, %d, %s, 0);' % (fD, B))
        elif xo == 15: e('  FCTIW(c, %d, %s, 1);' % (fD, B))
        elif xo == 40: e('  c->f[%d][0] = -%s;' % (fD, B))
        elif xo == 72: e('  c->f[%d][0] = %s;' % (fD, B))
        elif xo == 264: e('  c->f[%d][0] = fabs(%s);' % (fD, B))
        elif xo == 136: e('  c->f[%d][0] = -fabs(%s);' % (fD, B))
        elif xo == 583: e('  { u64 b = (u64)c->fpscr; double v; memcpy(&v, &b, 8); c->f[%d][0] = v; }' % fD)
        elif xo == 711: e('  { double v = %s; u64 b; memcpy(&b, &v, 8); c->fpscr = (u32)b; }' % B)
        elif xo in (38, 70, 134, 64): pass  # fpscr bit ops: ignore
        else:
            raise NotImplementedError('op63_%d' % xo)
        if w & 1: e('  SET_CR1(c);')

    def op4(self, pc, w):
        e = self.emit
        fD = (w >> 21) & 31; fA = (w >> 16) & 31; fB = (w >> 11) & 31; fC = (w >> 6) & 31
        xo5 = (w >> 1) & 31; xo10 = (w >> 1) & 0x3FF; xo6 = (w >> 1) & 0x3F
        def F(n, i): return 'c->f[%d][%d]' % (n, i)
        def setps(p0, p1):
            e('  { double a0 = (double)(float)(%s), a1 = (double)(float)(%s); c->f[%d][0] = a0; c->f[%d][1] = a1; }' % (p0, p1, fD, fD))
        if xo6 in (6, 7, 38, 39) and xo5 not in (10,11,12,13,14,15,18,20,21,23,24,25,26,28,29,30,31):
            W = (w >> 10) & 1; I = (w >> 7) & 7
            ea = self.ea_x(fA, fB)
            if xo6 == 6: e('  PSQ_L(c, %d, %s, %d, %d);' % (fD, ea, W, I))
            elif xo6 == 7: e('  PSQ_ST(c, %d, %s, %d, %d);' % (fD, ea, W, I))
            elif xo6 == 38: e('  { u32 ea = c->r[%d] + c->r[%d]; PSQ_L(c, %d, ea, %d, %d); c->r[%d] = ea; }' % (fA, fB, fD, W, I, fA))
            else: e('  { u32 ea = c->r[%d] + c->r[%d]; PSQ_ST(c, %d, ea, %d, %d); c->r[%d] = ea; }' % (fA, fB, fD, W, I, fA))
            return
        if xo5 == 10: setps('%s + %s' % (F(fA, 0), F(fB, 1)), F(fC, 1))
        elif xo5 == 11: setps(F(fC, 0), '%s + %s' % (F(fA, 0), F(fB, 1)))
        elif xo5 == 12: setps('%s * %s' % (F(fA, 0), F(fC, 0)), '%s * %s' % (F(fA, 1), F(fC, 0)))
        elif xo5 == 13: setps('%s * %s' % (F(fA, 0), F(fC, 1)), '%s * %s' % (F(fA, 1), F(fC, 1)))
        elif xo5 == 14: setps('%s * %s + %s' % (F(fA, 0), F(fC, 0), F(fB, 0)), '%s * %s + %s' % (F(fA, 1), F(fC, 0), F(fB, 1)))
        elif xo5 == 15: setps('%s * %s + %s' % (F(fA, 0), F(fC, 1), F(fB, 0)), '%s * %s + %s' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo5 == 18: setps('%s / %s' % (F(fA, 0), F(fB, 0)), '%s / %s' % (F(fA, 1), F(fB, 1)))
        elif xo5 == 20: setps('%s - %s' % (F(fA, 0), F(fB, 0)), '%s - %s' % (F(fA, 1), F(fB, 1)))
        elif xo5 == 21: setps('%s + %s' % (F(fA, 0), F(fB, 0)), '%s + %s' % (F(fA, 1), F(fB, 1)))
        elif xo5 == 23: setps('(%s >= 0.0 ? %s : %s)' % (F(fA, 0), F(fC, 0), F(fB, 0)), '(%s >= 0.0 ? %s : %s)' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo5 == 24: setps('1.0 / %s' % F(fB, 0), '1.0 / %s' % F(fB, 1))
        elif xo5 == 25: setps('%s * %s' % (F(fA, 0), F(fC, 0)), '%s * %s' % (F(fA, 1), F(fC, 1)))
        elif xo5 == 26: setps('FRSQRTE(%s)' % F(fB, 0), 'FRSQRTE(%s)' % F(fB, 1))
        elif xo5 == 28: setps('%s * %s - %s' % (F(fA, 0), F(fC, 0), F(fB, 0)), '%s * %s - %s' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo5 == 29: setps('%s * %s + %s' % (F(fA, 0), F(fC, 0), F(fB, 0)), '%s * %s + %s' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo5 == 30: setps('-(%s * %s - %s)' % (F(fA, 0), F(fC, 0), F(fB, 0)), '-(%s * %s - %s)' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo5 == 31: setps('-(%s * %s + %s)' % (F(fA, 0), F(fC, 0), F(fB, 0)), '-(%s * %s + %s)' % (F(fA, 1), F(fC, 1), F(fB, 1)))
        elif xo10 == 0: e('  FCMP(c, %d, %s, %s);' % (fD >> 2, F(fA, 0), F(fB, 0))); return
        elif xo10 == 32: e('  FCMP(c, %d, %s, %s);' % (fD >> 2, F(fA, 0), F(fB, 0))); return
        elif xo10 == 64: e('  FCMP(c, %d, %s, %s);' % (fD >> 2, F(fA, 1), F(fB, 1))); return
        elif xo10 == 96: e('  FCMP(c, %d, %s, %s);' % (fD >> 2, F(fA, 1), F(fB, 1))); return
        elif xo10 == 40: e('  c->f[%d][0] = -%s; c->f[%d][1] = -%s;' % (fD, F(fB, 0), fD, F(fB, 1)))
        elif xo10 == 72: e('  { double a = %s, b = %s; c->f[%d][0] = a; c->f[%d][1] = b; }' % (F(fB, 0), F(fB, 1), fD, fD))
        elif xo10 == 136: e('  c->f[%d][0] = -fabs(%s); c->f[%d][1] = -fabs(%s);' % (fD, F(fB, 0), fD, F(fB, 1)))
        elif xo10 == 264: e('  c->f[%d][0] = fabs(%s); c->f[%d][1] = fabs(%s);' % (fD, F(fB, 0), fD, F(fB, 1)))
        elif xo10 in (528, 560, 592, 624):
            sa = (xo10 - 528) // 64; sb = ((xo10 - 528) // 32) & 1
            e('  { double a = %s, b = %s; c->f[%d][0] = a; c->f[%d][1] = b; }' % (F(fA, sa), F(fB, sb), fD, fD))
        elif xo10 == 1014: e('  DCBZ(%s);' % self.ea_x(fA, fB))
        else:
            raise NotImplementedError('op4_%d_%d' % (xo5, xo10))
        if w & 1: e('  SET_CR1(c);')


class Recomp:
    def __init__(self, dol, syms):
        self.dol = Dol(dol)
        self.funcs = []  # (addr, size, name)
        for l in open(syms):
            m = re.match(r'(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)', l)
            if m: self.funcs.append((int(m.group(3), 16), int(m.group(4), 16), m.group(1)))
        self.funcs.sort()
        self.fstarts = {a: (a, s, n) for a, s, n in self.funcs}
        self.unimpl = collections.Counter()
        self.entries = {}  # entry addr -> (fstart, fend)
        self.jt_targets = collections.defaultdict(list)
        self.idle_pcs = {0x8023B02C}   # SelectThread: while (RunQueueBits == 0) ;

    def containing(self, a):
        import bisect
        i = bisect.bisect_right(self.funcs, (a, 1 << 40, '')) - 1
        if i >= 0:
            fa, fs, fn = self.funcs[i]
            if fa <= a < fa + fs: return fa, fa + fs
        return None

    def call_expr(self, t):
        if t in self.entries:
            return 'f_%08X(c)' % t
        return 'call_indirect(c, 0x%08X)' % t

    def scan(self):
        # entries = function starts + direct call/branch targets landing mid-function
        for a, s, n in self.funcs: self.entries[a] = (a, a + s)
        for a, s, n in self.funcs:
            for pc in range(a, a + s, 4):
                w = self.dol.word(pc); op = w >> 26
                t = None
                if op == 18:
                    li = sext(w & 0x03FFFFFC, 26); t = (li if (w >> 1) & 1 else pc + li) & 0xFFFFFFFF
                    if not (w & 1) and a <= t < a + s: t = None
                elif op == 16 and (w & 1):
                    bd = sext(w & 0xFFFC, 16); t = (bd if (w >> 1) & 1 else pc + bd) & 0xFFFFFFFF
                elif op == 16:
                    bd = sext(w & 0xFFFC, 16); t = (bd if (w >> 1) & 1 else pc + bd) & 0xFFFFFFFF
                    if a <= t < a + s: t = None
                if t is not None and t not in self.entries:
                    cf = self.containing(t)
                    if cf: self.entries[t] = cf
        # jump table targets: words in data that point into the function interior
        for i, o, b, s in self.dol.secs():
            if i < 7: continue
            for off in range(0, s - 3, 4):
                v = struct.unpack('>I', self.dol.d[o + off:o + off + 4])[0]
                if 0x80003100 <= v < 0x802B00E0 and (v & 3) == 0:
                    cf = self.containing(v)
                    if cf and v != cf[0]: self.jt_targets[cf[0]].append(v)
        for k in self.jt_targets: self.jt_targets[k] = sorted(set(self.jt_targets[k]))

    def run(self, outdir, per_file=400):
        os.makedirs(outdir, exist_ok=True)
        self.scan()
        ents = sorted(self.entries.items())
        with open(os.path.join(outdir, 'funcs.h'), 'w') as h:
            h.write('#pragma once\n#include "rt.h"\nextern int g_rs_n; u32 rs_take(Ctx* c);\n')
            for a, _ in ents: h.write('void f_%08X(Ctx* c);\nvoid r_%08X(Ctx* c);\n' % (a, a))
        names = {a: n for a, s, n in self.funcs}
        nfile = 0
        for k in range(0, len(ents), per_file):
            with open(os.path.join(outdir, 'gen_%03d.c' % nfile), 'w') as f:
                f.write('#include "funcs.h"\n\n')
                for a, (fs, fe) in ents[k:k + per_file]:
                    g = Gen(self, a, fe, fs, fe)
                    lines = g.gen()
                    f.write('/* %s */\nvoid r_%08X(Ctx* c);\nWEAK void f_%08X(Ctx* c) __attribute__((alias("r_%08X")));\nvoid r_%08X(Ctx* c) {\n' % (names.get(a, 'entry in %08X' % fs), a, a, a, a))
                    f.write('\n'.join(lines)); f.write('\n}\n\n')
            nfile += 1
        with open(os.path.join(outdir, 'table.c'), 'w') as t:
            t.write('#include "funcs.h"\nconst FuncEntry g_func_table[] = {\n')
            for a, _ in ents: t.write('  {0x%08X, f_%08X},\n' % (a, a))
            t.write('};\nconst int g_func_count = %d;\n' % len(ents))
            t.write('const unsigned g_fn_bounds[] = {\n')   # function [start, end) pairs, sorted (save-state call-stack checks)
            for fa, fsz, fn in self.funcs: t.write('  0x%08X, 0x%08X,\n' % (fa, fa + fsz))
            t.write('};\nconst int g_fn_nbounds = %d;\n' % len(self.funcs))
        with open(os.path.join(outdir, 'names.txt'), 'w') as nf:
            for a, s, n in self.funcs: nf.write('%08X %s\n' % (a, n))
        print('entries', len(ents), 'files', nfile, 'unimplemented', dict(self.unimpl))

if __name__ == '__main__':
    Recomp(sys.argv[1], sys.argv[2]).run(sys.argv[3])
