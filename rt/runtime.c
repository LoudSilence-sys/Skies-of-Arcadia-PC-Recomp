#define _GNU_SOURCE
/* runtime.c - host side of the recompiled Skies of Arcadia Legends */
#include "rt.h"
#include "names.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/time.h>
#include <ucontext.h>
#endif
#include <unistd.h>

u8* g_ram; u8* g_lcache; Ctx* g_ctx;
static FILE* g_log;
static const char* g_dol_path;

/* ---------------- names (for logs) ---------------- */
static struct { u32 a; char n[48]; } *g_names; static int g_nnames;
static void load_names(const char* p) {
    FILE* f = fopen(p, "r"); if (!f) return;
    g_names = calloc(20000, sizeof *g_names); char line[128];
    while (fgets(line, sizeof line, f)) { u32 a; char n[64]; if (sscanf(line, "%x %63s", &a, n) == 2 && g_nnames < 20000) { g_names[g_nnames].a = a; snprintf(g_names[g_nnames].n, 48, "%s", n); g_nnames++; } }
    fclose(f);
}
const char* name_of(u32 a) {
    const char* best = "?"; u32 ba = 0;
    for (int i = 0; i < g_nnames; i++) if (g_names[i].a <= a && g_names[i].a >= ba) { ba = g_names[i].a; best = g_names[i].n; }
    return best;
}

void rt_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(g_log ? g_log : stderr, fmt, ap); va_end(ap); if (g_log) fflush(g_log); }
void rt_fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); fprintf(g_log, "FATAL: "); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
#ifdef _WIN32
    { extern int __stdcall MessageBoxA(void*, const char*, const char*, unsigned); char b[2048]; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); MessageBoxA(0, b, "Skies of Arcadia - error", 0x10); }
#endif
    exit(1);
}

/* ---------------- function table ---------------- */
GuestFn lookup_fn(u32 a) {
    int lo = 0, hi = g_func_count - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_func_table[m].addr == a) return g_func_table[m].fn; if (g_func_table[m].addr < a) lo = m + 1; else hi = m - 1; }
    return 0;
}
void call_indirect(Ctx* c, u32 a) {
    GuestFn f = lookup_fn(a);
    if (!f) rt_fatal("call_indirect to unknown address %08X (lr %08X, in %s) r1 %08X r2 %08X r13 %08X\n", a, c->lr, name_of(c->lr), c->r[1], c->r[2], c->r[13]);
    f(c);
}

/* ---------------- time base ---------------- */
#define BUS_CLOCK 162000000u
#define TB_RATE (BUS_CLOCK / 4)
static struct timespec g_t0;
#ifdef _WIN32
extern int __stdcall QueryPerformanceCounter(long long*); extern int __stdcall QueryPerformanceFrequency(long long*);
u64 host_tb(void) {
    static long long f, t0; long long t; if (!f) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t); return (u64)((double)(t - t0) / (double)f * TB_RATE);
}
#else
u64 host_tb(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) * 1e-9;
    return (u64)(s * TB_RATE);
}
#endif

