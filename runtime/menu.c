/* menu.c — the port's options menu: Select on the main menu opens a
 * semi-transparent panel over the running game (the PC version keeps these
 * options in the window's menu bar and a D3D options dialog).
 *
 * Text uses the game's own font (Resource/640X480X16/Font/Records_0.bmp:
 * ! " ' . 0-9 ? and A-Z, white with a black outline on a green key). */
#include "w32.h"
#include "plat.h"
#include "render.h"
#include "menu.h"

char *getenv(const char *name);
unsigned char *rt_load_data_file(const char *rel, unsigned int *len);
undefined4 FUN_0040d5d4(int param_1);                      /* save/load D3D options */
void FUN_0045aba6(undefined4 a, undefined4 b, undefined4 c);
undefined4 FUN_0045ac75(undefined4 a, undefined4 b, undefined4 c);
extern volatile unsigned int g_dbg_presents;

volatile unsigned int g_mainmenu_stamp = 0xffffff00u;      /* set by the main menu handler */
int g_opt_widescreen = 1;                                  /* 16:9 3D in races (render.c) */
int g_opt_cardetail = 0;  /* NPC cars always with the full model (changes the AI; off) - lifted_rt.c */
int g_opt_drawdist = 1;   /* scenery blocks drawn: 0 as original, 1 far, 2 farthest, 3 maximum - lifted_rt.c */
extern int g_opt_bgm_vol, g_opt_sfx_vol;   /* % (audio.c) */

/* ---- font ------------------------------------------------------------------ */
static unsigned int s_font_tex;
static int s_font_w, s_font_h, s_font_ok = -1;
static struct { short x0, x1, y0, y1; } s_glyph[128];
static int s_dig_top, s_dig_bot;   /* rows of the cell the digit 0 covers (the % sign is drawn to match) */

static void font_load(void)
{
    static const char *rows[3] = { " !\"'.0123456789?", "ABCDEFGHIJKLMNOP", "QRSTUVWXYZ" };
    unsigned int len, off, x, y, *rgba;
    int w, h, bpp, stride, r;
    unsigned char *bmp = rt_load_data_file("Resource/640X480X16/Font/Records_0.bmp", &len);
    s_font_ok = 0;
    if (!bmp || len < 54 || bmp[0] != 'B' || bmp[1] != 'M') { rt_log("menu: font not found\n"); return; }
    off = bmp[10] | bmp[11] << 8 | bmp[12] << 16 | (unsigned)bmp[13] << 24;
    w = bmp[18] | bmp[19] << 8;
    h = bmp[22] | bmp[23] << 8;
    bpp = bmp[28];
    if (bpp != 24 || w <= 0 || h <= 0 || w > 1024 || h > 512) { rt_log("menu: font format %d\n", bpp); host_free(bmp); return; }
    stride = (w * 3 + 3) & ~3;
    rgba = (unsigned int *)host_malloc((unsigned)(w * h * 4));
    for (y = 0; y < (unsigned)h; y++) {
        const unsigned char *row = bmp + off + (size_t)(h - 1 - y) * stride;   /* bottom-up */
        for (x = 0; x < (unsigned)w; x++) {
            unsigned int b = row[x * 3], g = row[x * 3 + 1], rr = row[x * 3 + 2];
            int key = g > 200 && rr < 60 && b < 60;
            rgba[y * w + x] = key ? 0 : (rr | g << 8 | b << 16 | 0xff000000u);
        }
    }
    /* glyphs: a grid of 16 cells of 15x26 per row (the letters touch, so
     * there are no gaps to find); each glyph trimmed to its own columns */
    for (r = 0; r < 3; r++) {
        int n = (int)host_strlen(rows[r]), k, cw = w / 16, ch = h / 3;
        for (k = 0; k < n; k++) {
            unsigned char c = (unsigned char)rows[r][k];
            int x0 = k * cw, x1 = x0 + cw, y0 = r * ch, y1 = y0 + ch, a0 = x1, a1 = x0, xx;
            for (xx = x0; xx < x1; xx++) {
                int yy, used = 0;
                for (yy = y0; yy < y1 && !used; yy++) used = rgba[yy * w + xx] != 0;
                if (used) { if (xx < a0) a0 = xx; a1 = xx + 1; }
            }
            if (c == ' ' || a1 <= a0) continue;
            s_glyph[c].x0 = (short)a0; s_glyph[c].x1 = (short)a1;
            s_glyph[c].y0 = (short)y0; s_glyph[c].y1 = (short)y1;
        }
    }
    {   /* the digit 0's rows inside its cell */
        int yy, xx, t = -1, bt = -1;
        for (yy = s_glyph['0'].y0; yy < s_glyph['0'].y1; yy++)
            for (xx = s_glyph['0'].x0; xx < s_glyph['0'].x1; xx++)
                if (rgba[yy * w + xx]) { if (t < 0) t = yy; bt = yy + 1; break; }
        s_dig_top = t < 0 ? 0 : t - s_glyph['0'].y0;
        s_dig_bot = bt < 0 ? h / 3 : bt - s_glyph['0'].y0;
    }
    s_font_tex = rnd_texture_new();
    rnd_texture_upload(s_font_tex, w, h, rgba);
    s_font_w = w; s_font_h = h;
    host_free(rgba);
    host_free(bmp);
    s_font_ok = 1;
}

