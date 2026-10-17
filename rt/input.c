/* input.c - keyboard and controller bindings (remappable from the options screen, stored in settings.ini).
   The platform layer reports raw keyboard state (virtual-key codes) and XInput button/axis state; this file turns
   them into the GameCube pad state, plus the PC-only actions (fast-forward). */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int settings_get(const char* key, int def);
void settings_set(const char* key, int v);

/* XInput button bits */
#define XB_DUP 0x0001
#define XB_DDOWN 0x0002
#define XB_DLEFT 0x0004
#define XB_DRIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LS 0x0040
#define XB_RS 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000
#define XB_LT 0x10000   /* pseudo-buttons for the triggers */
#define XB_RT 0x20000

typedef struct { const char* name; const char* key; int vk; u32 pad; u32 pad_gc; } Bind;
/* default keys: arrows=stick, Z=A, X=B, C=X, V=Y, Enter=Start, Q=L, W=R, E=Z, IJKL=C-stick, TFGH=D-pad, Tab=fast-forward.
   pad: Xbox labels (A confirms, B cancels); pad_gc: GameCube positions (bottom = A, left = B, right = X, top = Y). */
static Bind g_bind[IN_N] = {
    [IN_A] = {"A", "a", 'Z', XB_A, XB_A},
    [IN_B] = {"B", "b", 'X', XB_B, XB_X},
    [IN_X] = {"X", "x", 'C', XB_X, XB_B},
    [IN_Y] = {"Y", "y", 'V', XB_Y, XB_Y},
    [IN_START] = {"Start", "start", 0x0D, XB_START, XB_START},
    [IN_Z] = {"Z", "z", 'E', XB_RB, XB_RB},
    [IN_L] = {"L", "l", 'Q', XB_LT, XB_LT},
    [IN_R] = {"R", "r", 'W', XB_RT, XB_RT},
    [IN_DUP] = {"D-Pad Up", "dup", 'T', XB_DUP, XB_DUP},
    [IN_DDOWN] = {"D-Pad Down", "ddown", 'G', XB_DDOWN, XB_DDOWN},
    [IN_DLEFT] = {"D-Pad Left", "dleft", 'F', XB_DLEFT, XB_DLEFT},
    [IN_DRIGHT] = {"D-Pad Right", "dright", 'H', XB_DRIGHT, XB_DRIGHT},
    [IN_SUP] = {"Stick Up", "sup", 0x26, 0, 0},
    [IN_SDOWN] = {"Stick Down", "sdown", 0x28, 0, 0},
    [IN_SLEFT] = {"Stick Left", "sleft", 0x25, 0, 0},
    [IN_SRIGHT] = {"Stick Right", "sright", 0x27, 0, 0},
    [IN_CUP] = {"C-Stick Up", "cup", 'I', 0, 0},
    [IN_CDOWN] = {"C-Stick Down", "cdown", 'K', 0, 0},
    [IN_CLEFT] = {"C-Stick Left", "cleft", 'J', 0, 0},
    [IN_CRIGHT] = {"C-Stick Right", "cright", 'L', 0, 0},
    [IN_FF] = {"Fast-Forward", "ff", 0x09, XB_RS, XB_RS},
};
static int g_loaded;
static void load(void) {
    if (g_loaded) return; g_loaded = 1;
    for (int i = 0; i < IN_N; i++) {
        char k[32]; snprintf(k, sizeof k, "key_%s", g_bind[i].key); g_bind[i].vk = settings_get(k, g_bind[i].vk);
        snprintf(k, sizeof k, "pad_%s", g_bind[i].key);
        int def = settings_get("pad_layout", 0) ? (int)g_bind[i].pad_gc : (int)g_bind[i].pad;
        g_bind[i].pad = (u32)settings_get(k, def);
    }
}
const char* input_name(int a) { return a >= 0 && a < IN_N ? g_bind[a].name : ""; }
int input_key(int a) { load(); return g_bind[a].vk; }
u32 input_pad(int a) { load(); return g_bind[a].pad; }
void input_set_key(int a, int vk) {
    load(); char k[32];
    for (int i = 0; i < IN_N; i++) if (i != a && g_bind[i].vk == vk) { g_bind[i].vk = g_bind[a].vk; snprintf(k, sizeof k, "key_%s", g_bind[i].key); settings_set(k, g_bind[i].vk); }   /* swap a clash */
    g_bind[a].vk = vk; snprintf(k, sizeof k, "key_%s", g_bind[a].key); settings_set(k, vk);
}
void input_set_pad(int a, u32 b) {
    load(); char k[32];
    for (int i = 0; i < IN_N; i++) if (i != a && g_bind[i].pad == b) { g_bind[i].pad = g_bind[a].pad; snprintf(k, sizeof k, "pad_%s", g_bind[i].key); settings_set(k, (int)g_bind[i].pad); }
    g_bind[a].pad = b; snprintf(k, sizeof k, "pad_%s", g_bind[a].key); settings_set(k, (int)b);
}
/* layout preset: resets the controller buttons to Xbox labels (0) or GameCube positions (1); keyboard untouched */
void input_set_layout(int gc) {
    load(); settings_set("pad_layout", gc);
    static const u32 xb[IN_N] = {[IN_A] = XB_A, [IN_B] = XB_B, [IN_X] = XB_X, [IN_Y] = XB_Y, [IN_START] = XB_START, [IN_Z] = XB_RB, [IN_L] = XB_LT, [IN_R] = XB_RT,
        [IN_DUP] = XB_DUP, [IN_DDOWN] = XB_DDOWN, [IN_DLEFT] = XB_DLEFT, [IN_DRIGHT] = XB_DRIGHT, [IN_FF] = XB_RS};
    for (int i = 0; i < IN_N; i++) {
        u32 b = xb[i]; if (gc && i == IN_B) b = XB_X; else if (gc && i == IN_X) b = XB_B;
        char k[32]; snprintf(k, sizeof k, "pad_%s", g_bind[i].key); g_bind[i].pad = b; settings_set(k, (int)b);
    }
}
void input_reset_keys(void) {
    static const int def[IN_N] = {[IN_A] = 'Z', [IN_B] = 'X', [IN_X] = 'C', [IN_Y] = 'V', [IN_START] = 0x0D, [IN_Z] = 'E', [IN_L] = 'Q', [IN_R] = 'W',
        [IN_DUP] = 'T', [IN_DDOWN] = 'G', [IN_DLEFT] = 'F', [IN_DRIGHT] = 'H', [IN_SUP] = 0x26, [IN_SDOWN] = 0x28, [IN_SLEFT] = 0x25, [IN_SRIGHT] = 0x27,
        [IN_CUP] = 'I', [IN_CDOWN] = 'K', [IN_CLEFT] = 'J', [IN_CRIGHT] = 'L', [IN_FF] = 0x09};
    load();
    for (int i = 0; i < IN_N; i++) { char k[32]; snprintf(k, sizeof k, "key_%s", g_bind[i].key); g_bind[i].vk = def[i]; settings_set(k, def[i]); }
}
const char* input_key_label(int vk) {
    static char b[16];
    switch (vk) {
    case 0x0D: return "Enter"; case 0x09: return "Tab"; case 0x20: return "Space"; case 0x10: return "Shift"; case 0x11: return "Ctrl"; case 0x12: return "Alt";
    case 0x08: return "Backspace"; case 0x1B: return "Esc"; case 0x25: return "Left"; case 0x26: return "Up"; case 0x27: return "Right"; case 0x28: return "Down";
    case 0x2D: return "Insert"; case 0x2E: return "Delete"; case 0x24: return "Home"; case 0x23: return "End"; case 0x21: return "Page Up"; case 0x22: return "Page Down";
    case 0xA0: return "L-Shift"; case 0xA1: return "R-Shift"; case 0xA2: return "L-Ctrl"; case 0xA3: return "R-Ctrl";
    case 0xBA: return ";"; case 0xBB: return "="; case 0xBC: return ","; case 0xBD: return "-"; case 0xBE: return "."; case 0xBF: return "/"; case 0xC0: return "`";
    case 0xDB: return "["; case 0xDC: return "\\"; case 0xDD: return "]"; case 0xDE: return "'";
    case 0: return "-";
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { b[0] = (char)vk; b[1] = 0; return b; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(b, sizeof b, "Num %d", vk - 0x60); return b; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(b, sizeof b, "F%d", vk - 0x6F); return b; }
    snprintf(b, sizeof b, "Key %02X", vk); return b;
}
const char* input_pad_label(u32 b) {
    switch (b) {
    case XB_A: return "A"; case XB_B: return "B"; case XB_X: return "X"; case XB_Y: return "Y"; case XB_START: return "Menu"; case XB_BACK: return "View";
    case XB_LB: return "LB"; case XB_RB: return "RB"; case XB_LT: return "LT"; case XB_RT: return "RT"; case XB_LS: return "L-Stick Click"; case XB_RS: return "R-Stick Click";
    case XB_DUP: return "D-Pad Up"; case XB_DDOWN: return "D-Pad Down"; case XB_DLEFT: return "D-Pad Left"; case XB_DRIGHT: return "D-Pad Right";
    case 0: return "-";
    }
    return "?";
}
int input_has_pad_binding(int a) { return a != IN_SUP && a != IN_SDOWN && a != IN_SLEFT && a != IN_SRIGHT && a != IN_CUP && a != IN_CDOWN && a != IN_CLEFT && a != IN_CRIGHT; }

static u8 axis(int neg, int pos) { return (u8)(0x80 + (pos ? 0x60 : 0) - (neg ? 0x60 : 0)); }
static u8 stick(int v) { int x = 0x80 + v / 256; if (x < 0) x = 0; if (x > 255) x = 255; return (u8)x; }

/* keys[256]: keyboard state; xb: XInput buttons (+XB_LT/XB_RT when pressed past half), or -1 without a controller.
   Returns the GameCube pad words; *ff = fast-forward held. */
void input_build(const u8* keys, long xb, int lt, int rt, int lx, int ly, int rx, int ry, u32* hi, u32* lo, int* ff) {
    load();
    u32 down = 0;   /* bit per action */
    for (int i = 0; i < IN_N; i++) {
        if (g_bind[i].vk && keys[g_bind[i].vk & 255]) down |= 1u << i;
        if (xb >= 0 && g_bind[i].pad && ((u32)xb & g_bind[i].pad) == g_bind[i].pad) down |= 1u << i;
    }
#define D(a) ((down >> (a)) & 1)
    u32 b0 = 0, b1 = 0x80; u8 tl = 0, tr = 0;
    if (D(IN_A)) b0 |= 0x01; if (D(IN_B)) b0 |= 0x02; if (D(IN_X)) b0 |= 0x04; if (D(IN_Y)) b0 |= 0x08; if (D(IN_START)) b0 |= 0x10;
    if (D(IN_DLEFT)) b1 |= 0x01; if (D(IN_DRIGHT)) b1 |= 0x02; if (D(IN_DDOWN)) b1 |= 0x04; if (D(IN_DUP)) b1 |= 0x08;
    if (D(IN_Z)) b1 |= 0x10; if (D(IN_R)) { b1 |= 0x20; tr = 0xFF; } if (D(IN_L)) { b1 |= 0x40; tl = 0xFF; }
    u8 sx = axis(D(IN_SLEFT), D(IN_SRIGHT)), sy = axis(D(IN_SDOWN), D(IN_SUP)), cx = axis(D(IN_CLEFT), D(IN_CRIGHT)), cy = axis(D(IN_CDOWN), D(IN_CUP));
    if (xb >= 0) {
        /* analog triggers: L/R analog values follow the trigger bound to them */
        if (g_bind[IN_L].pad == XB_LT && lt > 30 && !D(IN_L)) tl = (u8)lt;
        if (g_bind[IN_R].pad == XB_RT && rt > 30 && !D(IN_R)) tr = (u8)rt;
        if (g_bind[IN_L].pad == XB_RT && rt > 30 && !D(IN_L)) tl = (u8)rt;
        if (g_bind[IN_R].pad == XB_LT && lt > 30 && !D(IN_R)) tr = (u8)lt;
        if (abs(lx) > 7000 || abs(ly) > 7000) { sx = stick(lx); sy = stick(ly); }
        if (abs(rx) > 7000 || abs(ry) > 7000) { cx = stick(rx); cy = stick(ry); }
    }
    *hi = b0 << 24 | b1 << 16 | (u32)sx << 8 | sy;
    *lo = (u32)cx << 24 | (u32)cy << 16 | (u32)tl << 8 | tr;
    *ff = D(IN_FF);
#undef D
}