/* ---------------- SPRs ---------------- */
u32 MFSPR(Ctx* c, int spr) {
    switch (spr) {
    case 1: return c->xer; case 8: return c->lr; case 9: return c->ctr;
    case 22: { extern u32 sched_get_dec(void); return sched_get_dec(); } case 26: return c->srr0; case 27: return c->srr1;
    case 268: return (u32)host_tb(); case 269: return (u32)(host_tb() >> 32);
    case 272: case 273: case 274: case 275: return c->sprg[spr - 272];
    case 287: return 0x00083214; /* PVR: Gekko */
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: return c->gqr[spr - 912];
    case 920: return c->hid2; case 921: return c->wpar; case 1008: return c->hid0; case 1009: return 0; /* HID1 */
    case 1010: return c->iabr; case 1013: return c->dabr; case 1017: return c->l2cr; case 1011: return c->hid4;
    default: rt_log("mfspr %d\n", spr); return 0;
    }
}
void MTSPR(Ctx* c, int spr, u32 v) {
    switch (spr) {
    case 1: c->xer = v; return; case 8: c->lr = v; return; case 9: c->ctr = v; return;
    case 22: { extern void sched_set_dec(u32); c->dec = v; sched_set_dec(v); return; } case 26: c->srr0 = v; return; case 27: c->srr1 = v; return;
    case 272: case 273: case 274: case 275: c->sprg[spr - 272] = v; return;
    case 284: case 285: return; /* TB writes ignored */
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c->gqr[spr - 912] = v; return;
    case 920: c->hid2 = v; return; case 921: c->wpar = v; return; case 1008: c->hid0 = v; return;
    case 1010: c->iabr = v; return; case 1013: c->dabr = v; return; case 1017: c->l2cr = v & 0x7FFFFFFF; return; case 1011: c->hid4 = v; return;
    case 952: case 953: case 954: case 955: case 956: case 957: case 958: case 959: return; /* perf monitors */
    default: rt_log("mtspr %d = %08X\n", spr, v); return;
    }
}
void MTMSR(Ctx* c, u32 v) { c->msr = v; }
void RFI(Ctx* c) { rt_fatal("rfi reached (srr0 %08X)\n", c->srr0); }
void SYSCALL(Ctx* c, u32 pc) { rt_log("sc at %08X (%s)\n", pc, name_of(pc)); }
void TRAP(Ctx* c, u32 pc) { rt_fatal("trap at %08X (%s)\n", pc, name_of(pc)); }
void UNIMPL(Ctx* c, u32 pc, u32 insn) { rt_fatal("unimplemented insn %08X at %08X (%s)\n", insn, pc, name_of(pc)); }

/* ---------------- paired singles ---------------- */
static double deq(u32 raw, int type, int scale) {
    double v;
    switch (type) {
    case 4: v = (u8)raw; break; case 5: v = (u16)raw; break; case 6: v = (s8)raw; break; case 7: v = (s16)raw; break;
    default: { float f; memcpy(&f, &raw, 4); return f; }
    }
    return ldexp(v, -scale);
}
static u32 quant(double v, int type, int scale) {
    if (type == 0 || type > 7 || type < 4) { float f = (float)v; u32 r; memcpy(&r, &f, 4); return r; }
    double x = ldexp(v, scale); s32 i;
    double lo = type == 4 ? 0 : type == 5 ? 0 : type == 6 ? -128 : -32768;
    double hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
    if (x < lo) x = lo; if (x > hi) x = hi; i = (s32)x; return (u32)i;
}
static int sext6(u32 x) { x &= 63; return x & 32 ? (int)x - 64 : (int)x; }
void PSQ_L(Ctx* c, int fd, u32 ea, int w, int i) {
    u32 g = c->gqr[i]; int type = (g >> 16) & 7; int scale = sext6(g >> 24);
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    u32 r0 = sz == 1 ? R8(ea) : sz == 2 ? R16(ea) : R32(ea);
    c->f[fd][0] = deq(r0, type, scale);
    if (w) c->f[fd][1] = 1.0;
    else { u32 r1 = sz == 1 ? R8(ea + 1) : sz == 2 ? R16(ea + 2) : R32(ea + 4); c->f[fd][1] = deq(r1, type, scale); }
}
void PSQ_ST(Ctx* c, int fs, u32 ea, int w, int i) {
    u32 g = c->gqr[i]; int type = g & 7; int scale = sext6(g >> 8);
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    u32 a = quant(c->f[fs][0], type, scale);
    if (sz == 1) W8(ea, a); else if (sz == 2) W16(ea, a); else W32(ea, a);
    if (!w) { u32 b = quant(c->f[fs][1], type, scale); if (sz == 1) W8(ea + 1, b); else if (sz == 2) W16(ea + 2, b); else W32(ea + 4, b); }
}