/* the font has no %: two small zeros and a slash of outlined dots */
static float percent(float x, float y, float scale, unsigned int argb, int draw)
{
    float gh = (float)(s_dig_bot - s_dig_top), w = gh * 0.9f * scale;
    if (draw) {
        const float k = 0.5f;    /* the small zeros */
        float zw = (s_glyph['0'].x1 - s_glyph['0'].x0) * scale * k, zh = (s_glyph['0'].y1 - s_glyph['0'].y0) * scale * k;
        float top = y + s_dig_top * scale, bot = y + s_dig_bot * scale;
        float ytop = top - s_dig_top * scale * k, ybot = bot - s_dig_bot * scale * k;   /* zeros' cells */
        int i, pass;
        for (pass = 0; pass < 2; pass++)   /* the slash: black outline, then the colour */
            for (i = 0; i <= 16; i++) {
                float t = i / 16.0f, cx = x + w * 0.12f + (w * 0.76f) * t, cy = bot - (bot - top) * t;
                float r = (pass ? 1.3f : 2.6f) * scale;
                rnd_quad2d(0, 0, 0, 0, 0, 0, 0, cx - r, cy - r, cx + r, cy + r, pass ? argb : (argb & 0xff000000u));
            }
        rnd_quad2d(s_font_tex, s_font_w, s_font_h, s_glyph['0'].x0, s_glyph['0'].y0, s_glyph['0'].x1, s_glyph['0'].y1,
                   x, ytop, x + zw, ytop + zh, argb);
        rnd_quad2d(s_font_tex, s_font_w, s_font_h, s_glyph['0'].x0, s_glyph['0'].y0, s_glyph['0'].x1, s_glyph['0'].y1,
                   x + w - zw, ybot, x + w, ybot + zh, argb);
    }
    return w + scale;
}

/* draw a string (upper case, digits, ! " ' . ? %) at a scale; returns its width */
static int s_text_screen;              /* draw with rnd_quad2d_screen (the black bars) */
static float text(float x, float y, const char *s, float scale, unsigned int argb, int draw)
{
    float x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '%') { x += percent(x, y, scale, argb, draw); continue; }
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (c == ' ' || c >= 128 || s_glyph[c].x1 <= s_glyph[c].x0) { x += 9 * scale; continue; }
        if (draw)
            (s_text_screen ? rnd_quad2d_screen : rnd_quad2d)(s_font_tex, s_font_w, s_font_h,
                       s_glyph[c].x0, s_glyph[c].y0, s_glyph[c].x1, s_glyph[c].y1,
                       x, y, x + (s_glyph[c].x1 - s_glyph[c].x0) * scale, y + (s_glyph[c].y1 - s_glyph[c].y0) * scale, argb);
        x += (s_glyph[c].x1 - s_glyph[c].x0 + 1) * scale;
    }
    return x - x0;
}

/* ---- options ------------------------------------------------------------- */
/* pages (L / R): the port's display options, then the pages of the PC
 * version's game settings dialog (ARCADE FUN_0045b9c1, TIME ATTACK
 * FUN_0045c897, 1P VS 2P FUN_0045c04b) - read at the race start (FUN_00413657) */
enum { K_BACK, K_TEXT, K_CARS, K_SPEED, K_POLY, K_BG, K_FOG, K_FILTER, K_PERSP, K_WIDE, K_DRAW, K_MODE, K_DIFF, K_ONOFF, K_BIT, K_BIT2P,
       K_WIN, K_BGM, K_SFX, K_KEY, K_KEYRESET };
typedef struct { const char *label; int kind; unsigned addr, mask; } Item;
#define CONSOLE(a) { "LAPS", K_BIT, a, 1 }, { "LAP TIME", K_BIT, a, 2 }, { "SECTION LAP", K_BIT, a, 4 }, \
    { "TACHOMETER", K_BIT, a, 8 }, { "CURRENT SPEED", K_BIT, a, 0x10 }, { "GEAR", K_BIT, a, 0x20 }, \
    { "RANKING", K_BIT, a, 0x40 }, { "RADAR", K_BIT, a, 0x80 }, { "COURSE MAP", K_BIT, a, 0x100 }
static const Item s_display[] = {
    { "SPEED", K_SPEED }, { "POLYGONS", K_POLY }, { "BACKGROUND", K_BG }, { "FOG", K_FOG },
    { "TEXTURE FILTER", K_FILTER },    /* CAR DETAIL left out: it changed the AI, not the swap */    /* PERSPECTIVE left out: the GPU always corrects */
#if !defined(__vita__)
    { "WIDESCREEN", K_WIDE },          /* the Vita: 16:9 screen, always on */
    { "DRAW DISTANCE", K_DRAW },       /* the Vita: always MAXIMUM */
    { "WINDOW", K_WIN },               /* 1X..4X, borderless fullscreen (Alt+Enter) */
#endif
    { "BACK", K_BACK } };
static const Item s_audio[] = {
    { "BGM VOLUME", K_BGM }, { "SFX VOLUME", K_SFX }, { "BACK", K_BACK } };
