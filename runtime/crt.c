/* crt.c — MSVC 4.2 C runtime functions called by the game, on the host libc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "recomp.h"
#include "crt.h"
#include "runtime.h"

void *crt__memset(void *dst, int val, size_t n) { return memset(dst, val, n); }
void *crt__memcpy(void *dst, void *src, size_t n) { return memmove(dst, src, n); }
int   crt__memcmp(void *a, void *b, size_t n) { return memcmp(a, b, n); }
size_t crt__strlen(char *s) { return strlen(s); }
int   crt__strcmp(char *a, char *b) { return strcmp(a, b); }
char *crt__strncpy(char *dst, char *src, size_t n) { return strncpy(dst, src, n); }
char *crt___mbscpy(char *dst, char *src) { return strcpy(dst, src); }
int   crt__tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* MSVC ctype masks: _UPPER 1 _LOWER 2 _DIGIT 4 _SPACE 8 _PUNCT 16 _CONTROL 32 _BLANK 64 _HEX 128 */
int crt___isctype(int c, int type)
{
    int r = 0;
    if (c < -1 || c > 255) return 0;
    if (c == -1) return 0;
    if (isupper(c)) r |= 1;
    if (islower(c)) r |= 2;
    if (isdigit(c)) r |= 4;
    if (isspace(c)) r |= 8;
    if (ispunct(c)) r |= 16;
    if (iscntrl(c)) r |= 32;
    if (c == ' ') r |= 64;
    if (isxdigit(c)) r |= 128;
    if (type & 0x100) { if (isalpha(c)) r |= 0x100; }
    return r & type;
}

int crt__vsprintf(char *dst, char *fmt, va_list ap)
{
    return vsprintf(dst, fmt, ap);
}

int crt__sprintf(char *dst, char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsprintf(dst, fmt, ap);
    va_end(ap);
    return r;
}

void *crt__malloc(size_t n) { return calloc(1, n ? n : 1); }
void *crt__calloc(size_t n, size_t sz) { return calloc(n ? n : 1, sz ? sz : 1); }
void  crt__free(void *p) { free(p); }
void *crt_operator_new(uint n) { return calloc(1, n ? n : 1); }

void crt__exit(int code) { rt_log("game called exit(%d)\n", code); exit(code); }
void crt___exit(uint code) { crt__exit((int)code); }
void crt___amsg_exit(int code) { rt_fatal("CRT runtime error R60%02d\n", code); }
void crt___fpmath(int x) { (void)x; }

ulonglong crt___aulldiv(uint al, uint ah, uint bl, uint bh)
{
    ulonglong a = ((ulonglong)ah << 32) | al, b = ((ulonglong)bh << 32) | bl;
    return b ? a / b : 0;
}

ulonglong crt___aullrem(uint al, uint ah, uint bl, uint bh)
{
    ulonglong a = ((ulonglong)ah << 32) | al, b = ((ulonglong)bh << 32) | bl;
    return b ? a % b : 0;
}

uint crt___control87(uint nv, uint mask) { (void)nv; (void)mask; return 0x9001f; }
uint crt___controlfp(uint nv, uint mask) { (void)nv; (void)mask; return 0x9001f; }
int  crt___fcloseall(void) { return 0; }
void crt_entry(void) { }
void crt_FUN_004a8530(void *p) { free(p); }
void crt_FUN_004a86e0() { }
