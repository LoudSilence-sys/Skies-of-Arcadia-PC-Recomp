/* dcsnd.c - Dreamcast sound for the PC port.
 *
 * The GameCube game asks MusyX to play sequences (music and every sequenced sound effect) through a small
 * set of API calls.  We override those calls: when the requested song exists in the Dreamcast version, it is
 * played by the Dreamcast's own sound driver (BONITO.DRV from the user's discs) running on an emulated
 * AICA sound chip (Highly Theoretical core), with the Dreamcast's own banks (.MLT).  Anything that only
 * exists in Legends falls through to MusyX unchanged.
 *
 * GameCube bank slots 9/10/11 (cutscene banks) occupy the same sound-RAM regions as the music/event banks on
 * the Dreamcast, but the GameCube keeps them loaded side by side, so each of those slots gets its own
 * emulated sound chip.  Instances are clones of one warmed-up chip (the state is position independent).
 */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ht/sega.h"
#include "ht/dcsound.h"
#include "ht/yam.h"

void rt_log(const char* fmt, ...);
u64 host_tb(void);

/* ---------------- CPack / LZO1X ---------------- */
static int lzo1x(const u8* in, u32 inlen, u8* out, u32 outcap, u32* outlen) {
    const u8 *ip = in, *ie = in + inlen; u8 *op = out, *oe = out + outcap; u32 t, next = 0, state = 0; const u8* m;
#define NEED_IP(n) if ((u32)(ie - ip) < (u32)(n)) return -1
#define NEED_OP(n) if ((u32)(oe - op) < (u32)(n)) return -2
    NEED_IP(1);
    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) { next = t; goto match_next; }
        NEED_IP(t); NEED_OP(t); memcpy(op, ip, t); op += t; ip += t; state = 4;
    }
    for (;;) {
        NEED_IP(1); t = *ip++;
        if (t < 16) {
            if (state == 0) {
                if (t == 0) { while (ip < ie && *ip == 0) { t += 255; ip++; } NEED_IP(1); t += 15 + *ip++; }
                t += 3; NEED_IP(t); NEED_OP(t); memcpy(op, ip, t); op += t; ip += t; state = 4; continue;
            } else if (state != 4) {
                NEED_IP(1); next = t & 3; m = op - 1 - (t >> 2) - (*ip++ << 2); t = 2;
            } else {
                NEED_IP(1); next = t & 3; m = op - 1 - 0x800 - (t >> 2) - (*ip++ << 2); t = 3;
            }
        } else if (t >= 64) {
            NEED_IP(1); next = t & 3; m = op - 1 - ((t >> 2) & 7) - (*ip++ << 3); t = (t >> 5) + 1;
        } else if (t >= 32) {
            t &= 31;
            if (t == 0) { while (ip < ie && *ip == 0) { t += 255; ip++; } NEED_IP(1); t += 31 + *ip++; }
            t += 2; NEED_IP(2); u32 v = ip[0] | ip[1] << 8; ip += 2; m = op - 1 - (v >> 2); next = v & 3;
        } else {
            u32 hi = (t & 8) << 11; t &= 7;
            if (t == 0) { while (ip < ie && *ip == 0) { t += 255; ip++; } NEED_IP(1); t += 7 + *ip++; }
            t += 2; NEED_IP(2); u32 v = ip[0] | ip[1] << 8; ip += 2; next = v & 3;
            m = op - hi - (v >> 2);
            if (m == op) break;   /* end of stream */
            m -= 0x4000;
        }
        if (m < out) return -3;
        NEED_OP(t); while (t--) *op++ = *m++;
match_next:
        state = next; t = next;
        if (t) { NEED_IP(t); NEED_OP(t); memcpy(op, ip, t); op += t; ip += t; }
    }
    *outlen = (u32)(op - out);
    return 0;
}
static u32 rd32le(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u8* cpack_load(const char* path, u32* len) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return 0; } fclose(f);
    if (n < 16 || memcmp(b, "CPack!", 6)) { *len = (u32)n; return b; }
    u32 size = rd32le(b + 8), cs = rd32le(b + 12); u8* out = malloc(size + cs); u32 got = 0; u32 p = 0x10;
    while (got < size) {
        u32 off = rd32le(b + p), sz = rd32le(b + p + 4); p += 8;
        if (sz == cs) { memcpy(out + got, b + off, sz); got += sz; }
        else { u32 ol = 0; if (lzo1x(b + off, sz, out + got, size + cs - got, &ol)) { rt_log("dcsnd: LZO error in %s\n", path); free(b); free(out); return 0; } got += ol; }
    }
    free(b); *len = size; return out;
}

