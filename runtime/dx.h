/* dx.h — shared definitions of the DirectX 5 emulation (COM objects). */
#ifndef DX_H
#define DX_H
#include "w32.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_UNSUPPORTED 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_SURFACELOST 0x887601c2u

typedef int (*METHOD)();

/* every COM object starts with its vtable pointer */
typedef struct ComHdr { METHOD *vtbl; int refs; int kind; } ComHdr;
enum { K_DDRAW = 1, K_SURF, K_PAL, K_CLIP, K_D3D, K_D3DDEV, K_VIEWPORT, K_TEX, K_MAT, K_DSOUND, K_DSBUF, K_DINPUT, K_DIDEV };

int guid_is(const void *g, const char *s);   /* s like "6C14DB80-A733-11CE-A521-0020AF0BE560" */

/* surfaces */
typedef struct DDPal { ComHdr h; uint pal[256]; unsigned short pal565[256]; uint caps; } DDPal;

#define DD_MAXRECT 256   /* the race HUD alone is ~40-80 blits (each lap time digit) */
typedef struct DDSurf DDSurf;
typedef struct D3DTexIface { METHOD *vtbl; DDSurf *s; } D3DTexIface;
struct DDSurf {
    ComHdr h;
    uint caps;
    int w, h_, pitch, bpp;
    uint rmask, gmask, bmask, amask;
    unsigned char *pixels;
    DDSurf *back;          /* attached back buffer (primary) / next mip */
    DDSurf *zbuf;          /* attached Z buffer */
    DDPal *pal;
    uint ckey_lo, ckey_hi;
    int has_ckey;
    int locked;
    int dirty;             /* CPU pixels changed since last upload / present */
    int touched2d;         /* the back buffer got 2D writes this frame */
    int nrect;             /* areas of those writes: count, or -1 = anywhere */
    short rect[DD_MAXRECT][4];
    int nfill, fill_valid; /* back buffer: areas written since it was last reset
                              to RND_OVERLAY_EMPTY (count, -1 = anywhere) */
    short fillr[DD_MAXRECT][4];
    unsigned int tex2d;    /* GL copy of a 2D source surface (background blits) */
    int dirty2d;           /* pixels changed since tex2d was made */
    unsigned int tex2dk;   /* the same with the colour key as transparency (keyed background blits) */
    int dirty2dk;
    int band_end, band_val;  /* dx_ddraw.c pano_bottom_band: its last result (band_end 0 = none) */
    uint tex2dk_lo, tex2dk_hi;
    unsigned int readback_frame;   /* frame the 3D was last read into it (+1) */
    unsigned int tex;      /* GL texture (textures and the presenter) */
    int tex_w, tex_h;
    unsigned int handle;   /* D3D texture handle */
    D3DTexIface texif;
    int lost;
};

DDSurf *dd_surface_of(void *p);         /* accepts a surface or its texture interface */
DDSurf *dd_primary(void);
void dd_clear_rects(DDSurf *d);
int dd_bg_draw_gl(DDSurf *back);        /* frame start: draw queued background blits with GL */
int d3d_frame_is_open(void);
void dd_present(void);                  /* Flip / Blt to primary */
/* rects: areas that hold 2D pixels (x0,y0,x1,y1 each), nrect < 0 = whole frame */
void present_frame16(const unsigned short *px, int w, int h, int pitch_px, int overlay_only,
                     const short (*rects)[4], int nrect);
void present_swap(void);

/* Direct3D hooks used by DirectDraw */
void d3d_frame_begin_if_needed(void);
int  d3d_drew_this_frame(void);
void d3d_end_frame(void);

#endif
