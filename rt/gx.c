/* gx.c - software implementation of the GameCube GPU (Flipper) fed by fifo.c
   Scope (v1): vertex decode (direct + indexed), XF transform/projection/viewport, lighting (diffuse,
   attenuation), texgen (regular + post), textures (all main formats + TLUT), TEV (full combiner incl.
   konst/swap/compare), alpha test, z-buffer, blending, culling, near clipping, EFB copies to XFB/texture. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void rt_log(const char* fmt, ...);
int gx_vertex_size(int vat);
extern u32 g_cp_regs[256];

#define EFB_W 640
#define EFB_H 528
static u32 efb_c[EFB_W * EFB_H];   /* RGBA8 (r in low byte) */
static u32 efb_z[EFB_W * EFB_H];   /* 24-bit z */
static u32 bp[256];
static u32 xf_mem[0x1000];         /* 0x0000-0x0FFF: matrices, normals, post, lights */
static u32 xf_reg[0x100];          /* 0x1000-0x10FF */
static u8 tmem[1 << 20];
u64 host_tb(void);
u32 g_tex_epoch = 1;   /* see bind_tex: advances whenever texture memory may have changed */
u64 g_tris; int g_exp = -1; u32 g_rej[3];   /* SOA_EXP debug knobs: 1 no culling, 2 no depth test */

static float xff(u32 a) { u32 v = a < 0x1000 ? xf_mem[a] : xf_reg[a & 0xFF]; float f; memcpy(&f, &v, 4); return f; }

/* ---------------- state hooks ---------------- */
void gx_cp(u8 reg, u32 v) { (void)reg; (void)v; }
void gx_xf(u32 addr, int n, const u8* d) {
    for (int i = 0; i < n; i++) {
        u32 v = (u32)d[4 * i] << 24 | (u32)d[4 * i + 1] << 16 | (u32)d[4 * i + 2] << 8 | d[4 * i + 3];
        u32 a = addr + i;
        if (a < 0x1000) xf_mem[a] = v; else if (a < 0x1100) xf_reg[a & 0xFF] = v;
    }
}
void gx_xf_indexed(int which, u32 v) {
    /* which: 0 pos (A), 1 nrm (B), 2 tex (C), 3 light (D) */
    u32 idx = v >> 16, xfaddr = v & 0xFFF, n = ((v >> 12) & 0xF) + 1;
    u32 base = g_cp_regs[0xAC + which], stride = g_cp_regs[0xB0 + 0xC + which];
    u32 src = (base + idx * stride) & 0x01FFFFFF;
    for (u32 i = 0; i < n; i++) { u32 w = R32(0x80000000u | (src + 4 * i)); if (xfaddr + i < 0x1000) xf_mem[xfaddr + i] = w; }
}

static void efb_copy(u32 v);
static void load_tlut(void);
void gx_bp(u32 v) {
    u8 r = v >> 24; u32 val = v & 0xFFFFFF;
    if (r == 0xFE) { bp[0xFE] = val; return; }                        /* BP mask for next write */
    if (bp[0xFE]) { u32 m = bp[0xFE]; val = (bp[r] & ~m) | (val & m); bp[0xFE] = 0; }
    bp[r] = val;
    if (r >= 0xE0 && r <= 0xE7) { extern u32 g_tevcol[2][8]; g_tevcol[(val >> 23) & 1][r - 0xE0] = val; }   /* TEV color registers and konst colors share these addresses */
    if (r == 0x52) { efb_copy(val); g_tex_epoch++; }
    if (r == 0x65) { load_tlut(); g_tex_epoch++; }
    if (r == 0x63) g_tex_epoch++;
    if (r == 0x63 && val) {   /* TMEM preload (GXPreLoadEntireTexture), as Dolphin */
        u32 src = (bp[0x60] & 0xFFFFFF) << 5, ev = (bp[0x61] & 0xFFFFFF) << 5, od = (bp[0x62] & 0xFFFFFF) << 5, cnt = val & 0x7FFF, type = (val >> 15) & 3;
        if (type != 3) { for (u32 i = 0; i < cnt * 32 && ev + i < sizeof tmem; i++) tmem[ev + i] = g_ram[(src + i) & 0x01FFFFFF]; }
        else for (u32 l = 0; l < cnt && ev + 32 <= sizeof tmem && od + 32 <= sizeof tmem; l++, ev += 32, od += 32) {
            for (int i = 0; i < 32; i++) { tmem[ev + i] = g_ram[(src + l * 64 + i) & 0x01FFFFFF]; tmem[od + i] = g_ram[(src + l * 64 + 32 + i) & 0x01FFFFFF]; } }
    }
}

/* ---------------- vertex decoding ---------------- */
typedef struct { float pos[3], nrm[3]; float col[2][4]; float tex[8][2]; int pmi, tmi[8]; int has_nrm; } InV;
typedef struct { float x, y, z, w; float col[2][4]; float tex[8][3]; } OutV;