#if !defined(__vita__)
static const Item s_keys[] = {        /* mask: the action (pad bit 1 << n, plat.h) */
    { "ACCELERATE", K_KEY, 0, 4 }, { "BRAKE", K_KEY, 0, 6 }, { "STEER LEFT", K_KEY, 0, 2 },
    { "STEER RIGHT", K_KEY, 0, 3 }, { "SHIFT UP", K_KEY, 0, 9 }, { "SHIFT DOWN", K_KEY, 0, 8 },
    { "CHANGE VIEW", K_KEY, 0, 7 }, { "UP", K_KEY, 0, 0 }, { "DOWN", K_KEY, 0, 1 },
    { "START", K_KEY, 0, 10 }, { "CANCEL", K_KEY, 0, 5 }, { "OPTIONS MENU", K_KEY, 0, 11 },
    { "RESET KEYS", K_KEYRESET }, { "BACK", K_BACK } };
#endif
static const Item s_arcade[] = {
    { "GAME MODE", K_MODE, 0x89cdd8 }, { "DIFFICULTY", K_DIFF, 0x89cddc }, { "TIME COMPARE", K_ONOFF, 0x89cde0 },
    CONSOLE(0x89ce00), { "BACK", K_BACK } };
static const Item s_tattack[] = {
    { "GHOST CAR", K_ONOFF, 0x89ce10 }, { "TIME COMPARE", K_ONOFF, 0x89cde4 }, CONSOLE(0x89ce04), { "BACK", K_BACK } };
static const Item s_versus[] = {      /* console items per player: bits 0-7 1P, 8-15 2P */
    { "TIME COMPARE", K_ONOFF, 0x89cde8 }, { "LAPS", K_BIT2P, 0x89ce08, 1 }, { "LAP TIME", K_BIT2P, 0x89ce08, 2 },
    { "BEST LAP", K_BIT2P, 0x89ce08, 4 }, { "TACHOMETER", K_BIT2P, 0x89ce08, 8 },
    { "CURRENT SPEED", K_BIT2P, 0x89ce08, 0x10 }, { "GEAR", K_BIT2P, 0x89ce08, 0x20 },
    { "RANKING", K_BIT2P, 0x89ce08, 0x40 }, { "RADAR", K_BIT2P, 0x89ce08, 0x80 }, { "BACK", K_BACK } };
static const Item s_about[] = {
    { "PORT BY MDASHK", K_TEXT }, { "AI ASSISTED BY CLAUDE", K_TEXT }, { "DAYTONA USA DX V1.20", K_TEXT },
    { "BACK", K_BACK } };
static const struct { const char *title; const Item *it; int n; } s_page[] = {
    { "DISPLAY", s_display, (int)(sizeof s_display / sizeof s_display[0]) },
    { "AUDIO", s_audio, (int)(sizeof s_audio / sizeof s_audio[0]) },
    { "ARCADE", s_arcade, (int)(sizeof s_arcade / sizeof s_arcade[0]) },
    { "TIME ATTACK", s_tattack, (int)(sizeof s_tattack / sizeof s_tattack[0]) },
    { "1P VS 2P", s_versus, (int)(sizeof s_versus / sizeof s_versus[0]) },
#if !defined(__vita__)
    { "KEYS", s_keys, (int)(sizeof s_keys / sizeof s_keys[0]) },
#endif
    { "ABOUT", s_about, (int)(sizeof s_about / sizeof s_about[0]) } };
#define NPAGE (int)(sizeof s_page / sizeof s_page[0])
static int s_open, s_cur, s_pg, s_d3d_changed;

/* ---- car cheat: Triangle 7 times on the main menu shows all 11 cars, for
 * this session only.  0x89ce5c = extra cars unlocked (0..3, the car lists
 * show 8 + it, the extra ones last: Daytona, Uma, Uma 2).  The save keeps the
 * real count, and a car picked (0x89ce1c 1P / 0x89ce20 2P) that is not really
 * unlocked is saved as the first one, the Hornet
 * (FUN_0040d493 override) and the game's unlock progress at the end of a race
 * runs on it (FUN_0045b030 override); records are saved as usual. */
#define CARS_EXTRA G(int, 0x89ce5c)
void FUN_00468b40(int param_1, byte param_2);   /* sound effect */
static int s_cheat, s_cheat_real, s_cheat_taps;
static unsigned int s_cheat_last, s_cheat_msg;
static int s_save_pick[2];


static void cheat_cars_on(void)
{
    if (!s_cheat) {
        s_cheat = 1;
        s_cheat_real = CARS_EXTRA;
        if (s_cheat_real < 0 || s_cheat_real > 3) s_cheat_real = 0;
        CARS_EXTRA = 3;
    }
    s_cheat_msg = plat_time_ms();
    rt_log("cheat: all cars (real count %d)\n", s_cheat_real);
    FUN_00468b40(0, 0x3c);                 /* the menus' decide sound */
}

void port_cars_real(int enter)
{
    if (!s_cheat) return;
    if (enter) CARS_EXTRA = s_cheat_real;
    else {
        if (CARS_EXTRA != s_cheat_real) rt_log("cheat: real car count now %d\n", CARS_EXTRA);
        s_cheat_real = CARS_EXTRA;
        CARS_EXTRA = 3;
    }
}

