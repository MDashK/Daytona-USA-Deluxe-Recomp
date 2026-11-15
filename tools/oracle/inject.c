/* inject.c — load probe.dll into the running original game (no debugger).
 *
 *   inject.exe <probes.txt> <seconds> [delay_ms] [--kill]
 *
 * Copies the probe file to probes.txt next to probe.dll, waits for
 * "DAYTONA USA Deluxe.exe", waits delay_ms, injects probe.dll
 * (CreateRemoteThread + LoadLibraryA), waits <seconds> and, with --kill,
 * closes the game.  The log is probe_log.txt next to probe.dll.
 *
 * Build (x86): cl /O2 inject.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* the game's startup "Direct3D" device dialog: press OK for the user */
static DWORD s_game_pid;
static BOOL CALLBACK find_dialog(HWND w, LPARAM lp)
{
    DWORD pid = 0;
    char title[64];
    (void)lp;
    GetWindowThreadProcessId(w, &pid);
    if (pid == s_game_pid && IsWindowVisible(w) && GetWindowTextA(w, title, sizeof title) &&
        strcmp(title, "Direct3D") == 0) {
        PostMessageA(w, WM_COMMAND, IDOK, 0);
        printf("pressed OK on the Direct3D dialog\n");
        return FALSE;
    }
    return TRUE;
}
static void press_ok_for(DWORD pid, int ms)
{
    DWORD t0 = GetTickCount();
    s_game_pid = pid;
    while ((int)(GetTickCount() - t0) < ms) {
        if (!EnumWindows(find_dialog, 0)) { Sleep(500); return; }
        Sleep(100);
    }
}

int main(int argc, char **argv)
{
    char dir[MAX_PATH], dll[MAX_PATH], path[MAX_PATH], *slash;
    int secs, delay, kill = 0, i;
    DWORD pid, t0;
    HANDLE proc, th;
    void *mem;
    SIZE_T n;
    FILE *f;
    if (argc < 3) { printf("usage: inject probes.txt seconds [delay_ms] [--kill]\n"); return 1; }
    secs = atoi(argv[2]);
    delay = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 3000;
    for (i = 3; i < argc; i++) if (strcmp(argv[i], "--kill") == 0) kill = 1;

    GetModuleFileNameA(NULL, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    sprintf(dll, "%sprobe.dll", dir);
    if (!CopyFileA(argv[1], (sprintf(path, "%sprobes.txt", dir), path), FALSE) && _stricmp(argv[1], "probes.txt") != 0) {
        printf("cannot copy %s\n", argv[1]);
        return 1;
    }
    sprintf(path, "%sprobe_secs.txt", dir);
    if ((f = fopen(path, "w")) != NULL) { fprintf(f, "%d\n", secs); fclose(f); }

    printf("waiting for the game...\n");
    for (t0 = GetTickCount(); !(pid = find_game()); Sleep(200))
        if (GetTickCount() - t0 > 60000) { printf("game not found\n"); return 2; }
    t0 = GetTickCount();
    press_ok_for(pid, delay);
    if ((int)(GetTickCount() - t0) < delay) Sleep(delay - (GetTickCount() - t0));
    proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 3; }
    mem = VirtualAllocEx(proc, NULL, MAX_PATH, MEM_COMMIT, PAGE_READWRITE);
    WriteProcessMemory(proc, mem, dll, strlen(dll) + 1, &n);
    th = CreateRemoteThread(proc, NULL, 0,
                            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
                            mem, 0, NULL);
    if (!th) { printf("CreateRemoteThread failed (%lu)\n", GetLastError()); return 4; }
    WaitForSingleObject(th, 10000);
    {
        DWORD code = 0;
        GetExitCodeThread(th, &code);
        printf("injected into %lu (module %08lx), running %d s\n", pid, code, secs);
    }
    CloseHandle(th);
    for (i = 0; i < secs + 2; i++) {
        DWORD code = STILL_ACTIVE;
        Sleep(1000);
        GetExitCodeProcess(proc, &code);
        if (code != STILL_ACTIVE) { printf("game exited (code %lu) after %d s\n", code, i + 1); return 0; }
    }
    if (kill) TerminateProcess(proc, 0);
    printf("done\n");
    return 0;
}
