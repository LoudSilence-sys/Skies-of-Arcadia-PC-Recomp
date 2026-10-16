/* sched.c - timed hardware events and interrupt delivery at safe points */
#include "funcs.h"
#include "names.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

void rt_log(const char* fmt, ...);
void rt_fatal(const char* fmt, ...);
u64 host_tb(void);
u32 pi_intsr(void);
void vi_retrace_event(void);
void hw_tick(void);

#define TB_RATE 40500000ull
int g_poll_ctr = 2000;
int g_in_irq;
u64 g_irq_counts[16]; u64 g_dec_count;

/* event times in timebase ticks */
static u64 g_next_vi;
static u64 g_dec_deadline; static int g_dec_armed;
u64 g_vi_period = TB_RATE / 50;          /* PAL 50 Hz */
u64 g_frames;

void sched_set_dec(u32 v) {
    g_dec_deadline = host_tb() + (u64)(s32)v; g_dec_armed = 1;
}
u32 sched_get_dec(void) {
    if (!g_dec_armed) return 0xFFFFFFFF;
    s64 d = (s64)(g_dec_deadline - host_tb()); return (u32)d;
}

/* run fn(r3, r4) on the current context as if an exception occurred, preserving every register */
static void exception_call(Ctx* c, void (*fn)(Ctx*), u32 exc) {
    Ctx save = *c;
    g_in_irq = 1;
    c->msr &= ~0x8000u;                 /* EE off during handler */
    c->r[3] = exc;
    c->r[4] = R32(0x800000D4);          /* OSGetCurrentContext() */
    c->r[1] = (save.r[1] - 0x200) & ~0xFu;
    c->lr = 0;
    fn(c);
    u32 cur_ctx = R32(0x800000D4);
    *c = save;
    W32(0x800000D4, cur_ctx);
    g_in_irq = 0;
}

static void deliver(Ctx* c) {
    { static int n; if (n < 10 && !(c->msr & 0x8000u)) { n++; rt_log("deliver: EE off (msr %08X lr %08X)\n", c->msr, c->lr); } }
    if (g_in_irq || !(c->msr & 0x8000u)) return;
    /* decrementer */
    if (g_dec_armed && (s64)(host_tb() - g_dec_deadline) >= 0) {
        g_dec_armed = 0; g_dec_count++;
        exception_call(c, FN_DecrementerExceptionCallback, 8);
    }
    /* external interrupts via the PI */
    u32 cause = pi_intsr();
    u32 mask = R32(0xCC003004);
    static int n; if ((cause & ~0x10000u) && n < 40) { n++; rt_log("irq: cause %08X mask %08X msr %08X\n", cause, mask, c->msr); }
    for (int iter = 0; iter < 16 && (cause & mask); iter++, cause = pi_intsr(), mask = R32(0xCC003004)) {
        for (int b = 0; b < 16; b++) if (cause & mask & (1u << b)) g_irq_counts[b]++;
        if (cause & mask & 4) { static int n; if (n++ < 400) rt_log("deliver DI irq cause %08X sicsr %08X sipoll %08X\n", cause, R32(0xCC006434), R32(0xCC006430)); }
        { static int n; if ((R16(0xCC00500A) & 0x18) == 0x18 && n++ < 8) rt_log("AID pending: cause %08X mask %08X csr %04X osmask %08X %08X prio? \n", cause, mask, R16(0xCC00500A), R32(0x800000C4), R32(0x800000C8)); }
        exception_call(c, FN___OSDispatchInterrupt, 4);
    }
}

