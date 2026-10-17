/* hw.c - hardware register model: VI, PI, DI (disc), EXI, SI, AI/DSP, CP/PE + GX FIFO parsing */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_log(const char* fmt, ...);
void rt_fatal(const char* fmt, ...);
const char* name_of(u32 a);
u64 host_tb(void);
void fifo_write(u32 v, int size);
#define WHO (g_ctx ? g_ctx->lr : 0), name_of(g_ctx ? g_ctx->lr : 0)

static u16 g_r16[0x10000 / 2];        /* 0xCC00xxxx viewed as 16-bit big-endian registers */
static u16 g_mmio_cnt[0x10000];
#define LOGOK(a) (g_mmio_cnt[(a) & 0xFFFF]++ < (((a) & 0xFFF0) == 0x6000 ? 60 : ((a) & 0xFFF0) == 0x5000 ? 80 : 6))
#define R16REG(off) g_r16[((off) & 0xFFFF) >> 1]
static u32 r32reg(u32 off) { return (u32)R16REG(off) << 16 | R16REG(off + 2); }
static void w32reg(u32 off, u32 v) { R16REG(off) = (u16)(v >> 16); R16REG(off + 2) = (u16)v; }
u32 hw_reg32(u32 off) { return r32reg(off); }
void hw_reg32_set(u32 off, u32 v) { w32reg(off, v); }
u16 hw_reg16(u32 off) { return R16REG(off); }

/* ======================= disc ======================= */
typedef struct { u32 off, size; char* path; } DiscFile;
static DiscFile* g_files; static int g_nfiles;
static char g_root[1024];
static u8 g_boot[0x440], g_bi2[0x2000];
static u32 g_dol_off, g_fst_off, g_fst_size, g_app_size;

static void disc_add(u32 off, u32 size, const char* path) {
    g_files = realloc(g_files, sizeof(DiscFile) * (g_nfiles + 1));
    g_files[g_nfiles].off = off; g_files[g_nfiles].size = size; g_files[g_nfiles].path = strdup(path); g_nfiles++;
}
static int cmp_file(const void* a, const void* b) { u32 x = ((const DiscFile*)a)->off, y = ((const DiscFile*)b)->off; return x < y ? -1 : x > y; }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static long fsize(const char* p) { FILE* f = fopen(p, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }

static void fst_walk(const u8* fst, u32 count, const char* strs, u32 first, u32 last, const char* dir) {
    for (u32 i = first; i < last; ) {
        const u8* e = fst + i * 12;
        u32 nameoff = be32(e) & 0xFFFFFF; int isdir = e[0];
        char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, strs + nameoff);
        if (isdir) { u32 next = be32(e + 8); fst_walk(fst, count, strs, i + 1, next, path); i = next; }
        else { disc_add(be32(e + 4), be32(e + 8), path); i++; }
    }
}

void load_fst(const char* root) {
    snprintf(g_root, sizeof g_root, "%s", root);
    char p[1024]; FILE* f;
    snprintf(p, sizeof p, "%s/sys/boot.bin", root); f = fopen(p, "rb"); if (!f || fread(g_boot, 1, 0x440, f) != 0x440) rt_fatal("boot.bin\n"); fclose(f);
    snprintf(p, sizeof p, "%s/sys/bi2.bin", root); f = fopen(p, "rb"); if (!f || fread(g_bi2, 1, 0x2000, f) != 0x2000) rt_fatal("bi2.bin\n"); fclose(f);
    g_dol_off = be32(g_boot + 0x420); g_fst_off = be32(g_boot + 0x424); g_fst_size = be32(g_boot + 0x428);
    snprintf(p, sizeof p, "%s/sys/apploader.img", root); g_app_size = (u32)fsize(p);
    snprintf(p, sizeof p, "%s/sys/fst.bin", root);
    f = fopen(p, "rb"); if (!f) rt_fatal("cannot open %s\n", p);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u32 sz = (u32)((n + 31) & ~31);
    u32 addr = 0x81800000u - sz;
    u8* fst = g_ram + (addr & 0x01FFFFFF);
    if (fread(fst, 1, n, f) != (size_t)n) rt_fatal("read fst\n");
    fclose(f);
    W32(0x80000038, addr); W32(0x8000003C, sz); W32(0x80000034, addr);
    u32 count = be32(fst + 8);
    char fdir[1024]; snprintf(fdir, sizeof fdir, "%s/files", root);
    fst_walk(fst, count, (const char*)fst + count * 12, 1, count, fdir);
    qsort(g_files, g_nfiles, sizeof(DiscFile), cmp_file);
    memcpy(g_ram, g_boot, 0x20);   /* disc header at 0x80000000 */
    rt_log("fst at %08X size %X, %d files, dol at %X, fst at %X\n", addr, (u32)n, g_nfiles, g_dol_off, g_fst_off);
}

