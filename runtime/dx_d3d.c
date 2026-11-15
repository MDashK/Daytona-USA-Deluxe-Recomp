/* dx_d3d.c — Direct3D 5 immediate mode (IDirect3D2 / IDirect3DDevice2 /
 * IDirect3DViewport2 / IDirect3DTexture2 / IDirect3DMaterial2) on render.c. */
#include "dx.h"
#include "render.h"
#include "plat.h"

#define STUB_RET(n) { static int once; if (!once) { once = 1; rt_log("Direct3D: unimplemented %s\n", n); } return DD_OK; }

static void *com_new(METHOD *vtbl, int kind, unsigned int size)
{
    ComHdr *o = (ComHdr *)host_calloc(1, size);
    o->vtbl = vtbl; o->refs = 1; o->kind = kind;
    return o;
}
static uint X_AddRef(ComHdr *o) { return (uint)++o->refs; }
static uint X_Release(ComHdr *o) { return (uint)(--o->refs > 0 ? o->refs : 0); }

/* ---- state ---------------------------------------------------------------- */
static RState s_st;
/* DAYTONA_D3DSTATS: which methods the game calls */
static const char *s_cnt_name[64];
static int s_cnt[64], s_ncnt;
static void cnt(const char *n)
{
    int i;
    for (i = 0; i < s_ncnt; i++) if (s_cnt_name[i] == n) { s_cnt[i]++; return; }
    if (s_ncnt < 64) { s_cnt_name[s_ncnt] = n; s_cnt[s_ncnt++] = 1; }
}
#define CNT() cnt(__func__)
static void stats_add(const RVertex *v, int n);
static uint s_rs[64];
static int s_frame_open, s_drew;
#define MAXTEX 1024
static DDSurf *s_tex[MAXTEX];   /* texture handle -> surface */

static void rs_defaults(void)
{
    memset(s_rs, 0, sizeof s_rs);
    s_rs[3] = 1; s_rs[44] = 1; s_rs[45] = 1;     /* wrap */
    s_rs[7] = 1; s_rs[14] = 1; s_rs[23] = 4;     /* z on, write, LESSEQUAL */
    s_rs[9] = 2;                                 /* gouraud */
    s_rs[17] = 1; s_rs[18] = 1;                  /* nearest */
    s_rs[19] = 2; s_rs[20] = 1;                  /* ONE, ZERO */
    s_rs[21] = 4;                                /* MODULATEALPHA */
    s_rs[22] = 3;                                /* cull CCW */
    s_rs[25] = 8;                                /* alpha ALWAYS */
}

