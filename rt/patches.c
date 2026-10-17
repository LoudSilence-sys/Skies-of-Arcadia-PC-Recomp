/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 LoudSilence-sys and contributors
*/
/* patches.c - game-specific behaviour changes for the PC port (enhancements), as overrides of recompiled functions.
 *
 * Frame pacing: the main loop (fn_801DE0AC, called once per game frame) waits until at least one retrace has
 * passed since the frame started, then swaps buffers and calls VIWaitForRetrace (0x801DE168), so every game
 * frame takes two fields: 30 fps at 60 Hz, 25 fps at 50 Hz. With unlock_framerate (debug only: the game logic is fixed-step, so everything runs twice as fast) the final wait is skipped, giving one
 * frame per field. Real 60 fps comes from frame interpolation in gx.c instead.
 */
#include "rt.h"
#include <stdlib.h>
#include <string.h>

void rt_log(const char* fmt, ...);
int settings_get(const char* key, int def);
void r_80240B24(Ctx* c);

static int g_fps60 = -1;
void f_80240B24(Ctx* c) {   /* VIWaitForRetrace */
    if (c->lr == 0x801DE168) { extern void quickstate_poll(Ctx* c); quickstate_poll(c); }   /* quick save / load point */
    if (g_fps60 < 0) { g_fps60 = settings_get("unlock_framerate", 0); if (getenv("SOA_FPS60")) g_fps60 = atoi(getenv("SOA_FPS60")); if (g_fps60) rt_log("patch: 60 fps frame pacing\n"); }
    if (g_fps60 && c->lr == 0x801DE168) return;   /* end-of-frame wait in the main loop */
    { extern int g_vi_wait; g_vi_wait++; r_80240B24(c); g_vi_wait--; }   /* the game is waiting for the retrace: it may be delivered now (sched.c) */
}

/* The main loop's first retrace wait is a busy poll of VIGetRetraceCount (0x801DE128): tell the scheduler the game is
   waiting (sched.c holds the retrace back while the game is busy) */
void r_80241DC8(Ctx* c);
u64 g_vi_poll_tb;
void f_80241DC8(Ctx* c) {   /* VIGetRetraceCount */
    if (c->lr == 0x801DE128) { extern u64 host_tb(void); g_vi_poll_tb = host_tb(); }
    r_80241DC8(c);
}

/* debugging: log the object draw dispatch (SOA_DBGOBJ=frame) */
void r_80118FAC(Ctx* c); void r_80119054(Ctx* c);
extern int g_frame_no_pub;
static void objlog(Ctx* c, const char* who) {
    static int f = -1; if (f < 0) { const char* e = getenv("SOA_DBGOBJ"); f = e ? atoi(e) : 0; }
    if (f && g_frame_no_pub == f) { u32 o = c->r[3]; rt_log("obj %s lr %08X obj %08X +190 %f +1a8 %f +19c %08X +e4 %08X +184 %08X sp %08X\n", who, c->lr, o, RF32(o + 0x190), RF32(o + 0x1a8), R32(o + 0x19c), R32(o + 0xe4), R32(o + 0x184), c->r[1]); }
}
void f_80118FAC(Ctx* c) { objlog(c, "A"); r_80118FAC(c); }
void f_80119054(Ctx* c) { objlog(c, "B"); r_80119054(c); }

/* ---------------- encounter rate (settings: encounter_rate 0 normal, 1 half, 2 quarter, 3 off) ----------------
   The per-step random battle check (fn_800C1FD8) rolls rand() at 0x800C2430 into a uniform 0..100 value and starts
   a battle when it is below the area's rate; a step counter (lbl_8034A428) shrinks the roll over time so a battle
   eventually comes. Only that one call is changed: the game's random sequence still advances as normal. */