/* read len bytes from disc offset into guest RAM */
static void disc_read(u32 dst, u32 off, u32 len) {
    u8* out = g_ram + (dst & 0x01FFFFFF);
    if ((dst & 0x01FFFFFF) + len > RAM_SIZE) rt_fatal("disc read overflow %08X+%X\n", dst, len);
    memset(out, 0, len);
    for (u32 i = 0; i < len; ) {
        u32 o = off + i; u32 n = len - i; const char* path = 0; u32 base = 0; u32 fsz = 0; const u8* mem = 0;
        char tmp[1100];
        if (o < 0x440) { mem = g_boot; base = 0; fsz = 0x440; }
        else if (o < 0x2440) { mem = g_bi2; base = 0x440; fsz = 0x2000; }
        else if (o < 0x2440 + g_app_size) { snprintf(tmp, sizeof tmp, "%s/sys/apploader.img", g_root); path = tmp; base = 0x2440; fsz = g_app_size; }
        else if (o >= g_fst_off && o < g_fst_off + g_fst_size) { snprintf(tmp, sizeof tmp, "%s/sys/fst.bin", g_root); path = tmp; base = g_fst_off; fsz = g_fst_size; }
        else if (o >= g_dol_off && o < g_fst_off) { snprintf(tmp, sizeof tmp, "%s/sys/main.dol", g_root); path = tmp; base = g_dol_off; fsz = g_fst_off - g_dol_off; }
        else {
            int lo = 0, hi = g_nfiles - 1, k = -1;
            while (lo <= hi) { int m = (lo + hi) / 2; if (g_files[m].off <= o) { k = m; lo = m + 1; } else hi = m - 1; }
            if (k >= 0 && o < g_files[k].off + g_files[k].size) { path = g_files[k].path; base = g_files[k].off; fsz = g_files[k].size; }
            else {  /* gap: zeros up to next file */
                u32 next = (k + 1 < g_nfiles) ? g_files[k + 1].off : 0xFFFFFFFF;
                u32 skip = next - o; if (skip > n) skip = n; i += skip; continue;
            }
        }
        u32 avail = base + fsz - o; if (n > avail) n = avail;
        if (mem) memcpy(out + i, mem + (o - base), n);
        else { FILE* f = fopen(path, "rb"); if (!f) rt_fatal("disc: cannot open %s\n", path); fseek(f, o - base, SEEK_SET); size_t got = fread(out + i, 1, n, f); (void)got; fclose(f); }
        i += n;
    }
}

/* ======================= DI (disc interface) ======================= */
/* Commands complete asynchronously (TSTART stays set, then TCINT), as on hardware: the data is
   copied at once but the interrupt arrives after a short delay (0.2 ms + ~40 MB/s), so the DVD
   library never sees a transfer finish inside the register write that started it. SOA_DIFAST=1: instant. */
u64 host_tb(void);
static int g_di_pending; static u64 g_di_done_at;
static void di_complete(void) { w32reg(0x601C, r32reg(0x601C) & ~1u); w32reg(0x6000, r32reg(0x6000) | 0x10); }
static void di_exec(void) {
    { extern u32 g_tex_epoch; g_tex_epoch++; }
    u32 cmd = r32reg(0x6008), a1 = r32reg(0x600C), a2 = r32reg(0x6010);
    u32 mar = r32reg(0x6014), len = r32reg(0x6018);
    u32 op = cmd >> 24;
    { static int n; if (n++ < 400) rt_log("DI cmd %08X %08X %08X mar %08X len %X cr %08X\n", cmd, a1, a2, mar, len, r32reg(0x601C)); }
    switch (op) {
    case 0xA8:
        if ((cmd & 0xFF) == 0x40) disc_read(mar, 0, 0x20);          /* read disc ID */
        else disc_read(mar, a1 << 2, len);
        rt_log("DI read %08X <- disc %08X len %X\n", mar, a1 << 2, len);
        w32reg(0x6018, 0);
        break;
    case 0x12: { u8* p = g_ram + (mar & 0x01FFFFFF); memset(p, 0, 0x20); p[2] = 0x00; p[3] = 0x02; p[4] = 0x20; p[5] = 0x01; p[6] = 0x06; p[7] = 0x10; w32reg(0x6018, 0); break; }
    case 0xE0: w32reg(0x6020, 0); break;
    case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xAB: w32reg(0x6020, 0); break;
    default: rt_log("DI: unknown command %08X %08X %08X\n", cmd, a1, a2); break;
    }
    static int fast = -1; if (fast < 0) fast = getenv("SOA_DIFAST") != 0;
    if (fast) { di_complete(); return; }
    u32 bytes = (op == 0xA8) ? len : 32;
    g_di_done_at = host_tb() + 40500000ull * 2 / 10000 + 40500000ull * bytes / 40000000ull;
    g_di_pending = 1;
}

/* ======================= VI ======================= */
/* field rate from the VI vertical timing register: ~287 active lines per field = 50 Hz PAL, ~240 = 60 Hz (NTSC/EuRGB60) */
int vi_refresh_hz(void) { u32 acv = (R16REG(0x2000) >> 4) & 0x3FF; return acv == 0 ? 50 : acv > 260 ? 50 : 60; }
void vi_retrace_event(void) {
    { u32 en = (r32reg(0x6430) >> 4) & 0xF;   /* EN0 = bit 7 ... EN3 = bit 4 */
      if (en) {
        w32reg(0x6434, r32reg(0x6434) | 0x10000000u);   /* SI poll complete -> RDSTINT */
        u32 sr = r32reg(0x6438);
        for (int ch = 0; ch < 4; ch++) if (en & (8 >> ch)) sr |= ch == 0 ? (0x20000000u >> (8 * ch)) : (0x08000000u >> (8 * ch));   /* RDST on port 1, NOREP elsewhere */
        w32reg(0x6438, sr);
      } }
    for (int i = 0; i < 2; i++) {
        u32 off = 0x2030 + i * 4;
        if (R16REG(off) & 0x1000) R16REG(off) |= 0x8000;
    }
}
static u16 vi_current_line(void) {
    u64 t = host_tb() % (40500000ull / 50);
    return (u16)(1 + t * 313 / (40500000ull / 50));
}

