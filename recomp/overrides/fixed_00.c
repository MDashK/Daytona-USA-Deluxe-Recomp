/* overrides/fixed_00.c — hand-corrected functions.
 *
 * Functions listed in recomp/overrides.txt are not emitted by gen.py; their
 * code lives here instead.  Each one notes why Ghidra's output was wrong.
 */
#include "recomp.h"
#include "types.h"
#include "w32_protos.h"
#include "crt.h"
#include "protos.h"

/* 0041fda2: fills the 20 ranking strings.  Ghidra mis-tracked the stack
 * across the call to FUN_004134b2 and read the call's return address as a
 * value (checked against the disassembly). */
void FUN_0041fda2(void)
{
    int i;
    for (i = 0; i < 0x14; i++) {
        char *rec = (char *)(recomp_uptr)(i * 0xb + G(int, 0x4be3bc));
        crt__sprintf((char *)VA(0x4bdd7c + i * 0x10), (char *)VA(0x4be414), i + 1);
        crt__sprintf((char *)VA(0x4bdebc + i * 0x10), (char *)VA(0x4be418), rec);
        FUN_004134b2((char *)VA(0x4bdffc + i * 0x10), *(recomp_s32u *)(rec + 7));
        crt__sprintf((char *)VA(0x4be13c + i * 0x10), (char *)VA(0x4be41c), *(byte *)(rec + 4) + 1);
        crt__sprintf((char *)VA(0x4be27c + i * 0x10), (char *)VA(0x4be420), *(byte *)(rec + 5) + 1);
    }
}

/* 00402972: fills the 20 record strings of the records screen.  Same stack
 * mis-tracking around FUN_004134b2 as FUN_0041fda2: Ghidra let the loop
 * counter alias an outgoing argument and wrote through a garbage pointer
 * (crashed on the Vita after entering a best time). */
void FUN_00402972(void)
{
    int i;
    for (i = 0; i < 0x14; i++) {
        char *rec = (char *)(recomp_uptr)(i * 0xb + G(int, 0x4b61a4));
        int n = crt__sprintf((char *)VA(0x4b5a24 + i * 0x20), (char *)VA(0x4b6400), i + 1, rec);
        FUN_004134b2((char *)VA(0x4b5a24 + i * 0x20 + n), *(recomp_s32u *)(rec + 7));
        crt__sprintf((char *)VA(0x4b5ca4 + i * 0x20), (char *)VA(0x4b640c), *(byte *)(rec + 4) + 1);
        crt__sprintf((char *)VA(0x4b5f24 + i * 0x20), (char *)VA(0x4b6410), *(byte *)(rec + 5) + 1);
    }
}

/* 00462bab: builds the colour/fog words of the 4-vertex sprite quad at
 * 0x514bd8.  With param_2 == NULL the original uses a local default colour
 * {0xff,0xff,0xff,0xff} (r,g,b,a); Ghidra dropped those four stores as dead,
 * leaving stack garbage as the vertex colour (scenery drawn almost black). */
void FUN_00462bab(int param_1, uint *param_2)
{
    uint def[4] = { 0xff, 0xff, 0xff, 0xff };
    int fog, k;
    uint r, g, b, a;
    if (param_2 == (uint *)0) param_2 = def;
    {   /* debug: DAYTONA_HALVELOG=1 logs which polygons get the half colour */
        extern int getenv_flag(const char *); extern void rt_log(const char *, ...);
        static int lg = -1, n;
        if (lg < 0) lg = getenv_flag("DAYTONA_HALVELOG");
        if (lg && n < 40000 && (n++ % 97) == 0)
            rt_log("poly flags=%04x tex=%d course=%d va=%08x\n", *(ushort *)(param_1 + 4), *(ushort *)(param_1 + 8), G(char, 0x861050),
                   recomp_host_to_va((void *)(recomp_uptr)param_1));
    }
    if ((*(ushort *)(param_1 + 4) & 2) != 0 && G(char, 0x861050) != 5) {
        param_2[0] >>= 1;
        param_2[1] >>= 1;
        param_2[2] >>= 1;
    }
    if ((*(ushort *)(param_1 + 4) & 0x100) != 0) param_2[3] >>= 1;
    fog = 0xff - ((int)*(ushort *)(param_1 + 0x1e) - G(int, 0x514c58));
    if (fog > 0xff) fog = 0xff;
    if (fog < 0) fog = 0;
    r = param_2[0]; g = param_2[1]; b = param_2[2]; a = param_2[3];
    for (k = 0; k < 4; k++) {
        *(uint *)VA(0x514be8 + k * 0x20) = g << 8 | a << 24 | r << 16 | b;
        *(uint *)VA(0x514bec + k * 0x20) = (uint)fog << 24;
    }
}