static unsigned int conv_color(DDSurf *s, uint v, int *alpha)
{
    uint r, g, b, a = 255;
    if (s->bpp == 16) {
        uint rm = s->rmask, gm = s->gmask, bm = s->bmask, am = s->amask;
        int rs = 0, gs = 0, bs = 0, as = 0, rb = 0, gb = 0, bb = 0, ab = 0;
        uint t;
        for (t = rm; t && !(t & 1); t >>= 1) rs++; for (; t & 1; t >>= 1) rb++;
        for (t = gm; t && !(t & 1); t >>= 1) gs++; for (; t & 1; t >>= 1) gb++;
        for (t = bm; t && !(t & 1); t >>= 1) bs++; for (; t & 1; t >>= 1) bb++;
        for (t = am; t && !(t & 1); t >>= 1) as++; for (; t & 1; t >>= 1) ab++;
        r = rb ? ((v & rm) >> rs) * 255 / ((1u << rb) - 1) : 0;
        g = gb ? ((v & gm) >> gs) * 255 / ((1u << gb) - 1) : 0;
        b = bb ? ((v & bm) >> bs) * 255 / ((1u << bb) - 1) : 0;
        if (ab) { a = ((v & am) >> as) * 255 / ((1u << ab) - 1); *alpha = 1; }
    } else if (s->bpp == 8 && s->pal) {
        uint e = s->pal->pal[v & 255];
        r = e & 255; g = (e >> 8) & 255; b = (e >> 16) & 255;
    } else {
        r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void tex_upload(DDSurf *s)
{
    static unsigned int *buf;
    static int bufn;
    int x, y, alpha = s->amask != 0;
    int ck = s->has_ckey;
    if (!s->pixels) return;
    if (bufn < s->w * s->h_) { host_free(buf); bufn = s->w * s->h_; buf = (unsigned int *)host_malloc(bufn * 4); }
    for (y = 0; y < s->h_; y++) {
        const unsigned char *row = s->pixels + y * s->pitch;
        for (x = 0; x < s->w; x++) {
            uint v = s->bpp == 16 ? ((const unsigned short *)row)[x] : s->bpp == 8 ? row[x] : ((const uint *)row)[x];
            unsigned int c = conv_color(s, v, &alpha);
            if (ck && v >= s->ckey_lo && v <= s->ckey_hi) { c &= 0x00ffffff; alpha = 1; }
            buf[y * s->w + x] = c;
        }
    }
    if (!s->tex) s->tex = rnd_texture_new();
    {   /* DAYTONA_TEXDUMP=1: write the first textures as PPM (debug) */
        static int ndump = -1;
        if (ndump < 0) ndump = getenv_flag("DAYTONA_TEXDUMP") ? 0 : 1000;
        if (ndump < 120) {
            char name[64], hdr[32];
            void *f;
            host_snprintf(name, sizeof name, "tex_%02d_%dx%d_bpp%d.ppm", ndump++, s->w, s->h_, s->bpp);
            if ((f = host_fopen(name, "wb")) != NULL) {
                int i;
                host_snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", s->w, s->h_);
                host_fwrite(f, hdr, (unsigned)host_strlen(hdr));
                for (i = 0; i < s->w * s->h_; i++) { unsigned char p[3] = { (unsigned char)buf[i], (unsigned char)(buf[i] >> 8), (unsigned char)(buf[i] >> 16) }; if (!(buf[i] >> 24)) { p[0] = 255; p[1] = 0; p[2] = 255; } host_fwrite(f, p, 3); }
                host_fclose(f);
            }
            rt_log("texdump h=%u %s masks r%04x g%04x b%04x a%04x ck=%d\n", s->handle, name, s->rmask, s->gmask, s->bmask, s->amask, ck);
        }
    }
    rnd_texture_upload_3d(s->tex, s->w, s->h_, buf);
    s->tex_w = alpha;      /* reused as "has alpha" flag */
    s->dirty = 0;
}

static void build_state(void)
{
    uint th = s_rs[1];
    DDSurf *t = (th && th < MAXTEX) ? s_tex[th] : NULL;
    if (t && !t->pixels) t = NULL;                /* released */
    if (t && (t->dirty || !t->tex)) tex_upload(t);
    s_st.tex = t ? t->tex : 0;
    s_st.tex_alpha = t ? (t->tex_w || (s_rs[41] && t->has_ckey)) : 0;
    s_st.zenable = s_rs[7] != 0;
    s_st.zwrite = s_rs[14] != 0;
    s_st.zfunc = (int)s_rs[23];
    s_st.blend = s_rs[27] != 0;
    s_st.srcblend = (int)s_rs[19];
    s_st.dstblend = (int)s_rs[20];
    s_st.alphatest = s_rs[15] != 0;
    s_st.alpharef = (int)s_rs[24];
    s_st.alphafunc = (int)s_rs[25];
    s_st.cull = (int)s_rs[22];
    s_st.shade = (int)s_rs[9];
    s_st.texblend = (int)s_rs[21];
    s_st.filter = (s_rs[17] == 2 || s_rs[17] >= 4) ? 2 : 1;
    s_st.addr_u = (int)(s_rs[44] ? s_rs[44] : s_rs[3]);
    s_st.addr_v = (int)(s_rs[45] ? s_rs[45] : s_rs[3]);
    s_st.fog = s_rs[28] != 0;
    s_st.fogcolor = s_rs[34];
}

/* fill [x0,x1) x [y0,y1) of a 16-bit surface with RND_OVERLAY_EMPTY */
static void empty_fill(DDSurf *b, int x0, int y0, int x1, int y1)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->w) x1 = b->w;
    if (y1 > b->h_) y1 = b->h_;
    for (y = y0; y < y1; y++) {
        unsigned short *r = (unsigned short *)(b->pixels + y * b->pitch);
        x = x0;
        if (x < x1 && (((recomp_uptr)(r + x)) & 3)) r[x++] = RND_OVERLAY_EMPTY;
        {   /* two pixels per store */
            unsigned int *r32 = (unsigned int *)(r + x), v = RND_OVERLAY_EMPTY * 0x10001u;
            int n = (x1 - x) >> 1, i;
            for (i = 0; i < n; i++) r32[i] = v;
            x += n * 2;
        }
        for (; x < x1; x++) r[x] = RND_OVERLAY_EMPTY;
    }
}

int d3d_frame_is_open(void) { return s_frame_open; }

void d3d_frame_begin_if_needed(void)
{
    if (s_frame_open) return;
    s_frame_open = 1;
    {   /* widescreen: the engine projects into a 352x224 view (half sizes
         * 176x112 at +0x18/+0x1a of the camera block 0x4e94e4) scaled to
         * 640x480; a half width of 233 sees 16:9 at the same height, which
         * render.c spreads over the whole screen.  Only the full-screen view
         * (1 player); split screens stay 4:3. */
        extern int g_opt_widescreen;
        /* the name entry and the records (scenes 42, 43: a 2D page with 3D
         * letters) stay 4:3 */
        int scene = G(int, 0x866684), wide = g_opt_widescreen && scene != 42 && scene != 43;
        short *half = NULL;
        recomp_uptr cam = (recomp_uptr)G(uint, 0x4e94e4);
        static const unsigned cams[2] = { 0x56ec00, 0x56f000 };   /* near / far scenery pass */
        int i;
        for (i = 0; i < 2; i++) {
            short *h = &G(short, cams[i] + 0x18);
            if (h[1] != 112) continue;
            if (wide && h[0] == 176) h[0] = 233;
            else if (!wide && h[0] == 233) h[0] = 176;
        }
        if (cam) half = (short *)(cam + 0x18);
        if (half && half[1] == 112) {
            if (wide && half[0] == 176) half[0] = 233;
            else if (!wide && half[0] == 233) half[0] = 176;
        }
        rnd_set_wide(half && half[1] == 112 && half[0] == 233);
        {   /* DAYTONA_CAMSET=off:value - poke a camera field (experiment) */
            static int off = -2, val;
            if (off == -2) {
                const char *e = getenv("DAYTONA_CAMSET");
                off = -1;
                if (e) { off = (int)strtol(e, (char **)&e, 0); if (*e == ':') val = (int)strtol(e + 1, NULL, 0); }
            }
            if (cam && off >= 0) *(short *)(cam + off) = (short)val;
        }
    }
    {   /* DAYTONA_CAMLOG=1: the engine's camera/viewport block (debug) */
        static int on = -1, n;
        if (on < 0) on = getenv_flag("DAYTONA_CAMLOG");
        if (on && (n++ % 90) == 0) {
            unsigned char *c = (unsigned char *)(recomp_uptr)G(uint, 0x4e94e4);
            if (c) {
                int i;
                rt_log("cam %08x:", recomp_host_to_va(c));
                for (i = 0; i < 0xc0; i += 2) rt_log(" %d", *(short *)(c + i));
                rt_log("\n");
            }
        }
    }
    rnd_begin_frame();
    /* 2D drawn into the back buffer before the 3D: show it as background */
    {
        DDSurf *p = dd_primary();
        DDSurf *b = p ? (p->back ? p->back : p) : NULL;
        if (b && b->bpp == 16) {
            int k;
            /* the background blits the game queued (sky, fills) go out as GL
             * quads; 2D written with the CPU is shown as a full frame */
            if (!dd_bg_draw_gl(b) && b->touched2d)
                rnd_draw_frame16((const unsigned short *)b->pixels, b->w, b->h_, b->pitch / 2, 0, NULL, -1);
            /* from now on 2D drawn into the back buffer goes over the 3D:
             * mark the buffer empty with a colour the art does not use, so
             * that black stays black.  Only the areas written since the last
             * reset need it. */
            if (!b->fill_valid || b->nfill < 0) empty_fill(b, 0, 0, b->w, b->h_);
            else for (k = 0; k < b->nfill; k++) empty_fill(b, b->fillr[k][0], b->fillr[k][1], b->fillr[k][2], b->fillr[k][3]);
            b->fill_valid = 1;
            b->nfill = 0;
            dd_clear_rects(b);
        }
    }
}

