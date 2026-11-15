/* plat_win.c — Windows (x86) platform: a resizable window (1x..4x or
 * borderless fullscreen, Alt+Enter) with an OpenGL 1.x context, keyboard as
 * the pad, waveOut audio.
 *
 * Default keys (the options menu's KEYS page changes them): arrows = d-pad /
 * steer, X = cross (accelerate), Z = square (brake), C = triangle (view),
 * A/S = L/R (shift down/up), Esc = circle (back), Enter = start,
 * Backspace = select (the options menu).
 * Data directory: argv-independent; DAYTONA_DATA env var or ..\game\ next to the exe.
 */
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include "plat.h"

static HWND s_wnd;
static HDC s_dc;
static int s_quit;
static char s_datadir[MAX_PATH];
static LARGE_INTEGER s_freq, s_t0;

/* ---- crash report: exception + symbolised stack (needs daytona.pdb) ---- */
#include <dbghelp.h>
void rt_log(const char *fmt, ...);

/* DAYTONA_WATCH_RING=1: keep the last writes of the watchpoint, dump on crash */
typedef struct { DWORD eip, ret, val, cur, gsp; } WatchRec;
static WatchRec s_ring[8192];
static unsigned s_ring_n;
static void sym_line(HANDLE proc, DWORD a, const char *pfx)
{
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    DWORD64 disp = 0;
    memset(sym, 0, sizeof symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(proc, a, &disp, sym)) rt_log("%s%s+0x%llx", pfx, sym->Name, disp);
    else rt_log("%s0x%08lx", pfx, a);
}
static void ring_dump(HANDLE proc)
{
    unsigned i, n = s_ring_n < 8192 ? s_ring_n : 8192;
    if (!s_ring_n) return;
    rt_log("last %u watch writes (of %u):\n", n, s_ring_n);
    for (i = s_ring_n - n; i < s_ring_n; i++) {
        WatchRec *r = &s_ring[i & 8191];
        rt_log("  = %08lx cur=%08lx gsp=%08lx", r->val, r->cur, r->gsp);
        sym_line(proc, r->eip, " at ");
        sym_line(proc, r->ret, " from ");
        rt_log("\n");
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
    STACKFRAME64 sf;
    CONTEXT ctx = *ep->ContextRecord;
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    int i;
    rt_log("\n*** CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        rt_log(" (%s 0x%08lx)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
               (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    rt_log("\n");
    { extern void lifted_dump_state(void); lifted_dump_state(); }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);
    ring_dump(proc);
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = ctx.Eip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
    for (i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_I386, proc, th, &sf, &ctx, NULL,
                                       SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line;
        DWORD ldisp = 0;
        memset(sym, 0, sizeof symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        line.SizeOfStruct = sizeof line;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) {
            if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ldisp, &line))
                rt_log("  #%d %s+0x%llx  (%s:%lu)\n", i, sym->Name, disp, line.FileName, line.LineNumber);
            else
                rt_log("  #%d %s+0x%llx\n", i, sym->Name, disp);
        } else {
            rt_log("  #%d 0x%08llx\n", i, sf.AddrPC.Offset);
        }
    }
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void log_stack(HANDLE th, CONTEXT *c, int maxf)
{
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 sf;
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    int i;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = c->Eip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = c->Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c->Esp; sf.AddrStack.Mode = AddrModeFlat;
    for (i = 0; i < maxf && StackWalk64(IMAGE_FILE_MACHINE_I386, proc, th, &sf, c, NULL,
                                         SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
        DWORD64 disp = 0;
        memset(sym, 0, sizeof symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) rt_log("    %s+0x%llx\n", sym->Name, disp);
        else rt_log("    0x%08llx\n", sf.AddrPC.Offset);
    }
}

static HANDLE s_mainthread;

/* DAYTONA_WATCH=<hex VA>: hardware write watchpoint on a game address */
extern unsigned char g_image[];
static DWORD s_watch_va;
/* DAYTONA_WATCH=<VA in the image>, or DAYTONA_WATCH=h<host address> */
static unsigned char *s_watch_host;
static unsigned char *watch_ptr(void)
{
    const char *w = getenv("DAYTONA_WATCH");
    if (s_watch_host) return s_watch_host;
    if (w && (w[0] == 'h' || w[0] == 'H')) return (unsigned char *)(uintptr_t)strtoul(w + 1, NULL, 16);
    return g_image + (s_watch_va - 0x400000);
}
static LONG WINAPI watch_veh(EXCEPTION_POINTERS *ep)
{
    static int hits;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    {
        unsigned int nv = *(unsigned int *)watch_ptr();
        if (getenv("DAYTONA_WATCH_HI") && (nv >> 24) != strtoul(getenv("DAYTONA_WATCH_HI"), NULL, 16))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (getenv("DAYTONA_WATCH_EQ") && nv != strtoul(getenv("DAYTONA_WATCH_EQ"), NULL, 16))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (getenv("DAYTONA_WATCH_RING")) {
        extern unsigned int g_cur_func;
        WatchRec *r;
        if (getenv("DAYTONA_WATCH_SKIPCUR") && g_cur_func == strtoul(getenv("DAYTONA_WATCH_SKIPCUR"), NULL, 16))
            return EXCEPTION_CONTINUE_EXECUTION;
        r = &s_ring[s_ring_n++ & 8191];
        DWORD *bp = (DWORD *)ep->ContextRecord->Ebp;
        r->eip = ep->ContextRecord->Eip;
        r->ret = IsBadReadPtr(bp, 8) ? 0 : bp[1];
        r->val = *(unsigned int *)watch_ptr();
        r->cur = g_cur_func;
        { extern unsigned int g_esp; r->gsp = g_esp; }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (hits++ < (getenv("DAYTONA_WATCH_N") ? atoi(getenv("DAYTONA_WATCH_N")) : 12)) {
        CONTEXT c = *ep->ContextRecord;
        rt_log("[watch %08lx] = %08x\n", s_watch_va, *(unsigned int *)watch_ptr());
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        log_stack(GetCurrentThread(), &c, 6);
        { extern void lifted_dump_state(void); lifted_dump_state(); }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watch_setter(LPVOID p)
{
    CONTEXT c;
    (void)p;
    Sleep(50);
    SuspendThread(s_mainthread);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(s_mainthread, &c);
    c.Dr0 = (DWORD)(uintptr_t)watch_ptr();
    c.Dr7 = 1 | (1u << 16) | (3u << 18);   /* L0, write, 4 bytes */
    SetThreadContext(s_mainthread, &c);
    ResumeThread(s_mainthread);
    return 0;
}
static DWORD WINAPI sampler(LPVOID p)
{
    int period = (int)(INT_PTR)p;
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    for (;;) {
        CONTEXT c;
        Sleep(period);
        SuspendThread(s_mainthread);
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(s_mainthread, &c);
        rt_log("[sample @%u ms]\n", plat_time_ms());
        log_stack(s_mainthread, &c, 12);
        ResumeThread(s_mainthread);
    }
}

/* ---- window modes ---------------------------------------------------------- */
static int s_win_mode = -1, s_win_wide = 1, s_win_last = 1;   /* s_win_last: the windowed mode for Alt+Enter */

int plat_window_mode(void) { return s_win_mode < 0 ? 1 : s_win_mode; }

void plat_window_set(int mode, int wide)
{
    HMONITOR mon = MonitorFromWindow(s_wnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    if (mode < 0 || mode >= PLAT_WIN_MODES) mode = 1;
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(mon, &mi);
    s_win_wide = wide;
    if (mode == 4) {   /* borderless: a popup over the whole monitor */
        RECT m = mi.rcMonitor;
        SetWindowLongA(s_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s_wnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        /* 480 lines per step, the Vita screen's shape (dx_d3d.c widens the 3D to it) or 4:3 */
        int h = 480 * (mode + 1), w = wide ? (h * 960 + 272) / 544 : h * 4 / 3;
        RECT wa = mi.rcWork, r;
        int maxw, maxh;
        DWORD st = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        r.left = r.top = 0; r.right = 640; r.bottom = 480;
        AdjustWindowRect(&r, st, FALSE);
        maxw = (wa.right - wa.left) - ((r.right - r.left) - 640);
        maxh = (wa.bottom - wa.top) - ((r.bottom - r.top) - 480);
        if (w > maxw) { h = h * maxw / w; w = maxw; }   /* bigger than the screen: the largest that fits */
        if (h > maxh) { w = w * maxh / h; h = maxh; }
        r.left = r.top = 0; r.right = w; r.bottom = h;
        AdjustWindowRect(&r, st, FALSE);
        SetWindowLongA(s_wnd, GWL_STYLE, st);
        SetWindowPos(s_wnd, HWND_NOTOPMOST,
                     wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2,
                     wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_win_last = mode;
    }
    s_win_mode = mode;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE) { s_quit = 1; return 0; }
    if (m == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {   /* Alt+Enter */
        if (!(lp & (1 << 30))) plat_window_set(s_win_mode == 4 ? s_win_last : 4, s_win_wide);
        return 0;
    }
    /* Alt alone would open the system menu and stall the game; Alt+F4 still closes */
    if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && wp != VK_F4) return 0;
    if (m == WM_SYSCHAR) return 0;
    if (m == WM_SYSCOMMAND && (wp & 0xfff0) == SC_KEYMENU) return 0;
    if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && s_win_mode == 4) { SetCursor(NULL); return TRUE; }
    return DefWindowProcA(h, m, wp, lp);
}

/* ---- key bindings ----------------------------------------------------------- */
/* action i drives the pad bit 1 << i (plat.h) */
static const int s_key_def[PLAT_NKEYS] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    'X',          /* cross: accelerate, menus OK */
    VK_ESCAPE,    /* circle: back */
    'Z',          /* square: brake */
    'C',          /* triangle: change view */
    'A', 'S',     /* L / R: shift down / up */
    VK_RETURN,    /* start */
    VK_BACK,      /* select: the options menu */
};
static int s_key[PLAT_NKEYS] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'X', VK_ESCAPE, 'Z', 'C', 'A', 'S', VK_RETURN, VK_BACK };

int plat_key_default(int a) { return a >= 0 && a < PLAT_NKEYS ? s_key_def[a] : 0; }
int plat_key_get(int a) { return a >= 0 && a < PLAT_NKEYS ? s_key[a] : 0; }
void plat_key_set(int a, int vk)
{
    int i;
    if (a < 0 || a >= PLAT_NKEYS || vk <= 0 || vk > 254) return;
    for (i = 0; i < PLAT_NKEYS; i++)       /* a key used elsewhere: the two swap */
        if (i != a && s_key[i] == vk) s_key[i] = s_key[a];
    s_key[a] = vk;
}

int plat_key_capture(int start)
{
    static unsigned char held[256];
    int vk;
    if (GetForegroundWindow() != s_wnd) return 0;
    for (vk = 8; vk < 255; vk++) {
        int down;
        /* the generic Shift/Ctrl/Alt: the left/right ones (0xa0..0xa5) instead */
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
        down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!start && down && !held[vk]) { held[vk] = 1; return vk; }
        held[vk] = (unsigned char)down;     /* keys held at the start count once released */
    }
    return 0;
}

void plat_key_name(int vk, char *out, int n)
{
    static const struct { int vk; const char *s; } names[] = {
        { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
        { VK_RETURN, "ENTER" }, { VK_ESCAPE, "ESC" }, { VK_BACK, "BACKSPACE" }, { VK_TAB, "TAB" },
        { VK_SPACE, "SPACE" }, { VK_LSHIFT, "LEFT SHIFT" }, { VK_RSHIFT, "RIGHT SHIFT" },
        { VK_LCONTROL, "LEFT CTRL" }, { VK_RCONTROL, "RIGHT CTRL" }, { VK_LMENU, "LEFT ALT" },
        { VK_RMENU, "RIGHT ALT" }, { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" },
        { VK_END, "END" }, { VK_PRIOR, "PAGE UP" }, { VK_NEXT, "PAGE DOWN" }, { VK_CAPITAL, "CAPS LOCK" },
        { VK_LBUTTON, "MOUSE 1" }, { VK_RBUTTON, "MOUSE 2" }, { VK_MBUTTON, "MOUSE 3" },
        { VK_XBUTTON1, "MOUSE 4" }, { VK_XBUTTON2, "MOUSE 5" },
    };
    unsigned int i;
    char tmp[64];
    int k = 0;
    const char *p;
    if (n <= 0) return;
    out[0] = 0;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].vk == vk) { snprintf(out, n, "%s", names[i].s); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "NUM %d", vk - VK_NUMPAD0); return; }
    tmp[0] = 0;
    GetKeyNameTextA((LONG)(MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */) << 16), tmp, sizeof tmp);
    for (p = tmp; *p && k < n - 1; p++) {  /* only what the font has */
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == 0x27 || c == '!' || c == '?')
            out[k++] = c;
    }
    out[k] = 0;
    if (!k || out[0] == ' ') snprintf(out, n, "KEY %d", vk);
}

/* debug: arm the write watchpoint on a host address from code */
static LONG WINAPI watch_veh(EXCEPTION_POINTERS *ep);
static DWORD WINAPI watch_setter(LPVOID p);
void plat_debug_watch(void *addr)
{
    HANDLE th;
    if (s_watch_host) return;
    s_watch_host = (unsigned char *)addr;
    if (!s_mainthread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_mainthread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, watch_veh);
    th = CreateThread(NULL, 0, watch_setter, NULL, 0, NULL);
    WaitForSingleObject(th, INFINITE);   /* until the debug registers are set */
}

/* debug: log the caller chain of the current point */
void plat_debug_stack(int maxf)
{
    CONTEXT c;
    RtlCaptureContext(&c);
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    log_stack(GetCurrentThread(), &c, maxf);
}

int plat_init(void)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    RECT r = { 0, 0, 960, 544 };
    HGLRC rc;
    int pf;
    const char *env = getenv("DAYTONA_DATA");
    if (env) {
        snprintf(s_datadir, sizeof s_datadir, "%s", env);
    } else {
        /* next to the exe: data\dayusadx\ (the converter's output folder, as
         * on the Vita), the game folder itself, or ..\game (the repository) */
        char exe[MAX_PATH], probe[MAX_PATH + 64], *p;
        GetModuleFileNameA(NULL, exe, sizeof exe);
        p = strrchr(exe, '\\');
        if (p) *p = 0;
        snprintf(probe, sizeof probe, "%s\\data\\dayusadx\\DAYTONA USA Deluxe.exe", exe);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
            snprintf(s_datadir, sizeof s_datadir, "%s\\data\\dayusadx", exe);
        else {
            snprintf(probe, sizeof probe, "%s\\DAYTONA USA Deluxe.exe", exe);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
                snprintf(s_datadir, sizeof s_datadir, "%s", exe);
            else
                snprintf(s_datadir, sizeof s_datadir, "%s\\..\\game", exe);
        }
    }
    {
        size_t n = strlen(s_datadir);
        if (n && s_datadir[n - 1] != '\\' && s_datadir[n - 1] != '/') strcat(s_datadir, "/");
    }
    SetUnhandledExceptionFilter(crash_filter);
    /* a windowed program: "--console" on the command line opens a console
     * that shows the log as it is written (debugging) */
    if (strstr(GetCommandLineA(), "--console") && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    {   /* real pixels on scaled displays (else Windows stretches a smaller window, blurred) */
        typedef BOOL (WINAPI *DpiFn)(void);
        DpiFn f = (DpiFn)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        if (f) f();
    }
    if (getenv("DAYTONA_WATCH")) {
        s_watch_va = strtoul(getenv("DAYTONA_WATCH"), NULL, 16);
        if (!s_mainthread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_mainthread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        AddVectoredExceptionHandler(1, watch_veh);
        CreateThread(NULL, 0, watch_setter, NULL, 0, NULL);
    }
    if (getenv("DAYTONA_SAMPLE")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_mainthread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, sampler, (LPVOID)(INT_PTR)atoi(getenv("DAYTONA_SAMPLE")), 0, NULL);
    }
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_t0);
    timeBeginPeriod(1);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "DaytonaHost";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));   /* host/daytona.rc */
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_wnd = CreateWindowA("DaytonaHost", "Daytona USA Deluxe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    s_dc = GetDC(s_wnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pf = ChoosePixelFormat(s_dc, &pfd);
    SetPixelFormat(s_dc, pf, &pfd);
    rc = wglCreateContext(s_dc);
    wglMakeCurrent(s_dc, rc);
    {
        typedef BOOL (WINAPI *SwapFn)(int);
        SwapFn swap = (SwapFn)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap) swap(1);
    }
    return 1;
}

