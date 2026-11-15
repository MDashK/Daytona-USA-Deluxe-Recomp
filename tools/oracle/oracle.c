/* oracle.c — a tiny logging debugger for the ORIGINAL game (reference data).
 *
 * Attaches to a running "DAYTONA USA Deluxe.exe" (e.g. started windowed by
 * DxWnd), puts int3 breakpoints at the addresses listed in a probe file and,
 * on every hit, writes the requested values to a log, then lets the game run
 * on.  Used to compare the recompiled build against the original.
 *
 *   oracle.exe <probes.txt> <log.txt> [seconds] [--kill]
 *
 * probes.txt, one breakpoint per line:
 *   <hexaddr> <label> <maxhits> <stride> <expr>:<size> ...
 * expr: registers (eax..edi, esp, ebp, eip), hex constants, + and -,
 *       [expr] reads a dword.  size: bytes to dump (1, 2, 4 print as numbers).
 *   462bab poly 4000 1 [esp+4]+4:2 [esp+4]+8:2
 *
 * Build (x86): cl /O2 oracle.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBP 64
#define MAXDUMP 12
typedef struct {
    DWORD addr; char label[32]; int maxhits, stride, hits, active;
    BYTE orig;
    int ndump; char expr[MAXDUMP][96]; int size[MAXDUMP];
} Bp;

static Bp s_bp[MAXBP];
static int s_nbp;
static HANDLE s_proc;
static FILE *s_log;
static DWORD s_t0;

typedef struct { DWORD tid; HANDLE h; int pending; } Thr;
static Thr s_thr[256];
static int s_nthr;

static HANDLE thread_handle(DWORD tid)
{
    int i;
    for (i = 0; i < s_nthr; i++) if (s_thr[i].tid == tid) return s_thr[i].h;
    if (s_nthr < 256) {
        s_thr[s_nthr].tid = tid;
        s_thr[s_nthr].h = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
        s_thr[s_nthr].pending = -1;
        return s_thr[s_nthr++].h;
    }
    return NULL;
}
static Thr *thr(DWORD tid)
{
    int i;
    thread_handle(tid);
    for (i = 0; i < s_nthr; i++) if (s_thr[i].tid == tid) return &s_thr[i];
    return NULL;
}

static int rd(DWORD a, void *buf, DWORD n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(s_proc, (LPCVOID)(uintptr_t)a, buf, n, &got) && got == n;
}
static void wr_byte(DWORD a, BYTE b)
{
    DWORD old;
    SIZE_T n;
    VirtualProtectEx(s_proc, (LPVOID)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old);
    WriteProcessMemory(s_proc, (LPVOID)(uintptr_t)a, &b, 1, &n);
    VirtualProtectEx(s_proc, (LPVOID)(uintptr_t)a, 1, old, &old);
    FlushInstructionCache(s_proc, (LPCVOID)(uintptr_t)a, 1);
}

/* ---- expression evaluator */
static const char *s_p;
static CONTEXT *s_ctx;
static DWORD ev_expr(void);
static DWORD ev_term(void)
{
    DWORD v = 0;
    while (*s_p == ' ') s_p++;
    if (*s_p == '[') {
        DWORD a;
        s_p++;
        a = ev_expr();
        if (*s_p == ']') s_p++;
        if (!rd(a, &v, 4)) v = 0xDEADDEAD;
        return v;
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
        BYTE buf[256];
        DWORD a;
        int n = b->size[k], i;
        s_ctx = c; s_p = b->expr[k];
        a = ev_expr();
        fprintf(s_log, " %s=", b->expr[k]);
        if (!rd(a, buf, n)) { fprintf(s_log, "??"); continue; }
        if (n == 1) fprintf(s_log, "%02x", buf[0]);
        else if (n == 2) fprintf(s_log, "%04x", *(WORD *)buf);
        else if (n == 4) fprintf(s_log, "%08lx", *(DWORD *)buf);
        else for (i = 0; i < n; i++) fprintf(s_log, "%02x", buf[i]);
    }
    fprintf(s_log, "\n");
}

static void load_probes(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) { printf("cannot open %s\n", path); exit(1); }
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
            if (b->size[b->ndump] < 1 || b->size[b->ndump] > 256) b->size[b->ndump] = 4;
            strncpy(b->expr[b->ndump], tok, 95);
            b->ndump++;
        }
        s_nbp++;
    }
    fclose(f);
}

static DWORD find_game(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD pid = 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) do {
        if (_stricmp(pe.szExeFile, "DAYTONA USA Deluxe.exe") == 0) pid = pe.th32ProcessID;
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    return pid;
}

/* ---- hardware breakpoints (DR0..DR3): nothing is written into the code,
 * so code checksums do not see them.  Applied to every thread. */
static int s_hw_on = 1;
static void hw_apply(HANDLE th, int skip)   /* skip: slot left disabled (stepping over it) */
{
    CONTEXT c;
    int i;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    c.Dr7 = 0;
    for (i = 0; i < s_nbp && i < 4; i++) {
        DWORD a = s_bp[i].active && s_hw_on ? s_bp[i].addr : 0;
        (&c.Dr0)[i] = a;
        if (a && i != skip) c.Dr7 |= 1u << (2 * i);   /* local enable, execute, len 1 */
    }
    SetThreadContext(th, &c);
}
static void hw_apply_all(void)
{
    int i;
    for (i = 0; i < s_nthr; i++) hw_apply(s_thr[i].h, -1);
}

