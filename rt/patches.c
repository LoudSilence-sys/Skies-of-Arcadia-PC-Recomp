/* patches.c - game-specific behaviour changes for the PC port (enhancements), as overrides of recompiled functions.
 *
 * Frame pacing: the main loop (fn_801DE0AC, called once per game frame) waits until at least one retrace has
 * passed since the frame started, then swaps buffers and calls VIWaitForRetrace (0x801DE168), so every game
 * frame takes two fields: 30 fps at 60 Hz, 25 fps at 50 Hz. With unlock_framerate (debug only: the game logic is fixed-step, so everything runs twice as fast) the final wait is skipped, giving one
 * frame per field. Real 60 fps comes from frame interpolation in gx.c instead.
 */
#include "rt.h"
#include <stdlib.h>

void rt_log(const char* fmt, ...);
int settings_get(const char* key, int def);
void r_80240B24(Ctx* c);

static int g_fps60 = -1;
void f_80240B24(Ctx* c) {   /* VIWaitForRetrace */
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
