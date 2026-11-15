/* w32.h — shared header for the Win32 / DirectX shim implementation files.
 * Includes Ghidra's types so every shim matches the generated prototypes. */
#ifndef W32_H
#define W32_H
#include <stdarg.h>
#include "recomp.h"
#include "types.h"
#ifndef NULL
#define NULL ((void *)0)
#endif
#include "w32_protos.h"
#include "host.h"
#include "plat.h"

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
void rt_map_path(const char *in, char *out, int outsz);
const char *rt_data_dir(void);

#define W32_LOG(...) rt_log(__VA_ARGS__)
#define TRACE_STUB(name) do { static int once_; if (!once_) { once_ = 1; rt_log("stub: %s\n", name); } } while (0)

/* kernel object handles */
enum { H_NONE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_HEAP, H_MODULE };
int   h_new(int type, void *obj);
void *h_get(HANDLE h, int type);
void  h_free(HANDLE h);
#define H_TO_HANDLE(i) ((HANDLE)(recomp_uptr)(0x100 + (i) * 4))

/* file objects */
typedef struct W32File { void *fp; char path[512]; } W32File;

/* windows (user32) */
typedef struct W32Wnd {
    int used;
    WNDPROC proc;
    DLGPROC dlgproc;
    LONG userdata, style, exstyle;
    LONG longs[16];
    int x, y, w, h;
    HWND parent;
    int id;
    int visible;
    char cls[64];
    char text[128];
    int checked;   /* dialog button state */
    int nitems, cursel;              /* list / combo box */
    LONG itemdata[32];
    char itemtext[32][64];
} W32Wnd;
W32Wnd *wnd_get(HWND h);
HWND    wnd_main(void);
void    w32_post(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* module handles: the game exe (its image is g_image) and Resource.dll */
#define GAME_HINSTANCE ((HINSTANCE)g_image)
HINSTANCE res_dll_handle(void);
const unsigned char *res_dll_image(void);

/* resources from the exe (in g_image) or Resource.dll */
const unsigned char *res_find(HINSTANCE inst, int type, const char *name_or_id, unsigned int *size);

#endif
