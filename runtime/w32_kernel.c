/* w32_kernel.c — KERNEL32 (+ a little ADVAPI32) on the host. */
#include "w32.h"

/* ---- handle table ------------------------------------------------------- */
#define MAXH 256
static struct { int type; void *obj; } s_h[MAXH];

int h_new(int type, void *obj)
{
    int i;
    for (i = 1; i < MAXH; i++) {
        if (!s_h[i].type) { s_h[i].type = type; s_h[i].obj = obj; return i; }
    }
    rt_fatal("out of kernel handles");
    return 0;
}

static int h_index(HANDLE h)
{
    recomp_uptr v = (recomp_uptr)h;
    if (v < 0x100 || (v & 3)) return -1;
    v = (v - 0x100) / 4;
    return v < MAXH ? (int)v : -1;
}

void *h_get(HANDLE h, int type)
{
    int i = h_index(h);
    if (i <= 0 || s_h[i].type != type) return NULL;
    return s_h[i].obj;
}

void h_free(HANDLE h)
{
    int i = h_index(h);
    if (i > 0) { s_h[i].type = 0; s_h[i].obj = NULL; }
}

static DWORD s_lasterr;
DWORD w32_GetLastError(void) { return s_lasterr; }

/* ---- files ---------------------------------------------------------------- */
#define INVALID_HANDLE ((HANDLE)(recomp_uptr)0xffffffffu)

HANDLE w32_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                       DWORD disp, DWORD flags, HANDLE tmpl)
{
    char path[512];
    const char *mode;
    void *fp;
    W32File *f;
    (void)share; (void)sa; (void)flags; (void)tmpl;
    rt_map_path(name, path, sizeof path);
    /* 1 CREATE_NEW 2 CREATE_ALWAYS 3 OPEN_EXISTING 4 OPEN_ALWAYS 5 TRUNCATE_EXISTING */
    if (disp == 2 || disp == 5 || (disp == 1 && !host_fexists(path))) mode = "wb";
    else if (disp == 4 && !host_fexists(path)) mode = "w+b";
    else if (access & 0x40000000u) mode = "r+b";
    else mode = "rb";
    fp = host_fopen(path, mode);
    if (!fp && mode[0] == 'r' && mode[1] == '+') fp = host_fopen(path, "rb");
    if (!fp) {
        rt_log("CreateFileA: cannot open %s (%s)\n", name, path);
        s_lasterr = 2;
        return INVALID_HANDLE;
    }
    f = (W32File *)host_calloc(1, sizeof *f);
    f->fp = fp;
    host_snprintf(f->path, sizeof f->path, "%s", path);
    return H_TO_HANDLE(h_new(H_FILE, f));
}

BOOL w32_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    W32File *f = (W32File *)h_get(h, H_FILE);
    int r;
    (void)ov;
    if (!f) { if (got) *got = 0; return 0; }
    r = host_fread(f->fp, buf, n);
    if (got) *got = r < 0 ? 0 : (DWORD)r;
    return 1;
}

BOOL w32_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
    W32File *f = (W32File *)h_get(h, H_FILE);
    int r;
    (void)ov;
    if (!f) { if (put) *put = 0; return 0; }
    r = host_fwrite(f->fp, buf, n);
    if (put) *put = r < 0 ? 0 : (DWORD)r;
    return 1;
}

DWORD w32_SetFilePointer(HANDLE h, LONG dist, PLONG hi, DWORD method)
{
    W32File *f = (W32File *)h_get(h, H_FILE);
    if (hi) *hi = 0;
    if (!f) return 0xffffffffu;
    host_fseek(f->fp, dist, (int)method);
    return (DWORD)host_ftell(f->fp);
}

DWORD w32_GetFileSize(HANDLE h, LPDWORD hi)
{
    W32File *f = (W32File *)h_get(h, H_FILE);
    int cur, end;
    if (hi) *hi = 0;
    if (!f) return 0xffffffffu;
    cur = host_ftell(f->fp);
    host_fseek(f->fp, 0, 2);
    end = host_ftell(f->fp);
    host_fseek(f->fp, cur, 0);
    return (DWORD)end;
}

BOOL w32_FlushFileBuffers(HANDLE h) { (void)h; return 1; }
DWORD w32_GetFileType(HANDLE h) { (void)h; return 1; }

/* the LZ layer needs the host file of a handle */
void *w32_file_fp(HANDLE h)
{
    W32File *f = (W32File *)h_get(h, H_FILE);
    return f ? f->fp : NULL;
}

typedef struct W32Find { void *dir; char pat[128]; char dirpath[512]; } W32Find;

static int wild_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *s; s++) if (wild_match(pat, s)) return 1;
            return wild_match(pat, s);
        }
        if (!*s) return 0;
        if (*pat != '?' && (*pat | 32) != (*s | 32)) return 0;
    }
    return *s == 0;
}