/* ======================= EXI ======================= */
static u8 g_sram[64];
typedef struct { int dev; int have_cmd; u32 cmd; u32 pos; } ExiState;
static ExiState g_exi[3];
static void sram_init(void) {
    static int done; if (done) return; done = 1;
    memset(g_sram, 0, 64);
    g_sram[0x12] = 0;  g_sram[0x13] = 0x2C;
    { extern int settings_get(const char*, int); if (settings_get("pal60", 1)) g_sram[0x11] |= 0x40; }   /* NTD bit 6: EuRGB60 (60 Hz) available, the game then offers 60 Hz */
    u16 c = 0, ci = 0;
    for (int i = 12; i < 20; i += 2) { u16 w = (u16)(g_sram[i] << 8 | g_sram[i + 1]); c += w; ci += (u16)~w; }
    g_sram[0] = c >> 8; g_sram[1] = (u8)c; g_sram[2] = ci >> 8; g_sram[3] = (u8)ci;
}
/* ---- memory card in slot A (chn0 dev0), ported from Dolphin's CEXIMemoryCard; backed by a raw card image ---- */
static FILE* g_mc_f; static u8* g_mc; static u32 g_mc_size; static int g_mc_on;
static u8 g_mc_status = 0x40 | 0x01;   /* UNLOCKED | READY: the CARD lib then skips the DSP unlock */
static u8 g_mc_cmd, g_mc_irqsw, g_mc_irq; static u32 g_mc_pos, g_mc_addr; static u8 g_mc_prog[128];
static int g_mc_done_pending; static u64 g_mc_done_at;
static void mc_flash_id_from_header(void) {
    /* the CARD lib checks the SRAM flash ID against the card header serial; derive it from the header like Dolphin */
    u64 rnd = 0; for (int i = 0; i < 8; i++) rnd = rnd << 8 | g_mc[12 + i];
    u8 sum = 0;
    for (int i = 0; i < 12; i++) {
        rnd = (rnd * 0x41c64e6dull + 0x3039) >> 16;
        u8 id = (u8)(g_mc[i] - (u8)rnd); g_sram[0x14 + i] = id; sum += id;
        rnd = ((rnd * 0x41c64e6dull + 0x3039) >> 16) & 0x7FFF;
    }
    g_sram[0x3A] = sum ^ 0xFF;
}
/* GameCube card checksums: sum of 16-bit words and of their complements (0xFFFF -> 0) */
static void mc_csum(const u8* p, u32 n, u8* out) {
    u32 a = 0, b = 0;
    for (u32 i = 0; i + 1 < n; i += 2) { u32 w = (u32)p[i] << 8 | p[i + 1]; a = (a + w) & 0xFFFF; b = (b + (w ^ 0xFFFF)) & 0xFFFF; }
    if (a == 0xFFFF) a = 0; if (b == 0xFFFF) b = 0;
    out[0] = a >> 8; out[1] = (u8)a; out[2] = b >> 8; out[3] = (u8)b;
}
/* write a freshly formatted, empty card (as CARDFormat / tools/mkcard.py): header, two directories, two BATs */
static void mc_format_blank(u8* c, u32 size) {
    u32 nblk = size / 0x2000, mb = size / 0x20000;
    memset(c, 0xFF, size);
    memset(c, 0, 0x200);
    u64 t = host_tb() & 0xFFFFFFFFFFFFull, rnd = t;
    for (int i = 0; i < 12; i++) {   /* serial = flash ID 0 + LCG, as CARDFormat */
        rnd = ((rnd * 0x41C64E6Dull + 0x3039) >> 16); c[i] = (u8)rnd;
        rnd = ((rnd * 0x41C64E6Dull + 0x3039) >> 16) & 0x7FFF;
    }
    for (int i = 0; i < 8; i++) c[12 + i] = (u8)(t >> (56 - 8 * i));
    c[0x22] = (u8)(mb >> 8); c[0x23] = (u8)mb;   /* size in Mbit; encoding 0 = ANSI */
    memset(c + 0x26, 0xFF, 0x1FC - 0x26);
    mc_csum(c, 0x1FC, c + 0x1FC);
    for (int d = 1; d <= 2; d++) { u8* p = c + d * 0x2000; p[0x1FFA] = 0; p[0x1FFB] = (u8)(d - 1); mc_csum(p, 0x1FFC, p + 0x1FFC); }
    for (int d = 3; d <= 4; d++) {
        u8* p = c + d * 0x2000; memset(p, 0, 0x2000);
        p[5] = (u8)(d - 3); u32 fr = nblk - 5; p[6] = (u8)(fr >> 8); p[7] = (u8)fr; p[8] = 0; p[9] = 4;
        mc_csum(p + 4, 0x2000 - 4, p);
    }
}
static void mc_init(void) {
    static int done; if (done) return; done = 1;
    const char* path = getenv("SOA_CARD"); if (!path) path = "MemoryCardA.raw";
    g_mc_f = fopen(path, "r+b");
    if (!g_mc_f) {   /* new 251-block card, created already formatted (as Dolphin does) */
        g_mc_f = fopen(path, "w+b");
        if (!g_mc_f) { rt_log("memcard: cannot create %s\n", path); return; }
        g_mc_size = 0x10 * 0x20000; g_mc = calloc(1, g_mc_size);
        mc_format_blank(g_mc, g_mc_size);
        fwrite(g_mc, 1, g_mc_size, g_mc_f); fflush(g_mc_f);
    } else {
        fseek(g_mc_f, 0, SEEK_END); long sz = ftell(g_mc_f); fseek(g_mc_f, 0, SEEK_SET);
        if (sz < 0x80000) sz = 0x80000; g_mc_size = (u32)sz; g_mc = calloc(1, g_mc_size);
        if (fread(g_mc, 1, g_mc_size, g_mc_f) != g_mc_size) rt_log("memcard: short read\n");
    }
    g_mc_on = 1;
    mc_flash_id_from_header();
    rt_log("memcard: %s, %u blocks\n", path, g_mc_size / 8192 - 5);
}
static void mc_flush(u32 off, u32 n) { if (g_mc_f) { fseek(g_mc_f, off, SEEK_SET); fwrite(g_mc + off, 1, n, g_mc_f); fflush(g_mc_f); } }
static u32 mc_card_id(void) { return g_mc_size / 0x20000; }   /* size code: 4 = 59 blocks, 0x10 = 251, 0x80 = 2043 */
static u8 mc_xfer(u8 b) {
    if (g_mc_pos == 0) {
        g_mc_cmd = b; g_mc_pos = 1;
        { static int n; if (getenv("SOA_MCLOG") && n++ < 400) rt_log("memcard cmd %02X\n", b); }
        if (b == 0x89) { g_mc_status &= ~0x18; g_mc_status |= 0x01; g_mc_irq = 0; w32reg(0x6800, r32reg(0x6800) & ~2u); g_mc_pos = 0; }
        return 0xFF;
    }
    u32 p = g_mc_pos++; u8 out = 0xFF;
    switch (g_mc_cmd) {
    case 0x00: out = p == 1 ? 0x80 : (u8)(mc_card_id() >> (24 - ((p - 2) & 3) * 8)); break;   /* NintendoID */
    case 0x83: out = g_mc_status; break;   /* every byte after the command (as Dolphin) */                                     /* ReadStatus */
    case 0x85: out = p == 1 ? 0xC2 : p == 2 ? 0x21 : 0xFF; break;                             /* ReadID */
    case 0x81: if (p == 1) g_mc_irqsw = b; break;                                             /* SetInterrupt */
    case 0xF1:                                                                                /* SectorErase */
        if (p == 1) g_mc_addr = (u32)(b & 0x7F) << 17; else if (p == 2) g_mc_addr |= (u32)b << 9; break;
    case 0x52: case 0xF2:                                                                     /* ReadArray / PageProgram */
        if (p == 1) g_mc_addr = (u32)(b & 0x7F) << 17;
        else if (p == 2) g_mc_addr |= (u32)b << 9;
        else if (p == 3) g_mc_addr |= (u32)(b & 3) << 7;
        else if (p == 4) g_mc_addr |= b & 0x7F;
        else if (g_mc_cmd == 0xF2) g_mc_prog[(p - 5) & 0x7F] = b;
        else if (p >= 9) { out = g_mc[g_mc_addr % g_mc_size]; g_mc_addr = (g_mc_addr & ~0x1FFu) | ((g_mc_addr + 1) & 0x1FF); }
        break;
    case 0xF4: break;                                                                         /* ChipErase (on deselect) */
    }
    return out;
}
static void mc_cmd_done_later(void) { g_mc_status |= 0x80; g_mc_status &= ~0x01; g_mc_done_pending = 1; g_mc_done_at = host_tb() + 40500000ull / 4000; }
static void mc_deselect(void) {
    if (g_mc_pos == 0) return;
    { static int lg = -1, n; if (lg < 0) lg = getenv("SOA_MCLOG") != 0; if (lg && n++ < 400) rt_log("mc cmd %02X len %u addr %06X status %02X irqsw %d\n", g_mc_cmd, g_mc_pos, g_mc_addr, g_mc_status, g_mc_irqsw); }
    if (g_mc_cmd == 0xF1 && g_mc_pos >= 3) {
        u32 a = (g_mc_addr % g_mc_size) & ~0x1FFFu; memset(g_mc + a, 0xFF, 0x2000); mc_flush(a, 0x2000); mc_cmd_done_later();
    } else if (g_mc_cmd == 0xF2 && g_mc_pos >= 5) {
        u32 n = g_mc_pos - 5; if (n > 128) n = 128; u32 a = g_mc_addr % g_mc_size;
        for (u32 i = 0; i < n; i++) g_mc[(a & ~0x7Fu) | ((a + i) & 0x7F)] = g_mc_prog[i];
        mc_flush(a & ~0x7Fu, 128); mc_cmd_done_later();
    } else if (g_mc_cmd == 0xF4 && g_mc_pos >= 3) {
        memset(g_mc, 0xFF, g_mc_size); mc_flush(0, g_mc_size); mc_cmd_done_later();
    }
    g_mc_pos = 0;
}
static void mc_tick(void) {
    if (g_mc_done_pending && (s64)(host_tb() - g_mc_done_at) >= 0) {
        g_mc_done_pending = 0; g_mc_status = (g_mc_status | 0x01) & ~0x80; g_mc_irq = 1;
        if (g_mc_irqsw) w32reg(0x6800, r32reg(0x6800) | 2u);   /* EXIINT on channel 0 */
    }
}
static int is_card(int chn, ExiState* st) { return chn == 0 && st->dev == 0 && g_mc_on; }