/* 0045ecea: creates the DirectInput joystick devices.  The function has a
 * 9 KB frame (allocated through _chkstk) and Ghidra lost the stack around
 * it: EnumDevices was called without arguments and the creation loop ran
 * `i < 0` times, so no joystick (analog steering) was ever opened. */
typedef struct {
    int max, count;
    unsigned char inst[16][0x244];          /* DIDEVICEINSTANCEA */
} JoyEnum;

static int joy_enum_cb(const unsigned char *inst, JoyEnum *e)    /* 0045ee26 */
{
    if (!inst) return 1;
    memcpy(e->inst[e->count], inst, 0x244);
    e->count++;
    return e->count < e->max ? 1 : 0;       /* DIENUM_CONTINUE / STOP */
}

void FUN_0045ecea(int *param_1, undefined4 param_2, undefined4 param_3, int param_4, int param_5)
{
    static JoyEnum e;
    typedef int (*enum_fn)(int *, uint, void *, void *, uint);
    typedef int (*method2)(int *, void *);
    typedef int (*method3)(int *, undefined4, uint);
    int i;
    (void)param_2;
    memset(&e, 0, sizeof e);
    e.max = param_5 > 16 ? 16 : param_5;
    /* IDirectInput::EnumDevices(DIDEVTYPE_JOYSTICK, cb, &e, DIEDFL_ATTACHEDONLY) */
    ((enum_fn)(recomp_uptr)(*(int **)param_1)[4])(param_1, 4, (void *)joy_enum_cb, &e, 1);
    for (i = 0; i < e.count; i++) {
        int **slot = (int **)(recomp_uptr)(param_4 + i * 4);
        if (FUN_0042cd54(param_1, (undefined4)(recomp_uptr)(e.inst[i] + 4), (undefined4)(recomp_uptr)slot, 0) < 0) {
            *slot = 0;
        } else {
            int *dev = *slot;
            ((method2)(recomp_uptr)(*(int **)dev)[11])(dev, VA(0x4a6dc0));               /* SetDataFormat(c_dfDIJoystick) */
            ((method3)(recomp_uptr)(*(int **)dev)[13])(dev, param_3, 9);                /* SetCooperativeLevel */
        }
    }
    for (; i < param_5; i++) *(int *)(recomp_uptr)(param_4 + i * 4) = 0;
}

/* 0040d4fc: first-run defaults of the settings block.  Port change: the
 * display flags start with high-polygon mode on (bit 3) besides background
 * (bit 0) and mph (bit 1); the PC default was 3. */
void FUN_0040d4fc(void)
{
    G(uint, 0x89cee8) = 0xb;
    G(int, 0x89ce00) = 0x7fffffff;
    G(int, 0x89ce04) = 0x7fffffff;
    G(int, 0x89ce08) = 0x7fffffff;
    G(int, 0x89ce0c) = 0x7fffffff;
    G(int, 0x89cdc0) = 0;
    G(int, 0x89cdc4) = 1;
    G(int, 0x89cdc8) = 2;
    G(int, 0x89cdcc) = 3;
    G(int, 0x89cdd0) = 4;
    G(int, 0x89cdd4) = 5;
    G(int, 0x89cdb8) = 1;
    G(int, 0x89cdbc) = 1;
    G(int, 0x89cdd8) = 0;
    G(int, 0x89cddc) = 1;
    G(int, 0x89cde0) = 0;
    G(int, 0x89cdf0) = 0;
    G(int, 0x89ce10) = 1;
    G(int, 0x89cde4) = 1;
    G(int, 0x89cdf4) = 1;
    FUN_00452bc0();
}