static float rdcomp(const u8* p, int fmt, int shift) {
    float s = 1.0f / (float)(1 << shift);
    switch (fmt) {
    case 0: return p[0] * s;
    case 1: return (s8)p[0] * s;
    case 2: return (u16)(p[0] << 8 | p[1]) * s;
    case 3: return (s16)(p[0] << 8 | p[1]) * s;
    default: { u32 u = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; float f; memcpy(&f, &u, 4); return f; }
    }
}
static int compsz(int fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static void rdcolor(const u8* p, int fmt, float* c) {
    u32 r, g, b, a = 255;
    switch (fmt) {
    case 0: { u16 w = p[0] << 8 | p[1]; r = (w >> 11) * 255 / 31; g = ((w >> 5) & 63) * 255 / 63; b = (w & 31) * 255 / 31; break; }
    case 1: case 2: r = p[0]; g = p[1]; b = p[2]; break;
    case 3: { u16 w = p[0] << 8 | p[1]; r = (w >> 12) * 17; g = ((w >> 8) & 15) * 17; b = ((w >> 4) & 15) * 17; a = (w & 15) * 17; break; }
    case 4: { u32 w = (u32)p[0] << 16 | p[1] << 8 | p[2]; r = (w >> 18) * 255 / 63; g = ((w >> 12) & 63) * 255 / 63; b = ((w >> 6) & 63) * 255 / 63; a = (w & 63) * 255 / 63; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    c[0] = r / 255.f; c[1] = g / 255.f; c[2] = b / 255.f; c[3] = a / 255.f;
}
static const u8* arr_ptr(int arr, u32 idx) {
    u32 base = g_cp_regs[0xA0 + arr], stride = g_cp_regs[0xB0 + arr] & 0xFF;
    u32 a = (base + idx * stride) & 0x01FFFFFF;
    return g_ram + (a < RAM_SIZE - 64 ? a : 0);
}
static int col_sz(int f) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[f & 7]; }

static const u8* decode_vertex(const u8* p, int vat, InV* v) {
    u32 lo = g_cp_regs[0x50], hi = g_cp_regs[0x60], A = g_cp_regs[0x70 + vat], B = g_cp_regs[0x80 + vat], C = g_cp_regs[0x90 + vat];
    u32 mia = g_cp_regs[0x30], mib = g_cp_regs[0x40];
    v->pmi = mia & 63;
    for (int i = 0; i < 8; i++) v->tmi[i] = i < 4 ? (mia >> (6 + 6 * i)) & 63 : (mib >> (6 * (i - 4))) & 63;
    if (lo & 1) v->pmi = *p++ & 63;
    for (int i = 0; i < 8; i++) if (lo & (2u << i)) v->tmi[i] = *p++ & 63;
    /* position */
    int pt = (lo >> 9) & 3, pfmt = (A >> 1) & 7, pn = (A & 1) ? 3 : 2, psh = (A >> 4) & 31;
    const u8* q = 0;
    if (pt == 1) { q = p; p += compsz(pfmt) * pn; }
    else if (pt == 2) { q = arr_ptr(0, *p); p += 1; }
    else if (pt == 3) { q = arr_ptr(0, p[0] << 8 | p[1]); p += 2; }
    v->pos[2] = 0;
    if (q) for (int i = 0; i < pn; i++) v->pos[i] = rdcomp(q + i * compsz(pfmt), pfmt, pfmt == 4 ? 0 : psh);
    /* normal */
    int nt = (lo >> 11) & 3; v->has_nrm = nt != 0;
    if (nt) {
        int nfmt = (A >> 10) & 7, nbt = (A >> 9) & 1, idx3 = (A >> 31) & 1;
        int nsh = nfmt == 1 ? 6 : nfmt == 3 ? 14 : 0; int cnt = nbt ? 3 : 1;
        if (nt == 1) { q = p; p += compsz(nfmt) * 3 * cnt; }
        else {
            int isz = nt == 2 ? 1 : 2; u32 idx = isz == 1 ? p[0] : (p[0] << 8 | p[1]);
            q = arr_ptr(1, idx); p += isz * ((nbt && idx3) ? 3 : 1);
        }
        for (int i = 0; i < 3; i++) v->nrm[i] = rdcomp(q + i * compsz(nfmt), nfmt, nsh);
    }
    /* colors */
    for (int c = 0; c < 2; c++) {
        int ct = (lo >> (13 + 2 * c)) & 3, cf = (A >> (14 + 4 * c)) & 7;
        v->col[c][0] = v->col[c][1] = v->col[c][2] = v->col[c][3] = 1.0f;
        if (ct == 1) { rdcolor(p, cf, v->col[c]); p += col_sz(cf); }
        else if (ct >= 2) { u32 idx = ct == 2 ? p[0] : (p[0] << 8 | p[1]); p += ct == 2 ? 1 : 2; rdcolor(arr_ptr(2 + c, idx), cf, v->col[c]); }
    }
    /* texcoords */
    int tf[8] = {(A >> 22) & 7, (B >> 1) & 7, (B >> 10) & 7, (B >> 19) & 7, (B >> 28) & 7, (C >> 6) & 7, (C >> 15) & 7, (C >> 24) & 7};
    int te[8] = {(A >> 21) & 1, B & 1, (B >> 9) & 1, (B >> 18) & 1, (B >> 27) & 1, (C >> 5) & 1, (C >> 14) & 1, (C >> 23) & 1};
    int ts[8] = {(A >> 25) & 31, (B >> 4) & 31, (B >> 13) & 31, (B >> 22) & 31, C & 31, (C >> 9) & 31, (C >> 18) & 31, (C >> 27) & 31};
    for (int t = 0; t < 8; t++) {
        int tt = (hi >> (2 * t)) & 3; int n = te[t] ? 2 : 1;
        v->tex[t][0] = v->tex[t][1] = 0;
        if (!tt) continue;
        if (tt == 1) { q = p; p += compsz(tf[t]) * n; }
        else { u32 idx = tt == 2 ? p[0] : (p[0] << 8 | p[1]); p += tt == 2 ? 1 : 2; q = arr_ptr(4 + t, idx); }
        for (int i = 0; i < n; i++) v->tex[t][i] = rdcomp(q + i * compsz(tf[t]), tf[t], tf[t] == 4 ? 0 : ts[t]);
    }
    return p;
}

/* ---------------- transform + lighting ---------------- */
static void mtx_mul(u32 base, const float* in, float* out, int has_w) {
    for (int r = 0; r < 3; r++) {
        float* m = (float*)&xf_mem[base + r * 4];
        out[r] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + (has_w ? m[3] : 0);
    }
}
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static void unpack_rgba(u32 c, float* o) { o[0] = (c >> 24) / 255.f; o[1] = ((c >> 16) & 255) / 255.f; o[2] = ((c >> 8) & 255) / 255.f; o[3] = (c & 255) / 255.f; }

static void light_channel(int chan, const InV* in, const float* vpos, const float* vn, float* out) {
    for (int part = 0; part < 2; part++) {     /* 0: color (rgb), 1: alpha */
        u32 ctl = xf_reg[0x0E + chan + 2 * part];
        float mat[4], amb[4];
        if (ctl & 1) memcpy(mat, in->col[chan], sizeof mat); else unpack_rgba(xf_reg[0x0C + chan], mat);
        if (ctl & 2) {
            if (ctl & 0x40) memcpy(amb, in->col[chan], sizeof amb); else unpack_rgba(xf_reg[0x0A + chan], amb);
            float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
            u32 lmask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
            int dfn = (ctl >> 7) & 3, atten = (ctl >> 9) & 1, spot = (ctl >> 10) & 1;
            for (int l = 0; l < 8; l++) if (lmask & (1 << l)) {
                u32 lb = 0x600 + l * 16;
                float lc[4]; unpack_rgba(xf_mem[lb + 3], lc);
                float lp[3] = {xff(lb + 10), xff(lb + 11), xff(lb + 12)}, ld[3] = {xff(lb + 13), xff(lb + 14), xff(lb + 15)};
                float L[3] = {lp[0] - vpos[0], lp[1] - vpos[1], lp[2] - vpos[2]};
                float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], dist = sqrtf(dist2);
                float Ln[3] = {0, 0, 0}; if (dist > 0) { Ln[0] = L[0] / dist; Ln[1] = L[1] / dist; Ln[2] = L[2] / dist; }
                float d = vn[0] * Ln[0] + vn[1] * Ln[1] + vn[2] * Ln[2];
                if (dfn == 0) d = 1; else if (dfn == 2) d = d < 0 ? 0 : d;
                float att = 1;
                if (atten) {
                    float a0 = xff(lb + 4), a1 = xff(lb + 5), a2 = xff(lb + 6), k0 = xff(lb + 7), k1 = xff(lb + 8), k2 = xff(lb + 9);
                    if (spot) {
                        float cs = -(Ln[0] * ld[0] + Ln[1] * ld[1] + Ln[2] * ld[2]); if (cs < 0) cs = 0;
                        float num = a0 + a1 * cs + a2 * cs * cs; if (num < 0) num = 0;
                        float den = k0 + k1 * dist + k2 * dist2; att = den != 0 ? num / den : 0;
                    } else {   /* specular */
                        float h = vn[0] * ld[0] + vn[1] * ld[1] + vn[2] * ld[2]; if (h < 0) h = 0;
                        float num = a0 + a1 * h + a2 * h * h; float den = k0 + k1 * h + k2 * h * h; att = den != 0 ? num / den : 0;
                        if (vn[0] * Ln[0] + vn[1] * Ln[1] + vn[2] * Ln[2] <= 0) att = 0;
                    }
                }
                for (int k = 0; k < 4; k++) acc[k] += d * att * lc[k];
            }
            for (int k = 0; k < 4; k++) mat[k] *= clamp01(acc[k]);
        }
        if (part == 0) { out[0] = mat[0]; out[1] = mat[1]; out[2] = mat[2]; }
        else out[3] = mat[3];
    }
}

static void transform(const InV* in, OutV* o) {
    float vp[3], vn[3] = {0, 0, 1};
    mtx_mul(in->pmi * 4, in->pos, vp, 1);
    if (in->has_nrm) {
        u32 nb = 0x400 + (in->pmi & 31) * 3;
        for (int r = 0; r < 3; r++) { float* m = (float*)&xf_mem[nb + r * 3]; vn[r] = m[0] * in->nrm[0] + m[1] * in->nrm[1] + m[2] * in->nrm[2]; }
        float l = sqrtf(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]); if (l > 0) { vn[0] /= l; vn[1] /= l; vn[2] /= l; }
    }
    float P[6]; for (int i = 0; i < 6; i++) P[i] = xff(0x1020 + i);
    if (xf_reg[0x26] == 0) { o->x = P[0] * vp[0] + P[1] * vp[2]; o->y = P[2] * vp[1] + P[3] * vp[2]; o->z = P[4] * vp[2] + P[5]; o->w = -vp[2]; }
    else { o->x = P[0] * vp[0] + P[1]; o->y = P[2] * vp[1] + P[3]; o->z = P[4] * vp[2] + P[5]; o->w = 1; }
    int ncol = xf_reg[0x09] & 3;
    for (int c = 0; c < 2; c++) {
        if (c < ncol || 1) light_channel(c, in, vp, vn, o->col[c]);
    }
    int ntg = xf_reg[0x3F] & 15; int dual = xf_reg[0x12] & 1;
    for (int t = 0; t < 8; t++) {
        o->tex[t][0] = o->tex[t][1] = 0; o->tex[t][2] = 1;
        if (t >= ntg) continue;
        u32 tg = xf_reg[0x40 + t];
        int proj = (tg >> 1) & 1, form = (tg >> 2) & 1, type = (tg >> 4) & 7, row = (tg >> 7) & 31;
        float src[4] = {0, 0, 1, 1};
        if (row == 0) { src[0] = in->pos[0]; src[1] = in->pos[1]; src[2] = in->pos[2]; }
        else if (row == 1) { src[0] = in->nrm[0]; src[1] = in->nrm[1]; src[2] = in->nrm[2]; }
        else if (row >= 5 && row <= 12) { src[0] = in->tex[row - 5][0]; src[1] = in->tex[row - 5][1]; src[2] = 1; }
        if (form == 0) src[2] = 1;
        float r[3];
        if (type == 0) {
            float* m = (float*)&xf_mem[in->tmi[t] * 4];
            r[0] = m[0] * src[0] + m[1] * src[1] + m[2] * src[2] + m[3];
            r[1] = m[4] * src[0] + m[5] * src[1] + m[6] * src[2] + m[7];
            r[2] = proj ? m[8] * src[0] + m[9] * src[1] + m[10] * src[2] + m[11] : 1;
        } else if (type == 2 || type == 3) { r[0] = o->col[0][0]; r[1] = o->col[0][1]; r[2] = 1; }
        else { r[0] = src[0]; r[1] = src[1]; r[2] = 1; }
        if (dual) {
            u32 pt = xf_reg[0x50 + t]; int pidx = pt & 63;
            if ((pt >> 8) & 1) { float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); if (l > 0) { r[0] /= l; r[1] /= l; r[2] /= l; } }
            float* m = (float*)&xf_mem[0x500 + pidx * 4];
            float q0 = m[0] * r[0] + m[1] * r[1] + m[2] * r[2] + m[3], q1 = m[4] * r[0] + m[5] * r[1] + m[6] * r[2] + m[7], q2 = m[8] * r[0] + m[9] * r[1] + m[10] * r[2] + m[11];
            r[0] = q0; r[1] = q1; r[2] = q2;
        }
        o->tex[t][0] = r[0]; o->tex[t][1] = r[1]; o->tex[t][2] = r[2];
    }
}

