/* win32.c - Windows platform layer: window + WGL OpenGL 3.3 context, presentation, keyboard/XInput input */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pc.h"
#include <mmsystem.h>
#include <stdio.h>
#include "rt.h"
#include "glmini.h"

void rt_log(const char* fmt, ...);
void si_set_pad(u32 hi, u32 lo);

static HWND g_hwnd; static HDC g_hdc; static HGLRC g_glrc;
static int g_win_w = 1280, g_win_h = 960;
static HMODULE g_opengl32;
static u8 g_keys[256];
int g_last_key;          /* last key pressed (options: rebinding) */
static int g_focused = 1;
long g_xbuttons = -1;     /* XInput buttons this frame, -1 without a controller (options: rebinding) */
static int g_lx, g_ly;       /* left stick, for options navigation */
int settings_get(const char* key, int def);
static int g_fullscreen; static RECT g_saved_rect;
static void set_fullscreen(int on);
void platform_set_fullscreen(int on) { set_fullscreen(on); }
int platform_get_fullscreen(void) { return g_fullscreen; }
void platform_set_vsync(int on) { typedef BOOL (WINAPI* PFNSWAP)(int); PFNSWAP swap = (PFNSWAP)wglGetProcAddress("wglSwapIntervalEXT"); if (swap) swap(on ? 1 : 0); }
static void set_fullscreen(int on) {   /* borderless fullscreen on the window's monitor */
    if (on == g_fullscreen) return;
    if (on) {
        GetWindowRect(g_hwnd, &g_saved_rect);
        MONITORINFO mi = {sizeof mi}; GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, 0, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left, g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    g_fullscreen = on;
}

static void* win_getproc(const char* n) {
    void* p = (void*)wglGetProcAddress(n);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) p = (void*)GetProcAddress(g_opengl32, n);
    return p;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: ExitProcess(0); return 0;
    case WM_SIZE: g_win_w = LOWORD(l); g_win_h = HIWORD(l); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F1) { extern void options_toggle(void); if (!(l & (1 << 30))) options_toggle(); return 0; }
        if (w == VK_F3) { extern int g_perf_overlay; if (!(l & (1 << 30))) g_perf_overlay = !g_perf_overlay; return 0; }   /* performance readout */
        if (!(l & (1 << 30))) g_last_key = (int)(w & 255);   /* for rebinding in the options screen */
        if (w == VK_F5 || w == VK_F9) { if (!(l & (1 << 30))) { extern void quickstate_request(int load); quickstate_request(w == VK_F9); } return 0; }
        if (w == VK_F6 || w == VK_F7) { if (!(l & (1 << 30))) { extern void quickstate_slot(int d); quickstate_slot(w == VK_F7 ? 1 : -1); } return 0; }
        if (w == VK_F12 || w == VK_SNAPSHOT) { if (!(l & (1 << 30))) { extern void screenshot_request(void); screenshot_request(); } return 0; }
        if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) { if (!(l & (1 << 30))) set_fullscreen(!g_fullscreen); return 0; }   /* F11 / Alt+Enter */
        g_keys[w & 255] = 1; return 0;
    case WM_KEYUP: case WM_SYSKEYUP: g_keys[w & 255] = 0; return 0;
    case WM_KILLFOCUS: memset(g_keys, 0, sizeof g_keys); g_focused = 0; return 0;
    case WM_SETFOCUS: g_focused = 1; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

typedef HGLRC (WINAPI* PFNCREATECTX)(HDC, HGLRC, const int*);
int platform_init(void) {
    HINSTANCE inst = GetModuleHandleA(0);
    timeBeginPeriod(1);   /* 1 ms scheduler resolution (default is ~15.6 ms: frame waits overshoot by a whole field) */
    g_win_w = settings_get("window_width", 1280); g_win_h = settings_get("window_height", 960);
    if (g_win_w < 320) g_win_w = 320; if (g_win_h < 240) g_win_h = 240;
    WNDCLASSA wc = {0}; wc.style = CS_OWNDC; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "SoAWin";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, g_win_w, g_win_h}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);
    g_hwnd = CreateWindowA("SoAWin", "Skies of Arcadia Legends (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    SetPixelFormat(g_hdc, ChoosePixelFormat(g_hdc, &pfd), &pfd);
    HGLRC tmp = wglCreateContext(g_hdc); wglMakeCurrent(g_hdc, tmp);
    PFNCREATECTX create = (PFNCREATECTX)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create) {
        int attrs[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};   /* major, minor, core profile */
        g_glrc = create(g_hdc, 0, attrs);
    }
    if (!g_glrc) { MessageBoxA(0, "Your graphics driver does not support OpenGL 3.3.", "Skies of Arcadia", MB_OK); return 0; }
    wglMakeCurrent(g_hdc, g_glrc); wglDeleteContext(tmp);
    typedef BOOL (WINAPI* PFNSWAP)(int); PFNSWAP swap = (PFNSWAP)wglGetProcAddress("wglSwapIntervalEXT"); if (swap) swap(settings_get("vsync", 0) ? 1 : 0);
    g_opengl32 = LoadLibraryA("opengl32.dll");
    if (settings_get("fullscreen", 0)) set_fullscreen(1);
    return gl_load(win_getproc);
}

