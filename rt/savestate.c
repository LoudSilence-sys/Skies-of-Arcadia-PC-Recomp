/* savestate.c - quick save / quick load (F5 / F9, slot with F6 / F7).

   The game is statically recompiled, so the host C stack mirrors the guest call stack.  A state can only
   be taken and restored at one fixed point: the end-of-frame wait in the main loop (VIWaitForRetrace called
   at 0x801DE168).  At that point no interrupt handler or game callback is running and every module is
   between commands.  A state stores the guest return-address chain at that point; loading checks that the
   current chain is the same, so the host stack frames below us belong to the same guest functions. */
#include "pc.h"
void rt_fatal(const char* fmt, ...);
extern Ctx* g_ctx;
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>

u64 host_tb(void);
extern s64 g_tb_off;
unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);
char* stbi_zlib_decode_malloc_guesssize(const char* buffer, int len, int initial_size, int* outlen);

#define SS_MAGIC "SOAQSAV1"
#define SS_VERSION 1
#define NSLOT 10
#define CHAIN 24

/* ---------------- stream ---------------- */
void ss_io(SS* s, void* p, u64 n) {
    if (s->err || !n) return;
    if (s->load) {
        if (s->pos + n > s->len) { s->err = 1; memset(p, 0, n); return; }
        memcpy(p, s->buf + s->pos, n); s->pos += n; return;
    }
    if (s->len + n > s->cap) {
        u64 nc = s->cap ? s->cap : 1 << 20; while (nc < s->len + n) nc *= 2;
        u8* b = realloc(s->buf, nc); if (!b) { s->err = 1; return; }
        s->buf = b; s->cap = nc;
    }
    memcpy(s->buf + s->len, p, n); s->len += n;
}
void ss_time(SS* s, u64* t) {   /* absolute host time <-> offset from the snapshot moment */
    s64 rel = (s64)(*t - s->now); SSV(s, rel);
    if (s->load) *t = s->now + (u64)rel;
}

/* ---------------- runtime part ---------------- */
void rt_state(SS* s) { (void)s; }
static void ctx_io(SS* s, Ctx* c) {
    struct Thread* th = c->thread;
    ss_io(s, c, offsetof(Ctx, thread));
    c->thread = th;
}

/* ---------------- call chains ----------------
   The guest call stack at the save point: ra[0] is the return address of the hook call (c->lr), then the
   saved LR of every frame up to the boot entry (__start).  Every call site in the generated code is a resume
   point (rs_take), so a state saved under one chain can be loaded under another: we unwind the host stack
   to the bottom, restore the state and call back down through the saved chain. */
extern const unsigned g_fn_bounds[]; extern const int g_fn_nbounds;
static u32 g_boot_entry;
static int fn_of(u32 a) {   /* index of the function containing a, or -1 */
    int lo = 0, hi = g_fn_nbounds - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_fn_bounds[2 * m]) hi = m - 1; else if (a >= g_fn_bounds[2 * m + 1]) lo = m + 1; else return m; }
    return -1;
}
static int guest_chain(Ctx* c, u32* out) {
    int n = 0, boot = fn_of(g_boot_entry);
    out[n++] = c->lr;
    for (u32 f = c->r[1]; n < CHAIN; ) {
        if ((f & 3) || f < 0x80000000u || (f & 0x3FFFFFFF) >= RAM_SIZE) break;
        u32 back = R32(f); if (!back || back <= f || (back & 0x3FFFFFFF) >= RAM_SIZE) break;
        u32 ret = R32(back + 4); if (fn_of(ret) < 0) return 0;
        out[n++] = ret; f = back;
        if (fn_of(ret) == boot) return n;
    }
    return 0;   /* did not reach the boot entry: not a chain we understand */
}
/* can the generated code be re-entered along this chain?  Each level must be an unconditional bl into the
   function holding the next level, or an unconditional bctrl (the callee is then taken from the chain). */
