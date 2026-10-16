/* ax.c - AX audio microcode (CRC 4e8a8b21, used by MusyX here), ported to C from Dolphin's
   DSPHLE AX implementation (GPL-2.0-or-later), plus AI DMA timing and host audio output. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_log(const char* fmt, ...);
void dsp_send(u32 mail, int irq);
u64 host_tb(void);
extern u8* g_aram;
#define ARAM_SIZE 0x01000000u

/* ---------------- parameter block (GC AX, no LPF field for this ucode) ---------------- */
typedef struct { u16 volume, volume_delta; } VolumeData;
typedef struct {
    u16 next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
    u16 src_type, coef_select, mixer_control, running, is_stream;
    VolumeData main_l, main_r, auxa_l, auxa_r, auxb_l, auxb_r, auxb_s, main_s, auxa_s;   /* mixer */
    u16 itd[7];
    u16 num_updates[5], upd_hi, upd_lo;
    s16 dpop[9];
    s16 cur_volume, cur_volume_delta;
    u16 unknown3[3];
    u16 looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo;
    s16 coefs[16]; u16 gain, pred_scale; s16 yn1, yn2;
    u16 ratio_hi, ratio_lo, cur_addr_frac; s16 last_samples[4];
    u16 loop_pred_scale; u16 loop_yn1, loop_yn2;
    u16 lpf_on; s16 lpf_yn1; u16 lpf_a0; s16 lpf_b0;     /* not present in memory for this ucode */
    u16 loop_counter;
    u16 padding[24];
} AXPB;
#define PBW (sizeof(AXPB) / 2)
#define LPF_OFF ((size_t)((char*)&((AXPB*)0)->lpf_on - (char*)0) / 2)
#define LC_OFF ((size_t)((char*)&((AXPB*)0)->loop_counter - (char*)0) / 2)

static void read_pb(u32 addr, AXPB* pb) {
    u16* d = (u16*)pb; memset(pb, 0, sizeof *pb);
    for (size_t i = 0; i < LPF_OFF; i++) d[i] = R16(addr + 2 * i);
    for (size_t i = LC_OFF; i < PBW; i++) d[i] = R16(addr + 2 * (i - (LC_OFF - LPF_OFF)));
}
static void write_pb(u32 addr, const AXPB* pb) {
    const u16* d = (const u16*)pb;
    for (size_t i = 0; i < LPF_OFF; i++) W16(addr + 2 * i, d[i]);
    for (size_t i = LC_OFF; i < PBW; i++) W16(addr + 2 * (i - (LC_OFF - LPF_OFF)), d[i]);
}

/* ---------------- accelerator ---------------- */
typedef struct { u32 start, end, cur; u16 fmt; s16 gain, yn1, yn2; u16 pred_scale; int stopped; AXPB* pb; } Acc;
static u8 aram_rd(u32 a) { return (g_aram && a < ARAM_SIZE) ? g_aram[a] : 0; }
static s16 clamp16(s64 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }

static void acc_setup(Acc* a, AXPB* pb) {
    a->pb = pb;
    a->start = ((u32)pb->loop_addr_hi << 16 | pb->loop_addr_lo) & 0x3fffffff;
    a->end = ((u32)pb->end_addr_hi << 16 | pb->end_addr_lo) & 0x3fffffff;
    a->cur = ((u32)pb->cur_addr_hi << 16 | pb->cur_addr_lo) & 0xbfffffff;
    a->fmt = pb->sample_format; a->yn1 = pb->yn1; a->yn2 = pb->yn2; a->gain = (s16)pb->gain; a->pred_scale = pb->pred_scale & 0x7f;
    a->stopped = 0;
}
static u16 acc_raw(Acc* a) {
    switch (a->fmt & 3) {
    case 0: { u16 v = aram_rd(a->cur >> 1); return (a->cur & 1) ? (v & 0xF) : (v >> 4); }
    case 1: return aram_rd(a->cur);
    case 2: return (u16)(aram_rd(a->cur * 2) << 8 | aram_rd(a->cur * 2 + 1));
    }
    return 0;
}
static void acc_end_exception(Acc* a) {
    AXPB* pb = a->pb;
    if (pb->looping) {
        a->pred_scale = pb->loop_pred_scale & 0x7f;
        if (pb->is_stream != 1) { a->yn1 = (s16)pb->loop_yn1; a->yn2 = (s16)pb->loop_yn2; a->stopped = 0; }
        else { a->stopped = 0; pb->loop_counter++; }
    } else pb->running = 0;
}
static s16 acc_sample(Acc* a, const s16* coefs) {
    if (a->stopped) return 0;
    s16 raw = (s16)acc_raw(a);
    int ci = (a->pred_scale >> 4) & 7; s32 c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
    s16 val = 0; int step = 0;
    int decode = (a->fmt >> 2) & 3;
    if (decode == 0) {
        raw &= 0xF; int scale = 1 << (a->pred_scale & 0xF); if (raw >= 8) raw -= 16;
        s32 v32 = scale * raw + ((0x400 + c1 * a->yn1 + c2 * a->yn2) >> 11);
        val = clamp16(v32); step = 2;
        a->yn2 = a->yn1; a->yn1 = val; a->cur += 1;
        if ((a->end & 0xf) == 0 && a->cur == a->end) a->cur = a->start + 1;
        else if ((a->end & 0xf) == 1 && a->cur == a->end - 1) a->cur = a->start;
        else if ((a->cur & 15) == 0) { a->pred_scale = aram_rd((a->cur & ~15u) >> 1) & 0x7f; a->cur += 2; step += 2; }
    } else {
        int gs = ((a->fmt >> 4) & 3) == 0 ? 11 : ((a->fmt >> 4) & 3) == 1 ? 0 : 16;
        s32 v32 = (((s32)a->gain * raw) >> gs) + ((c1 * a->yn1) >> gs) + ((c2 * a->yn2) >> gs);
        val = (s16)v32; a->yn2 = a->yn1; a->yn1 = val; step = 2; a->cur += 1;
    }
    if (a->cur == a->end + step - 1) { a->cur = a->start; a->stopped = 1; acc_end_exception(a); }
    a->cur &= 0xbfffffff;
    return val;
}

/* ---------------- resampling ---------------- */
static s16 g_coeffs[0x800]; static int g_have_coeffs;
static u32 resample(Acc* acc, const s16* coefs, s16* out, u32 count, s16* last, u32 pos, u32 ratio, int srctype, const s16* pcoef) {
    s16 t[4]; u32 idx = 0;
    t[idx++ & 3] = last[0]; t[idx++ & 3] = last[1]; t[idx++ & 3] = last[2]; t[idx++ & 3] = last[3];
    if (pcoef && srctype == 0) {
        for (u32 i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = acc_sample(acc, coefs); pos -= 0x10000; }
            u16 fr = ((pos & 0xFFFF) >> 9) << 2; const s16* c = &pcoef[fr];
            s64 a0 = t[idx++ & 3], a1 = t[idx++ & 3], a2 = t[idx++ & 3], a3 = t[idx++ & 3];
            out[i] = clamp16((a0 * c[0] + a1 * c[1] + a2 * c[2] + a3 * c[3]) >> 15);
        }
    } else if (srctype == 0 || srctype == 1) {
        for (u32 i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = acc_sample(acc, coefs); pos -= 0x10000; }
            u16 fr = pos & 0xFFFF; u16 inv = (u16)-fr;
            if (fr) { s32 s0 = t[idx++ & 3], s1 = t[idx++ & 3]; out[i] = (s16)((s0 * inv + s1 * fr) >> 16); idx += 2; }
            else { out[i] = t[idx++ & 3]; idx += 3; }
        }
    } else {
        for (u32 i = 0; i < count; i++) out[i] = acc_sample(acc, coefs);
        memcpy(last, out + count - 4, 8); return pos;
    }
    last[3] = t[--idx & 3]; last[2] = t[--idx & 3]; last[1] = t[--idx & 3]; last[0] = t[--idx & 3];
    return pos;
}

/* ---------------- mixing ---------------- */
static int s_main[3][160], s_auxa[3][160], s_auxb[3][160];   /* L R S */
enum { MAIN_L = 1, MAIN_L_RAMP = 2, MAIN_R = 4, MAIN_R_RAMP = 8, MAIN_S = 0x10, MAIN_S_RAMP = 0x20,
       AUXA_L = 0x40, AUXA_L_RAMP = 0x80, AUXA_R = 0x100, AUXA_R_RAMP = 0x200, AUXA_S = 0x400, AUXA_S_RAMP = 0x800,
       AUXB_L = 0x1000, AUXB_L_RAMP = 0x2000, AUXB_R = 0x4000, AUXB_R_RAMP = 0x8000, AUXB_S = 0x10000, AUXB_S_RAMP = 0x20000, ALL_RAMPS = 0xAAAAAA };
