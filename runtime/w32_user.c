/* w32_user.c — USER32 / GDI32 / COMCTL32 / SHELL32 emulation.
 *
 * One real window (the game's main window, drawn by the GL presenter), a
 * message queue, and "headless" dialogs: DialogBoxParam runs the dialog
 * procedure's WM_INITDIALOG and then presses OK, so the game takes the
 * settings the dialog would have shown.  Control state (check boxes,
 * menu checks) is remembered so the game can read it back.
 */
#include "w32.h"
#include "audio.h"
#include "prof.h"
#include "menu.h"

#define WM_NULL 0
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_SIZE 5
#define WM_ACTIVATE 6
#define WM_SETFOCUS 7
#define WM_PAINT 0xf
#define WM_CLOSE 0x10
#define WM_QUIT 0x12
#define WM_ACTIVATEAPP 0x1c
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_INITDIALOG 0x110
#define WM_COMMAND 0x111

PlatPad g_pad;
static PlatPad s_prevpad;

/* ---- windows -------------------------------------------------------------- */
#define MAXW 96
static W32Wnd s_w[MAXW];
static HWND s_main;
static struct { char name[64]; WNDPROC proc; } s_cls[16];
static int s_ncls;

#define WND_HANDLE(i) ((HWND)(recomp_uptr)(0x10000 + (i) * 16))

W32Wnd *wnd_get(HWND h)
{
    recomp_uptr v = (recomp_uptr)h;
    if (v < 0x10000 || (v & 15)) return NULL;
    v = (v - 0x10000) / 16;
    if (v >= MAXW || !s_w[v].used) return NULL;
    return &s_w[v];
}

HWND wnd_main(void) { return s_main; }

static HWND wnd_new(const char *cls, HWND parent, int id)
{
    int i;
    for (i = 1; i < MAXW; i++) {
        if (!s_w[i].used) {
            memset(&s_w[i], 0, sizeof s_w[i]);
            s_w[i].used = 1;
            s_w[i].parent = parent;
            s_w[i].id = id;
            host_snprintf(s_w[i].cls, sizeof s_w[i].cls, "%s", cls ? cls : "");
            return WND_HANDLE(i);
        }
    }
    rt_fatal("out of windows");
    return NULL;
}

/* child control of a dialog, created on first reference */
static HWND ctl_get(HWND dlg, int id)
{
    int i;
    for (i = 1; i < MAXW; i++)
        if (s_w[i].used && s_w[i].parent == dlg && s_w[i].id == id) return WND_HANDLE(i);
    return wnd_new("control", dlg, id);
}

ATOM w32_RegisterClassExA(WNDCLASSEXA *wc)
{
    if (s_ncls >= 16) return 0;
    host_snprintf(s_cls[s_ncls].name, 64, "%s", wc->lpszClassName ? wc->lpszClassName : "");
    s_cls[s_ncls].proc = wc->lpfnWndProc;
    s_ncls++;
    return (ATOM)(0xc000 + s_ncls);
}

static LRESULT call_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    W32Wnd *w = wnd_get(h);
    if (!w) return 0;
    if (w->proc) return w->proc(h, m, wp, lp);
    if (w->dlgproc) return (LRESULT)w->dlgproc(h, m, wp, lp);
    return 0;
}