static int chain_resumable(const u32* ra, int n) {
    if (n < 2 || ra[0] != 0x801DE168) return 0;
    for (int k = 0; k < n; k++) {
        u32 pc = ra[k] - 4, w = R32(pc);
        if ((w >> 26) == 18 && (w & 3) == 1) {   /* bl */
            u32 t = pc + (u32)((s32)((w & 0x03FFFFFC) << 6) >> 6);
            if (k == 0) { if (t != 0x80240B24) return 0; }
            else if (fn_of(t) != fn_of(ra[k - 1])) return 0;
        } else if ((w & 0xFC0007FF) == 0x4C000421 && ((w >> 21) & 0x14) == 0x14) {   /* bctrl */
            if (k == 0) return 0;
        } else return 0;
    }
    return 1;
}
int g_rs_n;                      /* levels still to re-enter (generated code checks it at every function entry) */
static u32 g_rs_ra[CHAIN], g_rs_len, g_rs_bad;
u32 rs_take(Ctx* c) {
    int k = --g_rs_n;
    if (k < 0 || (k + 1 < (int)g_rs_len && c->lr != g_rs_ra[k + 1])) { g_rs_bad = 1; g_rs_n = 0; return 0; }
    if (k > 0) c->ctr = g_fn_bounds[2 * fn_of(g_rs_ra[k - 1])];   /* for an indirect call: the function of the next level */
    return g_rs_ra[k];
}

typedef struct {
    char magic[8]; u32 version, hdr_size; u64 raw_size, game_tb; u32 sp, nchain, chain[CHAIN]; s64 saved_at; u32 frame, resumable;
} Hdr;

static volatile int g_req;       /* 1 = save, 2 = load */
static u64 g_req_at;
static int g_slot = -1;
static volatile int g_writing;

static void slot_path(int slot, char* out, int cap) { snprintf(out, cap, "States/GEAP8P.s%02d", slot); }
int quickstate_get_slot(void) { if (g_slot < 0) { g_slot = settings_get("quickstate_slot", 1); if (g_slot < 1 || g_slot > NSLOT) g_slot = 1; } return g_slot; }
void quickstate_slot(int d) {
    int s = quickstate_get_slot() + d; if (s < 1) s = NSLOT; if (s > NSLOT) s = 1; g_slot = s;
    settings_set("quickstate_slot", s); settings_save();
    char p[64]; slot_path(s, p, sizeof p); struct stat st;
    if (!stat(p, &st)) { char ts[40]; strftime(ts, sizeof ts, "%d %b %H:%M", localtime(&st.st_mtime)); osd_message("State slot %d  (saved %s)", s, ts); }
    else osd_message("State slot %d  (empty)", s);
}
void quickstate_request(int load) {
    if (g_req) return;
    if (!load && g_writing) { osd_message("Still writing the last state..."); return; }
    g_req = load ? 2 : 1; g_req_at = host_tb();
}