/* ---------------- Dreamcast files ---------------- */
static char g_roots[4][600]; static int g_nroots;
typedef struct { char name[16]; u8* data; u32 len; int smsb_bank, songs, has_sfpb; } Mlt;
static Mlt g_mlt[160]; static int g_nmlt;
static u8* g_drv; static u32 g_drv_len;
static Mlt* g_ares;

static void mlt_scan(Mlt* m) {
    m->smsb_bank = -1; m->songs = 0; m->has_sfpb = 0;
    if (m->len < 0x20 || memcmp(m->data, "SMLT", 4)) return;
    u32 n = rd32le(m->data + 8);
    for (u32 i = 0; i < n; i++) {
        const u8* u = m->data + 0x20 + i * 0x20; u32 bank = rd32le(u + 4), fo = rd32le(u + 16), fl = rd32le(u + 20);
        if (fo == 0xFFFFFFFFu || fo + fl > m->len) continue;
        if (!memcmp(u, "SMSB", 4) && m->smsb_bank < 0) { m->smsb_bank = (int)bank; m->songs = (int)rd32le(m->data + fo + 12); }
        if (!memcmp(u, "SFPB", 4)) m->has_sfpb = 1;
    }
}
static Mlt* mlt_get(const char* name) {   /* name: upper case, no extension */
    for (int i = 0; i < g_nmlt; i++) if (!strcmp(g_mlt[i].name, name)) return g_mlt[i].data ? &g_mlt[i] : 0;
    if (g_nmlt >= 160) { /* evict the oldest non-resident entry */ free(g_mlt[1].data); memmove(&g_mlt[1], &g_mlt[2], sizeof(Mlt) * (g_nmlt - 2)); g_nmlt--; }
    Mlt* m = &g_mlt[g_nmlt++]; memset(m, 0, sizeof *m); snprintf(m->name, sizeof m->name, "%s", name);
    for (int r = 0; r < g_nroots && !m->data; r++) {
        char p[700];
        snprintf(p, sizeof p, "%s/SOUND/%s.MLT", g_roots[r], name); m->data = cpack_load(p, &m->len);
        if (!m->data) { snprintf(p, sizeof p, "%s/%s.MLT", g_roots[r], name); m->data = cpack_load(p, &m->len); }
    }
    if (m->data) mlt_scan(m);
    return m->data ? m : 0;
}

/* ---------------- emulated sound chips ---------------- */
#define NINST 4
#define NPORT 8
#define TB_RATE 40500000ull
typedef struct {
    u32 id; int busy, pending, paused, slot;
    int vol;                       /* current volume sent to the driver (0..127) */
    int fade_from, fade_to, fade_t, fade_len, fade_mode;   /* fade in 5 ms steps */
    u64 started; int last_st; int music;
} Port;
typedef struct {
    u8* st; void* dc; int used; u64 last_active;
    char bank_mlt[16][16];
    Port port[NPORT];
    u8 q[96][16]; int nq;
} Inst;
static Inst g_inst[NINST];
static int g_on, g_tried;
static u32 g_gen = 1;
static float g_gain = 1.718f;   /* +4.7 dB, see dc_vol (verified against the GameCube mix) */
static int g_log;
/* MusyX volume (linear 0..127) -> driver port volume (about 0.41 dB per step).  Music keeps the Legends
   balance against effects (it is mixed hotter there); effects are offset down and the whole Dreamcast
   mix gets +7.1 dB, so both land at the measured GameCube levels. */
