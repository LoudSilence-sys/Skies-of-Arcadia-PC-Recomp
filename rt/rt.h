/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 LoudSilence-sys and contributors
*/
/* rt.h - runtime model for recompiled Gekko code */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

#define WEAK __attribute__((weak))

typedef struct Ctx {
    u32 r[32];
    double f[32][2];     /* [ps0, ps1] */
    u32 cr, lr, ctr, xer, msr, fpscr;
    u32 gqr[8];
    u32 sr[16];
    u32 hid0, hid2, hid4, srr0, srr1, dec, wpar, l2cr, dabr, iabr;
    u32 sprg[4];
    u64 tb;
    struct Thread* thread;
} Ctx;

typedef void (*GuestFn)(Ctx*);
typedef struct { u32 addr; GuestFn fn; } FuncEntry;
extern const FuncEntry g_func_table[];
extern const int g_func_count;

/* ---------------- memory ---------------- */
#define RAM_SIZE 0x01800000u
extern u8* g_ram;          /* 24 MB main RAM (MEM1) */
extern u8* g_lcache;       /* 16 KB locked cache at 0xE0000000 */

u32  mmio_read(u32 a, int size);
void mmio_write(u32 a, u32 v, int size);
void bad_access(u32 a, int size, int write);

static inline u8* host_ptr(u32 a) {
    if ((a & 0x3E000000u) == 0 && (a & 0x01FFFFFFu) < RAM_SIZE) return g_ram + (a & 0x01FFFFFFu);
    if ((a & 0xFFFFC000u) == 0xE0000000u) return g_lcache + (a & 0x3FFF);
    return 0;
}
/* fast path: cached/uncached RAM mirrors */
#define RAMOK(a) (((a) & 0x3E000000u) == 0 && ((a) & 0x01FFFFFFu) < RAM_SIZE)

static inline u32 R32(u32 a) { if (RAMOK(a)) { u32 v; memcpy(&v, g_ram + (a & 0x01FFFFFFu), 4); return __builtin_bswap32(v); } u8* p = host_ptr(a); if (p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v);} return mmio_read(a, 4); }
static inline u16 R16(u32 a) { if (RAMOK(a)) { u16 v; memcpy(&v, g_ram + (a & 0x01FFFFFFu), 2); return __builtin_bswap16(v); } u8* p = host_ptr(a); if (p) { u16 v; memcpy(&v, p, 2); return __builtin_bswap16(v);} return (u16)mmio_read(a, 2); }
static inline u8  R8(u32 a)  { if (RAMOK(a)) return g_ram[a & 0x01FFFFFFu]; u8* p = host_ptr(a); if (p) return *p; return (u8)mmio_read(a, 1); }
static inline s16 R16S(u32 a) { return (s16)R16(a); }
static inline u32 R32BR(u32 a) { return __builtin_bswap32(R32(a)); }
static inline u16 R16BR(u32 a) { return __builtin_bswap16(R16(a)); }
static inline void W32(u32 a, u32 v) { if (RAMOK(a)) { v = __builtin_bswap32(v); memcpy(g_ram + (a & 0x01FFFFFFu), &v, 4); return; } u8* p = host_ptr(a); if (p) { v = __builtin_bswap32(v); memcpy(p, &v, 4); return; } mmio_write(a, v, 4); }
static inline void W16(u32 a, u32 v) { if (RAMOK(a)) { u16 x = __builtin_bswap16((u16)v); memcpy(g_ram + (a & 0x01FFFFFFu), &x, 2); return; } u8* p = host_ptr(a); if (p) { u16 x = __builtin_bswap16((u16)v); memcpy(p, &x, 2); return; } mmio_write(a, v & 0xFFFF, 2); }
static inline void W8(u32 a, u32 v)  { if (RAMOK(a)) { g_ram[a & 0x01FFFFFFu] = (u8)v; return; } u8* p = host_ptr(a); if (p) { *p = (u8)v; return; } mmio_write(a, v & 0xFF, 1); }
static inline void W32BR(u32 a, u32 v) { W32(a, __builtin_bswap32(v)); }
static inline void W16BR(u32 a, u32 v) { W16(a, __builtin_bswap16((u16)v)); }
static inline float RF32(u32 a) { u32 v = R32(a); float f; memcpy(&f, &v, 4); return f; }
static inline double RF64(u32 a) { u64 v = ((u64)R32(a) << 32) | R32(a + 4); double d; memcpy(&d, &v, 8); return d; }
static inline void WF32(u32 a, float f) { u32 v; memcpy(&v, &f, 4); W32(a, v); }
static inline void WF64(u32 a, double d) { u64 v; memcpy(&v, &d, 8); W32(a, (u32)(v >> 32)); W32(a + 4, (u32)v); }
static inline void DCBZ(u32 a) { a &= ~31u; for (int i = 0; i < 32; i += 4) W32(a + i, 0); }

