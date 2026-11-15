/* lifted_rt.c — state shared by the instruction-lifted engine routines:
 * the x86 registers, x87 stack, flags and a private guest stack. */
#include <stdio.h>
#include <stdlib.h>
#include "lifted.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double   g_st[8];
int      g_fp_top;
uint16_t g_fpu_cw = 0x27f;
uint32_t g_cur_func;
ptrdiff_t g_mem_base;
uint32_t g_fs_base, g_gs_base, g_es_base;

#define GUEST_STACK (1024 * 1024)
static uint32_t *s_stack;

void lifted_stack_init(void)
{
    if (!s_stack) s_stack = (uint32_t *)calloc(1, GUEST_STACK);
    g_esp = (uint32_t)(uintptr_t)((unsigned char *)s_stack + GUEST_STACK - 64);
}

void rt_fatal(const char *fmt, ...);
void lifted_null_call(uint32_t from)
{
    rt_fatal("lifted engine: call through a null pointer in 0x%08x", from);
}

/* called by the host crash reporter */
void rt_log(const char *fmt, ...);
extern volatile unsigned int g_dbg_presents;
unsigned int recomp_host_to_va(const void *p);
void lifted_dump_state(void)
{
    uint32_t a;
    rt_log("lifted: cur=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
           g_cur_func, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp);
    rt_log("memregs:");
    for (a = 0x4e949c; a <= 0x4e94f4; a += 4) rt_log(" %x=%08x", a & 0xff, *(uint32_t *)(uintptr_t)VAI(a));
    rt_log("\n");
}

void lifted_unbalanced(const char *fn, uint32_t before, uint32_t after)
{
    static int n;
    if (n++ < 20) rt_log("lifted: %s left the stack unbalanced by %d bytes\n", fn, (int)(after - before));
    (void)before;
}

/* segment registers (flat model) and MMX registers */
uint16_t g_seg_cs = 0x1b, g_seg_ds = 0x23, g_seg_es = 0x23, g_seg_fs = 0x3b, g_seg_gs = 0, g_seg_ss = 0x23;
uint64_t g_mm[8];

extern const struct lifted_pair lifted_pairs[];
extern const int lifted_pairs_count;
extern const struct lifted_pair lifted_icall_map[];
extern const int lifted_icall_map_count;

static int pair_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const struct lifted_pair *)a)->entry, y = (uintptr_t)((const struct lifted_pair *)b)->entry;
    return x < y ? -1 : x > y;
}

