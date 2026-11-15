/* crt.h — the statically linked MSVC 4.2 C runtime functions the game calls.
 *
 * The original CRT code (0x4a7e30+) is not recompiled; these are implemented
 * in runtime/crt.c on top of the host libc.  Names keep Ghidra's (FID) name
 * with a crt_ prefix so they never clash with the host libc.
 */
#ifndef RECOMP_CRT_H
#define RECOMP_CRT_H
#include <stdarg.h>
#include "recomp.h"

void *crt__memset(void *dst, int val, size_t n);
void *crt__memcpy(void *dst, void *src, size_t n);
int   crt__memcmp(void *a, void *b, size_t n);
size_t crt__strlen(char *s);
int   crt__strcmp(char *a, char *b);
char *crt__strncpy(char *dst, char *src, size_t n);
char *crt___mbscpy(char *dst, char *src);
int   crt__tolower(int c);
int   crt___isctype(int c, int type);
int   crt__sprintf(char *dst, char *fmt, ...);
int   crt__vsprintf(char *dst, char *fmt, va_list ap);
void *crt__malloc(size_t n);
void *crt__calloc(size_t n, size_t sz);
void  crt__free(void *p);
void *crt_operator_new(uint n);
void  crt__exit(int code);
void  crt___exit(uint code);
void  crt___amsg_exit(int code);
void  crt___fpmath(int x);
ulonglong crt___aulldiv(uint al, uint ah, uint bl, uint bh);
ulonglong crt___aullrem(uint al, uint ah, uint bl, uint bh);
uint  crt___control87(uint nv, uint mask);
uint  crt___controlfp(uint nv, uint mask);
int   crt___fcloseall(void);
void  crt_entry(void);
void  crt_FUN_004a8530(void *p);      /* operator delete */
void  crt_FUN_004a86e0();             /* __chkstk / _alloca_probe */

#endif