int d3d_drew_this_frame(void) { return s_drew; }

/* DAYTONA_D3DSTATS=1: per-frame summary of what reaches Direct3D */
static int s_st_on = -1, s_st_frames, s_st_calls, s_st_verts, s_st_tex, s_st_bad;
static float s_st_minx, s_st_maxx, s_st_miny, s_st_maxy, s_st_minz, s_st_maxz, s_st_minw, s_st_maxw;
static void stats_add(const RVertex *v, int n)
{
    int i;
    if (s_st_on < 0) s_st_on = getenv_flag("DAYTONA_D3DSTATS");
    if (!s_st_on) return;
    s_st_calls++;
    if (s_st.tex) s_st_tex++;
    if ((s_st_calls == 200 || s_st_calls == 300 || s_st_calls == 400) && s_st_frames % 60 == 59) {
        rt_log("d3d sample: tex=%d texblend=%d shade=%d fog=%d fogcol=%08x blend=%d %d/%d", s_st.tex, s_st.texblend, s_st.shade,
               s_st.fog, s_st.fogcolor, s_st.blend, s_st.srcblend, s_st.dstblend);
        { extern uint recomp_host_to_va(const void *p); rt_log(" va=%08x", recomp_host_to_va(v)); }
        for (i = 0; i < n && i < 4; i++) rt_log(" [c=%08x s=%08x uv=%.3f,%.3f]", v[i].color, v[i].specular, v[i].u, v[i].v);
        rt_log("\n");
    }
    for (i = 0; i < n; i++) {
        float x = v[i].x, y = v[i].y, z = v[i].z, w = v[i].rhw;
        if (x != x || y != y || z != z || w != w) { s_st_bad++; continue; }
        if (!s_st_verts) { s_st_minx = s_st_maxx = x; s_st_miny = s_st_maxy = y; s_st_minz = s_st_maxz = z; s_st_minw = s_st_maxw = w; }
        if (x < s_st_minx) s_st_minx = x; if (x > s_st_maxx) s_st_maxx = x;
        if (y < s_st_miny) s_st_miny = y; if (y > s_st_maxy) s_st_maxy = y;
        if (z < s_st_minz) s_st_minz = z; if (z > s_st_maxz) s_st_maxz = z;
        if (w < s_st_minw) s_st_minw = w; if (w > s_st_maxw) s_st_maxw = w;
        s_st_verts++;
    }
}
void d3d_end_frame(void)
{
    { void lifted_count_frame(void); if (s_drew) lifted_count_frame(); }
    {   /* DAYTONA_CARDUMP=1: per-frame dump of the car structures (debug,
         * compared with tools/oracle probes of the original) */
        static int on = -1, frame;
        if (on < 0) on = getenv_flag("DAYTONA_CARDUMP");
        if (on && frame == 1000) {   /* one image snapshot for offline searching */
            void *f = host_fopen("image_dump.bin", "wb");
            if (f) { host_fwrite(f, VA(0x400000), 0x4c1000); host_fclose(f); }
        }
        if (on && frame < 4000) {
            int k, i;
            rt_log("%d cars draws=%d verts=%d", frame++, s_st_calls, s_st_verts);
            if (getenv_flag("DAYTONA_CARFULL")) {
                static const int sel[4] = { 17, 22, 25, 38 };
                for (k = 0; k < 4; k++) {
                    const unsigned char *p = (const unsigned char *)VA(0x614800 + sel[k] * 0x100);
                    rt_log(" full%d=", sel[k]);
                    for (i = 0; i < 256; i++) rt_log("%02x", p[i]);
                }
            }
            for (k = 0; k < 40; k++) {   /* car render records 0x614800 + n*0x100 */
                const unsigned char *p = (const unsigned char *)VA(0x614800 + k * 0x100);
                char hx[40];
                for (i = 0; i < 16; i++) { hx[2 * i] = "0123456789abcdef"[p[i] >> 4]; hx[2 * i + 1] = "0123456789abcdef"[p[i] & 15]; }
                hx[32] = 0;
                rt_log(" %s", hx);
            }
            rt_log("\n");
        }
    }
    {   /* DAYTONA_FILLLOG=1: the engine's polygon buffers at the end of the frame (debug) */
        static int lg = -1;
        if (lg < 0) lg = getenv_flag("DAYTONA_FILLLOG");
        if (lg && s_drew) {
            const unsigned char *c1 = (const unsigned char *)VA(0x56ec00), *c2 = (const unsigned char *)VA(0x56f000);
            rt_log("fill c1 %04x/%04x c2 %04x/%04x\n", *(const unsigned short *)(c1 + 0xa8), *(const unsigned short *)(c1 + 0xaa),
                   *(const unsigned short *)(c2 + 0xa8), *(const unsigned short *)(c2 + 0xaa));
        }
    }
    s_frame_open = 0; s_drew = 0;
    if (s_st_on < 0) s_st_on = getenv_flag("DAYTONA_D3DSTATS");
    if (s_st_on > 0 && ++s_st_frames % 60 == 0) {
        { static unsigned int t0; unsigned int t = plat_time_ms(); rt_log("fps %.1f ", t0 ? 60000.0 / (t - t0) : 0.0); t0 = t; }
        rt_log("d3d: %d draws (%d textured) %d verts %d NaN  x %.1f..%.1f y %.1f..%.1f z %.3f..%.3f rhw %.4f..%.4f\n",
               s_st_calls, s_st_tex, s_st_verts, s_st_bad, s_st_minx, s_st_maxx, s_st_miny, s_st_maxy, s_st_minz, s_st_maxz, s_st_minw, s_st_maxw);
    }
    if (s_st_on > 0 && s_st_frames % 300 == 0) {
        int i;
        rt_log("d3d methods (300 frames):");
        for (i = 0; i < s_ncnt; i++) rt_log(" %s=%d", s_cnt_name[i], s_cnt[i]);
        rt_log("\n");
        s_ncnt = 0;
    }
    s_st_calls = s_st_verts = s_st_tex = s_st_bad = 0;
}

