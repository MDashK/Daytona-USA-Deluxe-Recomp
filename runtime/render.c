/* render.c — fixed-function OpenGL 1.x backend (Windows opengl32 / vitaGL).
 *
 * D3D TL vertices are already in screen space: each one is turned into a
 * clip-space position (x*w, y*w, z*w, w) with w = 1/rhw, so GL's perspective
 * divide gives back the screen position and texturing stays
 * perspective-correct.  Both matrices stay identity.
 */
#include <stdlib.h>
#include <string.h>
#if defined(__vita__)
#include <vitaGL.h>
#else
#include <windows.h>
#include <GL/gl.h>
#endif
#include "render.h"
#include "plat.h"
#include "prof.h"
void rt_log(const char *fmt, ...);

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif

static int s_vw = 640, s_vh = 480;          /* game coordinates */
static int s_sw, s_sh;                        /* screen (the picture's area) */
static int s_ox, s_oy, s_win_w, s_win_h;      /* its place in the window (a resizable PC window) */
static int s_vx, s_vy, s_vpw, s_vph;          /* 4:3 viewport on screen */
/* 2D frame textures in rotation: two sets (background drawn before the 3D,
 * overlay drawn after it) of FRAME_RING each.  The GPU renders a frame or two
 * behind the CPU, and the texture memory is written directly, so a texture
 * must not come round again before the GPU has drawn with it. */
#define FRAME_RING 4
#define NFRAME (2 * FRAME_RING)
static GLuint s_frame_tex[NFRAME];
static int s_frame_w[NFRAME], s_frame_h[NFRAME], s_frame_next[2];

#define MAXV 4096
static float s_pos[MAXV * 4], s_uv[MAXV * 2];
static unsigned char s_col[MAXV * 4];

/* The picture keeps the Vita screen's shape (960x544: the widened 3D view of
 * dx_d3d.c matches it), or 4:3 without widescreen, centred in a window of any
 * size (black bars around it). */
static void screen_layout(void)
{
    extern int g_opt_widescreen;
    int w, h;
    plat_screen_size(&w, &h);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    s_win_w = w; s_win_h = h;
    if (g_opt_widescreen) {
        if (w * 544 > h * 960) { s_sh = h; s_sw = (h * 960 + 272) / 544; }
        else { s_sw = w; s_sh = (w * 544 + 480) / 960; }
    } else {
        if (w * 3 > h * 4) { s_sh = h; s_sw = h * 4 / 3; }
        else { s_sw = w; s_sh = w * 3 / 4; }
    }
    s_ox = (w - s_sw) / 2;
    s_oy = (h - s_sh) / 2;
    s_vph = s_sh;
    s_vpw = s_sh * 4 / 3;
    if (s_vpw > s_sw) { s_vpw = s_sw; s_vph = s_sw * 3 / 4; }
    s_vx = (s_sw - s_vpw) / 2;
    s_vy = (s_sh - s_vph) / 2;
}

