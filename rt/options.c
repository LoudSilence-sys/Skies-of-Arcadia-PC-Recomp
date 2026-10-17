/* options.c - the options screen: opened from the title menu's "Options" entry or with F1 at any time.
   Drawn by the renderer over the game in the style of the game's own windows, in five pages (Display,
   Graphics, Gameplay, Audio, Controls).  Changes apply immediately where possible and are saved to
   settings.ini.  While it is open the game is paused and receives no controller input.
   Every option defaults to the original game's behaviour. */
#include "pc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void card_draw(float wx, float wy, float ww, float wh, float a);
static void card_input(unsigned press, int* sel);
static char g_mcn[127][48]; static int g_mcb[127], g_mcnum = -1, g_mcfree, g_mc_confirm = -1;
static char g_mc_status[160];

void gl_set_scale(int s);
void gl_set_widescreen(int on);
void ui_rect(float x, float y, float w, float h, const float* ct, const float* cb);
void ui_title(float x, float y, float w, float h, float a);
void ui_text(float x, float y, float size, const char* s, const float* c);
float ui_text_width(const char* s, float size);
WEAK void platform_set_fullscreen(int on) { (void)on; }
WEAK int platform_get_fullscreen(void) { return 0; }
WEAK void platform_set_vsync(int on) { (void)on; }
WEAK void dcsnd_set_enabled(int on) { (void)on; }
WEAK u32 platform_nav(void) { return 0; }
void quickstate_request_slot(int load, int slot); int quickstate_slot_info(int slot, char* when, int cap); void quickstate_slot_path(int slot, char* out, int cap);
void ui_image(float x, float y, float w, float h, unsigned tex, float a); unsigned gl_ui_texture(const u8* rgba, int w, int h); void gl_ui_texture_free(unsigned t);
u8* texpack_load_png(const char* path, int* w, int* h); void texpack_free(void* p);          /* raw keyboard/controller navigation bits (win32.c) */
WEAK int platform_take_key(void) { return 0; }     /* last key pressed (virtual-key code), cleared on read */
WEAK long platform_xbuttons(void) { return -1; }   /* raw controller buttons, -1 without a controller */
void interp_set(int on);
extern int g_aniso, g_encounter_rate, g_perf_overlay;
extern int g_vol_master, g_vol_music, g_vol_fx;
void dcsnd_volumes_changed(void);
WEAK void text_speed_changed(void) {}
WEAK void ws_hud_changed(void) {}

/* navigation bits */
enum { N_UP = 1, N_DOWN = 2, N_LEFT = 4, N_RIGHT = 8, N_OK = 16, N_BACK = 32, N_TABL = 64, N_TABR = 128, N_CLEAR = 256, N_ALT = 512, N_NATIVE = 0x80000000u };

/* ---------------- items ---------------- */
enum { K_CHOICE, K_SLIDER, K_ACTION, K_BIND };
typedef struct Item {
    const char* name; int kind; const char* key; int def;
    int n; const int* vals; const char* const* labels;   /* K_CHOICE: n values (vals, or 0..n-1) and their labels */
    const char* help;
    void (*apply)(int v);
    int (*get)(void);
    int arg;                                              /* K_BIND: action; K_ACTION: action id */
} Item;

static const char* const OFFON[] = {"Off", "On"};
static void ap_scale(int v) { gl_set_scale(v); }
static void ap_wide(int v) { gl_set_widescreen(v); }
static void ap_full(int v) { platform_set_fullscreen(v); }
static int get_full(void) { return platform_get_fullscreen(); }
static void ap_vsync(int v) { platform_set_vsync(v); }
static void ap_fps60(int v) { interp_set(v); }
static int g_restart_note;
static void ap_hz(int v) { (void)v; g_restart_note = 1; }
static void ap_dc(int v) { dcsnd_set_enabled(v); }
static void ap_msaa(int v) { gl_set_msaa(v); }
static void ap_aniso(int v) { g_aniso = v; }
static void ap_hd(int v) { (void)v; gl_texcache_clear(); }
static void ap_enc(int v) { g_encounter_rate = v; }
static void ap_perf(int v) { g_perf_overlay = v; }
static int get_perf(void) { return g_perf_overlay; }
static void ap_vol(void) { dcsnd_volumes_changed(); }
static void ap_vm(int v) { g_vol_master = v; ap_vol(); }
static void ap_vmu(int v) { g_vol_music = v; ap_vol(); }
static void ap_vfx(int v) { g_vol_fx = v; ap_vol(); }
static void ap_text(int v) { (void)v; text_speed_changed(); }
static void ap_wshud(int v) { (void)v; ws_hud_changed(); }
static void ap_layout(int v) { input_set_layout(v); }
static int get_slot(void) { return quickstate_get_slot(); }
static void ap_slot(int v) { quickstate_slot(v - quickstate_get_slot()); }