void r_80261404(Ctx* c);
int g_encounter_rate = -1;
void f_80261404(Ctx* c) {   /* rand */
    u32 lr = c->lr;
    r_80261404(c);
    if (lr != 0x800C2434) return;
    { static int lg = -1; if (lg < 0) lg = getenv("SOA_ENCLOG") ? 2000 : 0; if (lg > 0) { lg--; extern int g_frame_no_pub; rt_log("encounter check f%d: rand %u steps %u\n", g_frame_no_pub, c->r[3], R32(0x8034A428u)); } }
    if (g_encounter_rate < 0) g_encounter_rate = settings_get("encounter_rate", 0);
    if (g_encounter_rate <= 0) return;
    static u32 seq; seq++;
    int pass = g_encounter_rate == 1 ? (seq & 1) == 0 : g_encounter_rate == 2 ? (seq & 3) == 0 : 0;
    if (pass) return;
    c->r[3] = 16383;   /* maps to a roll of 99.997: no battle on this step */
    if (g_encounter_rate >= 3) W32(0x8034A428u, 0);   /* off: no build-up towards a forced battle either */
}

/* ---------------- widescreen: wider visibility culling ----------------
   Meshes are culled by a bounding-sphere test against the camera's screen extents (lbl_80348E10: +8 right = 319,
   +0xC left = -320 pixels from the centre). In widescreen the picture shows 4/3 more horizontally, so objects at
   the sides were being dropped. The side limits are widened for the duration of the test only; the projection and
   everything else that uses them keeps the 4:3 values (the renderer does the widening). */
#define CAM_EXT 0x80348E10u
void r_8028B454(Ctx* c);
void f_8028B454(Ctx* c) {
    extern int g_ws;
    if (!g_ws) { r_8028B454(c); return; }
    u32 r0 = R32(CAM_EXT + 8), l0 = R32(CAM_EXT + 0xC);
    float r = RF32(CAM_EXT + 8), l = RF32(CAM_EXT + 0xC);
    float rw = r * (4.0f / 3.0f), lw = l * (4.0f / 3.0f); u32 a, b; memcpy(&a, &rw, 4); memcpy(&b, &lw, 4);
    W32(CAM_EXT + 8, a); W32(CAM_EXT + 0xC, b);
    r_8028B454(c);
    W32(CAM_EXT + 8, r0); W32(CAM_EXT + 0xC, l0);
}

/* ---------------- title screen: third menu entry "Options" ----------------
   The title menu (state at [0x8034AD7C]) was built for the Dreamcast's three entries (New Game, Continue,
   Homepage): it keeps three per-item enable flags (+6..+8), a maximum index (+0x10), three fade/position
   animators, and the title texture still holds the third label and a third cursor position. The GameCube
   build only enables and draws two. We enable the third, draw it, and repaint its label as "Options". */
#define TITLE_MENU 0x8034AD7Cu
void r_801DA648(Ctx* c); void r_801DA924(Ctx* c); void r_801DAC38(Ctx* c); void f_8022C784(Ctx* c);
extern const unsigned char g_options_label[];   /* 156x40 RGBA (options_label.c) */
void options_open(void);
int options_is_open(void);
static void title_draw_sprite(Ctx* c, u32 inst, float z, float a, float pos) {
    u32 r3 = c->r[3], r4 = c->r[4], r5 = c->r[5]; double f1 = c->f[1][0], f2 = c->f[2][0], f3 = c->f[3][0], f4 = c->f[4][0];
    c->r[3] = inst; c->r[4] = 0; c->r[5] = 0; c->f[1][0] = z; c->f[2][0] = 0.0; c->f[3][0] = a; c->f[4][0] = pos;
    u32 lr = c->lr; c->lr = 0x801DAD4C; f_8022C784(c); c->lr = lr;
    c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->f[1][0] = f1; c->f[2][0] = f2; c->f[3][0] = f3; c->f[4][0] = f4;
}
/* repaint the unused third label ("Homepage", texels x 14..170, y 350..390 of the 1024x512 RGB5A3 title texture);
   the texture is recognised when the renderer binds it while the title menu is up */
