/* win32.c - Windows platform layer: window + WGL OpenGL 3.3 context, presentation, keyboard/XInput input */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "rt.h"
#include "glmini.h"

void rt_log(const char* fmt, ...);
void si_set_pad(u32 hi, u32 lo);

static HWND g_hwnd; static HDC g_hdc; static HGLRC g_glrc;
static int g_win_w = 1280, g_win_h = 960;
static HMODULE g_opengl32;
static u8 g_keys[256];

static void* win_getproc(const char* n) {
    void* p = (void*)wglGetProcAddress(n);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) p = (void*)GetProcAddress(g_opengl32, n);
    return p;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: ExitProcess(0); return 0;
    case WM_SIZE: g_win_w = LOWORD(l); g_win_h = HIWORD(l); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: g_keys[w & 255] = 1; if (w == VK_F11) { /* reserved */ } return 0;
    case WM_KEYUP: case WM_SYSKEYUP: g_keys[w & 255] = 0; return 0;
    case WM_KILLFOCUS: memset(g_keys, 0, sizeof g_keys); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

typedef HGLRC (WINAPI* PFNCREATECTX)(HDC, HGLRC, const int*);
int platform_init(void) {
    HINSTANCE inst = GetModuleHandleA(0);
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
    typedef BOOL (WINAPI* PFNSWAP)(int); PFNSWAP swap = (PFNSWAP)wglGetProcAddress("wglSwapIntervalEXT"); if (swap) swap(0);
    g_opengl32 = LoadLibraryA("opengl32.dll");
    return gl_load(win_getproc);
}

/* ---- XInput (loaded dynamically) ---- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef DWORD (WINAPI* PFNXGET)(DWORD, XState*);
static PFNXGET g_xget; static int g_xtried;

static u8 axis(int neg, int pos) { return (u8)(0x80 + (pos ? 0x60 : 0) - (neg ? 0x60 : 0)); }
static u8 stick(SHORT v) { int x = 0x80 + v / 256; if (x < 0) x = 0; if (x > 255) x = 255; return (u8)x; }

void platform_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    /* keyboard: arrows=stick, Z=A, X=B, C=X, V=Y, Enter=Start, Q=L, W=R, E=Z, IJKL=C-stick, TFGH=D-pad */
    u32 b0 = 0, b1 = 0x80; u8 sx, sy, cx, cy, tl = 0, tr = 0;
    if (g_keys['Z']) b0 |= 0x01; if (g_keys['X']) b0 |= 0x02; if (g_keys['C']) b0 |= 0x04; if (g_keys['V']) b0 |= 0x08; if (g_keys[VK_RETURN]) b0 |= 0x10;
    if (g_keys['F']) b1 |= 0x01; if (g_keys['H']) b1 |= 0x02; if (g_keys['G']) b1 |= 0x04; if (g_keys['T']) b1 |= 0x08;
    if (g_keys['E']) b1 |= 0x10; if (g_keys['W']) { b1 |= 0x20; tr = 0xFF; } if (g_keys['Q']) { b1 |= 0x40; tl = 0xFF; }
    sx = axis(g_keys[VK_LEFT], g_keys[VK_RIGHT]); sy = axis(g_keys[VK_DOWN], g_keys[VK_UP]);
    cx = axis(g_keys['J'], g_keys['L']); cy = axis(g_keys['K'], g_keys['I']);
    if (!g_xtried) { g_xtried = 1; HMODULE x = LoadLibraryA("xinput1_4.dll"); if (!x) x = LoadLibraryA("xinput9_1_0.dll"); if (x) g_xget = (PFNXGET)GetProcAddress(x, "XInputGetState"); }
    XState xs;
    if (g_xget && g_xget(0, &xs) == 0) {
        WORD w = xs.pad.buttons;
        if (w & 0x1000) b0 |= 0x01; if (w & 0x2000) b0 |= 0x02; if (w & 0x4000) b0 |= 0x04; if (w & 0x8000) b0 |= 0x08; if (w & 0x0010) b0 |= 0x10;
        if (w & 0x0004) b1 |= 0x01; if (w & 0x0008) b1 |= 0x02; if (w & 0x0002) b1 |= 0x04; if (w & 0x0001) b1 |= 0x08;
        if (w & 0x0200) b1 |= 0x10;
        if (xs.pad.lt > 30) { tl = xs.pad.lt; if (xs.pad.lt > 200) b1 |= 0x40; }
        if (xs.pad.rt > 30) { tr = xs.pad.rt; if (xs.pad.rt > 200) b1 |= 0x20; }
        if (abs(xs.pad.lx) > 7000 || abs(xs.pad.ly) > 7000) { sx = stick(xs.pad.lx); sy = stick(xs.pad.ly); }
        if (abs(xs.pad.rx) > 7000 || abs(xs.pad.ry) > 7000) { cx = stick(xs.pad.rx); cy = stick(xs.pad.ry); }
    }
    si_set_pad(b0 << 24 | b1 << 16 | (u32)sx << 8 | sy, (u32)cx << 24 | (u32)cy << 16 | (u32)tl << 8 | tr);
}

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
    gl_blit_to_screen(g_win_w, g_win_h);
    SwapBuffers(g_hdc);
}
#endif