/* 00455770: main menu input.  Port change: the cursor skips entry 3
 * (Multiplayer, DirectPlay over the network, not available on the Vita).
 * Entries: 0 Arcade, 1 Time Attack, 2 1P vs 2P, 3 Multiplayer, 4 Records, 5 Exit. */
undefined4 FUN_00455770(short param_1, undefined4 param_2, int param_3)
{
    recomp_u32u keys = 0;
    uint pad = FUN_00441bae(param_3, &keys);
    (void)param_2;
    if (param_1 != 0 || G(int, 0x4df03c) != 3) return 1;
    {   /* the main menu is up: Select opens the port's options (runtime/menu.c) */
        extern volatile unsigned int g_mainmenu_stamp, g_dbg_presents;
        unsigned int plat_time_ms(void); void rt_log(const char *, ...);
        static int once;
        if (!once) { once = 1; rt_log("main menu at %u ms\n", plat_time_ms()); }
        g_mainmenu_stamp = g_dbg_presents;
    }
    if ((keys >> 8) & 0x40) {                 /* confirm */
        G(int, 0x4df03c) = 4;
        FUN_00468b40(0, 0x3e);
    } else if ((keys >> 8) & 0x20) {          /* back */
        FUN_00468b40(0, 0x3c);
        G(int, 0x4df03c) = 6;
    } else if (pad & 0x200) {                 /* up */
        do { if (--G(int, 0x4df040) < 0) G(int, 0x4df040) = 5; } while (G(int, 0x4df040) == 3);
        FUN_00468b40(0, 0x3d);
    } else if (pad & 0x400) {                 /* down */
        do { if (++G(int, 0x4df040) > 5) G(int, 0x4df040) = 0; } while (G(int, 0x4df040) == 3);
        FUN_00468b40(0, 0x3d);
    }
    return 1;
}

/* 004433c0: the main message loop.  On WM_QUIT the original leaves through
 * the compiler's SEH epilogue FUN_0044348f, which reads the caller's EBP;
 * Ghidra made that `0 - 0x1c` and quitting from the menu (Exit) crashed. */
WPARAM FUN_004433c0(HWND param_1)
{
    MSG m;
    for (;;) {
        while (!w32_PeekMessageA(&m, (HWND)0, 0, 0, 1)) FUN_004552a0(param_1);
        if (m.message == 0x12) break;                 /* WM_QUIT */
        if (FUN_0044ade3(&m) == 0) {
            w32_TranslateMessage(&m);
            w32_DispatchMessageA(&m);
        }
    }
    return m.wParam;
}

/* ---- draw distance --------------------------------------------------------
 * The course table loaded at 0x604500 (CSx_BLK.BIN) has one 0x30-byte record
 * per track section at <header[0]> + n*0x30; +0x24 points at the list of the
 * course blocks drawn from there looking ahead (count, block numbers: the next
 * ~9 blocks), +0x28 at the one looking back.  The far end of the ahead list
 * is where scenery pops in.  Port option (runtime/menu.c): longer ahead lists,
 * made from the following sections' lists (and the previous section's, for
 * the sides in curves and widescreen); stored in our own memory, the records
 * point there (offsets are relative to the table, 32-bit wrap). */
void rt_log(const char *fmt, ...);
void *host_malloc(unsigned int n);
void host_free(void *p);
static unsigned short *s_blk_lists;