/* ---------------- textures ---------------- */
typedef struct { u32 addr, w, h, fmt, wrap_s, wrap_t, tlut_fmt, tlut_off; int tm; u32 even, odd; } TexDesc;
static void load_tlut(void) {
    u32 src = (bp[0x64] & 0xFFFFFF) << 5; u32 v = bp[0x65];
    u32 tmem_off = (v & 0x3FF) << 9; u32 cnt = ((v >> 10) & 0x7FF) * 32;
    { extern int g_dbgf, g_frame_no_pub; if (g_dbgf && g_frame_no_pub == g_dbgf) rt_log("loadtlut src %08X tmem %05X bytes %u first %02X%02X\n", src, tmem_off, cnt, R8(0x80000000u | src), R8(0x80000000u | (src + 1))); }
    for (u32 i = 0; i < cnt && tmem_off + i < sizeof tmem; i++) tmem[tmem_off + i] = R8(0x80000000u | ((src + i) & 0x01FFFFFF));
}
static void get_tex(int map, TexDesc* t) {
    int b = map < 4 ? map : map - 4 + 0x20;
    u32 mode0 = bp[0x80 + b], img0 = bp[0x88 + b], img3 = bp[0x94 + b], tlut = bp[0x98 + b];
    t->w = (img0 & 0x3FF) + 1; t->h = ((img0 >> 10) & 0x3FF) + 1; t->fmt = (img0 >> 20) & 15;
    t->addr = (img3 & 0xFFFFFF) << 5; t->wrap_s = mode0 & 3; t->wrap_t = (mode0 >> 2) & 3;
    t->tlut_off = (tlut & 0x3FF) << 9; t->tlut_fmt = (tlut >> 10) & 3;   /* same addressing as BP 0x65 TLUT loads (as Dolphin) */
    u32 img1 = bp[0x8C + b], img2 = bp[0x90 + b];
    t->tm = (img1 >> 21) & 1; t->even = (img1 & 0x7FFF) << 5; t->odd = (img2 & 0x7FFF) << 5;   /* preloaded (manually managed) TMEM */
}
static u32 rgb5a3(u16 w) {
    u32 r, g, b, a;
    if (w & 0x8000) { r = ((w >> 10) & 31) * 255 / 31; g = ((w >> 5) & 31) * 255 / 31; b = (w & 31) * 255 / 31; a = 255; }
    else { a = ((w >> 12) & 7) * 255 / 7; r = ((w >> 8) & 15) * 17; g = ((w >> 4) & 15) * 17; b = (w & 15) * 17; }
    return r | g << 8 | b << 16 | a << 24;
}
static u32 rgb565(u16 w) { return ((w >> 11) * 255 / 31) | (((w >> 5) & 63) * 255 / 63) << 8 | ((w & 31) * 255 / 31) << 16 | 0xFF000000u; }
static u32 tlut_color(const TexDesc* t, u32 idx) {
    u32 o = t->tlut_off + idx * 2; u16 w = (u16)(tmem[o & 0xFFFFF] << 8 | tmem[(o + 1) & 0xFFFFF]);
    if (t->tlut_fmt == 0) { u32 i = w & 0xFF, a = w >> 8; return i | i << 8 | i << 16 | a << 24; }
    if (t->tlut_fmt == 1) return rgb565(w);
    return rgb5a3(w);
}
static inline const u8* tptr(const TexDesc* t, u32 off) { return t->tm ? &tmem[(t->even + off) & 0xFFFFF] : g_ram + ((t->addr + off) & 0x01FFFFFF); }
static u32 texel(const TexDesc* t, int x, int y) {
    /* block sizes per format */
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0}, bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int f = t->fmt; u32 base = t->addr & 0x01FFFFFF;
    if (f == 14) { /* CMPR: 8x8 blocks of 4 4x4 DXT1 sub-blocks */
        int bx = x / 8, by = y / 8, bpr = (t->w + 7) / 8;
        u32 off = base + (by * bpr + bx) * 32; int sx = (x % 8) / 4, sy = (y % 8) / 4; off += (sy * 2 + sx) * 8;
        const u8* p = tptr(t, off - base);
        u16 c0 = p[0] << 8 | p[1], c1 = p[2] << 8 | p[3]; u32 cols[4]; cols[0] = rgb565(c0); cols[1] = rgb565(c1);
        u32 r0 = cols[0] & 255, g0 = (cols[0] >> 8) & 255, b0 = (cols[0] >> 16) & 255, r1 = cols[1] & 255, g1 = (cols[1] >> 8) & 255, b1 = (cols[1] >> 16) & 255;
        if (c0 > c1) { cols[2] = ((2 * r0 + r1) / 3) | ((2 * g0 + g1) / 3) << 8 | ((2 * b0 + b1) / 3) << 16 | 0xFF000000u; cols[3] = ((r0 + 2 * r1) / 3) | ((g0 + 2 * g1) / 3) << 8 | ((b0 + 2 * b1) / 3) << 16 | 0xFF000000u; }
        else { cols[2] = ((r0 + r1) / 2) | ((g0 + g1) / 2) << 8 | ((b0 + b1) / 2) << 16 | 0xFF000000u; cols[3] = 0; }
        int px = x % 4, py = y % 4; int sel = (p[4 + py] >> (6 - 2 * px)) & 3;
        return cols[sel];
    }
    if (!bw[f]) return 0xFFFF00FF;
    int bpr = (t->w + bw[f] - 1) / bw[f];
    int bx = x / bw[f], by = y / bh[f], px = x % bw[f], py = y % bh[f];
    u32 blk = by * bpr + bx;
    switch (f) {
    case 0: { const u8* p = tptr(t, (blk * 32 + (py * 8 + px) / 2)); u32 i = (px & 1 ? *p & 15 : *p >> 4) * 17; return i | i << 8 | i << 16 | i << 24; }  /* I4 */
    case 1: { u32 i = *tptr(t, blk * 32 + py * 8 + px); return i | i << 8 | i << 16 | i << 24; }                                                       /* I8 */
    case 2: { u8 b = *tptr(t, blk * 32 + py * 8 + px); u32 a = (b >> 4) * 17, i = (b & 15) * 17; return i | i << 8 | i << 16 | a << 24; }               /* IA4 */
    case 3: { const u8* p = tptr(t, (blk * 32 + (py * 4 + px) * 2)); u32 a = p[0], i = p[1]; return i | i << 8 | i << 16 | a << 24; }               /* IA8 */
    case 4: { const u8* p = tptr(t, (blk * 32 + (py * 4 + px) * 2)); return rgb565(p[0] << 8 | p[1]); }
    case 5: { const u8* p = tptr(t, (blk * 32 + (py * 4 + px) * 2)); return rgb5a3(p[0] << 8 | p[1]); }
    case 6: {   /* RGBA8: AR and GB halves; in preloaded TMEM they live in the even/odd banks */
        if (t->tm) { const u8* pa = &tmem[(t->even + blk * 32 + (py * 4 + px) * 2) & 0xFFFFF]; const u8* pg = &tmem[(t->odd + blk * 32 + (py * 4 + px) * 2) & 0xFFFFF]; return pa[1] | pg[0] << 8 | pg[1] << 16 | (u32)pa[0] << 24; }
        const u8* p = tptr(t, (blk * 64 + (py * 4 + px) * 2)); u32 a = p[0], r = p[1]; u32 g = p[32], b = p[33]; return r | g << 8 | b << 16 | a << 24; }
    case 8: { const u8* p = tptr(t, (blk * 32 + (py * 8 + px) / 2)); return tlut_color(t, px & 1 ? *p & 15 : *p >> 4); }   /* C4 */
    case 9: return tlut_color(t, *tptr(t, blk * 32 + py * 8 + px));                                                           /* C8 */
    case 10: { const u8* p = tptr(t, (blk * 32 + (py * 4 + px) * 2)); return tlut_color(t, (p[0] << 8 | p[1]) & 0x3FFF); } /* C14X2 */
    }
    return 0xFFFF00FF;
}
static int wrapc(int v, int n, int mode) {
    if (mode == 0) return v < 0 ? 0 : v >= n ? n - 1 : v;
    if (mode == 2) { int p = 2 * n; v %= p; if (v < 0) v += p; return v < n ? v : p - 1 - v; }
    v %= n; if (v < 0) v += n; return v;
}
static void sample(int map, float s, float t, float* out) {
    TexDesc d; get_tex(map, &d);
    int x = (int)floorf(s * d.w), y = (int)floorf(t * d.h);
    x = wrapc(x, d.w, d.wrap_s); y = wrapc(y, d.h, d.wrap_t);
    u32 c = texel(&d, x, y);
    out[0] = (c & 255) / 255.f; out[1] = ((c >> 8) & 255) / 255.f; out[2] = ((c >> 16) & 255) / 255.f; out[3] = (c >> 24) / 255.f;
}

/* ---------------- TEV ---------------- */
static int s11(u32 v) { v &= 0x7FF; return v & 0x400 ? (int)v - 0x800 : (int)v; }
u32 g_tevcol[2][8];   /* [0] = TEV registers, [1] = konst colors; RA/BG words per register */
static void tev_regs(float reg[4][4], float konst[4][4]) {
    for (int k = 0; k < 2; k++) for (int i = 0; i < 4; i++) {
        u32 ra = g_tevcol[k][2 * i], bg = g_tevcol[k][2 * i + 1];
        float* dst = k ? konst[i] : reg[i];
        if (k) { dst[0] = (ra & 0xFF) / 255.f; dst[3] = ((ra >> 12) & 0xFF) / 255.f; dst[2] = (bg & 0xFF) / 255.f; dst[1] = ((bg >> 12) & 0xFF) / 255.f; }
        else { dst[0] = s11(ra) / 255.f; dst[3] = s11(ra >> 12) / 255.f; dst[2] = s11(bg) / 255.f; dst[1] = s11(bg >> 12) / 255.f; }
    }
}
typedef struct { float reg[4][4], konst[4][4]; int init; } TevState;
static TevState g_tev;