void present_frame16(const unsigned short *px, int w, int h, int pitch_px, int overlay,
                     const short (*rects)[4], int nrect)
{
    if (!overlay) { rnd_begin_frame(); }
    rnd_draw_frame16(px, w, h, pitch_px, overlay, rects, nrect);
}

void present_swap(void) { rnd_swap(); }

/* ---- IDirect3DTexture2 (an interface of a DDSurf) ---------------------- */
static uint T_QueryInterface(D3DTexIface *t, void *iid, void **out) { CNT(); return ((uint (*)())t->s->h.vtbl[0])(t->s, iid, out); }
static uint T_AddRef(D3DTexIface *t) { CNT(); return (uint)++t->s->h.refs; }
static uint T_Release(D3DTexIface *t) { CNT(); return (uint)(--t->s->h.refs > 0 ? t->s->h.refs : 0); }
static uint T_GetHandle(D3DTexIface *t, void *dev, uint *h)
{ CNT();
    int i;
    (void)dev;
    if (!t->s->handle) {
        for (i = 1; i < MAXTEX && s_tex[i]; i++) ;
        if (i == MAXTEX) return DDERR_GENERIC;
        s_tex[i] = t->s;
        t->s->handle = (uint)i;
    }
    *h = t->s->handle;
    return DD_OK;
}
static uint T_PaletteChanged(D3DTexIface *t) { CNT(); t->s->dirty = 1; return DD_OK; }
static uint T_Load(D3DTexIface *t, void *srcv)
{ CNT();
    DDSurf *d = t->s, *s = dd_surface_of(srcv);
    int y, n;
    if (!s || !d->pixels || !s->pixels) return DDERR_INVALIDPARAMS;
    n = (s->w * s->bpp / 8) < d->pitch ? s->w * s->bpp / 8 : d->pitch;
    for (y = 0; y < d->h_ && y < s->h_; y++) memcpy(d->pixels + y * d->pitch, s->pixels + y * s->pitch, (unsigned)n);
    d->rmask = s->rmask; d->gmask = s->gmask; d->bmask = s->bmask; d->amask = s->amask;
    if (s->has_ckey) { d->has_ckey = 1; d->ckey_lo = s->ckey_lo; d->ckey_hi = s->ckey_hi; }
    if (s->pal) d->pal = s->pal;
    d->dirty = 1;
    return DD_OK;
}
/* IDirect3DTexture (v1) has Initialize between Release and GetHandle; the
 * game uses IDirect3DTexture2, whose order this is. */
METHOD vt_d3dtexture[] = { (METHOD)T_QueryInterface, (METHOD)T_AddRef, (METHOD)T_Release,
                           (METHOD)T_GetHandle, (METHOD)T_PaletteChanged, (METHOD)T_Load };