static u32 mixctl(u32 mc) {
    u32 r = 0;
    if (mc & 0x10) { r |= MAIN_L | MAIN_R; if ((mc & 6) == 0) r |= AUXB_L | AUXB_R; if ((mc & 7) == 1) r |= AUXA_L | AUXA_R | AUXA_S; }
    else {
        r |= MAIN_L | MAIN_R;
        if (mc & 1) r |= AUXA_L | AUXA_R;
        if (mc & 2) r |= AUXB_L | AUXB_R;
        if (mc & 4) { r |= MAIN_S; if (r & AUXA_L) r |= AUXA_S; if (r & AUXB_L) r |= AUXB_S; }
    }
    if (mc & 8) r |= ALL_RAMPS;
    return r;
}
static void mixadd(int* out, const s16* in, u32 n, VolumeData* vd, s16* dpop, int ramp) {
    u16 vol = vd->volume, d = ramp ? vd->volume_delta : 0;
    for (u32 i = 0; i < n; i++) { s64 s = (s64)in[i] * vol; s >>= 15; s16 s16v = clamp16((s32)s); out[i] += s16v; vol += d; *dpop = s16v; }
    vd->volume = vol;
}
static void process_voice(AXPB* pb, int off, u32 mctl) {
    if (pb->running != 1) return;
    s16 smp[32]; Acc acc; acc_setup(&acc, pb);
    const s16* pc = g_have_coeffs ? g_coeffs + pb->coef_select * 0x200 : 0;
    u32 pos = resample(&acc, pb->coefs, smp, 32, pb->last_samples, pb->cur_addr_frac, (u32)pb->ratio_hi << 16 | pb->ratio_lo, pb->src_type, pc);
    pb->cur_addr_frac = pos & 0xFFFF;
    pb->cur_addr_hi = (u16)(acc.cur >> 16); pb->cur_addr_lo = (u16)acc.cur;
    pb->yn1 = acc.yn1; pb->yn2 = acc.yn2; pb->pred_scale = acc.pred_scale;
    for (int i = 0; i < 32; i++) { s32 v = (s16)pb->cur_volume; smp[i] = clamp16(((s32)smp[i] * v) >> 15); pb->cur_volume += pb->cur_volume_delta; }
    #define MIX(flag, buf, vd, dp) if (mctl & flag) mixadd(buf + off, smp, 32, &pb->vd, &pb->dpop[dp], (mctl & (flag << 1)) != 0)
    MIX(MAIN_L, s_main[0], main_l, 0); MIX(MAIN_R, s_main[1], main_r, 3); MIX(MAIN_S, s_main[2], main_s, 6);
    MIX(AUXA_L, s_auxa[0], auxa_l, 1); MIX(AUXA_R, s_auxa[1], auxa_r, 4); MIX(AUXA_S, s_auxa[2], auxa_s, 7);
    MIX(AUXB_L, s_auxb[0], auxb_l, 2); MIX(AUXB_R, s_auxb[1], auxb_r, 5); MIX(AUXB_S, s_auxb[2], auxb_s, 8);
}
static void process_pb_list(u32 pb_addr) {
    int guard = 0;
    while (pb_addr && guard++ < 256) {
        AXPB pb; read_pb(pb_addr, &pb);
        u32 ua = (u32)pb.upd_hi << 16 | pb.upd_lo; u16 upd[64];
        for (int i = 0; i < 64; i++) upd[i] = R16(ua + 2 * i);
        u32 mctl = mixctl(pb.mixer_control);
        u32 start = 0;
        for (int ms = 0; ms < 5; ms++) {
            u16 cnt = pb.num_updates[ms];
            if (start + cnt <= 32) for (u32 i = start; i < start + cnt; i++) { u16 o = upd[2 * i], v = upd[2 * i + 1]; if (o < PBW) ((u16*)&pb)[o >= LPF_OFF ? o + (LC_OFF - LPF_OFF) : o] = v; }
            start += cnt;
            mctl = mixctl(pb.mixer_control);
            process_voice(&pb, ms * 32, mctl);
        }
        write_pb(pb_addr, &pb);
        pb_addr = (u32)pb.next_pb_hi << 16 | pb.next_pb_lo;
    }
}