static void blk_extend(void)
{
    extern int g_opt_drawdist;
    unsigned char *base = (unsigned char *)VA(0x604500);
    unsigned int hdr0 = *(recomp_u32u *)base, n = 0, r, total = 0;
    /* these lists do not pick the scenery (the engine draws N consecutive
     * blocks, runtime/lifted_rt.c lifted_draw_distance); experiment only */
    extern char *getenv(const char *);
    const char *ee = getenv("DAYTONA_BLKEXTEND");
    int extra = ee ? (ee[0] - '0') * 3 : 0;
    (void)g_opt_drawdist;
    int bmin = 0x7fff, bmax = 0, f;
    unsigned short *out;
    if (s_blk_lists) { host_free(s_blk_lists); s_blk_lists = 0; }
    if (!extra || hdr0 < 16 || hdr0 > 0x4000) return;
    /* the lists run from the nearest block outwards (ids +1 or -1 per block,
     * wrapping round the lap): continue each past its far end */
    for (n = 0; n < 64; n++) {
        unsigned char *rec = base + hdr0 + n * 0x30;
        unsigned int a = *(recomp_u32u *)(rec + 0x24), b = *(recomp_u32u *)(rec + 0x28);
        if (a >= hdr0 || b >= hdr0 || (!a && !b)) break;
        for (f = 0; f < 2; f++) {
            unsigned int o = f ? b : a;
            const recomp_u16u *l = (const recomp_u16u *)(base + o);
            int i;
            if (!o) continue;
            for (i = 0; i < l[0]; i++) {
                if (l[1 + i] < bmin) bmin = l[1 + i];
                if (l[1 + i] > bmax) bmax = l[1 + i];
            }
        }
    }
    if (n < 3 || bmax <= bmin) return;
    s_blk_lists = (unsigned short *)host_malloc(n * 2 * 40 * 2);
    if (!s_blk_lists) return;
    out = s_blk_lists;
    for (r = 0; r < n; r++) {
        for (f = 0; f < 2; f++) {
            unsigned char *field = base + hdr0 + r * 0x30 + 0x24 + 4 * f;
            unsigned int o = *(recomp_u32u *)field;
            const recomp_u16u *l = (const recomp_u16u *)(base + o);
            unsigned short ids[40];
            int cnt = 0, i, k, step, next, added = 0, guard = 0;
            if (!o || l[0] < 2) continue;
            for (i = 0; i < l[0] && cnt < 40; i++) ids[cnt++] = l[1 + i];
            step = ids[cnt - 1] - ids[cnt - 2];
            if (step != 1 && step != -1) {               /* a wrap at the end: */
                step = ids[cnt - 1] == bmin ? -1 : ids[cnt - 1] == bmax ? 1 : 0;
                if (!step) continue;
            }
            next = ids[cnt - 1];
            while (added < extra && cnt < 40 && guard++ < 64) {
                next += step;
                if (next > bmax) next = bmin;
                if (next < bmin) next = bmax;
                for (k = 0; k < cnt && ids[k] != next; k++) { }
                if (k < cnt) break;                       /* came round: the whole lap */
                ids[cnt++] = (unsigned short)next;
                added++;
            }
            out[0] = (unsigned short)cnt;
            for (i = 0; i < cnt; i++) out[1 + i] = ids[i];
            *(recomp_u32u *)field = (unsigned int)((recomp_uptr)out - (recomp_uptr)base);
            total += (unsigned)cnt;
            out += 1 + cnt;
        }
    }
    rt_log("draw distance: %u sections, blocks %x-%x, +%d per list (%u entries)\n", n, bmin, bmax, extra, total);
}

/* 004337e0: loads the course block table; port: then lengthens the lists */
void FUN_004337e0(undefined4 param_1, void *param_2)
{
    unsigned short i, j;
    unsigned char b;
    unsigned int course;
    const recomp_u16u *t;
    unsigned char *p;
    G(void *, 0x8641a4) = param_2;
    FUN_00413517(param_1, param_2);
    course = G(unsigned char, 0x861050);
    G(unsigned short, 0x8640ca) = *(recomp_u16u *)VA(0x4b1e28 + course * 4);
    G(unsigned short, 0x8640cc) = *(recomp_u16u *)VA(0x4b1e2a + course * 4);
    G(unsigned short, 0x86419c) = *(recomp_u16u *)VA(0x4b1e78 + course * 2);
    G(unsigned short, 0x86419e) = *(recomp_u16u *)VA(0x4b1e88 + course * 2);
    t = (const recomp_u16u *)VA(0x4b1e40 + course * 8);
    p = (unsigned char *)VA(0x604500) + G(int, 0x604508);
    if (*t == 0) {
        for (i = 0; i < G(unsigned short, 0x8640cc); i++) { *p = (unsigned char)(*p << 4); p += 0x2c; }
    } else {
        j = 0;
        for (i = 0; i < 4; i++) {
            b = ((unsigned char *)VA(0x4b1e70))[i];
            for (; j <= *t; j++) { *p = (unsigned char)(b | *p << 4); p += 0x2c; }
            t++;
        }
        b = ((unsigned char *)VA(0x4b1e70))[i];
        for (; j <= G(unsigned short, 0x8640cc); j++) { *p = (unsigned char)(b | *p << 4); p += 0x2c; }
    }
    if (course == 2) {
        int n = G(int, 0x604500);
        ((unsigned char *)VA(0x607503))[n] = 1;
        ((unsigned char *)VA(0x607533))[n] = 1;
        ((unsigned char *)VA(0x607563))[n] = 1;
        ((unsigned char *)VA(0x607593))[n] = 2;
        ((unsigned char *)VA(0x6075c3))[n] = 2;
        ((unsigned char *)VA(0x6075f3))[n] = 2;
    }
    blk_extend();
}