HWND w32_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                         HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND hw;
    W32Wnd *wnd;
    int i;
    uint cs[12];
    hw = wnd_new(((recomp_uptr)cls > 0xffff) ? cls : "atom", parent, (int)(recomp_uptr)menu);
    wnd = wnd_get(hw);
    wnd->style = (LONG)style;
    wnd->exstyle = (LONG)ex;
    wnd->x = x; wnd->y = y;
    plat_screen_size(&wnd->w, &wnd->h);
    if (title) host_snprintf(wnd->text, sizeof wnd->text, "%s", title);
    for (i = 0; i < s_ncls; i++)
        if ((recomp_uptr)cls > 0xffff && host_stricmp(s_cls[i].name, cls) == 0) wnd->proc = s_cls[i].proc;
    rt_log("CreateWindowExA(%s, \"%s\") -> %p\n", wnd->cls, wnd->text, (void *)hw);
    if (!parent && !s_main) s_main = hw;
    /* CREATESTRUCTA */
    cs[0] = (uint)(recomp_uptr)param; cs[1] = (uint)(recomp_uptr)inst; cs[2] = (uint)(recomp_uptr)menu;
    cs[3] = (uint)(recomp_uptr)parent; cs[4] = (uint)wnd->h; cs[5] = (uint)wnd->w; cs[6] = (uint)y; cs[7] = (uint)x;
    cs[8] = style; cs[9] = (uint)(recomp_uptr)title; cs[10] = (uint)(recomp_uptr)cls; cs[11] = ex;
    if (call_proc(hw, WM_CREATE, 0, (LPARAM)(recomp_uptr)cs) == -1) {
        wnd->used = 0;
        return NULL;
    }
    (void)h; (void)w;
    if (hw == s_main) {
        /* the game only runs while it believes it is the active app */
        w32_post(hw, WM_ACTIVATEAPP, 1, 0);
        w32_post(hw, WM_ACTIVATE, 1, 0);
        w32_post(hw, WM_SETFOCUS, 0, 0);
    }
    return hw;
}

BOOL w32_DestroyWindow(HWND h)
{
    W32Wnd *w = wnd_get(h);
    if (!w) return 0;
    call_proc(h, WM_DESTROY, 0, 0);
    w->used = 0;
    if (h == s_main) s_main = NULL;
    return 1;
}

LRESULT w32_DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (m == WM_CLOSE) w32_DestroyWindow(h);
    return 0;
}

BOOL w32_ShowWindow(HWND h, int cmd) { W32Wnd *w = wnd_get(h); if (w) w->visible = cmd != 0; return 1; }
BOOL w32_UpdateWindow(HWND h) { (void)h; return 1; }
BOOL w32_RedrawWindow(HWND h, RECT *r, HRGN rg, UINT f) { (void)h; (void)r; (void)rg; (void)f; return 1; }
BOOL w32_SetForegroundWindow(HWND h) { (void)h; return 1; }
BOOL w32_IsIconic(HWND h) { (void)h; return 0; }
HWND w32_GetFocus(void) { return s_main; }
BOOL w32_EnableWindow(HWND h, BOOL e) { (void)h; (void)e; return 0; }
BOOL w32_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL r) { (void)h; (void)x; (void)y; (void)cx; (void)cy; (void)r; return 1; }
BOOL w32_SetWindowPos(HWND h, HWND a, int x, int y, int cx, int cy, UINT f) { (void)h; (void)a; (void)x; (void)y; (void)cx; (void)cy; (void)f; return 1; }
BOOL w32_AdjustWindowRectEx(LPRECT r, DWORD s, BOOL m, DWORD e) { (void)r; (void)s; (void)m; (void)e; return 1; }
BOOL w32_ClientToScreen(HWND h, LPPOINT p) { (void)h; (void)p; return 1; }
BOOL w32_ScreenToClient(HWND h, LPPOINT p) { (void)h; (void)p; return 1; }
BOOL w32_GetCursorPos(LPPOINT p) { p->x = 0; p->y = 0; return 1; }
int w32_ShowCursor(BOOL show) { static int c = 0; c += show ? 1 : -1; return c; }

BOOL w32_GetClientRect(HWND h, LPRECT r)
{
    int w, hh;
    (void)h;
    plat_screen_size(&w, &hh);
    r->left = 0; r->top = 0; r->right = 640; r->bottom = 480;
    return 1;
}

BOOL w32_GetWindowRect(HWND h, LPRECT r) { return w32_GetClientRect(h, r); }

