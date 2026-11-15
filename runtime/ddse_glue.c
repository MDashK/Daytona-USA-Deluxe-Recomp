/* ddse_glue.c — DDSE.dll (the game's 2D sprite/polygon engine), recompiled
 * from the DLL into recomp/gen_ddse.  DdseFunc builds a table of drawing
 * functions (MMX or plain variants) and returns it. */
#include "w32.h"

void *DdseFunc(void);   /* recomp/gen_ddse */

int w32_DdseFunc()
{
    return (int)(recomp_uptr)DdseFunc();
}

/* WINCPUID.DLL: 0 = no CPU information -> DDSE picks its non-MMX routines */
int w32_GetProcessorID()
{
    return 0;
}
