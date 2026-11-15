/* recomp_unaligned.h — types for memory accesses that may be unaligned.
 *
 * The x86 code reads floats, doubles and 64-bit values at any address
 * (the engine's guest stack and some data are only 2-byte aligned).  On ARM
 * (PS Vita) a plain `*(float *)p` becomes VLDR / LDRD, which fault when p is
 * not 4-byte aligned; through these aligned(1) types GCC loads via integer
 * registers instead (LDR tolerates unaligned addresses).  The generators
 * (recomp/gen.py, tools/lift_engine.py) emit them for every float / double /
 * 64-bit dereference. */
#ifndef RECOMP_UNALIGNED_H
#define RECOMP_UNALIGNED_H
#if defined(__GNUC__)
typedef float recomp_f32u __attribute__((aligned(1)));
typedef double recomp_f64u __attribute__((aligned(1)));
typedef unsigned long long recomp_u64u __attribute__((aligned(1)));
/* integers too: an int that is converted to float right away is otherwise
 * loaded straight into a VFP register (VLDR), which needs alignment */
typedef unsigned int recomp_u32u __attribute__((aligned(1)));
typedef int recomp_s32u __attribute__((aligned(1)));
typedef unsigned short recomp_u16u __attribute__((aligned(1)));
typedef short recomp_s16u __attribute__((aligned(1)));
#else
typedef float recomp_f32u;
typedef double recomp_f64u;
typedef unsigned long long recomp_u64u;
typedef unsigned int recomp_u32u;
typedef int recomp_s32u;
typedef unsigned short recomp_u16u;
typedef short recomp_s16u;
#endif
#endif