static int g_title_menu_up;
static void title_paint_label(u32 base) {
    const int X0 = 14, Y0 = 350, LW = 156, LH = 40, W = 1024;
    for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) {
        const unsigned char* p = g_options_label + 4 * (y * LW + x);
        u32 r = p[0], g = p[1], b = p[2], al = p[3]; u16 v;
        if (al >= 0xE0) v = (u16)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        else v = (u16)((al >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
        int tx = X0 + x, ty = Y0 + y; u32 blk = (u32)(ty / 4) * (W / 4) + tx / 4;
        W16(base + blk * 32 + ((ty & 3) * 4 + (tx & 3)) * 2, v);
    }
}
void tex_bind_hook(u32 addr, u32 w, u32 h, u32 fmt) {
    extern int g_frame_no_pub;
    if (!g_title_menu_up || g_frame_no_pub - g_title_menu_up > 2 || w != 1024 || h != 512 || fmt != 5) return;
    static u32 last; u32 base = 0x80000000u | (addr & 0x01FFFFFF);
    /* marker: the label's centre texel */
    const unsigned char* p = g_options_label + 4 * (20 * 156 + 78); u16 want = p[3] >= 0xE0 ? (u16)(0x8000 | (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3)) : (u16)((p[3] >> 5) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4));
    int tx = 14 + 78, ty = 350 + 20; u32 at = base + ((u32)(ty / 4) * 256 + tx / 4) * 32 + ((ty & 3) * 4 + (tx & 3)) * 2;
    if (R16(at) == want && last == base) return;
    title_paint_label(base); last = base;
}
void f_801DA648(Ctx* c) {   /* title menu init */
    r_801DA648(c);
    { extern int g_frame_no_pub; g_title_menu_up = g_frame_no_pub; }
}
void f_801DA924(Ctx* c) {   /* title menu input: returns the chosen item, or < 0 */
    u32 m = R32(TITLE_MENU);
    if (m) { W8(m + 8, 1); W16(m + 0x10, 2); }
    if (options_is_open()) { c->r[3] = (u32)-1; return; }
    r_801DA924(c);
    if ((s32)c->r[3] == 2) { options_open(); W8(m + 5, (u8)-2); c->r[3] = (u32)-2; }
}
void f_801DAC38(Ctx* c) {   /* title menu draw */
    r_801DAC38(c);
    u32 m = R32(TITLE_MENU); if (!m) return;
    { extern int g_frame_no_pub; g_title_menu_up = g_frame_no_pub; }
    title_draw_sprite(c, 0x802FC4F8u, -0.901f, RF32(m + 0x188), RF32(m + 0x138));   /* third label, faded with New Game */
    if ((s8)R8(m + 1) == 2) title_draw_sprite(c, 0x802FC3B8u, -0.851f, 1.0f, RF32(m + 0x48));   /* cursor on it */
}

/* ---- text speed (options: Normal / Fast / Instant) ----
   fn_8010D8E4 types out a message window one character per frame.  Its object: +0 flags (0x20 = reveal
   up to the next stop code, which the game sets when A or B is pressed during typing; 0x1000 = the window
   may not be hurried, e.g. text timed to a cutscene), +2 state (1 = typing), +54 length, +56 characters
   shown, +58 frames until the next one. */
void r_8010D8E4(Ctx* c);
int g_text_speed = -1;
void text_speed_changed(void) { g_text_speed = -1; }
static int text_skippable(u32 o) { return !(R16(o) & 0x1000); }
void f_8010D8E4(Ctx* c) {
    if (g_text_speed < 0) g_text_speed = settings_get("text_speed", 0);
    u32 o = c->r[3];
    int typing = g_text_speed > 0 && (s16)R16(o + 2) == 1 && (s16)R16(o + 56) < (s16)R16(o + 54) && text_skippable(o);
    { static int lg = -1; if (lg < 0) lg = getenv("SOA_TXTLOG") ? 300 : 0; if (lg > 0 && (s16)R16(o + 2) == 1) { lg--; u32 i = R32(c->r[13] - 29004), st = R32(0x803150E0u + i * 4); rt_log("text: obj %08X flags %04X state %d len %d shown %d speed %d style %u/%08X typing %d\n", o, R16(o), (s16)R16(o + 2), (s16)R16(o + 54), (s16)R16(o + 56), g_text_speed, i, st ? R32(st + 8) : 0, typing); } }
    if (typing && g_text_speed >= 2) W16(o, R16(o) | 0x20);
    r_8010D8E4(c);
    { static int lg = -1; static u32 pk; if (lg < 0) lg = getenv("SOA_TXTLOG2") ? 400 : 0;
      u32 k = R16(o) << 16 | (R16(o + 2) & 0xFF) << 8 | (c->r[3] & 0xFF);
      if (lg > 0 && k != pk) { lg--; pk = k; extern int g_frame_no_pub; rt_log("msg f%d obj %08X flags %04X state %d ret %d len %d shown %d +3C %04X +3E %04X +54.. %08X\n", g_frame_no_pub, o, R16(o), (s16)R16(o + 2), (int)c->r[3], (s16)R16(o + 54), (s16)R16(o + 56), R16(o + 60), R16(o + 62), R32(o + 64)); } }
    if (typing && g_text_speed == 1 && !(R16(o + 0) & 0x20) && (s16)R16(o + 2) == 1 && (s16)R16(o + 56) < (s16)R16(o + 54)) {
        u32 r3 = c->r[3]; c->r[3] = o; r_8010D8E4(c); c->r[3] = r3;   /* a second character this frame */
    }
}