void port_cars_save(int enter)
{
    if (!s_cheat) return;
    if (enter) {
        s_save_pick[0] = G(int, 0x89ce1c); s_save_pick[1] = G(int, 0x89ce20);
        CARS_EXTRA = s_cheat_real;
        if (s_save_pick[0] > s_cheat_real + 7) G(int, 0x89ce1c) = 0;
        if (s_save_pick[1] > s_cheat_real + 7) G(int, 0x89ce20) = 0;
    } else {
        CARS_EXTRA = 3;
        G(int, 0x89ce1c) = s_save_pick[0]; G(int, 0x89ce20) = s_save_pick[1];
    }
}
static int s_capture = -1;            /* KEYS: the action waiting for a key */
static unsigned int s_capture_at;
static PlatPad s_prev;

#define DISPLAY_FLAGS G(unsigned int, 0x89cee8)    /* bit0 background, bit1 mph, bit3 high polygon */
#define D3D_FOG G(int, 0x4bf220)
#define D3D_PERSP G(int, 0x4bf218)
#define D3D_FILTER G(int, 0x4bf21c)
#define D3D_APPLY G(int, 0x4bf214)

/* the game settings the port keeps (the game resets them at each start) */
static const unsigned s_game_addr[] = { 0x89cdd8, 0x89cddc, 0x89cde0, 0x89ce00, 0x89ce10, 0x89cde4, 0x89ce04,
                                        0x89cde8, 0x89ce08 };
#define NGAME (int)(sizeof s_game_addr / sizeof s_game_addr[0])
static unsigned s_game_val[NGAME];
static int s_game_have[NGAME];

static const char *value(const Item *it)
{
    static char buf[32];
    unsigned v = it->addr ? G(unsigned, it->addr) : 0;
    switch (it->kind) {
    case K_BGM: host_snprintf(buf, sizeof buf, "%d%%", g_opt_bgm_vol); return buf;
    case K_SFX: host_snprintf(buf, sizeof buf, "%d%%", g_opt_sfx_vol); return buf;
#if !defined(__vita__)
    case K_WIN: {
        static const char *m[PLAT_WIN_MODES] = { "1X", "2X", "3X", "4X", "FULLSCREEN" };
        int w = plat_window_mode();
        return w >= 0 && w < PLAT_WIN_MODES ? m[w] : "";
    }
    case K_KEY:
        if (s_capture == (int)it->mask) return "PRESS A KEY";
        plat_key_name(plat_key_get((int)it->mask), buf, sizeof buf);
        return buf;
#endif
    case K_SPEED: return (DISPLAY_FLAGS & 2) ? "MPH" : "KMH";
    case K_POLY: return (DISPLAY_FLAGS & 8) ? "HIGH" : "LOW";
    case K_BG: return (DISPLAY_FLAGS & 1) ? "ON" : "OFF";
    case K_FOG: return D3D_FOG ? "ON" : "OFF";
    case K_FILTER: return D3D_FILTER ? "ON" : "OFF";
    case K_PERSP: return D3D_PERSP ? "ON" : "OFF";
    case K_WIDE: return g_opt_widescreen ? "ON" : "OFF";
    case K_CARS: return g_opt_cardetail ? "FULL" : "NORMAL";
    case K_DRAW: return g_opt_drawdist == 0 ? "NORMAL" : g_opt_drawdist == 1 ? "FAR" : g_opt_drawdist == 2 ? "FARTHEST" : "MAXIMUM";
    case K_MODE: return v == 1 ? "GRAND PRIX" : v == 2 ? "ENDURANCE" : "NORMAL";
    case K_DIFF: return v == 0 ? "EASY" : v == 2 ? "HARD" : "NORMAL";
    case K_ONOFF: return v ? "ON" : "OFF";
    case K_BIT: return (v & it->mask) ? "ON" : "OFF";
    case K_BIT2P: {
        int a = (v & it->mask) != 0, b = (v & (it->mask << 8)) != 0;
        return a && b ? "1P 2P" : a ? "1P" : b ? "2P" : "OFF";
    }
    }
    return "";
}

