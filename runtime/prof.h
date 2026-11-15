/* prof.h — sampling profiler hooks.
 *
 * The main thread publishes what it is doing (g_prof_phase, plus the lifted
 * routine / recompiled function it is in); a sampler thread on another core
 * (vita/plat_vita.c) reads it every millisecond and logs a breakdown. */
#ifndef PROF_H
#define PROF_H
enum { PH_GAME, PH_DRAW, PH_FRAME2D, PH_SWAP, PH_IDLE, PH_AUDIOLOCK, PH_BLT, PH_N };
extern volatile int g_prof_phase;
extern volatile unsigned int g_lift_depth;
#define PROF_PHASE(p) int _prof_prev = g_prof_phase; g_prof_phase = (p)
#define PROF_END() (g_prof_phase = _prof_prev)
void prof_sample(void);          /* called by the sampler thread */
void prof_report(unsigned int presents);
#endif