static BOOL find_next(W32Find *fd, LPWIN32_FIND_DATAA out)
{
    char name[260];
    int isdir;
    unsigned int size;
    if (!fd->dir) return 0;
    while (host_readdir(fd->dir, name, sizeof name, &isdir, &size)) {
        if (!wild_match(fd->pat, name)) continue;
        memset(out, 0, sizeof *out);
        out->dwFileAttributes = isdir ? 0x10 : 0x80;
        out->nFileSizeLow = size;
        host_snprintf(out->cFileName, sizeof out->cFileName, "%s", name);
        return 1;
    }
    return 0;
}

HANDLE w32_FindFirstFileA(LPCSTR spec, LPWIN32_FIND_DATAA out)
{
    char path[512];
    char *slash;
    W32Find *fd;
    rt_map_path(spec, path, sizeof path);
    fd = (W32Find *)host_calloc(1, sizeof *fd);
    slash = host_strrchr(path, '/');
    if (slash) {
        host_snprintf(fd->pat, sizeof fd->pat, "%s", slash + 1);
        *slash = 0;
        host_snprintf(fd->dirpath, sizeof fd->dirpath, "%s", path);
    } else {
        host_snprintf(fd->pat, sizeof fd->pat, "%s", path);
        host_snprintf(fd->dirpath, sizeof fd->dirpath, ".");
    }
    if (!host_strchr(fd->pat, '*') && !host_strchr(fd->pat, '?')) {
        /* a plain name: test that one file instead of reading the whole
         * directory (slow on the Vita's memory card; the sound loader checks
         * a file for every sound it loads) */
        char full[640];
        void *f;
        host_snprintf(full, sizeof full, "%s/%s", fd->dirpath, fd->pat);
        if ((f = host_fopen(full, "rb")) != NULL) {
            memset(out, 0, sizeof *out);
            out->dwFileAttributes = 0x80;
            host_fseek(f, 0, 2);
            out->nFileSizeLow = (unsigned int)host_ftell(f);
            host_fclose(f);
            host_snprintf(out->cFileName, sizeof out->cFileName, "%s", fd->pat);
            fd->dir = NULL;                     /* FindNextFile: no more */
            return H_TO_HANDLE(h_new(H_FIND, fd));
        }
    }
    fd->dir = host_opendir(fd->dirpath);
    if (!fd->dir || !find_next(fd, out)) {
        if (fd->dir) host_closedir(fd->dir);
        host_free(fd);
        s_lasterr = 2;
        return INVALID_HANDLE;
    }
    return H_TO_HANDLE(h_new(H_FIND, fd));
}

BOOL w32_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA out)
{
    W32Find *fd = (W32Find *)h_get(h, H_FIND);
    if (!fd) return 0;
    if (!find_next(fd, out)) { s_lasterr = 18; return 0; }
    return 1;
}

BOOL w32_FindClose(HANDLE h)
{
    W32Find *fd = (W32Find *)h_get(h, H_FIND);
    if (!fd) return 0;
    if (fd->dir) host_closedir(fd->dir);
    host_free(fd);
    h_free(h);
    return 1;
}

BOOL w32_CloseHandle(HANDLE h)
{
    int i = h_index(h);
    if (i <= 0) return 0;
    if (s_h[i].type == H_FILE) {
        W32File *f = (W32File *)s_h[i].obj;
        host_fclose(f->fp);
        host_free(f);
    } else if (s_h[i].type == H_FIND) {
        return w32_FindClose(h);
    }
    h_free(h);
    return 1;
}

BOOL w32_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
    char path[512];
    (void)sa;
    rt_map_path(name, path, sizeof path);
    return host_mkdir(path) == 0;
}

BOOL w32_DeleteFileA(LPCSTR name)
{
    char path[512];
    rt_map_path(name, path, sizeof path);
    return host_remove(path) == 0;
}

/* One drive, Q:, reported as a CD-ROM whose root is the data directory
 * (the game looks for its CD; all paths are mapped into the data dir). */
DWORD w32_GetLogicalDrives(void) { return 1u << ('Q' - 'A') | (1u << 2); }
UINT w32_GetDriveTypeA(LPCSTR root)
{
    if (root && (root[0] | 32) == 'q') return 5;   /* DRIVE_CDROM */
    return 3;                                       /* DRIVE_FIXED */
}

/* ---- heap / global memory -------------------------------------------- */
HANDLE w32_GetProcessHeap(void) { return (HANDLE)(recomp_uptr)0x50000; }
HANDLE w32_HeapCreate(DWORD o, SIZE_T i, SIZE_T m) { (void)o; (void)i; (void)m; return (HANDLE)(recomp_uptr)0x50000; }
BOOL w32_HeapDestroy(HANDLE h) { (void)h; return 1; }