#include <stdlib.h>
/* headless input script: SOA_INPUT="frame:buttons:len,..." buttons from {start,a,b,x,y,up,down,left,right} */
WEAK void platform_pump(void) {
    static const char* sc; static int init; if (!init) { init = 1; sc = getenv("SOA_INPUT"); }
    if (!sc) return;
    extern void si_set_pad(u32 hi, u32 lo);
    u32 b0 = 0, b1 = 0x80, sx = 0x80, sy = 0x80;
    const char* p = sc;
    while (*p) {
        long f = strtol(p, (char**)&p, 10); if (*p != ':') break; p++;
        char btn[16]; int k = 0; while (*p && *p != ':' && k < 15) btn[k++] = *p++; btn[k] = 0; if (*p == ':') p++;
        long len = strtol(p, (char**)&p, 10); if (*p == ',') p++;
        extern int g_frame_no_pub; static int xfb = -1; if (xfb < 0) xfb = getenv("SOA_INPUT_XFB") != 0;
        long now_f = xfb ? (long)g_frame_no_pub : (long)g_frames;
        if (now_f >= f && now_f < f + len) {
            if (!strcmp(btn, "start")) b0 |= 0x10; else if (!strcmp(btn, "a")) b0 |= 1; else if (!strcmp(btn, "b")) b0 |= 2;
            else if (!strcmp(btn, "x")) b0 |= 4; else if (!strcmp(btn, "y")) b0 |= 8;
            else if (!strcmp(btn, "up")) sy = 0xE0; else if (!strcmp(btn, "down")) sy = 0x20; else if (!strcmp(btn, "left")) sx = 0x20; else if (!strcmp(btn, "right")) sx = 0xE0;
            else if (!strcmp(btn, "dleft")) b1 |= 0x01; else if (!strcmp(btn, "dright")) b1 |= 0x02; else if (!strcmp(btn, "ddown")) b1 |= 0x04; else if (!strcmp(btn, "dup")) b1 |= 0x08;
        }
    }
    si_set_pad(b0 << 24 | b1 << 16 | sx << 8 | sy, 0x80800000u);
}
void rt_poll(Ctx* c) {
    g_poll_ctr = 2000;
    u64 now = host_tb();
    { static u64 limit; static int init; if (!init) { init = 1; const char* e = getenv("SOA_SECONDS"); limit = e ? (u64)atoi(e) * TB_RATE : 0; }
      if (limit && now > limit) { extern u64 g_irq_counts[16], g_fifo_bytes, g_draws, g_tris; extern int g_frame_no_pub;
        fprintf(stderr, "exit after %llus: vi %llu xfb %d fifo %llu draws %llu tris %llu\n", (unsigned long long)(limit / TB_RATE), (unsigned long long)g_frames, g_frame_no_pub, (unsigned long long)g_fifo_bytes, (unsigned long long)g_draws, (unsigned long long)g_tris); exit(0); } }
    if (!g_next_vi) g_next_vi = now + g_vi_period;
    if ((s64)(now - g_next_vi) >= 0) {
        g_next_vi += g_vi_period;
        if ((s64)(now - g_next_vi) > (s64)(g_vi_period * 4)) g_next_vi = now + g_vi_period; /* don't spiral */
        g_frames++;
        { extern void platform_pump(void); platform_pump(); }
        vi_retrace_event();
        { extern int vi_refresh_hz(void); static int hz = 50; int nh = vi_refresh_hz(); if (nh != hz) { hz = nh; g_vi_period = TB_RATE / hz; rt_log("VI: %d Hz\n", hz); } }
    }
    hw_tick();
    deliver(c);
}

/* called from the OS idle loop: sleep until the next event, then deliver */
void rt_idle(Ctx* c) {
    rt_poll(c);
    u64 now = host_tb();
    u64 next = g_next_vi;
    if (g_dec_armed && (s64)(g_dec_deadline - next) < 0) next = g_dec_deadline;
    if ((s64)(next - now) > 0) {
        u64 us = (next - now) * 1000000ull / TB_RATE;
        if (us > 20000) us = 20000;
        { extern u64 g_pf_idle; u64 t0 = host_tb(); usleep((useconds_t)us); g_pf_idle += host_tb() - t0; }
    }
    rt_poll(c);
}
