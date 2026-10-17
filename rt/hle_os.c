/* hle_os.c - native replacements for SDK functions (strong symbols override the weak recompiled ones) */
#include "funcs.h"
#include "names.h"
#include <stdio.h>
void rt_log(const char* fmt, ...);

/* The IPL audio-system boot (DSP init ucode + ARAM check) has no meaning on PC. */
void FN___OSInitAudioSystem(Ctx* c) { rt_log("HLE __OSInitAudioSystem\n"); }
void FN___OSStopAudioSystem(Ctx* c) { rt_log("HLE __OSStopAudioSystem\n"); }

/* MMU / BAT setup runs in real mode via rfi; the host has no MMU to program. */
void FN___OSInitMemoryProtection(Ctx* c) { rt_log("HLE __OSInitMemoryProtection\n"); }
void FN_RealMode(Ctx* c) { rt_log("HLE RealMode %08X\n", c->r[3]); }

/* ---- threads: this game only ever runs the default thread, so a context
   "switch" is always back to the running host thread ---- */
static u32 g_main_ctx;
void FN_OSSaveContext(Ctx* c) { if (!g_main_ctx) g_main_ctx = c->r[3]; c->r[3] = 0; }
void FN_OSLoadContext(Ctx* c) {
    u32 ctx = c->r[3];
    u32 idle = 0; (void)idle;
    if (g_main_ctx && ctx != g_main_ctx && R32(0x800000E4) && ctx == R32(0x800000E4) && ctx != g_main_ctx) {
        extern void rt_fatal(const char*, ...); rt_fatal("OSLoadContext to a second thread %08X (not supported yet)\n", ctx);
    }
}
void rt_backtrace(Ctx* c);
void FN___DVDStoreErrorCode(Ctx* c) { rt_log("__DVDStoreErrorCode(%08X)\n", c->r[3]); rt_backtrace(c); RN___DVDStoreErrorCode(c); }
void FN_cbForStateError(Ctx* c) { rt_log("cbForStateError(%08X)\n", c->r[3]); rt_backtrace(c); RN_cbForStateError(c); }
void FN___DVDInterruptHandler(Ctx* c) { static int n; if (n++ < 400) rt_log("DVD irq handler run\n"); RN___DVDInterruptHandler(c); }
void FN_SIInterruptHandler(Ctx* c) { static int n; if (n++ < 50) rt_log("SI handler csr %08X\n", R32(0xCC006434)); RN_SIInterruptHandler(c); }
void FN_DVDGetDriveStatus(Ctx* c) {
    static s32 last = 12345; RN_DVDGetDriveStatus(c);
    if ((s32)c->r[3] != last) { last = (s32)c->r[3]; rt_log("DVDGetDriveStatus -> %d\n", last); }
}

#include "pc.h"
void hle_state(SS* s) { SSV(s, g_main_ctx); }