BOOL w32_SetRect(LPRECT r, int l, int t, int ri, int b)
{
    r->left = l; r->top = t; r->right = ri; r->bottom = b;
    return 1;
}

LONG w32_GetWindowLongA(HWND h, int idx)
{
    W32Wnd *w = wnd_get(h);
    if (!w) return 0;
    switch (idx) {
    case -4: return (LONG)(recomp_uptr)w->proc;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    case -12: return w->id;
    }
    if (idx >= 0 && idx < 64) return w->longs[idx / 4];
    return 0;
}

LONG w32_SetWindowLongA(HWND h, int idx, LONG v)
{
    W32Wnd *w = wnd_get(h);
    LONG old;
    if (!w) return 0;
    old = w32_GetWindowLongA(h, idx);
    switch (idx) {
    case -4: w->proc = (WNDPROC)(recomp_uptr)v; break;
    case -16: w->style = v; break;
    case -20: w->exstyle = v; break;
    case -21: w->userdata = v; break;
    default: if (idx >= 0 && idx < 64) w->longs[idx / 4] = v;
    }
    return old;
}

/* ---- message queue -------------------------------------------------------- */
#define QN 256
static MSG s_q[QN];
static int s_qh, s_qt;
static int s_quit = -1;

void w32_post(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int n = (s_qt + 1) % QN;
    if (n == s_qh) return;   /* full: drop */
    s_q[s_qt].hwnd = hwnd;
    s_q[s_qt].message = msg;
    s_q[s_qt].wParam = wp;
    s_q[s_qt].lParam = lp;
    s_q[s_qt].time = plat_time_ms();
    s_qt = n;
}

BOOL w32_PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { w32_post(h, m, wp, lp); return 1; }
void w32_PostQuitMessage(int code) { s_quit = code; }

/* virtual keys for the pad (for code that reads WM_KEYDOWN / GetAsyncKeyState) */
static const struct { unsigned int bit; int vk; } s_keymap[] = {
    { PAD_UP, 0x26 }, { PAD_DOWN, 0x28 }, { PAD_LEFT, 0x25 }, { PAD_RIGHT, 0x27 },
    { PAD_CROSS, 0x0d }, { PAD_CIRCLE, 0x1b }, { PAD_START, 0x0d },
    { PAD_TRIANGLE, 0x09 },     /* Tab: the Records / Time Attack option */
};

static void pump_platform(void)
{
    unsigned int i;
    if (!plat_poll(&g_pad)) { s_quit = 0; return; }
    {   /* the options menu sees the pad first (runtime/menu.c) */
        static int loaded;
        if (!loaded) { loaded = 1; menu_load_settings(); }
        menu_input(&g_pad);
    }
    audio_update();
    for (i = 0; i < sizeof s_keymap / sizeof s_keymap[0]; i++) {
        unsigned int b = s_keymap[i].bit;
        if ((g_pad.buttons & b) && !(s_prevpad.buttons & b)) w32_post(s_main, WM_KEYDOWN, s_keymap[i].vk, 1);
        if (!(g_pad.buttons & b) && (s_prevpad.buttons & b)) w32_post(s_main, WM_KEYUP, s_keymap[i].vk, 0xc0000001u);
    }
    s_prevpad = g_pad;
}

static BOOL take(LPMSG m, int remove)
{
    if (s_qh == s_qt) {
        if (s_quit >= 0) {
            memset(m, 0, sizeof *m);
            m->message = WM_QUIT;
            m->wParam = (WPARAM)s_quit;
            return 1;
        }
        return 0;
    }
    *m = s_q[s_qh];
    if (remove) s_qh = (s_qh + 1) % QN;
    return 1;
}

BOOL w32_PeekMessageA(LPMSG m, HWND h, UINT mn, UINT mx, UINT rm)
{
    static unsigned int last;
    unsigned int now = plat_time_ms();
    (void)h; (void)mn; (void)mx;
    if (s_qh == s_qt && now != last) { last = now; pump_platform(); }
    return take(m, rm & 1);
}