/* ---------------- boot ---------------- */
extern const u8 soa_dol_blob[];
static void load_dol(const char* path, u32* entry) {
    (void)path; const u8* d = soa_dol_blob;   /* the exact image the code was recompiled from */
    #define BE(o) ((u32)d[o] << 24 | (u32)d[(o)+1] << 16 | (u32)d[(o)+2] << 8 | d[(o)+3])
    u32 bss = BE(0xD8), bsz = BE(0xDC);
    memset(g_ram + (bss & 0x01FFFFFF), 0, bsz);   /* header BSS range overlaps .sdata/.sdata2: clear first */
    for (int i = 0; i < 18; i++) {
        u32 off = BE(i * 4), addr = BE(0x48 + i * 4), sz = BE(0x90 + i * 4);
        if (sz) memcpy(g_ram + (addr & 0x01FFFFFF), d + off, sz);
    }
    *entry = BE(0xE0);
}

static void setup_lowmem(void) {
    /* mirrors Dolphin's CBoot::SetupGCMemory + apploader results for GEAP8P */
    memcpy(g_ram, "GEAP8P", 6); W16(0x80000006, 0x0000); /* disc id, disc num/version */
    W32(0x8000001C, 0xC2339F3D);  /* DVD magic word */
    W32(0x80000020, 0x0D15EA5E);  /* boot magic */
    W32(0x80000024, 0x00000001);  /* version */
    W32(0x80000028, RAM_SIZE);    /* physical memory size */
    W32(0x8000002C, 0x00000003);  /* console type: retail (HW2) */
    W32(0x80000030, 0x00000000);  /* arena lo (OSInit computes it) */
    W32(0x80000034, 0x817FE8C0);  /* arena hi (Dolphin default) */
    W32(0x80000038, 0x817FE8C0);  /* FST location (filled below) */
    W32(0x8000003C, 0x00000000);  /* FST max length */
    W32(0x800000CC, 0x00000001);  /* video mode: PAL50 */
    W32(0x800000D0, 0x01000000);  /* ARAM size */
    W32(0x800000EC, 0x81800000);  /* dev debugger monitor address */
    W32(0x800000F0, RAM_SIZE);    /* simulated memory size */
    W32(0x800000F8, BUS_CLOCK);   /* bus clock */
    W32(0x800000FC, 486000000u);  /* CPU clock */
    W32(0x800030D8, 0); W32(0x800030DC, 0); /* system time */
    W16(0x800030E0, 6);           /* PADInit: pad config */
    W8(0x800030E3, 0);
    W16(0x800030E6, 0x8201);      /* DVD drive info */
    W32(0x800030F0, 0);           /* DOL execute parameters */
}

extern void load_fst(const char* root);

void rt_backtrace(Ctx* c) {
    rt_log("  bt: lr %08X %s\n", c->lr, name_of(c->lr));
    u32 fp = c->r[1]; for (int i = 0; i < 16 && fp > 0x80000000u && fp < 0x81800000u; i++) { u32 nfp = R32(fp); if (!nfp) break; u32 lr = R32(nfp + 4); rt_log("  #%d %08X %s\n", i, lr, name_of(lr)); fp = nfp; }
}
static unsigned long* g_prof; static int g_nprof;
#ifndef _WIN32
static void on_prof(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc = (ucontext_t*)uc_;
    if (g_nprof < 4000000) g_prof[g_nprof++] = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
}
#endif
static void prof_dump(void) {
    if (!g_prof) return;
    FILE* f = fopen("prof.txt", "w"); for (int i = 0; i < g_nprof; i++) fprintf(f, "%lx\n", g_prof[i]); fclose(f);
}
void rt_report(void);
static void on_sig(int sig) {
    fprintf(stderr, "signal %d: lr %08X (%s) r1 %08X r3 %08X\n", sig, g_ctx->lr, name_of(g_ctx->lr), g_ctx->r[1], g_ctx->r[3]);
    rt_log("signal %d: lr %08X (%s)\n", sig, g_ctx->lr, name_of(g_ctx->lr));
    { extern void fifo_debug(void); fifo_debug(); }
    { u32 fp = g_ctx->r[1]; for (int i = 0; i < 24 && fp > 0x80000000u && fp < 0x81800000u; i++) { u32 nfp = R32(fp); if (!nfp) break; u32 lr = R32(nfp + 4); fprintf(stderr, "  #%d %08X %s\n", i, lr, name_of(lr)); fp = nfp; } }
    { extern u64 g_irq_counts[16], g_dec_count, g_frames, g_fifo_bytes, g_draws; fprintf(stderr, "frames %llu fifo %llu bytes draws %llu dec %llu irq:", (unsigned long long)g_frames, (unsigned long long)g_fifo_bytes, (unsigned long long)g_draws, (unsigned long long)g_dec_count);
      for (int b = 0; b < 16; b++) if (g_irq_counts[b]) fprintf(stderr, " bit%d=%llu", b, (unsigned long long)g_irq_counts[b]); fprintf(stderr, "\n"); }
    _exit(2);
}

