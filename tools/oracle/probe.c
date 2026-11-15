/* probe.c — in-process probes for the ORIGINAL game (reference data).
 *
 * Injected into "DAYTONA USA Deluxe.exe" by oracle.exe (no debugger is
 * attached: the game quits when it sees one).  Reads probes.txt from its own
 * directory, puts hardware execute breakpoints (DR0..DR3) on the game's
 * threads and logs the requested values from a vectored exception handler
 * into probe_log.txt.  The code is never modified.
 *
 * probes.txt, one breakpoint per line (max 4):
 *   <hexaddr> <label> <maxhits> <stride> <expr>:<size> ...
 * expr: eax..edi, esp, ebp, eip, hex constants, + and -, [expr] = dword read.
 *
 * Build (x86): cl /LD /O2 probe.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBP 4
#define MAXDUMP 48
typedef struct {
    DWORD addr; char label[32]; int maxhits, stride, hits, active;
    int ndump; char expr[MAXDUMP][96]; int size[MAXDUMP];
} Bp;

static Bp s_bp[MAXBP];
static int s_nbp;
static FILE *s_log;
static DWORD s_t0;
static CRITICAL_SECTION s_cs;
static volatile LONG s_pending_tid[MAXBP];   /* thread stepping over slot i */

static int readable(DWORD a, DWORD n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (DWORD)(uintptr_t)mi.BaseAddress + (DWORD)mi.RegionSize;
}

static const char *s_p;
static CONTEXT *s_ctx;
static DWORD ev_expr(void);
static DWORD ev_term(void)
{
    DWORD v;
    while (*s_p == ' ') s_p++;
    if (*s_p == '[') {
        DWORD a;
        s_p++;
        a = ev_expr();
        if (*s_p == ']') s_p++;
        return readable(a, 4) ? *(DWORD *)(uintptr_t)a : 0xDEADDEAD;
    }
    {
        static const char *rn[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp", "eip" };
        DWORD rv[9];
        int i;
        rv[0] = s_ctx->Eax; rv[1] = s_ctx->Ebx; rv[2] = s_ctx->Ecx; rv[3] = s_ctx->Edx;
        rv[4] = s_ctx->Esi; rv[5] = s_ctx->Edi; rv[6] = s_ctx->Esp; rv[7] = s_ctx->Ebp; rv[8] = s_ctx->Eip;
        for (i = 0; i < 9; i++)
            if (_strnicmp(s_p, rn[i], 3) == 0) { s_p += 3; return rv[i]; }
    }
    v = strtoul(s_p, (char **)&s_p, 16);
    return v;
}
static DWORD ev_expr(void)
{
    DWORD v = ev_term();
    for (;;) {
        while (*s_p == ' ') s_p++;
        if (*s_p == '+') { s_p++; v += ev_term(); }
        else if (*s_p == '-') { s_p++; v -= ev_term(); }
        else return v;
    }
}

static void log_hit(Bp *b, CONTEXT *c)
{
    int k;
    fprintf(s_log, "%.3f %s #%d", (GetTickCount() - s_t0) / 1000.0, b->label, b->hits);
    for (k = 0; k < b->ndump; k++) {
        DWORD a;
        int n = b->size[k], i;
        const BYTE *p;
        s_ctx = c; s_p = b->expr[k];
        a = ev_expr();
        fprintf(s_log, " %s=", b->expr[k]);
        if (n == 0) { fprintf(s_log, "%08lx", a); continue; }   /* :0 = the value itself */
        if (!readable(a, n)) { fprintf(s_log, "??"); continue; }
        p = (const BYTE *)(uintptr_t)a;
        if (n == 1) fprintf(s_log, "%02x", p[0]);
        else if (n == 2) fprintf(s_log, "%04x", *(const WORD *)p);
        else if (n == 4) fprintf(s_log, "%08lx", *(const DWORD *)p);
        else for (i = 0; i < n; i++) fprintf(s_log, "%02x", p[i]);
    }
    fprintf(s_log, "\n");
}

static DWORD dr7_all(void)
{
    DWORD d = 0;
    int i;
    for (i = 0; i < s_nbp; i++) if (s_bp[i].active) d |= 1u << (2 * i);
    return d;
}

static LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    DWORD tid = GetCurrentThreadId();
    int i;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    /* stepped over a breakpoint: re-arm */
    for (i = 0; i < s_nbp; i++) if (s_pending_tid[i] == (LONG)tid) {
        s_pending_tid[i] = 0;
        c->EFlags &= ~0x100u;
        c->Dr6 = 0;
        c->Dr7 = dr7_all();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (i = 0; i < s_nbp; i++) {
        Bp *b = &s_bp[i];
        if (!b->active || !(c->Dr6 & (1u << i)) || c->Eip != b->addr) continue;
        EnterCriticalSection(&s_cs);
        if ((b->hits++ % b->stride) == 0 && s_log) log_hit(b, c);
        if (b->hits >= b->maxhits * b->stride) {
            b->active = 0;
            fprintf(s_log, "# %s done\n", b->label);
            fflush(s_log);
        }
        LeaveCriticalSection(&s_cs);
        c->Dr6 = 0;
        c->Dr7 = dr7_all() & ~(3u << (2 * i));     /* this slot off for one instruction */
        if (b->active) { c->EFlags |= 0x100; s_pending_tid[i] = (LONG)tid; }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void set_thread(DWORD tid, int on)
{
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    CONTEXT c;
    int i;
    if (!th) return;
    SuspendThread(th);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &c)) {
        for (i = 0; i < MAXBP; i++) (&c.Dr0)[i] = i < s_nbp && on ? s_bp[i].addr : 0;
        c.Dr7 = on ? dr7_all() : 0;
        SetThreadContext(th, &c);
    }
    ResumeThread(th);
    CloseHandle(th);
}
static void set_all_threads(int on)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) set_thread(te.th32ThreadID, on);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static void load_probes(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    if (!f) { fprintf(s_log, "# cannot open %s\n", path); return; }
    while (fgets(line, sizeof line, f) && s_nbp < MAXBP) {
        Bp *b = &s_bp[s_nbp];
        char *tok, *ctx = NULL;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        memset(b, 0, sizeof *b);
        tok = strtok_s(line, " \t\r\n", &ctx); if (!tok) continue;
        b->addr = strtoul(tok, NULL, 16);
        tok = strtok_s(NULL, " \t\r\n", &ctx); strncpy(b->label, tok ? tok : "bp", 31);
        tok = strtok_s(NULL, " \t\r\n", &ctx); b->maxhits = tok ? atoi(tok) : 100;
        tok = strtok_s(NULL, " \t\r\n", &ctx); b->stride = tok ? atoi(tok) : 1;
        if (b->stride < 1) b->stride = 1;
        while ((tok = strtok_s(NULL, " \t\r\n", &ctx)) != NULL && b->ndump < MAXDUMP) {
            char *colon = strrchr(tok, ':');
            b->size[b->ndump] = colon ? atoi(colon + 1) : 4;
            if (colon) *colon = 0;
            if (b->size[b->ndump] < 0 || b->size[b->ndump] > 256) b->size[b->ndump] = 4;
            strncpy(b->expr[b->ndump], tok, 95);
            b->ndump++;
        }
        b->active = 1;
        s_nbp++;
    }
    fclose(f);
}