BOOL w32_GetMessageA(LPMSG m, HWND h, UINT mn, UINT mx)
{
    (void)h; (void)mn; (void)mx;
    while (!take(m, 1)) { pump_platform(); { PROF_PHASE(PH_IDLE); plat_sleep_ms(5); PROF_END(); } }
    return m->message != WM_QUIT;
}

BOOL w32_WaitMessage(void) { pump_platform(); if (s_qh == s_qt) { PROF_PHASE(PH_IDLE); plat_sleep_ms(5); PROF_END(); } return 1; }
BOOL w32_TranslateMessage(MSG *m) { (void)m; return 0; }
LRESULT w32_DispatchMessageA(MSG *m)
{
    if (m->message != 0x100 && m->message != 0x101)
        rt_log("msg 0x%x wp=0x%x lp=0x%x\n", m->message, (unsigned)m->wParam, (unsigned)m->lParam);
    return call_proc(m->hwnd ? m->hwnd : s_main, m->message, m->wParam, m->lParam);
}
int w32_TranslateAcceleratorA(HWND h, HACCEL a, LPMSG m) { (void)h; (void)a; (void)m; return 0; }
BOOL w32_IsDialogMessageA(HWND h, LPMSG m) { (void)h; (void)m; return 0; }

UINT w32_RegisterWindowMessageA(LPCSTR s) { static UINT n = 0xc100; (void)s; return n++; }

SHORT w32_GetAsyncKeyState(int vk)
{
    unsigned int i;
    for (i = 0; i < sizeof s_keymap / sizeof s_keymap[0]; i++)
        if (s_keymap[i].vk == vk && (g_pad.buttons & s_keymap[i].bit)) return (SHORT)0x8000;
    return 0;
}

/* ---- controls / dialogs ------------------------------------------------- */
#define BM_GETCHECK 0xf0
#define BM_SETCHECK 0xf1

LRESULT w32_SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    W32Wnd *w = wnd_get(h);
    if (!w) return 0;
    if (w->proc || w->dlgproc) return call_proc(h, m, wp, lp);
    switch (m) {
    case BM_GETCHECK: return w->checked;
    case BM_SETCHECK: w->checked = (int)wp; return 0;
    case 0x143: case 0x180:          /* CB_ADDSTRING / LB_ADDSTRING */
    case 0x14a: case 0x181: {        /* CB_INSERTSTRING / LB_INSERTSTRING (appended) */
        int i = w->nitems;
        if (i >= 32) return -1;
        w->nitems++;
        w->itemdata[i] = 0;
        host_snprintf(w->itemtext[i], 64, "%s", lp ? (const char *)(recomp_uptr)lp : "");
        return i;
    }
    case 0x146: case 0x18b: return w->nitems;                                    /* GETCOUNT */
    case 0x14b: case 0x184: w->nitems = 0; w->cursel = 0; return 0;              /* RESETCONTENT */
    case 0x147: case 0x188: return w->nitems ? w->cursel : -1;                   /* GETCURSEL */
    case 0x14e: case 0x186: w->cursel = (int)wp; return (LRESULT)wp;             /* SETCURSEL */
    case 0x150: case 0x199: return ((int)wp >= 0 && (int)wp < w->nitems) ? w->itemdata[wp] : -1;   /* GETITEMDATA */
    case 0x151: case 0x19a: if ((int)wp >= 0 && (int)wp < w->nitems) w->itemdata[wp] = (LONG)lp; return 0;  /* SETITEMDATA */
    case 0x148: case 0x189:                                                         /* GETLBTEXT / LB_GETTEXT */
        if ((int)wp < 0 || (int)wp >= w->nitems) return -1;
        if (lp) host_strcpy((char *)(recomp_uptr)lp, w->itemtext[wp]);
        return (LRESULT)host_strlen(w->itemtext[wp]);
    case 0x149: case 0x18a: return ((int)wp >= 0 && (int)wp < w->nitems) ? (LRESULT)host_strlen(w->itemtext[wp]) : -1;
    case 0x14c: case 0x18f: {                                                       /* FINDSTRING */
        int i;
        for (i = 0; i < w->nitems; i++) if (lp && host_stricmp(w->itemtext[i], (const char *)(recomp_uptr)lp) == 0) return i;
        return -1;
    }
    case 0xd: /* WM_GETTEXT */
        if (lp && wp) host_snprintf((char *)(recomp_uptr)lp, (unsigned)wp, "%s", w->text);
        return (LRESULT)host_strlen(w->text);
    case 0xc: /* WM_SETTEXT */
        if (lp) host_snprintf(w->text, sizeof w->text, "%s", (char *)(recomp_uptr)lp);
        return 1;
    }
    return 0;
}