static const int RES_V[] = {1, 2, 3, 4, 5, 6};
static const char* const RES_L[] = {"1x", "2x", "3x", "4x", "5x", "6x"};
static const char* const HZ_L[] = {"50 Hz", "60 Hz"};
static const char* const DC_L[] = {"GameCube", "Dreamcast"};
static const int MSAA_V[] = {1, 2, 4, 8};
static const char* const MSAA_L[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
static const int ANI_V[] = {1, 2, 4, 8, 16};
static const char* const ANI_L[] = {"Off", "2x", "4x", "8x", "16x"};
static const char* const POST_L[] = {"Off", "FXAA", "Sharpen", "CRT"};
static const char* const ENC_L[] = {"Normal", "Half", "Quarter", "None"};
static const char* const TXT_L[] = {"Normal", "Fast", "Instant"};
static const char* const FFM_L[] = {"Hold", "Toggle"};
static const int FFS_V[] = {2, 3, 4, 6, 8};
static const char* const FFS_L[] = {"2x", "3x", "4x", "6x", "8x"};
static const int BS_V[] = {1, 2, 3, 4};
static const char* const BS_L[] = {"Normal", "2x", "3x", "4x"};
static const char* const HUD_L[] = {"Centered", "Screen edges"};
static const char* const LAY_L[] = {"Xbox", "GameCube"};
static const char* const PROMPT_L[] = {"Auto", "GameCube", "Xbox", "PlayStation", "Switch", "Keyboard"};
WEAK void prompts_changed(void) {}
static void ap_prompts(int v) { (void)v; prompts_changed(); }
static const int SLOT_V[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
static const char* const SLOT_L[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
enum { ACT_RESET_KEYS = 1, ACT_BACK };

#define CH(nm, k, d, arr, lab, hp, ap) {nm, K_CHOICE, k, d, (int)(sizeof lab / sizeof lab[0]), arr, lab, hp, ap}
static Item P_DISPLAY[] = {
    CH("Resolution", "resolution_scale", 3, RES_V, RES_L, "Internal rendering resolution, as a multiple of the original 640x480.", ap_scale),
    CH("Widescreen (16:9)", "widescreen", 0, 0, OFFON, "Shows more of the world at the sides. Menus and text stay 4:3.", ap_wide),
    CH("Widescreen HUD", "ws_hud", 0, 0, HUD_L, "In widescreen, the minimap and battle HUD sit at the screen edges instead of the 4:3 frame.", ap_wshud),
    {"Fullscreen", K_CHOICE, "fullscreen", 0, 2, 0, OFFON, "Borderless fullscreen. Alt+Enter or F11 also switches.", ap_full, get_full},
    CH("Integer scaling", "integer_scaling", 0, 0, OFFON, "Scale the picture by whole steps of 480 lines for even pixels (may leave a border).", 0),
    CH("V-Sync", "vsync", 0, 0, OFFON, "Synchronise with the display to avoid tearing.", ap_vsync),
    CH("Refresh rate", "pal60", 1, 0, HZ_L, "60 Hz runs the game at full speed (PAL60). Applies after a restart.", ap_hz),
    CH("Pause when in background", "pause_unfocused", 1, 0, OFFON, "Pause the game while the window is not focused.", 0),
};
static Item P_GRAPHICS[] = {
    CH("60 FPS (interpolated)", "fps60", 1, 0, OFFON, "Generates in-between frames so the game moves at 60 frames per second.", ap_fps60),
    CH("Anti-aliasing", "msaa", 1, MSAA_V, MSAA_L, "Smooths jagged edges. Higher settings cost more GPU time.", ap_msaa),
    CH("Texture filtering", "anisotropy", 4, ANI_V, ANI_L, "Anisotropic filtering keeps textures sharp at steep angles.", ap_aniso),
    CH("Output filter", "post_filter", 0, 0, POST_L, "FXAA smooths edges cheaply, Sharpen crisps the picture, CRT imitates a television.", 0),
    CH("HD texture packs", "hd_textures", 1, 0, OFFON, "Loads Dolphin-format texture packs from Load/Textures/GEAP8P.", ap_hd),
    {"Performance overlay", K_CHOICE, 0, 0, 2, 0, OFFON, "Frame rate and timing in the top-left corner (F3).", ap_perf, get_perf},
};
static Item P_GAMEPLAY[] = {
    CH("Encounter rate", "encounter_rate", 0, 0, ENC_L, "How often random battles happen. Normal is the original rate.", ap_enc),
    CH("Battle speed", "battle_speed", 1, BS_V, BS_L, "Speeds up battle animations. Choosing commands stays at normal speed.", 0),
    CH("Text speed", "text_speed", 0, 0, TXT_L, "How fast dialogue and battle messages appear.", ap_text),
    CH("Uncensored text", "uncensored_text", 0, 0, OFFON, "Restores the original \"rum\" that the western releases renamed \"loqua\".", 0),
    CH("Fast-forward", "ff_toggle", 0, 0, FFM_L, "Hold the fast-forward button (Tab / right stick), or press it to switch.", 0),
    CH("Fast-forward speed", "ff_speed", 3, FFS_V, FFS_L, "How much faster the game runs while fast-forwarding.", 0),
    CH("Rewind", "rewind", 0, 0, OFFON, "Keeps the last minute of play: hold Backspace (or View / Create) to step back.", 0),
    {"Save state slot", K_CHOICE, "quickstate_slot", 1, 10, SLOT_V, SLOT_L, "F5 saves the exact moment to this slot, F9 loads it. F6/F7 also change slot.", ap_slot, get_slot},
};
static Item P_AUDIO[] = {
    CH("Soundtrack", "dc_sound", 1, 0, DC_L, "Dreamcast music and sound (needs the Dreamcast discs), or the GameCube sound.", ap_dc),
    {"Master volume", K_SLIDER, "volume_master", 100, 0, 0, 0, "Overall volume.", ap_vm},
    {"Music volume", K_SLIDER, "volume_music", 100, 0, 0, 0, "Volume of the music.", ap_vmu},
    {"Effects volume", K_SLIDER, "volume_effects", 100, 0, 0, 0, "Volume of sound effects and voices.", ap_vfx},
};
static Item P_CONTROLS[5 + IN_N];
static int g_ctl_n;
static void controls_init(void) {
    if (g_ctl_n) return;
    int k = 0;
    P_CONTROLS[k++] = (Item){"Controller layout", K_CHOICE, "pad_layout", 0, 2, 0, LAY_L, "Xbox: A confirms, B cancels. GameCube: buttons by position, as on the original pad.", ap_layout};
    P_CONTROLS[k++] = (Item){"Button names", K_CHOICE, "button_prompts", 0, 6, 0, PROMPT_L, "How the game's messages name buttons. Auto follows the keyboard or controller you last used.", ap_prompts};
    P_CONTROLS[k++] = (Item){"Vibration", K_CHOICE, "vibration", 1, 2, 0, OFFON, "Controller rumble where the game uses it.", 0};
    static const int order[IN_N] = {IN_A, IN_B, IN_X, IN_Y, IN_START, IN_L, IN_R, IN_Z, IN_SUP, IN_SDOWN, IN_SLEFT, IN_SRIGHT,
                                   IN_CUP, IN_CDOWN, IN_CLEFT, IN_CRIGHT, IN_DUP, IN_DDOWN, IN_DLEFT, IN_DRIGHT, IN_FF, IN_REWIND};
    for (int i = 0; i < IN_N; i++) P_CONTROLS[k++] = (Item){input_name(order[i]), K_BIND, 0, 0, 0, 0, 0, "The keyboard key and controller button for each GameCube button.", 0, 0, order[i]};
    P_CONTROLS[k++] = (Item){"Reset keyboard to defaults", K_ACTION, 0, 0, 0, 0, 0, "Restores the default keys. The controller layout above resets the controller.", 0, 0, ACT_RESET_KEYS};
    g_ctl_n = k;
}
typedef struct { const char* name; Item* items; int n; } Page;
static void g_mcnum_reset(void);
static Page g_pages[7] = {
    {"Display", P_DISPLAY, sizeof P_DISPLAY / sizeof P_DISPLAY[0]},
    {"Graphics", P_GRAPHICS, sizeof P_GRAPHICS / sizeof P_GRAPHICS[0]},
    {"Gameplay", P_GAMEPLAY, sizeof P_GAMEPLAY / sizeof P_GAMEPLAY[0]},
    {"Audio", P_AUDIO, sizeof P_AUDIO / sizeof P_AUDIO[0]},
    {"Controls", P_CONTROLS, 0},
    {"States", 0, 10},
    {"Card", 0, 0},
};
#define NPAGE 7
#define PAGE_STATES 5
#define PAGE_CARD 6

static int item_val(const Item* it) { return it->get ? it->get() : it->key ? settings_get(it->key, it->def) : it->def; }
static int choice_index(const Item* it) {
    int v = item_val(it);
    if (!it->vals) return v < 0 ? 0 : v >= it->n ? it->n - 1 : v;
    int best = 0; for (int i = 0; i < it->n; i++) if (it->vals[i] <= v) best = i;   /* nearest at or below */
    return best;
}
static void item_change(Item* it, int dir) {
    if (it->kind == K_CHOICE) {
        int i = choice_index(it) + dir; if (i < 0) i = it->n - 1; if (i >= it->n) i = 0;
        int v = it->vals ? it->vals[i] : i;
        if (it->key && !it->get) settings_set(it->key, v);
        else if (it->key && it->apply != ap_slot) settings_set(it->key, v);
        if (it->apply) it->apply(v);
    } else if (it->kind == K_SLIDER) {
        int v = item_val(it) + dir * 5; if (v < 0) v = 0; if (v > 100) v = 100;
        settings_set(it->key, v); if (it->apply) it->apply(v);
    }
}

/* ---------------- state ---------------- */
static int g_open, g_page, g_sel[NPAGE], g_scroll[NPAGE], g_fade;
static int g_tabrow;              /* the page tabs have the focus */
static int g_capture;             /* waiting for a key or button for this action (+1), 0 = not */
static int g_capture_t; static long g_capture_xb;
static u32 g_prev = 0xFFFFFFFFu; static int g_rep;

int options_is_open(void) { return g_open; }
void options_open(void) {
    if (g_open) return;
    controls_init(); g_pages[4].n = g_ctl_n;
    g_open = 1; g_fade = 0; g_prev = 0xFFFFFFFFu; g_capture = 0; g_tabrow = 0; g_mcnum_reset();
    platform_take_key();
    rt_log("options: open\n");
}
static void options_close(void) { g_open = 0; g_capture = 0; settings_save(); rt_log("options: closed, settings saved\n"); }
void options_toggle(void) { if (g_open) options_close(); else options_open(); }

static void capture_poll(void) {
    Item* it = &g_pages[g_page].items[g_sel[g_page]];
    int a = it->arg;
    int vk = platform_take_key();
    if (vk == 0x1B) { g_capture = 0; return; }                                      /* Esc: cancel */
    if (vk && !(vk >= 0x70 && vk <= 0x87)) {                                         /* function keys are reserved */
        input_set_key(a, vk); osd_message("%s: %s", input_name(a), input_key_label(vk)); g_capture = 0; g_prev = 0xFFFFFFFFu; return;
    }
    long xb = platform_xbuttons();
    if (xb >= 0 && input_has_pad_binding(a)) {
        long press = xb & ~g_capture_xb; g_capture_xb = xb;
        for (u32 b = 1; b <= 0x20000; b <<= 1) if (press & b) {
            if (b == 0x20 /* View */) { g_capture = 0; g_prev = 0xFFFFFFFFu; return; }
            input_set_pad(a, b); osd_message("%s: %s", input_name(a), input_pad_label(b)); g_capture = 0; g_prev = 0xFFFFFFFFu; return;
        }
    }
    if (++g_capture_t > 60 * 8) g_capture = 0;   /* nothing pressed for 8 s */
}

/* called once per video field with the GameCube pad state (hi word); the platform adds raw navigation */
void options_input(u32 hi) {
    {   /* tests: SOA_OPTTEST=frame,page,row opens the options on that page and row */
        static int init, fr = -1, pg, row; extern int g_frame_no_pub;
        if (!init) { init = 1; const char* e = getenv("SOA_OPTTEST"); if (e) sscanf(e, "%d,%d,%d", &fr, &pg, &row); }
        if (fr >= 0 && g_frame_no_pub >= fr) { fr = -1; options_open(); g_page = pg; g_sel[pg] = row;
            const char* ops = getenv("SOA_CARDOPS");   /* tests: e.g. "e0,d0,d0,i,b" export / delete (twice = confirm) / import / backup */
            for (; ops && *ops; ops++) { int k = atoi(ops + 1); g_sel[PAGE_CARD] = k;
                if (*ops == 'e') card_input(N_OK, &g_sel[PAGE_CARD]); else if (*ops == 'd') card_input(N_CLEAR, &g_sel[PAGE_CARD]);
                else if (*ops == 'i') { g_sel[PAGE_CARD] = g_mcnum; card_input(N_OK, &g_sel[PAGE_CARD]); } else if (*ops == 'b') { g_sel[PAGE_CARD] = g_mcnum + 1; card_input(N_OK, &g_sel[PAGE_CARD]); }
                else continue;
                rt_log("cardop %c: %s\n", *ops, g_mc_status); while (ops[1] && ops[1] != ',') ops++; }
            g_sel[PAGE_CARD] = 0; }
    }
    if (!g_open) return;
    g_fade++;
    if (g_capture) { capture_poll(); return; }
    u32 b = hi >> 16, nav = platform_nav();
    int sx = (hi >> 8) & 0xFF, sy = hi & 0xFF;
    if (nav & N_NATIVE) { nav &= ~N_NATIVE; b = 0; sx = sy = 0x80; }   /* the platform reads keyboard and controller itself */
    if (b & 0x0008 || sy > 0xB0) nav |= N_UP;
    if (b & 0x0004 || sy < 0x50) nav |= N_DOWN;
    if (b & 0x0001 || sx < 0x50) nav |= N_LEFT;
    if (b & 0x0002 || sx > 0xB0) nav |= N_RIGHT;
    if (b & 0x0100) nav |= N_OK;
    if (b & (0x0200 | 0x1000)) nav |= N_BACK;
    if (b & 0x0800) nav |= N_ALT;    /* Y */
    if (b & 0x0040) nav |= N_TABL;   /* L */
    if (b & 0x0020) nav |= N_TABR;   /* R */
    if (g_prev == 0xFFFFFFFFu) { g_prev = nav; return; }   /* ignore what was held when opening */
    u32 press = nav & ~g_prev;
    if (nav & (N_UP | N_DOWN | N_LEFT | N_RIGHT) && nav == g_prev) { if (++g_rep > 18 && (g_rep & 3) == 0) press |= nav & (N_UP | N_DOWN | N_LEFT | N_RIGHT); }   /* auto-repeat */
    else g_rep = 0;
    g_prev = nav;
    Page* pg = &g_pages[g_page];
    int* sel = &g_sel[g_page];
    if (press & N_TABL) { g_page = (g_page + NPAGE - 1) % NPAGE; return; }
    if (press & N_TABR) { g_page = (g_page + 1) % NPAGE; return; }
    if (g_tabrow) {
        if (press & N_LEFT) g_page = (g_page + NPAGE - 1) % NPAGE;
        if (press & N_RIGHT) g_page = (g_page + 1) % NPAGE;
        if (press & (N_DOWN | N_OK)) { g_tabrow = 0; g_sel[g_page] = 0; }
        if (press & N_UP) { g_tabrow = 0; g_sel[g_page] = g_page == PAGE_STATES ? 5 : g_pages[g_page].n; }   /* wraps to "Back" (bottom row of the states grid) */
        if (press & N_BACK) options_close();
        return;
    }
    if (g_page == PAGE_CARD) { card_input(press, sel); return; }
    if (g_page == PAGE_STATES) {   /* 5 x 2 grid of slots */
        if (press & N_LEFT) *sel = (*sel + 9) % 10;
        if (press & N_RIGHT) *sel = (*sel + 1) % 10;
        if (press & N_UP) { if (*sel < 5) g_tabrow = 1; else *sel -= 5; }
        if (press & N_DOWN) { if (*sel < 5) *sel += 5; else g_tabrow = 1; }
        char when[40];
        if ((press & N_OK) && quickstate_slot_info(*sel + 1, when, sizeof when)) { options_close(); quickstate_request_slot(1, *sel + 1); }
        else if (press & N_ALT) { options_close(); quickstate_request_slot(0, *sel + 1); }
        else if (press & N_BACK) options_close();
        return;
    }
    int nrow = pg->n + 1;   /* + Back */
    if (press & N_UP) { if (*sel == 0) g_tabrow = 1; else (*sel)--; }
    if (press & N_DOWN) { if (*sel == nrow - 1) g_tabrow = 1; else (*sel)++; }
    if (*sel >= nrow) *sel = nrow - 1;
    if (*sel == pg->n) { if (press & N_OK) options_close(); else if (press & N_BACK) options_close(); return; }
    Item* it = &pg->items[*sel];
    if (press & (N_LEFT | N_RIGHT)) item_change(it, press & N_RIGHT ? 1 : -1);
    if (press & N_OK) {
        if (it->kind == K_CHOICE) item_change(it, 1);
        else if (it->kind == K_BIND) { g_capture = 1; g_capture_t = 0; g_capture_xb = platform_xbuttons(); platform_take_key(); }
        else if (it->kind == K_ACTION && it->arg == ACT_RESET_KEYS) { input_reset_keys(); osd_message("Keyboard controls reset to defaults"); }
    }
    if ((press & N_CLEAR) && it->kind == K_BIND) input_set_key(it->arg, 0);
    if (press & N_BACK) options_close();
}

/* ---------------- drawing (virtual 640x480 coordinates inside the given 4:3 rectangle) ---------------- */
static void states_draw(float wx, float wy, float ww, float wh, float a);

static float X0, Y0, K;
static void rect(float x, float y, float w, float h, const float* a, const float* b) { ui_rect(X0 + x * K, Y0 + y * K, w * K, h * K, a, b); }
static void text(float x, float y, float sz, const char* s, const float* c) {
    float sh[4] = {0.02f, 0.03f, 0.12f, c[3] * 0.9f};
    ui_text(X0 + (x + 1.5f) * K, Y0 + (y + 1.5f) * K, sz * K, s, sh);   /* drop shadow, as the game's text */
    ui_text(X0 + x * K, Y0 + y * K, sz * K, s, c);
}
static float tw(const char* s, float sz) { return ui_text_width(s, sz * K) / K; }

static void item_value_text(const Item* it, char* v, int cap) {
    if (it->kind == K_SLIDER) { snprintf(v, cap, "%d%%", item_val(it)); return; }
    if (it->kind == K_BIND) {
        int a = it->arg; const char* kl = input_key_label(input_key(a));
        if (input_has_pad_binding(a)) snprintf(v, cap, "%s   |   %s", kl, input_pad_label(input_pad(a)));
        else snprintf(v, cap, "%s   |   %s", kl, a >= IN_CUP && a <= IN_CRIGHT ? "Right stick" : "Left stick");
        return;
    }
    if (it->kind == K_ACTION) { v[0] = 0; return; }
    int i = choice_index(it);
    if (it->vals == RES_V) { int ws = settings_get("widescreen", 0), n = it->vals[i]; snprintf(v, cap, "%dx  %dx%d%s", n, ws ? 640 * n * 4 / 3 : 640 * n, 480 * n, n == 5 ? " (4K)" : ""); return; }
    snprintf(v, cap, "%s", it->labels[i]);
}

void options_draw(float x, float y, float w, float h) {
    (void)h; X0 = x; Y0 = y; K = w / 640.f;
    if (!g_ctl_n) { controls_init(); g_pages[4].n = g_ctl_n; }
    float a = g_fade > 8 ? 1.f : g_fade / 8.f;
    float dim[4] = {0, 0, 0.05f, 0.45f * a};
    rect(0, 0, 640, 480, dim, dim);
    /* window: the game's translucent blue gradient with a light rim */
    float rim[4] = {0.62f, 0.78f, 1.f, 0.95f * a}, top[4] = {0.10f, 0.22f, 0.62f, 0.88f * a}, bot[4] = {0.03f, 0.08f, 0.32f, 0.88f * a};
    float wx = 44, wy = 92, ww = 552, wh = 358;
    rect(wx - 3, wy - 3, ww + 6, wh + 6, rim, rim);
    float dark[4] = {0.02f, 0.05f, 0.20f, 0.95f * a};
    rect(wx - 1, wy - 1, ww + 2, wh + 2, dark, dark);
    rect(wx, wy, ww, wh, top, bot);
    ui_title(X0 + (320 - 104) * K, Y0 + 34 * K, 208 * K, 54 * K, a);
    float white[4] = {1, 1, 1, a}, gold[4] = {1.f, 0.90f, 0.35f, a}, grey[4] = {0.66f, 0.72f, 0.86f, a}, dimw[4] = {0.75f, 0.80f, 0.92f, a};
    /* tabs */
    /* tabs: widths follow the labels */
    { const float fs = 15.f; float tot = 0; for (int p = 0; p < NPAGE; p++) tot += tw(g_pages[p].name, fs);
      float gap = (ww - 20 - tot) / NPAGE, bx = wx + 10;
      for (int p = 0; p < NPAGE; p++) {
          const char* nm = g_pages[p].name; float w = tw(nm, fs) + gap;
          if (p == g_page) {
              float t0[4] = {0.50f, 0.72f, 1.f, (g_tabrow ? 0.55f : 0.35f) * a}, t1[4] = {0.25f, 0.45f, 0.95f, 0.20f * a};
              rect(bx + 1, wy + 8, w - 2, 30, t0, t1);
              float ul[4] = {1.f, 0.90f, 0.35f, a}; rect(bx + gap / 2 - 2, wy + 36, w - gap + 4, 2, ul, ul);
          }
          text(bx + gap / 2, wy + 14, fs, nm, p == g_page ? gold : dimw);
          bx += w;
      } }
    float sep[4] = {0.62f, 0.78f, 1.f, 0.35f * a}; rect(wx + 10, wy + 44, ww - 20, 1, sep, sep);
    if (g_page == PAGE_STATES) { states_draw(wx, wy, ww, wh, a); return; }
    if (g_page == PAGE_CARD) { card_draw(wx, wy, ww, wh, a); return; }
    /* rows */
    Page* pg = &g_pages[g_page];
    const int VIS = 8; const float RH = 29, ry0 = wy + 54;
    int nrow = pg->n + 1, sel = g_sel[g_page];
    int* sc = &g_scroll[g_page];
    if (sel < *sc) *sc = sel; if (sel >= *sc + VIS) *sc = sel - VIS + 1;
    if (*sc > nrow - VIS) *sc = nrow - VIS; if (*sc < 0) *sc = 0;
    for (int r = 0; r < VIS && *sc + r < nrow; r++) {
        int i = *sc + r; float ry = ry0 + r * RH;
        int focus = i == sel && !g_tabrow;
        if (focus) {
            float h0[4] = {0.45f, 0.68f, 1.f, 0.40f * a}, h1[4] = {0.25f, 0.45f, 0.95f, 0.25f * a};
            rect(wx + 10, ry - 3, ww - 20, RH - 2, h0, h1);
            text(wx + 16, ry - 1, 19, ">", gold);
        }
        if (i == pg->n) { text(320 - tw("Back", 19) / 2, ry, 19, "Back", focus ? gold : white); continue; }
        Item* it = &pg->items[i];
        text(wx + 36, ry, 19, it->name, it->kind == K_ACTION && focus ? gold : white);
        char v[64]; item_value_text(it, v, sizeof v);
        if (it->kind == K_SLIDER) {   /* bar */
            float bw = 150, bx = wx + ww - 30 - bw - 52, by = ry + 7;
            float bg[4] = {0.02f, 0.05f, 0.20f, 0.9f * a}, f0[4] = {0.55f, 0.80f, 1.f, a}, f1[4] = {0.30f, 0.55f, 1.f, a};
            rect(bx, by, bw, 9, bg, bg); rect(bx, by, bw * item_val(it) / 100.f, 9, f0, f1);
            text(wx + ww - 30 - tw(v, 19), ry, 19, v, focus ? gold : white);
            continue;
        }
        if (it->kind == K_BIND) {   /* two columns: keyboard, controller */
            const float* col = focus ? gold : white;
            if (focus && g_capture) { const char* m = "Press a key or button..."; text(wx + ww - 24 - tw(m, 19), ry, 19, m, gold); continue; }
            int a2 = it->arg; const char* kl = input_key_label(input_key(a2));
            const char* pl = input_has_pad_binding(a2) ? input_pad_label(input_pad(a2)) : a2 >= IN_CUP && a2 <= IN_CRIGHT ? "Right stick" : "Left stick";
            float c1 = wx + ww - 190, c2 = wx + ww - 76;
            text(c1 - tw(kl, 19) / 2, ry, 19, kl, col);
            text(c2 - tw(pl, 19) / 2, ry, 19, pl, input_has_pad_binding(a2) ? col : grey);
            continue;
        }
        if (!v[0]) continue;
        char vv[80]; snprintf(vv, sizeof vv, focus && it->kind == K_CHOICE ? "< %s >" : "%s", v);
        text(wx + ww - 24 - tw(vv, 19), ry, 19, vv, focus ? gold : white);
    }
    if (nrow > VIS) {   /* scroll bar */
        float trk[4] = {0.02f, 0.05f, 0.20f, 0.7f * a}, th[4] = {0.62f, 0.78f, 1.f, 0.8f * a};
        float sh = VIS * RH, hh = sh * VIS / nrow, hy = ry0 - 3 + (sh - hh) * *sc / (nrow - VIS);
        rect(wx + ww - 8, ry0 - 3, 3, sh, trk, trk); rect(wx + ww - 8, hy, 3, hh, th, th);
    }
    /* help for the focused item, then the key legend */
    rect(wx + 10, wy + wh - 62, ww - 20, 1, sep, sep);
    const char* help = g_tabrow ? "Choose a page." : sel < pg->n ? pg->items[sel].help : "Close the options (settings are saved).";
    if (g_capture) help = "Press the new key or controller button.   Esc / View: cancel";
    if (g_restart_note && g_page == 0 && !g_capture) help = "Refresh rate applies after a restart.";
    text(wx + 18, wy + wh - 54, 15, help, grey);
    const char* hint = g_page == 4 ? "L/R or Q/E: page    A: rebind    Delete: clear key    B: back" : "L/R or Q/E: page    Left/Right: change    B: back";
    text(320 - tw(hint, 14) / 2, wy + wh - 26, 14, hint, dimw);
    const char* keys = "F5 Save state    F9 Load state    F6/F7 Slot    F12 Screenshot    F3 Performance";
    float kw = tw(keys, 12); text(320 - kw / 2, wy + wh + 8, 12, keys, grey);
}

/* ---------------- the states page: 10 save-state slots with thumbnails ---------------- */
static unsigned g_th[10]; static int g_th_tw[10], g_th_th[10]; static long long g_th_stamp[10]; static volatile int g_th_dirty = 1;
void states_page_dirty(void) { g_th_dirty = 1; }
#include <sys/stat.h>
static void states_refresh(void) {
    g_th_dirty = 0;
    for (int i = 0; i < 10; i++) {
        char p[80], tp[96]; quickstate_slot_path(i + 1, p, sizeof p); snprintf(tp, sizeof tp, "%s.png", p);
        struct stat st; long long stamp = stat(tp, &st) ? 0 : (long long)st.st_mtime;
        if (stat(p, &st)) stamp = 0;
        if (stamp == g_th_stamp[i] && (g_th[i] || !stamp)) continue;
        if (g_th[i]) { gl_ui_texture_free(g_th[i]); g_th[i] = 0; }
        g_th_stamp[i] = stamp;
        if (!stamp) continue;
        int w, h; u8* px = texpack_load_png(tp, &w, &h);
        if (px) { g_th[i] = gl_ui_texture(px, w, h); g_th_tw[i] = w; g_th_th[i] = h; texpack_free(px); }
    }
}
static void states_draw(float wx, float wy, float ww, float wh, float a) {
    if (g_th_dirty) states_refresh();
    float white[4] = {1, 1, 1, a}, gold[4] = {1.f, 0.90f, 0.35f, a}, grey[4] = {0.66f, 0.72f, 0.86f, a}, dimw[4] = {0.75f, 0.80f, 0.92f, a};
    int sel = g_sel[PAGE_STATES], cur = quickstate_get_slot();
    float cw = 100, chh = 75, gx0 = wx + (ww - 5 * cw - 4 * 6) / 2, gy0 = wy + 58;
    for (int i = 0; i < 10; i++) {
        float x = gx0 + (i % 5) * (cw + 6), y = gy0 + (i / 5) * (chh + 50);
        int focus = i == sel && !g_tabrow;
        float rim[4] = {0.62f, 0.78f, 1.f, (focus ? 1.f : 0.45f) * a}, bg[4] = {0.02f, 0.05f, 0.20f, 0.9f * a};
        if (focus) { float g4[4] = {1.f, 0.90f, 0.35f, a}; rect(x - 3, y - 3, cw + 6, chh + 6, g4, g4); }
        else rect(x - 1, y - 1, cw + 2, chh + 2, rim, rim);
        rect(x, y, cw, chh, bg, bg);
        char when[40]; int has = quickstate_slot_info(i + 1, when, sizeof when);
        if (has && g_th[i]) {   /* letterbox the thumbnail into the cell */
            float ar = (float)g_th_tw[i] / g_th_th[i], dw = cw, dh = cw / ar; if (dh > chh) { dh = chh; dw = chh * ar; }
            ui_image(X0 + (x + (cw - dw) / 2) * K, Y0 + (y + (chh - dh) / 2) * K, dw * K, dh * K, g_th[i], a);
        } else { const char* e = has ? "No picture" : "Empty"; text(x + cw / 2 - tw(e, 14) / 2, y + chh / 2 - 9, 14, e, grey); }
        char lab[24]; snprintf(lab, sizeof lab, i + 1 == cur ? "Slot %d *" : "Slot %d", i + 1);
        text(x + cw / 2 - tw(lab, 15) / 2, y + chh + 4, 15, lab, focus ? gold : white);
        if (has) text(x + cw / 2 - tw(when, 12) / 2, y + chh + 22, 12, when, dimw);
    }
    float sep[4] = {0.62f, 0.78f, 1.f, 0.35f * a}; rect(wx + 10, wy + wh - 62, ww - 20, 1, sep, sep);
    char help[120]; snprintf(help, sizeof help, "Slot %d * is the one F5 / F9 use (F6 / F7 change it).", cur);
    text(wx + 18, wy + wh - 54, 15, help, grey);
    const char* hint = "A: load    Y / S: save here    L/R or Q/E: page    B: back";
    text(320 - tw(hint, 14) / 2, wy + wh - 26, 14, hint, dimw);
}

/* ---------------- the memory card page ---------------- */
int mc_list(char names[][48], int* blocks, int max, int* free_blocks); int mc_export(int k, char* out, int cap); int mc_delete(int k);
int mc_import_folder(int* skipped, int* noroom); int mc_backup(char* out, int cap);
static void card_refresh(void) { g_mcnum = mc_list(g_mcn, g_mcb, 127, &g_mcfree); }
static void card_input(unsigned press, int* sel) {
    if (g_mcnum < 0) card_refresh();
    int n = g_mcnum < 0 ? 0 : g_mcnum, rows = n + 2;   /* saves, import, backup */
    if (press & N_UP) { if (*sel == 0) g_tabrow = 1; else (*sel)--; g_mc_confirm = -1; }
    if (press & N_DOWN) { if (*sel < rows - 1) (*sel)++; else g_tabrow = 1; g_mc_confirm = -1; }
    if (*sel >= rows) *sel = rows - 1;
    if (press & N_OK) {
        if (*sel < n) { char p[200]; if (mc_export(*sel, p, sizeof p)) snprintf(g_mc_status, sizeof g_mc_status, "Exported to %s", p); else snprintf(g_mc_status, sizeof g_mc_status, "Export failed"); }
        else if (*sel == n) { int sk, nr; int k = mc_import_folder(&sk, &nr); card_refresh();
            snprintf(g_mc_status, sizeof g_mc_status, "Imported %d save%s%s%s", k, k == 1 ? "" : "s", sk ? " (others already on the card)" : "", nr ? " - not enough room for some" : ""); }
        else { char p[200]; if (mc_backup(p, sizeof p)) snprintf(g_mc_status, sizeof g_mc_status, "Backed up to %s", p); else snprintf(g_mc_status, sizeof g_mc_status, "Backup failed"); }
    }
    if ((press & (N_CLEAR | N_ALT)) && *sel < n) {
        if (g_mc_confirm != *sel) { g_mc_confirm = *sel; snprintf(g_mc_status, sizeof g_mc_status, "Press again to delete \"%s\" from the card", g_mcn[*sel] + 5); }
        else { mc_delete(*sel); g_mc_confirm = -1; card_refresh(); snprintf(g_mc_status, sizeof g_mc_status, "Deleted"); }
    }
    if (press & N_BACK) options_close();
}
static void card_draw(float wx, float wy, float ww, float wh, float a) {
    if (g_mcnum < 0) card_refresh();
    float white[4] = {1, 1, 1, a}, gold[4] = {1.f, 0.90f, 0.35f, a}, grey[4] = {0.66f, 0.72f, 0.86f, a}, dimw[4] = {0.75f, 0.80f, 0.92f, a}, red[4] = {1.f, 0.55f, 0.45f, a};
    int n = g_mcnum < 0 ? 0 : g_mcnum, rows = n + 2, sel = g_sel[PAGE_CARD];
    const int VIS = 8; const float RH = 29, ry0 = wy + 54;
    int* sc = &g_scroll[PAGE_CARD]; if (sel < *sc) *sc = sel; if (sel >= *sc + VIS) *sc = sel - VIS + 1; if (*sc > rows - VIS) *sc = rows - VIS; if (*sc < 0) *sc = 0;
    for (int r = 0; r < VIS && *sc + r < rows; r++) {
        int i = *sc + r; float ry = ry0 + r * RH; int focus = i == sel && !g_tabrow;
        if (focus) { float h0[4] = {0.45f, 0.68f, 1.f, 0.40f * a}, h1[4] = {0.25f, 0.45f, 0.95f, 0.25f * a}; rect(wx + 10, ry - 3, ww - 20, RH - 2, h0, h1); text(wx + 16, ry - 1, 19, ">", gold); }
        if (i < n) {
            text(wx + 36, ry + 1, 17, g_mcn[i] + 5, i == g_mc_confirm ? red : focus ? gold : white);
            char b[24]; snprintf(b, sizeof b, "%d block%s", g_mcb[i], g_mcb[i] == 1 ? "" : "s"); text(wx + ww - 24 - tw(b, 15), ry + 2, 15, b, dimw);
        } else text(wx + 36, ry, 19, i == n ? "Import .gci saves from the GCI folder" : "Back up the memory card", focus ? gold : white);
    }
    if (g_mcnum < 0) text(wx + 36, ry0, 19, "No memory card", grey);
    float sep[4] = {0.62f, 0.78f, 1.f, 0.35f * a}; rect(wx + 10, wy + wh - 62, ww - 20, 1, sep, sep);
    char st[200]; if (g_mc_status[0]) snprintf(st, sizeof st, "%s", g_mc_status); else snprintf(st, sizeof st, "Slot A: %d saves, %d blocks free. Use this outside the game's save screens.", n, g_mcfree);
    text(wx + 18, wy + wh - 54, 14, st, grey);
    const char* hint = "A: export / run    Delete / Y: delete save    B: back";
    text(320 - tw(hint, 14) / 2, wy + wh - 26, 14, hint, dimw);
}
static void g_mcnum_reset(void) { g_mcnum = -1; g_mc_status[0] = 0; g_mc_confirm = -1; }