static void tev_konst(int stage, float* kc, float* ka) {
    u32 ks = bp[0xF6 + stage / 2];
    int kcs = stage & 1 ? (ks >> 14) & 31 : (ks >> 4) & 31, kas = stage & 1 ? (ks >> 19) & 31 : (ks >> 9) & 31;
    static const float fr[8] = {1, 7 / 8.f, 3 / 4.f, 5 / 8.f, 1 / 2.f, 3 / 8.f, 1 / 4.f, 1 / 8.f};
    if (kcs < 8) kc[0] = kc[1] = kc[2] = fr[kcs];
    else if (kcs >= 12 && kcs < 16) { float* k = g_tev.konst[kcs - 12]; kc[0] = k[0]; kc[1] = k[1]; kc[2] = k[2]; }
    else if (kcs >= 16) { float v = g_tev.konst[(kcs - 16) & 3][(kcs - 16) >> 2]; kc[0] = kc[1] = kc[2] = v; }
    else kc[0] = kc[1] = kc[2] = 1;
    if (kas < 8) *ka = fr[kas]; else if (kas >= 16) *ka = g_tev.konst[(kas - 16) & 3][(kas - 16) >> 2]; else *ka = 1;
}
static void swap(const float* in, float* out, int table) {
    u32 a = bp[0xF6 + table * 2], b = bp[0xF7 + table * 2];
    int sel[4] = {a & 3, (a >> 2) & 3, b & 3, (b >> 2) & 3};
    float t[4] = {in[0], in[1], in[2], in[3]};
    for (int i = 0; i < 4; i++) out[i] = t[sel[i]];
}
static float cmp_val(const float* v, int mode) {   /* packed comparisons */
    if (mode == 0) return roundf(v[0] * 255);
    if (mode == 1) return roundf(v[1] * 255) * 256 + roundf(v[0] * 255);
    return roundf(v[2] * 255) * 65536 + roundf(v[1] * 255) * 256 + roundf(v[0] * 255);
}

static int run_tev(const float ras[2][4], const float tex[8][3], float* outc) {
    int nstages = ((bp[0x00] >> 10) & 15) + 1;
    float r[4][4]; memcpy(r, g_tev.reg, sizeof r);
    for (int s = 0; s < nstages; s++) {
        u32 ord = bp[0x28 + s / 2]; u32 o = s & 1 ? ord >> 12 : ord;
        int tmap = o & 7, tcoord = (o >> 3) & 7, ten = (o >> 6) & 1, chan = (o >> 7) & 7;
        u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
        float texc[4] = {1, 1, 1, 1}, rasc[4] = {0, 0, 0, 0};
        if (ten) {
            float q = tex[tcoord][2]; if (q == 0) q = 1;
            float t4[4]; sample(tmap, tex[tcoord][0] / q, tex[tcoord][1] / q, t4);
            swap(t4, texc, (ae >> 2) & 3);
        }
        if (chan == 0 || chan == 1) { float t4[4] = {ras[chan][0], ras[chan][1], ras[chan][2], ras[chan][3]}; swap(t4, rasc, ae & 3); }
        else if (chan == 7) { rasc[0] = rasc[1] = rasc[2] = rasc[3] = 0; }
        float kc[3], ka; tev_konst(s, kc, &ka);
        /* color */
        float cin[4][3];
        int csel[4] = {(ce >> 12) & 15, (ce >> 8) & 15, (ce >> 4) & 15, ce & 15};   /* a b c d */
        for (int i = 0; i < 4; i++) {
            float* d = cin[i]; int sel = csel[i];
            switch (sel) {
            case 0: case 2: case 4: case 6: d[0] = r[sel / 2][0]; d[1] = r[sel / 2][1]; d[2] = r[sel / 2][2]; break;
            case 1: case 3: case 5: case 7: d[0] = d[1] = d[2] = r[sel / 2][3]; break;
            case 8: d[0] = texc[0]; d[1] = texc[1]; d[2] = texc[2]; break;
            case 9: d[0] = d[1] = d[2] = texc[3]; break;
            case 10: d[0] = rasc[0]; d[1] = rasc[1]; d[2] = rasc[2]; break;
            case 11: d[0] = d[1] = d[2] = rasc[3]; break;
            case 12: d[0] = d[1] = d[2] = 1; break;
            case 13: d[0] = d[1] = d[2] = 0.5f; break;
            case 14: d[0] = kc[0]; d[1] = kc[1]; d[2] = kc[2]; break;
            default: d[0] = d[1] = d[2] = 0; break;
            }
        }
        float ain[4];
        int asel[4] = {(ae >> 13) & 7, (ae >> 10) & 7, (ae >> 7) & 7, (ae >> 4) & 7};
        for (int i = 0; i < 4; i++) {
            switch (asel[i]) {
            case 0: ain[i] = r[0][3]; break; case 1: ain[i] = r[1][3]; break; case 2: ain[i] = r[2][3]; break; case 3: ain[i] = r[3][3]; break;
            case 4: ain[i] = texc[3]; break; case 5: ain[i] = rasc[3]; break; case 6: ain[i] = ka; break; default: ain[i] = 0; break;
            }
        }
        float res[4];
        int cbias = (ce >> 16) & 3, cop = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdest = (ce >> 22) & 3;
        if (cbias != 3) {
            static const float bias[3] = {0, 0.5f, -0.5f}, scl[4] = {1, 2, 4, 0.5f};
            for (int k = 0; k < 3; k++) {
                float a = clamp01(cin[0][k]), b = clamp01(cin[1][k]), c = clamp01(cin[2][k]);
                float v = a * (1 - c) + b * c; v = cop ? -v : v; v = (cin[3][k] + v + bias[cbias]) * scl[cscale];
                res[k] = cclamp ? clamp01(v) : (v < -4 ? -4 : v > 4 ? 4 : v);
            }
        } else {
            int mode = (cscale << 1) | cop;
            for (int k = 0; k < 3; k++) {
                int pass;
                if (mode < 6) { float va = cmp_val(cin[0], mode >> 1), vb = cmp_val(cin[1], mode >> 1); pass = (mode & 1) ? va == vb : va > vb; }
                else { float va = roundf(cin[0][k] * 255), vb = roundf(cin[1][k] * 255); pass = (mode & 1) ? va == vb : va > vb; }
                res[k] = cin[3][k] + (pass ? cin[2][k] : 0); if (cclamp) res[k] = clamp01(res[k]);
            }
        }
        int abias = (ae >> 16) & 3, aop = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adest = (ae >> 22) & 3;
        if (abias != 3) {
            static const float bias[3] = {0, 0.5f, -0.5f}, scl[4] = {1, 2, 4, 0.5f};
            float a = clamp01(ain[0]), b = clamp01(ain[1]), c = clamp01(ain[2]);
            float v = a * (1 - c) + b * c; v = aop ? -v : v; v = (ain[3] + v + bias[abias]) * scl[ascale];
            res[3] = aclamp ? clamp01(v) : v;
        } else {
            int mode = (ascale << 1) | aop; int pass;
            float va = roundf(clamp01(ain[0]) * 255), vb = roundf(clamp01(ain[1]) * 255);
            pass = (mode & 1) ? va == vb : va > vb;
            res[3] = ain[3] + (pass ? ain[2] : 0); if (aclamp) res[3] = clamp01(res[3]);
        }
        r[cdest][0] = res[0]; r[cdest][1] = res[1]; r[cdest][2] = res[2]; r[adest][3] = res[3];
    }
    for (int k = 0; k < 4; k++) outc[k] = clamp01(r[0][k]);
    /* alpha test */
    u32 ac = bp[0xF3]; int a8 = (int)roundf(outc[3] * 255);
    int ref0 = ac & 255, ref1 = (ac >> 8) & 255, c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
    #define ACMP(c, ref) ((c) == 0 ? 0 : (c) == 1 ? a8 < (ref) : (c) == 2 ? a8 == (ref) : (c) == 3 ? a8 <= (ref) : (c) == 4 ? a8 > (ref) : (c) == 5 ? a8 != (ref) : (c) == 6 ? a8 >= (ref) : 1)
    int p0 = ACMP(c0, ref0), p1 = ACMP(c1, ref1);
    int pass = logic == 0 ? (p0 && p1) : logic == 1 ? (p0 || p1) : logic == 2 ? (p0 != p1) : (p0 == p1);
    return pass;
}