HWND w32_GetDlgItem(HWND dlg, int id) { return ctl_get(dlg, id); }
int w32_GetDlgCtrlID(HWND h) { W32Wnd *w = wnd_get(h); return w ? w->id : 0; }
LRESULT w32_SendDlgItemMessageA(HWND dlg, int id, UINT m, WPARAM wp, LPARAM lp) { return w32_SendMessageA(ctl_get(dlg, id), m, wp, lp); }
BOOL w32_CheckDlgButton(HWND dlg, int id, UINT c) { wnd_get(ctl_get(dlg, id))->checked = (int)c; return 1; }
UINT w32_IsDlgButtonChecked(HWND dlg, int id) { return (UINT)wnd_get(ctl_get(dlg, id))->checked; }

BOOL w32_CheckRadioButton(HWND dlg, int first, int last, int check)
{
    int i;
    for (i = first; i <= last; i++) wnd_get(ctl_get(dlg, i))->checked = (i == check);
    return 1;
}

static int s_dlg_ended;
static INT_PTR s_dlg_result;

BOOL w32_EndDialog(HWND dlg, INT_PTR r)
{
    (void)dlg;
    s_dlg_ended = 1;
    s_dlg_result = r;
    return 1;
}

INT_PTR w32_DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    HWND dlg = wnd_new("dialog", parent, 0);
    W32Wnd *w = wnd_get(dlg);
    int saved_ended = s_dlg_ended;
    INT_PTR saved_res = s_dlg_result, r;
    (void)inst;
    w->dlgproc = proc;
    s_dlg_ended = 0;
    s_dlg_result = 0;
    rt_log("DialogBoxParamA(#%u): auto-accepting\n", (unsigned)(recomp_uptr)tmpl);
    proc(dlg, WM_INITDIALOG, 0, lp);
    if (!s_dlg_ended) proc(dlg, WM_COMMAND, 1 /* IDOK */, 0);
    r = s_dlg_ended ? s_dlg_result : 1;
    w->used = 0;
    s_dlg_ended = saved_ended;
    s_dlg_result = saved_res;
    return r;
}

HWND w32_CreateDialogParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    HWND dlg = wnd_new("dialog", parent, 0);
    (void)inst;
    wnd_get(dlg)->dlgproc = proc;
    rt_log("CreateDialogParamA(#%u)\n", (unsigned)(recomp_uptr)tmpl);
    proc(dlg, WM_INITDIALOG, 0, lp);
    return dlg;
}

INT_PTR w32_PropertySheetA(LPCPROPSHEETHEADERA h) { (void)h; rt_log("PropertySheetA: cancelled\n"); return 0; }

int w32_MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
    (void)h;
    rt_log("MessageBox [%s]: %s (type 0x%x)\n", cap ? cap : "", text ? text : "", type);
    if ((type & 0xf) == 4 || (type & 0xf) == 3) return 6;  /* IDYES */
    return 1;                                                /* IDOK */
}