static lifted_fn pair_find(struct lifted_pair **sorted, const struct lifted_pair *tab, int n, lifted_fn key)
{
    int lo = 0, hi = n - 1;
    if (!*sorted) {
        *sorted = (struct lifted_pair *)malloc(sizeof(struct lifted_pair) * (n ? n : 1));
        memcpy(*sorted, tab, sizeof(struct lifted_pair) * n);
        qsort(*sorted, n, sizeof(struct lifted_pair), pair_cmp);
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uintptr_t v = (uintptr_t)(*sorted)[mid].entry;
        if (v == (uintptr_t)key) return (*sorted)[mid].raw;
        if (v < (uintptr_t)key) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* raw lifted body of an entry wrapper / C wrapper (for tail jumps) */
lifted_fn lifted_raw_of(lifted_fn entry)
{
    static struct lifted_pair *sorted;
    return pair_find(&sorted, lifted_pairs, lifted_pairs_count, entry);
}

/* what lifted code calls for a host code pointer: C wrappers of lifted
 * routines -> their guest-stack entry, C functions with arguments -> bridge */
void *recomp_code_ptr(uint32_t va);
unsigned int recomp_host_to_va(const void *p);
lifted_fn lifted_icall_of(lifted_fn fn)
{
    static struct lifted_pair *sorted;
    lifted_fn m = pair_find(&sorted, lifted_icall_map, lifted_icall_map_count, fn);
    if (m) return m;
    {   /* a game code address instead of a host one: the Uma 2 car (horse)
         * draws a part through 0x473d80 stored as a plain address (the image
         * relocations only fix the pointers in the exe's own data) */
        uint32_t va = recomp_host_to_va((const void *)fn);   /* inside g_image (host code never is) */
        if (va >= 0x401000u && va < 0x4ae000u) {
            static int logged;
            lifted_fn c = (lifted_fn)recomp_code_ptr(va);
            if (logged < 4) { logged++; rt_log("icall: game address %08x -> %s\n", va, c ? "recompiled code" : "nothing"); }
            if (c) {
                m = pair_find(&sorted, lifted_icall_map, lifted_icall_map_count, c);
                return m ? m : c;
            }
        }
    }
    return fn;
}

/* ---- DAYTONA_LPROBE: engine register dumps at chosen routine entries */
unsigned int recomp_host_to_va(const void *p);
int g_lprobe_on = -1;
static uint32_t s_lp_va[16];
static int s_lp_n, s_lp_hits[16];
/* DAYTONA_LCOUNT=from:to - calls per frame of each lifted routine over race
 * frames [from,to) (finding the engine's loops, debug) */
static unsigned int *s_lc;
uint32_t g_lprobe_caller;

/* Draw distance (port option, runtime/menu.c, g_opt_drawdist 0 NORMAL /
 * 1 FAR / 2 FARTHEST / 3 MAXIMUM).  The engine (lifted_0047b008) takes N consecutive
 * course blocks from a start block, stepping +1/-1 with the track direction;
 * N and the start come from the viewer (0x516d7c +0x68/+0x69, +0x6f).  It
 * marks them visible (the AI cars use that), then 0x47b46c splits the range by
 * polygon count: the near blocks are drawn by lifted_0047bbe8 without any
 * culling, the far ones by 0x47bd70 (run in the second CPU context, an arcade
 * leftover) with a 4-corner test each (0x47e22d).
 *  - lifted_far_extend: more far blocks (cheap when off screen: 4 points)
 *  - lifted_draw_distance: a block behind the start (widescreen sides)
 *  - lifted_corner_*: the far test skipped a block when all 4 corners were off
 *    screen, even on different sides, so big ground blocks crossing the view
 *    vanished (floor missing under visible buildings); now only when all are
 *    off the same side or beyond the far limit. */
static int s_blk_start, s_blk_count, s_far_count, s_corner_fix;
/* the far blocks go into a fixed polygon buffer (camera 0x56f000, +0xa8 from
 * 0x2100 up to its end 0x3fc0): past the end the engine wrote over its other
 * data (a crash in a replay with 24 blocks more).  The extra blocks follow the
 * last frame's fill, which also keeps the heavy views from dropping frames. */
#define FAR_FILL_HI 0x2c00    /* ~ the polygons FARTHEST reached in heavy views (Vita FPS) */
#define FAR_FILL_LO 0x2800
static int s_far_fill = 0x2100, s_far_extra = -1, s_far_now = -1, s_rest_mode;
unsigned int plat_time_ms(void);
static int drawdist_level(void)
{
    extern int g_opt_drawdist;
    return g_opt_drawdist < 0 ? 0 : g_opt_drawdist > 3 ? 3 : g_opt_drawdist;
}
/* after the marking loop (0x47b29f): also flag the block behind the range's
 * start, where cars beside the camera blink in widescreen (experiment:
 * DAYTONA_MARKBEHIND=1; the flags also steer the AI) */
void lifted_mark_behind(void)
{
    static int on = -1;
    int step = (int8_t)MEM8(VAI(0x4ea4ac)), mask = (int16_t)MEM16(VAI(0x8640ca)), b;
    if (on < 0) { const char *e = getenv("DAYTONA_MARKBEHIND"); on = e && *e == '1'; }
    if (!on || (step != 1 && step != -1) || mask <= 0) return;
    b = ((int)MEM32(VAI(0x4e949c)) - step) & mask;
    MEM8(VAI(0x4ea048) + (uint32_t)b) = 1;
}

void lifted_far_extend(void)
{
    static const int ahead[4] = { 0, 4, 8, 24 }, behind[4] = { 0, 0, 1, 2 };   /* 24: still in the far polygon buffer (32 overflowed it) */
    int d = drawdist_level();
    int near = (int)MEM32(VAI(0x4e94b8)), far = (int)MEM32(VAI(0x4e94a4));
    int step = (int8_t)MEM8(VAI(0x4ea4ac)), nblk = (int16_t)MEM16(VAI(0x8640ca)) + 1, add;
    s_far_count = far;
    s_far_now = far;
    if (!d || near <= 0 || far < 0 || nblk <= 1 || (step != 1 && step != -1)) return;
    add = ahead[d];
    if (s_far_extra < 0 || s_far_extra > add) s_far_extra = add;
    {   /* and the frame time: the game is capped at 29.4 FPS (34 ms a frame);
         * frames that take longer (a heavy curve on the Vita) cut the extra
         * blocks at once, and they come back slowly while it holds the cap */
        static unsigned int last, ema = 34 << 4;
        unsigned int now = plat_time_ms(), dt = last ? now - last : 34;
        last = now;
        if (dt > 200) dt = 200;
        ema = (ema * 7 + (dt << 4)) / 8;                 /* x16 fixed point */
        if (ema > (355 << 4) / 10) s_far_extra = s_far_extra > 2 ? s_far_extra - 2 : 0;
        else if (s_far_fill < FAR_FILL_LO && ema < (348 << 4) / 10 && s_far_extra < add && !(now & 3)) s_far_extra++;
    }
    if (s_far_fill > FAR_FILL_HI) s_far_extra = s_far_extra > 2 ? s_far_extra - 2 : 0;
    add = s_far_extra;
    {   /* DAYTONA_AHEAD=n / DAYTONA_FARZ=hex: override the top level (testing) */
        static int ov = -2;
        if (ov == -2) { const char *e = getenv("DAYTONA_AHEAD"); ov = e ? atoi(e) : -1; }
        if (d == 3 && ov >= 0) add = ov;
    }
    if (near + far + add + behind[d] > nblk) add = nblk - behind[d] - near - far;
    if (add > 0) MEM32(VAI(0x4e94a4)) = (uint32_t)(far + add);
    s_far_now = (int)MEM32(VAI(0x4e94a4));
}

/* MAXIMUM: after the far pass, the same pass over the rest of the loop (from
 * the end of the far blocks round to the block behind the near ones).  Its
 * 4-corner test keeps only what is in view (the other side of an oval, the
 * infield ground, a pit area across the track); skipped when the far polygon
 * buffer is filling up.  Same call as 0x47b3b5..0x47b3ec. */
void FUN_0044bd60(void);
unsigned int plat_time_ms(void);
void lifted_entry_0047bd70(void);
void lifted_far_rest(void)
{
    uint32_t save[23];
    int d = drawdist_level(), i, near, step, nblk, rest, start;
    {   /* off by default: the Vita lost frames for little gain; DAYTONA_RESTPASS=1 */
        static int on = -1;
        if (on < 0) { const char *e = getenv("DAYTONA_RESTPASS"); on = e && *e == '1'; }
        if (!on) return;
    }
    if (d < 3 || s_far_fill > FAR_FILL_HI) return;
    near = (int)MEM32(VAI(0x4e94b8));
    step = (int8_t)MEM8(VAI(0x4ea4ac));
    nblk = (int16_t)MEM16(VAI(0x8640ca)) + 1;
    if (near <= 0 || s_far_now < 0 || nblk <= 1 || (step != 1 && step != -1)) return;
    rest = nblk - near - s_far_now - 3;           /* the near pass adds up to 2 behind */
    if (rest <= 0) return;
    start = (int)MEM32(VAI(0x4e949c)) + step * (near + s_far_now);
    for (i = 0; i < 23; i++) save[i] = MEM32(VAI(0x4e949c + i * 4));
    MEM32(VAI(0x51c504)) = VAI(0x4ea8c8);
    MEM32(VAI(0x51c508)) = (uint32_t)rest;
    MEM32(VAI(0x51c50c)) = (uint32_t)start;
    MEM32(VAI(0x4e94ac)) = (uint32_t)(uintptr_t)lifted_entry_0047bd70;
    s_rest_mode = 1;
    FUN_0044bd60();
    s_rest_mode = 0;
    for (i = 0; i < 23; i++) MEM32(VAI(0x4e949c + i * 4)) = save[i];
}
/* Objects (grandstands, buildings: blocks 128 and up) come from a list per
 * camera block and direction in the course file (record base + block * 0x30,
 * list offset at +0x20 + direction * 4, from the block file at [0x8641a4]):
 * they joined the list one block at a time as the camera came near, so they
 * popped in.  The second pass gets the lists of the next blocks merged in (a
 * copy; the file's offset points to it during the pass, lifted_scenery_done
 * puts it back). */
static uint32_t s_pvs_slot, s_pvs_orig;
static short s_pvs_buf[1 + 512];
void lifted_pvs_extend(void)
{
    static const int ahead[4] = { 0, 2, 4, 6 }, behind[4] = { 0, 0, 1, 1 };
    int d = drawdist_level(), idx, b0, step, mask, n, k, j, m;
    uint32_t v = MEM32(VAI(0x516d7c)), blk = MEM32(VAI(0x8641a4)), base, slot, orig;
    s_pvs_slot = 0;
    if (!d || !v || !blk) return;
    idx = ((int8_t)MEM8(v + 0x6e)) >> 1;
    b0 = (int16_t)MEM16(v + 0x52);
    step = (int8_t)MEM8(VAI(0x4ea4ac));
    mask = (int16_t)MEM16(VAI(0x8640ca));
    if ((step != 1 && step != -1) || mask <= 0 || idx < 0 || idx > 2) return;
    base = blk + MEM32(blk) + 4;
    slot = MEM32(v + 0x60) + (uint32_t)(idx * 4) + 0x24;
    orig = MEM32(slot);
    if (!orig) return;
    n = (int16_t)MEM16(blk + orig);
    if (n < 0 || n > 256) return;
    for (k = 0; k < n; k++) s_pvs_buf[1 + k] = (short)MEM16(blk + orig + 2 + 2 * k);
    for (j = -behind[d]; j <= ahead[d]; j++) {
        int b = (b0 + step * j) & mask, cnt;
        uint32_t l;
        if (!j) continue;
        l = MEM32(base + (uint32_t)b * 0x30 - 4 + (uint32_t)(idx * 4) + 0x24);
        if (!l) continue;
        cnt = (int16_t)MEM16(blk + l);
        for (k = 0; k < cnt && k < 256 && n < 512; k++) {
            short id = (short)MEM16(blk + l + 2 + 2 * k);
            for (m = 0; m < n && s_pvs_buf[1 + m] != id; m++) ;
            if (m == n) s_pvs_buf[1 + n++] = id;
        }
    }
    s_pvs_buf[0] = (short)n;
    s_pvs_slot = slot; s_pvs_orig = orig;
    MEM32(slot) = (uint32_t)(uintptr_t)s_pvs_buf - blk;
}

/* The vertex transform (0x470089) flags a vertex closer than depth 0x80000
 * whose camera-space x or y is beyond a limit ([0x4e94c4], from the object's
 * block +8), and 0x46d384 then drops the whole polygon (no clipping): big
 * ground polygons beside the camera vanished.  In 4:3 those are mostly off
 * screen; with the wider view the limit grows by the same 233/176. */
void lifted_side_limit(void)
{
    static int lg = -1, n;
    extern int g_opt_widescreen;
    int32_t c4 = (int32_t)MEM32(VAI(0x4e94c4));
    uint32_t cam = MEM32(VAI(0x4e94e4));
    if (lg < 0) lg = getenv("DAYTONA_CLIPLOG") != 0;
    if (lg && n++ % 4000 == 0) rt_log("side limit %d (0x%x) near %d hw %d\n", c4, c4, (int32_t)MEM32(VAI(0x4e94c0)), cam ? (int16_t)MEM16(cam + 0x18) : 0);
    if (g_opt_widescreen && cam && (int16_t)MEM16(cam + 0x18) == 233 && c4 > 0 && c4 < 0x40000000)
    {
        static int k = -1;      /* DAYTONA_SIDEK=percent (testing); screen coords overflow past ~570 */
        if (k < 0) { const char *e = getenv("DAYTONA_SIDEK"); k = e ? atoi(e) : 233 * 100 / 176; }
        MEM32(VAI(0x4e94c4)) = (uint32_t)((int64_t)c4 * k / 100);
    }
}

/* car detail: the AI update gives each car a level (0x4852ba: 4/3/2 by
 * distance, 0 off the visible blocks) that the AI itself also uses (forcing it
 * changed the race), so only the two places that pick the car's model data by
 * level (0x481835, 0x4819b9) see 4 with CAR DETAIL FULL.  That data also feeds
 * the cars' simulation: the AI drifts a little from the original. */
int lifted_car_full(uint32_t level)
{
    extern int g_opt_cardetail;          /* menu.c CAR DETAIL */
    return g_opt_cardetail && (level == 2 || level == 3);
}

void lifted_draw_distance(void)
{
    static const int behind[4] = { 0, 0, 1, 2 };
    int d = drawdist_level();
    int count = (int)MEM32(VAI(0x4e94b8)), step = (int8_t)MEM8(VAI(0x4ea4ac));
    s_blk_start = (int)MEM32(VAI(0x4e949c)); s_blk_count = count;
    if (!behind[d] || count <= 0 || (step != 1 && step != -1)) return;
    MEM32(VAI(0x4e949c)) = MEM32(VAI(0x4e949c)) - (uint32_t)(step * behind[d]);
    MEM32(VAI(0x4e94b8)) = (uint32_t)(count + behind[d]);
}
static unsigned s_corner_and;
void lifted_corner_reset(void) { s_corner_and = 0x1f; }
/* after 0x49a488: [0x4e949c] depth (16.16), [0x4e94a0]/[0x4e94a4] projected
 * x/y, [0x4e94b0] projected radius; view half sizes at 0x56ec18/0x56ec1a */
void lifted_corner_point(void)
{
    int z = (int)MEM32(VAI(0x4e949c)), x = (int)MEM32(VAI(0x4e94a0)), y = (int)MEM32(VAI(0x4e94a4));
    int r = (int)MEM32(VAI(0x4e94b0)), hw = (int16_t)MEM16(VAI(0x56ec18)), hh = (int16_t)MEM16(VAI(0x56ec1a));
    unsigned code;
    static int farz = -1;
    if (farz < 0) { const char *e = getenv("DAYTONA_FARZ"); farz = e ? (int)strtol(e, NULL, 16) : 0x1000000; }
    if (z <= 0 || z > farz) code = 16;
    else code = (x - r > hw ? 1u : 0u) | (x + r < -hw ? 2u : 0u) | (y - r > hh ? 4u : 0u) | (y + r < -hh ? 8u : 0u);
    s_corner_and &= code;
}

void lifted_corner_result(void)
{
    if (s_rest_mode && MEM16(VAI(0x56f000) + 0xa8) > FAR_FILL_HI - 0x200) {
        MEM32(VAI(0x4e94c0)) = 4;          /* the rest pass stops short of the buffer's end */
        return;
    }
    if (drawdist_level() && MEM32(VAI(0x4e94c0)) == 4 && !s_corner_and) {
        MEM32(VAI(0x4e94c0)) = 1;          /* partly visible: drawn with clipping */
        s_corner_fix++;
    }
}
/* DAYTONA_BLKLOG=1: per frame, the scenery block range and the polygon
 * buffers' fill (camera +0xa8 write pointer, +0xaa end) */
static int s_blklog = -1;
volatile int g_scenery_drawn;          /* a course was drawn this frame (dx_ddraw.c) */
void lifted_scenery_done(void)
{
    g_scenery_drawn = 1;
    uint32_t v = MEM32(VAI(0x516d7c)), c1 = VAI(0x56ec00), c2 = VAI(0x56f000);
    s_far_fill = MEM16(c2 + 0xa8);
    {   /* cars whose level goes visible -> 0 -> visible within 3 frames (they
         * blink): logged always, up to 300 lines (DAYTONA_CARFLICK=0 off).
         * level: 4 near, 3 mid, 2 far; 0 = not drawn */
        static int on = -1, lines;
        static unsigned char prev[64], gone[64], before[64];
        if (on < 0) { const char *e = getenv("DAYTONA_CARFLICK"); on = !(e && *e == '0'); }
        if (on && lines < 300) {
            uint32_t car = MEM32(VAI(0x5172cc));
            int n = (int8_t)MEM8(VAI(0x5172b9)), i;
            for (i = 0; i < n && i < 64 && car; i++, car = MEM32(car + 0x82)) {
                int lv = MEM8(car + 0x96);
                if (prev[i] && !lv) { gone[i] = 1; before[i] = prev[i]; }
                else if (gone[i] && lv) {
                    int blk = (int)MEM8((uint32_t)(int16_t)MEM16(car + 0x7e) + MEM32(car + 0x86));
                    lines++;
                    rt_log("carflick f=%u car %d level %d -> 0 for %d frames -> %d | car block %d camera block %d visible %d+%d blocks\n",
                           g_dbg_presents, i, before[i], gone[i], lv, blk, (int16_t)MEM16(v + 0x52), s_blk_count, s_far_count);
                    gone[i] = 0;
                } else if (gone[i] && ++gone[i] > 3) gone[i] = 0;
                prev[i] = (unsigned char)lv;
            }
        }
    }
    if (s_pvs_slot) { MEM32(s_pvs_slot) = s_pvs_orig; s_pvs_slot = 0; }   /* the course file as it was */
    {   /* DAYTONA_CLIPLOG=1: the transform's near limits (viewer +0x20 near z, +0x28 side limit) */
        static int on = -1, n;
        if (on < 0) on = getenv("DAYTONA_CLIPLOG") != 0;
        if (on && n++ % 120 == 0)
            rt_log("clip d4=%08x c0=%08x c4=%08x d4cur=%08x\n", v + 0x20, MEM32(v + 0x20), MEM32(v + 0x28), MEM32(VAI(0x4e94d4)));
    }
    {   /* DAYTONA_PVSLOG=1: the second pass's block list (0x47bfd4) for this view (debug) */
        static int on = -1;
        if (on < 0) on = getenv("DAYTONA_PVSLOG") != 0;
        if (on) {
            int idx = ((int8_t)MEM8(v + 0x6e)) >> 1, n, k;
            uint32_t rec = MEM32(v + 0x60) + (uint32_t)(idx * 4), l = MEM32(rec + 0x24);
            if (l) {
                l += MEM32(VAI(0x8641a4));
                n = (int16_t)MEM16(l);
                {
                    uint32_t blk = MEM32(VAI(0x8641a4)), base = blk + MEM32(blk) + 4;
                    rt_log("pvs v60=%08x rec#=%d.%d ", MEM32(v + 0x60), (int)(MEM32(v + 0x60) - base) / 0x30,
                           (int)(MEM32(v + 0x60) - base) % 0x30);
                }
                rt_log("pvs v52=%d v6e=%d n=%d:", (int16_t)MEM16(v + 0x52), (int8_t)MEM8(v + 0x6e), n);
                for (k = 0; k < n && k < 40; k++) rt_log(" %d", (int16_t)MEM16(l + 2 + 2 * k));
                rt_log("\n");
            }
        }
    }
    if (s_blklog < 0) s_blklog = getenv("DAYTONA_BLKLOG") != 0;
    if (s_blklog)
        rt_log("blk start=%d near=%d far=%d fix=%d v52=%d v68=%d v69=%d v6f=%d nblk=%d step=%d hw=%d/%d | c1 %04x/%04x c2 %04x/%04x\n",
               s_blk_start, s_blk_count, s_far_count, s_corner_fix, (int16_t)MEM16(v + 0x52), (int8_t)MEM8(v + 0x68),
               (int8_t)MEM8(v + 0x69), (int8_t)MEM8(v + 0x6f), (int16_t)MEM16(VAI(0x8640ca)) + 1, (int8_t)MEM8(VAI(0x4ea4ac)),
               (int16_t)MEM16(c1 + 0x18), (int16_t)MEM16(c2 + 0x18), MEM16(c1 + 0xa8), MEM16(c1 + 0xaa), MEM16(c2 + 0xa8), MEM16(c2 + 0xaa));
    s_corner_fix = 0;
}
#define NEDGE 8192
static uint32_t s_edge_from[NEDGE], s_edge_to[NEDGE], s_edge_n[NEDGE];
static void edge_add(uint32_t from, uint32_t to)
{
    uint32_t h = ((from * 2654435761u) ^ (to * 40503u)) & (NEDGE - 1);
    while (s_edge_n[h] && (s_edge_from[h] != from || s_edge_to[h] != to)) h = (h + 1) & (NEDGE - 1);
    s_edge_from[h] = from; s_edge_to[h] = to; s_edge_n[h]++;
}
static int s_lc_from = -1, s_lc_to, s_lc_frame, s_lc_active;
void lifted_count_frame(void)
{
    s_lc_frame++;
    s_lc_active = s_lc && s_lc_frame >= s_lc_from && s_lc_frame < s_lc_to;
    if (s_lc && s_lc_frame == s_lc_to) {
        unsigned int va, n = (unsigned)(s_lc_to - s_lc_from);
        rt_log("lcount: calls per frame over %u frames\n", n);
        for (va = 0; va < 0x3b000; va++)
            if (s_lc[va]) rt_log("lcount %06x %.2f\n", 0x46c000 + va, s_lc[va] / (double)n);
        for (va = 0; va < NEDGE; va++)
            if (s_edge_n[va]) rt_log("ledge %06x %06x %.2f\n", s_edge_from[va], s_edge_to[va], s_edge_n[va] / (double)n);
    }
}

void lifted_probe(uint32_t va)
{
    int i, k;
    if (s_lc_active) {
        if (va >= 0x46c000 && va < 0x4a7000) s_lc[va - 0x46c000]++;
        edge_add(g_lift_depth > 0 && g_lprobe_caller >= 0x46c000 ? g_lprobe_caller : 0, va);
        if (s_lp_n == 0) return;
    }
    if (g_lprobe_on < 0) {
        const char *e = getenv("DAYTONA_LPROBE");
        const char *c = getenv("DAYTONA_LCOUNT");
        if (c) {
            char *end;
            s_lc_from = (int)strtol(c, &end, 0);
            s_lc_to = *end == ':' ? (int)strtol(end + 1, NULL, 0) : s_lc_from + 300;
            s_lc = (unsigned int *)calloc(0x3b000, sizeof *s_lc);
        }
        g_lprobe_on = 0;
        while (e && *e && s_lp_n < 16) {
            char *end;
            s_lp_va[s_lp_n++] = (uint32_t)strtoul(e, &end, 16);
            e = (*end == ',') ? end + 1 : end;
            g_lprobe_on = 1;
        }
        if (s_lc) g_lprobe_on = 1;
        if (!g_lprobe_on) return;
    }
    if (s_lp_n == 0) return;
    for (i = 0; i < s_lp_n; i++) if (s_lp_va[i] == va) break;
    if (i == s_lp_n || s_lp_hits[i] >= 3000) return;
    rt_log("lp %06x #%d eax=%08x edx=%08x", va, ++s_lp_hits[i], g_eax, g_edx);
    for (k = 0; k < 26; k++) {
        uint32_t v = MEM32(VAI(0x4e949c + 4 * k));
        uint32_t m = recomp_host_to_va((const void *)(uintptr_t)v);
        rt_log(" %08x", m ? m : v);
    }
    rt_log("\n");
}
