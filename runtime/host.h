/* host.h — host libc services for the Win32 shims.
 *
 * The shim files include Ghidra's types.h (which reuses CRT names such as
 * FILE and _locale_t), so they cannot include system headers.  This header
 * uses only basic C types; host.c implements it with the real libc.
 */
#ifndef HOST_H
#define HOST_H

void *host_malloc(unsigned int n);
void *host_calloc(unsigned int n, unsigned int sz);
void *host_realloc(void *p, unsigned int n);
void  host_free(void *p);
unsigned int host_msize(void *p);

int   host_vsnprintf(char *buf, unsigned int n, const char *fmt, char *va_dummy);
int   host_snprintf(char *buf, unsigned int n, const char *fmt, ...);
int   host_vsprintf_va(char *buf, const char *fmt, void *va_list_ptr);
unsigned int host_strlen(const char *s);
int   host_stricmp(const char *a, const char *b);
int   host_strnicmp(const char *a, const char *b, unsigned int n);
char *host_strcpy(char *d, const char *s);
char *host_strncpy(char *d, const char *s, unsigned int n);
char *host_strcat(char *d, const char *s);
char *host_strchr(const char *s, int c);
char *host_strrchr(const char *s, int c);
int   host_strcmp(const char *a, const char *b);
int   host_memcmp(const void *a, const void *b, unsigned int n);
void *host_memmove(void *d, const void *s, unsigned int n);
int   host_atoi(const char *s);
const char *host_strstr_i(const char *hay, const char *needle);

/* files: modes "rb", "r+b", "wb" */
void *host_fopen(const char *path, const char *mode);
int   host_fread(void *f, void *buf, unsigned int n);
int   host_fwrite(void *f, const void *buf, unsigned int n);
int   host_fseek(void *f, int off, int whence);
int   host_ftell(void *f);
void  host_fclose(void *f);
int   host_fexists(const char *path);
int   host_mkdir(const char *path);
int   host_remove(const char *path);

/* directory scan; returns a handle or 0 */
void *host_opendir(const char *dir);
int   host_readdir(void *d, char *name, unsigned int namesz, int *isdir, unsigned int *size);
void  host_closedir(void *d);

typedef struct HostTime { int year, month, wday, day, hour, min, sec, ms; } HostTime;
void  host_localtime(HostTime *t);
void  host_exit(int code);
int   getenv_flag(const char *name);

#endif