/* settings.ini next to the exe: "key = value" lines, '#' or ';' comments */
static char g_set_k[64][48]; static int g_set_v[64]; static int g_set_n = -1;
int settings_get(const char* key, int def) {
    if (g_set_n < 0) {
        g_set_n = 0; FILE* f = fopen("settings.ini", "r"); char line[256];
        while (f && fgets(line, sizeof line, f) && g_set_n < 64) {
            char k[48]; int v;
            if (line[0] == '#' || line[0] == ';') continue;
            if (sscanf(line, " %47[A-Za-z0-9_] = %d", k, &v) == 2) { strcpy(g_set_k[g_set_n], k); g_set_v[g_set_n++] = v; }
        }
        if (f) fclose(f);
        else if ((f = fopen("settings.ini", "w"))) {   /* write defaults so the options are discoverable */
            fputs("# Skies of Arcadia Legends (recompiled) settings\n"
                  "# resolution_scale: internal rendering resolution, 1 = native 640x480, 2 = 1280x960, 3 = 1920x1440, 4 = 2560x1920 ...\n"
                  "resolution_scale = 3\n"
                  "# fullscreen: 1 = start in borderless fullscreen (F11 or Alt+Enter toggles in game)\n"
                  "fullscreen = 0\n"
                  "# vsync: 1 = sync presentation to the display\n"
                  "vsync = 0\n"
                  "# window size at startup\n"
                  "window_width = 1280\n"
                  "window_height = 960\n"
                  "# pal60: 1 = let the game use its 60 Hz (EuRGB60) mode, 0 = 50 Hz PAL\n"
                  "pal60 = 1\n"
                  "# fps60: 1 = 60 fps through frame interpolation (the game itself steps at 30)\n"
                  "fps60 = 1\n", f);
            fclose(f);
        }
    }
    for (int i = 0; i < g_set_n; i++) if (!strcmp(g_set_k[i], key)) return g_set_v[i];
    if (!strcmp(key, "resolution_scale")) return 3;
    return def;
}

void settings_set(const char* key, int v) {
    settings_get(key, 0);
    for (int i = 0; i < g_set_n; i++) if (!strcmp(g_set_k[i], key)) { g_set_v[i] = v; return; }
    if (g_set_n < 64) { snprintf(g_set_k[g_set_n], 48, "%s", key); g_set_v[g_set_n++] = v; }
}
/* rewrite settings.ini from the current values (the options screen saves through this) */
void settings_save(void) {
    static const char* doc[][2] = {
        {"resolution_scale", "internal rendering resolution, 1 = native 640x480, 2 = 1280x960, 3 = 1920x1440, 4 = 2560x1920, 5 = 3200x2400, 6 = 3840x2880"},
        {"fullscreen", "1 = start in borderless fullscreen (F11 or Alt+Enter toggles in game)"},
        {"vsync", "1 = sync presentation to the display"},
        {"window_width", "window size at startup"}, {"window_height", 0},
        {"pal60", "1 = let the game use its 60 Hz (EuRGB60) mode, 0 = 50 Hz PAL"},
        {"fps60", "1 = 60 fps through frame interpolation (the game itself steps at 30)"},
        {"dc_sound", "1 = Dreamcast music and sound effects (needs dc_path.txt), 0 = GameCube"},
        {"widescreen", "1 = 16:9 (wider view in 3D; menus and text keep their shape), 0 = original 4:3"},
    };
    FILE* f = fopen("settings.ini", "w"); if (!f) return;
    fputs("# Skies of Arcadia Legends (recompiled) settings - also editable in game: title screen > Options, or F1\n", f);
    static const int defs[] = {3, 0, 0, 1280, 960, 1, 1, 1, 1};
    for (unsigned i = 0; i < sizeof doc / sizeof doc[0]; i++) { if (doc[i][1]) fprintf(f, "# %s: %s\n", doc[i][0], doc[i][1]); fprintf(f, "%s = %d\n", doc[i][0], settings_get(doc[i][0], defs[i])); }
    fclose(f);
}

