/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 LoudSilence-sys and contributors
   Command-processor behaviour follows Dolphin's OpcodeDecoding/CPMemory (GPL-2.0-or-later, https://github.com/dolphin-emu/dolphin).
*/
/* fifo.c - GX command stream parser (write-gather pipe at 0xCC008000 + display lists) */
#include "rt.h"
#include <stdio.h>
#include <string.h>

void rt_log(const char* fmt, ...);
void pe_finish(void);
void pe_token(u16 t);
void gx_draw(int prim, int vat, int count, const u8* data, int vsize);   /* renderer hook */
void gx_bp(u32 v);
void gx_cp(u8 reg, u32 v);
void gx_xf(u32 addr, int n, const u8* data);
void gx_xf_indexed(int which, u32 v);

static u32 cp_vcd_lo, cp_vcd_hi, cp_vat_a[8], cp_vat_b[8], cp_vat_c[8];
u32 g_cp_regs[256];
u64 g_fifo_bytes, g_draws;

static int comp_size(int fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static int col_size(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }
static int idx_size(int t) { return t == 2 ? 1 : t == 3 ? 2 : 0; }

int gx_vertex_size(int vat) {
    u32 lo = cp_vcd_lo, hi = cp_vcd_hi, a = cp_vat_a[vat], b = cp_vat_b[vat], c = cp_vat_c[vat];
    int sz = 0;
    if (lo & 1) sz++;                       /* pos/nrm matrix index */
    for (int i = 0; i < 8; i++) if (lo & (2u << i)) sz++;   /* tex matrix indices */
    int pos = (lo >> 9) & 3, nrm = (lo >> 11) & 3, c0 = (lo >> 13) & 3, c1 = (lo >> 15) & 3;
    if (pos == 1) sz += comp_size((a >> 1) & 7) * ((a & 1) ? 3 : 2); else sz += idx_size(pos);
    if (nrm) {
        int nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        if (nrm == 1) sz += comp_size((a >> 10) & 7) * (nbt ? 9 : 3);
        else sz += idx_size(nrm) * ((nbt && idx3) ? 3 : 1);
    }
    if (c0 == 1) sz += col_size((a >> 14) & 7); else sz += idx_size(c0);
    if (c1 == 1) sz += col_size((a >> 18) & 7); else sz += idx_size(c1);
    int tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
    int telm[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
    for (int i = 0; i < 8; i++) {
        int t = (hi >> (2 * i)) & 3;
        if (t == 1) sz += comp_size(tfmt[i]) * (telm[i] ? 2 : 1); else sz += idx_size(t);
    }
    return sz;
}

static void load_cp(u8 reg, u32 v) {
    g_cp_regs[reg] = v;
    if (reg == 0x50) cp_vcd_lo = v;
    else if (reg == 0x60) cp_vcd_hi = v;
    else if ((reg & 0xF8) == 0x70) cp_vat_a[reg & 7] = v;
    else if ((reg & 0xF8) == 0x80) cp_vat_b[reg & 7] = v;
    else if ((reg & 0xF8) == 0x90) cp_vat_c[reg & 7] = v;
    gx_cp(reg, v);
}
static void load_bp(u32 v) {
    u8 reg = v >> 24;
    if (reg == 0x45) pe_finish();
    else if (reg == 0x48) pe_token((u16)v);
    else if (reg == 0x47) { extern void pe_token_noirq(u16); pe_token_noirq((u16)v); }
    gx_bp(v);
}

static u32 rd32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* parse as much as possible; returns bytes consumed */
static char g_hist[16][96]; static int g_hpos;
#define HIST(...) snprintf(g_hist[g_hpos++ & 15], 96, __VA_ARGS__)
static size_t parse(const u8* p, size_t n, int depth) {
    size_t i = 0;
    while (i < n) {
        u8 op = p[i];
        size_t need;
        if (op == 0x00 || op == 0x48 || op == 0x44) { i++; continue; }
        if (op == 0x08) { need = 6; if (i + need > n) break; HIST("CP %02X = %08X", p[i + 1], rd32(p + i + 2)); load_cp(p[i + 1], rd32(p + i + 2)); i += need; continue; }
        if (op == 0x10) {
            if (i + 5 > n) break; u32 h = rd32(p + i + 1); int cnt = ((h >> 16) & 0xF) + 1;
            need = 5 + 4 * cnt; if (i + need > n) break; HIST("XF %04X x%d", h & 0xFFFF, cnt); gx_xf(h & 0xFFFF, cnt, p + i + 5); i += need; continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) { need = 5; if (i + need > n) break; gx_xf_indexed((op >> 3) & 3, rd32(p + i + 1)); i += need; continue; }
        if (op == 0x40) {
            need = 9; if (i + need > n) break;
            u32 a = rd32(p + i + 1) & 0x01FFFFFF, s = rd32(p + i + 5);
            HIST("CALL_DL %08X %X", a, s); if (depth < 4 && a + s <= RAM_SIZE) parse(g_ram + a, s, depth + 1);
            i += need; continue;
        }
        if (op == 0x61) { need = 5; if (i + need > n) break; HIST("BP %08X", rd32(p + i + 1)); load_bp(rd32(p + i + 1)); i += need; continue; }
        if (op >= 0x80 && op < 0xC0) {
            if (i + 3 > n) break;
            int cnt = p[i + 1] << 8 | p[i + 2]; int vat = op & 7; int vs = gx_vertex_size(vat);
            need = 3 + (size_t)cnt * vs; if (i + need > n) break;
            g_draws++; HIST("DRAW %02X n=%d vs=%d vcd %08X/%08X vat %08X %08X %08X", op, cnt, vs, cp_vcd_lo, cp_vcd_hi, cp_vat_a[vat], cp_vat_b[vat], cp_vat_c[vat]);
            gx_draw((op >> 3) & 7, vat, cnt, p + i + 3, vs);
            i += need; continue;
        }
        { static int warned; if (warned++ < 3) { rt_log("fifo: unknown opcode %02X at depth %d; history:\n", op, depth); for (int k = 0; k < 16; k++) rt_log("   %s\n", g_hist[(g_hpos + k) & 15]); } }
        i++;
    }
    return i;
}

static u8 g_buf[1 << 22]; static size_t g_len;
void fifo_debug(void) { fprintf(stderr, "fifo pending %zu bytes:", g_len); for (size_t i = 0; i < g_len && i < 48; i++) fprintf(stderr, " %02X", g_buf[i]); fprintf(stderr, "\n"); if (g_len >= 3 && g_buf[0] >= 0x80) fprintf(stderr, "  draw op %02X count %d vsize %d\n", g_buf[0], g_buf[1] << 8 | g_buf[2], gx_vertex_size(g_buf[0] & 7)); }
/* The write-gather pipe feeds the CPU FIFO described by the PI registers. Normally that is the same memory as the
   GP FIFO (CP registers) and the GPU consumes it, so the commands are parsed straight away. While a display list is
   being recorded (GXBeginDisplayList), the SDK points the CPU FIFO at the list's buffer instead: then the bytes
   only land in RAM, to be executed when the game calls the list (opcode 0x40) - this is how the game defers its
   translucent passes to the end of the frame. */
u64 g_dl_recorded;
void fifo_write(u32 v, int size) {
    { u32 hw_reg32(u32); void hw_reg32_set(u32, u32); u16 hw_reg16(u32);
      u32 pib = hw_reg32(0x300C) & 0x03FFFFE0u, cpb = ((u32)hw_reg16(0x20) | (u32)hw_reg16(0x22) << 16) & 0x03FFFFE0u;
      if (pib && pib != cpb) {
          u32 wr = hw_reg32(0x3014), a = wr & 0x03FFFFFFu;
          for (int k = size - 1; k >= 0; k--) { if ((a & 0x01FFFFFF) < RAM_SIZE) g_ram[a & 0x01FFFFFF] = (u8)(v >> (8 * k)); a++; }
          hw_reg32_set(0x3014, (wr & ~0x03FFFFFFu) | (a & 0x03FFFFFFu));
          g_dl_recorded += size;
          return;
      } }
    if (g_len + 4 > sizeof g_buf) g_len = 0;
    for (int k = size - 1; k >= 0; k--) g_buf[g_len++] = (u8)(v >> (8 * k));
    g_fifo_bytes += size;
    if (g_len >= 32 || size == 4) {
        size_t used = parse(g_buf, g_len, 0);
        memmove(g_buf, g_buf + used, g_len - used); g_len -= used;
    }
}

/* default renderer hooks (no output yet) */






#include "pc.h"
void fifo_state(SS* s) {
    SSV(s, cp_vcd_lo); SSV(s, cp_vcd_hi); SSV(s, cp_vat_a); SSV(s, cp_vat_b); SSV(s, cp_vat_c); SSV(s, g_cp_regs);
    SSV(s, g_len); if (g_len > sizeof g_buf) { g_len = 0; s->err = 1; } ss_io(s, g_buf, g_len);
}
void fifo_import(const u32* cp) { for (int r = 0x30; r < 0xC0; r++) load_cp((u8)r, cp[r]); g_len = 0; }