static u32 sram_off(u32 cmd) { return (((cmd & 0x7FFFFFFFu) - 0x20000100u) >> 6) & 63; }
static int is_sram_cmd(u32 cmd) { u32 c = cmd & 0x7FFFFFFFu; return c >= 0x20000100u && c < 0x20001100u; }
static u32 exi_dev_read(int chn, ExiState* st, int n) {
    u32 v = 0;
    if (is_card(chn, st)) { for (int i = 0; i < n; i++) v |= (u32)mc_xfer(0) << (24 - 8 * i); return v; }
    if (chn == 0 && st->dev == 1) {
        for (int i = 0; i < n; i++) {
            u8 b = 0;
            if (is_sram_cmd(st->cmd)) b = g_sram[(sram_off(st->cmd) + st->pos + i) & 63];
            v |= (u32)b << (24 - 8 * i);
        }
        st->pos += n;
    }
    return v;
}
static void exi_dev_write(int chn, ExiState* st, u32 v, int n) {
    if (is_card(chn, st)) { for (int i = 0; i < n; i++) mc_xfer((u8)(v >> (24 - 8 * i))); return; }
    if (!st->have_cmd) { st->cmd = v; st->have_cmd = 1; st->pos = 0; return; }
    if (chn == 0 && st->dev == 1 && is_sram_cmd(st->cmd) && (st->cmd & 0x80000000u))
        for (int i = 0; i < n; i++) g_sram[(sram_off(st->cmd) + st->pos + i) & 63] = (u8)(v >> (24 - 8 * i));
    st->pos += n;
}
static void exi_write(u32 a, u32 v) {
    int chn = ((a & 0xFF)) / 0x14; int r = ((a & 0xFF) % 0x14);
    ExiState* st = &g_exi[chn];
    u32 csro = 0x6800 + chn * 0x14;
    sram_init(); mc_init();
    if (r == 0) {
        u32 irq = (1u << 1) | (1u << 3) | (1u << 11);
        u32 cur = r32reg(csro);
        if (chn == 0 && g_mc_on) cur |= 1u << 12;   /* card inserted in slot A */
        u32 keep = cur & (irq | (1u << 12)); keep &= ~(v & irq);
        if (chn == 0 && (v & 2)) g_mc_irq = 0;
        int newdev = (v >> 7) & 7; int olddev = (cur >> 7) & 7;
        w32reg(csro, (v & ~(irq | (1u << 12))) | keep);
        if (newdev != olddev) {
            if (is_card(chn, st)) mc_deselect();
            st->have_cmd = 0; st->dev = newdev == 1 ? 0 : newdev == 2 ? 1 : newdev == 4 ? 2 : -1;
            if (is_card(chn, st)) g_mc_pos = 0;
        }
        return;
    }
    w32reg(a, v);
    if (r == 0xC && (v & 1)) {
        int dma = (v >> 1) & 1, rw = (v >> 2) & 3, len = ((v >> 4) & 3) + 1;
        if (dma) {
            u32 mar = r32reg(csro + 4), n = r32reg(csro + 8);
            for (u32 i = 0; i < n; i += 4) {
                if (rw == 0) W32(0x80000000u | (mar + i), exi_dev_read(chn, st, 4));
                else exi_dev_write(chn, st, R32(0x80000000u | (mar + i)), 4);
            }
        } else {
            u32 d = r32reg(csro + 0x10);
            if (rw == 2 && is_card(chn, st)) {
                u32 o = 0; for (int i = 0; i < len; i++) o |= (u32)mc_xfer((u8)(d >> (24 - 8 * i))) << (24 - 8 * i);
                w32reg(csro + 0x10, o);
            } else if (rw == 0) w32reg(csro + 0x10, exi_dev_read(chn, st, len));
            else exi_dev_write(chn, st, d, len);
        }
        w32reg(a, v & ~1u);
        w32reg(csro, r32reg(csro) | (1u << 3));
    }
}