#define MUSIC_BIAS_DB 9.5   /* measured: menu SE matched GC at -2.9 dB, battleship music needed +5 dB */
static int dc_vol(int v, int music) {
    if (v <= 0) return 0;
    double db = 20.0 * log10(v / 127.0) - (music ? 0.0 : MUSIC_BIAS_DB);
    int d = (int)lround(127.0 + db / 0.41);
    return d < 1 ? 1 : d > 127 ? 127 : d;
}
static const char* inst_name[NINST] = {"main", "slot9", "slot10", "slot11"};

static void w32(Inst* in, u32 a, u32 v) { dcsound_setword(in->dc, a, v); }
static u32 r32(Inst* in, u32 a) { return dcsound_getword(in->dc, a); }
static void w8(Inst* in, u32 a, u8 v) { u32 w = r32(in, a & ~3u); int sh = (a & 3) * 8; w32(in, a & ~3u, (w & ~(0xFFu << sh)) | ((u32)v << sh)); }
static void run(Inst* in, s16* buf, u32 n) {
    static s16 dummy[2 * 4096];
    while (n) { u32 k = n > 4096 ? 4096 : n; u32 got = k; if (sega_execute(in->st, 0x7FFFFFFF, buf ? buf : dummy, &got) < 0) { rt_log("dcsnd: execute error\n"); return; } if (!got) return; n -= got; if (buf) buf += 2 * got; }
}
static void cmd(Inst* in, int a, int b, int c, int d, int e, int f) {
    if (in->nq >= 96) return;
    u8* p = in->q[in->nq++]; memset(p, 0, 16); p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)c; p[3] = (u8)d; p[4] = (u8)e; p[5] = (u8)f;
}
static void flush_cmds(Inst* in) {
    if (!in->nq || (r32(in, 0x12400) & 0xFF)) return;
    int n = in->nq > 32 ? 32 : in->nq;
    for (int i = 0; i < n; i++) for (int k = 0; k < 16; k += 4) w32(in, 0x12200 + i * 16 + k, rd32le(in->q[i] + k));
    memmove(in->q, in->q + n, (size_t)(in->nq - n) * 16); in->nq -= n;
    w8(in, 0x12400, 1);
}
static void upload_units(Inst* in, Mlt* m, int only_bank /* -1 = all */) {
    u32 n = rd32le(m->data + 8);
    for (u32 i = 0; i < n; i++) {
        const u8* u = m->data + 0x20 + i * 0x20; u32 bank = rd32le(u + 4), addr = rd32le(u + 8), fo = rd32le(u + 16), fl = rd32le(u + 20);
        if (fo == 0xFFFFFFFFu || fo + fl > m->len) continue;
        if (only_bank >= 0 && (int)bank != only_bank && memcmp(u, "SFOB", 4)) continue;
        dcsound_upload_to_ram(in->dc, addr, m->data + fo, fl);
        if (!memcmp(u, "SMSB", 4) && bank < 16) { w32(in, 0x14000 + 8 * bank, addr); snprintf(in->bank_mlt[bank], 16, "%s", m->name); }
        if (!memcmp(u, "SMPB", 4) && bank < 16) w32(in, 0x14080 + 8 * bank, addr);
        if (!memcmp(u, "SFPB", 4)) w32(in, 0x14200, addr);
        if (!memcmp(u, "SFOB", 4)) w32(in, 0x14280, addr);
    }
}

