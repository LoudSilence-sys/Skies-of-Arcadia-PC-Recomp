/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 LoudSilence-sys and contributors
*/
/* pc.h - PC-port features shared between runtime files (not included by the generated game code) */
#pragma once
#ifndef WEAK
#include "rt.h"
#endif
/* input bindings (input.c) */
enum { IN_A, IN_B, IN_X, IN_Y, IN_START, IN_Z, IN_L, IN_R, IN_DUP, IN_DDOWN, IN_DLEFT, IN_DRIGHT,
       IN_SUP, IN_SDOWN, IN_SLEFT, IN_SRIGHT, IN_CUP, IN_CDOWN, IN_CLEFT, IN_CRIGHT, IN_FF, IN_REWIND, IN_N };
void input_build(const u8* keys, long xb, int lt, int rt, int lx, int ly, int rx, int ry, u32* hi, u32* lo, int* ff);
const char* input_name(int a); int input_key(int a); u32 input_pad(int a);
void input_set_key(int a, int vk); void input_set_pad(int a, u32 b); void input_set_layout(int gc); void input_reset_keys(void);
const char* input_key_label(int vk); const char* input_pad_label(u32 b); int input_has_pad_binding(int a);
/* on-screen messages (gl_backend.c) */
void osd_message(const char* fmt, ...);
/* settings (runtime.c) */
int settings_get(const char* key, int def);
void settings_set(const char* key, int v);
void settings_save(void);
void rt_log(const char* fmt, ...);
/* graphics (gl_backend.c) */
void gl_set_msaa(int samples); void gl_texcache_clear(void);
/* quick save states (savestate.c) */
void quickstate_request(int load); void quickstate_slot(int delta); int quickstate_get_slot(void);
/* save-state stream: each module serialises its state through ss_io; times go through ss_time
   (stored relative to the moment of the snapshot so pending events keep their distance) */
typedef struct SS { u8* buf; u64 len, cap, pos; int load, err; u64 now; } SS;
void ss_io(SS* s, void* p, u64 n);
void ss_time(SS* s, u64* t);
#define SSV(s, v) ss_io((s), &(v), sizeof(v))
void rt_state(SS* s); void hw_state(SS* s); void sched_state(SS* s); void gx_state(SS* s); void fifo_state(SS* s);
void ax_state(SS* s); void dcsnd_state(SS* s); void hle_state(SS* s);