void rnd_init(void)
{
    screen_layout();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glGenTextures(NFRAME, s_frame_tex);
    {
        int k;
        for (k = 0; k < NFRAME; k++) {
            glBindTexture(GL_TEXTURE_2D, s_frame_tex[k]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void rnd_set_virtual_size(int w, int h) { s_vw = w; s_vh = h; }
#define DD_HUD_MAXQ 512

/* Widescreen: the 3D of a frame covers the whole 16:9 screen.  The engine
 * projects s_ext more game pixels on each side (dx_d3d.c widens its view), so
 * game x in [-s_ext, s_vw + s_ext] maps to the full width at the 4:3 area's
 * pixel aspect.  2D (HUD, menus, frames) stays in the 4:3 area. */
static int s_wide;
static float s_ext;
void rnd_set_wide(int on)
{
    s_wide = on && s_sw * 3 > s_sh * 4;
    s_ext = s_wide ? ((float)s_vh * s_sw / s_sh - s_vw) / 2 : 0;
}
float rnd_wide_ext(void) { return s_wide ? s_ext : 0; }

/* WIDE HUD (with WIDESCREEN, menu.c): in widescreen the race HUD's side parts
 * go to the screen edges.  Only while the race HUD is up (its LAP or POSITION
 * label was drawn in the last frame: not on the results, GAME OVER...).  2D
 * areas (game coordinates) in the corners where the HUD sits: LAP + lap time
 * and SECTION LAP on the left, POSITION on the right; the condition box and
 * course map are D3D quads on the right (d3d != 0, dx_d3d.c).
 * Returns -1 left, +1 right, 0 stays. */
static int s_hud_live;
int rnd_hud_side(int x0, int y0, int x1, int y1, int d3d)
{
    if (!s_wide || !s_hud_live) return 0;
    if (d3d) {
        /* only the real pieces: the condition box (fixed place), the course
         * map (the big quad at the lower right, its size depends on the
         * course) and small icons inside one of them - the GOAL letters on
         * Desert City fell in a looser rule */
        static int mx0 = 495, my0 = 274, mx1 = 623, my1 = 480;
        int w = x1 - x0, h = y1 - y0;
        if (x0 >= 476 && x0 <= 484 && x1 >= 604 && x1 <= 612 && y0 >= 116 && y1 <= 260) return 1;   /* condition box */
        if (w >= 80 && h >= 80 && x0 >= 400 && y0 >= 260 && y0 <= 320 && x1 >= 590 && x1 <= 640 && y1 <= 490) {   /* course map (x from 451 on the city course) */
            mx0 = x0; my0 = y0; mx1 = x1; my1 = y1;
            return 1;
        }
        if (w <= 36 && h <= 36) {
            if (x0 >= 470 && x1 <= 618 && y0 >= 112 && y1 <= 264) return 1;                         /* in the box */
            if (x0 >= mx0 - 20 && x1 <= mx1 + 20 && y0 >= my0 - 20 && y1 <= my1 + 24) return 1;     /* on the map */
        }
        return 0;
    }
    if (x1 <= 192 && (y1 <= 330 || y0 >= 370)) return -1;   /* lap times: a row per lap */
    if (x0 >= 460 && y1 <= 220) return 1;   /* POSITION; time attack: TOTAL TIME, COURSE RECORD, FASTEST LAP */
    return 0;
}
static int hud_label(const short *r)        /* LAP (87,17)-(145,35) / POSITION (495,17)-(597,35) */
{
    return r[1] >= 10 && r[3] <= 40 && (r[2] <= 192 || r[0] >= 460);
}
/* rect a minus rect b: up to 4 pieces into out, returns the count */
static int rect_sub(const float *a, const float *b, float (*out)[4])
{
    int n = 0;
    float x0 = a[0], y0 = a[1], x1 = a[2], y1 = a[3];
    if (b[0] >= x1 || b[2] <= x0 || b[1] >= y1 || b[3] <= y0) { memcpy(out[0], a, sizeof out[0]); return 1; }
    if (b[1] > y0) { out[n][0] = x0; out[n][1] = y0; out[n][2] = x1; out[n][3] = b[1]; n++; y0 = b[1]; }
    if (b[3] < y1) { out[n][0] = x0; out[n][1] = b[3]; out[n][2] = x1; out[n][3] = y1; n++; y1 = b[3]; }
    if (b[0] > x0) { out[n][0] = x0; out[n][1] = y0; out[n][2] = b[0]; out[n][3] = y1; n++; }
    if (b[2] < x1) { out[n][0] = b[2]; out[n][1] = y0; out[n][2] = x1; out[n][3] = y1; n++; }
    return n;
}

/* game x -> clip x for the 3D (and the background drawn behind it) */
static float clip_x3d(float x)
{
    return s_wide ? (x + s_ext) * 2.0f / (s_vw + 2 * s_ext) - 1.0f : x * 2.0f / s_vw - 1.0f;
}

/* The Vita GPU does not clip polygons to the viewport (guard band), so the
 * scenery the game draws past the 4:3 screen edges would show in the side
 * bars: keep a scissor on the game area. */
/* viewport / scissor only when they change: vitaGL turns scissor changes
 * into extra GPU work, and the batches switch views a lot in widescreen */
static int s_cvp[4] = { -1, -1, -1, -1 }, s_csc[4] = { -1, -1, -1, -1 }, s_csc_on = -1;
static void gl_viewport(int x, int y, int w, int h)
{
    x += s_ox; y += s_oy;
    if (x == s_cvp[0] && y == s_cvp[1] && w == s_cvp[2] && h == s_cvp[3]) return;
    glViewport(x, y, w, h);
    s_cvp[0] = x; s_cvp[1] = y; s_cvp[2] = w; s_cvp[3] = h;
}
static void gl_scissor(int on, int x, int y, int w, int h)
{
    if (!on) {
        if (s_ox || s_oy) { on = 1; x = 0; y = 0; w = s_sw; h = s_sh; }   /* not over the bars */
        else {
            if (s_csc_on != 0) { glDisable(GL_SCISSOR_TEST); s_csc_on = 0; }
            return;
        }
    }
    x += s_ox; y += s_oy;
    if (s_csc_on != 1) { glEnable(GL_SCISSOR_TEST); s_csc_on = 1; }
    if (x == s_csc[0] && y == s_csc[1] && w == s_csc[2] && h == s_csc[3]) return;
    glScissor(x, y, w, h);
    s_csc[0] = x; s_csc[1] = y; s_csc[2] = w; s_csc[3] = h;
}

static void scissor_view(void)
{
    gl_scissor(1, s_vx, s_vy, s_vpw, s_vph);
}

/* where 3D goes: the 4:3 area, or the whole screen in widescreen */
static void view_3d(void)
{
    if (s_wide) {
        gl_viewport(0, 0, s_sw, s_sh);
        gl_scissor(0, 0, 0, 0, 0);          /* the whole screen: no scissor */
    } else {
        gl_viewport(s_vx, s_vy, s_vpw, s_vph);
        scissor_view();
    }
}

static void view_2d(void)
{
    gl_viewport(s_vx, s_vy, s_vpw, s_vph);
    scissor_view();
}

void rnd_flush(void);

void rnd_begin_frame(void)
{
    rnd_flush();
    {   /* the window changed size (or widescreen was switched): lay it out again */
        int w, h, ow = s_sw, oh = s_sh;
        extern int g_opt_widescreen;
        static int last_wide = -1;
        plat_screen_size(&w, &h);
        if (w != s_win_w || h != s_win_h || g_opt_widescreen != last_wide) {
            screen_layout();
            last_wide = g_opt_widescreen;
            if (s_sw != ow || s_sh != oh) rnd_set_wide(s_wide);
        }
    }
    /* the whole window (bars included) */
    if (s_csc_on != 0) { glDisable(GL_SCISSOR_TEST); s_csc_on = 0; }
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_viewport(0, 0, s_sw, s_sh);
    view_2d();
}

static void scissor_game(int x0, int y0, int x1, int y1)
{
    int sx0 = s_vx + x0 * s_vpw / s_vw, sx1 = s_vx + x1 * s_vpw / s_vw;
    int sy0 = s_vy + (s_vh - y1) * s_vph / s_vh, sy1 = s_vy + (s_vh - y0) * s_vph / s_vh;
    if (s_wide) {   /* clears reaching the 4:3 edges reach the screen edges */
        float k = s_sw / (s_vw + 2 * s_ext);
        sx0 = x0 <= 0 ? 0 : (int)((x0 + s_ext) * k);
        sx1 = x1 >= s_vw ? s_sw : (int)((x1 + s_ext) * k);
    }
    gl_scissor(1, sx0, sy0, sx1 - sx0, sy1 - sy0);
}

void rnd_clear(int color, int depth, unsigned int argb, const int *rects, int nrects)
{
    GLbitfield m = 0;
    int i;
    rnd_flush();
    if (color) {
        glClearColor(((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f, 1.0f);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) { glDepthMask(GL_TRUE); glClearDepth(1.0); m |= GL_DEPTH_BUFFER_BIT; }
    if (!m) return;
    if (!rects || nrects <= 0) {
        scissor_game(0, 0, s_vw, s_vh);
        glClear(m);
    } else {
        for (i = 0; i < nrects; i++) {
            scissor_game(rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]);
            glClear(m);
        }
    }
    view_3d();
}

static GLenum gl_cmp(int f)
{
    static const GLenum t[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return (f >= 1 && f <= 8) ? t[f] : GL_LEQUAL;
}

static GLenum gl_blend(int b)
{
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

static GLenum gl_wrap(int a)
{
    return a == 3 ? GL_CLAMP_TO_EDGE : a == 2 ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void apply_state(const RState *st)
{
    if (st->tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, st->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->filter == 2 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->filter == 2 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(st->addr_u));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(st->addr_v));
        /* DECAL(1)/COPY(5): texture only; MODULATE(2)/MODULATEALPHA(4): texture * color */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                  (st->texblend == 1 || st->texblend == 5) ? GL_REPLACE : GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    if (st->zenable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(st->zfunc));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    if (st->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(gl_blend(st->srcblend), gl_blend(st->dstblend));
    } else {
        glDisable(GL_BLEND);
    }
    if (st->alphatest || (st->tex && st->tex_alpha)) {
        glEnable(GL_ALPHA_TEST);
        if (st->alphatest) glAlphaFunc(gl_cmp(st->alphafunc), st->alpharef / 255.0f);
        else glAlphaFunc(GL_GREATER, 0.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("DAYTONA_NOCULL") != NULL;
    if (!nocull && (st->cull == 2 || st->cull == 3)) {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glCullFace(st->cull == 3 ? GL_BACK : GL_FRONT);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glShadeModel(st->shade == 1 ? GL_FLAT : GL_SMOOTH);
}

/* D3D TL vertices -> clip space (see the top of the file), at s_pos[base..] */
static void put_verts(int base, const RVertex *v, int n, const RState *st, const float *uvk)
{
    static const float ident[4] = { 1, 0, 1, 0 };
    if (!uvk) uvk = ident;
    float sx = 2.0f / s_vw, sy = 2.0f / s_vh;
    int i;
    for (i = 0; i < n; i++) {
        float w = v[i].rhw > 1e-8f ? 1.0f / v[i].rhw : 1.0f;
        unsigned int c = v[i].color;
        float z = v[i].z;
        float *p = s_pos + (base + i) * 4;
        unsigned char *col = s_col + (base + i) * 4;
        if (z < 0) z = 0; else if (z > 1) z = 1;
        p[0] = (s_wide ? clip_x3d(v[i].x) : v[i].x * sx - 1.0f) * w;
        p[1] = (1.0f - v[i].y * sy) * w;
        p[2] = (z * 2.0f - 1.0f) * w;
        p[3] = w;
        s_uv[(base + i) * 2 + 0] = v[i].u * uvk[0] + uvk[1];
        s_uv[(base + i) * 2 + 1] = v[i].v * uvk[2] + uvk[3];
        if (st->fog) {
            /* vertex fog: specular alpha 255 = no fog, 0 = full fog (textured
             * polygons too: darkened toward the fog colour, no fog pass) */
            unsigned int f = v[i].specular >> 24, nf = 255 - f;
            unsigned int fr = (st->fogcolor >> 16) & 255, fg = (st->fogcolor >> 8) & 255, fb = st->fogcolor & 255;
            col[0] = (unsigned char)((((c >> 16) & 255) * f + fr * nf) / 255);
            col[1] = (unsigned char)((((c >> 8) & 255) * f + fg * nf) / 255);
            col[2] = (unsigned char)(((c & 255) * f + fb * nf) / 255);
        } else {
            col[0] = (unsigned char)(c >> 16);
            col[1] = (unsigned char)(c >> 8);
            col[2] = (unsigned char)c;
        }
        col[3] = (unsigned char)(c >> 24);
    }
}

static void gl_arrays(int tex)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, s_pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, s_col);
    if (tex) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, s_uv);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
}

static void gl_arrays_off(void)
{
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Batching: the game draws ~300 small quads a frame and every GL draw costs
 * state setup plus a vertex copy in vitaGL.  Triangles with the same state
 * collect here (as an indexed list) and go out in one glDrawElements. */
static RState s_bst;
static int s_bn, s_bni;                       /* vertices / indices in the batch */
static unsigned short s_bidx[MAXV * 3];

/* DAYTONA_FLUSHSTATS=1: draw calls per frame and why batches broke (debug) */
static int s_fs_on = -1, s_fs_calls, s_fs_tex, s_fs_other, s_fs_tris, s_fs_frames, s_fs_uvout, s_fs_verts;
static void flush_stats_frame(void)
{
    if (s_fs_on <= 0) return;
    if (++s_fs_frames % 60 == 0)
        rt_log("flushstats: %d calls/frame (tex change %d, other state %d), %d tris/call, uv outside 0..1 %d%%\n",
               s_fs_calls / 60, s_fs_tex / 60, s_fs_other / 60, s_fs_calls ? s_fs_tris / s_fs_calls : 0,
               s_fs_verts ? s_fs_uvout * 100 / s_fs_verts : 0);
    if (s_fs_frames % 60 == 0) s_fs_calls = s_fs_tex = s_fs_other = s_fs_tris = s_fs_uvout = s_fs_verts = 0;
}
void rnd_flush(void)
{
    if (!s_bni) return;
    if (s_fs_on < 0) s_fs_on = getenv("DAYTONA_FLUSHSTATS") != NULL;
    if (s_fs_on) { s_fs_calls++; s_fs_tris += s_bni / 3; }
    apply_state(&s_bst);
    view_3d();
    gl_arrays(s_bst.tex != 0);
    glDrawElements(GL_TRIANGLES, s_bni, GL_UNSIGNED_SHORT, s_bidx);
    gl_arrays_off();
    s_bn = s_bni = 0;
}


/* ---- texture atlas --------------------------------------------------------
 * In a race the game sorts its polygons back to front and nearly every one
 * uses another texture: 400-800 GL draws of 2-3 triangles a frame, each one
 * expensive in vitaGL (~a third of the frame on the Vita).  The D3D textures
 * (all <= 256x256, UVs within 0..1) are also copied into one big texture, with
 * a 2-pixel border repeating the edges (filtering), and draws are batched
 * across textures.  DAYTONA_NOATLAS=1 turns it off. */
#define ATL_W 2048
#define ATL_H 2048
#define ATL_G 2
#define ATL_MAXNAME 16384
#define ATL_MAXSHELF 64
#define ATL_MAXFREE 512
static GLuint s_atl_tex;
static int s_atl_on = -1, s_atl_stride;
static struct { short x, y, w, h; } s_atl[ATL_MAXNAME];      /* by GL name; w 0 = not in it */
static struct { int y, h, x; } s_shelf[ATL_MAXSHELF];
static int s_nshelf, s_shelf_top;
static struct { short x, y, w, h; } s_afree[ATL_MAXFREE];
static int s_nafree;
#if !defined(__vita__)
static unsigned int *s_atl_cpu;
#endif

static int atlas_init(void)
{
    if (s_atl_on >= 0) return s_atl_on;
    s_atl_on = getenv("DAYTONA_NOATLAS") == NULL;
    if (!s_atl_on) return 0;
    glGenTextures(1, &s_atl_tex);
    glBindTexture(GL_TEXTURE_2D, s_atl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATL_W, ATL_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#if defined(__vita__)
    {
        SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
        unsigned int st = gt && sceGxmTextureGetType(gt) == SCE_GXM_TEXTURE_LINEAR_STRIDED ? sceGxmTextureGetStride(gt) : 0;
        s_atl_stride = st ? (int)(st / 4) : ((ATL_W + 7) & ~7);
        if (!vglGetTexDataPointer(GL_TEXTURE_2D)) { rt_log("render: no texture atlas\n"); s_atl_on = 0; }
    }
#else
    s_atl_stride = ATL_W;
    s_atl_cpu = (unsigned int *)calloc((size_t)ATL_W * ATL_H, 4);
    if (!s_atl_cpu) s_atl_on = 0;
#endif
    return s_atl_on;
}

/* a free area of (w + 2G) x (h + 2G): a freed one of the same size, else shelves */
static int atlas_alloc(int w, int h, int *ox, int *oy)
{
    int i, cw = w + 2 * ATL_G, ch = h + 2 * ATL_G;
    for (i = 0; i < s_nafree; i++)
        if (s_afree[i].w == w && s_afree[i].h == h) {
            *ox = s_afree[i].x; *oy = s_afree[i].y;
            s_afree[i] = s_afree[--s_nafree];
            return 1;
        }
    for (i = 0; i < s_nshelf; i++)
        if (s_shelf[i].h == ch && s_shelf[i].x + cw <= ATL_W) {
            *ox = s_shelf[i].x; *oy = s_shelf[i].y; s_shelf[i].x += cw;
            return 1;
        }
    if (s_nshelf < ATL_MAXSHELF && s_shelf_top + ch <= ATL_H && cw <= ATL_W) {
        s_shelf[s_nshelf].y = s_shelf_top; s_shelf[s_nshelf].h = ch; s_shelf[s_nshelf].x = cw;
        *ox = 0; *oy = s_shelf_top;
        s_shelf_top += ch;
        s_nshelf++;
        return 1;
    }
    return 0;
}

static void atlas_release(unsigned int tex)
{
    if (tex >= ATL_MAXNAME || !s_atl[tex].w) return;
    if (s_nafree < ATL_MAXFREE) {
        s_afree[s_nafree].x = s_atl[tex].x; s_afree[s_nafree].y = s_atl[tex].y;
        s_afree[s_nafree].w = s_atl[tex].w; s_afree[s_nafree].h = s_atl[tex].h;
        s_nafree++;
    }
    s_atl[tex].w = 0;
}

static void atlas_put(unsigned int tex, int w, int h, const unsigned int *rgba)
{
    int x, y, ax, ay, cw = w + 2 * ATL_G, ch = h + 2 * ATL_G;
    unsigned int *dst;
    if (tex >= ATL_MAXNAME || w > 256 || h > 256 || w < 1 || h < 1 || !atlas_init()) return;
    if (s_atl[tex].w && (s_atl[tex].w != w || s_atl[tex].h != h)) atlas_release(tex);
    if (!s_atl[tex].w) {
        if (!atlas_alloc(w, h, &ax, &ay)) {
            static int told;
            if (!told++) rt_log("render: texture atlas full (%dx%d not added)\n", w, h);
            return;
        }
        s_atl[tex].x = (short)ax; s_atl[tex].y = (short)ay; s_atl[tex].w = (short)w; s_atl[tex].h = (short)h;
    }
    ax = s_atl[tex].x; ay = s_atl[tex].y;
#if defined(__vita__)
    glBindTexture(GL_TEXTURE_2D, s_atl_tex);
    dst = (unsigned int *)vglGetTexDataPointer(GL_TEXTURE_2D);
    if (!dst) return;
#else
    dst = s_atl_cpu;
#endif
    for (y = 0; y < ch; y++) {           /* rows with the edges repeated into the border */
        int sy = y - ATL_G;
        const unsigned int *src;
        unsigned int *o = dst + (size_t)(ay + y) * s_atl_stride + ax;
        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        src = rgba + (size_t)sy * w;
        for (x = 0; x < ATL_G; x++) { o[x] = src[0]; o[ATL_G + w + x] = src[w - 1]; }
        memcpy(o + ATL_G, src, (size_t)w * 4);
    }
#if !defined(__vita__)
    glBindTexture(GL_TEXTURE_2D, s_atl_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, ATL_W);
    glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, s_atl_cpu + (size_t)ay * ATL_W + ax);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
}

/* the atlas place of a draw's texture: UV scale/offset, or 0 */
static int atlas_map(const RState *st, const RVertex *v, int n, float *k)
{
    int i;
    unsigned int t = st->tex;
    if (!t || t >= ATL_MAXNAME || !s_atl[t].w || s_atl_on <= 0) return 0;
    for (i = 0; i < n; i++)
        if (v[i].u < -0.001f || v[i].u > 1.001f || v[i].v < -0.001f || v[i].v > 1.001f) return 0;
    k[0] = (float)s_atl[t].w / ATL_W; k[1] = (float)(s_atl[t].x + ATL_G) / ATL_W;
    k[2] = (float)s_atl[t].h / ATL_H; k[3] = (float)(s_atl[t].y + ATL_G) / ATL_H;
    return 1;
}

void rnd_draw(int prim, const RVertex *v, int n, const unsigned short *idx, int nidx, const RState *st)
{
    int i, count = idx ? nidx : n, ntri, base;
    const float *uvk = NULL;
    PROF_PHASE(PH_DRAW);
    if (count <= 0 || n <= 0) { PROF_END(); return; }
    {   /* DAYTONA_NO3D=1: no 3D polygons (only the 2D and the background; debug) */
        static int no3d = -1;
        if (no3d < 0) { const char *e = getenv("DAYTONA_NO3D"); no3d = e && *e == '1'; }
        if (no3d) { PROF_END(); return; }
    }
    if (n > MAXV) n = MAXV;
    if (prim == RP_TRIANGLES) ntri = count / 3;
    else if (prim == RP_TRISTRIP || prim == RP_TRIFAN) ntri = count - 2;
    else ntri = -1;
    if (ntri < 0) {
        /* points / lines: not batched */
        GLenum mode = prim == RP_POINTS ? GL_POINTS : prim == RP_LINES ? GL_LINES : GL_LINE_STRIP;
        rnd_flush();
        put_verts(0, v, n, st, NULL);
        apply_state(st);
        view_3d();
        gl_arrays(st->tex != 0);
        if (idx) glDrawElements(mode, nidx, GL_UNSIGNED_SHORT, idx);
        else glDrawArrays(mode, 0, n);
        gl_arrays_off();
        PROF_END();
        return;
    }
    if (ntri <= 0) { PROF_END(); return; }
    {   /* texture atlas: same state with the atlas texture instead */
        static RState sa;
        static float k[4];
        if (atlas_map(st, v, n, k)) {
            sa = *st;
            sa.tex = s_atl_tex;
            sa.addr_u = sa.addr_v = 3;
            st = &sa;
            uvk = k;
        }
    }
    if (s_fs_on > 0) {
        int k;
        if (s_bni && memcmp(st, &s_bst, sizeof *st) != 0) {
            RState t = *st;
            t.tex = s_bst.tex; t.tex_alpha = s_bst.tex_alpha;
            if (memcmp(&t, &s_bst, sizeof t) == 0) s_fs_tex++; else s_fs_other++;
        }
        if (st->tex) for (k = 0; k < n; k++) { s_fs_verts++; if (v[k].u < -0.01f || v[k].u > 1.01f || v[k].v < -0.01f || v[k].v > 1.01f) s_fs_uvout++; }
    }
    if (s_bni && (memcmp(st, &s_bst, sizeof *st) != 0 || s_bn + n > MAXV || s_bni + ntri * 3 > MAXV * 3))
        rnd_flush();
    if (!s_bni) s_bst = *st;
    base = s_bn;
    put_verts(base, v, n, st, uvk);
    /* same winding and same last (flat-shading) vertex as GL fans/strips */
#define VI(k) (unsigned short)(base + (idx ? idx[k] : (k)))
    for (i = 0; i < ntri; i++) {
        unsigned short *o = s_bidx + s_bni + i * 3;
        if (prim == RP_TRIANGLES) { o[0] = VI(i * 3); o[1] = VI(i * 3 + 1); o[2] = VI(i * 3 + 2); }
        else if (prim == RP_TRIFAN) { o[0] = VI(0); o[1] = VI(i + 1); o[2] = VI(i + 2); }
        else if (i & 1) { o[0] = VI(i + 1); o[1] = VI(i); o[2] = VI(i + 2); }
        else { o[0] = VI(i); o[1] = VI(i + 1); o[2] = VI(i + 2); }
    }
#undef VI
    s_bn += n;
    s_bni += ntri * 3;
    PROF_END();
}

unsigned int rnd_texture_new(void)
{
    GLuint t;
    glGenTextures(1, &t);
    return t;
}

void rnd_texture_upload(unsigned int tex, int w, int h, const unsigned int *rgba)
{
    rnd_flush();                 /* batched draws use the old contents */
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

/* a D3D texture: also into the atlas */
void rnd_texture_upload_3d(unsigned int tex, int w, int h, const unsigned int *rgba)
{
    rnd_texture_upload(tex, w, h, rgba);
    atlas_put(tex, w, h, rgba);
}

void rnd_texture_free(unsigned int tex)
{
    rnd_flush();
    atlas_release(tex);
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

/* ---- 2D frame (DirectDraw back buffer) ------------------------------------
 * RGB565 -> RGBA8888 through two 256-entry tables (low/high byte) */
static unsigned int s_lo_t[256], s_hi_t[256];
static void conv_tables(void)
{
    static int init;
    int b;
    if (init) return;
    for (b = 0; b < 256; b++) {
        unsigned int c = (unsigned int)b, R, G, B;
        G = (c >> 5) << 2; B = (c & 31) << 3;                    /* low byte: G bits 0-2, B */
        s_lo_t[b] = (G << 8) | ((B | (B >> 5)) << 16);
        R = (c >> 3) << 3; G = ((c & 7) << 5) | ((c & 7) >> 1);  /* high byte: R, G bits 3-5 */
        s_hi_t[b] = (R | (R >> 5)) | (G << 8);
    }
    init = 1;
}

/* write columns/rows [x0,x1) x [y0,y1) of the frame into a texture image.
 * fmt16: RGB565 (background, a plain copy) or RGBA5551 (overlay); else RGBA8888 */
static void frame_put(void *dst, int dstride, int fmt16, int overlay, const unsigned short *px, int pitch_px,
                      int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = y0; y < y1; y++) {
        const unsigned short *r = px + y * pitch_px;
        if (fmt16) {
            unsigned short *o = (unsigned short *)dst + y * dstride;
            if (!overlay) {
                memcpy(o + x0, r + x0, (size_t)(x1 - x0) * 2);
            } else {
                for (x = x0; x < x1; x++) {
                    unsigned int c = r[x];
                    /* 565 -> 5551: R and the top 5 bits of G stay, B moves up one */
                    o[x] = c == RND_OVERLAY_EMPTY ? 0 : (unsigned short)((c & 0xffc0u) | ((c & 0x1fu) << 1) | 1u);
                }
            }
        } else {
            unsigned int *o = (unsigned int *)dst + y * dstride;
            if (overlay) {
                for (x = x0; x < x1; x++) {
                    unsigned int c = r[x];
                    o[x] = c == RND_OVERLAY_EMPTY ? 0u : (s_lo_t[c & 255] | s_hi_t[c >> 8] | 0xff000000u);
                }
            } else {
                for (x = x0; x < x1; x++) {
                    unsigned int c = r[x];
                    o[x] = s_lo_t[c & 255] | s_hi_t[c >> 8] | 0xff000000u;
                }
            }
        }
    }
}

static void frame_clear(void *dst, int dstride, int fmt16, int x0, int y0, int x1, int y1)
{
    int y, bpp = fmt16 ? 2 : 4;
    for (y = y0; y < y1; y++)
        memset((unsigned char *)dst + ((size_t)y * dstride + x0) * bpp, 0, (size_t)(x1 - x0) * bpp);
}

/* per texture: pixel format and the areas holding overlay pixels */
static int s_frame_fmt16[NFRAME], s_frame_nrect[NFRAME];
static short s_frame_rect[NFRAME][256][4];   /* DD_MAXRECT */

#if defined(__vita__)
static int s_try16[2] = { 1, 1 };

/* (re)create a frame texture; 16-bit when vitaGL gives the expected GXM
 * format (checked, so a different mapping falls back to RGBA8888) */
static void frame_tex_create(int cur, int set, int w, int h)
{
    int ok16 = 0;
    if (s_try16[set]) {
        if (set == 0) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
        {
            SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
            unsigned int want = set == 0 ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB : SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA;
            ok16 = gt && vglGetTexDataPointer(GL_TEXTURE_2D) && (unsigned int)sceGxmTextureGetFormat(gt) == want;
        }
        if (!ok16) {
            void rt_log(const char *fmt, ...);
            rt_log("render: 16-bit %s frame texture unavailable, using RGBA8888\n", set ? "overlay" : "background");
            s_try16[set] = 0;
        }
    }
    if (!ok16) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    s_frame_fmt16[cur] = ok16;
    s_frame_w[cur] = w; s_frame_h[cur] = h;
    s_frame_nrect[cur] = -1;                   /* contents unknown: clear all on first use */
}

#else
/* host: a CPU copy of each frame texture, uploaded whole (same logic as the
 * Vita's direct writes, so the area tracking is tested here too) */
static unsigned int *s_host_buf[NFRAME];
#endif

static int rect_inside(const short *a, const short (*rs)[4], int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[0] >= rs[i][0] && a[1] >= rs[i][1] && a[2] <= rs[i][2] && a[3] <= rs[i][3]) return 1;
    return 0;
}

void rnd_draw_frame16(const unsigned short *px, int w, int h, int pitch_px, int overlay,
                      const short (*rects)[4], int nrect)
{
    static const float uv0[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    float pos[8] = { -1, 1, 1, 1, -1, -1, 1, -1 };
    float uv[8];
    int i, cur, set = overlay ? 1 : 0;
    PROF_PHASE(PH_FRAME2D);
    rnd_flush();
    if (w > 1024 || h > 512) { PROF_END(); return; }
    conv_tables();
    s_frame_next[set] = (s_frame_next[set] + 1) % FRAME_RING;
    cur = set * FRAME_RING + s_frame_next[set];
    glBindTexture(GL_TEXTURE_2D, s_frame_tex[cur]);
    {
        void *dst;
        int dstride, fmt16;
#if defined(__vita__)
        /* write straight into the texture memory (vitaGL's glTexSubImage2D
         * waited for the GPU and converted pixel by pixel: ~35 ms per frame).
         * The rings keep the GPU from reading a texture while it is written. */
        if (s_frame_w[cur] != w || s_frame_h[cur] != h) frame_tex_create(cur, set, w, h);
        dst = vglGetTexDataPointer(GL_TEXTURE_2D);
        fmt16 = s_frame_fmt16[cur];
        {
            /* GetStride only applies to LINEAR_STRIDED textures; a plain
             * linear texture's rows are the width rounded up to 8 pixels */
            SceGxmTexture *gt = vglGetGxmTexture(GL_TEXTURE_2D);
            unsigned int st = gt && sceGxmTextureGetType(gt) == SCE_GXM_TEXTURE_LINEAR_STRIDED ? sceGxmTextureGetStride(gt) : 0;
            dstride = st ? (int)(st / (fmt16 ? 2 : 4)) : ((w + 7) & ~7);
        }
#else
        int fresh = s_frame_w[cur] != w || s_frame_h[cur] != h;
        if (fresh) {
            free(s_host_buf[cur]);
            s_host_buf[cur] = (unsigned int *)malloc((size_t)w * h * 4);
            s_frame_nrect[cur] = -1;
        }
        dst = s_host_buf[cur];
        fmt16 = 0;
        dstride = w;
#endif
        if (!dst) { PROF_END(); return; }
        if (overlay && nrect >= 0) {
            /* only the areas the game drew into hold pixels: clear what this
             * texture held last time (unless redrawn now), convert the new areas */
            int k, pn = s_frame_nrect[cur];
            if (pn < 0) frame_clear(dst, dstride, fmt16, 0, 0, w, h);
            else for (k = 0; k < pn; k++)
                if (!rect_inside(s_frame_rect[cur][k], rects, nrect))
                    frame_clear(dst, dstride, fmt16, s_frame_rect[cur][k][0], s_frame_rect[cur][k][1],
                                s_frame_rect[cur][k][2], s_frame_rect[cur][k][3]);
            for (k = 0; k < nrect; k++) {
                int x0 = rects[k][0], y0 = rects[k][1], x1 = rects[k][2], y1 = rects[k][3];
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                if (x0 < x1 && y0 < y1) frame_put(dst, dstride, fmt16, 1, px, pitch_px, x0, y0, x1, y1);
                memcpy(s_frame_rect[cur][k], rects[k], sizeof rects[k]);
            }
            s_frame_nrect[cur] = nrect;
        } else {
            frame_put(dst, dstride, fmt16, overlay, px, pitch_px, 0, 0, w, h);
            s_frame_nrect[cur] = -1;
        }
#if !defined(__vita__)
        if (fresh) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst);
            s_frame_w[cur] = w; s_frame_h[cur] = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        }
#endif
    }
    for (i = 0; i < 8; i++) uv[i] = uv0[i];
    if (overlay) {
        /* held for 10 frames: a frame drawn without the labels or with an
         * unknown area list must not send the HUD back for a frame */
        static int miss = 100;
        int k, live = 0;
        for (k = 0; k < nrect; k++) if (hud_label(rects[k])) live = 1;
        if (live) miss = 0;
        else if (nrect >= 0 && miss < 100) miss++;
        s_hud_live = miss <= 10;
    }
    if (overlay && s_wide && nrect > 0) {
        /* wide HUD: each area the game drew as its own quad, the HUD's side
         * parts moved out; the other areas leave out the moved ones (a big
         * sprite over the HUD, TIME EXTENSION) so no pixel shows twice */
        int k, side = 0, np = 0, j, m;
        static float piece[DD_HUD_MAXQ][5];
        for (k = 0; k < nrect; k++) if (rnd_hud_side(rects[k][0], rects[k][1], rects[k][2], rects[k][3], 0)) side = 1;
        for (k = 0; k < nrect && np < DD_HUD_MAXQ; k++) {
            float a[4], cur[16][4], nxt[16][4], tmp[4][4];
            int nc = 1, sd = rnd_hud_side(rects[k][0], rects[k][1], rects[k][2], rects[k][3], 0);
            a[0] = rects[k][0]; a[1] = rects[k][1]; a[2] = rects[k][2]; a[3] = rects[k][3];
            memcpy(cur[0], a, sizeof a);
            if (!sd)
                for (j = 0; j < nrect && nc; j++) {
                    float b[4];
                    int nn = 0, q;
                    if (!rnd_hud_side(rects[j][0], rects[j][1], rects[j][2], rects[j][3], 0)) continue;
                    b[0] = rects[j][0]; b[1] = rects[j][1]; b[2] = rects[j][2]; b[3] = rects[j][3];
                    for (m = 0; m < nc; m++) {
                        int c = rect_sub(cur[m], b, tmp);
                        for (q = 0; q < c && nn < 16; q++) memcpy(nxt[nn++], tmp[q], sizeof tmp[q]);
                    }
                    memcpy(cur, nxt, sizeof(float) * 4 * nn);
                    nc = nn;
                }
            for (m = 0; m < nc && np < DD_HUD_MAXQ; m++) {
                memcpy(piece[np], cur[m], sizeof cur[m]);
                piece[np++][4] = (float)sd;
            }
        }
        if (side) {
            static float qp[DD_HUD_MAXQ * 12], qt[DD_HUD_MAXQ * 12];
            int nq = 0;
            for (k = 0; k < np && nq < DD_HUD_MAXQ; k++) {
                float x0 = piece[k][0], y0 = piece[k][1], x1 = piece[k][2], y1 = piece[k][3];
                float sh = piece[k][4] * s_ext;
                float px0 = clip_x3d(x0 + sh), px1 = clip_x3d(x1 + sh);
                float py0 = 1.0f - y0 * 2.0f / s_vh, py1 = 1.0f - y1 * 2.0f / s_vh;
                float u0 = x0 / w, u1 = x1 / w, v0 = y0 / h, v1 = y1 / h;
                float *P = qp + nq * 12, *T = qt + nq * 12;
                if (x1 <= x0 || y1 <= y0) continue;
                P[0] = px0; P[1] = py0; P[2] = px1; P[3] = py0; P[4] = px0; P[5] = py1;
                P[6] = px1; P[7] = py0; P[8] = px1; P[9] = py1; P[10] = px0; P[11] = py1;
                T[0] = u0; T[1] = v0; T[2] = u1; T[3] = v0; T[4] = u0; T[5] = v1;
                T[6] = u1; T[7] = v0; T[8] = u1; T[9] = v1; T[10] = u0; T[11] = v1;
                nq++;
            }
            view_3d();
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_FOG);
            glEnable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.5f);
            glDisable(GL_BLEND);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, qp);
            glTexCoordPointer(2, GL_FLOAT, 0, qt);
            glDrawArrays(GL_TRIANGLES, 0, nq * 6);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisable(GL_ALPHA_TEST);
            glDepthMask(GL_TRUE);
            PROF_END();
            return;
        }
    }
    view_2d();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    if (overlay) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    glDisable(GL_BLEND);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_ALPHA_TEST);
    glDepthMask(GL_TRUE);
    PROF_END();
}

void rnd_texture_upload16(unsigned int tex, int w, int h, const unsigned short *px565, int pitch_px)
{
    unsigned int *buf = (unsigned int *)malloc((size_t)w * h * 4);
    if (!buf) return;
    conv_tables();
    frame_put(buf, w, 0, 0, px565, pitch_px, 0, 0, w, h);
    rnd_texture_upload(tex, w, h, buf);
    free(buf);
}

void rnd_texture_upload16_key(unsigned int tex, int w, int h, const unsigned short *px565, int pitch_px,
                              unsigned int klo, unsigned int khi)
{
    unsigned int *buf = (unsigned int *)malloc((size_t)w * h * 4);
    int x, y;
    if (!buf) return;
    conv_tables();
    frame_put(buf, w, 0, 0, px565, pitch_px, 0, 0, w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned int p = px565[y * pitch_px + x];
            if (p >= klo && p <= khi) buf[y * w + x] = 0;
        }
    rnd_texture_upload(tex, w, h, buf);
    free(buf);
}

static int s_blit_key;
/* a background copy with a colour key (the mountains over the sky) */
void rnd_blit2d_key(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1)
{
    s_blit_key = 1;
    rnd_blit2d(tex, tw, th, sx0, sy0, dx0, dy0, dx1, dy1);
    s_blit_key = 0;
}

static int s_blit_mirror, s_blit_vmirror;
/* any of the variants: key = colour key transparency, hmir / vmir = source
 * columns / rows running backwards from sx0 / sy0 */
void rnd_blit2d_ex(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1,
                   int key, int hmir, int vmir)
{
    s_blit_key = key; s_blit_mirror = hmir; s_blit_vmirror = vmir;
    rnd_blit2d(tex, tw, th, sx0, sy0, dx0, dy0, dx1, dy1);
    s_blit_key = s_blit_mirror = s_blit_vmirror = 0;
}
/* rows running upwards from sy0 (the panorama continued below its last row) */
void rnd_blit2d_vmirror(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, int key)
{
    s_blit_vmirror = 1; s_blit_key = key;
    rnd_blit2d(tex, tw, th, sx0, sy0, dx0, dy0, dx1, dy1);
    s_blit_vmirror = 0; s_blit_key = 0;
}
/* the same copy with the source columns running backwards (sx0 = the column at dx0) */
void rnd_blit2d_mirror(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, int key)
{
    s_blit_mirror = 1; s_blit_key = key;
    rnd_blit2d(tex, tw, th, sx0, sy0, dx0, dy0, dx1, dy1);
    s_blit_mirror = 0; s_blit_key = 0;
}

void rnd_blit2d(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1)
{
    float pos[8], uv[8];
    float x0 = clip_x3d((float)dx0), x1 = clip_x3d((float)dx1);
    float y0 = 1.0f - dy0 * 2.0f / s_vh, y1 = 1.0f - dy1 * 2.0f / s_vh;
    float u0 = (float)sx0 / tw, u1 = (float)(s_blit_mirror ? sx0 - (dx1 - dx0) : sx0 + dx1 - dx0) / tw;
    float v0 = (float)sy0 / th, v1 = (float)(s_blit_vmirror ? sy0 - (dy1 - dy0) : sy0 + dy1 - dy0) / th;
    PROF_PHASE(PH_FRAME2D);
    rnd_flush();
    pos[0] = x0; pos[1] = y0; pos[2] = x1; pos[3] = y0; pos[4] = x0; pos[5] = y1; pos[6] = x1; pos[7] = y1;
    uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0; uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
    view_3d();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    if (s_blit_key) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
    else glDisable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* keyed: nearest, so the transparent texels leave no dark fringe */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_blit_key ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_blit_key ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_ALPHA_TEST);
    glDepthMask(GL_TRUE);
    PROF_END();
}

/* a blended 2D quad in game coordinates (the Select menu): textured when tex
 * is nonzero (modulated by argb), else a plain colour */
/* the black bars beside the 4:3 area, in game pixels per side (0: none) -
 * whatever the game draws, from the screen's shape */
float rnd_border_ext(void)
{
    return s_sw * 3 > s_sh * 4 ? ((float)s_vh * s_sw / s_sh - s_vw) / 2 : 0;
}

static int s_q2d_screen;
void rnd_quad2d(unsigned int tex, int tw, int th, float sx0, float sy0, float sx1, float sy1,
                float dx0, float dy0, float dx1, float dy1, unsigned int argb);
/* rnd_quad2d over the whole screen: game x from -rnd_border_ext() to 640 + it */
void rnd_quad2d_screen(unsigned int tex, int tw, int th, float sx0, float sy0, float sx1, float sy1,
                       float dx0, float dy0, float dx1, float dy1, unsigned int argb)
{
    s_q2d_screen = 1;
    rnd_quad2d(tex, tw, th, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, argb);
    s_q2d_screen = 0;
}

void rnd_quad2d(unsigned int tex, int tw, int th, float sx0, float sy0, float sx1, float sy1,
                float dx0, float dy0, float dx1, float dy1, unsigned int argb)
{
    float pos[8], uv[8];
    unsigned char col[16];
    float x0 = dx0 * 2.0f / s_vw - 1.0f, x1 = dx1 * 2.0f / s_vw - 1.0f;
    float y0 = 1.0f - dy0 * 2.0f / s_vh, y1 = 1.0f - dy1 * 2.0f / s_vh;
    int i;
    rnd_flush();
    if (s_q2d_screen) {
        float e = rnd_border_ext();
        x0 = (dx0 + e) * 2.0f / (s_vw + 2 * e) - 1.0f;
        x1 = (dx1 + e) * 2.0f / (s_vw + 2 * e) - 1.0f;
    }
    pos[0] = x0; pos[1] = y0; pos[2] = x1; pos[3] = y0; pos[4] = x0; pos[5] = y1; pos[6] = x1; pos[7] = y1;
    if (tex) {
        float u0 = sx0 / tw, u1 = sx1 / tw, v0 = sy0 / th, v1 = sy1 / th;
        uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0; uv[4] = u0; uv[5] = v1; uv[6] = u1; uv[7] = v1;
    }
    for (i = 0; i < 4; i++) {
        col[i * 4] = (unsigned char)(argb >> 16); col[i * 4 + 1] = (unsigned char)(argb >> 8);
        col[i * 4 + 2] = (unsigned char)argb; col[i * 4 + 3] = (unsigned char)(argb >> 24);
    }
    if (s_q2d_screen) { gl_viewport(0, 0, s_sw, s_sh); gl_scissor(0, 0, 0, 0, 0); }
    else view_2d();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_ALPHA_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

/* the 3D drawn so far this frame, as RGB565 at game resolution, under the 2D
 * already in dst: pixels equal to `empty` take the 3D (fades and wipes read
 * the screen back; the 3D only exists on the GPU) */
int rnd_read_frame565(unsigned short *dst, int w, int h, int pitch_px, unsigned short empty)
{
    unsigned char *rgba;
    int x, y;
    rnd_flush();
    rgba = (unsigned char *)malloc((size_t)s_vpw * s_vph * 4);
    if (!rgba) return 0;
    glReadPixels(s_ox + s_vx, s_oy + s_vy, s_vpw, s_vph, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (y = 0; y < h; y++) {
        int sy = s_vph - 1 - y * s_vph / h;                      /* GL rows go up */
        const unsigned char *src = rgba + (size_t)sy * s_vpw * 4;
        unsigned short *o = dst + y * pitch_px;
        for (x = 0; x < w; x++) {
            const unsigned char *p = src + (x * s_vpw / w) * 4;
            if (o[x] == empty)
                o[x] = (unsigned short)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    free(rgba);
    return 1;
}

/* widescreen: darken the parts of the 3D outside the 4:3 area */
void rnd_dim_sides(float a)
{
    float pos[16];
    int i;
    if (!s_wide || a <= 0.01f) return;
    if (a > 1) a = 1;
    rnd_flush();
    {
        /* 2 screen pixels into the 4:3 area: no seam of undimmed 3D between
         * the sides and the faded picture (a vertical line on the Vita) */
        float px = 4.0f / s_sw;
        float xl0 = clip_x3d(-s_ext), xl1 = clip_x3d(0) + px, xr0 = clip_x3d((float)s_vw) - px, xr1 = clip_x3d(s_vw + s_ext);
        float q[16] = { xl0, 1, xl1, 1, xl0, -1, xl1, -1, xr0, 1, xr1, 1, xr0, -1, xr1, -1 };
        for (i = 0; i < 16; i++) pos[i] = q[i];
    }
    view_3d();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, a);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1, 1, 1, 1);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void rnd_fill2d(int x0, int y0, int x1, int y1, unsigned int argb)
{
    int r[4];
    r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
    rnd_clear(1, 0, argb, r, 1);
}

void rnd_swap(void)
{
    rnd_flush();
    flush_stats_frame();
    PROF_PHASE(PH_SWAP);
    gl_scissor(0, 0, 0, 0, 0);
    plat_swap();
    PROF_END();
    view_2d();
}
