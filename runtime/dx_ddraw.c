/* dx_ddraw.c — DirectDraw (IDirectDraw/2, IDirectDrawSurface/2/3, palettes,
 * clippers) on CPU surfaces.  The primary surface's back buffer is shown
 * through the GL presenter on Flip (or on a Blt to the primary). */
#include "dx.h"
#include "prof.h"
#include "render.h"
#include "menu.h"

#define STUB_RET(n) { static int once; if (!once) { once = 1; rt_log("DirectDraw: unimplemented %s\n", n); } return DD_OK; }

/* ---- GUIDs --------------------------------------------------------------- */
static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
int guid_is(const void *gv, const char *s)
{
    const unsigned char *g = (const unsigned char *)gv;
    unsigned char b[16];
    int i, k = 0;
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    if (!g) return 0;
    for (i = 0; s[i] && k < 16; ) {
        if (s[i] == '-') { i++; continue; }
        b[order[k++]] = (unsigned char)(hexv(s[i]) * 16 + hexv(s[i + 1]));
        i += 2;
    }
    return host_memcmp(g, b, 16) == 0;
}

static void *com_new(METHOD *vtbl, int kind, unsigned int size)
{
    ComHdr *o = (ComHdr *)host_calloc(1, size);
    o->vtbl = vtbl;
    o->refs = 1;
    o->kind = kind;
    return o;
}

static uint com_addref(ComHdr *o) { return (uint)++o->refs; }

/* ---- surfaces --------------------------------------------------------------- */
extern METHOD vt_surface[];
extern METHOD vt_d3dtexture[];
static DDSurf *s_primary;
static int s_mode_w = 640, s_mode_h = 480, s_mode_bpp = 16;

DDSurf *dd_primary(void) { return s_primary; }

DDSurf *dd_surface_of(void *p)
{
    ComHdr *o = (ComHdr *)p;
    if (!o) return NULL;
    if (o->vtbl == vt_d3dtexture) return ((D3DTexIface *)p)->s;
    if (o->kind == K_SURF) return (DDSurf *)p;
    return NULL;
}

static void set_format(DDSurf *s, int bpp, uint r, uint g, uint b, uint a)
{
    s->bpp = bpp; s->rmask = r; s->gmask = g; s->bmask = b; s->amask = a;
}

static DDSurf *surf_new(int w, int h, int bpp, uint caps)
{
    DDSurf *s = (DDSurf *)com_new(vt_surface, K_SURF, sizeof(DDSurf));
    s->w = w; s->h_ = h; s->caps = caps;
    if (bpp == 16) set_format(s, 16, 0xf800, 0x07e0, 0x001f, 0);
    else if (bpp == 8) set_format(s, 8, 0, 0, 0, 0);
    else set_format(s, bpp, 0xff0000, 0xff00, 0xff, 0);
    s->pitch = ((w * (bpp / 8)) + 3) & ~3;
    s->pixels = (unsigned char *)host_calloc(1, s->pitch * (h ? h : 1) + 16);
    s->texif.vtbl = vt_d3dtexture;
    s->texif.s = s;
    s->dirty = 1;
    return s;
}

static void fill_desc(DDSurf *s, uint *d)
{
    memset(d + 1, 0, 104);
    d[0] = 108;
    d[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;   /* CAPS HEIGHT WIDTH PITCH PIXELFORMAT */
    d[2] = (uint)s->h_;
    d[3] = (uint)s->w;
    d[4] = (uint)s->pitch;
    if (s->back) { d[1] |= 0x20; d[5] = 1; }
    d[9] = (uint)(recomp_uptr)s->pixels;
    d[18] = 32;
    if (s->caps & 0x20000) {           /* Z buffer */
        d[19] = 0x400; d[21] = 16;
        d[1] |= 0x40; d[6] = 16;
    } else if (s->bpp == 8) {
        d[19] = 0x40 | 0x20; d[21] = 8;
    } else {
        d[19] = 0x40 | (s->amask ? 1 : 0);
        d[21] = (uint)s->bpp; d[22] = s->rmask; d[23] = s->gmask; d[24] = s->bmask; d[25] = s->amask;
    }
    if (s->has_ckey) { d[1] |= 0x10000; d[16] = s->ckey_lo; d[17] = s->ckey_hi; }
    d[26] = s->caps;
}

/* IDirectDrawSurface methods */
static uint S_QueryInterface(DDSurf *s, void *iid, void **out)
{
    if (guid_is(iid, "2CDCD9E0-25A0-11CF-A31A-00AA00B93356") || guid_is(iid, "93281502-8cf8-11d0-89ab-00a0c9054129")) {
        *out = &s->texif; s->h.refs++; return DD_OK;      /* IDirect3DTexture/2 */
    }
    if (guid_is(iid, "6C14DB81-A733-11CE-A521-0020AF0BE560") || guid_is(iid, "57805885-6eec-11cf-9441-a82303c10e27") ||
        guid_is(iid, "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB")) {
        *out = s; s->h.refs++; return DD_OK;
    }
    *out = NULL;
    rt_log("Surface::QueryInterface: unknown interface\n");
    return E_NOINTERFACE;
}
static uint S_AddRef(DDSurf *s) { return com_addref(&s->h); }
static void bg_sync(DDSurf *s);
static unsigned int s_rb_level;       /* brightness of the frame read back (fades) */
extern volatile unsigned int g_dbg_presents;
int g_lock_noreadback;                /* set around the fade step (overrides FUN_00437911) */
static int s_fade_level = -1;         /* that step's level 0 (black) .. 255 */
static unsigned int s_fade_at;
void dd_fade_level(int level) { s_fade_level = level; s_fade_at = g_dbg_presents + 1; }   /* the present to come */
extern volatile int g_scenery_drawn;  /* lifted_rt.c */
/* 3D screens without a course (name entry): a copy of the screen every 4th
 * frame.  The game snapshots the back buffer when it starts a transition (the
 * records sliding in after the name entry), sometimes before that frame's 3D
 * is drawn: the 3D then only exists on the GPU and the snapshot came out
 * black.  Such a snapshot takes the 3D from this copy. */
static unsigned short *s_last3d;
static unsigned int s_last3d_at;
static int s_prev_drew;
static void snaplog(const char *what, DDSurf *s, int filled)
{
    static int n;
    if (n < 200) {
        n++;
        rt_log("snap %s %dx%d frame %u 3d=%d prev3d=%d copy_age=%d filled=%d\n", what, s->w, s->h_, g_dbg_presents,
               d3d_drew_this_frame(), s_prev_drew, s_last3d ? (int)(g_dbg_presents - s_last3d_at) : -1, filled);
    }
}

/* rough brightness of a 16-bit surface (every 7th pixel) */
static unsigned int frame_level(DDSurf *s)
{
    unsigned int sum = 0;
    int y, x;
    for (y = 3; y < s->h_; y += 7) {
        const unsigned short *r = (const unsigned short *)(s->pixels + y * s->pitch);
        for (x = 3; x < s->w; x += 7) sum += (r[x] >> 11) + ((r[x] >> 6) & 31) + (r[x] & 31);
    }
    return sum;
}
extern volatile unsigned int g_dbg_presents;
static void readlog(const char *what, DDSurf *s, DDSurf *d);
static uint S_Release(DDSurf *s)
{
    if (--s->h.refs > 0) return (uint)s->h.refs;
    bg_sync(s);
    if (s->tex2d) { rnd_texture_free(s->tex2d); s->tex2d = 0; }
    if (s->tex2dk) { rnd_texture_free(s->tex2dk); s->tex2dk = 0; }
    if (s->tex) { rnd_texture_free(s->tex); s->tex = 0; }      /* D3D texture (and its atlas place) */
    if (s == s_primary) s_primary = NULL;
    host_free(s->pixels);
    s->pixels = NULL;
    /* the object itself is leaked on purpose: stale pointers stay harmless */
    return 0;
}
static uint S_AddAttachedSurface(DDSurf *s, void *a)
{
    DDSurf *t = dd_surface_of(a);
    if (t && (t->caps & 0x20000)) s->zbuf = t; else if (t) s->back = t;
    if (t) t->h.refs++;
    return DD_OK;
}
static uint S_AddOverlayDirtyRect(void) STUB_RET("AddOverlayDirtyRect")

static int clip_rect(int *x0, int *y0, int *x1, int *y1, int w, int h)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > w) *x1 = w;
    if (*y1 > h) *y1 = h;
    return *x0 < *x1 && *y0 < *y1;
}