void plat_shutdown(void) { }

void plat_fatal(const char *msg)
{
    if (!getenv("DAYTONA_NOGUI")) MessageBoxA(s_wnd, msg, "Daytona USA Deluxe", MB_OK | MB_ICONERROR);
}
const char *plat_data_dir(void) { return s_datadir; }
void plat_fix_case(char *path) { (void)path; }   /* the Windows file system is case-insensitive */

unsigned int plat_time_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (unsigned int)((t.QuadPart - s_t0.QuadPart) * 1000 / s_freq.QuadPart);
}

void plat_sleep_ms(unsigned int ms) { Sleep(ms); }
struct job { void (*fn)(void *); void *arg; };
static DWORD WINAPI job_thread(LPVOID p)
{
    struct job j = *(struct job *)p;
    free(p);
    j.fn(j.arg);
    return 0;
}
void plat_thread_start(void (*fn)(void *), void *arg)
{
    struct job *j = (struct job *)malloc(sizeof *j);
    HANDLE h;
    j->fn = fn; j->arg = arg;
    h = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (h) CloseHandle(h); else { free(j); fn(arg); }
}
void plat_screen_size(int *w, int *h)
{
    RECT r;
    if (s_wnd && GetClientRect(s_wnd, &r) && r.right > 0 && r.bottom > 0) { *w = r.right; *h = r.bottom; }
    else { *w = 960; *h = 544; }
}
/* DAYTONA_SHOT=N: save the frame every N swaps to build_host/shot_XXXX.bmp */
static void debug_shot(void)
{
    static int frame, every = -1, from;
    if (every < 0) {
        every = getenv("DAYTONA_SHOT") ? atoi(getenv("DAYTONA_SHOT")) : 0;
        from = getenv("DAYTONA_SHOT_FROM") ? atoi(getenv("DAYTONA_SHOT_FROM")) : 0;   /* first frame */
    }
    frame++;
    if (every > 0 && frame >= from && frame % every == 0) {
        static unsigned char *px;
        static int pxsize;
        int w, h, size;
        char name[64];
        FILE *f;
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        plat_screen_size(&w, &h);
        w &= ~3;
        size = w * h * 3;
        if (size > pxsize) { free(px); px = (unsigned char *)malloc(size); pxsize = px ? size : 0; }
        if (!px) return;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, 0x80E0 /* GL_BGR */, GL_UNSIGNED_BYTE, px);
        snprintf(name, sizeof name, "shot_%04d.bmp", frame / every);
        f = fopen(name, "wb");
        if (!f) return;
        memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
        fh.bfType = 0x4d42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + size;
        ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px, size, 1, f);
        fclose(f);
    }
}