/* ======================= SI (controllers) ======================= */
static int g_si_pending; static u64 g_si_done_at;
static int g_ar_pending; static u64 g_ar_done_at;
static u32 g_pad_hi = 0x00808080, g_pad_lo = 0x80800000;   /* neutral */
void si_set_pad(u32 hi, u32 lo) { g_pad_hi = hi; g_pad_lo = lo; }
u32 si_pad_hi_raw(void) { return g_pad_hi; }
int options_is_open(void);
/* what the game sees: nothing while the options screen has the controller */
static u32 pad_hi(void) { return options_is_open() ? 0x00808080u : g_pad_hi; }
static u32 pad_lo(void) { return options_is_open() ? 0x80800000u : g_pad_lo; }
static void si_transfer(u32 csr) {
    int chn = (csr >> 1) & 3;
    u8 buf[128]; for (int i = 0; i < 128; i += 2) { u16 w = R16REG(0x6480 + i); buf[i] = w >> 8; buf[i + 1] = (u8)w; }
    u8 cmd = buf[0];
    u8 out[16] = {0}; int n = 0;
    if (chn != 0) { w32reg(0x6438, r32reg(0x6438) | (8u << (24 - 8 * chn))); goto fin; }   /* NOREP on other ports */
    if (cmd == 0x00 || cmd == 0xFF) { out[0] = 0x09; out[1] = 0x00; out[2] = 0x00; n = 3; }
    else if (cmd == 0x41 || cmd == 0x42) { u8 o[10] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F, 0, 0}; memcpy(out, o, 10); n = 10; }
    else if (cmd == 0x40) { u32 h = pad_hi(), l = pad_lo(); u8 o[8] = {h >> 24, h >> 16, h >> 8, h, l >> 24, l >> 16, l >> 8, l}; memcpy(out, o, 8); n = 8; }
    for (int i = 0; i < n; i += 2) R16REG(0x6480 + i) = (u16)(out[i] << 8 | (i + 1 < n ? out[i + 1] : 0));