/* DAYTONA_BLTSTAT=1: per-frame blit/fill counts and pixels (debug) */
static int s_bs_on = -1;
static unsigned int s_bs[8];   /* copies, copy px, keyed, keyed px, scaled, scaled px, fills, fill px */
static void bltstat_frame(void)
{
    static unsigned int frames;
    if (s_bs_on <= 0) return;
    if (++frames % 60 == 0)
        rt_log("bltstat/frame: copy %u (%u px) keyed %u (%u px) scaled %u (%u px) fill %u (%u px)\n",
               s_bs[0] / 60, s_bs[1] / 60, s_bs[2] / 60, s_bs[3] / 60, s_bs[4] / 60, s_bs[5] / 60, s_bs[6] / 60, s_bs[7] / 60);
    if (frames % 60 == 0) memset(s_bs, 0, sizeof s_bs);
}

static void blit(DDSurf *d, int dx0, int dy0, int dx1, int dy1, DDSurf *s, int sx0, int sy0, int sx1, int sy1, int use_ckey, uint klo, uint khi)
{
    int bpp = d->bpp / 8, x, y, xa, xb;
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    unsigned int xstep, ystep;   /* 16.16 source steps (no division per pixel) */
    PROF_PHASE(PH_BLT);
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) { PROF_END(); return; }
    if (s->bpp != d->bpp) { rt_log("Blt: format mismatch %d -> %d\n", s->bpp, d->bpp); PROF_END(); return; }
    xstep = (unsigned int)(((unsigned long long)sw << 16) / (unsigned)dw);
    ystep = (unsigned int)(((unsigned long long)sh << 16) / (unsigned)dh);
    /* destination columns that land inside both surfaces */
    xa = 0; xb = dw;
    if (dx0 + xa < 0) xa = -dx0;
    if (dx0 + xb > d->w) xb = d->w - dx0;
    while (xa < xb && sx0 + (int)(((unsigned long long)xa * xstep) >> 16) < 0) xa++;
    while (xb > xa && sx0 + (int)(((unsigned long long)(xb - 1) * xstep) >> 16) >= s->w) xb--;
    if (xa >= xb) { PROF_END(); return; }
    if (s_bs_on < 0) s_bs_on = getenv_flag("DAYTONA_BLTSTAT");
    if (s_bs_on > 0) {
        int k = sw != dw || sh != dh ? 4 : use_ckey ? 2 : 0;
        s_bs[k]++; s_bs[k + 1] += (unsigned)((xb - xa) * dh);
    }
    for (y = 0; y < dh; y++) {
        int yy = dy0 + y, sy = sy0 + (int)(((unsigned long long)y * ystep) >> 16);
        unsigned char *dr, *sr;
        unsigned int fx;
        if (yy < 0 || yy >= d->h_ || sy < 0 || sy >= s->h_) continue;
        dr = d->pixels + yy * d->pitch;
        sr = s->pixels + sy * s->pitch;
        if (!use_ckey && sw == dw) {
            host_memmove(dr + (dx0 + xa) * bpp, sr + (sx0 + xa) * bpp, (unsigned)((xb - xa) * bpp));
            continue;
        }
        fx = (unsigned int)xa * xstep;
        if (bpp == 2) {
            unsigned short *o = (unsigned short *)dr + dx0;
            const unsigned short *in = (const unsigned short *)sr + sx0;
            if (sw == dw) {
                for (x = xa; x < xb; x++) {
                    uint v = in[x];
                    if (v >= klo && v <= khi) continue;
                    o[x] = (unsigned short)v;
                }
            } else {
                for (x = xa; x < xb; x++, fx += xstep) {
                    uint v = in[fx >> 16];
                    if (use_ckey && v >= klo && v <= khi) continue;
                    o[x] = (unsigned short)v;
                }
            }
        } else if (bpp == 1) {
            for (x = xa; x < xb; x++, fx += xstep) {
                uint v = sr[sx0 + (fx >> 16)];
                if (use_ckey && v >= klo && v <= khi) continue;
                dr[dx0 + x] = (unsigned char)v;
            }
        } else {
            for (x = xa; x < xb; x++, fx += xstep) {
                uint v = ((uint *)sr)[sx0 + (fx >> 16)];
                if (use_ckey && (v & 0xffffff) >= klo && (v & 0xffffff) <= khi) continue;
                ((uint *)dr)[dx0 + x] = v;
            }
        }
    }
    PROF_END();
}

static void fill(DDSurf *d, int x0, int y0, int x1, int y1, uint color)
{
    int x, y, bpp = d->bpp / 8;
    if (!clip_rect(&x0, &y0, &x1, &y1, d->w, d->h_)) return;
    if (s_bs_on > 0) { s_bs[6]++; s_bs[7] += (unsigned)((x1 - x0) * (y1 - y0)); }
    for (y = y0; y < y1; y++) {
        unsigned char *r = d->pixels + y * d->pitch;
        if (bpp == 1) memset(r + x0, (int)color, (unsigned)(x1 - x0));
        else if (bpp == 2) {
            unsigned short *r16 = (unsigned short *)r;
            x = x0;
            if (x < x1 && ((recomp_uptr)(r16 + x) & 3)) r16[x++] = (unsigned short)color;
            {   /* two pixels per store */
                unsigned int *r32 = (unsigned int *)(r16 + x), v = (color & 0xffffu) * 0x10001u;
                int n = (x1 - x) >> 1, i;
                for (i = 0; i < n; i++) r32[i] = v;
                x += n * 2;
            }
            for (; x < x1; x++) r16[x] = (unsigned short)color;
        }
        else for (x = x0; x < x1; x++) ((uint *)r)[x] = color;
    }
}

static void mark(DDSurf *d)
{
    d->dirty = 1;
    d->dirty2d = 1;
    d->dirty2dk = 1;
    d->band_end = 0;
    if (s_primary && (d == s_primary->back || d == s_primary)) d->touched2d = 1;
    d->nrect = -1;             /* written anywhere */
    d->nfill = -1;
}