LPVOID w32_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n)
{
    (void)heap; (void)flags;
    /* always zeroed: fresh heap pages on Win9x were, and the game relies on it */
    return host_calloc(1, n);
}

BOOL w32_HeapFree(HANDLE heap, DWORD flags, LPVOID p) { (void)heap; (void)flags; host_free(p); return 1; }

LPVOID w32_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T n)
{
    unsigned int old = host_msize(p);
    void *q = host_realloc(p, n);
    (void)heap;
    if (q && (flags & 8) && n > old) memset((char *)q + old, 0, n - old);
    return q;
}

SIZE_T w32_HeapSize(HANDLE heap, DWORD flags, LPCVOID p) { (void)heap; (void)flags; return host_msize((void *)p); }
SIZE_T w32_HeapCompact(HANDLE heap, DWORD flags) { (void)heap; (void)flags; return 0x1000000; }

/* The blocks GlobalAlloc / VirtualAlloc handed out and not freed yet.  The
 * game frees some twice: FUN_00414ac6 (a sound's sample data) frees [0x48]
 * without clearing it, and the sound banks are freed again at the next scene
 * change.  Windows just fails such a GlobalFree; here it read a released
 * block (a crash on Windows after a race) or, worse, could free a block that
 * reused the address.  Only live blocks are freed now. */
#define LIVE_N 16384                       /* power of two */
static void *s_live[LIVE_N];
#define LIVE_GONE ((void *)1)
static unsigned live_hash(void *p) { return (unsigned)(((recomp_uptr)p >> 3) * 2654435761u) & (LIVE_N - 1); }
static int s_live_full;
static void live_add(void *p)
{
    unsigned h = live_hash(p), i;
    if (!p) return;
    for (i = 0; i < LIVE_N; i++, h = (h + 1) & (LIVE_N - 1))
        if (!s_live[h] || s_live[h] == LIVE_GONE) { s_live[h] = p; return; }
    s_live_full = 1;                       /* table full: frees are no longer checked */
}
static int live_remove(void *p)
{
    unsigned h = live_hash(p), i;
    for (i = 0; i < LIVE_N && s_live[h]; i++, h = (h + 1) & (LIVE_N - 1))
        if (s_live[h] == p) { s_live[h] = LIVE_GONE; return 1; }
    return s_live_full;
}
static void live_free(void *p, const char *what)
{
    static int logged;
    if (!p) return;
    if (live_remove(p)) host_free(p);
    else if (logged < 4) { logged++; rt_log("%s(%p): not a live block (freed twice by the game), ignored\n", what, p); }
}

/* GlobalAlloc: the handle is the pointer itself (as with GMEM_FIXED). */
HGLOBAL w32_GlobalAlloc(UINT flags, SIZE_T n) { void *p = host_calloc(1, n); (void)flags; live_add(p); return p; }
HGLOBAL w32_GlobalFree(HGLOBAL h) { live_free(h, "GlobalFree"); return NULL; }
LPVOID w32_GlobalLock(HGLOBAL h) { return h; }
BOOL w32_GlobalUnlock(HGLOBAL h) { (void)h; return 1; }
HGLOBAL w32_GlobalHandle(LPCVOID p) { return (HGLOBAL)p; }

LPVOID w32_VirtualAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD p) { void *q = host_calloc(1, n); (void)a; (void)t; (void)p; live_add(q); return q; }
BOOL w32_VirtualFree(LPVOID a, SIZE_T n, DWORD t) { (void)n; if (t & 0x8000) live_free(a, "VirtualFree"); return 1; }

/* ---- strings ---------------------------------------------------------------- */
LPSTR w32_lstrcpyA(LPSTR d, LPCSTR s) { return host_strcpy(d, s); }
LPSTR w32_lstrcatA(LPSTR d, LPCSTR s) { return host_strcat(d, s); }
int w32_lstrlenA(LPCSTR s) { return s ? (int)host_strlen(s) : 0; }
int w32_lstrcmpA(LPCSTR a, LPCSTR b) { int r = host_strcmp(a, b); return r < 0 ? -1 : r > 0; }

int w32_MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR s, int n, LPWSTR d, int dn)
{
    int i, len;
    (void)cp; (void)fl;
    len = n < 0 ? (int)host_strlen(s) + 1 : n;
    if (!dn) return len;
    for (i = 0; i < len && i < dn; i++) d[i] = (unsigned char)s[i];
    return i;
}

int w32_WideCharToMultiByte(UINT cp, DWORD fl, LPCWSTR s, int n, LPSTR d, int dn, LPCSTR def, LPBOOL used)
{
    int i, len = 0;
    (void)cp; (void)fl; (void)def; (void)used;
    if (n < 0) { while (s[len]) len++; len++; } else len = n;
    if (!dn) return len;
    for (i = 0; i < len && i < dn; i++) d[i] = (char)(s[i] < 256 ? s[i] : '?');
    return i;
}