/* ---- menus: remember item states so the game can read its settings ------- */
#define MAXMI 128
static struct { UINT id, state; } s_mi[MAXMI];
static int s_nmi;

static UINT *mi_state(UINT id)
{
    int i;
    for (i = 0; i < s_nmi; i++) if (s_mi[i].id == id) return &s_mi[i].state;
    if (s_nmi == MAXMI) return &s_mi[0].state;
    s_mi[s_nmi].id = id;
    s_mi[s_nmi].state = 0;
    return &s_mi[s_nmi++].state;
}

#define FAKE_MENU ((HMENU)(recomp_uptr)0x30000)
HMENU w32_GetMenu(HWND h) { (void)h; return FAKE_MENU; }
/* Read the initial item states (grayed / checked) from the menu resource. */
static void menu_load_states(const unsigned char *p, const unsigned char *end)
{
    /* MENUHEADER (version, offset) then MENUITEMTEMPLATEs */
    unsigned int off = p[2] | (p[3] << 8);
    int depth = 0;
    p += 4 + off;
    while (p + 2 <= end) {
        unsigned int fl = p[0] | (p[1] << 8);
        p += 2;
        if (!(fl & 0x10)) {                      /* MF_POPUP has no id */
            unsigned int id = p[0] | (p[1] << 8);
            p += 2;
            if (id) *mi_state(id) = fl & 0x0b;   /* GRAYED | DISABLED | CHECKED */
        }
        while (p + 1 < end && (p[0] | p[1])) p += 2;   /* UTF-16 text */
        p += 2;
        if (fl & 0x10) depth++;
        if (fl & 0x80) { if (depth == 0) break; depth--; }
    }
}

HMENU w32_LoadMenuA(HINSTANCE i, LPCSTR n)
{
    unsigned int sz;
    const unsigned char *r = res_find(i, 4, n, &sz);
    if (!r) r = res_find(res_dll_handle(), 4, n, &sz);
    if (r) menu_load_states(r, r + sz);
    else rt_log("LoadMenuA: menu %u not found\n", (unsigned)(recomp_uptr)n);
    return FAKE_MENU;
}
HMENU w32_GetSystemMenu(HWND h, BOOL r) { (void)h; (void)r; return FAKE_MENU; }
BOOL w32_SetMenu(HWND h, HMENU m) { (void)h; (void)m; return 1; }
BOOL w32_DrawMenuBar(HWND h) { (void)h; return 1; }
BOOL w32_InsertMenuA(HMENU m, UINT p, UINT f, UINT_PTR id, LPCSTR s) { (void)m; (void)p; (void)f; (void)id; (void)s; return 1; }

DWORD w32_CheckMenuItem(HMENU m, UINT id, UINT c)
{
    UINT *s = mi_state(id), old = *s & 8;
    (void)m;
    *s = (*s & ~8u) | (c & 8);
    return old;
}

BOOL w32_EnableMenuItem(HMENU m, UINT id, UINT e)
{
    UINT *s = mi_state(id), old = *s & 3;
    (void)m;
    *s = (*s & ~3u) | (e & 3);
    return (BOOL)old;
}

UINT w32_GetMenuState(HMENU m, UINT id, UINT f) { (void)m; (void)f; return *mi_state(id); }

/* ---- misc resources / GDI ------------------------------------------------ */
HICON w32_LoadIconA(HINSTANCE i, LPCSTR n) { (void)i; (void)n; return (HICON)(recomp_uptr)0x31000; }
HCURSOR w32_LoadCursorA(HINSTANCE i, LPCSTR n) { (void)i; (void)n; return (HCURSOR)(recomp_uptr)0x32000; }
HACCEL w32_LoadAcceleratorsA(HINSTANCE i, LPCSTR n) { (void)i; (void)n; return (HACCEL)(recomp_uptr)0x33000; }
HANDLE w32_LoadImageA(HINSTANCE i, LPCSTR n, UINT t, int cx, int cy, UINT f) { (void)i; (void)n; (void)t; (void)cx; (void)cy; (void)f; return (HANDLE)(recomp_uptr)0x34000; }
BOOL w32_WinHelpA(HWND h, LPCSTR f, UINT c, ULONG_PTR d) { (void)h; (void)f; (void)c; (void)d; return 1; }