/* a 2D write to a known area: the overlay upload converts only these areas */
static DDSurf *s_mark_src;          /* source of the blit being marked (HUD placement) */
static void mark_rect(DDSurf *d, int x0, int y0, int x1, int y1)
{
    {   /* DAYTONA_HUDLOG=1: the 2D blits of one frame every 150 (debug) */
        extern volatile unsigned int g_dbg_presents;
        static int lg = -1;
        if (lg < 0) lg = getenv_flag("DAYTONA_HUDLOG");
        if (lg && s_primary && d == s_primary->back && g_dbg_presents % 150 == 0)
            rt_log("hud f=%u (%d,%d,%d,%d) src %dx%d %p\n", g_dbg_presents, x0, y0, x1, y1,
                   s_mark_src ? s_mark_src->w : 0, s_mark_src ? s_mark_src->h_ : 0, (void *)s_mark_src);
    }
    d->dirty = 1;
    d->dirty2d = 1;
    d->dirty2dk = 1;
    d->band_end = 0;
    if (s_primary && (d == s_primary->back || d == s_primary)) d->touched2d = 1;
    if (!clip_rect(&x0, &y0, &x1, &y1, d->w, d->h_)) return;
    if (d->nfill >= 0) {
        if (d->nfill >= DD_MAXRECT) d->nfill = -1;
        else {
            d->fillr[d->nfill][0] = (short)x0; d->fillr[d->nfill][1] = (short)y0;
            d->fillr[d->nfill][2] = (short)x1; d->fillr[d->nfill][3] = (short)y1;
            d->nfill++;
        }
    }
    if (d->nrect < 0) return;
    if (d->nrect >= DD_MAXRECT) { d->nrect = -1; return; }
    d->rect[d->nrect][0] = (short)x0; d->rect[d->nrect][1] = (short)y0;
    d->rect[d->nrect][2] = (short)x1; d->rect[d->nrect][3] = (short)y1;
    d->nrect++;
}

/* ---- background blits through GL ------------------------------------------
 * In a race the game builds the background in the back buffer before the 3D:
 * the sky (a 640x330 copy from a 931x512 panorama) and a colour fill below
 * it.  Copying that with the CPU, uploading the frame and resetting it cost
 * several ms a frame on the Vita.  Plain copies (no colour key, no scaling)
 * and fills into the back buffer before the 3D are queued instead and drawn
 * as GL quads when the 3D starts; the panorama goes to a texture once.  Any
 * other access to the back buffer first performs the queue with the CPU, so
 * the result never depends on the order. */
typedef struct BgOp { DDSurf *src; int sx0, sy0, dx0, dy0, dx1, dy1; uint color; int key; uint klo, khi; } BgOp;
#define MAXBG 16
static BgOp s_bg[MAXBG];
static int s_nbg, s_bg_on = -1;

static int is_back(DDSurf *d) { return s_primary && s_primary->back && d == s_primary->back; }
static void stale_fill(DDSurf *s, const char *what);

static void bg_materialize(void)
{
    int i, n = s_nbg;
    DDSurf *b = s_primary ? s_primary->back : NULL;
    s_nbg = 0;
    if (!b) return;
    for (i = 0; i < n; i++) {
        BgOp *o = &s_bg[i];
        if (!o->src) fill(b, o->dx0, o->dy0, o->dx1, o->dy1, o->color);
        else blit(b, o->dx0, o->dy0, o->dx1, o->dy1, o->src, o->sx0, o->sy0,
                  o->sx0 + (o->dx1 - o->dx0), o->sy0 + (o->dy1 - o->dy0), o->key, o->klo, o->khi);
        mark_rect(b, o->dx0, o->dy0, o->dx1, o->dy1);
    }
}

/* before a surface is read or written outside the queue */
static void bg_sync(DDSurf *s)
{
    int i;
    if (!s_nbg || !s) return;
    if (is_back(s) || s == s_primary) { bg_materialize(); return; }
    for (i = 0; i < s_nbg; i++) if (s_bg[i].src == s) { bg_materialize(); return; }
}

/* queue a background op if possible; 1 = queued */
static int bg_queue_k(DDSurf *d, DDSurf *src, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, uint color,
                     int key, uint klo, uint khi);
static int bg_queue(DDSurf *d, DDSurf *src, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, uint color)
{
    return bg_queue_k(d, src, sx0, sy0, dx0, dy0, dx1, dy1, color, 0, 0, 0);
}
/* plain or colour-keyed 1:1 copies and fills */
static int bg_queue_k(DDSurf *d, DDSurf *src, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, uint color,
                     int key, uint klo, uint khi)
{
    {   /* DAYTONA_BGLOG=1: background copies of one frame every 200 (debug) */
        static int lg = -1;
        if (lg < 0) lg = getenv_flag("DAYTONA_BGLOG");
        if (lg && g_dbg_presents % 200 == 0 && is_back(d))
            rt_log("bg f=%u src %dx%d s(%d,%d) d(%d,%d,%d,%d) col=%04x key=%d open=%d touched=%d nbg=%d\n", g_dbg_presents,
                   src ? src->w : 0, src ? src->h_ : 0, sx0, sy0, dx0, dy0, dx1, dy1, color, key, d3d_frame_is_open(), d->touched2d, s_nbg);
    }
    if (s_bg_on < 0) s_bg_on = !getenv_flag("DAYTONA_NOBGGL");
    if (!s_bg_on || !is_back(d) || d->bpp != 16 || d3d_frame_is_open()) return 0;
    if (d->touched2d && !s_nbg) return 0;       /* CPU 2D already in this frame */
    if (s_nbg >= MAXBG || (src && (src == d || src->bpp != 16 || !src->pixels))) { bg_materialize(); return 0; }
    if (dx1 <= dx0 || dy1 <= dy0) return 1;
    s_bg[s_nbg].src = src; s_bg[s_nbg].sx0 = sx0; s_bg[s_nbg].sy0 = sy0;
    s_bg[s_nbg].dx0 = dx0; s_bg[s_nbg].dy0 = dy0; s_bg[s_nbg].dx1 = dx1; s_bg[s_nbg].dy1 = dy1;
    s_bg[s_nbg].color = color;
    s_bg[s_nbg].key = key; s_bg[s_nbg].klo = klo; s_bg[s_nbg].khi = khi;
    s_nbg++;
    return 1;
}