/* ---- widescreen HUD (options: Widescreen HUD = screen edges) ----
   fn_800E28B4 draws the field minimap in the bottom-right corner of the 4:3 frame; in widescreen its draws
   are moved to the right edge of the 16:9 picture (gx.c draw_out, g_hud_anchor). */
void r_800E28B4(Ctx* c);
static int g_ws_hud = -1;
void ws_hud_changed(void) { g_ws_hud = -1; }
void f_800E28B4(Ctx* c) {
    if (g_ws_hud < 0) g_ws_hud = settings_get("ws_hud", 0);
    extern int g_hud_anchor; int old = g_hud_anchor;
    if (g_ws_hud) g_hud_anchor = 1;
    r_800E28B4(c);
    g_hud_anchor = old;
}

/* Battle HUD: spirit gauge (fn_80027D78), character status (fn_800230E8), command ring (fn_80025798).
   Their pieces move to the nearer screen edge and the gauge bar stretches across (g_hud_anchor = 2). Text
   they queue (fn_801E4AC8 string, fn_801E4730 number) is drawn later from the main loop, so its x is tagged
   with a large offset that the renderer recognises, removes and anchors (gx.c draw_out). */
void r_80027D78(Ctx* c); void r_800230E8(Ctx* c); void r_80025798(Ctx* c); void r_801E4AC8(Ctx* c); void r_801E4730(Ctx* c);
static void hud_call(Ctx* c, void (*fn)(Ctx*), int mode) {
    if (g_ws_hud < 0) g_ws_hud = settings_get("ws_hud", 0);
    extern int g_hud_anchor, g_ws; int old = g_hud_anchor;
    if (g_ws_hud && g_ws) g_hud_anchor = mode;
    fn(c);
    g_hud_anchor = old;
}
void f_80027D78(Ctx* c) { hud_call(c, r_80027D78, 3); }
void f_800230E8(Ctx* c) { hud_call(c, r_800230E8, 2); }
void f_80025798(Ctx* c) { extern u64 g_frames, g_cmd_frame; g_cmd_frame = g_frames; hud_call(c, r_80025798, 2); }
static void hud_text_x(Ctx* c) { extern int g_hud_anchor; if (g_hud_anchor >= 2) { s32 x = (s16)c->r[3]; c->r[3] = (u32)(x + (x < 320 ? 4096 : 8192)); } }
void f_801E4AC8(Ctx* c) { hud_text_x(c); r_801E4AC8(c); }
void f_801E4730(Ctx* c) { hud_text_x(c); r_801E4730(c); }

/* ---- battle detection (battle speed option) ----
   fn_8000A1B4 runs every frame while a battle is on (battle draw root); the command ring (fn_80025798, see
   the HUD hooks) is drawn while the game waits for the player's command. */
void r_8000A1B4(Ctx* c);
u64 g_battle_frame, g_cmd_frame;
void f_8000A1B4(Ctx* c) { extern u64 g_frames; g_battle_frame = g_frames; r_8000A1B4(c); }