static void game_name(char *o, unsigned a)   /* "Game89cdd8" */
{
    int i;
    o[0] = 'G'; o[1] = 'a'; o[2] = 'm'; o[3] = 'e';
    for (i = 0; i < 6; i++) o[4 + i] = "0123456789abcdef"[(a >> (20 - 4 * i)) & 15];
    o[10] = 0;
}
#define PORT_KEY "Software\\SEGA\\DAYTONA USA Deluxe\\Port"
/* our own settings, in the emulated registry next to the game's */
static void wide_save(void)
{
    HKEY k;
    DWORD v = (DWORD)g_opt_widescreen, dd = (DWORD)g_opt_drawdist;
    int i;
    if (w32_RegCreateKeyA((HKEY)(recomp_uptr)0x80000001u, PORT_KEY, &k) == 0) {
        w32_RegSetValueExA(k, "Widescreen", 0, 4, (BYTE *)&v, 4);
        w32_RegSetValueExA(k, "DrawDistance2", 0, 4, (BYTE *)&dd, 4);
        v = (DWORD)g_opt_cardetail;
        w32_RegSetValueExA(k, "CarDetail", 0, 4, (BYTE *)&v, 4);
        v = (DWORD)g_opt_bgm_vol;
        w32_RegSetValueExA(k, "BgmVolume", 0, 4, (BYTE *)&v, 4);
        v = (DWORD)g_opt_sfx_vol;
        w32_RegSetValueExA(k, "SfxVolume", 0, 4, (BYTE *)&v, 4);
#if !defined(__vita__)
        v = (DWORD)plat_window_mode();
        w32_RegSetValueExA(k, "WindowMode", 0, 4, (BYTE *)&v, 4);
        for (i = 0; i < PLAT_NKEYS; i++) {
            char name[8] = { 'K', 'e', 'y', 0, 0, 0 };
            name[3] = (char)('0' + i / 10); name[4] = (char)('0' + i % 10);
            v = (DWORD)plat_key_get(i);
            w32_RegSetValueExA(k, name, 0, 4, (BYTE *)&v, 4);
        }
#endif
        for (i = 0; i < NGAME; i++)
            if (s_game_have[i]) {
                char name[16];
                game_name(name, s_game_addr[i]);
                v = (DWORD)s_game_val[i];
                w32_RegSetValueExA(k, name, 0, 4, (BYTE *)&v, 4);
            }
        w32_RegCloseKey(k);
    }
}

void menu_load_settings(void)
{
    HKEY k;
    DWORD v = 1, n = 4, t = 4;
    int i;
#if !defined(__vita__)
    int win = 1;                           /* 2X by default */
#endif
    if (w32_RegCreateKeyA((HKEY)(recomp_uptr)0x80000001u, PORT_KEY, &k) == 0) {
        if (w32_RegQueryValueExA(k, "Widescreen", NULL, &t, (BYTE *)&v, &n) == 0) g_opt_widescreen = v != 0;
        n = 4;
        if (w32_RegQueryValueExA(k, "DrawDistance2", NULL, &t, (BYTE *)&v, &n) == 0 && v <= 3) g_opt_drawdist = (int)v;
        n = 4;
        if (w32_RegQueryValueExA(k, "BgmVolume", NULL, &t, (BYTE *)&v, &n) == 0 && v <= 200) g_opt_bgm_vol = (int)v;
        n = 4;
        if (w32_RegQueryValueExA(k, "SfxVolume", NULL, &t, (BYTE *)&v, &n) == 0 && v <= 200) g_opt_sfx_vol = (int)v;
        n = 4;
#if !defined(__vita__)
        if (w32_RegQueryValueExA(k, "WindowMode", NULL, &t, (BYTE *)&v, &n) == 0 && v < PLAT_WIN_MODES) win = (int)v;
        for (i = 0; i < PLAT_NKEYS; i++) {
            char name[8] = { 'K', 'e', 'y', 0, 0, 0 };
            name[3] = (char)('0' + i / 10); name[4] = (char)('0' + i % 10);
            n = 4;
            if (w32_RegQueryValueExA(k, name, NULL, &t, (BYTE *)&v, &n) == 0 && v > 0 && v < 255) plat_key_set(i, (int)v);
        }
#endif
        n = 4;
        for (i = 0; i < NGAME; i++) {   /* put in once the game has set its defaults (menu_input) */
            char name[16];
            game_name(name, s_game_addr[i]);
            n = 4;
            if (w32_RegQueryValueExA(k, name, NULL, &t, (BYTE *)&v, &n) == 0) { s_game_val[i] = v; s_game_have[i] = 1; }
        }
        w32_RegCloseKey(k);
    }
    {   /* DAYTONA_DRAWDIST=0..2 / DAYTONA_WIDE=0..1: override (testing) */
        const char *e = getenv("DAYTONA_DRAWDIST");
        if (e && *e >= '0' && *e <= '3') g_opt_drawdist = *e - '0';
        e = getenv("DAYTONA_CARLOD");
        if (e && (*e == '0' || *e == '1')) g_opt_cardetail = *e - '0';
        e = getenv("DAYTONA_WIDE");
        if (e && (*e == '0' || *e == '1')) g_opt_widescreen = *e - '0';
#if !defined(__vita__)
        e = getenv("DAYTONA_WINMODE");     /* 0..3 = 1X..4X, 4 fullscreen */
        if (e && *e >= '0' && *e <= '4') win = *e - '0';
#endif
    }
#if defined(__vita__)
    g_opt_widescreen = 1;
    g_opt_drawdist = 3;
#else
    plat_window_set(win, g_opt_widescreen);
#endif
}

static void game_changed(unsigned addr)
{
    int i;
    for (i = 0; i < NGAME; i++)
        if (s_game_addr[i] == addr) { s_game_val[i] = G(unsigned, addr); s_game_have[i] = 1; }
    wide_save();
}