/* ---- the game's software fades (port: speed) ------------------------------
 * The generated code kept every loop variable in the stack frame and read the
 * pixels through unaligned-safe (byte) accessors: the attract -> title fade
 * ran at half speed on the Vita.  Same results, plain loops.  The DDSURFACEDESC
 * blocks hold lPitch at +0x10 and lpSurface at +0x24; rects are left, top,
 * right, bottom. */
typedef unsigned short px16;
char *getenv(const char *name);
extern volatile unsigned int g_dbg_presents;
static void fade_log(const char *fn, int dd, int sd, recomp_s32u *r, int lvl)
{
    static int on = -1, n;
    if (on < 0) on = getenv("DAYTONA_FADELOG") != 0;
    if (on && n++ < 300)
        rt_log("fade %s f=%u dst=%08x src=%08x rect=%d,%d,%d,%d arg=%08x\n", fn, g_dbg_presents,
               *(recomp_u32u *)(dd + 0x24), *(recomp_u32u *)(sd + 0x24), r[0], r[1], r[2], r[3], lvl);
}

/* 00437bb1: dst = lut[src] over a rect (fade through a 64K-entry table) */
void FUN_00437bb1(int param_1, recomp_s32u *param_2, int param_3, recomp_s32u *param_4, int param_5)
{
    int w = param_4[2] - param_4[0], h = param_4[3] - param_4[1], x, y;
    unsigned sp = *(recomp_u32u *)(param_3 + 0x10) >> 1, dp = *(recomp_u32u *)(param_1 + 0x10) >> 1;
    const px16 *src = (const px16 *)(recomp_uptr)*(recomp_u32u *)(param_3 + 0x24) + param_4[1] * sp + param_4[0];
    px16 *dst = (px16 *)(recomp_uptr)*(recomp_u32u *)(param_1 + 0x24) + param_2[1] * dp + param_2[0];
    const px16 *lut = (const px16 *)(recomp_uptr)(unsigned)param_5;
    fade_log("lut", param_1, param_3, param_4, param_5);
    for (y = 0; y < h; y++, src += sp, dst += dp)
        for (x = 0; x < w; x++) dst[x] = lut[src[x]];
}

/* 00436ea4: moves each colour channel of dst towards src by at most param_5
 * (src pixels equal to src's first pixel are transparent); channel masks and
 * shifts of the pixel format at 0x864bfc.. */
void FUN_00436ea4(int param_1, recomp_s32u *param_2, int param_3, recomp_s32u *param_4, int param_5)
{
    int w = param_4[2] - param_4[0], h = param_4[3] - param_4[1], x, y, c;
    unsigned sp = *(recomp_u32u *)(param_3 + 0x10) >> 1, dp = *(recomp_u32u *)(param_1 + 0x10) >> 1;
    const px16 *src0 = (const px16 *)(recomp_uptr)*(recomp_u32u *)(param_3 + 0x24);
    const px16 *src = src0 + param_4[1] * sp + param_4[0];
    px16 *dst = (px16 *)(recomp_uptr)*(recomp_u32u *)(param_1 + 0x24) + param_2[1] * dp + param_2[0];
    unsigned key = *src0;
    fade_log("xfade", param_1, param_3, param_4, param_5);
    unsigned mask[3], shift[3];
    for (c = 0; c < 3; c++) {
        mask[c] = G(unsigned, 0x864c08 + c * 4);
        shift[c] = G(unsigned, 0x864bfc + c * 4) & 31;
    }
    for (y = 0; y < h; y++, src += sp, dst += dp)
        for (x = 0; x < w; x++) {
            unsigned s = src[x], d, out = 0;
            if (s == key) continue;
            d = dst[x];
            for (c = 0; c < 3; c++) {
                int a = (int)(s & mask[c]) >> shift[c], b = (int)(d & mask[c]) >> shift[c];
                if (b < a) { if (b + param_5 < a) b = a - param_5; }
                else if (a < b && a + param_5 < b) b = a + param_5;
                out |= (unsigned)b << shift[c];
            }
            dst[x] = (px16)out;
        }
}

