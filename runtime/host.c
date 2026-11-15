/* host.c — implements host.h on the real C library (and POSIX/Win32 dirs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include "host.h"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <malloc.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* malloc with a size header so HeapSize/GlobalSize work */
typedef struct { unsigned int size, magic; } Hdr;
#define HMAGIC 0x48454150u

void *host_malloc(unsigned int n)
{
    Hdr *h = (Hdr *)malloc(sizeof(Hdr) + (n ? n : 1));
    if (!h) return NULL;
    h->size = n;
    h->magic = HMAGIC;
    return h + 1;
}

void *host_calloc(unsigned int n, unsigned int sz)
{
    void *p = host_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void host_free(void *p)
{
    Hdr *h;
    if (!p) return;
    h = (Hdr *)p - 1;
    if (h->magic != HMAGIC) return;   /* not ours: leak rather than corrupt */
    h->magic = 0;
    free(h);
}

unsigned int host_msize(void *p)
{
    Hdr *h;
    if (!p) return 0;
    h = (Hdr *)p - 1;
    return h->magic == HMAGIC ? h->size : 0;
}

void *host_realloc(void *p, unsigned int n)
{
    void *q;
    unsigned int old;
    if (!p) return host_malloc(n);
    old = host_msize(p);
    q = host_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    host_free(p);
    return q;
}

int host_snprintf(char *buf, unsigned int n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int host_vsprintf_va(char *buf, const char *fmt, void *ap)
{
    return vsprintf(buf, fmt, *(va_list *)ap);
}

unsigned int host_strlen(const char *s) { return (unsigned int)strlen(s); }
int host_stricmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int host_strnicmp(const char *a, const char *b, unsigned int n)
{
    while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; n--; }
    return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0;
}
char *host_strcpy(char *d, const char *s) { return strcpy(d, s); }
char *host_strncpy(char *d, const char *s, unsigned int n) { return strncpy(d, s, n); }
char *host_strcat(char *d, const char *s) { return strcat(d, s); }
char *host_strchr(const char *s, int c) { return strchr(s, c); }
char *host_strrchr(const char *s, int c) { return strrchr(s, c); }
int host_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int host_memcmp(const void *a, const void *b, unsigned int n) { return memcmp(a, b, n); }
void *host_memmove(void *d, const void *s, unsigned int n) { return memmove(d, s, n); }
int host_atoi(const char *s) { return atoi(s); }
const char *host_strstr_i(const char *h, const char *n)
{
    size_t ln = strlen(n);
    for (; *h; h++) if (host_strnicmp(h, n, (unsigned)ln) == 0) return h;
    return NULL;
}

void *host_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int host_fread(void *f, void *buf, unsigned int n) { return (int)fread(buf, 1, n, (FILE *)f); }
int host_fwrite(void *f, const void *buf, unsigned int n) { return (int)fwrite(buf, 1, n, (FILE *)f); }
int host_fseek(void *f, int off, int whence) { return fseek((FILE *)f, off, whence); }
int host_ftell(void *f) { return (int)ftell((FILE *)f); }
void host_fclose(void *f) { fclose((FILE *)f); }
int host_fexists(const char *path) { FILE *f = fopen(path, "rb"); if (f) { fclose(f); return 1; } return 0; }

#if defined(_WIN32)
int host_mkdir(const char *path) { return _mkdir(path); }
#else
int host_mkdir(const char *path) { return mkdir(path, 0777); }
#endif
int host_remove(const char *path) { return remove(path); }

#if defined(_WIN32)
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first; } WDir;
void *host_opendir(const char *dir)
{
    char pat[600];
    WDir *d = (WDir *)calloc(1, sizeof *d);
    snprintf(pat, sizeof pat, "%s/*", dir);
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}
int host_readdir(void *dv, char *name, unsigned int namesz, int *isdir, unsigned int *size)
{
    WDir *d = (WDir *)dv;
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return 0;
    d->first = 0;
    snprintf(name, namesz, "%s", d->fd.cFileName);
    *isdir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *size = d->fd.nFileSizeLow;
    return 1;
}
void host_closedir(void *dv) { WDir *d = (WDir *)dv; FindClose(d->h); free(d); }
#else
typedef struct { DIR *d; char path[512]; } PDir;
void *host_opendir(const char *dir)
{
    PDir *p;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    p = (PDir *)calloc(1, sizeof *p);
    p->d = d;
    snprintf(p->path, sizeof p->path, "%s", dir);
    return p;
}
int host_readdir(void *dv, char *name, unsigned int namesz, int *isdir, unsigned int *size)
{
    PDir *p = (PDir *)dv;
    struct dirent *e = readdir(p->d);
    struct stat st;
    char full[1024];
    if (!e) return 0;
    snprintf(name, namesz, "%s", e->d_name);
    snprintf(full, sizeof full, "%s/%s", p->path, e->d_name);
    if (stat(full, &st) == 0) { *isdir = S_ISDIR(st.st_mode); *size = (unsigned int)st.st_size; }
    else { *isdir = 0; *size = 0; }
    return 1;
}
void host_closedir(void *dv) { PDir *p = (PDir *)dv; closedir(p->d); free(p); }
#endif

void host_localtime(HostTime *t)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    t->year = tm->tm_year + 1900; t->month = tm->tm_mon + 1; t->wday = tm->tm_wday;
    t->day = tm->tm_mday; t->hour = tm->tm_hour; t->min = tm->tm_min; t->sec = tm->tm_sec; t->ms = 0;
}

void host_exit(int code) { exit(code); }
int getenv_flag(const char *name) { const char *v = getenv(name); return v && *v && *v != '0'; }