/* runs in its own thread: arm, keep new threads armed, flush the log */
static DWORD WINAPI worker(LPVOID p)
{
    char dir[MAX_PATH], path[MAX_PATH], *slash;
    int secs = 0, total;
    GetModuleFileNameA((HMODULE)p, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    sprintf(path, "%sprobe_log.txt", dir);
    s_log = fopen(path, "w");
    if (!s_log) return 0;
    sprintf(path, "%sprobes.txt", dir);
    load_probes(path);
    sprintf(path, "%sprobe_secs.txt", dir);
    {
        FILE *f = fopen(path, "r");
        total = 120;
        if (f) { if (fscanf(f, "%d", &total) != 1) total = 120; fclose(f); }
    }
    s_t0 = GetTickCount();
    fprintf(s_log, "# %d probes, %d s\n", s_nbp, total);
    fflush(s_log);
    InitializeCriticalSection(&s_cs);
    AddVectoredExceptionHandler(1, veh);
    for (secs = 0; secs < total; secs++) {
        set_all_threads(1);      /* also catches threads created since */
        Sleep(1000);
        EnterCriticalSection(&s_cs);
        if (s_log) fflush(s_log);
        LeaveCriticalSection(&s_cs);
    }
    set_all_threads(0);
    EnterCriticalSection(&s_cs);
    fprintf(s_log, "# end\n");
    fclose(s_log);
    s_log = NULL;
    LeaveCriticalSection(&s_cs);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, worker, h, 0, NULL);
    }
    return TRUE;
}
