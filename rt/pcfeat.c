/* pcfeat.c - PC-port features that are independent of the game code:
   - custom texture packs in Dolphin's format (Load/Textures/GEAP8P or GEA, .png or .dds, names from Dolphin)
   - on-screen messages
   - screenshots (PNG, written on a worker thread) */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FALSE
#include "ext/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ext/stb_image_write.h"

u64 host_tb(void);

/* ---------------- texture packs ---------------- */
typedef struct { char* name; char* path; } PackEnt;
static PackEnt* g_pk; static u32 g_pk_cap, g_pk_n;
static int g_pk_scanned;
static u32 strhash(const char* s) { u32 h = 2166136261u; while (*s) { h ^= (u8)*s++; h *= 16777619u; } return h; }
static void pk_insert(const char* name, const char* path) {
    if ((g_pk_n + 1) * 2 > g_pk_cap) {
        u32 nc = g_pk_cap ? g_pk_cap * 2 : 4096; PackEnt* n = calloc(nc, sizeof *n);
        for (u32 i = 0; i < g_pk_cap; i++) if (g_pk[i].name) { u32 h = strhash(g_pk[i].name) & (nc - 1); while (n[h].name) h = (h + 1) & (nc - 1); n[h] = g_pk[i]; }
        free(g_pk); g_pk = n; g_pk_cap = nc;
    }
    u32 h = strhash(name) & (g_pk_cap - 1);
    while (g_pk[h].name) { if (!strcmp(g_pk[h].name, name)) return; h = (h + 1) & (g_pk_cap - 1); }   /* first found wins */
    g_pk[h].name = strdup(name); g_pk[h].path = strdup(path); g_pk_n++;
}
static const char* pk_find(const char* name) {
    if (!g_pk_n || !name || !name[0]) return 0;
    u32 h = strhash(name) & (g_pk_cap - 1);
    while (g_pk[h].name) { if (!strcmp(g_pk[h].name, name)) return g_pk[h].path; h = (h + 1) & (g_pk_cap - 1); }
    return 0;
}
static void pk_scan_dir(const char* dir, int depth) {
    DIR* d = opendir(dir); if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 8) pk_scan_dir(p, depth + 1); continue; }
        size_t n = strlen(e->d_name);
        if (n < 9 || strncmp(e->d_name, "tex1_", 5)) continue;
        const char* ext = e->d_name + n - 4;
        if (strcmp(ext, ".png") && strcmp(ext, ".dds") && strcmp(ext, ".PNG") && strcmp(ext, ".DDS")) continue;
        char name[256]; snprintf(name, sizeof name, "%.*s", (int)(n - 4), e->d_name);
        if (strstr(name, "_mip")) continue;   /* explicit mip levels are found next to their base level */
        char* arb = strstr(name, "_arb"); if (arb) memmove(arb, arb + 4, strlen(arb + 4) + 1);
        pk_insert(name, p);
    }
    closedir(d);
}
int texpack_enabled(void) {
    static int en = -1; static u64 chk;
    u64 now = host_tb(); if (en < 0 || now - chk > 40500000ull) { en = settings_get("hd_textures", 1); chk = now; }
    if (!en) return 0;
    if (!g_pk_scanned) {
        g_pk_scanned = 1;
        static const char* ids[] = {"GEAP8P", "GEAE8P", "GEAJ8P", "GEA"};
        for (int i = 0; i < 4; i++) { char d[64]; snprintf(d, sizeof d, "Load/Textures/%s", ids[i]); pk_scan_dir(d, 0); }
        if (g_pk_n) { rt_log("texture pack: %u textures\n", g_pk_n); osd_message("HD texture pack: %u textures", g_pk_n); }
    }
    return g_pk_n != 0;
}
const char* texpack_lookup(const char* full, const char* wild_tlut, const char* wild_tex) {
    const char* p = pk_find(full); if (!p) p = pk_find(wild_tlut); if (!p) p = pk_find(wild_tex);
    return p;
}
/* PNG -> RGBA8 (caller frees with texpack_free) */
u8* texpack_load_png(const char* path, int* w, int* h) { int n; return stbi_load(path, w, h, &n, 4); }
void texpack_free(void* p) { stbi_image_free(p); }
/* the mip level files that belong to a base path: "<base>_mipN.<ext>" */
void texpack_mip_path(const char* base, int level, char* out, int cap) {
    const char* dot = strrchr(base, '.'); int n = dot ? (int)(dot - base) : (int)strlen(base);
    snprintf(out, cap, "%.*s_mip%d%s", n, base, level, dot ? dot : "");
}

/* ---------------- on-screen messages ---------------- */
static char g_osd[4][160]; static u64 g_osd_t[4]; static int g_osd_n;
void osd_message(const char* fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    rt_log("osd: %s\n", b);
    int i = g_osd_n++ & 3; snprintf(g_osd[i], sizeof g_osd[i], "%s", b); g_osd_t[i] = host_tb();
}
/* the messages still visible, newest last; alpha fades over the last half second of 3 s */
int osd_get(int k, const char** text, float* alpha) {
    u64 now = host_tb(); int found = 0;
    for (int j = 0; j < 4; j++) {
        int i = (g_osd_n - 4 + j) & 3; if (g_osd_n - 4 + j < 0 || !g_osd[i][0]) continue;
        double age = (double)(now - g_osd_t[i]) / 40500000.0; if (age > 3.0) continue;
        if (found++ == k) { *text = g_osd[i]; *alpha = age < 2.5 ? 1.f : (float)((3.0 - age) / 0.5); return 1; }
    }
    return 0;
}

/* ---------------- screenshots ---------------- */
int g_shot_req;
void screenshot_request(void) { g_shot_req = 1; }
typedef struct { u8* px; int w, h; char path[256]; } Shot;
static void* shot_worker(void* a) {
    Shot* s = a;
    stbi_write_png_compression_level = 6;
    int ok = stbi_write_png(s->path, s->w, s->h, 4, s->px, s->w * 4);
    if (ok) osd_message("Screenshot saved: %s", s->path); else osd_message("Screenshot failed");
    free(s->px); free(s); return 0;
}
/* called by the renderer with the finished frame (rows top-down, RGBA) */
void screenshot_write(u8* rgba, int w, int h) {
#ifdef _WIN32
    mkdir("Screenshots");
#else
    mkdir("Screenshots", 0755);
#endif
    Shot* s = malloc(sizeof *s); s->px = rgba; s->w = w; s->h = h;
    for (int i = 0; i < w * h; i++) rgba[4 * i + 3] = 255;
    time_t t = time(0); struct tm* lt = localtime(&t);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", lt);
    snprintf(s->path, sizeof s->path, "Screenshots/SkiesOfArcadia_%s.png", ts);
    static char last[32]; static int seq; if (!strcmp(last, ts)) snprintf(s->path, sizeof s->path, "Screenshots/SkiesOfArcadia_%s_%d.png", ts, ++seq); else { seq = 0; strcpy(last, ts); }
    pthread_t th; if (pthread_create(&th, 0, shot_worker, s)) shot_worker(s); else pthread_detach(th);
}