/* 00437afd: copies a rect */
void FUN_00437afd(int param_1, recomp_s32u *param_2, int param_3, recomp_s32u *param_4)
{
    int w = param_4[2] - param_4[0], h = param_4[3] - param_4[1], y;
    unsigned sp = *(recomp_u32u *)(param_3 + 0x10) >> 1, dp = *(recomp_u32u *)(param_1 + 0x10) >> 1;
    const px16 *src = (const px16 *)(recomp_uptr)*(recomp_u32u *)(param_3 + 0x24) + param_4[1] * sp + param_4[0];
    px16 *dst = (px16 *)(recomp_uptr)*(recomp_u32u *)(param_1 + 0x24) + param_2[1] * dp + param_2[0];
    fade_log("copy", param_1, param_3, param_4, 0);
    if (w <= 0) return;
    for (y = 0; y < h; y++, src += sp, dst += dp) crt__memcpy(dst, (void *)src, (size_t)w * 2);
}

/* 00437911: one step of a screen fade: dst rect = lut[level](snapshot), or a
 * copy (level 255) / black fill (level 0).  The whole rect is overwritten, so
 * the back buffer Lock skips the 3D readback (dx_ddraw.c) - it cost a GPU
 * stall every fade frame on the Vita. */
extern int g_lock_noreadback;
void dd_fade_level(int level);           /* dx_ddraw.c: dims the widescreen sides */
undefined4 FUN_00437911(int param_1)
{
    int f, lvl;
    recomp_s32u pt[2];
    unsigned int desc[0x1b];
    if (!param_1 || !*(recomp_s32u *)(param_1 + 0x1c)) return 1;
    pt[0] = *(recomp_s32u *)(param_1 + 0xc);
    pt[1] = *(recomp_s32u *)(param_1 + 0x10);
    f = *(recomp_s32u *)(param_1 + 0x1c);
    lvl = *(recomp_s32u *)(f + 4);
    dd_fade_level(lvl);
    g_lock_noreadback = 1;
    if (lvl < 0xff && lvl < 1) {
        FUN_00458027(*(recomp_s32u **)(param_1 + 8), (LONG *)(param_1 + 0xc), 0);
    } else {
        crt__memset(desc, 0, 0x6c);
        desc[0] = 0x6c;
        FUN_00457bf8(*(recomp_s32u **)(param_1 + 8), 0, (undefined4)(recomp_uptr)desc, 1, 0);
        if (lvl >= 0xff)
            FUN_00437afd((int)(recomp_uptr)desc, pt, f + 0x18, (recomp_s32u *)(param_1 + 0xc));
        else if (desc[0x54 / 4] == 0x10)       /* ddpfPixelFormat.dwRGBBitCount */
            FUN_00437bb1((int)(recomp_uptr)desc, pt, f + 0x18, (recomp_s32u *)(param_1 + 0xc),
                         f + ((lvl + (lvl >> 31 & 7)) >> 3 & 0x1f) * 0x20000 + 0x84);
        FUN_00457c62(*(recomp_s32u **)(param_1 + 8), 0);
    }
    g_lock_noreadback = 0;
    return 0;
}

/* 0043750a: builds the 32 fade levels (param_2 + level * 0x20000, 64K entries
 * each) for every colour present in the snapshot (desc param_1: height +8,
 * width +0xc, pitch +0x10, pixels +0x24).  Each channel goes through the level
 * row of the table at 0x4c5930 (32 shorts a row; the 6-bit green index runs
 * into the next row, as in the original).  The original's "done" bitmap set a
 * different bit than it tested, so it recomputed colours; the entries depend
 * only on the colour, so a correct bitmap gives the same table. */