/* ---------------- command list ---------------- */
static void setup_processing(u32 a) {
    int* bufs[9] = {s_main[0], s_main[1], s_main[2], s_auxa[0], s_auxa[1], s_auxa[2], s_auxb[0], s_auxb[1], s_auxb[2]};
    for (int i = 0; i < 9; i++) {
        s32 v = (s32)((u32)R16(a + 6 * i) << 16 | R16(a + 6 * i + 2)); s16 d = (s16)R16(a + 6 * i + 4);
        for (int j = 0; j < 160; j++) bufs[i][j] = v ? v + j * d : 0;
    }
}
static void up32(u32 addr, const int* src, int n) { for (int i = 0; i < n; i++) W32(addr + 4 * i, (u32)src[i]); }
static void mix_aux(int id, u32 wr, u32 rd) {
    int (*b)[160] = id == 0 ? s_auxa : s_auxb;
    if (wr) for (int k = 0; k < 3; k++) { up32(wr, b[k], 160); wr += 640; }
    for (int k = 0; k < 3; k++) for (int i = 0; i < 160; i++) s_main[k][i] += (s32)R32(rd + 640 * k + 4 * i);
}
static s16* g_out_hook;
void ai_feed(const s16* lr_interleaved_be_rl, int frames);   /* host output */
static void output(u32 lr, u32 surr) {
    up32(surr, s_main[2], 160);
    for (int i = 0; i < 160; i++) {
        s16 l = clamp16(s_main[0][i]), r = clamp16(s_main[1][i]);
        W16(lr + 4 * i, (u16)r); W16(lr + 4 * i + 2, (u16)l);
    }
}
static u16 g_cmd[512];
static void handle_cmdlist(void) {
    u32 i = 0, pb_addr = 0; int end = 0, guard = 0;
    #define HL() ((u32)g_cmd[i] << 16 | g_cmd[i + 1])
    while (!end && guard++ < 1000 && i < 500) {
        u16 c = g_cmd[i++];
        switch (c) {
        case 0x00: setup_processing(HL()); i += 2; break;
        case 0x01: {
            u32 a = HL(); i += 2; u16 v[3] = {g_cmd[i], g_cmd[i + 1], g_cmd[i + 2]}; i += 3;
            int (*bb[3])[160] = {s_main, s_auxa, s_auxb};
            for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) for (int s = 0; s < 160; s++) { s64 x = (s32)R32(a); a += 4; bb[k][j][s] += (s32)((x * v[k]) >> 15); }
            break; }
        case 0x02: pb_addr = HL(); i += 2; break;
        case 0x03: process_pb_list(pb_addr); break;
        case 0x04: case 0x05: { u32 w = HL(); i += 2; u32 r = HL(); i += 2; mix_aux(c - 4, w, r); break; }
        case 0x06: { u32 a = HL(); i += 2; for (int k = 0; k < 3; k++) up32(a + 640 * k, s_main[k], 160); break; }
        case 0x07: { u32 a = HL(); i += 2; for (int s = 0; s < 160; s++) { s32 v = (s32)R32(a + 4 * s); s_main[0][s] = v; s_main[1][s] = v; s_main[2][s] = 0; } break; }
        case 0x08: i += 10; break;
        case 0x09: { u32 r = HL(); i += 2; mix_aux(1, 0, r); break; }
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: { u32 a = HL(); i += 2; u16 sz = g_cmd[i++]; if (sz < 512) for (u32 k = 0; k < sz; k++) g_cmd[k] = R16(a + 2 * k); i = 0; break; }
        case 0x0E: { u32 surr = HL(); i += 2; u32 lr = HL(); i += 2; output(lr, surr); break; }
        case 0x0F: end = 1; break;
        case 0x10: { u32 ul = HL(); i += 2; u32 dl = HL(); i += 2;
            up32(ul, s_auxb[0], 160); up32(ul + 640, s_auxb[1], 160);
            for (int s = 0; s < 160; s++) { s32 v = (s32)R32(dl + 4 * s); s_auxb[0][s] = v; s_main[0][s] += v; }
            for (int s = 0; s < 160; s++) { s32 v = (s32)R32(dl + 640 + 4 * s); s_auxb[1][s] = v; s_main[1][s] += v; }
            break; }
        case 0x11: { u32 a = HL(); i += 2; for (int s = 0; s < 160; s++) { s32 v = (s32)R32(a + 4 * s); s_main[0][s] = -v; s_main[1][s] = v; s_main[2][s] = 0; } break; }
        default: { static int n; if (n++ < 10) rt_log("AX: unknown command %04X\n", c); end = 1; break; }
        }
    }
}