static void toggle(const Item *it, int dir)
{
    unsigned *v = it->addr ? &G(unsigned, it->addr) : NULL;
    switch (it->kind) {
    case K_SPEED: DISPLAY_FLAGS ^= 2; break;
    case K_POLY:
        DISPLAY_FLAGS ^= 8;
        FUN_0045aba6(0xc, 0, 0);           /* what the menu bar's handler does after it */
        FUN_0045aba6(0xb, 0, 0);
        break;
    case K_BG: DISPLAY_FLAGS ^= 1; break;
    case K_FOG: D3D_FOG = !D3D_FOG; D3D_APPLY = 1; s_d3d_changed = 1; break;
    case K_FILTER: D3D_FILTER = !D3D_FILTER; D3D_APPLY = 1; s_d3d_changed = 1; break;
    case K_PERSP: D3D_PERSP = !D3D_PERSP; D3D_APPLY = 1; s_d3d_changed = 1; break;
    case K_WIDE:
        g_opt_widescreen = !g_opt_widescreen;
#if !defined(__vita__)
        if (plat_window_mode() < 4) plat_window_set(plat_window_mode(), g_opt_widescreen);   /* the window's shape */
#endif
        wide_save();
        break;
    case K_BGM: case K_SFX: {
        int *p = it->kind == K_BGM ? &g_opt_bgm_vol : &g_opt_sfx_vol;
        *p += dir * 10;
        if (*p < 0) *p = 0;
        if (*p > 200) *p = 200;
        wide_save();
        break;
    }
#if !defined(__vita__)
    case K_WIN:
        plat_window_set((plat_window_mode() + PLAT_WIN_MODES + dir) % PLAT_WIN_MODES, g_opt_widescreen);
        wide_save();
        break;
    case K_KEYRESET: {
        int i;
        for (i = 0; i < PLAT_NKEYS; i++) plat_key_set(i, plat_key_default(i));
        wide_save();
        break;
    }
#endif
    case K_CARS: g_opt_cardetail = !g_opt_cardetail; wide_save(); break;
    case K_DRAW: g_opt_drawdist = (g_opt_drawdist + 4 + dir) % 4; wide_save(); break;   /* applies at once */
    case K_MODE: case K_DIFF: *v = (*v + 3 + dir) % 3; game_changed(it->addr); break;
    case K_ONOFF: *v = !*v; game_changed(it->addr); break;
    case K_BIT: *v ^= it->mask; game_changed(it->addr); break;
    case K_BIT2P: {   /* OFF -> 1P -> 2P -> 1P 2P */
        int st = ((*v & it->mask) ? 1 : 0) | ((*v & (it->mask << 8)) ? 2 : 0);
        st = (st + 4 + dir) % 4;
        *v = (*v & ~(it->mask | it->mask << 8)) | ((st & 1) ? it->mask : 0) | ((st & 2) ? it->mask << 8 : 0);
        game_changed(it->addr);
        break;
    }
    }
}

static void close_menu(void)
{
    rt_log("menu: closed\n");
    s_open = 0;
    if (s_d3d_changed) FUN_0040d5d4(1);    /* save the D3D options like the dialog does */
    s_d3d_changed = 0;
}

int menu_is_open(void) { return s_open; }

/* called with each new pad state; while the menu is open the game sees an
 * idle pad (and the button that closed it stays hidden until released) */