void w32_OutputDebugStringA(LPCSTR s) { rt_log("[debug] %s%s", s, (s && s[0] && s[host_strlen(s) - 1] == '\n') ? "" : "\n"); }

/* ---- time / locale ------------------------------------------------------- */
void w32_GetLocalTime(LPSYSTEMTIME st)
{
    HostTime t;
    host_localtime(&t);
    st->wYear = (WORD)t.year; st->wMonth = (WORD)t.month; st->wDayOfWeek = (WORD)t.wday;
    st->wDay = (WORD)t.day; st->wHour = (WORD)t.hour; st->wMinute = (WORD)t.min;
    st->wSecond = (WORD)t.sec; st->wMilliseconds = 0;
}

LANGID w32_GetSystemDefaultLangID(void) { return 0x0409; }   /* English (US) */
LCID w32_GetSystemDefaultLCID(void) { return 0x0409; }
int w32_GetLocaleInfoA(LCID lc, LCTYPE type, LPSTR buf, int n)
{
    const char *v = "1";
    (void)lc;
    if (type == 0x1001) v = "English";
    else if (type == 0x1002) v = "United States";
    else if (type == 0x3) v = "ENU";
    else if (type == 0x59) v = "en";
    if (!n) return (int)host_strlen(v) + 1;
    host_snprintf(buf, n, "%s", v);
    return (int)host_strlen(buf) + 1;
}

/* ---- modules -------------------------------------------------------------- */
HMODULE w32_GetModuleHandleA(LPCSTR name) { (void)name; return GAME_HINSTANCE; }

DWORD w32_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n)
{
    (void)m;
    host_snprintf(buf, n, "Q:\\DAYTONA USA Deluxe.exe");
    return host_strlen(buf);
}

#define FAKE_DXERROR ((HMODULE)(recomp_uptr)0x76000000)
#define FAKE_USER32  ((HMODULE)(recomp_uptr)0x77000000)

HMODULE w32_LoadLibraryA(LPCSTR name)
{
    rt_log("LoadLibraryA(%s)\n", name);
    if (!name) return NULL;
    if (host_stricmp(name, "Resource.dll") == 0) return res_dll_handle();
    if (host_stricmp(name, "DXError.dll") == 0) return FAKE_DXERROR;
    if (host_stricmp(name, "user32.dll") == 0 || host_stricmp(name, "user32") == 0) return FAKE_USER32;
    return NULL;
}

BOOL w32_FreeLibrary(HMODULE m) { (void)m; return 1; }

/* DXError.dll: DDErrorToResourceID & co. map an HRESULT to a string id in
 * the DLL's string table; one generic message is enough here. */
static UINT dxerr_to_resid(HRESULT hr) { rt_log("DirectX error 0x%08x\n", (unsigned)hr); return 0; }

FARPROC w32_GetProcAddress(HMODULE m, LPCSTR name)
{
    if ((recomp_uptr)name <= 0xffff) { rt_log("GetProcAddress(#%u): not provided\n", (unsigned)(recomp_uptr)name); return NULL; }
    if (m == FAKE_DXERROR && host_strstr_i(name, "ErrorToResourceID")) return (FARPROC)dxerr_to_resid;
    rt_log("GetProcAddress(%s): not provided\n", name);
    return NULL;
}

/* ---- sync objects ------------------------------------------------------- */
HANDLE w32_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) { (void)sa; (void)own; (void)name; return H_TO_HANDLE(h_new(H_MUTEX, NULL)); }
HANDLE w32_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL man, BOOL init, LPCSTR name) { (void)sa; (void)man; (void)init; (void)name; return H_TO_HANDLE(h_new(H_EVENT, NULL)); }
DWORD w32_WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }

/* ---- process ------------------------------------------------------------- */
void w32_ExitProcess(UINT code) { rt_log("ExitProcess(%u)\n", code); host_exit((int)code); }
BOOL w32_TerminateProcess(HANDLE p, UINT code) { (void)p; w32_ExitProcess(code); return 1; }
HANDLE w32_GetCurrentProcess(void) { return (HANDLE)(recomp_uptr)0xffffffffu; }
DWORD w32_GetVersion(void) { return 0xc0000a04; }   /* Windows 95 */
LPSTR w32_GetCommandLineA(void) { static char cl[] = "DAYTONA.EXE"; return cl; }
void w32_RaiseException(DWORD code, DWORD flags, DWORD n, ULONG_PTR *args)
{
    (void)flags; (void)n; (void)args;
    rt_fatal("RaiseException(0x%08x)", code);
}

BOOL w32_GetUserNameA(LPSTR buf, LPDWORD n)
{
    host_snprintf(buf, *n, "PLAYER");
    *n = 7;
    return 1;
}