/* ---------------- condition register ---------------- */
#define CRBIT(c, n) (((c)->cr >> (31 - (n))) & 1)
#define SETCRBIT(c, n, v) do { u32 b_ = 1u << (31 - (n)); if (v) (c)->cr |= b_; else (c)->cr &= ~b_; } while (0)
#define GETCRF(c, f) (((c)->cr >> (28 - 4 * (f))) & 0xF)
#define SETCRF(c, f, v) do { (c)->cr = ((c)->cr & ~(0xFu << (28 - 4 * (f)))) | ((u32)(v) << (28 - 4 * (f))); } while (0)
#define SO_BIT(c) (((c)->xer >> 31) & 1)
#define CMPS(c, f, a, b) do { s32 a_ = (a), b_ = (b); SETCRF(c, f, (a_ < b_ ? 8 : a_ > b_ ? 4 : 2) | SO_BIT(c)); } while (0)
#define CMPU(c, f, a, b) do { u32 a_ = (a), b_ = (b); SETCRF(c, f, (a_ < b_ ? 8 : a_ > b_ ? 4 : 2) | SO_BIT(c)); } while (0)
#define SET_CR0(c, v) CMPS(c, 0, v, 0)
#define SET_CR1(c) SETCRF(c, 1, ((c)->fpscr >> 28) & 0xF)
#define GETCA(c) (((c)->xer >> 29) & 1)
#define SETCA(c, v) do { if (v) (c)->xer |= 0x20000000u; else (c)->xer &= ~0x20000000u; } while (0)
#define ROTL(x, n) ((u32)(((u32)(x) << ((n) & 31)) | ((u32)(x) >> ((32 - ((n) & 31)) & 31))))

static inline void FCMP(Ctx* c, int f, double a, double b) {
    int v = (isnan(a) || isnan(b)) ? 1 : (a < b ? 8 : a > b ? 4 : 2);
    SETCRF(c, f, v);
}
static inline u32 DIVW(u32 a, u32 b) { s32 x = (s32)a, y = (s32)b; if (y == 0 || (x == (s32)0x80000000 && y == -1)) return x < 0 && y == 0 ? 0xFFFFFFFFu : 0; return (u32)(x / y); }
static inline u32 DIVWU(u32 a, u32 b) { return b ? a / b : 0; }
static inline double FMA_S(double a, double c, double b) { return (double)(float)fma(a, c, b); }
static inline double FRSQRTE(double x) { return 1.0 / sqrt(x); }
static inline void FCTIW(Ctx* c, int d, double v, int trn) {
    s32 r;
    if (isnan(v)) r = (s32)0x80000000;
    else {
        double t = trn ? trunc(v) : nearbyint(v);
        if (t >= 2147483647.0) r = 0x7FFFFFFF; else if (t <= -2147483648.0) r = (s32)0x80000000; else r = (s32)t;
    }
    u64 bits = 0xFFF8000000000000ull | (u32)r; memcpy(&c->f[d][0], &bits, 8);
}

/* ---------------- paired-single quantization ---------------- */
void PSQ_L(Ctx* c, int fd, u32 ea, int w, int i);
void PSQ_ST(Ctx* c, int fs, u32 ea, int w, int i);

/* ---------------- system ---------------- */
u32  MFSPR(Ctx* c, int spr);
void MTSPR(Ctx* c, int spr, u32 v);
void MTMSR(Ctx* c, u32 v);
void RFI(Ctx* c);
void SYSCALL(Ctx* c, u32 pc);
void TRAP(Ctx* c, u32 pc);
void UNIMPL(Ctx* c, u32 pc, u32 insn);
void call_indirect(Ctx* c, u32 addr);
extern Ctx* g_ctx;
extern int g_poll_ctr;
void rt_poll(Ctx* c);
void rt_idle(Ctx* c);
#define IRQ_CHECK(c) do { if (--g_poll_ctr <= 0) rt_poll(c); } while (0)