static int dcsnd_init(void) {
    g_tried = 1;
    if (getenv("SOA_NODC")) return 0;
    g_log = getenv("SOA_DCLOG") != 0;
    { const char* e = getenv("SOA_DCGAIN"); if (e) g_gain = (float)atof(e); }
    FILE* f = fopen("dc_path.txt", "r");
    if (f) { char line[600]; while (g_nroots < 4 && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0] && line[0] != '#') snprintf(g_roots[g_nroots++], 600, "%s", line); } fclose(f); }
    { const char* e = getenv("SOA_DCROOT"); if (e && g_nroots < 4) snprintf(g_roots[g_nroots++], 600, "%s", e); }
    for (int r = 0; r < g_nroots && !g_drv; r++) { char p[700]; snprintf(p, sizeof p, "%s/BONITO.DRV", g_roots[r]); g_drv = cpack_load(p, &g_drv_len); }
    g_ares = mlt_get("ARESNEW");
    if (!g_drv || !g_ares) { rt_log("dcsnd: Dreamcast sound files not found (dc_path.txt) - using GameCube sound\n"); return 0; }
    sega_init();
    u32 sz = sega_get_state_size(2);
    Inst* in = &g_inst[0];
    in->st = calloc(1, sz); sega_clear_state(in->st, 2);
    sega_enable_dsp(in->st, 1); sega_enable_dry(in->st, 1); sega_enable_dsp_dynarec(in->st, 0);
    in->dc = sega_get_dcsound_state(in->st);
    u8* prog = malloc(g_drv_len); memset(prog, 0, 4); memcpy(prog + 4, g_drv + 0x20, g_drv_len - 0x20);
    if (sega_upload_program(in->st, prog, g_drv_len - 0x20 + 4)) { rt_log("dcsnd: driver upload failed\n"); free(prog); return 0; }
    free(prog);
    run(in, 0, 4410);
    if (r32(in, 0xF8) != 0x43455845u) { rt_log("dcsnd: driver did not start\n"); return 0; }
    upload_units(in, g_ares, -1);
    /* reverb ring buffer (the driver refuses to load the FX program without it), then FX program + output */
    { static u8 z[0x10000]; dcsound_upload_to_ram(in->dc, 0x1F0000, z, sizeof z); }
    w32(in, 0x14288, 0x1F0000); w32(in, 0x1428C, 0x10000);
    cmd(in, 0x84, 0, 0, 0, 0, 0); cmd(in, 0x82, 0, 0, 0, 0, 0); flush_cmds(in); run(in, 0, 441);
    run(in, 0, 44100 * 6);    /* effects DSP warm-up */
    for (int i = 1; i < NINST; i++) { g_inst[i] = g_inst[0]; g_inst[i].st = malloc(sz); memcpy(g_inst[i].st, in->st, sz); g_inst[i].dc = sega_get_dcsound_state(g_inst[i].st); }
    in->used = 1;
    rt_log("dcsnd: Dreamcast sound driver running (%s), %d bank folders\n", g_roots[0], g_nroots);
    return 1;
}