void plat_swap(void) { debug_shot(); SwapBuffers(s_dc); }

int plat_poll(PlatPad *pad)
{
    MSG m;
    int focus;
    unsigned int i;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    memset(pad, 0, sizeof *pad);
    /* DAYTONA_KEYS="from-to:mask;..." (seconds, hex PAD_ mask): scripted input */
    {
        static char keys[512];
        static int init;
        const char *k;
        double t = plat_time_ms() / 1000.0;
        if (!init) { init = 1; if (getenv("DAYTONA_KEYS")) snprintf(keys, sizeof keys, "%s", getenv("DAYTONA_KEYS")); }
        for (k = keys; *k; ) {
            double a, b;
            unsigned int m;
            if (sscanf(k, "%lf-%lf:%x", &a, &b, &m) == 3 && t >= a && t < b) pad->buttons |= m;
            while (*k && *k != ';') k++;
            if (*k) k++;
        }
    }
    focus = GetForegroundWindow() == s_wnd;
    if (focus) {
        int alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        for (i = 0; i < PLAT_NKEYS; i++) {
            if (s_key[i] == VK_RETURN && alt) continue;   /* Alt+Enter: fullscreen, not Start */
            if (GetAsyncKeyState(s_key[i]) & 0x8000) pad->buttons |= 1u << i;
        }
    }
    {
        if (pad->buttons & PAD_LEFT) pad->lx = -128;
        if (pad->buttons & PAD_RIGHT) pad->lx = 127;
        if (pad->buttons & PAD_CROSS) pad->rtrig = 255;
        if (pad->buttons & PAD_SQUARE) pad->ltrig = 255;
    }
    return !s_quit;
}