/* ---------------- accessors for the GL backend ---------------- */
int g_use_gl;
u32* gx_bp_regs(void) { return bp; }
void gx_tev_colors(float reg[4][4], float konst[4][4]) { tev_regs(g_tev.reg, g_tev.konst); memcpy(reg, g_tev.reg, sizeof g_tev.reg); memcpy(konst, g_tev.konst, sizeof g_tev.konst); }
static u32 tex_bytes(u32 fmt, u32 w, u32 h) {
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0}, bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    if (!bw[fmt]) return 32;
    u32 bx = (w + bw[fmt] - 1) / bw[fmt], by = (h + bh[fmt] - 1) / bh[fmt];
    return bx * by * (fmt == 6 ? 64 : 32);
}
int gx_tex_info(int map, u32* key, u32* w, u32* h, u32* mode0, u32* nbytes, u32* addr) {
    TexDesc d; get_tex(map, &d);
    int b = map < 4 ? map : map - 4 + 0x20;
    *w = d.w; *h = d.h; *mode0 = bp[0x80 + b]; *addr = d.addr; *nbytes = tex_bytes(d.fmt, d.w, d.h);
    key[0] = d.addr; key[1] = d.w | d.h << 16; key[2] = d.fmt | d.tlut_fmt << 4 | d.tlut_off << 8; key[3] = 0;
    if (d.tm) {   /* preloaded: key and hash come from TMEM */
        key[0] = 0xF0000000u | d.even; u32 n = *nbytes, hsh = 2166136261u;
        static u32 ce, cn, cf, cepoch, ch;
        if (cepoch == g_tex_epoch && ce == d.even && cn == n && cf == d.fmt) hsh = ch;
        else {
            for (u32 i = 0; i < n; i += 4) { hsh ^= tmem[(d.even + i) & 0xFFFFF]; hsh *= 16777619u; }
            if (d.fmt == 6) for (u32 i = 0; i < n / 2; i += 4) { hsh ^= tmem[(d.odd + i) & 0xFFFFF]; hsh *= 16777619u; }
            ce = d.even; cn = n; cf = d.fmt; cepoch = g_tex_epoch; ch = hsh;
        }
        key[3] = hsh; *nbytes = 0;
    }
    if (d.fmt == 8 || d.fmt == 9 || d.fmt == 10) {
        u32 n = d.fmt == 8 ? 32 : 512; u32 hsh = 2166136261u;
        for (u32 i = 0; i < n; i++) { hsh ^= tmem[(d.tlut_off + i) & 0xFFFFF]; hsh *= 16777619u; }
        key[3] = hsh;
    }
    return 1;
}
float gx_viewport_wd(void) { return xff(0x101A); }
/* number of mip levels usable for a texture map (1 = no mipmapping) */
int gx_tex_levels(int map) {
    TexDesc d; get_tex(map, &d);
    int b = map < 4 ? map : map - 4 + 0x20;
    u32 m0 = bp[0x80 + b], m1 = bp[0x84 + b];
    if (!((m0 >> 5) & 3) || d.tm) return 1;
    int maxl = ((m1 >> 8) & 0xFF) + 15; maxl /= 16;
    int n = 1; u32 w = d.w, h = d.h;
    while (n <= maxl && (w > 1 || h > 1)) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n > 11 ? 11 : n;
}
/* decode mip level lvl (levels follow each other in memory, each padded to whole blocks) */
void gx_tex_decode_level(int map, int lvl, u32* out, u32* ow, u32* oh) {
    TexDesc d; get_tex(map, &d);
    for (int l = 0; l < lvl; l++) { d.addr += tex_bytes(d.fmt, d.w, d.h); d.w = d.w > 1 ? d.w / 2 : 1; d.h = d.h > 1 ? d.h / 2 : 1; }
    *ow = d.w; *oh = d.h;
    for (u32 y = 0; y < d.h; y++) for (u32 x = 0; x < d.w; x++) out[y * d.w + x] = texel(&d, x, y);
}
void gx_tex_decode(int map, u32* out) {
    TexDesc d; get_tex(map, &d);
    for (u32 y = 0; y < d.h; y++) for (u32 x = 0; x < d.w; x++) out[y * d.w + x] = texel(&d, x, y);
}
void gx_dump_efb(const char* path, int w, int h);
void gl_vertex(const float* pos4, const float* c0, const float* c1, const float tex[8][3]);
void gl_flush(void);
void gl_read_efb(u32* dst, int x, int y, int w, int h);
void gl_read_efb_depth(u32* dst, int x, int y, int w, int h);
void gl_clear(int x, int y, int w, int h, u32 rgba, u32 z24, int color, int alpha, int depth);

/* ---------------- rasterization ---------------- */
typedef struct { float x, y, z, iw; float col[2][4]; float tex[8][3]; } SV;   /* screen vertex, attributes premultiplied by iw */

static int zcmp(int f, u32 z, u32 cur) {
    switch (f) { case 0: return 0; case 1: return z < cur; case 2: return z == cur; case 3: return z <= cur; case 4: return z > cur; case 5: return z != cur; case 6: return z >= cur; default: return 1; }
}
static float bfac(int f, const float* s, const float* d, int k, int is_src) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 2: return is_src ? d[k] : s[k];
    case 3: return is_src ? 1 - d[k] : 1 - s[k];
    case 4: return s[3]; case 5: return 1 - s[3];
    case 6: return d[3]; case 7: return 1 - d[3];
    }
    return 1;
}

static void raster_tri(const SV* a, const SV* b, const SV* c) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    g_tris++;
    u32 sc0 = bp[0x20], sc1 = bp[0x21], sco = bp[0x59];
    int sy0 = (sc0 & 0x7FF) - 342, sx0 = ((sc0 >> 12) & 0x7FF) - 342, sy1 = (sc1 & 0x7FF) - 342, sx1 = ((sc1 >> 12) & 0x7FF) - 342;
    int ox = ((sco & 0x3FF) << 1) - 342, oy = (((sco >> 10) & 0x3FF) << 1) - 342; (void)ox; (void)oy;
    if (sx1 < sx0 || sy1 < sy0) { sx0 = 0; sy0 = 0; sx1 = EFB_W - 1; sy1 = EFB_H - 1; }
    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x))), maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y))), maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < sx0) minx = sx0; if (miny < sy0) miny = sy0; if (maxx > sx1) maxx = sx1; if (maxy > sy1) maxy = sy1;
    if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx >= EFB_W) maxx = EFB_W - 1; if (maxy >= EFB_H) maxy = EFB_H - 1;
    u32 zm = bp[0x40], bm = bp[0x41];
    int zen = zm & 1, zf = (zm >> 1) & 7, zup = (zm >> 4) & 1;
    int ben = bm & 1, cup = (bm >> 3) & 1, aup = (bm >> 4) & 1, dfac = (bm >> 5) & 7, sfac = (bm >> 8) & 7, bsub = (bm >> 11) & 1;
    int ntex = 8, ncol = 2;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) / area;
        float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float z = w0 * a->z + w1 * b->z + w2 * c->z;
        u32 zi = z < 0 ? 0 : z > 16777215.f ? 16777215 : (u32)z;
        int idx = y * EFB_W + x;
        if (zen && !zcmp(zf, zi, efb_z[idx])) continue;
        float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw; float invw = iw != 0 ? 1 / iw : 0;
        float ras[2][4], tex[8][3];
        for (int ch = 0; ch < ncol; ch++) for (int k = 0; k < 4; k++) ras[ch][k] = clamp01((w0 * a->col[ch][k] + w1 * b->col[ch][k] + w2 * c->col[ch][k]) * invw);
        for (int t = 0; t < ntex; t++) for (int k = 0; k < 3; k++) tex[t][k] = (w0 * a->tex[t][k] + w1 * b->tex[t][k] + w2 * c->tex[t][k]) * invw;
        float out[4];
        if (!run_tev(ras, tex, out)) continue;
        u32 dc = efb_c[idx]; float d[4] = {(dc & 255) / 255.f, ((dc >> 8) & 255) / 255.f, ((dc >> 16) & 255) / 255.f, (dc >> 24) / 255.f};
        float res[4];
        if (ben) {
            for (int k = 0; k < 4; k++) {
                if (bsub) res[k] = clamp01(d[k] - out[k]);
                else res[k] = clamp01(out[k] * bfac(sfac, out, d, k, 1) + d[k] * bfac(dfac, out, d, k, 0));
            }
        } else memcpy(res, out, sizeof res);
        if (!ben && ((bm >> 1) & 1) && ((bm >> 12) & 15) != 3) {
            u32 sc = (u32)(res[0] * 255 + .5f) | (u32)(res[1] * 255 + .5f) << 8 | (u32)(res[2] * 255 + .5f) << 16 | (u32)(res[3] * 255 + .5f) << 24, r;
            switch ((bm >> 12) & 15) { case 0: r = 0; break; case 1: r = sc & dc; break; case 2: r = sc & ~dc; break; case 4: r = ~sc & dc; break; case 5: r = dc; break;
            case 6: r = sc ^ dc; break; case 7: r = sc | dc; break; case 8: r = ~(sc | dc); break; case 9: r = ~(sc ^ dc); break; case 10: r = ~dc; break;
            case 11: r = sc | ~dc; break; case 12: r = ~sc; break; case 13: r = ~sc | dc; break; case 14: r = ~(sc & dc); break; default: r = 0xFFFFFFFFu; }
            for (int k = 0; k < 4; k++) res[k] = ((r >> (8 * k)) & 255) / 255.f;
        }
        u32 nc = dc;
        if (cup) nc = (nc & 0xFF000000u) | (u32)(res[0] * 255 + .5f) | (u32)(res[1] * 255 + .5f) << 8 | (u32)(res[2] * 255 + .5f) << 16;
        if (aup) nc = (nc & 0x00FFFFFFu) | (u32)(res[3] * 255 + .5f) << 24;
        efb_c[idx] = nc;
        if (zen && zup) efb_z[idx] = zi;
    }
}