void menu_input(PlatPad *pad)
{
    static unsigned int swallow;
    unsigned int b = pad->buttons, pressed = b & ~s_prev.buttons;
    /* the main menu is up and idle: its sprite set exists (FUN_00455621
     * creates it, FUN_0045572b frees it) and its state is 3 (waiting) */
    int in_main_menu = G(int, 0x4df044) != 0 && G(int, 0x4df03c) == 3;
    s_prev = *pad;
    {   /* DAYTONA_GOTO=seconds:scene - post a scene change once (debug, e.g. 42 name entry) */
        static int sec = -2, scene;
        if (sec == -2) {
            const char *e = getenv("DAYTONA_GOTO");
            sec = -1;
            if (e) { sec = 0; while (*e >= '0' && *e <= '9') sec = sec * 10 + (*e++ - '0');
                     if (*e == ':') { e++; scene = 0; while (*e >= '0' && *e <= '9') scene = scene * 10 + (*e++ - '0'); } }
        }
        if (sec >= 0 && plat_time_ms() >= (unsigned)sec * 1000u) {
            sec = -1;
            rt_log("debug: goto scene %d\n", scene);
            FUN_0045ac75(0x14, (undefined4)scene, 0);
        }
    }
    {   /* scene changes in the log (the game's scene table 0x4be740, e.g. 42 name entry) */
        static int last = -1;
        int sc = G(int, 0x866684);
        if (sc != last) {
            rt_log("scene %d at frame %u\n", sc, g_dbg_presents);
            /* autosave: the game writes its save only on EXIT (FUN_004325e4);
             * after the name entry (42) and the records (43) the records and
             * the settings block are written now, so closing the app another
             * way keeps them (the car cheat stays out: FUN_0040d493 override) */
            if (last == 42 || last == 43) {
                extern void FUN_0040d493(int param_1);
                extern void FUN_0040d2cf(int param_1);
                unsigned int t0 = plat_time_ms();
                FUN_0040d2cf(1);           /* records */
                FUN_0040d493(1);           /* settings block (car unlocks) */
                rt_log("autosave after scene %d (%u ms)\n", last, plat_time_ms() - t0);
            }
            last = sc;
        }
    }
    if (in_main_menu) {   /* the saved game settings, over the game's defaults (once) */
        static int applied;
        if (!applied) {
            int i;
            applied = 1;
            for (i = 0; i < NGAME; i++) if (s_game_have[i]) G(unsigned, s_game_addr[i]) = s_game_val[i];
        }
    }
    {   /* DAYTONA_MENUTEST=1: open at once (to look at it, testing) */
        static int t = -1;
        if (t < 0) { const char *e = getenv("DAYTONA_MENUTEST"); t = e && *e == '1'; if (t) { s_open = 1; s_cur = 0; } }
    }
#if !defined(__vita__)
    {   /* the window mode changed by Alt+Enter: kept for the next start */
        static int saved = -1;
        if (saved < 0) saved = plat_window_mode();
        if (plat_window_mode() != saved) { saved = plat_window_mode(); wide_save(); }
    }
    if (s_open && s_capture >= 0) {        /* KEYS: waiting for a key */
        int vk = plat_key_capture(0);
        if (vk) {
            plat_key_set(s_capture, vk);
            rt_log("menu: key %d = %d\n", s_capture, vk);
            s_capture = -1;
            wide_save();
        } else if (plat_time_ms() - s_capture_at > 6000) s_capture = -1;   /* nothing pressed: as it was */
        if (s_capture < 0) s_prev.buttons = ~0u;   /* no press counts until the keys are let go */
        pad->buttons = 0; pad->lx = pad->ly = pad->rx = pad->ry = 0; pad->ltrig = pad->rtrig = 0;
        return;
    }
#endif
    if (!s_open && in_main_menu && (pressed & PAD_TRIANGLE)) {
        unsigned int now = plat_time_ms();
        if (now - s_cheat_last > 1500) s_cheat_taps = 0;   /* in a row */
        s_cheat_last = now;
        if (++s_cheat_taps >= 7) { s_cheat_taps = 0; cheat_cars_on(); }
    }
    {   /* DAYTONA_CHEATCARS=1: on at the first main menu (testing) */
        static int t = -1;
        if (t < 0) { const char *e = getenv("DAYTONA_CHEATCARS"); t = e && *e == '1'; }
        if (t == 1 && in_main_menu) {
            t = 0;
            cheat_cars_on();
            if (getenv("DAYTONA_CHEATCAR")) {     /* =n: the arcade car cursor starts on car n (testing) */
                G(int, 0x4c4704) = host_atoi(getenv("DAYTONA_CHEATCAR"));
            }
            if (getenv("DAYTONA_CHEATSAVE")) {   /* and save the settings block at once (testing) */
                extern void FUN_0040d493(int param_1);
                G(int, 0x89ce1c) = 10;           /* Uma 2 picked */
                FUN_0040d493(1);
                rt_log("cheat: saved, now count %d pick %d\n", CARS_EXTRA, G(int, 0x89ce1c));
            }
        }
    }
    if (!s_open) {
        if ((pressed & PAD_SELECT) && in_main_menu) {
            rt_log("menu: open at %u ms\n", plat_time_ms());
            s_open = 1;
            s_cur = 0;
            s_pg = 0;
            swallow = PAD_SELECT;
        }
    } else {
        int n = s_page[s_pg].n;
        const Item *it;
        if (pressed & (PAD_L | PAD_R)) {   /* page */
            s_pg = (s_pg + NPAGE + ((pressed & PAD_L) ? -1 : 1)) % NPAGE;
            if (s_cur >= s_page[s_pg].n) s_cur = s_page[s_pg].n - 1;
            n = s_page[s_pg].n;
        }
        if (pressed & PAD_UP) do s_cur = (s_cur + n - 1) % n; while (s_page[s_pg].it[s_cur].kind == K_TEXT);
        if (pressed & PAD_DOWN) do s_cur = (s_cur + 1) % n; while (s_page[s_pg].it[s_cur].kind == K_TEXT);
        while (s_page[s_pg].it[s_cur].kind == K_TEXT) s_cur = (s_cur + 1) % n;   /* text lines: not selectable */
        it = &s_page[s_pg].it[s_cur];
        if (pressed & (PAD_LEFT | PAD_RIGHT | PAD_CROSS)) {
            if (it->kind == K_BACK) { if (pressed & PAD_CROSS) { close_menu(); swallow = b; } }
#if !defined(__vita__)
            else if (it->kind == K_KEY) {
                if (pressed & PAD_CROSS) { s_capture = (int)it->mask; s_capture_at = plat_time_ms(); plat_key_capture(1); }
            } else if (it->kind == K_KEYRESET) { if (pressed & PAD_CROSS) toggle(it, 1); }
#endif
            else toggle(it, (pressed & PAD_LEFT) ? -1 : 1);
        }
        if (pressed & (PAD_CIRCLE | PAD_SELECT | PAD_START)) { close_menu(); swallow = b; }
    }
    if (s_open) swallow = b | swallow;
    swallow &= b;                          /* a hidden button shows again once released */
    if (s_open || swallow) {
        pad->buttons &= ~swallow;
        if (s_open) { pad->buttons = 0; pad->lx = pad->ly = pad->rx = pad->ry = 0; pad->ltrig = pad->rtrig = 0; }
    }
}

/* on the main menu, in the black bar at the upper right (never over the 4:3
 * picture): which button opens the options */