/* ---- audio: waveOut with a feeder thread -------------------------------- */
static PlatAudioCb s_cb;
static CRITICAL_SECTION s_cs;
#define NBUF 4
#define BUFFRAMES 1024

static DWORD WINAPI audio_thread(LPVOID p)
{
    HWAVEOUT wo;
    WAVEFORMATEX f;
    WAVEHDR hdr[NBUF];
    static short buf[NBUF][BUFFRAMES * 2];
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    int i;
    (void)p;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = 44100; f.wBitsPerSample = 16;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return 0;
    memset(hdr, 0, sizeof hdr);
    for (i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwBufferLength = BUFFRAMES * 4;
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        memset(buf[i], 0, sizeof buf[i]);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    for (;;) {
        WaitForSingleObject(ev, 50);
        for (i = 0; i < NBUF; i++) {
            if (hdr[i].dwFlags & WHDR_DONE) {
                EnterCriticalSection(&s_cs);
                s_cb(buf[i], BUFFRAMES);
                LeaveCriticalSection(&s_cs);
                waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
            }
        }
    }
}

void plat_audio_start(PlatAudioCb cb)
{
    s_cb = cb;
    InitializeCriticalSection(&s_cs);
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
}

void plat_audio_lock(void) { if (s_cb) EnterCriticalSection(&s_cs); }
void plat_audio_unlock(void) { if (s_cb) LeaveCriticalSection(&s_cs); }