/* ---- IDirect3DMaterial2 ------------------------------------------------- */
typedef struct D3DMat { ComHdr h; float diffuse[4]; uint handle; } D3DMat;
#define MAXMAT 64
static D3DMat *s_mat[MAXMAT];
static uint M_QueryInterface(ComHdr *o, void *iid, void **out) { CNT(); (void)iid; *out = o; o->refs++; return DD_OK; }
static uint M_SetMaterial(D3DMat *m, float *mat) { CNT(); memcpy(m->diffuse, mat + 1, 16); return DD_OK; }
static uint M_GetMaterial(D3DMat *m, float *mat) { CNT(); memcpy(mat + 1, m->diffuse, 16); return DD_OK; }
static uint M_GetHandle(D3DMat *m, void *dev, uint *h)
{ CNT();
    int i;
    (void)dev;
    if (!m->handle) {
        for (i = 1; i < MAXMAT && s_mat[i]; i++) ;
        if (i < MAXMAT) { s_mat[i] = m; m->handle = (uint)i; }
    }
    *h = m->handle;
    return DD_OK;
}
METHOD vt_material[] = { (METHOD)M_QueryInterface, (METHOD)X_AddRef, (METHOD)X_Release,
                         (METHOD)M_SetMaterial, (METHOD)M_GetMaterial, (METHOD)M_GetHandle };

/* ---- IDirect3DViewport2 ------------------------------------------------- */
typedef struct D3DView { ComHdr h; uint vp[16]; uint bgmat; } D3DView;
static uint V_QueryInterface(ComHdr *o, void *iid, void **out) { CNT(); (void)iid; *out = o; o->refs++; return DD_OK; }
static uint V_Initialize(void) { CNT(); return DD_OK; }
static uint V_GetViewport(D3DView *v, uint *d) { CNT(); memcpy(d + 1, v->vp + 1, 40); return DD_OK; }
static uint V_SetViewport(D3DView *v, uint *d) { CNT(); memcpy(v->vp, d, 44); return DD_OK; }
static uint V_TransformVertices(void) STUB_RET("Viewport::TransformVertices")
static uint V_LightElements(void) STUB_RET("Viewport::LightElements")
static uint V_SetBackground(D3DView *v, uint mat) { CNT(); v->bgmat = mat; return DD_OK; }
static uint V_GetBackground(D3DView *v, uint *mat, int *valid) { CNT(); *mat = v->bgmat; if (valid) *valid = v->bgmat != 0; return DD_OK; }
static uint V_SetBackgroundDepth(void) { CNT(); return DD_OK; }
static uint V_GetBackgroundDepth(void) STUB_RET("Viewport::GetBackgroundDepth")
static uint V_Clear(D3DView *v, uint n, int *rects, uint flags)
{ CNT();
    uint argb = 0;
    if (v->bgmat && v->bgmat < MAXMAT && s_mat[v->bgmat]) {
        float *c = s_mat[v->bgmat]->diffuse;
        argb = ((uint)(c[0] * 255) << 16) | ((uint)(c[1] * 255) << 8) | (uint)(c[2] * 255);
    }
    d3d_frame_begin_if_needed();
    rnd_clear(flags & 1, (flags & 2) != 0, argb, rects, (int)n);
    return DD_OK;
}
static uint V_AddLight(void) { CNT(); return DD_OK; }
static uint V_DeleteLight(void) { CNT(); return DD_OK; }
static uint V_NextLight(void) { CNT(); return DDERR_GENERIC; }
static uint V_GetViewport2(D3DView *v, uint *d) { CNT(); memcpy(d + 1, v->vp + 1, 40); return DD_OK; }
static uint V_SetViewport2(D3DView *v, uint *d) { CNT(); memcpy(v->vp, d, 44); return DD_OK; }
METHOD vt_viewport[] = { (METHOD)V_QueryInterface, (METHOD)X_AddRef, (METHOD)X_Release, (METHOD)V_Initialize,
                         (METHOD)V_GetViewport, (METHOD)V_SetViewport, (METHOD)V_TransformVertices, (METHOD)V_LightElements,
                         (METHOD)V_SetBackground, (METHOD)V_GetBackground, (METHOD)V_SetBackgroundDepth, (METHOD)V_GetBackgroundDepth,
                         (METHOD)V_Clear, (METHOD)V_AddLight, (METHOD)V_DeleteLight, (METHOD)V_NextLight,
                         (METHOD)V_GetViewport2, (METHOD)V_SetViewport2 };

/* ---- IDirect3DDevice2 ----------------------------------------------------- */
typedef struct D3DDev { ComHdr h; void *d3d; D3DView *vp; } D3DDev;
static void *s_d3d;
static D3DView *s_cur_vp;

static void fill_devdesc(uint *d)
{
    uint *tri;
    memset(d, 0, 252);
    d[0] = 204;
    d[1] = 0x7ff;                  /* all fields valid */
    d[2] = 2;                      /* D3DCOLOR_RGB */
    d[3] = 0x1 | 0x2 | 0x4 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x80000;  /* FLOATTLVERTEX ... DRAWPRIMTLVERTEX HWRASTERIZATION */
    d[4] = 8; d[5] = 1;            /* transform caps */
    d[6] = 1;                      /* clipping */
    d[7] = 16; d[8] = 0xf; d[9] = 1; d[10] = 8;   /* lighting caps */
    tri = d + 25;                  /* dpcTriCaps at byte 100 */
    tri[0] = 56;
    tri[1] = 0x7f;                 /* MiscCaps: masking, cull none/cw/ccw ... */
    tri[2] = 0x1 | 0x2 | 0x10 | 0x80 | 0x100 | 0x200;   /* dither, rop2.., zbuffer, fog vertex/table, zbias.. */
    tri[3] = 0xff;                 /* ZCmpCaps */
    tri[4] = 0x1fff; tri[5] = 0x1fff;
    tri[6] = 0xff;                 /* AlphaCmpCaps */
    tri[7] = 0xffff;               /* ShadeCaps */
    tri[8] = 0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x100;     /* perspective, pow2, alpha, transparency(ckey), border, sqonly.. */
    tri[9] = 0x3f;                 /* filter caps */
    tri[10] = 0xff;                /* blend caps */
    tri[11] = 0x7;                 /* address caps */
    memcpy(d + 11, tri, 56);       /* line caps the same */
    d[39] = 0x400 | 0x100;         /* render depth: 16, 32 bit */
    d[40] = 0x400;                 /* z depth: 16 */
    d[41] = 0;                     /* max buffer size: unlimited */
    d[42] = 65535;                 /* max vertex count */
    d[43] = 1; d[44] = 1; d[45] = 256; d[46] = 256;
}