typedef struct { u8* raw; u64 len; Hdr h; int slot; } Job;
static void* writer(void* a) {
    Job* j = a; int clen = 0;
    unsigned char* z = stbi_zlib_compress(j->raw, (int)j->len, &clen, 5);
    char p[64], tmp[72]; slot_path(j->slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int ok = 0;
    if (z) {
        FILE* f = fopen(tmp, "wb");
        if (f) { ok = fwrite(&j->h, sizeof j->h, 1, f) == 1 && fwrite(z, 1, clen, f) == (size_t)clen; ok = fclose(f) == 0 && ok; }
        if (ok) { remove(p); ok = rename(tmp, p) == 0; }
        free(z);
    }
    if (ok) osd_message("Saved state to slot %d", j->slot); else osd_message("Saving the state failed (could not write %s)", p);
    rt_log("quickstate: slot %d saved: %llu bytes raw, %d compressed\n", j->slot, (unsigned long long)j->len, clen);
    free(j->raw); free(j); g_writing = 0;
    return 0;
}

static void all_state(SS* s, Ctx* c) {
    ctx_io(s, c);
    ss_io(s, g_ram, RAM_SIZE); ss_io(s, g_lcache, 0x4000);
    rt_state(s); hw_state(s); sched_state(s); fifo_state(s); gx_state(s); ax_state(s); dcsnd_state(s); hle_state(s);
    u32 end = 0x454E4421; SSV(s, end); if (s->load && end != 0x454E4421) s->err = 1;
}

static void do_save(Ctx* c) {
    Job* j = calloc(1, sizeof *j); SS s = {0}; s.now = host_tb();
    all_state(&s, c);
    if (s.err) { free(s.buf); free(j); osd_message("Saving the state failed (out of memory)"); return; }
    memcpy(j->h.magic, SS_MAGIC, 8); j->h.version = SS_VERSION; j->h.hdr_size = sizeof j->h;
    j->h.raw_size = s.len; j->h.game_tb = s.now + g_tb_off; j->h.sp = c->r[1]; j->h.nchain = guest_chain(c, j->h.chain); j->h.resumable = chain_resumable(j->h.chain, j->h.nchain);
    j->h.saved_at = (s64)time(0); { extern int g_frame_no_pub; j->h.frame = g_frame_no_pub; }
    j->raw = s.buf; j->len = s.len; j->slot = quickstate_get_slot();
#ifdef _WIN32
    mkdir("States");
#else
    mkdir("States", 0755);
#endif
    g_writing = 1;
    pthread_t th; if (pthread_create(&th, 0, writer, j)) writer(j); else pthread_detach(th);
}

/* apply a decompressed state to the machine */
static int apply_state(Ctx* c, char* raw, int rl, const Hdr* h, int slot) {
    SS s = {0}; s.load = 1; s.buf = (u8*)raw; s.len = rl; s.now = host_tb();
    all_state(&s, c);
    if (s.err || s.pos != s.len) { rt_log("quickstate: stream error at %llu of %llu\n", (unsigned long long)s.pos, (unsigned long long)s.len); osd_message("Slot %d is damaged - the game may be unstable now, reload a save", slot); return 0; }
    g_tb_off = (s64)(h->game_tb - s.now);
    { extern void dcsnd_volumes_changed(void); dcsnd_volumes_changed(); }
    return 1;
}
static void loaded_message(const Hdr* h, int slot) {
    char ts[40]; time_t t = (time_t)h->saved_at; strftime(ts, sizeof ts, "%d %b %H:%M", localtime(&t));
    osd_message("Loaded state from slot %d  (saved %s)", slot, ts);
    rt_log("quickstate: slot %d loaded\n", slot);
}

/* loading under a different call chain: unwind to rs_run, restore, re-enter the saved chain */
static void* g_top[5]; static int g_top_set;
static char* g_x_raw; static int g_x_len, g_x_slot; static Hdr g_x_h;
static Ctx g_rs_ctx; static int g_resuming;
static GuestFn g_entry_fn;
void rs_run(Ctx* c, GuestFn entry, u32 entry_addr) {
    g_entry_fn = entry; g_boot_entry = entry_addr; g_top_set = 1;
    if (__builtin_setjmp(g_top)) {
        c = g_ctx;
        int ok = apply_state(c, g_x_raw, g_x_len, &g_x_h, g_x_slot);
        free(g_x_raw); g_x_raw = 0;
        if (!ok) rt_fatal("quick load failed: slot %d is damaged\n", g_x_slot);
        for (u32 i = 0; i < g_x_h.nchain; i++) g_rs_ra[i] = g_x_h.chain[i];
        g_rs_len = g_x_h.nchain; g_rs_bad = 0; g_rs_ctx = *c; g_resuming = 1;
        g_rs_n = (int)g_rs_len;
        rt_log("quickstate: re-entering %u levels\n", g_rs_len);
    }
    g_entry_fn(g_ctx);
    if (g_resuming || g_rs_bad) rt_fatal("quick load failed: could not re-enter the saved game state\n");
    rt_log("entry returned\n");
}

static void do_load(Ctx* c) {
    int slot = quickstate_get_slot(); char p[64]; slot_path(slot, p, sizeof p);
    FILE* f = fopen(p, "rb"); if (!f) { osd_message("Slot %d is empty", slot); return; }
    Hdr h; long fl = 0; u8* z = 0; int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, SS_MAGIC, 8);
    if (ok && (h.version != SS_VERSION || h.hdr_size != sizeof h)) { fclose(f); osd_message("Slot %d was saved by a different version and can't be loaded", slot); return; }
    if (ok) { fseek(f, 0, SEEK_END); fl = ftell(f) - (long)sizeof h; fseek(f, sizeof h, SEEK_SET); z = malloc(fl > 0 ? fl : 1); ok = fl > 0 && fread(z, 1, fl, f) == (size_t)fl; }
    fclose(f);
    if (!ok) { free(z); osd_message("Slot %d is damaged", slot); return; }
    u32 chain[CHAIN]; u32 n = guest_chain(c, chain);
    int same = h.sp == c->r[1] && h.nchain == n && n && !memcmp(h.chain, chain, n * 4);
    if (getenv("SOA_QRESUME")) same = 0;   /* test: always take the re-entry path */
    if (!same && (!h.resumable || !g_top_set)) {
        rt_log("quickstate: slot %d not resumable here (%u levels saved, %u now)\n", slot, h.nchain, n);
        free(z); osd_message("Slot %d can't be loaded here", slot); return;
    }
    int rl = 0; char* raw = stbi_zlib_decode_malloc_guesssize((const char*)z, (int)fl, (int)h.raw_size, &rl); free(z);
    if (!raw || (u64)rl != h.raw_size) { free(raw); osd_message("Slot %d is damaged", slot); return; }
    if (same) {
        int ok2 = apply_state(c, raw, rl, &h, slot); free(raw);
        if (ok2) loaded_message(&h, slot);
        return;
    }
    g_x_raw = raw; g_x_len = rl; g_x_h = h; g_x_slot = slot;
    __builtin_longjmp(g_top, 1);
}

