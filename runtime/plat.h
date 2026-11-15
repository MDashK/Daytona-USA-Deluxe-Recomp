/* plat.h — the thin per-platform layer (host Windows test build / PS Vita).
 *
 * Everything Win32/DirectX-shaped lives in portable runtime code; a platform
 * only supplies a GL context, input, an audio output stream, timing and the
 * file-system quirks.  No Windows or Ghidra types cross this interface.
 */
#ifndef PLAT_H
#define PLAT_H

/* lifecycle */
int  plat_init(void);                 /* window + GL context, audio */
void plat_shutdown(void);
void plat_fatal(const char *msg);     /* show the message, then the caller exits */
const char *plat_data_dir(void);      /* with trailing slash */
void plat_fix_case(char *path);       /* resolve case-insensitive names in place */

/* time */
unsigned int plat_time_ms(void);
void plat_sleep_ms(unsigned int ms);
void plat_thread_start(void (*fn)(void *), void *arg);   /* a background job (low priority) */

/* video: the GL context is current after plat_init; size in pixels */
void plat_screen_size(int *w, int *h);
void plat_swap(void);

/* input: polled once per frame; returns 0 if the app should quit */
#define PAD_UP       0x0001
#define PAD_DOWN     0x0002
#define PAD_LEFT     0x0004
#define PAD_RIGHT    0x0008
#define PAD_CROSS    0x0010
#define PAD_CIRCLE   0x0020
#define PAD_SQUARE   0x0040
#define PAD_TRIANGLE 0x0080
#define PAD_L        0x0100
#define PAD_R        0x0200
#define PAD_START    0x0400
#define PAD_SELECT   0x0800
typedef struct PlatPad {
    unsigned int buttons;
    int lx, ly, rx, ry;          /* -128..127 */
    int ltrig, rtrig;            /* 0..255 (L2/R2 on a DS4 via Vita TV; else 0) */
} PlatPad;
int  plat_poll(PlatPad *pad);

#if !defined(__vita__)
/* Windows only: the window and the keyboard (the options menu, menu.c) */
#define PLAT_WIN_MODES 5              /* 0..3: 1x..4x of 480 lines, 4: borderless fullscreen */
int  plat_window_mode(void);
void plat_window_set(int mode, int wide);   /* wide: the 16:9 shape, else 4:3 */
#define PLAT_NKEYS 12                 /* one key per pad button: action i is the bit 1 << i */
int  plat_key_get(int action);        /* a virtual-key code */
void plat_key_set(int action, int vk);
int  plat_key_default(int action);
int  plat_key_capture(int start);     /* start: begin; then the first key pressed, else 0 */
void plat_key_name(int vk, char *out, int n);   /* upper case, the menu font's characters */
#endif

/* audio: the platform pulls 44100 Hz stereo s16 from this callback */
typedef void (*PlatAudioCb)(short *out, int frames);
void plat_audio_start(PlatAudioCb cb);
void plat_audio_lock(void);
void plat_audio_unlock(void);

#endif