static uint DV_QueryInterface(ComHdr *o, void *iid, void **out) { CNT(); (void)iid; *out = o; o->refs++; return DD_OK; }
static void fill_devdesc_sized(uint *d)
{
    uint tmp[63], n = d[0];
    fill_devdesc(tmp);
    if (n < 4 || n > 252) n = 204;
    memcpy(d, tmp, n);
    d[0] = n;
}
static uint DV_GetCaps(D3DDev *d, uint *hw, uint *hel) { CNT(); (void)d; if (hw) fill_devdesc_sized(hw); if (hel) fill_devdesc_sized(hel); return DD_OK; }
static uint DV_SwapTextureHandles(D3DDev *d, D3DTexIface *a, D3DTexIface *b)
{ CNT();
    uint ha = a->s->handle, hb = b->s->handle;
    (void)d;
    a->s->handle = hb; b->s->handle = ha;
    if (ha < MAXTEX) s_tex[ha] = b->s;
    if (hb < MAXTEX) s_tex[hb] = a->s;
    return DD_OK;
}
static uint DV_GetStats(void) STUB_RET("Device::GetStats")
static uint DV_AddViewport(D3DDev *d, D3DView *v) { CNT(); d->vp = v; if (!s_cur_vp) s_cur_vp = v; return DD_OK; }
static uint DV_DeleteViewport(D3DDev *d, D3DView *v) { CNT(); if (d->vp == v) d->vp = NULL; if (s_cur_vp == v) s_cur_vp = NULL; return DD_OK; }
static uint DV_NextViewport(void) STUB_RET("Device::NextViewport")
static uint DV_EnumTextureFormats(D3DDev *d, int (*cb)(), void *ctx)
{
    static const uint fmts[][5] = {
        { 16, 0xf800, 0x07e0, 0x001f, 0 },      /* RGB565 */
        { 16, 0x7c00, 0x03e0, 0x001f, 0 },      /* RGB555 */
        { 16, 0x7c00, 0x03e0, 0x001f, 0x8000 }, /* ARGB1555 */
        { 16, 0x0f00, 0x00f0, 0x000f, 0xf000 }, /* ARGB4444 */
    };
    uint desc[27];
    int i;
    (void)d;
    for (i = 0; i < 4; i++) {
        memset(desc, 0, sizeof desc);
        desc[0] = 108;
        desc[1] = 0x1000;
        desc[18] = 32;
        desc[19] = 0x40 | (fmts[i][4] ? 1 : 0);
        desc[21] = fmts[i][0]; desc[22] = fmts[i][1]; desc[23] = fmts[i][2]; desc[24] = fmts[i][3]; desc[25] = fmts[i][4];
        if (!cb(desc, ctx)) break;
    }
    return DD_OK;
}
static uint DV_BeginScene(void) { CNT(); d3d_frame_begin_if_needed(); return DD_OK; }
static uint DV_EndScene(void) { CNT(); return DD_OK; }
static uint DV_GetDirect3D(D3DDev *d, void **out) { CNT(); *out = d->d3d; return DD_OK; }
static uint DV_SetCurrentViewport(D3DDev *d, D3DView *v) { CNT(); (void)d; s_cur_vp = v; return DD_OK; }
static uint DV_GetCurrentViewport(D3DDev *d, void **out) { CNT(); (void)d; *out = s_cur_vp; return DD_OK; }
static uint DV_SetRenderTarget(void) { CNT(); return DD_OK; }
static uint DV_GetRenderTarget(D3DDev *d, void **out) { CNT(); (void)d; *out = dd_primary() ? dd_primary()->back : NULL; return DD_OK; }