HDC w32_GetDC(HWND h) { (void)h; return (HDC)(recomp_uptr)0x40000; }
int w32_ReleaseDC(HWND h, HDC dc) { (void)h; (void)dc; return 1; }

HDC w32_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    (void)h;
    memset(ps, 0, sizeof *ps);
    ps->hdc = (HDC)(recomp_uptr)0x40000;
    return ps->hdc;
}

BOOL w32_EndPaint(HWND h, PAINTSTRUCT *ps) { (void)h; (void)ps; return 1; }
BOOL w32_TextOutA(HDC dc, int x, int y, LPCSTR s, int n) { (void)dc; (void)x; (void)y; (void)s; (void)n; return 1; }
COLORREF w32_SetBkColor(HDC dc, COLORREF c) { (void)dc; return c; }
HGDIOBJ w32_SelectObject(HDC dc, HGDIOBJ o) { (void)dc; return o; }
HGDIOBJ w32_GetStockObject(int i) { return (HGDIOBJ)(recomp_uptr)(0x41000 + i); }
BOOL w32_DeleteObject(HGDIOBJ o) { (void)o; return 1; }
HFONT w32_CreateFontIndirectA(LOGFONTA *lf) { (void)lf; return (HFONT)(recomp_uptr)0x42000; }
int w32_GetDeviceCaps(HDC dc, int idx)
{
    (void)dc;
    switch (idx) {
    case 8: return 640;    /* HORZRES */
    case 10: return 480;   /* VERTRES */
    case 12: return 16;    /* BITSPIXEL */
    case 14: return 1;     /* PLANES */
    case 38: return 0;     /* RASTERCAPS: no palette */
    }
    return 0;
}

UINT w32_GetSystemPaletteEntries(HDC dc, UINT start, UINT n, LPPALETTEENTRY pe)
{
    UINT i;
    (void)dc;
    for (i = 0; pe && i < n; i++) { pe[i].peRed = pe[i].peGreen = pe[i].peBlue = (BYTE)(start + i); pe[i].peFlags = 0; }
    return n;
}

int w32_wsprintfA(LPSTR buf, LPCSTR fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = host_vsprintf_va(buf, fmt, &ap);
    va_end(ap);
    return r;
}

/* comctl32 / shell32: the window chrome the port does not have */
HWND w32_CreateToolbarEx(HWND hwnd, DWORD ws, UINT id, int nb, HINSTANCE bi, UINT_PTR bid, LPCTBBUTTON b,
                         int n, int dx, int dy, int bx, int by, UINT sz)
{
    (void)ws; (void)nb; (void)bi; (void)bid; (void)b; (void)n; (void)dx; (void)dy; (void)bx; (void)by; (void)sz;
    return wnd_new("toolbar", hwnd, (int)id);
}
HWND w32_CreateStatusWindowA(LONG st, LPCSTR t, HWND p, UINT id) { (void)st; (void)t; return wnd_new("status", p, (int)id); }
HIMAGELIST w32_ImageList_Create(int cx, int cy, UINT f, int a, int b) { (void)cx; (void)cy; (void)f; (void)a; (void)b; return (HIMAGELIST)(recomp_uptr)0x43000; }
BOOL w32_ImageList_Destroy(HIMAGELIST h) { (void)h; return 1; }
int w32_ImageList_ReplaceIcon(HIMAGELIST h, int i, HICON ic) { (void)h; (void)ic; return i; }
BOOL w32_Shell_NotifyIconA(DWORD m, PNOTIFYICONDATAA d) { (void)m; (void)d; return 1; }