static void to_screen(const OutV* o, SV* s) {
    float iw = o->w != 0 ? 1 / o->w : 0;
    float sx = xff(0x101A), sy = xff(0x101B), sz = xff(0x101C), ox = xff(0x101D), oy = xff(0x101E), oz = xff(0x101F);
    s->x = o->x * iw * sx + ox - 342; s->y = o->y * iw * sy + oy - 342; s->z = o->z * iw * sz + oz;
    s->iw = iw;
    for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 4; k++) s->col[ch][k] = o->col[ch][k] * iw;
    for (int t = 0; t < 8; t++) for (int k = 0; k < 3; k++) s->tex[t][k] = o->tex[t][k] * iw;
}
static void lerpv(const OutV* a, const OutV* b, float t, OutV* o) {
    const float* pa = (const float*)a; const float* pb = (const float*)b; float* po = (float*)o;
    for (size_t i = 0; i < sizeof(OutV) / sizeof(float); i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}
/* face culling, as Dolphin's software clipper: sign of the clip-space normal, inverted for a positive viewport height.
   GenMode cull 1 drops triangles that are not "backface" by this test, cull 2 drops "backface" ones, 3 drops all. */
static int cull_reject(const OutV* v0, const OutV* v1, const OutV* v2) {
    int cull = (bp[0x00] >> 14) & 3; if (g_exp & 1) cull = 0;
    if (cull == 0) return 0; if (cull == 3) return 1;
    float nz = (v0->x * v2->w - v2->x * v0->w) * v1->y + (v2->x * v0->y - v0->x * v2->y) * v1->w + (v2->y * v0->w - v0->y * v2->w) * v1->x;
    int back = nz <= 0; if (xff(0x101B) > 0) back = !back;
    return cull == 1 ? !back : back;
}
/* clip against near plane (z <= 0 in GX clip space means visible: -w <= z <= 0) */
static void clip_tri(const OutV* a, const OutV* b, const OutV* c) {
    if (g_exp < 0) { const char* e = getenv("SOA_EXP"); g_exp = e ? atoi(e) : 0; }
    if (cull_reject(a, b, c)) { g_rej[2]++; return; }
    OutV in[8], out[8]; int n = 3; in[0] = *a; in[1] = *b; in[2] = *c;
    int m = 0;
    for (int i = 0; i < n; i++) {
        const OutV* p = &in[i]; const OutV* q = &in[(i + 1) % n];
        float dp = p->w + p->z, dq = q->w + q->z;      /* inside if z >= -w */
        if (dp >= 0) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0)) { float t = dp / (dp - dq); lerpv(p, q, t, &out[m++]); }
    }
    if (m < 3) { g_rej[0]++; return; }
    SV s[8]; for (int i = 0; i < m; i++) { if (out[i].w <= 0) { g_rej[1]++; return; } to_screen(&out[i], &s[i]); }
    if (g_use_gl) {

        g_tris++;
        for (int i = 1; i + 1 < m; i++) {
            int idx[3] = {0, i, i + 1};
            for (int k = 0; k < 3; k++) {
                const SV* v = &s[idx[k]]; const OutV* o = &out[idx[k]]; float w = o->w;
                float zn = v->z / 16777215.f; if (zn < 0) zn = 0; if (zn > 1) zn = 1;
                float pos[4] = {(v->x / 320.f - 1.f) * w, (1.f - v->y / 264.f) * w, (zn * 2.f - 1.f) * w, w};
                gl_vertex(pos, o->col[0], o->col[1], o->tex);
            }
        }
        return;
    }
    for (int i = 1; i + 1 < m; i++) raster_tri(&s[0], &s[i], &s[i + 1]);
}

/* lines and points: expanded to screen-space quads (width from SU_LPSIZE, BP 0x22, in 1/6 pixel) */
static void emit_sv(const SV* v, const OutV* o) {
    float w = o->w; float zn = v->z / 16777215.f; if (zn < 0) zn = 0; if (zn > 1) zn = 1;
    float pos[4] = {(v->x / 320.f - 1.f) * w, (1.f - v->y / 264.f) * w, (zn * 2.f - 1.f) * w, w};
    gl_vertex(pos, o->col[0], o->col[1], o->tex);
}
static void quad_sv(SV q[4], const OutV* oa, const OutV* ob, const OutV* oc, const OutV* od) {
    if (g_use_gl) { emit_sv(&q[0], oa); emit_sv(&q[1], ob); emit_sv(&q[2], oc); emit_sv(&q[0], oa); emit_sv(&q[2], oc); emit_sv(&q[3], od); }
    else { raster_tri(&q[0], &q[1], &q[2]); raster_tri(&q[0], &q[2], &q[3]); }
    g_tris += 2;
}
static void draw_line(const OutV* a, const OutV* b) {
    if (a->w <= 0 || b->w <= 0) return;
    SV sa, sb; to_screen(a, &sa); to_screen(b, &sb);
    float hw = (bp[0x22] & 0xFF) / 6.0f * 0.5f; if (hw < 0.5f) hw = 0.5f;
    float dx = sb.x - sa.x, dy = sb.y - sa.y;
    /* GX widens lines along the minor axis */
    float ox = 0, oy = 0; if (fabsf(dx) > fabsf(dy)) oy = hw; else ox = hw;
    SV q[4] = {sa, sa, sb, sb};
    q[0].x -= ox; q[0].y -= oy; q[1].x += ox; q[1].y += oy; q[2].x += ox; q[2].y += oy; q[3].x -= ox; q[3].y -= oy;
    quad_sv(q, a, a, b, b);
}
static void draw_point(const OutV* a) {
    if (a->w <= 0) return;
    SV sa; to_screen(a, &sa);
    float hs = ((bp[0x22] >> 8) & 0xFF) / 6.0f * 0.5f; if (hs < 0.5f) hs = 0.5f;
    SV q[4] = {sa, sa, sa, sa};
    q[0].x -= hs; q[0].y -= hs; q[1].x += hs; q[1].y -= hs; q[2].x += hs; q[2].y += hs; q[3].x -= hs; q[3].y += hs;
    quad_sv(q, a, a, a, a);
}

void gx_draw(int prim, int vat, int count, const u8* data, int vsize) {
    { static int n; extern int g_frame_no_pub; if (g_frame_no_pub > 600 && n < 6) { n++;
        rt_log("draw: vp %f %f %f %f %f %f proj %f %f %f %f %f %f type %u sc %06X %06X off %06X\n", xff(0x101A), xff(0x101B), xff(0x101C), xff(0x101D), xff(0x101E), xff(0x101F),
            xff(0x1020), xff(0x1021), xff(0x1022), xff(0x1023), xff(0x1024), xff(0x1025), xf_reg[0x26], bp[0x20], bp[0x21], bp[0x59]); } }
    static OutV vs[65536];
    if (count > 65536) count = 65536;
    if (!g_tev.init || 1) { tev_regs(g_tev.reg, g_tev.konst); g_tev.init = 1; }
    const u8* p = data;
    for (int i = 0; i < count; i++) { InV in; const u8* q = decode_vertex(p, vat, &in); (void)q; p += vsize; transform(&in, &vs[i]); }
    if (g_exp < 0) { const char* e = getenv("SOA_EXP"); g_exp = e ? atoi(e) : 0; }
    u64 tris0 = g_tris; g_rej[0] = g_rej[1] = g_rej[2] = 0;
    switch (prim) {
    case 0: for (int i = 0; i + 3 < count; i += 4) { clip_tri(&vs[i], &vs[i + 1], &vs[i + 2]); clip_tri(&vs[i], &vs[i + 2], &vs[i + 3]); } break;
    case 2: for (int i = 0; i + 2 < count; i += 3) clip_tri(&vs[i], &vs[i + 1], &vs[i + 2]); break;
    case 3: for (int i = 0; i + 2 < count; i++) { if (i & 1) clip_tri(&vs[i + 1], &vs[i], &vs[i + 2]); else clip_tri(&vs[i], &vs[i + 1], &vs[i + 2]); } break;
    case 4: for (int i = 1; i + 1 < count; i++) clip_tri(&vs[0], &vs[i], &vs[i + 1]); break;
    case 5: for (int i = 0; i + 1 < count; i += 2) draw_line(&vs[i], &vs[i + 1]); break;
    case 6: for (int i = 0; i + 1 < count; i++) draw_line(&vs[i], &vs[i + 1]); break;
    case 7: for (int i = 0; i < count; i++) draw_point(&vs[i]); break;
    default: break;
    }
    if (g_use_gl) gl_flush();
    { extern int g_dbgf; int dbgf = g_dbgf; extern int g_frame_no_pub;
      static u32 dtex[4]; static int dtn = -1; if (dtn < 0) { dtn = 0; const char* e = getenv("SOA_DBGTEX"); if (e) dtn = sscanf(e, "%x,%x,%x,%x", &dtex[0], &dtex[1], &dtex[2], &dtex[3]); }
      int texhit = 0; if (dtn > 0) { static int hits; TexDesc d0; get_tex(bp[0x28] & 7, &d0); for (int k = 0; k < dtn; k++) if (d0.addr == dtex[k] && hits < 40) { texhit = 1; hits++; } }
      if ((dbgf && g_frame_no_pub == dbgf) || texhit) {
        static int nd; nd++;
        if (g_use_gl) gl_read_efb(efb_c, 0, 0, 640, 480);
        double m[3] = {0, 0, 0}; for (int y = 0; y < 480; y += 8) for (int x = 0; x < 640; x += 8) { u32 c = efb_c[y * EFB_W + x]; m[0] += c & 255; m[1] += (c >> 8) & 255; m[2] += (c >> 16) & 255; }
        int ns = ((bp[0] >> 10) & 15) + 1;
        rt_log("D%d tris %d rej %u/%u/%u prim %d n %d vat %d | rgb %3.0f %3.0f %3.0f | gen %06X ns %d blend %06X z %06X ac %06X pix %06X c0 %06X a0 %06X ord %06X %06X ras0 %08X %08X ind %06X fog %06X\n", nd, (int)(g_tris - tris0), g_rej[0], g_rej[1], g_rej[2], prim, count, vat,
            m[0] / 4800, m[1] / 4800, m[2] / 4800, bp[0], ns, bp[0x41], bp[0x40], bp[0xF3], bp[0x43], bp[0xC0], bp[0xC1], bp[0x28], bp[0x29], xf_reg[0x0E], xf_reg[0x0C], bp[0x10], bp[0xF2]);
        { float bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9, amin = 9, amax = -9; const u8* pp = data;
          for (int i = 0; i < count && i < 64; i++) { InV iv; decode_vertex(pp, vat, &iv); pp += vsize; OutV ov; transform(&iv, &ov); SV sv; to_screen(&ov, &sv);
            if (sv.x < bx0) bx0 = sv.x; if (sv.x > bx1) bx1 = sv.x; if (sv.y < by0) by0 = sv.y; if (sv.y > by1) by1 = sv.y; float a = ov.col[0][3]; if (a < amin) amin = a; if (a > amax) amax = a; }
          { InV iv; decode_vertex(data, vat, &iv); OutV ov; transform(&iv, &ov); SV sv; to_screen(&ov, &sv); rt_log("   z0 %.1f w %.4f clipz %.4f vp %f %f proj %u %f %f\n", sv.z, ov.w, ov.z, xff(0x101C), xff(0x101F), xf_reg[0x26], xff(0x1024), xff(0x1025)); }
          { const u8* pq = data; for (int i = 0; i < count && i < 4; i++) { InV iv; decode_vertex(pq, vat, &iv); pq += vsize; OutV ov; transform(&iv, &ov); rt_log("   vtx%d pos %.2f %.2f %.2f -> clip %.3f %.3f %.3f %.3f\n", i, iv.pos[0], iv.pos[1], iv.pos[2], ov.x, ov.y, ov.z, ov.w); } }
          rt_log("   bbox %.0f,%.0f - %.0f,%.0f  alpha %.3f..%.3f  vsize %d\n", bx0, by0, bx1, by1, amin, amax, vsize);
          rt_log("   konst %08X %08X %08X %08X | %08X %08X | ksel %06X %06X | reg0 %08X %08X | zm %06X sc %06X %06X\n", g_tevcol[1][0], g_tevcol[1][1], g_tevcol[1][2], g_tevcol[1][3], g_tevcol[1][6], g_tevcol[1][7], bp[0xF6], bp[0xF7], g_tevcol[0][0], g_tevcol[0][1], bp[0x40], bp[0x20], bp[0x21]); }
        { InV iv; decode_vertex(data, vat, &iv); OutV ov; transform(&iv, &ov); float sc[4] = {0, 0, 0, 0}; float q = ov.tex[0][2] ? ov.tex[0][2] : 1; sample(0, ov.tex[0][0] / q, ov.tex[0][1] / q, sc);
          rt_log("   v0: in tex %.4f %.4f tmi %d -> %.4f %.4f %.4f  sample %.2f %.2f %.2f %.2f texgen %08X vcd hi %08X\n", iv.tex[0][0], iv.tex[0][1], iv.tmi[0], ov.tex[0][0], ov.tex[0][1], ov.tex[0][2], sc[0], sc[1], sc[2], sc[3], xf_reg[0x40], g_cp_regs[0x60]); }
        { InV iv; decode_vertex(data, vat, &iv); rt_log("   xf: ctl c0 %08X a0 %08X c1 %08X a1 %08X mat %08X %08X amb %08X %08X nchan %u | vcd %08X vatA %08X | v0 col %.3f %.3f %.3f %.3f\n", xf_reg[0x0E], xf_reg[0x10], xf_reg[0x0F], xf_reg[0x11], xf_reg[0x0C], xf_reg[0x0D], xf_reg[0x0A], xf_reg[0x0B], xf_reg[0x09], g_cp_regs[0x50], g_cp_regs[0x70 + vat], iv.col[0][0], iv.col[0][1], iv.col[0][2], iv.col[0][3]); }
        for (int s2 = 0; s2 < ns; s2++) { u32 o = bp[0x28 + s2 / 2]; if (s2 & 1) o >>= 12; if (!((o >> 6) & 1)) continue; int mp = o & 7; TexDesc d; get_tex(mp, &d);
            int bb = mp < 4 ? mp : mp - 4 + 0x20; double ta[4] = {0, 0, 0, 0}; int nn = 0;
            for (u32 yy = 0; yy < d.h; yy += (d.h + 7) / 8) for (u32 xx = 0; xx < d.w; xx += (d.w + 7) / 8) { u32 c = texel(&d, xx, yy); for (int k = 0; k < 4; k++) ta[k] += (c >> (8 * k)) & 255; nn++; }
            { char tp[96]; snprintf(tp, sizeof tp, "frames/tex_%08X_f%u_%ux%u.ppm", d.addr, d.fmt, d.w, d.h); FILE* tf = fopen(tp, "rb"); if (!tf && d.w * d.h <= 1024 * 1024) { tf = fopen(tp, "wb"); if (tf) { fprintf(tf, "P6\n%u %u\n255\n", d.w * 2, d.h);
                for (u32 yy = 0; yy < d.h; yy++) { for (u32 xx = 0; xx < d.w; xx++) { u32 c = texel(&d, xx, yy); fputc(c & 255, tf); fputc((c >> 8) & 255, tf); fputc((c >> 16) & 255, tf); } for (u32 xx = 0; xx < d.w; xx++) { u32 c = texel(&d, xx, yy) >> 24; fputc(c, tf); fputc(c, tf); fputc(c, tf); } } } } if (tf) fclose(tf); }
            rt_log("   st%d map %d fmt %u %ux%u addr %08X mode0 %06X mode1 %06X img1 %06X img2 %06X tlut %06X mean %3.0f %3.0f %3.0f a%3.0f ce %06X ae %06X\n", s2, mp, d.fmt, d.w, d.h, d.addr, bp[0x80 + bb], bp[0x84 + bb], bp[0x8C + bb], bp[0x90 + bb], bp[0x98 + bb],
                   ta[0] / nn, ta[1] / nn, ta[2] / nn, ta[3] / nn, bp[0xC0 + 2 * s2], bp[0xC1 + 2 * s2]); }
        static int r0 = -1, r1; if (r0 < 0) { const char* e = getenv("SOA_DBGRANGE"); r0 = 0; r1 = -1; if (e) sscanf(e, "%d,%d", &r0, &r1); }
        if (nd % 10 == 0 || nd < 40 || (nd >= r0 && nd <= r1)) { char p[64]; snprintf(p, sizeof p, "frames/dbg_%04d.ppm", nd); gx_dump_efb(p, 640, 480); }
      } }
}

