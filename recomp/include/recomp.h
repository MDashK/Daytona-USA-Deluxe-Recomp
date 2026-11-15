/* recomp.h — common header for the generated (Ghidra-decompiled) game code.
 *
 * Every generated translation unit includes this.  It provides:
 *   - Ghidra's base types (undefined4, uint, float10, code, ...)
 *   - Ghidra's helper operators (CONCATxy, SUBxy, CARRYn, SBORROWn, ROUND...)
 *   - the image: the game's .data/.rdata/.bss live in g_image, laid out
 *     exactly as in the original 32-bit address space, so every DAT_xxxxxxxx
 *     is a fixed offset into it (see runtime/image.c).
 *
 * Requirements on the target: 32-bit pointers, little endian, signed char.
 * (x86-32 Windows host build and the PS Vita both qualify.)
 */
#ifndef RECOMP_H
#define RECOMP_H

/* No system headers here: Ghidra's types (types.h) reuse many CRT names
 * (_locale_t, FILE, ...).  Only the few libc functions needed are declared. */
typedef unsigned int size_t;
typedef unsigned int recomp_uptr;
typedef char recomp_assert_32bit[(sizeof(void *) == 4) ? 1 : -1];
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
double rint(double);
double floor(double);
double ceil(double);
double sqrt(double);
double fabs(double);
#define RECOMP_ALIGN8_ALIGNED
#if defined(_MSC_VER)
#define RECOMP_ALIGN8
#else
#define RECOMP_ALIGN8 __attribute__((aligned(8)))
#endif

/* ---- Ghidra base types ---------------------------------------------------- */
typedef unsigned char      undefined;
typedef unsigned char      undefined1;
typedef unsigned short     undefined2;
typedef unsigned int       undefined3;
typedef unsigned int       undefined4;
typedef unsigned long long undefined5;
typedef unsigned long long undefined6;
typedef unsigned long long undefined7;
typedef unsigned long long undefined8;
typedef unsigned char      byte;
typedef unsigned short     word;
typedef unsigned int       dword;
typedef unsigned long long qword;
typedef unsigned int       uint;
typedef unsigned int       uint3;
typedef unsigned short     ushort;
typedef unsigned char      uchar;
typedef unsigned long      ulong;
typedef long long          longlong;
typedef unsigned long long ulonglong;
typedef signed char        sbyte;
typedef unsigned short     wchar16;
#include "recomp_unaligned.h"
typedef double             float10;     /* x87 80-bit: double precision is close enough */
typedef unsigned int       ImageBaseOffset32;
typedef void              *pointer;
typedef void              *pointer32;
typedef void              *addr;
typedef char               string;
typedef char               TerminatedCString;
typedef unsigned short     unicode;
#ifndef __cplusplus
typedef unsigned char      bool;
#define true  1
#define false 0
#endif
/* `code` is the type of an unknown function: unprototyped so any call compiles. */
typedef int code();

/* ---- the image ------------------------------------------------------------ */
#define IMAGE_BASE   0x00400000u
#define IMAGE_SIZE   0x004c1000u
extern unsigned char g_image[];          /* DAYTONA USA Deluxe.exe */
extern unsigned char g_image_ddse[];     /* DDSE.dll (2D sprite engine) */
#define DDSE_BASE    0x10000000u
#define DDSE_SIZE    0x00168000u
/* a generated module may define RECOMP_IMG/RECOMP_BASE for its own image */
#ifndef RECOMP_IMG
#define RECOMP_IMG   g_image
#define RECOMP_BASE  IMAGE_BASE
#endif
/* host pointer for an original virtual address */
#define VA(a)        ((void *)(RECOMP_IMG + ((uint)(a) - RECOMP_BASE)))
/* the same, as a 32-bit integer (for address arithmetic Ghidra left numeric) */
#define VAI(a)       ((uint)(recomp_uptr)VA(a))
/* a global of type T at original address a */
#define G(T, a)      (*(T *)VA(a))