void FUN_0043750a(int param_1, int param_2)
{
    static unsigned done[2048];
    unsigned h = *(recomp_u32u *)(param_1 + 8), w = *(recomp_u32u *)(param_1 + 0xc);
    unsigned pitch = *(recomp_u32u *)(param_1 + 0x10) & ~1u, x, y;
    const unsigned char *row = (const unsigned char *)(recomp_uptr)*(recomp_u32u *)(param_1 + 0x24);
    const short *tbl = (const short *)VA(0x4c5930);
    unsigned mr = G(unsigned, 0x864c08), mg = G(unsigned, 0x864c0c), mb = G(unsigned, 0x864c10);
    unsigned sr = G(unsigned, 0x864bfc) & 31, sg = G(unsigned, 0x864c00) & 31, sb = G(unsigned, 0x864c04) & 31;
    px16 *lut = (px16 *)(recomp_uptr)(unsigned)param_2;
    crt__memset(done, 0, sizeof done);
    for (y = 0; y < h; y++, row += pitch) {
        const px16 *p = (const px16 *)row;
        for (x = 0; x < w; x++) {
            unsigned c = p[x], l, ir, ig, ib;
            if (!c || (done[c >> 5] & (1u << (c & 31)))) continue;
            done[c >> 5] |= 1u << (c & 31);
            ir = (int)(c & mr) >> sr; ig = (int)(c & mg) >> sg; ib = (int)(c & mb) >> sb;
            for (l = 0; l < 32; l++)
                lut[l * 0x10000 + c] = (px16)((tbl[l * 32 + ig] << sg) | (tbl[l * 32 + ib] << sb) | (tbl[l * 32 + ir] << sr));
        }
    }
}

/* ---- the port's car cheat (runtime/menu.c: Triangle x7 on the main menu) ----
 * 0x89ce5c = extra cars unlocked (0..3; the car lists show 8 + it), part of
 * the settings block saved as DAYTONA_PROTECTED_OBJECT.  While the cheat is
 * on it reads 3; these two keep the real count for the game's own progress
 * and for the save. */
void port_cars_real(int enter);
void port_cars_save(int enter);

/* 0045b030: race end (Ghidra's code); the unlock progress (wins per course
 * and difficulty in 0x89cee4) runs on the real count. */
void FUN_0045b030(void)
{
    uint uVar1;
    uint local_1c;
    G(unsigned char, 0x517200) = 0;
    FUN_00452397();
    FUN_00453280();
    G(short, 0x8666da) = (short)(G(short, 0x8666da) + 1);
    FUN_0045b167();
    if (G(char, 0x517209) == '\x01') {
        port_cars_real(1);
        local_1c = G(uint, 0x89cee4);
        uVar1 = 1u << ((char)G(int, 0x89ce5c) * 6 + (char)G(int, 0x89ce18) & 0x1fU);
        if (G(int, 0x89ce5c) == 0) {
            if ((0 < G(int, 0x89cddc)) && (local_1c = G(uint, 0x89cee4) | uVar1, local_1c == 0x3f))
                G(int, 0x89ce5c) = 1;
        } else if (G(int, 0x89ce5c) == 1) {
            if ((1 < G(int, 0x89cddc)) && (local_1c = G(uint, 0x89cee4) | uVar1, local_1c == 0xfff))
                G(int, 0x89ce5c) = 2;
        } else if (G(int, 0x89ce5c) == 2 && G(char, 0x8640c0) == '\t' && 0 < G(int, 0x89cddc) &&
                   (local_1c = G(uint, 0x89cee4) | uVar1, local_1c == 0x3ffff)) {
            G(int, 0x89ce5c) = 3;
        }
        G(uint, 0x89cee4) = local_1c;
        port_cars_real(0);
    }
}

/* 0040d493: load (0) / save (1) of the settings block 0x89c6a0..+0xe94. */
void FUN_0040d493(int param_1)
{
    if (param_1 == 0) {
        if (FUN_00418f57(VAI(0x4b9354), (LPCSTR)VA(0x4b9338), (LPBYTE)VA(0x89c6a0), 0xe94, VAI(0x4ec4d0)) == 0)
            FUN_0040d4fc();
    } else {
        port_cars_save(1);
        FUN_00418da4(VAI(0x4b9330), (LPCSTR)VA(0x4b9314), (BYTE *)VA(0x89c6a0), 0xe94, VAI(0x4ec4d0));
        port_cars_save(0);
    }
}