/* ---------------- GameCube bank slots ---------------- */
#define SLOT_TABLE 0x803187C0u   /* lbl_80317F60 + 0x860: 12 x 60 bytes */
static int find_slot(u32 group) {
    for (int s = 0; s < 12; s++) { u32 e = SLOT_TABLE + s * 0x3C; if ((s32)R32(e + 0x10) >= 0 && R32(e + 8) == group && R32(e)) return s; }
    return -1;
}
static void slot_name(int s, char* out) {   /* bank file name -> upper-case base name */
    char raw[40]; u32 e = SLOT_TABLE + s * 0x3C + 0x14; int k = 0;
    for (; k < 31; k++) { raw[k] = (char)R8(e + k); if (!raw[k]) break; } raw[k] = 0;
    const char* b = raw; for (const char* p = raw; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    k = 0; for (; b[k] && b[k] != '.' && k < 15; k++) out[k] = (char)(b[k] >= 'a' && b[k] <= 'z' ? b[k] - 32 : b[k]); out[k] = 0;
}

static Inst* id_inst(u32 id, Port** pp) {
    if ((id & 0xF0000000u) != 0x40000000u) return 0;
    int i = (id >> 3) & 3, p = id & 7; Inst* in = &g_inst[i];
    if (in->port[p].id != id) { *pp = 0; return in; }
    *pp = &in->port[p]; return in;
}
static int is_dc_id(u32 id) { return (id & 0xF0000000u) == 0x40000000u; }

/* returns a fake sequence id, or 0xFFFFFFFF to let MusyX play it */
static u32 dc_play_name(int s, const char* name, int song);
static u32 dc_play(u32 group, u32 songid) {
    if (!g_tried) g_on = dcsnd_init();
    if (!g_on) return 0xFFFFFFFFu;
    int s = find_slot(group); if (s < 0) return 0xFFFFFFFFu;
    int song = (int)(songid - R32(SLOT_TABLE + s * 0x3C + 0xC));
    char name[16]; slot_name(s, name);
    return dc_play_name(s, name, song);
}
static u32 dc_play_name(int s, const char* name, int song) {
    int inst = s >= 9 ? s - 8 : 0; Inst* in = &g_inst[inst];
    int bank, count; Mlt* m = 0;
    if (!strcmp(name, "TONE")) return 0xFFFFFFFFu;
    if (!strcmp(name, "S1000000")) { bank = 1; count = 34; }   /* ARESNEW bank 1; Legends added songs 34.. */
    else { m = mlt_get(name); if (!m || m->smsb_bank < 0) { if (g_log) rt_log("dcsnd: %s #%d not on Dreamcast -> GC\n", name, song); return 0xFFFFFFFFu; } bank = m->smsb_bank; count = m->songs; }
    if (song < 0 || song >= count) { if (g_log) rt_log("dcsnd: %s #%d beyond Dreamcast bank (%d songs) -> GC\n", name, song, count); return 0xFFFFFFFFu; }
    if (m && strcmp(in->bank_mlt[bank], m->name)) {
        upload_units(in, m, -1);
        cmd(in, 0x82, 0, 0, 0, 0, 0);
        if (m->has_sfpb) cmd(in, 0x84, 0, 0, 0, 0, 0);
    }
    /* pick a port: a free one, else the oldest */
    int p = -1; u64 oldest = ~0ull;
    for (int i = 0; i < NPORT; i++) if (!in->port[i].busy && !in->port[i].pending && !in->port[i].paused) { p = i; break; }
    if (p < 0) { for (int i = 0; i < NPORT; i++) if (in->port[i].started < oldest) { oldest = in->port[i].started; p = i; } cmd(in, 0x02, 0, p, 1, 0, 0); }
    Port* pt = &in->port[p];
    g_gen = (g_gen + 1) & 0xFFFF; if (!g_gen) g_gen = 1;
    memset(pt, 0, sizeof *pt);
    pt->id = 0x40000000u | g_gen << 5 | (u32)inst << 3 | (u32)p; pt->pending = 1; pt->slot = s; pt->vol = 127; pt->started = host_tb();
    pt->music = name[0] == 'M' && name[1] == '0';
    cmd(in, 0x05, 0, p, dc_vol(127, pt->music), 0, 0);
    cmd(in, 0x01, 0, p, bank, song, 0);
    in->used = 1; in->last_active = host_tb();
    if (g_log) rt_log("dcsnd: t=%llus play %s #%d -> %s port %d bank %d (id %08X)\n", (unsigned long long)(host_tb() / TB_RATE), name, song, inst_name[inst], p, bank, pt->id);
    return pt->id;
}

static void port_poll(Inst* in, u64 now) {
    for (int p = 0; p < NPORT; p++) {
        Port* pt = &in->port[p]; if (!pt->id) continue;
        u32 stw = r32(in, 0x12600 + p * 64); int playing = stw & 1;
        if (g_log && (int)(stw & 0xFF) != pt->last_st) { rt_log("dcsnd: t=%llus %s port %d status %08X busy %d pending %d\n", (unsigned long long)(now / TB_RATE), inst_name[in - g_inst], p, stw, pt->busy, pt->pending); pt->last_st = (int)(stw & 0xFF); }
        if (pt->pending) { if (playing) { pt->pending = 0; pt->busy = 1; } else if ((s64)(now - pt->started) > (s64)(TB_RATE / 5) && !in->nq) { pt->pending = 0; pt->busy = 0; } }
        else if (pt->busy && !playing && !pt->paused) pt->busy = 0;
        if (pt->fade_len) {
            pt->fade_t++;
            int v = pt->fade_from + (pt->fade_to - pt->fade_from) * pt->fade_t / pt->fade_len;
            if (v != pt->vol) { pt->vol = v; cmd(in, 0x05, 0, p, dc_vol(v, pt->music), 0, 0); }
            if (pt->fade_t >= pt->fade_len) {
                pt->fade_len = 0;
                if (pt->fade_mode == 1) { cmd(in, 0x02, 0, p, 0, 0, 0); pt->busy = 0; pt->pending = 0; }
                else if (pt->fade_mode == 2) { cmd(in, 0x03, 0, p, 0, 0, 0); pt->paused = 1; }
            }
        }
        if (pt->busy || pt->pending || pt->paused) in->last_active = now;
    }
}

/* mixing: called with each AI DMA block (stereo, host order L,R) */
void dcsnd_mix(s16* buf, u32 frames, int rate) {
    { static const char* t; static int init; if (!init) { init = 1; t = getenv("SOA_DCTEST"); if (t) { g_on = dcsnd_init(); } }
      if (t && g_on) { static u64 t0; if (!t0) t0 = host_tb(); if (host_tb() - t0 > TB_RATE * 3) { char nm[16]; int sg = 0, k = 0; while (t[k] && t[k] != ':' && k < 15) { nm[k] = t[k]; k++; } nm[k] = 0; if (t[k] == ':') sg = atoi(t + k + 1); u32 id = dc_play_name(2, nm, sg); rt_log("dcsnd: test play %s #%d -> %08X\n", nm, sg, id); t = 0; } } }
    if (!g_on || !frames || rate <= 0) return;
    static double frac; static float carry[2];
    static s16 tmp[2 * 4200]; static float acc[2 * 4200];
    double step = 44100.0 / rate;
    u32 m = (u32)floor(frac + (frames - 1) * step) + 1; if (m > 4100) m = 4100;
    memset(acc, 0, sizeof(float) * 2 * (m + 1));
    acc[0] = carry[0]; acc[1] = carry[1];
    u64 now = host_tb();
    for (int i = 0; i < NINST; i++) {
        Inst* in = &g_inst[i]; if (!in->used) continue;
        port_poll(in, now);
        { static int noskip = -1; if (noskip < 0) noskip = getenv("SOA_DCNOSKIP") != 0; if (!noskip && (s64)(now - in->last_active) > (s64)(TB_RATE * 4) && !in->nq) continue; }   /* idle: skip (reverb tail has decayed) */
        flush_cmds(in);
        run(in, tmp, m);
        { static FILE* df; static int dinit; if (!dinit) { dinit = 1; const char* e = getenv("SOA_DCDUMP"); if (e) df = fopen(e, "wb"); } if (df && i == 0) fwrite(tmp, 4, m, df); }
        if (g_log) { static u64 dt; static int mx; for (u32 k = 0; k < 2 * m; k++) { int v = tmp[k] < 0 ? -tmp[k] : tmp[k]; if (v > mx) mx = v; } if (now - dt > TB_RATE * 2) { rt_log("dcsnd: inst %d raw peak %d m %u seq %08X\n", i, mx, m, r32(in, 0x11000 + 0x10)); mx = 0; dt = now; } }
        for (u32 k = 0; k < m; k++) { acc[2 * (k + 1)] += tmp[2 * k]; acc[2 * (k + 1) + 1] += tmp[2 * k + 1]; }
    }
    static float dcb_x[2], dcb_y[2]; static double lvl; static u32 lvl_n; static u64 lvl_t;
    for (u32 o = 0; o < frames; o++) {
        double t = frac + o * step; u32 k = (u32)t; float f = (float)(t - k);
        if (k + 1 > m) k = m - 1;
        for (int c = 0; c < 2; c++) {
            float x = acc[2 * k + c] * (1 - f) + acc[2 * (k + 1) + c] * f;
            float v = x - dcb_x[c] + 0.999f * dcb_y[c]; dcb_x[c] = x; dcb_y[c] = v;   /* DC blocker (emulated chip has an output offset) */
            lvl += (double)v * v; lvl_n++;
            float s = buf[2 * o + c] + v * g_gain;
            buf[2 * o + c] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
    }
    if (g_log && now - lvl_t > TB_RATE * 2) { rt_log("dcsnd: t=%llus level %.1f dB\n", (unsigned long long)(now / TB_RATE), 10 * log10(lvl / (lvl_n ? lvl_n : 1) + 1)); lvl = 0; lvl_n = 0; lvl_t = now; }
    frac = frac + frames * step - m; if (frac < 0) frac = 0;
    carry[0] = acc[2 * m]; carry[1] = acc[2 * m + 1];
}

/* ---------------- MusyX overrides ---------------- */
void r_8027E6F8(Ctx*); void r_802703C0(Ctx*); void r_802704D0(Ctx*); void r_80270388(Ctx*); void r_80270440(Ctx*);
void r_80270358(Ctx*); void r_80270610(Ctx*); void r_80270538(Ctx*);

void f_8027E6F8(Ctx* c) {   /* sndSeqPlayEx(groupId, songId, arrfile, params, studio) */
    u32 id = dc_play(c->r[3] & 0xFFFF, c->r[4] & 0xFFFF);
    if (id != 0xFFFFFFFFu) { c->r[3] = id; return; }
    r_8027E6F8(c);
}
void f_802703C0(Ctx* c) {   /* sndSeqStop(seqId) */
    u32 id = c->r[3];
    if (!is_dc_id(id)) { r_802703C0(c); return; }
    Port* pt; Inst* in = id_inst(id, &pt);
    if (g_log) rt_log("dcsnd: t=%llus stop %08X\n", (unsigned long long)(host_tb() / TB_RATE), id);
    if (pt) { cmd(in, 0x02, 0, id & 7, 0, 0, 0); pt->busy = pt->pending = pt->paused = 0; pt->fade_len = 0; pt->id = 0; }
}
void f_802704D0(Ctx* c) {   /* sndSeqVolume(vol, time ms, seqId, mode) */
    u32 id = c->r[5];
    if (!is_dc_id(id)) { r_802704D0(c); return; }
    Port* pt; Inst* in = id_inst(id, &pt); if (!pt) return;
    int vol = c->r[3] & 0xFF; if (vol > 127) vol = 127; int ms = c->r[4] & 0xFFFF; int mode = c->r[6] & 0xFF;
    if (ms < 5) {
        pt->fade_len = 0; if (vol != pt->vol) { pt->vol = vol; cmd(in, 0x05, 0, id & 7, dc_vol(vol, pt->music), 0, 0); }
        if (mode == 1) { cmd(in, 0x02, 0, id & 7, 0, 0, 0); pt->busy = pt->pending = 0; }
        else if (mode == 2) { cmd(in, 0x03, 0, id & 7, 0, 0, 0); pt->paused = 1; }
    } else { pt->fade_from = pt->vol; pt->fade_to = vol; pt->fade_t = 0; pt->fade_len = ms / 5; pt->fade_mode = mode; }
    if (g_log) rt_log("dcsnd: t=%llus volume %08X -> %d over %d ms (mode %d)\n", (unsigned long long)(host_tb() / TB_RATE), id, vol, ms, mode);
}
void f_80270388(Ctx* c) {   /* sndSeqPause */
    u32 id = c->r[3];
    if (!is_dc_id(id)) { r_80270388(c); return; }
    Port* pt; Inst* in = id_inst(id, &pt);
    if (g_log) rt_log("dcsnd: t=%llus pause %08X\n", (unsigned long long)(host_tb() / TB_RATE), id);
    if (!pt || pt->paused) return;
    cmd(in, 0x03, 0, id & 7, 0, 0, 0); pt->paused = 1;
}
void f_80270440(Ctx* c) {   /* sndSeqContinue */
    u32 id = c->r[3];
    if (!is_dc_id(id)) { r_80270440(c); return; }
    Port* pt; Inst* in = id_inst(id, &pt);
    if (g_log) rt_log("dcsnd: t=%llus continue %08X\n", (unsigned long long)(host_tb() / TB_RATE), id);
    if (!pt || !pt->paused) return;
    cmd(in, 0x04, 0, id & 7, 0, 0, 0); pt->paused = 0; in->last_active = host_tb();
}
void f_80270358(Ctx* c) {   /* sndSeqGetValid -> bool */
    u32 id = c->r[3];
    if (!is_dc_id(id)) { r_80270358(c); return; }
    Port* pt; id_inst(id, &pt);
    c->r[3] = pt && (pt->busy || pt->pending || pt->paused) ? 1 : 0;
}
void f_80270610(Ctx* c) {   /* sndSilence: stop everything */
    if (g_log) rt_log("dcsnd: t=%llus silence all\n", (unsigned long long)(host_tb() / TB_RATE));
    r_80270610(c);
    for (int i = 0; i < NINST; i++) for (int p = 0; p < NPORT; p++) {
        Port* pt = &g_inst[i].port[p]; if (!pt->id) continue;
        cmd(&g_inst[i], 0x02, 0, p, 1, 0, 0); memset(pt, 0, sizeof *pt);
    }
}
void f_80270538(Ctx* c) {   /* sndSeqSendMidiCtrl(seqId, channel, ctrl, value) -> bool */
    u32 id = c->r[3];
    if (!is_dc_id(id)) { r_80270538(c); return; }
    Port* pt; Inst* in = id_inst(id, &pt);
    if (!pt) { c->r[3] = 0; return; }
    { static int n; if (g_log && n++ < 200) rt_log("dcsnd: t=%llus midi %08X ch %d ctrl %d val %d\n", (unsigned long long)(host_tb() / TB_RATE), id, c->r[4] & 15, c->r[5] & 127, c->r[6] & 127); }
    /* driver system command 0x86 injects a raw MIDI event: type 3 = control change */
    u32 ev = 3u << 24 | (id & 7) << 20 | (c->r[4] & 15) << 16 | (c->r[5] & 127) << 8 | (c->r[6] & 127);
    if (in->nq >= 96) { c->r[3] = 0; return; }
    cmd(in, 0x86, 0, 0, 0, 0, 0); memcpy(in->q[in->nq - 1] + 4, &ev, 4);   /* little-endian word at +4 */
    c->r[3] = 1;
}
void r_802706A4(Ctx*);
void f_802706A4(Ctx* c) {   /* sndOutputMode(0 = mono, 1 = stereo, 2 = surround): AICA MONO bit (reg 0x2800 bit 15) */
    int mono = (c->r[3] & 0xFF) == 0;
    r_802706A4(c);
    if (!g_tried) g_on = dcsnd_init();
    if (!g_on) return;
    for (int i = 0; i < NINST; i++) yam_aica_store_reg(dcsound_get_yam_state(g_inst[i].dc), 0x2800, mono ? 0x8000 : 0, 0x8000, 0);
    if (g_log) rt_log("dcsnd: output mode %s\n", mono ? "mono" : "stereo");
}