fin:
    g_si_done_at = host_tb() + 40500000ull / 2000;   /* ~0.5 ms per transfer */
    g_si_pending = 1;
}

/* ======================= PI interrupt status ======================= */
u32 pi_intsr(void) {
    u32 s = 0;
    u32 disr = r32reg(0x6000), dicvr = r32reg(0x6004);
    if (((disr & 0x10) && (disr & 0x08)) || ((disr & 0x04) && (disr & 0x02)) || ((disr & 0x40) && (disr & 0x20)) || ((dicvr & 4) && (dicvr & 2))) s |= 0x4;
    for (int c = 0; c < 3; c++) {
        u32 csr = r32reg(0x6800 + c * 0x14);
        if (((csr & 2) && (csr & 1)) || ((csr & 8) && (csr & 4)) || ((csr & 0x800) && (csr & 0x400))) s |= 0x10;
    }
    u32 aicr = r32reg(0x6C00); if ((aicr & 8) && (aicr & 4)) s |= 0x20;
    u16 dsp = R16REG(0x500A); if (((dsp & 0x08) && (dsp & 0x10)) || ((dsp & 0x20) && (dsp & 0x40)) || ((dsp & 0x80) && (dsp & 0x100))) s |= 0x40;
    for (int i = 0; i < 4; i++) { u16 d = R16REG(0x2030 + i * 4); if ((d & 0x8000) && (d & 0x1000)) s |= 0x100; }
    u16 pe = R16REG(0x100A); if ((pe & 0x4) && (pe & 0x1)) s |= 0x200; if ((pe & 0x8) && (pe & 0x2)) s |= 0x400;
    u32 sicsr = r32reg(0x6434); if (((sicsr & 0x80000000u) && (sicsr & 0x40000000u)) || ((sicsr & 0x10000000u) && (sicsr & 0x08000000u))) s |= 0x8;
    s |= 0x00010000;
    return s;
}

extern int g_poll_ctr;
/* PE draw-sync tokens and draw-done: the GPU reaches them some time after the CPU writes them, and the token
   register and the interrupt appear together at that moment. Games lean on this ordering: this one draws a late
   pass (the ship's sail, effects) from the token callback and copies the frame out afterwards, waiting on the
   token register in between. So the register keeps its old value until the interrupt is actually delivered,
   which the next poll does right away (g_poll_ctr = 0: the next loop back-edge). */
static int g_pe_pend_fin, g_pe_pend_tok; static u16 g_pe_tok;
void pe_finish(void) { g_pe_pend_fin = 1; g_poll_ctr = 0; }
void pe_token(u16 t) { g_pe_tok = t; g_pe_pend_tok = 2; g_poll_ctr = 0; }
void pe_token_noirq(u16 t) { g_pe_tok = t; if (!g_pe_pend_tok) g_pe_pend_tok = 1; g_poll_ctr = 0; }
void pe_commit(void) {   /* called at every poll before interrupts are delivered */
    if (g_pe_pend_tok) { R16REG(0x100E) = g_pe_tok; if (g_pe_pend_tok == 2) R16REG(0x100A) |= 0x4; g_pe_pend_tok = 0; }
    if (g_pe_pend_fin) { R16REG(0x100A) |= 0x8; g_pe_pend_fin = 0; }
}

u64 hw_pad_reads(void);
int ai_dma_tick(int rate, u16* csr);
void ai_dma_control(u32 start, u16 ctl, u32 aicr);
void hw_tick(void) {
    mc_tick();
    if (g_di_pending && (s64)(host_tb() - g_di_done_at) >= 0) { g_di_pending = 0; di_complete(); }
    if (g_ar_pending && (s64)(host_tb() - g_ar_done_at) >= 0) { g_ar_pending = 0; R16REG(0x500A) = (R16REG(0x500A) & ~0x200) | 0x20; }
    ai_dma_tick((r32reg(0x6C00) & 0x40) ? 32000 : 48000, &R16REG(0x500A));
    if (g_si_pending && (s64)(host_tb() - g_si_done_at) >= 0) { g_si_pending = 0; w32reg(0x6434, (r32reg(0x6434) & ~1u) | 0x80000000u); }
}