/* ---------------- EFB copies ---------------- */
int g_xfb_ready; u32 g_xfb_addr;
static void efb_copy(u32 v) {
    { extern int g_frame_no_pub; extern int g_dbgf; static int n; if ((g_dbgf && g_frame_no_pub == g_dbgf) || (!((v >> 14) & 1) && g_frame_no_pub > 1300 && g_frame_no_pub < 1320 && n++ < 60)) rt_log("texcopy %06X fmt %d src %06X wh %06X dst %08X stride %X pixfmt %X\n", v, ((v >> 4) & 7) | (((v >> 3) & 1) << 3), bp[0x49], bp[0x4A], (bp[0x4B] & 0xFFFFFF) << 5, bp[0x4D], bp[0x43]); }
    u32 tl = bp[0x49], wh = bp[0x4A];
    int sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    u32 dst = (bp[0x4B] & 0xFFFFFF) << 5; u32 stride = (bp[0x4D] & 0x3FF) << 5;
    int to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1, half = (v >> 9) & 1;
    extern int g_dump_every; extern int g_dbgf;
    extern int g_frame_no_pub;
    int need_xfb_pixels = (g_dump_every && (g_frame_no_pub + 1) % g_dump_every == 0) || g_dbgf || !g_use_gl;   /* GL presents the EFB directly; the XFB in RAM is only for dumps */
    int tpf0 = (v >> 3) & 15, fmt0 = tpf0 / 2 + (tpf0 & 1) * 8;
    int gpu_copy = 0;
    if (g_use_gl && !to_xfb && (bp[0x43] & 7) != 3 && !g_dbgf) {   /* texture copy kept on the GPU (not depth) */
        extern int gl_efb_copy_tex(int sx, int sy, int w, int h, int half, int fmt, int inten, int noalpha, u32 dst, u32 nbytes);
        int tw = half ? w / 2 : w, th = half ? h / 2 : h; int bpp = (fmt0 == 0) ? 4 : (fmt0 == 2 || fmt0 == 7 || fmt0 == 8 || fmt0 == 1 || fmt0 == 9 || fmt0 == 10) ? 8 : (fmt0 == 6) ? 32 : 16;
        int bw = bpp == 4 ? 8 : bpp == 8 ? 8 : 4, bh = bpp == 4 ? 8 : 4; u32 nb = (u32)((tw + bw - 1) / bw) * ((th + bh - 1) / bh) * (bpp == 32 ? 64 : 32);
        gpu_copy = gl_efb_copy_tex(sx, sy, w, h, half, fmt0, (v >> 15) & 1, (bp[0x43] & 7) != 1, dst, nb);
    }
    if (g_use_gl && !gpu_copy && (!to_xfb || need_xfb_pixels)) { int rw = w, rh = h; if (sx + rw > EFB_W) rw = EFB_W - sx; if (sy + rh > EFB_H) rh = EFB_H - sy; if (rw > 0 && rh > 0) gl_read_efb(efb_c, sx, sy, rw, rh); }
    if (to_xfb && !need_xfb_pixels) {
        g_xfb_ready = 1; g_xfb_addr = dst;
        extern void gx_frame_done(int w, int h);
        gx_frame_done(w, h);
    } else if (to_xfb) {
        /* write YUYV into RAM so VI-based code and screenshots see it */
        for (int y = 0; y < h; y++) for (int x = 0; x + 1 < w; x += 2) {
            u32 c0 = efb_c[(sy + y) * EFB_W + sx + x], c1 = efb_c[(sy + y) * EFB_W + sx + x + 1];
            float r0 = c0 & 255, g0 = (c0 >> 8) & 255, b0 = (c0 >> 16) & 255, r1 = c1 & 255, g1 = (c1 >> 8) & 255, b1 = (c1 >> 16) & 255;
            float y0 = 0.257f * r0 + 0.504f * g0 + 0.098f * b0 + 16, y1 = 0.257f * r1 + 0.504f * g1 + 0.098f * b1 + 16;
            float rr = (r0 + r1) / 2, gg = (g0 + g1) / 2, bb = (b0 + b1) / 2;
            float u = -0.148f * rr - 0.291f * gg + 0.439f * bb + 128, vv = 0.439f * rr - 0.368f * gg - 0.071f * bb + 128;
            u32 a = (dst + y * stride + x * 2) & 0x01FFFFFF;
            if (a + 4 > RAM_SIZE) continue;
            g_ram[a] = (u8)y0; g_ram[a + 1] = (u8)u; g_ram[a + 2] = (u8)y1; g_ram[a + 3] = (u8)vv;
        }
        g_xfb_ready = 1; g_xfb_addr = dst;
        extern void gx_frame_done(int w, int h);
        gx_frame_done(w, h);
    } else if (!gpu_copy) {
        /* texture copy (formats per Dolphin's EFBCopyFormat; layout = GX texture block layout) */
        int tpf = (v >> 3) & 15; int fmt = tpf / 2 + (tpf & 1) * 8; int inten = (v >> 15) & 1;
        int noalpha = (bp[0x43] & 7) != 1;
        int depth = (bp[0x43] & 7) == 3;   /* Z24 pixel format: copy the depth buffer (hi, mid, low bytes as r, g, b) */
        u32* src = efb_c;
        if (depth) {
            static u32 zb[EFB_W * EFB_H];
            if (g_use_gl) { int rw = w, rh = h; if (sx + rw > EFB_W) rw = EFB_W - sx; if (sy + rh > EFB_H) rh = EFB_H - sy; if (rw > 0 && rh > 0) gl_read_efb_depth(zb, sx, sy, rw, rh); }
            else memcpy(zb, efb_z, sizeof zb);
            for (int y = 0; y < h && sy + y < EFB_H; y++) for (int x = 0; x < w && sx + x < EFB_W; x++) { u32 z = zb[(sy + y) * EFB_W + sx + x]; zb[(sy + y) * EFB_W + sx + x] = (z >> 16 & 255) | (z >> 8 & 255) << 8 | (z & 255) << 16 | 0xFF000000u; }
            src = zb; inten = 0;
        }
        int scale = half ? 2 : 1; int tw = w / scale, th = h / scale;
        /* bits per texel and block size */
        int bpp = (fmt == 0) ? 4 : (fmt == 2 || fmt == 7 || fmt == 8 || fmt == 1 || fmt == 9 || fmt == 10) ? 8 : (fmt == 6) ? 32 : 16;
        int bw = bpp == 4 ? 8 : bpp == 8 ? 8 : 4, bh = bpp == 4 ? 8 : 4;
        int bpr = (tw + bw - 1) / bw;
        u32 base = dst & 0x01FFFFFF;
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
            u32 c;
            if (scale == 2) {  /* 2x2 box filter */
                u32 acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { u32 q = src[(sy + y * 2 + dy) * EFB_W + sx + x * 2 + dx]; for (int k = 0; k < 4; k++) acc[k] += (q >> (8 * k)) & 255; }
                c = (acc[0] / 4) | (acc[1] / 4) << 8 | (acc[2] / 4) << 16 | (acc[3] / 4) << 24;
            } else c = src[(sy + y) * EFB_W + sx + x];
            u32 r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = noalpha ? 255 : c >> 24;
            if (inten) { u32 Y = (u32)(0.257f * r + 0.504f * g + 0.098f * b + 16); if (Y > 255) Y = 255; r = g = b = Y; }
            int bx = x / bw, by = y / bh, px = x % bw, py = y % bh; u32 blk = by * bpr + bx;
            u32 o;
            switch (fmt) {
            case 0: /* R4 / I4 */ o = base + blk * 32 + (py * 8 + px) / 2; if (o >= RAM_SIZE) break; g_ram[o] = (px & 1) ? (u8)((g_ram[o] & 0xF0) | (r >> 4)) : (u8)((g_ram[o] & 0x0F) | (r & 0xF0)); break;
            case 2: /* RA4 / IA4 */ o = base + blk * 32 + py * 8 + px; if (o >= RAM_SIZE) break; g_ram[o] = (u8)((a & 0xF0) | (r >> 4)); break;
            case 7: o = base + blk * 32 + py * 8 + px; if (o < RAM_SIZE) g_ram[o] = (u8)a; break;   /* A8 */
            case 1: case 8: o = base + blk * 32 + py * 8 + px; if (o < RAM_SIZE) g_ram[o] = (u8)r; break;   /* R8 / I8 */
            case 9: o = base + blk * 32 + py * 8 + px; if (o < RAM_SIZE) g_ram[o] = (u8)g; break;           /* G8 */
            case 10: o = base + blk * 32 + py * 8 + px; if (o < RAM_SIZE) g_ram[o] = (u8)b; break;          /* B8 */
            case 3: o = base + blk * 32 + (py * 4 + px) * 2; if (o + 1 < RAM_SIZE) { g_ram[o] = (u8)a; g_ram[o + 1] = (u8)r; } break;   /* RA8 / IA8 */
            case 11: o = base + blk * 32 + (py * 4 + px) * 2; if (o + 1 < RAM_SIZE) { g_ram[o] = (u8)g; g_ram[o + 1] = (u8)r; } break;  /* RG8 */
            case 12: o = base + blk * 32 + (py * 4 + px) * 2; if (o + 1 < RAM_SIZE) { g_ram[o] = (u8)b; g_ram[o + 1] = (u8)g; } break;  /* GB8 */
            case 4: { o = base + blk * 32 + (py * 4 + px) * 2; u16 w16 = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); if (o + 1 < RAM_SIZE) { g_ram[o] = w16 >> 8; g_ram[o + 1] = (u8)w16; } break; }
            case 5: { o = base + blk * 32 + (py * 4 + px) * 2; u16 w16 = a >= 224 ? (u16)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)) : (u16)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)); if (o + 1 < RAM_SIZE) { g_ram[o] = w16 >> 8; g_ram[o + 1] = (u8)w16; } break; }
            case 6: o = base + blk * 64 + (py * 4 + px) * 2; if (o + 33 < RAM_SIZE) { g_ram[o] = (u8)a; g_ram[o + 1] = (u8)r; g_ram[o + 32] = (u8)g; g_ram[o + 33] = (u8)b; } break;
            default: break;
            }
        }
    }
    if (clear) {
        u32 ar = bp[0x4F], gb = bp[0x50], cz = bp[0x51] & 0xFFFFFF;
        u32 cc = (ar & 255) | ((gb >> 8) & 255) << 8 | (gb & 255) << 16 | ((ar >> 8) & 255) << 24;
        if (g_use_gl) { u32 bm = bp[0x41], zm = bp[0x40]; gl_clear(sx, sy, w, h, cc, cz, (bm >> 3) & 1, (bm >> 4) & 1, (zm >> 4) & 1); }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = (sy + y) * EFB_W + sx + x; if (i < EFB_W * EFB_H) { efb_c[i] = cc; efb_z[i] = cz; } }
    }
}

