/* plat_vita.c — PS Vita platform: vitaGL (960x544), sceCtrl, sceAudioOut.
 *
 * Game data (supplied by the user) lives in ux0:data/dayusadx/:
 *   DAYTONA USA Deluxe.exe (v1.20E), Resource.dll, Resource/, CDDA_BGM/
 * Controls (runtime/dx_sound_input.c): left stick / d-pad steer, cross
 * accelerate, square / circle brake, triangle view, L/R (or d-pad up/down)
 * shift, START = Enter, SELECT = Esc.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <vitaGL.h>
#include "plat.h"
#include "prof.h"

static void heartbeat_start(void);

/* the game uses deep call chains with large stack frames */
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;
/* The newlib heap is reserved at startup, before the C++ static constructors
 * (vitaGL's) run: if the reservation fails every malloc fails and the
 * constructors abort.  The vpk enables extended memory (ATTRIBUTE2=12,
 * vita/package.sh); vitaGL takes what is left for textures and buffers. */
unsigned int _newlib_heap_size_user = 160 * 1024 * 1024;

/* the softfp VitaSDK lacks the user-space Dmacmgr stub vitaGL wants */
int sceDmacMemcpy(void *dst, const void *src, SceSize size) { memcpy(dst, src, size); return 0; }

int plat_init(void)
{
    SceAppUtilInitParam ip;
    SceAppUtilBootParam bp;
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    memset(&ip, 0, sizeof ip);
    memset(&bp, 0, sizeof bp);
    sceAppUtilInit(&ip, &bp);
    sceCommonDialogSetConfigParam(&(SceCommonDialogConfigParam){0});
    vglInitExtended(0, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    /* the game thread gets core 0 to itself; audio and the profiler run on 1 and 2 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000);
    heartbeat_start();
    return 1;
}


/* heartbeat + sampling profiler (runtime/prof.c): every millisecond look at
 * what the main thread is doing; every 2 s log a heartbeat, every 4 s the
 * profile.  Runs on core 2 so it does not steal time from the game. */
/* background jobs (the music file loads): core 2, below the game */
struct job { void (*fn)(void *); void *arg; };
static int job_thread(SceSize args, void *argp)
{
    struct job j = *(struct job *)argp;
    (void)args;
    j.fn(j.arg);
    return sceKernelExitDeleteThread(0);
}
void plat_thread_start(void (*fn)(void *), void *arg)
{
    struct job j;
    SceUID th;
    j.fn = fn; j.arg = arg;
    th = sceKernelCreateThread("job", job_thread, 0x10000100 + 20, 0x8000, 0, 0x40000 /* core 2 */, NULL);
    if (th < 0) { fn(arg); return; }
    sceKernelStartThread(th, sizeof j, &j);
}

static int heartbeat_thread(SceSize args, void *argp)
{
    extern volatile unsigned int g_recomp_last, g_dbg_presents, g_dbg_polls;
    extern unsigned int g_cur_func;
    void rt_log(const char *fmt, ...);
    unsigned int t0 = plat_time_ms(), t_hb = t0, t_prof = t0;
    (void)args; (void)argp;
    for (;;) {
        unsigned int t;
        sceKernelDelayThread(1000);
        prof_sample();
        t = plat_time_ms();
        if (t - t_hb >= 2000) {
            t_hb = t;
            rt_log("hb t=%u presents=%u polls=%u fn=%08x lifted=%08x\n", t,
                   g_dbg_presents, g_dbg_polls, g_recomp_last, g_cur_func);
        }
        if (t - t_prof >= 4000) {
            t_prof = t;
            prof_report(g_dbg_presents);
        }
    }
    return 0;
}
static void heartbeat_start(void)
{
    SceUID th = sceKernelCreateThread("heartbeat", heartbeat_thread, 0x10000100, 0x4000, 0, 0x40000 /* core 2 */, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

void plat_shutdown(void) { sceKernelExitProcess(0); }

void plat_fatal(const char *msg)
{
    SceMsgDialogParam p;
    SceMsgDialogUserMessageParam um;
    sceMsgDialogParamInit(&p);
    memset(&um, 0, sizeof um);
    p.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    p.userMsgParam = &um;
    um.msg = (const SceChar8 *)msg;
    um.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    if (sceMsgDialogInit(&p) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

const char *plat_data_dir(void) { return "ux0:data/dayusadx/"; }
void plat_fix_case(char *path) { (void)path; }   /* ux0: is case-insensitive */

unsigned int plat_time_ms(void) { return (unsigned int)(sceKernelGetProcessTimeWide() / 1000); }
void plat_sleep_ms(unsigned int ms) { sceKernelDelayThread(ms * 1000); }
void plat_screen_size(int *w, int *h) { *w = 960; *h = 544; }
void plat_swap(void) { vglSwapBuffers(GL_FALSE); }

int plat_poll(PlatPad *pad)
{
    extern volatile unsigned int g_dbg_polls;
    g_dbg_polls++;
    SceCtrlData c;
    unsigned int b = 0;
    memset(&c, 0, sizeof c);
    sceCtrlPeekBufferPositive(0, &c, 1);
    if (c.buttons & SCE_CTRL_UP) b |= PAD_UP;
    if (c.buttons & SCE_CTRL_DOWN) b |= PAD_DOWN;
    if (c.buttons & SCE_CTRL_LEFT) b |= PAD_LEFT;
    if (c.buttons & SCE_CTRL_RIGHT) b |= PAD_RIGHT;
    if (c.buttons & SCE_CTRL_CROSS) b |= PAD_CROSS;
    if (c.buttons & SCE_CTRL_CIRCLE) b |= PAD_CIRCLE;
    if (c.buttons & SCE_CTRL_SQUARE) b |= PAD_SQUARE;
    if (c.buttons & SCE_CTRL_TRIANGLE) b |= PAD_TRIANGLE;
    if (c.buttons & SCE_CTRL_LTRIGGER) b |= PAD_L;
    if (c.buttons & SCE_CTRL_RTRIGGER) b |= PAD_R;
    if (c.buttons & SCE_CTRL_START) b |= PAD_START;
    if (c.buttons & SCE_CTRL_SELECT) b |= PAD_SELECT;
    pad->buttons = b;
    pad->lx = (int)c.lx - 128;
    pad->ly = (int)c.ly - 128;
    pad->rx = (int)c.rx - 128;
    pad->ry = (int)c.ry - 128;
    /* deadzone, rescaled so steering starts smoothly at its edge */
    pad->lx = pad->lx > 16 ? (pad->lx - 16) * 127 / 111 : pad->lx < -16 ? (pad->lx + 16) * 128 / 112 : 0;
    pad->ly = pad->ly > 16 ? (pad->ly - 16) * 127 / 111 : pad->ly < -16 ? (pad->ly + 16) * 128 / 112 : 0;
    if (b & PAD_LEFT) pad->lx = -128;
    if (b & PAD_RIGHT) pad->lx = 127;
    /* pedals: cross/R2 accelerate, square/L2 brake (digital on the Vita) */
    pad->rtrig = (b & PAD_CROSS) ? 255 : 0;
    pad->ltrig = (b & PAD_SQUARE) ? 255 : 0;
    return 1;
}

/* ---- audio ------------------------------------------------------------- */
static PlatAudioCb s_cb;
static SceKernelLwMutexWork s_mtx;
#define GRAIN 1024

static int audio_thread(SceSize args, void *argp)
{
    static short buf[2][GRAIN * 2];
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, 44100, SCE_AUDIO_OUT_MODE_STEREO);
    int cur = 0;
    (void)args; (void)argp;
    if (port < 0) return 0;
    for (;;) {
        sceKernelLockLwMutex(&s_mtx, 1, NULL);
        s_cb(buf[cur], GRAIN);
        sceKernelUnlockLwMutex(&s_mtx, 1);
        sceAudioOutOutput(port, buf[cur]);
        cur ^= 1;
    }
    return 0;
}

void plat_audio_start(PlatAudioCb cb)
{
    SceUID th;
    s_cb = cb;
    sceKernelCreateLwMutex(&s_mtx, "audio", 2, 0, NULL);
    th = sceKernelCreateThread("audio", audio_thread, 0x10000100, 0x10000, 0, 0x20000 /* core 1 */, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

void plat_audio_lock(void) { if (s_cb) sceKernelLockLwMutex(&s_mtx, 1, NULL); }
void plat_audio_unlock(void) { if (s_cb) sceKernelUnlockLwMutex(&s_mtx, 1); }

void plat_debug_stack(int maxf) { (void)maxf; }