/* ======================= DSP mailboxes (fake DSP) ======================= */
static u32 g_dsp_out[64]; static int g_dsp_out_n;
static int g_dsp_boot_stage = -1; static u32 g_dsp_in_hi;
void dsp_send(u32 mail, int irq) {
    if (g_dsp_out_n < 64) g_dsp_out[g_dsp_out_n++] = mail;
    if (irq) R16REG(0x500A) |= 0x80;   /* DSPINT */
}
void dsp_mail_from_cpu(u32 mail);
WEAK void dsp_mail_from_cpu(u32 mail) {
    rt_log("DSP <- %08X\n", mail);
}
static void dsp_cpu_mail(u32 mail) {
    if (g_dsp_boot_stage >= 0) {           /* IPL-style task boot protocol */
        g_dsp_boot_stage++;
        if (g_dsp_boot_stage == 10) { g_dsp_boot_stage = -1; rt_log("DSP task booted\n"); dsp_send(0xDCD10000, 1); }
        return;
    }
    dsp_mail_from_cpu(mail);
}
static void dsp_reset(void) { g_dsp_out_n = 0; g_dsp_boot_stage = 0; dsp_send(0x8071FEED, 0); }

/* ======================= ARAM (16 MB audio RAM) ======================= */
u8* g_aram;
#define ARAM_SIZE 0x01000000u
static void aram_dma(void) {
    if (!g_aram) g_aram = calloc(1, ARAM_SIZE);
    u32 mm = ((u32)(R16REG(0x5020) & 0x3FF) << 16 | (R16REG(0x5022) & 0xFFE0)) & 0x01FFFFFF;
    u32 ar = (u32)(R16REG(0x5024) & 0x3FF) << 16 | (R16REG(0x5026) & 0xFFE0);
    u32 len = (u32)(R16REG(0x5028) & 0x3FF) << 16 | (R16REG(0x502A) & 0xFFE0);
    int to_mram = (R16REG(0x5028) >> 15) & 1;
    for (u32 i = 0; i < len; i++) {
        u32 a = ar + i, m = mm + i;
        if (m >= RAM_SIZE) break;
        if (to_mram) g_ram[m] = a < ARAM_SIZE ? g_aram[a] : 0;
        else if (a < ARAM_SIZE) g_aram[a] = g_ram[m];
    }
    /* completion after a realistic delay (~80 MB/s), with the DMA-busy bit set meanwhile */
    g_ar_done_at = host_tb() + 40500000ull * (len + 64) / 80000000ull + 40;
    g_ar_pending = 1; R16REG(0x500A) |= 0x200; { extern u32 g_tex_epoch; g_tex_epoch++; }
}

/* ======================= access dispatch ======================= */
u32 mmio_read(u32 a, int size) {
    u32 off = a & 0xFFFF, v;
    if ((a & 0xFFFF0000u) != 0xCC000000u) { rt_log("read from unmapped %08X [lr %08X %s]\n", a, WHO); return 0; }
    if (off == 0x202C && size >= 2) R16REG(0x202C) = vi_current_line();
    if (off == 0x3000) { v = pi_intsr(); goto done; }
    if (off == 0x6800) { sram_init(); mc_init(); if (g_mc_on) w32reg(0x6800, r32reg(0x6800) | 0x1000u); }
    if (off == 0x5016) { v = 1; goto done; }    /* ARAM ready */
    if (off == 0x6404) { static u64 nr, np; nr++; if (g_pad_hi >> 24) { if (np++ < 5) rt_log("pad read with buttons %08X (read #%llu)\n", g_pad_hi, (unsigned long long)nr); } v = pad_hi(); w32reg(0x6434, r32reg(0x6434) & ~0x10000000u); w32reg(0x6438, r32reg(0x6438) & ~0x20000000u); goto done; }
    if (off == 0x6408) { v = pad_lo(); goto done; }
    if (off == 0x6410 || off == 0x641C || off == 0x6428) { v = 0x80000000u; w32reg(0x6434, r32reg(0x6434) & ~0x10000000u); goto done; }   /* ports 2-4: no controller */
    if (off == 0x5004 && size == 2) { v = g_dsp_out_n ? (0x8000 | (g_dsp_out[0] >> 16)) : 0; goto done; }
    if (off == 0x5006 && size == 2) { v = g_dsp_out_n ? (g_dsp_out[0] & 0xFFFF) : 0; if (g_dsp_out_n) { memmove(g_dsp_out, g_dsp_out + 1, --g_dsp_out_n * 4); } goto done; }
    if (off == 0x5004 && size == 4) { v = g_dsp_out_n ? (0x80000000u | g_dsp_out[0]) : 0; if (g_dsp_out_n) memmove(g_dsp_out, g_dsp_out + 1, --g_dsp_out_n * 4); goto done; }
    if (off == 0x5000 && size == 2) { v = 0; goto done; }   /* CPU->DSP mailbox always consumed */
    if (off == 0x6C08) { v = (u32)(host_tb() * 48000ull / 40500000ull); goto done; }   /* AI sample counter */
    if (off == 0x0000 && size == 2) { v = 0x0C | (R16REG(0) & ~0x1F); goto done; }
    if (size == 4) v = r32reg(off);
    else if (size == 2) v = R16REG(off & ~1u);
    else v = (off & 1) ? (R16REG(off & ~1u) & 0xFF) : (R16REG(off & ~1u) >> 8);
done:
    { static u32 last_a; static u64 spins; if (a == last_a) { if (++spins % 4096 == 0) { extern void rt_poll(Ctx*); rt_poll(g_ctx); } if (spins == 20000000) rt_fatal("spinning on %08X (lr %08X %s)\n", a, WHO); } else { last_a = a; spins = 0; } }
    if (LOGOK(a)) rt_log("  mmio R%d %08X -> %08X  [lr %08X %s]\n", size * 8, a, v, WHO);
    return v;
}