int main(int argc, char** argv) {
#ifndef _WIN32
    if (getenv("SOA_PROF")) {
        g_prof = malloc(sizeof(unsigned long) * 4000000);
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, 0);
        struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, 0);
    }
#endif
    atexit(prof_dump);
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGSEGV, on_sig);
    static char rootbuf[1024];
    if (argc < 2) {
        FILE* pf = fopen("game_path.txt", "r");
        if (pf && fgets(rootbuf, sizeof rootbuf, pf)) { rootbuf[strcspn(rootbuf, "\r\n")] = 0; fclose(pf); }
        else strcpy(rootbuf, ".");
        static char* av[2]; av[0] = argv[0]; av[1] = rootbuf; argv = av; argc = 2;
    }
    { char t[1100]; snprintf(t, sizeof t, "%s/sys/fst.bin", argv[1]); FILE* tf = fopen(t, "rb");
      if (!tf) { char msg[1400]; snprintf(msg, sizeof msg, "Game files not found at '%s'.\nPut the path of your extracted Skies of Arcadia Legends (PAL) folder - the one containing sys and files - into game_path.txt next to this program.", argv[1]); rt_fatal("%s\n", msg); }
      fclose(tf); }
#ifndef _WIN32
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
#endif
    g_log = fopen("rt.log", "w");
    load_names("names.txt");
    g_ram = calloc(1, RAM_SIZE); g_lcache = calloc(1, 0x4000);
    char p[1024]; snprintf(p, sizeof p, "%s/sys/main.dol", argv[1]); g_dol_path = p;
    u32 entry; load_dol(p, &entry);
    setup_lowmem();
    load_fst(argv[1]);
    { extern void ax_load_coeffs(const char*); ax_load_coeffs("dsp_coef.bin"); }
    { extern int g_scale; int sc = settings_get("resolution_scale", 3); if (getenv("SOA_SCALE")) sc = atoi(getenv("SOA_SCALE"));
#ifndef _WIN32
      if (!getenv("SOA_SCALE")) sc = 1;   /* headless test runs stay native */
#endif
      g_scale = sc < 1 ? 1 : sc > 8 ? 8 : sc; }
    { extern int g_ws; g_ws = settings_get("widescreen", 1) ? 1 : 0; if (getenv("SOA_WS")) g_ws = atoi(getenv("SOA_WS")); }
    { extern int g_use_gl; extern int gl_init(void); if (!getenv("SOA_SOFT")) { if (gl_init()) g_use_gl = 1; else rt_log("GL init failed: software renderer\n"); } }
    static Ctx c0; Ctx* c = &c0; g_ctx = c;
    c->msr = 0x00002032; c->hid0 = 0x0011C464; c->hid2 = 0xE0000000;
    c->r[1] = 0x816FFFF0;
    rt_log("boot: entry %08X\n", entry);
    GuestFn f = lookup_fn(entry);
    if (!f) rt_fatal("no entry fn\n");
    f(c);
    rt_log("entry returned\n");
    return 0;
}