/* ---------------- mail protocol ---------------- */
static int g_state, g_cmdsize, g_booted;
u64 g_ax_frames;
void dsp_mail_from_cpu(u32 mail) {
    if (!g_booted) { g_booted = 1; }
    { static int n; if (n++ < 30) rt_log("AX mail %08X state %d\n", mail, g_state); }
    switch (g_state) {
    case 0:
        if ((mail & 0xFFFF0000u) == 0xBABE0000u) { g_cmdsize = mail & 0xFFFF; g_state = 1; }
        else { static int n; if (n++ < 10) rt_log("AX: unexpected mail %08X\n", mail); }
        break;
    case 1:
        if (g_cmdsize < 512) for (int k = 0; k < g_cmdsize; k++) g_cmd[k] = R16(mail + 2 * k);
        { static int n; if (n++ < 3) { rt_log("AX cmdlist (%d words):", g_cmdsize); for (int k = 0; k < g_cmdsize && k < 40; k++) rt_log(" %04X", g_cmd[k]); rt_log("\n"); } }
        handle_cmdlist(); g_ax_frames++;
        dsp_send(0xDCD10002, 1);        /* DSP_YIELD */
        g_state = 2;
        break;
    case 2:
        if ((mail & 0xFFFF) == 0x0000) { dsp_send(0xDCD10001, 1); g_state = 0; }     /* resume */
        else if ((mail & 0xFFFF) == 0x0003) g_state = 0;                             /* continue */
        else { g_state = 0; }
        break;
    }
}

/* ---------------- AI DMA + host output ---------------- */
static u32 g_dma_addr, g_dma_len; static int g_dma_on; static u64 g_dma_next;
static FILE* g_wav; static u32 g_wav_bytes;
void ai_dma_control(u32 start, u16 ctl, u32 aicr) {
    int on = (ctl >> 15) & 1;
    if (on && !g_dma_on) { g_dma_next = host_tb(); }
    g_dma_on = on; g_dma_addr = start; g_dma_len = (ctl & 0x7FFF) * 32;
}
WEAK void host_audio_write(const s16* stereo, int frames, int rate) {
    /* default: optional WAV capture (SOA_WAV=path) */
    static int init; if (!init) { init = 1; const char* p = getenv("SOA_WAV"); if (p) { g_wav = fopen(p, "wb"); if (g_wav) { u8 h[44] = {0}; fwrite(h, 1, 44, g_wav); } } }
    if (!g_wav) return;
    fwrite(stereo, 4, frames, g_wav); g_wav_bytes += frames * 4;
    u8 h[44]; memcpy(h, "RIFF", 4); u32 v = 36 + g_wav_bytes; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); u16 s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2); v = rate; memcpy(h + 24, &v, 4);
    v = rate * 4; memcpy(h + 28, &v, 4); s = 4; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2); memcpy(h + 36, "data", 4); v = g_wav_bytes; memcpy(h + 40, &v, 4);
    long pos = ftell(g_wav); fseek(g_wav, 0, SEEK_SET); fwrite(h, 1, 44, g_wav); fseek(g_wav, pos, SEEK_SET);
}
/* called from hw_tick: plays one DMA block per block-duration and raises AIDINT */
int ai_dma_tick(int rate, u16* csr) {
    if (!g_dma_on || !g_dma_len) return 0;
    u64 now = host_tb();
    if ((s64)(now - g_dma_next) < 0) return 0;
    u32 frames = g_dma_len / 4;
    static s16 buf[8192];
    if (frames > 4096) frames = 4096;
    for (u32 i = 0; i < frames; i++) {
        u32 a = (g_dma_addr + 4 * i) & 0x01FFFFFF;
        s16 r = (s16)(g_ram[a] << 8 | g_ram[a + 1]), l = (s16)(g_ram[a + 2] << 8 | g_ram[a + 3]);
        buf[2 * i] = l; buf[2 * i + 1] = r;
    }
    { extern void dcsnd_mix(s16*, u32, int); extern u64 g_pf_snd; u64 t = host_tb(); dcsnd_mix(buf, frames, rate); g_pf_snd += host_tb() - t; }
    host_audio_write(buf, frames, rate);
    g_dma_next += (u64)frames * 40500000ull / rate;
    if ((s64)(now - g_dma_next) > 40500000 / 10) g_dma_next = now;   /* don't spiral after stalls */
    *csr |= 0x08;    /* AIDINT */
    return 1;
}
void ax_load_coeffs(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return;
    u8 raw[0x1000]; if (fread(raw, 1, 0x1000, f) == 0x1000) { for (int i = 0; i < 0x800; i++) g_coeffs[i] = (s16)(raw[2 * i] << 8 | raw[2 * i + 1]); g_have_coeffs = 1; }
    fclose(f);
}