/* ---- XInput (loaded dynamically) ---- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef DWORD (WINAPI* PFNXGET)(DWORD, XState*);
static PFNXGET g_xget; static int g_xtried;


void platform_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (!g_xtried) { g_xtried = 1; HMODULE x = LoadLibraryA("xinput1_4.dll"); if (!x) x = LoadLibraryA("xinput9_1_0.dll"); if (x) g_xget = (PFNXGET)GetProcAddress(x, "XInputGetState"); }
    XState xs; long xb = -1; int lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    if (g_xget && g_xget(0, &xs) == 0) {
        xb = xs.pad.buttons; lt = xs.pad.lt; rt = xs.pad.rt; lx = xs.pad.lx; ly = xs.pad.ly; rx = xs.pad.rx; ry = xs.pad.ry;
        if (lt > 200) xb |= 0x10000; if (rt > 200) xb |= 0x20000;
    }
    g_xbuttons = xb; g_lx = lx; g_ly = ly;
    u32 hi, lo; int ff;
    static const u8 none[256];
    input_build(g_focused ? g_keys : none, xb, lt, rt, lx, ly, rx, ry, &hi, &lo, &ff);
    {   /* fast-forward: hold (default) or toggle; speed from settings */
        extern int g_ff_mult; static int prev, on;
        int toggle = settings_get("ff_toggle", 0), mult = settings_get("ff_speed", 3);
        if (toggle) { if (ff && !prev) { on = !on; osd_message(on ? "Fast-forward on (%dx)" : "Fast-forward off", mult); } }
        else on = ff;
        prev = ff;
        extern int options_is_open(void);
        g_ff_mult = on && !options_is_open() ? mult : 1;
    }
    si_set_pad(hi, lo);
}
int platform_focused(void) { return g_focused; }
/* options screen navigation from the raw keyboard and controller (bit layout in options.c) */
u32 platform_nav(void) {
    u32 n = 0x80000000u; const u8* k = g_keys; long xb = g_xbuttons;
    if (!g_focused) return n;
#define KD(vk) (k[(vk) & 255])
#define BK(a) (input_key(a) && KD(input_key(a)))
    if (KD(VK_UP) || BK(IN_SUP) || BK(IN_DUP)) n |= 1;
    if (KD(VK_DOWN) || BK(IN_SDOWN) || BK(IN_DDOWN)) n |= 2;
    if (KD(VK_LEFT) || BK(IN_SLEFT) || BK(IN_DLEFT)) n |= 4;
    if (KD(VK_RIGHT) || BK(IN_SRIGHT) || BK(IN_DRIGHT)) n |= 8;
    if (KD(VK_RETURN) || KD(VK_SPACE) || BK(IN_A) || BK(IN_START)) n |= 16;
    if (KD(VK_ESCAPE) || KD(VK_BACK) || BK(IN_B)) n |= 32;
    if (KD(VK_PRIOR) || BK(IN_L) || KD('Q')) n |= 64;
    if (KD(VK_NEXT) || BK(IN_R) || KD('E')) n |= 128;
    if (KD(VK_DELETE)) n |= 256;
#undef KD
#undef BK
    if (xb >= 0) {
        if (xb & 0x0001) n |= 1; if (xb & 0x0002) n |= 2; if (xb & 0x0004) n |= 4; if (xb & 0x0008) n |= 8;
        if (xb & 0x1000) n |= 16; if (xb & 0x2000) n |= 32; if (xb & 0x0100) n |= 64; if (xb & 0x0200) n |= 128;
        if (g_ly > 16000) n |= 1; if (g_ly < -16000) n |= 2; if (g_lx < -16000) n |= 4; if (g_lx > 16000) n |= 8;
    }
    return n;
}
int platform_take_key(void) { int k = g_last_key; g_last_key = 0; return k; }
long platform_xbuttons(void) { return g_xbuttons; }

/* ---- audio out (waveOut): fed 5 ms blocks by the AI DMA, submitted in 20 ms chunks ---- */
#include <mmsystem.h>
#define NBUF 12
#define CHUNK 640
static HWAVEOUT g_wo; static WAVEHDR g_hdr[NBUF]; static short g_wbuf[NBUF][CHUNK * 2]; static int g_wfill, g_wcur, g_wrate;
void host_audio_write(const s16* stereo, int frames, int rate) {
    if (!g_wo || rate != g_wrate) {
        if (g_wo) { waveOutReset(g_wo); waveOutClose(g_wo); g_wo = 0; }
        WAVEFORMATEX wf = {0}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = rate; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = rate * 4;
        if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_wo = 0; return; }
        memset(g_hdr, 0, sizeof g_hdr); g_wfill = 0; g_wcur = 0; g_wrate = rate;
    }
    for (int i = 0; i < frames; i++) {
        WAVEHDR* h = &g_hdr[g_wcur];
        if (h->dwFlags & WHDR_PREPARED) { if (!(h->dwFlags & WHDR_DONE)) return; waveOutUnprepareHeader(g_wo, h, sizeof *h); }
        g_wbuf[g_wcur][2 * g_wfill] = stereo[2 * i]; g_wbuf[g_wcur][2 * g_wfill + 1] = stereo[2 * i + 1];
        if (++g_wfill == CHUNK) {
            memset(h, 0, sizeof *h); h->lpData = (LPSTR)g_wbuf[g_wcur]; h->dwBufferLength = CHUNK * 4;
            waveOutPrepareHeader(g_wo, h, sizeof *h); waveOutWrite(g_wo, h, sizeof *h);
            g_wfill = 0; g_wcur = (g_wcur + 1) % NBUF;
        }
    }
}

void platform_present(void) {
    extern void gl_blit_to_screen(int w, int h);
    {   /* fast-forward renders 2-4x the frames: show at most one per display refresh */
        extern int g_ff_mult; static LARGE_INTEGER f, last; LARGE_INTEGER now; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
        if (g_ff_mult > 1 && (now.QuadPart - last.QuadPart) * 1000 < f.QuadPart * 15) return;
        last = now;
    }
    gl_blit_to_screen(g_win_w, g_win_h);
    SwapBuffers(g_hdc);
}
#endif
