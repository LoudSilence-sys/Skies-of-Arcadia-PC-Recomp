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
    r_80240B24(c);
}
