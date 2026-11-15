/* main.c — start the recompiled game.
 *
 *  1. platform: window/GL context, input, audio output
 *  2. load "DAYTONA USA Deluxe.exe" (v1.20E) from the data directory into
 *     g_image (its .data/.rdata: the game's tables, strings, initial state)
 *  3. call the game's WinMain (0x004434d0), which never returns until quit.
 */
#include "recomp.h"
#include "runtime.h"
#include "plat.h"
#include "render.h"
#include "audio.h"

void FUN_004434d0(undefined4 hinst, undefined4 hprev, undefined4 cmdline, undefined4 show);
int host_snprintf(char *buf, unsigned int n, const char *fmt, ...);
int host_fexists(const char *path);

int main(int argc, char **argv)
{
    char exe[512];
    static char cmdline[] = "";
    (void)argc; (void)argv;
    if (!plat_init()) return 1;
    rt_log("Daytona USA Deluxe - recompiled port. data: %s\n", rt_data_dir());
    host_snprintf(exe, sizeof exe, "%sDAYTONA USA Deluxe.exe", rt_data_dir());
    plat_fix_case(exe);
    if (!host_fexists(exe)) {
        rt_fatal("Game files not found.\n\nMake them from your original CD with the converter (DayConv.exe)\n"
                 "and copy its data\\dayusadx folder so the game is in:\n%s", rt_data_dir());
    }
    if (!image_load(exe)) rt_fatal("Could not load %s (needs the v1.20E executable)", exe);
    host_snprintf(exe, sizeof exe, "%sddse.dll", rt_data_dir());
    plat_fix_case(exe);
    if (!image_load_ddse(exe)) rt_fatal("Could not load %s", exe);
    rnd_init();
    audio_init();
    FUN_004434d0((undefined4)(recomp_uptr)g_image, 0, (undefined4)(recomp_uptr)cmdline, 1);
    rt_log("WinMain returned\n");
    plat_shutdown();
    return 0;
}