static void select_hint(void)
{
    char key[32], l1[48];
    const char *lines[3];
    float e = rnd_border_ext(), avail, w = 0, scale, y;
    int i, n;
    if (G(int, 0x4df044) == 0 || G(int, 0x4df03c) != 3) return;   /* the main menu, idle (menu_input) */
    if (s_font_ok < 0) font_load();
    if (s_font_ok <= 0) return;
    if (s_cheat_msg && plat_time_ms() - s_cheat_msg < 4000) {   /* the cheat's notice, 4 s */
        static const char *m[2] = { "ALL CARS", "UNLOCKED" };
        float mw = 0, sc;
        for (i = 0; i < 2; i++) { float t = text(0, 0, m[i], 1.0f, 0, 0); if (t > mw) mw = t; }
        if (e >= 40) {                     /* in the bar, like the hint */
            sc = (e - 10) / mw;
            if (sc > 0.55f) sc = 0.55f;
            s_text_screen = 1;
            for (i = 0; i < 2; i++) {
                float tw = text(0, 0, m[i], sc, 0, 0);
                text(640 + (e - tw) / 2, 10 + i * 28 * sc, m[i], sc, 0xffffe020u, 1);
            }
            s_text_screen = 0;
        } else {                           /* 4:3 screen: a short line at the top */
            float tw = text(0, 0, "ALL CARS UNLOCKED", 0.6f, 0, 0);
            text(320 - tw / 2, 6, "ALL CARS UNLOCKED", 0.6f, 0xffffe020u, 1);
        }
        return;
    }
    if (e < 40) return;                    /* no room beside the picture */
#if defined(__vita__)
    host_snprintf(key, sizeof key, "SELECT");
#else
    plat_key_name(plat_key_get(11), key, sizeof key);   /* the OPTIONS MENU key */
#endif
    host_snprintf(l1, sizeof l1, "%s", key);
    lines[0] = "PRESS"; lines[1] = l1; lines[2] = "FOR OPTIONS";
    n = 3;
    for (i = 0; i < n; i++) { float t = text(0, 0, lines[i], 1.0f, 0, 0); if (t > w) w = t; }
    avail = e - 10;                        /* 5 px margins inside the bar */
    scale = w > 0 ? avail / w : 0.5f;
    if (scale > 0.55f) scale = 0.55f;
    {   /* slow pulse */
        unsigned int t = plat_time_ms() % 2400, a = t < 1200 ? t : 2400 - t;
        unsigned int alpha = 0x90 + a * 0x6f / 1200;
        s_text_screen = 1;
        for (i = 0, y = 10; i < n; i++, y += 28 * scale) {
            float tw = text(0, 0, lines[i], scale, 0, 0);
            text(640 + (e - tw) / 2, y, lines[i], scale, (alpha << 24) | (i == 1 ? 0xffe020u : 0xffffffu), 1);
        }
        s_text_screen = 0;
    }
}

void menu_draw(void)
{
    int i, n = s_page[s_pg].n;
    float y, x0 = 130, x1 = 510, top = 20, row = 26;
    if (!s_open) { select_hint(); return; }
    if (s_font_ok < 0) font_load();
    rnd_quad2d(0, 0, 0, 0, 0, 0, 0, 0, 0, 640, 480, 0x80000000u);              /* dim the game */
    rnd_dim_sides(0.5f);                                                      /* and the widescreen sides */
    rnd_quad2d(0, 0, 0, 0, 0, 0, 0, x0, top, x1, top + 70 + n * row, 0xc0101040u);
    for (i = 0; i < NPAGE; i++) {   /* page dots (L / R) */
        float cx = 320 + (i - (NPAGE - 1) / 2.0f) * 18;
        rnd_quad2d(0, 0, 0, 0, 0, 0, 0, cx - 4, top + 44, cx + 4, top + 52, i == s_pg ? 0xffffe020u : 0x80ffffffu);
    }
    if (s_font_ok <= 0) return;
    {
        const char *t = s_page[s_pg].title;
        float w = text(0, 0, t, 1.0f, 0, 0);
        text(320 - w / 2, top + 10, t, 1.0f, 0xffffffffu, 1);
        text(x0 + 12, top + 14, "L", 0.7f, 0xa0ffffffu, 1);
        text(x1 - 24, top + 14, "R", 0.7f, 0xa0ffffffu, 1);
    }
    for (i = 0, y = top + 62; i < n; i++, y += row) {
        const Item *it = &s_page[s_pg].it[i];
        unsigned int c = i == s_cur ? 0xffffe020u : 0xffffffffu;   /* selected: yellow (ARGB) */
        const char *v = value(it);
        if (i == s_cur) rnd_quad2d(0, 0, 0, 0, 0, 0, 0, x0 + 6, y - 3, x1 - 6, y + row - 4, 0x60ffffffu);
        if (it->kind == K_TEXT) {        /* ABOUT lines, centred */
            float w = text(0, 0, it->label, 0.75f, 0, 0);
            text(320 - w / 2, y, it->label, 0.75f, 0xffffffffu, 1);
            continue;
        }
        text(x0 + 18, y, it->label, 0.75f, c, 1);
        if (*v) {
            float w = text(0, 0, v, 0.75f, 0, 0);
            text(x1 - 18 - w, y, v, 0.75f, c, 1);
        }
    }
}
