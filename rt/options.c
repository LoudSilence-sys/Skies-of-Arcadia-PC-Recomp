/* options.c - the options screen: opened from the title menu's "Options" entry or with F1 at any time.
   Drawn by the renderer over the game in the style of the game's own windows; changes apply immediately
   where possible and are saved to settings.ini. While it is open the game receives no controller input. */
#include "rt.h"
#include <stdio.h>
#include <string.h>

void rt_log(const char* fmt, ...);
int settings_get(const char* key, int def);
void settings_set(const char* key, int v);
void settings_save(void);
void gl_set_scale(int s);
void ui_rect(float x, float y, float w, float h, const float* ct, const float* cb);
void ui_title(float x, float y, float w, float h, float a);
void ui_text(float x, float y, float size, const char* s, const float* c);
float ui_text_width(const char* s, float size);
WEAK void platform_set_fullscreen(int on) { (void)on; }
WEAK int platform_get_fullscreen(void) { return 0; }
WEAK void platform_set_vsync(int on) { (void)on; }
WEAK void dcsnd_set_enabled(int on) { (void)on; }
void interp_set(int on);

enum { O_RES, O_FULL, O_VSYNC, O_FPS60, O_HZ, O_DC, O_BACK, O_N };
static int g_open, g_sel, g_fade;   /* g_fade: frames since opening, for the fade-in */
static int g_restart_note;
static u32 g_prev;                  /* previous pad buttons, for edge detection */

int options_is_open(void) { return g_open; }
void options_open(void) { if (!g_open) { g_open = 1; g_sel = 0; g_fade = 0; g_prev = 0xFFFFFFFFu; rt_log("options: open\n"); } }
static void options_close(void) { g_open = 0; settings_save(); rt_log("options: closed, settings saved\n"); }
void options_toggle(void) { if (g_open) options_close(); else options_open(); }

static int val(int i) {
    switch (i) {
    case O_RES: return settings_get("resolution_scale", 3);
    case O_FULL: return platform_get_fullscreen();
    case O_VSYNC: return settings_get("vsync", 0);
    case O_FPS60: return settings_get("fps60", 1);
    case O_HZ: return settings_get("pal60", 1);
    case O_DC: return settings_get("dc_sound", 1);
    }
    return 0;
}
static void change(int i, int dir) {
    int v = val(i);
    switch (i) {
    case O_RES: v += dir; if (v < 1) v = 6; if (v > 6) v = 1; settings_set("resolution_scale", v); gl_set_scale(v); break;
    case O_FULL: v = !v; settings_set("fullscreen", v); platform_set_fullscreen(v); break;
    case O_VSYNC: v = !v; settings_set("vsync", v); platform_set_vsync(v); break;
    case O_FPS60: v = !v; settings_set("fps60", v); interp_set(v); break;
    case O_HZ: v = !v; settings_set("pal60", v); g_restart_note = 1; break;
    case O_DC: v = !v; settings_set("dc_sound", v); dcsnd_set_enabled(v); break;
    }
}
/* called once per video field with the raw controller buttons (GameCube layout, hi word of the pad state) */
void options_input(u32 hi) {
    if (!g_open) return;
    g_fade++;
    u32 b = hi >> 16;   /* b0: A=0x100 B=0x200 X=0x400 Y=0x800 Start=0x1000 | b1: dleft=1 dright=2 ddown=4 dup=8 */
    int sx = (hi >> 8) & 0xFF, sy = hi & 0xFF;
    if (sx < 0x50) b |= 0x10000; if (sx > 0xB0) b |= 0x20000; if (sy < 0x50) b |= 0x40000; if (sy > 0xB0) b |= 0x80000;   /* stick as directions */
    if (g_prev == 0xFFFFFFFFu) { g_prev = b; return; }   /* ignore what was held when opening */
    u32 press = b & ~g_prev; g_prev = b;
    int up = press & (0x0008 | 0x80000), down = press & (0x0004 | 0x40000), left = press & (0x0001 | 0x10000), right = press & (0x0002 | 0x20000);
    if (up) g_sel = (g_sel + O_N - 1) % O_N;
    if (down) g_sel = (g_sel + 1) % O_N;
    if (g_sel != O_BACK && (left || right)) change(g_sel, right ? 1 : -1);
    if (press & 0x0100) { if (g_sel == O_BACK) options_close(); else change(g_sel, 1); }
    else if (press & (0x0200 | 0x1000)) options_close();
}

