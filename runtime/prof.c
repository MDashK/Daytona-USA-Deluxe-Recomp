/* prof.c — sampling profiler (see prof.h). */
#include <stdio.h>
#include <string.h>
#include "prof.h"

void rt_log(const char *fmt, ...);
extern volatile unsigned int g_recomp_last;
extern unsigned int g_cur_func;

volatile int g_prof_phase;
volatile unsigned int g_lift_depth;

#define HB 2048
static unsigned int s_key[HB], s_cnt[HB];
static unsigned int s_phase[PH_N], s_lift, s_total;
static unsigned int s_last_presents;
/* frames longer than 45 ms (under ~22 FPS) and the longest one, per report:
 * the 4 s averages hide short drops */
extern volatile unsigned int g_dbg_presents;
static unsigned int s_fr_last, s_fr_run, s_fr_slow, s_fr_max;

void prof_sample(void)
{
    int ph = g_prof_phase;
    unsigned int key, h, p = g_dbg_presents;
    s_total++;
    if (p != s_fr_last) {
        if (s_fr_run > s_fr_max) s_fr_max = s_fr_run;
        if (s_fr_run > 45) s_fr_slow++;
        s_fr_run = 0;
        s_fr_last = p;
    } else s_fr_run++;
    if (ph < 0 || ph >= PH_N) ph = PH_GAME;
    s_phase[ph]++;
    if (ph != PH_GAME) return;
    /* lifted engine: g_cur_func is exact; C code: the last entered function */
    if (g_lift_depth) { s_lift++; key = g_cur_func | 0x80000000u; }
    else key = g_recomp_last;
    h = (key * 2654435761u) >> 21;
    for (;;) {
        h &= HB - 1;
        if (s_key[h] == key || s_cnt[h] == 0) { s_key[h] = key; s_cnt[h]++; return; }
        h++;
    }
}

void prof_report(unsigned int presents)
{
    static const char *names[PH_N] = { "game", "draw", "frame2d", "swap", "idle", "audiolock", "blt" };
    unsigned int i, k, t = s_total ? s_total : 1;
    char line[256];
    int n = 0;
    n += snprintf(line + n, sizeof line - n, "prof %u samples, %u frames:", s_total, presents - s_last_presents);
    for (i = 0; i < PH_N; i++) n += snprintf(line + n, sizeof line - n, " %s %u%%", names[i], s_phase[i] * 100 / t);
    rt_log("%s (lifted %u%%) slow %u max %u ms\n", line, s_lift * 100 / t, s_fr_slow, s_fr_max);
    s_fr_slow = s_fr_max = 0;
    for (k = 0; k < 14; k++) {            /* top functions */
        unsigned int best = 0, bi = 0;
        for (i = 0; i < HB; i++) if (s_cnt[i] > best) { best = s_cnt[i]; bi = i; }
        if (!best) break;
        rt_log("  %5.1f%% %s%08x\n", best * 100.0 / t, (s_key[bi] & 0x80000000u) ? "L " : "C ", s_key[bi] & 0x7fffffffu);
        s_cnt[bi] = 0;
    }
    memset(s_key, 0, sizeof s_key); memset(s_cnt, 0, sizeof s_cnt);
    memset(s_phase, 0, sizeof s_phase); s_lift = s_total = 0;
    s_last_presents = presents;
}