/* screenshot of the EFB as PPM */
void gx_dump_efb(const char* path, int w, int h) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { u32 c = efb_c[y * EFB_W + x]; fputc(c & 255, f); fputc((c >> 8) & 255, f); fputc((c >> 16) & 255, f); }
    fclose(f);
}
static int g_frame_no; int g_frame_no_pub; int g_dbgf;
#ifdef _WIN32
int g_dump_every = 0;    /* SOA_DUMP=N: write every Nth frame to frames/ */
#else
int g_dump_every = 25;
#endif
static void dbg_init(void) { static int d; if (!d) { d = 1; const char* e = getenv("SOA_DBGFRAME"); if (e) g_dbgf = atoi(e); e = getenv("SOA_DUMP"); if (e) g_dump_every = atoi(e); } }
WEAK void platform_present(void) { }
void gx_frame_done(int w, int h) {
    g_tex_epoch++;
    { extern u64 g_pf_present; u64 t = host_tb(); if (g_use_gl) platform_present(); g_pf_present += host_tb() - t; }
    {   /* every ~5 s: frame rate and where the host time went (ms per frame) */
        extern u64 g_pf_hash, g_pf_flush, g_pf_read, g_pf_present, g_pf_draws, g_pf_upload, g_pf_snd, g_pf_idle; static u64 t0, n;
        u64 now = host_tb(); n++;
        if (!t0) t0 = now;
        else if (now - t0 > 5ull * 40500000ull) {
            double ms = 1000.0 / 40500000.0 / n;
            rt_log("perf: %.1f fps | per frame ms: flush %.2f (hash %.2f, upload %.2f) readback %.2f present %.2f dc-sound %.2f idle %.2f | draws %.0f\n", n * 40500000.0 / (now - t0),
                   g_pf_flush * ms, g_pf_hash * ms, g_pf_upload * ms, g_pf_read * ms, g_pf_present * ms, g_pf_snd * ms, g_pf_idle * ms, (double)g_pf_draws / n);
            t0 = now; n = 0; g_pf_hash = g_pf_flush = g_pf_read = g_pf_present = g_pf_draws = g_pf_upload = g_pf_snd = g_pf_idle = 0;
        }
    }
    dbg_init(); g_frame_no++; g_frame_no_pub = g_frame_no;
    { static int armed = 1; if (armed && getenv("SOA_REDDBG")) { double m[3] = {0, 0, 0}; for (int y = 0; y < 480; y += 8) for (int x = 0; x < 640; x += 8) { u32 c = efb_c[y * EFB_W + x]; m[0] += c & 255; m[1] += (c >> 8) & 255; m[2] += (c >> 16) & 255; }
        if (m[0] > 2 * m[1] + 4800 * 10 && m[0] > 60 * 4800) { armed = 0; g_dbgf = g_frame_no + 1; rt_log("red frame %d detected, tracing next\n", g_frame_no); gx_dump_efb("frames/red_before.ppm", 640, 480); } } }
    { static int bt = -1; if (bt < 0) { const char* e = getenv("SOA_BT_FRAME"); bt = e ? atoi(e) : 0; }
      if (bt && g_frame_no == bt) { extern void rt_backtrace(Ctx*); rt_log("backtrace at frame %d\n", bt); rt_backtrace(g_ctx); } }
    if (g_dump_every && g_frame_no % g_dump_every == 0) {
        char p[64]; snprintf(p, sizeof p, "frames/f%05d.ppm", g_frame_no);
        gx_dump_efb(p, w, h);
    }
}
