/* rt_core.c — logging, fatal errors, path mapping, bad code pointers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "recomp.h"
#include "runtime.h"
#include "plat.h"

static FILE *s_log;

static void log_open(void)
{
    char p[512];
    if (s_log) return;
    snprintf(p, sizeof p, "%sdaytona_log.txt", rt_data_dir());
    s_log = fopen(p, "w");
}

/* progress counters for the platform heartbeat */
volatile uint g_recomp_last;
volatile unsigned int g_dbg_presents, g_dbg_polls;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    log_open();
    va_start(ap, fmt);
    if (s_log) { vfprintf(s_log, fmt, ap); fflush(s_log); }
    va_end(ap);
#ifndef __vita__
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#endif
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s\n", buf);
    plat_fatal(buf);
    exit(1);
}

void recomp_unimplemented(const char *what, uint va)
{
    rt_log("unimplemented: %s (0x%08x)\n", what, va);
}

void recomp_trap_call(uint va)
{
    rt_fatal("call to an address without recompiled code: 0x%08x", va);
}

/* Code pointers in .data that do not start a recompiled function become
 * pointers into g_image; calling one lands in invalid memory, so make them
 * point at the image copy (harmless if only compared or read as data). */
void *rt_bad_code_ptr(unsigned int va)
{
    return VA(va);
}

uint recomp_fs_dummy[16];

const char *rt_data_dir(void)
{
    return plat_data_dir();
}

/* "C:\\Program Files\\SEGA\\Daytona\\Resource\\x.bmp", ".\\Resource\\x.bmp",
 * "Resource\\x.bmp" -> "<datadir>Resource/x.bmp" (case preserved). */
void rt_map_path(const char *in, char *out, int outsz)
{
    char tmp[512];
    const char *p = in;
    int i;
    const char *res;
    snprintf(tmp, sizeof tmp, "%s", in);
    for (i = 0; tmp[i]; i++) if (tmp[i] == '\\') tmp[i] = '/';
    p = tmp;
    if (p[0] && p[1] == ':') p += 2;
    /* anything under the game directory: keep from the first known folder */
    res = NULL;
    {
        static const char *roots[] = { "resource/", "cdda_bgm/", "ghost/", "save/", NULL };
        char low[512];
        int k;
        for (i = 0; p[i] && i < 511; i++) low[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
        low[i] = 0;
        for (k = 0; roots[k] && !res; k++) {
            char *q = strstr(low, roots[k]);
            if (q) res = p + (q - low);
        }
    }
    if (!res) {
        res = p;
        while (res[0] == '.' && res[1] == '/') res += 2;
        while (res[0] == '/') {
            const char *s = strrchr(res, '/');
            res = s + 1;   /* absolute path outside the game: keep the file name */
        }
    }
    snprintf(out, outsz, "%s%s", rt_data_dir(), res);
    plat_fix_case(out);
}

code *swi(int n)
{
    rt_fatal("x86 software interrupt %d reached (data decoded as code)", n);
    return 0;
}

#ifdef RECOMP_TRACE
/* first entry into each function is logged once, with a running call count */
static unsigned char s_seen[0xb0000 / 8];
static unsigned int s_calls;
void recomp_enter(uint va)
{
    uint i = va - 0x401000;
    s_calls++;
    if (i < 0xb0000 && !(s_seen[i >> 3] & (1 << (i & 7)))) {
        s_seen[i >> 3] |= (unsigned char)(1 << (i & 7));
        rt_log("enter %08x (#%u)\n", va, s_calls);
    }
}
#endif

/* MMX PMADDWD (only in DDSE's MMX routines, which are not selected) */
unsigned long long pmaddwd(unsigned long long a, unsigned long long b)
{
    int r0 = (short)(a) * (short)(b) + (short)(a >> 16) * (short)(b >> 16);
    int r1 = (short)(a >> 32) * (short)(b >> 32) + (short)(a >> 48) * (short)(b >> 48);
    return (unsigned long long)(unsigned int)r0 | ((unsigned long long)(unsigned int)r1 << 32);
}