/* DAYTONA_PRIMLOG=1: log draws other than 4-vertex quads (debug) */
static void prim_log(const char *what, uint prim, uint vt, const RVertex *v, uint n)
{
    static int on = -1, cnt;
    if (on < 0) on = getenv("DAYTONA_PRIMLOG") ? host_atoi(getenv("DAYTONA_PRIMLOG")) : 0;
    if (on == 3) {   /* quads inside the minimap box, all corners */
        uint i;
        if (cnt > 300 || !getenv("DAYTONA_PRIMTEX") || s_rs[1] != (uint)host_atoi(getenv("DAYTONA_PRIMTEX")) || n < 2 || (v[0].u - v[1].u) * (v[0].u - v[1].u) + (v[0].v - v[n - 1].v) * (v[0].v - v[n - 1].v) < 0.09f) return;
        cnt++;
        rt_log("map? tex=%u blend=%u/%u/%u tb=%u ck=%u at=%u", s_rs[1], s_rs[27], s_rs[19], s_rs[20], s_rs[21], s_rs[41], s_rs[15]);
        for (i = 0; i < n; i++) rt_log(" (%.1f,%.1f z%.3f w%.3f c%08x s%08x uv%.3f,%.3f)", v[i].x, v[i].y, v[i].z, v[i].rhw, v[i].color, v[i].specular, v[i].u, v[i].v);
        rt_log("\n");
        return;
    }
    if (!on || cnt > 400 || (prim == 5 && n == 4 && !(n && v[0].x > 480 && v[0].y > 240 && on == 2))) return;
    cnt++;
    rt_log("%s prim=%u vt=%u n=%u tex=%u blend=%u/%u/%u tb=%u v0=(%.1f,%.1f,%.3f c=%08x uv=%.3f,%.3f)\n", what, prim, vt, n, s_rs[1], s_rs[27], s_rs[19], s_rs[20], s_rs[21],
           n ? v[0].x : 0.f, n ? v[0].y : 0.f, n ? v[0].z : 0.f, n ? v[0].color : 0, n ? v[0].u : 0.f, n ? v[0].v : 0.f);
}
/* Begin/Vertex/End */
static RVertex s_bv[1024];
static int s_bn, s_bprim;
static uint DV_Begin(D3DDev *d, uint prim, uint vt, uint fl) { CNT(); (void)d; (void)vt; (void)fl; s_bprim = (int)prim; s_bn = 0; return DD_OK; }
static uint DV_BeginIndexed(void) STUB_RET("Device::BeginIndexed")
static uint DV_Vertex(D3DDev *d, RVertex *v) { CNT(); (void)d; if (s_bn < 1024) s_bv[s_bn++] = *v; return DD_OK; }
static uint DV_Index(void) STUB_RET("Device::Index")
static uint DV_End(D3DDev *d, uint fl)
{ CNT(); prim_log("Begin/End", (uint)s_bprim, 3, s_bv, (uint)s_bn);
    (void)d; (void)fl;
    d3d_frame_begin_if_needed();
    build_state();
    stats_add(s_bv, s_bn);
    rnd_draw(s_bprim, s_bv, s_bn, NULL, 0, &s_st);
    s_drew = 1;
    return DD_OK;
}
static uint DV_GetRenderState(D3DDev *d, uint st, uint *v) { CNT(); (void)d; *v = st < 64 ? s_rs[st] : 0; return DD_OK; }
static uint DV_SetRenderState(D3DDev *d, uint st, uint v) { CNT(); (void)d; if (st < 64) s_rs[st] = v; return DD_OK; }
static uint DV_GetLightState(void) STUB_RET("Device::GetLightState")
static uint DV_SetLightState(void) { CNT(); return DD_OK; }
static uint DV_SetTransform(void) { CNT(); return DD_OK; }
static uint DV_GetTransform(void) STUB_RET("Device::GetTransform")
static uint DV_MultiplyTransform(void) STUB_RET("Device::MultiplyTransform")
/* DAYTONA_HUDLOG=1: the draws of one frame every 150, bounding box + depth (debug) */
static void hud_d3dlog(const RVertex *v, uint n)
{
    extern volatile unsigned int g_dbg_presents;
    static int lg = -1;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    uint i;
    if (lg < 0) lg = getenv("DAYTONA_HUDLOG") != NULL;
    if (!lg || g_dbg_presents % 150 != 0) return;
    for (i = 0; i < n; i++) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
        if (v[i].z < z0) z0 = v[i].z;
        if (v[i].z > z1) z1 = v[i].z;
    }
    rt_log("d3d f=%u n=%u (%.0f,%.0f,%.0f,%.0f) z %.4f..%.4f w %g tex=%u ze=%d zw=%d bl=%d\n", g_dbg_presents, n,
           x0, y0, x1, y1, z0, z1, n ? v[0].rhw : 0.f, s_st.tex, s_st.zenable, s_st.zwrite, s_st.blend);
}
/* wide HUD: the HUD's D3D quads (condition box, course map, their markers,
 * tachometer) are the only draws with rhw 1e-5; those on the right go to the
 * screen edge */