/* ---- Ghidra helper operators --------------------------------------------- */
#define CONCAT11(a,b) ((ushort)(((uint)(uchar)(a) << 8) | (uchar)(b)))
#define CONCAT12(a,b) ((uint)(((uint)(uchar)(a) << 16) | (ushort)(b)))
#define CONCAT13(a,b) ((uint)(((uint)(uchar)(a) << 24) | ((uint)(b) & 0xffffffu)))
#define CONCAT21(a,b) ((uint)(((uint)(ushort)(a) << 8) | (uchar)(b)))
#define CONCAT22(a,b) ((uint)(((uint)(ushort)(a) << 16) | (ushort)(b)))
#define CONCAT31(a,b) ((uint)(((uint)(a) << 8) | (uchar)(b)))
#define CONCAT14(a,b) ((ulonglong)(((ulonglong)(uchar)(a) << 32) | (uint)(b)))
#define CONCAT24(a,b) ((ulonglong)(((ulonglong)(ushort)(a) << 32) | (uint)(b)))
#define CONCAT34(a,b) ((ulonglong)(((ulonglong)((uint)(a) & 0xffffffu) << 32) | (uint)(b)))
#define CONCAT44(a,b) ((ulonglong)(((ulonglong)(uint)(a) << 32) | (uint)(b)))
#define CONCAT15(a,b) ((ulonglong)(((ulonglong)(uchar)(a) << 40) | ((ulonglong)(b) & 0xffffffffffull)))
#define CONCAT16(a,b) ((ulonglong)(((ulonglong)(uchar)(a) << 48) | ((ulonglong)(b) & 0xffffffffffffull)))
#define CONCAT17(a,b) ((ulonglong)(((ulonglong)(uchar)(a) << 56) | ((ulonglong)(b) & 0xffffffffffffffull)))
#define CONCAT26(a,b) ((ulonglong)(((ulonglong)(ushort)(a) << 48) | ((ulonglong)(b) & 0xffffffffffffull)))
#define CONCAT42(a,b) ((ulonglong)(((ulonglong)(uint)(a) << 16) | (ushort)(b)))
#define CONCAT41(a,b) ((ulonglong)(((ulonglong)(uint)(a) << 8) | (uchar)(b)))
#define CONCAT32(a,b) ((ulonglong)(((ulonglong)((uint)(a) & 0xffffffu) << 16) | (ushort)(b)))

/* SUBxy(v, n): y bytes of the x-byte value v, starting at byte n */
#define SUB_(v, n, T)  ((T)((ulonglong)(v) >> ((n) * 8)))
#define SUB21(v,n) SUB_(v,n,uchar)
#define SUB31(v,n) SUB_(v,n,uchar)
#define SUB41(v,n) SUB_(v,n,uchar)
#define SUB42(v,n) SUB_(v,n,ushort)
#define SUB43(v,n) (SUB_(v,n,uint) & 0xffffffu)
#define SUB81(v,n) SUB_(v,n,uchar)
#define SUB82(v,n) SUB_(v,n,ushort)
#define SUB84(v,n) SUB_(v,n,uint)
#define SUB161(v,n) SUB_(v,n,uchar)
/* float -> bits */
static inline uint recomp_f2u(float f) { uint u; memcpy(&u, &f, 4); return u; }

#define ZEXT14(v) ((uint)(uchar)(v))
#define ZEXT24(v) ((uint)(ushort)(v))
#define ZEXT48(v) ((ulonglong)(uint)(v))
#define ZEXT18(v) ((ulonglong)(uchar)(v))
#define ZEXT28(v) ((ulonglong)(ushort)(v))
#define ZEXT12(v) ((ushort)(uchar)(v))
#define SEXT14(v) ((int)(signed char)(v))
#define SEXT24(v) ((int)(short)(v))
#define SEXT48(v) ((longlong)(int)(v))
#define SEXT18(v) ((longlong)(signed char)(v))
#define SEXT28(v) ((longlong)(short)(v))
#define SEXT12(v) ((short)(signed char)(v))