/* hide the debugger from PEB.BeingDebugged */
typedef LONG (WINAPI *NtQIP)(HANDLE, int, PVOID, ULONG, PULONG);
static void hide_debugger(void)
{
    struct { PVOID r1; PVOID peb; PVOID r2[2]; ULONG_PTR pid; PVOID r3; } pbi;
    NtQIP q = (NtQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    BYTE zero = 0;
    SIZE_T n;
    if (q && q(s_proc, 0, &pbi, sizeof pbi, NULL) == 0) {
        WriteProcessMemory(s_proc, (BYTE *)pbi.peb + 2, &zero, 1, &n);
        fprintf(s_log, "# PEB %p BeingDebugged cleared\n", pbi.peb);
    }
}

int main(int argc, char **argv)
{
    DWORD pid, deadline;
    int seconds = argc > 3 ? atoi(argv[3]) : 60, kill = argc > 4 && strcmp(argv[4], "--kill") == 0;
    int i, armed = 0;
    if (argc < 3) { printf("usage: oracle probes.txt log.txt [seconds] [--kill]\n"); return 1; }
    load_probes(argv[1]);
    if (s_nbp > 4) { printf("max 4 probes (hardware breakpoints)\n"); s_nbp = 4; }
    s_log = fopen(argv[2], "w");
    if (!s_log) return 1;
    printf("waiting for the game...\n");
    for (deadline = GetTickCount() + 60000; !(pid = find_game()); ) {
        if (GetTickCount() > deadline) { printf("game not found\n"); return 2; }
        Sleep(200);
    }
    Sleep(getenv("ORACLE_DELAY") ? atoi(getenv("ORACLE_DELAY")) : 1500);
    s_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!DebugActiveProcess(pid)) { printf("attach failed (%lu)\n", GetLastError()); return 3; }
    DebugSetProcessKillOnExit(FALSE);
    hide_debugger();
    s_t0 = GetTickCount();
    for (i = 0; i < s_nbp; i++) s_bp[i].active = getenv("ORACLE_NOBP") == NULL;
    printf("attached to %lu, %d probes, %d s\n", pid, s_nbp, seconds);
    deadline = GetTickCount() + seconds * 1000;
    for (;;) {
        DEBUG_EVENT ev;
        DWORD cont = DBG_CONTINUE;
        if (!WaitForDebugEvent(&ev, 200)) {
            if (GetTickCount() > deadline) break;
            continue;
        }
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord;
            DWORD at = (DWORD)(uintptr_t)er->ExceptionAddress;
            if (er->ExceptionCode == EXCEPTION_BREAKPOINT && !armed) {
                hide_debugger();
                hw_apply_all();                    /* the attach breakpoint */
                armed = 1;
            } else if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
                HANDLE th = thread_handle(ev.dwThreadId);
                Thr *t = thr(ev.dwThreadId);
                CONTEXT c;
                int hit = -1;
                c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                GetThreadContext(th, &c);
                for (i = 0; i < s_nbp; i++) if (s_bp[i].active && (c.Dr6 & (1u << i)) && c.Eip == s_bp[i].addr) hit = i;
                if (hit >= 0) {
                    Bp *b = &s_bp[hit];
                    if ((b->hits++ % b->stride) == 0) log_hit(b, &c);
                    if (b->hits >= b->maxhits * b->stride) { b->active = 0; fprintf(s_log, "# %s done\n", b->label); }
                    c.Dr6 = 0;
                    c.EFlags |= 0x100;             /* step one instruction with the slot off */
                    SetThreadContext(th, &c);
                    hw_apply(th, hit);
                    if (t) t->pending = hit;
                } else if (t && t->pending >= 0) {
                    c.Dr6 = 0;
                    SetThreadContext(th, &c);
                    hw_apply(th, -1);
                    t->pending = -1;
                } else cont = DBG_EXCEPTION_NOT_HANDLED;
            } else {
                if (ev.u.Exception.dwFirstChance == 0)
                    fprintf(s_log, "# exception %08lx at %08lx (second chance)\n", er->ExceptionCode, at);
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            hw_apply(thread_handle(ev.dwThreadId), -1);
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            fprintf(s_log, "# game exited (code %lu)\n", ev.u.ExitProcess.dwExitCode);
            fclose(s_log);
            return 0;
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
        } else if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            thread_handle(ev.dwThreadId);
        } else if (ev.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT) {
            char buf[256] = {0};
            DWORD n = ev.u.DebugString.nDebugStringLength;
            if (!ev.u.DebugString.fUnicode && rd((DWORD)(uintptr_t)ev.u.DebugString.lpDebugStringData, buf, n < 255 ? n : 255))
                fprintf(s_log, "# debugstr: %s\n", buf);
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (GetTickCount() > deadline) break;
    }
    /* finish: clear the debug registers and detach */
    s_hw_on = 0;
    hw_apply_all();
    {
        DEBUG_EVENT ev;
        while (WaitForDebugEvent(&ev, 100))
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId,
                               ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                               ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_SINGLE_STEP &&
                               ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT
                               ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE);
    }
    DebugActiveProcessStop(pid);
    if (kill) TerminateProcess(s_proc, 0);
    fprintf(s_log, "# end\n");
    fclose(s_log);
    printf("done\n");
    return 0;
}