#define HUD_RHW(w) ((w) > 0.0f && (w) < 2e-5f)
static RVertex *hud_move(RVertex *v, uint n)
{
    static RVertex mv[64];
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, ext;
    uint i;
    int side;
    if (!n || n > 64 || !HUD_RHW(v[0].rhw) || (ext = rnd_wide_ext()) <= 0) return v;
    for (i = 0; i < n; i++) {
        if (!HUD_RHW(v[i].rhw)) return v;
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    side = rnd_hud_side((int)x0, (int)y0, (int)x1, (int)y1, 1);
    {   /* DAYTONA_HUDLOG=2: the HUD quads left in place (debug) */
        static int lg = -1, n;
        if (lg < 0) lg = getenv("DAYTONA_HUDLOG") ? host_atoi(getenv("DAYTONA_HUDLOG")) == 2 : 0;
        if (lg && !side && n++ < 3000)
            rt_log("hudstay (%.0f,%.0f,%.0f,%.0f) n=%u tex=%u\n", x0, y0, x1, y1, n, s_st.tex);
    }
    if (!side) return v;
    for (i = 0; i < n; i++) { mv[i] = v[i]; mv[i].x += side * ext; }
    return mv;
}
static uint DV_DrawPrimitive(D3DDev *d, uint prim, uint vt, RVertex *v, uint n, uint fl)
{ CNT(); prim_log("DrawPrimitive", prim, vt, v, n);
    (void)d; (void)fl;
    if (vt != 3) { static int once; if (!once) { once = 1; rt_log("DrawPrimitive: vertex type %u not supported\n", vt); } return DD_OK; }
    d3d_frame_begin_if_needed();
    build_state();
    hud_d3dlog(v, n);
    stats_add(v, (int)n);
    v = hud_move(v, n);
    rnd_draw((int)prim, v, (int)n, NULL, 0, &s_st);
    s_drew = 1;
    return DD_OK;
}
static uint DV_DrawIndexedPrimitive(D3DDev *d, uint prim, uint vt, RVertex *v, uint n, unsigned short *idx, uint ni, uint fl)
{ CNT(); prim_log("DrawIndexed", prim, vt, v, n);
    (void)d; (void)fl;
    if (vt != 3) return DD_OK;
    d3d_frame_begin_if_needed();
    build_state();
    hud_d3dlog(v, n);
    stats_add(v, (int)n);
    v = hud_move(v, n);
    rnd_draw((int)prim, v, (int)n, idx, (int)ni, &s_st);
    s_drew = 1;
    return DD_OK;
}
static uint DV_SetClipStatus(void) { CNT(); return DD_OK; }
static uint DV_GetClipStatus(void) STUB_RET("Device::GetClipStatus")

METHOD vt_device[] = {
    (METHOD)DV_QueryInterface, (METHOD)X_AddRef, (METHOD)X_Release, (METHOD)DV_GetCaps,
    (METHOD)DV_SwapTextureHandles, (METHOD)DV_GetStats, (METHOD)DV_AddViewport, (METHOD)DV_DeleteViewport,
    (METHOD)DV_NextViewport, (METHOD)DV_EnumTextureFormats, (METHOD)DV_BeginScene, (METHOD)DV_EndScene,
    (METHOD)DV_GetDirect3D, (METHOD)DV_SetCurrentViewport, (METHOD)DV_GetCurrentViewport, (METHOD)DV_SetRenderTarget,
    (METHOD)DV_GetRenderTarget, (METHOD)DV_Begin, (METHOD)DV_BeginIndexed, (METHOD)DV_Vertex,
    (METHOD)DV_Index, (METHOD)DV_End, (METHOD)DV_GetRenderState, (METHOD)DV_SetRenderState,
    (METHOD)DV_GetLightState, (METHOD)DV_SetLightState, (METHOD)DV_SetTransform, (METHOD)DV_GetTransform,
    (METHOD)DV_MultiplyTransform, (METHOD)DV_DrawPrimitive, (METHOD)DV_DrawIndexedPrimitive, (METHOD)DV_SetClipStatus,
    (METHOD)DV_GetClipStatus,
};

/* ---- IDirect3D2 ------------------------------------------------------------ */
static uint D3_QueryInterface(ComHdr *o, void *iid, void **out) { CNT(); (void)iid; *out = o; o->refs++; return DD_OK; }
static uint D3_EnumDevices(ComHdr *o, int (*cb)(), void *ctx)
{
    static unsigned char hal_guid[16] = { 0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e };
    static char desc[] = "PS Vita GPU (VitaGL)", name[] = "Direct3D HAL";
    uint hw[63], hel[63];
    (void)o;
    fill_devdesc(hw);
    fill_devdesc(hel);
    cb(hal_guid, desc, name, hw, hel, ctx);
    return DD_OK;
}
static uint D3_CreateLight(ComHdr *o, void **out) { CNT(); (void)o; *out = NULL; return DDERR_UNSUPPORTED; }
static uint D3_CreateMaterial(ComHdr *o, void **out, void *u) { CNT(); (void)o; (void)u; *out = com_new(vt_material, K_MAT, sizeof(D3DMat)); return DD_OK; }
static uint D3_CreateViewport(ComHdr *o, void **out, void *u) { CNT(); (void)o; (void)u; *out = com_new(vt_viewport, K_VIEWPORT, sizeof(D3DView)); return DD_OK; }
static uint D3_FindDevice(ComHdr *o, uint *search, uint *result)
{ CNT();
    static unsigned char hal_guid[16] = { 0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e };
    (void)o; (void)search;
    if (result) {
        uint tmp[63];
        memcpy(result + 1, hal_guid, 16);
        fill_devdesc(tmp);
        memcpy(result + 5, tmp, 204);
        memcpy(result + 5 + 51, tmp, 204);
    }
    return DD_OK;
}
static uint D3_CreateDevice(ComHdr *o, void *clsid, void *surf, void **out)
{ CNT();
    D3DDev *d = (D3DDev *)com_new(vt_device, K_D3DDEV, sizeof(D3DDev));
    (void)clsid; (void)surf;
    d->d3d = o;
    rs_defaults();
    rt_log("Direct3D: CreateDevice\n");
    *out = d;
    return DD_OK;
}
METHOD vt_d3d[] = { (METHOD)D3_QueryInterface, (METHOD)X_AddRef, (METHOD)X_Release, (METHOD)D3_EnumDevices,
                    (METHOD)D3_CreateLight, (METHOD)D3_CreateMaterial, (METHOD)D3_CreateViewport, (METHOD)D3_FindDevice,
                    (METHOD)D3_CreateDevice };

void *d3d_create(void)
{
    if (!s_d3d) s_d3d = com_new(vt_d3d, K_D3D, sizeof(ComHdr));
    else ((ComHdr *)s_d3d)->refs++;
    return s_d3d;
}