void mmio_write(u32 a, u32 v, int size) {
    u32 off = a & 0xFFFF;
    if ((a & 0xFFFF0000u) != 0xCC000000u) { rt_log("write to unmapped %08X <- %08X [lr %08X %s]\n", a, v, WHO); return; }
    if ((off & 0xF000) == 0x8000) { fifo_write(v, size); return; }
    if (LOGOK(a)) rt_log("  mmio W%d %08X <- %08X  [lr %08X %s]\n", size * 8, a, v, WHO);
    if (off >= 0x6800 && off < 0x683C && size == 4) { exi_write(a, v); return; }
    if (off == 0x6434 && size == 4) { u32 cur = r32reg(0x6434); u32 keep = (cur & 0x80000000u & ~(v & 0x80000000u)) | (cur & 0x10000000u); u32 nv = (v & ~0x90000000u) | keep; w32reg(0x6434, nv); if (v & 1) si_transfer(nv); return; }
    if (off == 0x6438 && size == 4) { w32reg(0x6438, r32reg(0x6438) & ~v); return; }
    if (off == 0x6000 && size == 4) { u32 cur = r32reg(0x6000); u32 irq = 0x54; u32 keep = cur & irq & ~(v & irq); w32reg(0x6000, (v & ~irq) | keep); return; }
    if (off == 0x6004 && size == 4) { u32 cur = r32reg(0x6004); u32 keep = cur & 4 & ~(v & 4); w32reg(0x6004, (v & ~4u & ~1u) | keep); return; }
    if (off == 0x100A && size == 2) { u16 cur = R16REG(0x100A); u16 keep = cur & 0xC & ~(v & 0xC); R16REG(0x100A) = (u16)((v & 0x3) | keep); return; }
    { static int n; if ((off == 0x500A || off == 0x5000 || off == 0x5002) && n < 3000 && getenv("SOA_DSPLOG")) { n++; rt_log("dspw %04X <- %04X csr %04X [lr %08X %s]\n", off, v, R16REG(0x500A), WHO); } }
    if (off == 0x5000 && size == 2) { g_dsp_in_hi = v & 0xFFFF; return; }
    if (off == 0x5002 && size == 2) { dsp_cpu_mail(g_dsp_in_hi << 16 | (v & 0xFFFF)); return; }
    if (off == 0x5000 && size == 4) { dsp_cpu_mail(v); return; }
    if (off == 0x500A && size == 2 && (((v & 1)) || ((v & 0x800) && !(R16REG(0x500A) & 0x800)))) { rt_log("DSP reset/boot (csr %04X)\n", v); dsp_reset(); }
    if (off == 0x500A && size == 2) { u16 cur = R16REG(0x500A); u16 irq = 0xA8; u16 keep = cur & irq & ~(v & irq); R16REG(0x500A) = (u16)((v & ~irq & ~1u) | keep); return; }
    if (off == 0x6C00 && size == 4) { u32 cur = r32reg(0x6C00); u32 keep = cur & 8 & ~(v & 8); w32reg(0x6C00, (v & ~8u) | keep); return; }
    if (off == 0x3000) return;
    if (size == 4) w32reg(off, v);
    else if (size == 2) R16REG(off & ~1u) = (u16)v;
    else { u16* r = &R16REG(off & ~1u); if (off & 1) *r = (*r & 0xFF00) | (v & 0xFF); else *r = (u16)((*r & 0xFF) | (v << 8)); }
    if (off == 0x601C && (v & 1)) di_exec();
    if (off == 0x502A || (off == 0x5028 && size == 4)) aram_dma();
    if (off == 0x5036 || (off == 0x5034 && size == 4)) ai_dma_control((((u32)R16REG(0x5030) & 0x3FF) << 16 | (R16REG(0x5032) & 0xFFE0)) | 0x80000000u, R16REG(0x5036), r32reg(0x6C00));
}

void bad_access(u32 a, int size, int write) { rt_fatal("bad %s%d at %08X\n", write ? "W" : "R", size * 8, a); }

/* ---------------- save states ---------------- */
#include "pc.h"
void hw_state(SS* s) {
    SSV(s, g_r16);
    if (!g_aram) g_aram = calloc(1, ARAM_SIZE);
    ss_io(s, g_aram, ARAM_SIZE);
    SSV(s, g_sram); SSV(s, g_exi);
    SSV(s, g_di_pending); ss_time(s, &g_di_done_at);
    SSV(s, g_si_pending); ss_time(s, &g_si_done_at);
    SSV(s, g_ar_pending); ss_time(s, &g_ar_done_at);
    SSV(s, g_mc_status); SSV(s, g_mc_cmd); SSV(s, g_mc_irqsw); SSV(s, g_mc_irq); SSV(s, g_mc_pos); SSV(s, g_mc_addr); SSV(s, g_mc_prog);
    SSV(s, g_mc_done_pending); ss_time(s, &g_mc_done_at);
    SSV(s, g_pe_pend_fin); SSV(s, g_pe_pend_tok); SSV(s, g_pe_tok);
    SSV(s, g_dsp_out); SSV(s, g_dsp_out_n); SSV(s, g_dsp_boot_stage); SSV(s, g_dsp_in_hi);
}