/* called at the end-of-frame wait in the main loop */
void quickstate_poll(Ctx* c) {
    {   /* tests: SOA_QSAVE / SOA_QLOAD = frame numbers (comma separated) */
        static int init, fs[8], ns, fl[8], nl; extern int g_frame_no_pub; static int last = -1;
        if (!init) { init = 1; const char* e = getenv("SOA_QSAVE"); if (e) ns = sscanf(e, "%d,%d,%d,%d", &fs[0], &fs[1], &fs[2], &fs[3]); e = getenv("SOA_QLOAD"); if (e) nl = sscanf(e, "%d,%d,%d,%d", &fl[0], &fl[1], &fl[2], &fl[3]); }
        if (g_frame_no_pub != last) { last = g_frame_no_pub; for (int i = 0; i < ns; i++) if (fs[i] && g_frame_no_pub >= fs[i]) { fs[i] = 0; g_req = 1; } for (int i = 0; i < nl; i++) if (fl[i] && g_frame_no_pub >= fl[i]) { fl[i] = 0; g_req = 2; } }
        static int lg = -1; if (lg < 0) lg = getenv("SOA_QCHAIN") != 0;
        if (lg) { static u32 prev[CHAIN]; u32 ch[CHAIN] = {0}; int n = guest_chain(c, ch); if (memcmp(prev, ch, sizeof ch)) { memcpy(prev, ch, sizeof ch); rt_log("qchain frame %d sp %08X:", g_frame_no_pub, c->r[1]); for (int i = 0; i < n; i++) rt_log(" %08X", ch[i]); rt_log("\n"); } }
    }
    if (g_resuming) {   /* back at the save point after re-entering the saved chain */
        struct Thread* th = c->thread; *c = g_rs_ctx; c->thread = th;
        g_resuming = 0; g_rs_n = 0; loaded_message(&g_x_h, g_x_slot);
        return;
    }
    if (!g_req) return;
    int r = g_req; g_req = 0;
    if (r == 1) do_save(c); else do_load(c);
}
/* a request that never reaches the main loop (loading screens, some cutscenes) */
void quickstate_tick(void) {
    if (g_req && host_tb() - g_req_at > 2 * 40500000ull) { osd_message(g_req == 1 ? "Can't save a state right now - try again in a moment" : "Can't load a state right now - try again in a moment"); g_req = 0; }
}