#define CARRY1(a,b) ((uchar)((uchar)(a) + (uchar)(b)) < (uchar)(a))
#define CARRY2(a,b) ((ushort)((ushort)(a) + (ushort)(b)) < (ushort)(a))
#define CARRY4(a,b) ((uint)((uint)(a) + (uint)(b)) < (uint)(a))
#define SCARRY1(a,b) (((((signed char)(a) + (signed char)(b)) ^ (signed char)(a)) & (((signed char)(a) + (signed char)(b)) ^ (signed char)(b)) & 0x80) != 0)
#define SCARRY2(a,b) ((((short)((short)(a) + (short)(b)) ^ (short)(a)) & ((short)((short)(a) + (short)(b)) ^ (short)(b)) & 0x8000) != 0)
#define SCARRY4(a,b) ((((int)((uint)(a) + (uint)(b)) ^ (int)(a)) & ((int)((uint)(a) + (uint)(b)) ^ (int)(b))) < 0)
#define SBORROW1(a,b) ((((signed char)(a) ^ (signed char)(b)) & ((signed char)(a) ^ (signed char)((signed char)(a) - (signed char)(b))) & 0x80) != 0)
#define SBORROW2(a,b) ((((short)(a) ^ (short)(b)) & ((short)(a) ^ (short)((short)(a) - (short)(b))) & 0x8000) != 0)
#define SBORROW4(a,b) ((((int)(a) ^ (int)(b)) & ((int)(a) ^ (int)((uint)(a) - (uint)(b)))) < 0)

#define NAN_(x)  ((double)(x) != (double)(x))
#define NaN(x)   ((double)(x) != (double)(x))
#define ROUND(x) ((longlong)rint((double)(x)))     /* x87 default: round to nearest */
#define TRUNC(x) ((longlong)(double)(x))
#define FLOOR(x) floor((double)(x))
#define CEIL(x)  ceil((double)(x))
#define SQRT(x)  sqrt((double)(x))
#define ABS(x)   fabs((double)(x))

/* x87 float -> int conversion as MSVC's __ftol does it (truncate). */
static inline longlong crt___ftol(double x) { return (longlong)x; }

/* rotate */
#define ROL4(v,n) ((uint)(((uint)(v) << ((n) & 31)) | ((uint)(v) >> ((32 - ((n) & 31)) & 31))))
#define ROR4(v,n) ((uint)(((uint)(v) >> ((n) & 31)) | ((uint)(v) << ((32 - ((n) & 31)) & 31))))

/* x._N_M_ : M bytes at byte offset N of x (lvalue for M = 1, 2, 4) */
#define PIECE1(x,n) (*(uchar *)((uchar *)&(x) + (n)))
#define PIECE2(x,n) (*(ushort *)((uchar *)&(x) + (n)))
#define PIECE4(x,n) (*(uint *)((uchar *)&(x) + (n)))
#define PIECE3(x,n) (recomp_get3((uchar *)&(x) + (n)))
static inline uint recomp_get3(const uchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
/* the bits of a float (Ghidra's `(int)fVar` reinterpretation, see gen.py float_bits) */
uint recomp_host_to_va(const void *p);
static inline uint recomp_fbits(float f) { union { float f; uint u; } x; x.f = f; return x.u; }
static inline void recomp_set3(uchar *p, uint v) { p[0] = (uchar)v; p[1] = (uchar)(v >> 8); p[2] = (uchar)(v >> 16); }

typedef int  int3;
typedef long long int5;
typedef long long int6;
typedef long long int7;
typedef unsigned long long uint5;
typedef unsigned long long uint6;
typedef unsigned long long uint7;
typedef unsigned int  uint3_t;

extern uint recomp_fs_dummy[16];

static const uint recomp_nan_bits = 0x7fc00000u;
#ifndef NAN
#define NAN (*(const float *)&recomp_nan_bits)
#endif

/* x86 "int n" (only reached in mis-decoded data) */
code *swi(int n);
/* MMX helpers Ghidra emits as calls */
ulonglong pmaddwd(ulonglong a, ulonglong b);

/* call tracing (build with -DRECOMP_TRACE to log every function entered) */
#ifdef RECOMP_TRACE
void recomp_enter(uint va);
#define RECOMP_ENTER(a) recomp_enter(a)
#elif defined(RECOMP_LASTFN)
/* only remember the last function entered (shown by the Vita heartbeat) */
extern volatile uint g_recomp_last;
#define RECOMP_ENTER(a) (g_recomp_last = (a))
#else
#define RECOMP_ENTER(a) ((void)0)
#endif

/* runtime hooks */
void recomp_unimplemented(const char *what, uint va);
void recomp_trap_call(uint va);
void *recomp_code_ptr(uint va);       /* original code address -> host function */

#endif /* RECOMP_H */