/* ---- drawing (virtual 640x480 coordinates inside the given 4:3 rectangle) ---- */
static float X0, Y0, K;
static void rect(float x, float y, float w, float h, const float* a, const float* b) { ui_rect(X0 + x * K, Y0 + y * K, w * K, h * K, a, b); }
static void text(float x, float y, float sz, const char* s, const float* c) {
    float sh[4] = {0.02f, 0.03f, 0.12f, c[3] * 0.9f};
    ui_text(X0 + (x + 1.5f) * K, Y0 + (y + 1.5f) * K, sz * K, s, sh);   /* drop shadow, as the game's text */
    ui_text(X0 + x * K, Y0 + y * K, sz * K, s, c);
}
static float tw(const char* s, float sz) { return ui_text_width(s, sz * K) / K; }
void options_draw(float x, float y, float w, float h) {
    (void)h; X0 = x; Y0 = y; K = w / 640.f;
    float a = g_fade > 8 ? 1.f : g_fade / 8.f;
    float dim[4] = {0, 0, 0.05f, 0.45f * a};
    rect(0, 0, 640, 480, dim, dim);
    /* window: the game's translucent blue gradient with a light rim */
    float rim[4] = {0.62f, 0.78f, 1.f, 0.95f * a}, top[4] = {0.10f, 0.22f, 0.62f, 0.88f * a}, bot[4] = {0.03f, 0.08f, 0.32f, 0.88f * a};
    float wx = 92, wy = 104, ww = 456, wh = 318;
    rect(wx - 3, wy - 3, ww + 6, wh + 6, rim, rim);
    float dark[4] = {0.02f, 0.05f, 0.20f, 0.95f * a};
    rect(wx - 1, wy - 1, ww + 2, wh + 2, dark, dark);
    rect(wx, wy, ww, wh, top, bot);
    ui_title(X0 + (320 - 117) * K, Y0 + 62 * K, 234 * K, 60 * K, a);
    static const char* names[O_N] = {"Resolution", "Fullscreen", "V-Sync", "60 FPS (interpolated)", "Refresh rate", "Sound", "Back"};
    float white[4] = {1, 1, 1, a}, gold[4] = {1.f, 0.90f, 0.35f, a}, grey[4] = {0.62f, 0.68f, 0.82f, a};
    for (int i = 0; i < O_N; i++) {
        float ry = wy + 22 + i * 38;
        if (i == g_sel) {
            float h0[4] = {0.45f, 0.68f, 1.f, 0.40f * a}, h1[4] = {0.25f, 0.45f, 0.95f, 0.25f * a};
            rect(wx + 12, ry - 4, ww - 24, 34, h0, h1);
            text(wx + 18, ry - 1, 22, ">", gold);
        }
        if (i == O_BACK) { text(320 - tw("Back", 22) / 2, ry, 22, "Back", i == g_sel ? gold : white); continue; }
        text(wx + 40, ry, 22, names[i], white);
        char v[48]; int n = val(i);
        switch (i) {
        case O_RES: snprintf(v, sizeof v, "%dx  %dx%d%s", n, 640 * n, 480 * n, n == 5 ? " (4K)" : ""); break;
        case O_HZ: snprintf(v, sizeof v, "%s", n ? "60 Hz" : "50 Hz"); break;
        case O_DC: snprintf(v, sizeof v, "%s", n ? "Dreamcast" : "GameCube"); break;
        default: snprintf(v, sizeof v, "%s", n ? "On" : "Off"); break;
        }
        char vv[56]; snprintf(vv, sizeof vv, i == g_sel ? "< %s >" : "%s", v);
        text(wx + ww - 24 - tw(vv, 22), ry, 22, vv, i == g_sel ? gold : white);
    }
    const char* hint = g_restart_note ? "Refresh rate applies after a restart" : "Left/Right: change    A: select    B: back";
    text(320 - tw(hint, 16) / 2, wy + wh - 26, 16, hint, grey);
}
