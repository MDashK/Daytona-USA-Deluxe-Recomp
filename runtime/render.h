/* render.h — the GL backend behind Direct3D and the DirectDraw presenter.
 * Plain C types only (render.c includes the GL headers). */
#ifndef RENDER_H
#define RENDER_H

typedef struct RVertex { float x, y, z, rhw; unsigned int color, specular; float u, v; } RVertex;  /* D3DTLVERTEX */

enum { RP_POINTS = 1, RP_LINES, RP_LINESTRIP, RP_TRIANGLES, RP_TRISTRIP, RP_TRIFAN };

typedef struct RState {
    unsigned int tex;            /* GL texture name, 0 = untextured */
    int tex_alpha;               /* texture has alpha (color key / alpha format) */
    int zenable, zwrite, zfunc;  /* D3DCMPFUNC */
    int blend, srcblend, dstblend;
    int alphatest, alpharef, alphafunc;
    int cull;                    /* D3DCULL */
    int shade;                   /* 1 flat 2 gouraud */
    int texblend;                /* D3DTEXTUREBLEND */
    int filter;                  /* 1 nearest 2 linear */
    int addr_u, addr_v;          /* 1 wrap 2 mirror 3 clamp */
    int fog;                     /* fog enabled (vertex fog in specular alpha) */
    unsigned int fogcolor;
    int specular;
} RState;

void rnd_init(void);
void rnd_set_virtual_size(int w, int h);          /* game resolution (640x480 / 320x240) */
void rnd_set_wide(int on);                        /* this frame's 3D over the whole 16:9 screen */
float rnd_wide_ext(void);
int rnd_hud_side(int x0, int y0, int x1, int y1, int d3d);   /* wide HUD: -1 left, +1 right, 0 stays */                         /* game pixels added on each side, 0 = 4:3 */
void rnd_begin_frame(void);
void rnd_clear(int color, int depth, unsigned int argb, const int *rects, int nrects);
void rnd_draw(int prim, const RVertex *v, int n, const unsigned short *idx, int nidx, const RState *st);
void rnd_flush(void);                             /* draw the batched triangles now */
unsigned int rnd_texture_new(void);
void rnd_texture_upload(unsigned int tex, int w, int h, const unsigned int *rgba);
void rnd_texture_upload_3d(unsigned int tex, int w, int h, const unsigned int *rgba);   /* + atlas */
void rnd_texture_free(unsigned int tex);
/* a 16-bit (RGB565) frame: full screen, or as an overlay where 0 is transparent */
/* the back buffer is filled with this once the 3D starts; 2D drawn over the
 * 3D is every other pixel (black included) */
#define RND_OVERLAY_EMPTY 0x0020
/* rects (x0,y0,x1,y1): the only areas of an overlay frame that hold pixels;
 * nrect < 0 = the whole frame */
void rnd_draw_frame16(const unsigned short *px, int w, int h, int pitch_px, int overlay,
                      const short (*rects)[4], int nrect);
/* 2D in game coordinates, drawn now (background before the 3D) */
void rnd_texture_upload16(unsigned int tex, int w, int h, const unsigned short *px565, int pitch_px);
void rnd_texture_upload16_key(unsigned int tex, int w, int h, const unsigned short *px565, int pitch_px,
                              unsigned int klo, unsigned int khi);   /* key range -> transparent */
void rnd_blit2d_mirror(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, int key);
void rnd_blit2d_vmirror(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1, int key);
void rnd_blit2d_ex(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1,
                   int key, int hmir, int vmir);
void rnd_blit2d_key(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1);
void rnd_blit2d(unsigned int tex, int tw, int th, int sx0, int sy0, int dx0, int dy0, int dx1, int dy1);
void rnd_fill2d(int x0, int y0, int x1, int y1, unsigned int argb);
void rnd_quad2d(unsigned int tex, int tw, int th, float sx0, float sy0, float sx1, float sy1,
                float dx0, float dy0, float dx1, float dy1, unsigned int argb);
int rnd_read_frame565(unsigned short *dst, int w, int h, int pitch_px, unsigned short empty);
float rnd_border_ext(void);                        /* black bars beside 4:3, game pixels per side */
void rnd_quad2d_screen(unsigned int tex, int tw, int th, float sx0, float sy0, float sx1, float sy1,
                       float dx0, float dy0, float dx1, float dy1, unsigned int argb);   /* x past 0..640 */
void rnd_dim_sides(float a);                      /* widescreen: darken outside 4:3 (0..1) */
void rnd_swap(void);

#endif