static unsigned int rgb565_to_argb(uint c)
{
    unsigned int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

/* one background copy at rows [dy0, dy1) taking source rows from sy
 * (upwards when vmir), with the widescreen extensions: a copy reaching a 4:3
 * edge carries on past it from its source, mirrored past either end of the
 * panorama (it is not seamless: a cut cloud at x 0, hills at the end) */
static void bg_piece(const BgOp *o, DDSurf *s, unsigned int tex, int bw, int dy0, int dy1, int sy, int vmir)
{
    int ext = (int)(rnd_wide_ext() + 0.99f), k = o->key;
    rnd_blit2d_ex(tex, s->w, s->h_, o->sx0, sy, o->dx0, dy0, o->dx1, dy1, k, 0, vmir);
    if (ext <= 0) return;
    if (o->dx0 <= 0) {                              /* left: source [sx0-ext, sx0) */
        int a0 = o->sx0 - ext;
        if (a0 >= 0) {
            rnd_blit2d_ex(tex, s->w, s->h_, a0, sy, o->dx0 - ext, dy0, o->dx0, dy1, k, 0, vmir);
        } else {
            rnd_blit2d_ex(tex, s->w, s->h_, -a0, sy, o->dx0 - ext, dy0, o->dx0 - o->sx0, dy1, k, 1, vmir);
            if (o->sx0 > 0) rnd_blit2d_ex(tex, s->w, s->h_, 0, sy, o->dx0 - o->sx0, dy0, o->dx0, dy1, k, 0, vmir);
        }
    }
    if (o->dx1 >= bw) {                             /* right: source from where it ended */
        int b0 = o->sx0 + (o->dx1 - o->dx0), n = ext;
        if (b0 >= s->w) n = 0;
        else if (b0 + n > s->w) n = s->w - b0;
        if (n > 0) rnd_blit2d_ex(tex, s->w, s->h_, b0, sy, o->dx1, dy0, o->dx1 + n, dy1, k, 0, vmir);
        if (n < ext) rnd_blit2d_ex(tex, s->w, s->h_, s->w, sy, o->dx1 + n, dy0, o->dx1 + ext, dy1, k, 1, vmir);
    }
}

static unsigned int bg_texture(DDSurf *s, const BgOp *o)
{
    if (o->key) {
        if (!s->tex2dk || s->dirty2dk || s->tex2dk_lo != o->klo || s->tex2dk_hi != o->khi) {
            if (!s->tex2dk) s->tex2dk = rnd_texture_new();
            rnd_texture_upload16_key(s->tex2dk, s->w, s->h_, (const unsigned short *)s->pixels, s->pitch / 2, o->klo, o->khi);
            s->dirty2dk = 0; s->tex2dk_lo = o->klo; s->tex2dk_hi = o->khi;
        }
        return s->tex2dk;
    }
    if (!s->tex2d || s->dirty2d) {
        if (!s->tex2d) s->tex2d = rnd_texture_new();
        rnd_texture_upload16(s->tex2d, s->w, s->h_, (const unsigned short *)s->pixels, s->pitch / 2);
        s->dirty2d = 0;
    }
    return s->tex2d;
}

/* rows at the bottom of a panorama that form one band (the sea), from the
 * bottom up until a row's average colour jumps (the horizon): what is
 * repeated under the panorama.  Kept in the surface until its pixels change:
 * a course draws two panoramas a frame (sky, keyed mountains) and scanning
 * one every frame cost the Vita ~8 FPS. */
static int pano_bottom_band(DDSurf *s, int sy_end)
{
    int y, lr = -1, lg = 0, lb = 0, band = 0, step, key = sy_end;
    if (key == s->band_end) return s->band_val;
    { static int scans; if (++scans <= 8) rt_log("bg: sea band scan of a %dx%d panorama\n", s->w, s->h_); }
    if (sy_end > s->h_) sy_end = s->h_;
    for (y = sy_end - 1; y >= 0 && band < sy_end; y--) {
        const unsigned short *row = (const unsigned short *)(s->pixels + y * s->pitch);
        int x, n = 0, r = 0, g = 0, b = 0;
        step = s->w / 64 > 0 ? s->w / 64 : 1;
        for (x = 0; x < s->w; x += step, n++) {
            unsigned int p = row[x];
            r += (p >> 11) << 3; g += ((p >> 5) & 63) << 2; b += (p & 31) << 3;
        }
        r /= n; g /= n; b /= n;
        if (lr >= 0 && (r - lr) * (r - lr) + (g - lg) * (g - lg) + (b - lb) * (b - lb) > 24 * 24) break;
        lr = r; lg = g; lb = b;
        band++;
    }
    band -= 6;                             /* not the rows blending into the horizon */
    if (band < 8) band = 8;
    s->band_end = key; s->band_val = band;
    return band;
}

int dd_bg_draw_gl(DDSurf *back)
{
    int i, n = s_nbg, bottom = 0, have_pano = 0, copy_bottom = 0;
    if (!n) return 0;
    if (back->touched2d) { bg_materialize(); return 0; }
    s_nbg = 0;
    for (i = 0; i < n; i++)
        if (s_bg[i].src && s_bg[i].src->w > back->w && s_bg[i].dy1 > copy_bottom) copy_bottom = s_bg[i].dy1;
    for (i = 0; i < n; i++) {
        BgOp *o = &s_bg[i];
        if (!o->src) {
            /* a black fill right under the panorama (Sea-Side Street Galaxy:
             * the sea ends there; the road hides it in a race, the high replay
             * cameras show it): treated as nothing, the panorama goes on
             * mirrored below.  Coloured fills (Silver Ocean's sea) stay. */
            if (copy_bottom && o->color == 0 && o->dy0 >= copy_bottom - 1 && o->dy0 <= copy_bottom + 1 &&
                o->dx0 <= 0 && o->dx1 >= back->w) {
                static int logged;
                if (!logged) { logged = 1; rt_log("bg: black fill under the sky (y %d) -> the sea goes on\n", o->dy0); }
                continue;
            }
            if (o->dy1 > bottom) bottom = o->dy1;
            rnd_fill2d(o->dx0, o->dy0, o->dx1, o->dy1, rgb565_to_argb(o->color));
        } else {
            DDSurf *s = o->src;
            if (o->dy1 > bottom) bottom = o->dy1;
            if (s->w > back->w) have_pano = 1;
            bg_piece(o, s, bg_texture(s, o), back->w, o->dy0, o->dy1, o->sy0, 0);
        }
    }
    if (have_pano && bottom < back->h_) {
        /* nothing below the background (Sea-Side: under the bridge, the sea
         * ends): the bottom band of the lowest panorama copies (the sea, not
         * the sky above it) goes on downwards, mirrored back and forth so it
         * joins without a seam */
        for (i = 0; i < n; i++) {
            BgOp *o = &s_bg[i];
            int sb, band, y, k;
            unsigned int tex;
            if (!o->src || o->src->w <= back->w || o->dy1 != bottom) continue;
            sb = o->sy0 + (o->dy1 - o->dy0);             /* source row after the copy */
            band = pano_bottom_band(o->src, sb);
            if (band > o->dy1 - o->dy0) band = o->dy1 - o->dy0;
            tex = bg_texture(o->src, o);
            for (y = bottom, k = 0; y < back->h_; y += band, k++) {
                int t = back->h_ - y < band ? back->h_ - y : band;
                if (k & 1) bg_piece(o, o->src, tex, back->w, y, y + t, sb - band, 0);
                else bg_piece(o, o->src, tex, back->w, y, y + t, sb, 1);
            }
        }
    }
    return 1;
}

/* the back buffer holds no 2D pixels any more (frame begin / present) */
void dd_clear_rects(DDSurf *d) { d->touched2d = 0; d->nrect = 0; }

static uint S_Blt(DDSurf *d, int *drect, void *srcv, int *srect, uint flags, uint *fx)
{
    DDSurf *s = dd_surface_of(srcv);
    int dx0 = 0, dy0 = 0, dx1 = d->w, dy1 = d->h_;
    if (drect) { dx0 = drect[0]; dy0 = drect[1]; dx1 = drect[2]; dy1 = drect[3]; }
    if ((flags & 0x400) && bg_queue(d, NULL, 0, 0, dx0, dy0, dx1, dy1, fx ? fx[20] : 0)) {
        /* queued: drawn with GL when the 3D starts */
    } else if (flags & 0x400) {                /* DDBLT_COLORFILL */
        bg_sync(d);
        fill(d, dx0, dy0, dx1, dy1, fx ? fx[20] : 0);
        mark_rect(d, dx0, dy0, dx1, dy1);
    } else if (flags & 0x2000000) {            /* DDBLT_DEPTHFILL */
        if (d->caps & 0x20000) fill(d, dx0, dy0, dx1, dy1, 0xffff);
        if (s_primary && (d == s_primary->back || d == s_primary)) d3d_frame_begin_if_needed();
    } else if (s) {
        int sx0 = 0, sy0 = 0, sx1 = s->w, sy1 = s->h_;
        uint klo = s->ckey_lo, khi = s->ckey_hi;
        int key = (flags & 0x8000) && s->has_ckey;
        if (srect) { sx0 = srect[0]; sy0 = srect[1]; sx1 = srect[2]; sy1 = srect[3]; }
        if ((flags & 0x10000) && fx) { key = 1; klo = fx[23]; khi = fx[24]; }
        readlog("Blt", s, d);
        stale_fill(s, "Blt");
        {   /* DAYTONA_BGLOG: copies from a panorama into other surfaces */
            static int lg = -1, n;
            if (lg < 0) lg = getenv_flag("DAYTONA_BGLOG");
            if (lg && n < 200 && s->w > 640 && !is_back(d)) { n++;
                rt_log("pano f=%u -> %dx%d s(%d,%d,%d,%d) d(%d,%d,%d,%d) key=%d\n", g_dbg_presents, d->w, d->h_, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, key); }
        }
        {   /* DAYTONA_BLTLOG=1: log blits into the back buffer (debug) */
            static int lg = -1, n;
            if (lg < 0) lg = getenv_flag("DAYTONA_BLTLOG");
            if (lg && n < 3000 && s_primary && d == s_primary->back && (n++ % 7) == 0)
                rt_log("Blt %dx%d (%d,%d,%d,%d) <- %dx%d (%d,%d,%d,%d) fl=%x\n", d->w, d->h_, dx0, dy0, dx1, dy1,
                       s->w, s->h_, sx0, sy0, sx1, sy1, flags);
        }
        if (sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 &&
            bg_queue_k(d, s, sx0, sy0, dx0, dy0, dx1, dy1, 0, key, klo, khi))
            goto done;
        bg_sync(d);
        bg_sync(s);
        blit(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, key, klo, khi);
        s_mark_src = s;
        mark_rect(d, dx0, dy0, dx1, dy1);
        s_mark_src = NULL;
    }
done:
    if (d == s_primary) dd_present();
    return DD_OK;
}
static uint S_BltBatch(void) STUB_RET("BltBatch")
static uint S_BltFast(DDSurf *d, int x, int y, void *srcv, int *srect, uint flags)
{
    DDSurf *s = dd_surface_of(srcv);
    int sx0 = 0, sy0 = 0, sx1, sy1;
    if (!s) return DDERR_INVALIDPARAMS;
    sx1 = s->w; sy1 = s->h_;
    if (srect) { sx0 = srect[0]; sy0 = srect[1]; sx1 = srect[2]; sy1 = srect[3]; }
    readlog("BltFast", s, d);
    stale_fill(s, "BltFast");
    {
        static int lg = -1, n;
        if (lg < 0) lg = getenv_flag("DAYTONA_BGLOG");
        if (lg && n < 200 && s->w > 640 && !is_back(d)) { n++;
            rt_log("pano fast f=%u -> %dx%d s(%d,%d,%d,%d) at %d,%d\n", g_dbg_presents, d->w, d->h_, sx0, sy0, sx1, sy1, x, y); }
    }
    {
        static int lg = -1, n;
        if (lg < 0) lg = getenv_flag("DAYTONA_BLTLOG");
        if (lg && n < 3000 && s_primary && d == s_primary->back && s->w == 931 && ++n)
            rt_log("BltFast %dx%d at %d,%d <- %dx%d (%d,%d,%d,%d) fl=%x\n", d->w, d->h_, x, y,
                   s->w, s->h_, sx0, sy0, sx1, sy1, flags);
    }
    if (bg_queue_k(d, s, sx0, sy0, x, y, x + (sx1 - sx0), y + (sy1 - sy0), 0,
                   (flags & 1) && s->has_ckey, s->ckey_lo, s->ckey_hi)) {
        if (d == s_primary) dd_present();
        return DD_OK;
    }
    bg_sync(d);
    bg_sync(s);
    blit(d, x, y, x + (sx1 - sx0), y + (sy1 - sy0), s, sx0, sy0, sx1, sy1,
         (flags & 1) && s->has_ckey, s->ckey_lo, s->ckey_hi);
    s_mark_src = s;
    mark_rect(d, x, y, x + (sx1 - sx0), y + (sy1 - sy0));
    s_mark_src = NULL;
    if (d == s_primary) dd_present();
    return DD_OK;
}
static uint S_DeleteAttachedSurface(DDSurf *s, uint f, void *a)
{
    DDSurf *t = dd_surface_of(a);
    (void)f;
    if (t == s->zbuf) s->zbuf = NULL;
    if (t == s->back) s->back = NULL;
    return DD_OK;
}
static uint S_EnumAttachedSurfaces(DDSurf *s, void *ctx, int (*cb)())
{
    uint desc[27];
    if (s->back) { s->back->h.refs++; fill_desc(s->back, desc); cb(s->back, desc, ctx); }
    if (s->zbuf) { s->zbuf->h.refs++; fill_desc(s->zbuf, desc); cb(s->zbuf, desc, ctx); }
    return DD_OK;
}
static uint S_EnumOverlayZOrders(void) STUB_RET("EnumOverlayZOrders")
static uint S_Flip(DDSurf *s, void *target, uint flags)
{
    (void)target; (void)flags;
    if (s == s_primary) dd_present();
    return DD_OK;
}
static uint S_GetAttachedSurface(DDSurf *s, uint *caps, void **out)
{
    DDSurf *t = NULL;
    if (caps && (caps[0] & 0x20000)) t = s->zbuf;
    else if (caps && (caps[0] & 0x400000)) t = s->back;
    else t = s->back;
    if (!t) { *out = NULL; rt_log("GetAttachedSurface(caps 0x%x): none\n", caps ? caps[0] : 0); return DDERR_NOTFOUND; }
    t->h.refs++;
    *out = t;
    return DD_OK;
}
static uint S_GetBltStatus(void) { return DD_OK; }
static uint S_GetCaps(DDSurf *s, uint *caps) { caps[0] = s->caps; return DD_OK; }
static uint S_GetClipper(DDSurf *s, void **out) { (void)s; *out = NULL; return 0x887600cdu; }
static uint S_GetColorKey(DDSurf *s, uint f, uint *k) { (void)f; if (!s->has_ckey) return 0x887600d7u /* DDERR_NOCOLORKEY */; k[0] = s->ckey_lo; k[1] = s->ckey_hi; return DD_OK; }
static uint S_GetDC(DDSurf *s, HDC *dc) { (void)s; *dc = (HDC)(recomp_uptr)0x40000; return DD_OK; }
static uint S_GetFlipStatus(void) { return DD_OK; }
static uint S_GetOverlayPosition(void) STUB_RET("GetOverlayPosition")
static uint S_GetPalette(DDSurf *s, void **out) { if (!s->pal) { *out = NULL; return 0x887601a7u; } s->pal->h.refs++; *out = s->pal; return DD_OK; }
static uint S_GetPixelFormat(DDSurf *s, uint *pf)
{
    uint d[27];
    fill_desc(s, d);
    memcpy(pf, d + 18, 32);
    return DD_OK;
}
static uint S_GetSurfaceDesc(DDSurf *s, uint *d) { fill_desc(s, d); return DD_OK; }
static uint S_Initialize(void) { return DD_OK; }
static uint S_IsLost(DDSurf *s) { (void)s; return DD_OK; }
/* DAYTONA_READLOG=1: log reads of the screen surfaces (debug) */
static void readlog(const char *what, DDSurf *s, DDSurf *d)
{
    static int on = -1, n;
    if (on < 0) on = getenv_flag("DAYTONA_READLOG");
    if (on && n < 400 && s_primary && (s == s_primary || s == s_primary->back) && (!getenv("DAYTONA_READLOG3D") || d3d_frame_is_open())) {
        n++;
        rt_log("read %s %s -> %dx%d frame %u (3d=%d)\n", what, s == s_primary ? "primary" : "back", d ? d->w : 0, d ? d->h_ : 0,
               g_dbg_presents, d3d_frame_is_open());
    }
}

static void stale_fill(DDSurf *s, const char *what)
{
    if (is_back(s) && s->bpp == 16 && !g_lock_noreadback && !d3d_drew_this_frame() && s_prev_drew) {
        /* a read before this frame's 3D, right after 3D frames (transitions) */
        int filled = 0;
        if (s_last3d && g_dbg_presents - s_last3d_at <= 8 && s->w <= 640 && s->h_ <= 480) {
            int x, y;
            for (y = 0; y < s->h_; y++) {
                unsigned short *r = (unsigned short *)(s->pixels + y * s->pitch);
                const unsigned short *c = s_last3d + y * 640;
                for (x = 0; x < s->w; x++) if (r[x] == RND_OVERLAY_EMPTY) r[x] = c[x];
            }
            mark(s);
            filled = 1;
        }
        snaplog(what, s, filled);
    }
}
static uint S_Lock(DDSurf *s, int *rect, uint *d, uint flags, HANDLE ev)
{
    (void)flags; (void)ev;
    if (!s->pixels) return DDERR_SURFACELOST;
    readlog("Lock", s, NULL);
    bg_sync(s);
    /* the game reads the back buffer during a 3D frame (fade to black, the
     * wipe from the name entry to the records): give it the 3D too */
    stale_fill(s, "Lock");
    if (is_back(s) && s->bpp == 16 && !g_lock_noreadback && d3d_frame_is_open() && d3d_drew_this_frame() && s->readback_frame != g_dbg_presents + 1) {
        s->readback_frame = g_dbg_presents + 1;      /* once a frame */
        if (rnd_read_frame565((unsigned short *)s->pixels, s->w, s->h_, s->pitch / 2, RND_OVERLAY_EMPTY)) {
            mark(s);
            s_rb_level = frame_level(s);
        }
    }
    fill_desc(s, d);
    if (rect) d[9] = (uint)(recomp_uptr)(s->pixels + rect[1] * s->pitch + rect[0] * (s->bpp / 8));
    s->locked = 1;
    return DD_OK;
}
static uint S_ReleaseDC(void) { return DD_OK; }
static uint S_Restore(DDSurf *s) { s->lost = 0; return DD_OK; }
static uint S_SetClipper(void) { return DD_OK; }
static uint S_SetColorKey(DDSurf *s, uint flags, uint *k)
{
    if (!k) { s->has_ckey = 0; return DD_OK; }
    if (getenv_flag("DAYTONA_CKEYLOG"))
        rt_log("SetColorKey %dx%d fl=%x key=%04x..%04x px0=%04x\n", s->w, s->h_, flags, k[0], k[1],
               s->pixels && s->bpp == 16 ? *(unsigned short *)s->pixels : 0xffff);
    if (flags & 8) {               /* DDCKEY_SRCBLT */
        s->has_ckey = 1;
        s->ckey_lo = k[0];
        s->ckey_hi = k[1] < k[0] ? k[0] : k[1];
        s->dirty = 1;
    }
    return DD_OK;
}
static uint S_SetOverlayPosition(void) STUB_RET("SetOverlayPosition")
static uint S_SetPalette(DDSurf *s, void *p) { s->pal = (DDPal *)p; if (s->pal) s->pal->h.refs++; s->dirty = 1; return DD_OK; }
static uint S_Unlock(DDSurf *s, void *p) { (void)p; s->locked = 0; mark(s); return DD_OK; }
static uint S_UpdateOverlay(void) STUB_RET("UpdateOverlay")
static uint S_UpdateOverlayDisplay(void) STUB_RET("UpdateOverlayDisplay")
static uint S_UpdateOverlayZOrder(void) STUB_RET("UpdateOverlayZOrder")
static uint S_GetDDInterface(DDSurf *s, void **out) { (void)s; *out = NULL; return DDERR_UNSUPPORTED; }
static uint S_PageLock(void) { return DD_OK; }
static uint S_PageUnlock(void) { return DD_OK; }
static uint S_SetSurfaceDesc(void) STUB_RET("SetSurfaceDesc")

METHOD vt_surface[] = {
    (METHOD)S_QueryInterface, (METHOD)S_AddRef, (METHOD)S_Release, (METHOD)S_AddAttachedSurface,
    (METHOD)S_AddOverlayDirtyRect, (METHOD)S_Blt, (METHOD)S_BltBatch, (METHOD)S_BltFast,
    (METHOD)S_DeleteAttachedSurface, (METHOD)S_EnumAttachedSurfaces, (METHOD)S_EnumOverlayZOrders, (METHOD)S_Flip,
    (METHOD)S_GetAttachedSurface, (METHOD)S_GetBltStatus, (METHOD)S_GetCaps, (METHOD)S_GetClipper,
    (METHOD)S_GetColorKey, (METHOD)S_GetDC, (METHOD)S_GetFlipStatus, (METHOD)S_GetOverlayPosition,
    (METHOD)S_GetPalette, (METHOD)S_GetPixelFormat, (METHOD)S_GetSurfaceDesc, (METHOD)S_Initialize,
    (METHOD)S_IsLost, (METHOD)S_Lock, (METHOD)S_ReleaseDC, (METHOD)S_Restore,
    (METHOD)S_SetClipper, (METHOD)S_SetColorKey, (METHOD)S_SetOverlayPosition, (METHOD)S_SetPalette,
    (METHOD)S_Unlock, (METHOD)S_UpdateOverlay, (METHOD)S_UpdateOverlayDisplay, (METHOD)S_UpdateOverlayZOrder,
    (METHOD)S_GetDDInterface, (METHOD)S_PageLock, (METHOD)S_PageUnlock, (METHOD)S_SetSurfaceDesc,
};

/* ---- palette / clipper --------------------------------------------------- */
extern METHOD vt_palette[], vt_clipper[];

static void pal_update565(DDPal *p, uint first, uint n)
{
    uint i;
    for (i = first; i < first + n && i < 256; i++) {
        uint e = p->pal[i];   /* PALETTEENTRY: r, g, b, flags */
        uint r = e & 0xff, g = (e >> 8) & 0xff, b = (e >> 16) & 0xff;
        p->pal565[i] = (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}
static uint P_QueryInterface(DDPal *p, void *iid, void **out) { (void)iid; *out = p; p->h.refs++; return DD_OK; }
static uint P_AddRef(DDPal *p) { return com_addref(&p->h); }
static uint P_Release(DDPal *p) { return (uint)(--p->h.refs > 0 ? p->h.refs : 0); }
static uint P_GetCaps(DDPal *p, uint *c) { *c = p->caps; return DD_OK; }
static uint P_GetEntries(DDPal *p, uint f, uint first, uint n, uint *e) { (void)f; memcpy(e, p->pal + first, n * 4); return DD_OK; }
static uint P_Initialize(void) { return DD_OK; }
static uint P_SetEntries(DDPal *p, uint f, uint first, uint n, uint *e)
{
    (void)f;
    if (first + n > 256) n = 256 - first;
    memcpy(p->pal + first, e, n * 4);
    pal_update565(p, first, n);
    if (s_primary && s_primary->pal == p) { s_primary->dirty = 1; if (s_primary->back) s_primary->back->dirty = 1; }
    return DD_OK;
}
METHOD vt_palette[] = { (METHOD)P_QueryInterface, (METHOD)P_AddRef, (METHOD)P_Release, (METHOD)P_GetCaps,
                        (METHOD)P_GetEntries, (METHOD)P_Initialize, (METHOD)P_SetEntries };

static uint C_QueryInterface(ComHdr *c, void *iid, void **out) { (void)iid; *out = c; c->refs++; return DD_OK; }
static uint C_AddRef(ComHdr *c) { return com_addref(c); }
static uint C_Release(ComHdr *c) { return (uint)(--c->refs > 0 ? c->refs : 0); }
static uint C_GetClipList(void) { return DDERR_UNSUPPORTED; }
static uint C_GetHWnd(ComHdr *c, HWND *h) { (void)c; *h = wnd_main(); return DD_OK; }
static uint C_Ok(void) { return DD_OK; }
METHOD vt_clipper[] = { (METHOD)C_QueryInterface, (METHOD)C_AddRef, (METHOD)C_Release, (METHOD)C_GetClipList,
                        (METHOD)C_GetHWnd, (METHOD)C_Ok, (METHOD)C_Ok, (METHOD)C_Ok, (METHOD)C_Ok };

/* ---- IDirectDraw / IDirectDraw2 ----------------------------------------- */
typedef struct DDraw { ComHdr h; } DDraw;
extern METHOD vt_ddraw[];
void *d3d_create(void);

static uint D_QueryInterface(DDraw *d, void *iid, void **out)
{
    if (guid_is(iid, "6C14DB80-A733-11CE-A521-0020AF0BE560") || guid_is(iid, "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356")) {
        *out = d; d->h.refs++; return DD_OK;
    }
    if (guid_is(iid, "3BBA0080-2421-11CF-A31A-00AA00B93356") || guid_is(iid, "6aae1ec1-662a-11d0-889d-00aa00bbb76a")) {
        *out = d3d_create(); return DD_OK;
    }
    *out = NULL;
    rt_log("DirectDraw::QueryInterface: unknown interface\n");
    return E_NOINTERFACE;
}
static uint D_AddRef(DDraw *d) { return com_addref(&d->h); }
static uint D_Release(DDraw *d) { return (uint)(--d->h.refs > 0 ? d->h.refs : 0); }
static uint D_Compact(void) { return DD_OK; }
static uint D_CreateClipper(DDraw *d, uint f, void **out, void *o) { (void)d; (void)f; (void)o; *out = com_new(vt_clipper, K_CLIP, sizeof(ComHdr)); return DD_OK; }
static uint D_CreatePalette(DDraw *d, uint flags, uint *entries, void **out, void *o)
{
    DDPal *p = (DDPal *)com_new(vt_palette, K_PAL, sizeof(DDPal));
    (void)d; (void)o;
    p->caps = flags;
    if (entries) { memcpy(p->pal, entries, 256 * 4); pal_update565(p, 0, 256); }
    *out = p;
    return DD_OK;
}
static uint D_CreateSurface(DDraw *d, uint *desc, void **out, void *o)
{
    uint flags = desc[1], caps = (flags & 1) ? desc[26] : 0;
    int w = (flags & 4) ? (int)desc[3] : s_mode_w, h = (flags & 2) ? (int)desc[2] : s_mode_h;
    int bpp = s_mode_bpp;
    DDSurf *s;
    (void)d; (void)o;
    if (flags & 0x1000) {
        uint pff = desc[19];
        bpp = (pff & 0x400) ? 16 : (int)desc[21];
        if (pff & 0x20) bpp = 8;
    }
    if (caps & 0x20000) bpp = 16;      /* Z buffer */
    if (caps & 0x200) { w = s_mode_w; h = s_mode_h; }
    s = surf_new(w, h, bpp, caps);
    if ((flags & 0x1000) && bpp == 16 && !(caps & 0x20000)) {
        s->rmask = desc[22]; s->gmask = desc[23]; s->bmask = desc[24]; s->amask = desc[25];
        if (!s->rmask) { s->rmask = 0xf800; s->gmask = 0x7e0; s->bmask = 0x1f; }
    }
    if (flags & 0x10000) { s->has_ckey = 1; s->ckey_lo = desc[16]; s->ckey_hi = desc[17]; }
    if (caps & 0x200) {
        s_primary = s;
        if ((flags & 0x20) && desc[5] >= 1) {
            s->back = surf_new(w, h, bpp, 0x4 | 0x2000 | (caps & 0x2000));
        }
    }
    rt_log("CreateSurface %dx%dx%d caps 0x%x%s\n", w, h, bpp, caps, (caps & 0x200) ? " (primary)" : "");
    *out = s;
    return DD_OK;
}
static uint D_DuplicateSurface(void) STUB_RET("DuplicateSurface")
static uint D_EnumDisplayModes(DDraw *d, uint flags, uint *want, void *ctx, int (*cb)())
{
    static const int modes[][3] = { { 320, 240, 8 }, { 320, 240, 16 }, { 640, 480, 8 }, { 640, 480, 16 } };
    uint desc[27];
    int i;
    (void)d; (void)flags; (void)want;
    for (i = 0; i < 4; i++) {
        DDSurf tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.w = modes[i][0]; tmp.h_ = modes[i][1]; tmp.bpp = modes[i][2];
        tmp.pitch = tmp.w * tmp.bpp / 8;
        if (tmp.bpp == 16) { tmp.rmask = 0xf800; tmp.gmask = 0x7e0; tmp.bmask = 0x1f; }
        fill_desc(&tmp, desc);
        desc[9] = 0;
        if (!cb(desc, ctx)) break;
    }
    return DD_OK;
}
static uint D_EnumSurfaces(void) STUB_RET("EnumSurfaces")
static uint D_FlipToGDISurface(void) { return DD_OK; }
static uint D_GetCaps(DDraw *d, uint *hal, uint *hel)
{
    (void)d;
    if (hal) {
        uint n = hal[0] ? hal[0] : 316;
        memset(hal + 1, 0, n - 4);
        hal[1] = 0x1 | 0x40 | 0x400 | 0x40000 | 0x4000000 | 0x80000000u;  /* 3D BLT BLTSTRETCH COLORKEY BLTCOLORFILL ... */
        hal[2] = 0x1 | 0x200;   /* dwCaps2 */
        hal[3] = 0x1;           /* CKey caps: DDCKEYCAPS_DESTBLT.. */
        hal[13] = 0x2000 | 0x1000 | 0x200 | 0x10 | 0x8 | 0x4 | 0x4000;   /* approximate */
        hal[15] = 8 * 1024 * 1024; hal[16] = 8 * 1024 * 1024;           /* vid mem */
    }
    if (hel) { uint n = hel[0] ? hel[0] : 316; memset(hel + 1, 0, n - 4); hel[1] = 0x40 | 0x40000; }
    return DD_OK;
}
static uint D_GetDisplayMode(DDraw *d, uint *desc)
{
    DDSurf tmp;
    (void)d;
    memset(&tmp, 0, sizeof tmp);
    tmp.w = s_mode_w; tmp.h_ = s_mode_h; tmp.bpp = s_mode_bpp; tmp.pitch = tmp.w * tmp.bpp / 8;
    tmp.rmask = 0xf800; tmp.gmask = 0x7e0; tmp.bmask = 0x1f;
    fill_desc(&tmp, desc);
    return DD_OK;
}
static uint D_GetFourCCCodes(DDraw *d, uint *n, uint *c) { (void)d; (void)c; *n = 0; return DD_OK; }
static uint D_GetGDISurface(void) STUB_RET("GetGDISurface")
static uint D_GetMonitorFrequency(DDraw *d, uint *f) { (void)d; *f = 60; return DD_OK; }
static uint D_GetScanLine(DDraw *d, uint *l) { (void)d; *l = 0; return DD_OK; }
static uint D_GetVerticalBlankStatus(DDraw *d, int *b) { (void)d; *b = 1; return DD_OK; }
static uint D_Initialize(void) { return DD_OK; }
static uint D_RestoreDisplayMode(void) { return DD_OK; }
static uint D_SetCooperativeLevel(void) { return DD_OK; }
static uint D_SetDisplayMode(DDraw *d, uint w, uint h, uint bpp)
{
    (void)d;
    rt_log("SetDisplayMode %ux%ux%u\n", w, h, bpp);
    s_mode_w = (int)w; s_mode_h = (int)h; s_mode_bpp = (int)bpp;
    return DD_OK;
}
static uint D_WaitForVerticalBlank(void) { return DD_OK; }
static uint D_GetAvailableVidMem(DDraw *d, uint *caps, uint *total, uint *freem)
{
    (void)d; (void)caps;
    if (total) *total = 8 * 1024 * 1024;
    if (freem) *freem = 6 * 1024 * 1024;
    return DD_OK;
}

METHOD vt_ddraw[] = {
    (METHOD)D_QueryInterface, (METHOD)D_AddRef, (METHOD)D_Release, (METHOD)D_Compact,
    (METHOD)D_CreateClipper, (METHOD)D_CreatePalette, (METHOD)D_CreateSurface, (METHOD)D_DuplicateSurface,
    (METHOD)D_EnumDisplayModes, (METHOD)D_EnumSurfaces, (METHOD)D_FlipToGDISurface, (METHOD)D_GetCaps,
    (METHOD)D_GetDisplayMode, (METHOD)D_GetFourCCCodes, (METHOD)D_GetGDISurface, (METHOD)D_GetMonitorFrequency,
    (METHOD)D_GetScanLine, (METHOD)D_GetVerticalBlankStatus, (METHOD)D_Initialize, (METHOD)D_RestoreDisplayMode,
    (METHOD)D_SetCooperativeLevel, (METHOD)D_SetDisplayMode, (METHOD)D_WaitForVerticalBlank, (METHOD)D_GetAvailableVidMem,
};

static DDraw *s_ddraw;

int w32_DirectDrawCreate(void *guid, void *out, void *outer)
{
    (void)guid; (void)outer;
    if (!s_ddraw) s_ddraw = (DDraw *)com_new(vt_ddraw, K_DDRAW, sizeof(DDraw));
    else s_ddraw->h.refs++;
    *(void **)out = s_ddraw;
    rt_log("DirectDrawCreate\n");
    return DD_OK;
}

int w32_DirectDrawEnumerateA(void *cb, void *ctx)
{
    static char desc[] = "Primary Display Driver", name[] = "display";
    ((int (*)())cb)(NULL, desc, name, ctx);
    return DD_OK;
}

/* ---- presentation ------------------------------------------------------- */
static unsigned short *s_conv;

void dd_present(void)
{
    extern volatile unsigned int g_dbg_presents;
    g_dbg_presents++;
    DDSurf *p = s_primary, *src;
    int overlay;
    if (!p) return;
    src = p->back ? p->back : p;
    /* a started 3D frame (even a z clear without polygons: name entry, records)
     * drew the background with GL and reset the back buffer to the empty
     * colour: it must go over that, not replace it (the screen went black) */
    overlay = d3d_drew_this_frame() || d3d_frame_is_open();
    if (s_nbg) bg_materialize();
    if (src->bpp == 16) {
        if (!overlay || src->touched2d)
            present_frame16((const unsigned short *)src->pixels, src->w, src->h_, src->pitch / 2, overlay,
                            (const short (*)[4])src->rect, overlay ? src->nrect : -1);
    } else if (src->bpp == 8) {
        int x, y;
        const unsigned short *pal = p->pal ? p->pal->pal565 : NULL;
        if (!s_conv) s_conv = (unsigned short *)host_malloc(640 * 480 * 2);
        if (pal && src->w <= 640 && src->h_ <= 480) {
            for (y = 0; y < src->h_; y++) {
                const unsigned char *r = src->pixels + y * src->pitch;
                unsigned short *o = s_conv + y * src->w;
                for (x = 0; x < src->w; x++) o[x] = pal[r[x]];
            }
            present_frame16(s_conv, src->w, src->h_, src->w, 0, NULL, -1);
        }
    }
    {
        static float last_dim;
        static unsigned int last_at;
        if (overlay && src->bpp == 16 && src->readback_frame == g_dbg_presents && s_rb_level && rnd_wide_ext() > 0) {
            /* a fade on the frame read back this frame: the 3D past the 4:3
             * edges is not in it, so darken it the same way */
            unsigned int now = frame_level(src);
            float k = now >= s_rb_level ? 1.0f : (float)now / s_rb_level;
            last_dim = 1.0f - k;
            last_at = g_dbg_presents;
            rnd_dim_sides(last_dim);
        } else if (overlay && s_fade_at == g_dbg_presents && s_fade_level >= 0 && rnd_wide_ext() > 0) {
            /* a fade step (FUN_00437911): the 4:3 area is the faded snapshot */
            last_dim = s_fade_level >= 255 ? 0.0f : 1.0f - s_fade_level / 255.0f;
            last_at = g_dbg_presents;
            rnd_dim_sides(last_dim);
        } else if (overlay && last_dim > 0 && g_dbg_presents - last_at <= 2 && rnd_wide_ext() > 0) {
            rnd_dim_sides(last_dim);     /* the last frames of a fade may not read back */
        }
    }
    s_prev_drew = overlay;
    g_scenery_drawn = 0;
    dd_clear_rects(src);
    bltstat_frame();
    menu_draw();
    d3d_end_frame();
    present_swap();
}
