/* image.c — load the original binaries' data into their images.
 *
 * The generated code addresses every global as an offset into an image that
 * mirrors the original 32-bit address space:
 *   g_image      "DAYTONA USA Deluxe.exe"  0x00400000 .. +0x4c1000
 *   g_image_ddse "DDSE.dll"                0x10000000 .. +0x168000
 * At startup the sections of the files (supplied by the user, never
 * redistributed) are copied in and the base relocations applied:
 *   - a pointer to data   -> rebased into the image
 *   - a pointer to code   -> the host address of the recompiled function
 * so pointer tables in .data (mode handlers, callbacks, string tables...)
 * work exactly as in the original.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"
#include "runtime.h"

#if defined(_MSC_VER)
__declspec(align(4096)) unsigned char g_image[IMAGE_SIZE];
__declspec(align(4096)) unsigned char g_image_ddse[DDSE_SIZE];
#else
unsigned char g_image[IMAGE_SIZE] __attribute__((aligned(4096)));
unsigned char g_image_ddse[DDSE_SIZE] __attribute__((aligned(4096)));
#endif

typedef struct { uint va; void *fn; } FuncEntry;
extern const FuncEntry recomp_functable[];
extern const int recomp_functable_count;
extern const FuncEntry recomp_functable_lifted[];
extern const int recomp_functable_lifted_count;
extern const FuncEntry recomp_functable_ddse[];
extern const int recomp_functable_ddse_count;

static uint rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint)p[3] << 24); }
static uint rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static void *lookup(const FuncEntry *t, int n, uint va)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint v = t[mid].va;
        if (v == va) return t[mid].fn;
        if (v < va) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void *recomp_code_ptr(uint va)
{
    if (va >= DDSE_BASE && va < DDSE_BASE + DDSE_SIZE)
        return lookup(recomp_functable_ddse, recomp_functable_ddse_count, va);
    {
        void *fn = lookup(recomp_functable, recomp_functable_count, va);
        /* engine code labels that are not functions in the decompiled C */
        if (!fn) fn = lookup(recomp_functable_lifted, recomp_functable_lifted_count, va);
        return fn;
    }
}

uint recomp_host_to_va(const void *p)
{
    uintptr_t u = (uintptr_t)p;
    if (u >= (uintptr_t)g_image && u < (uintptr_t)g_image + IMAGE_SIZE)
        return (uint)(u - (uintptr_t)g_image) + IMAGE_BASE;
    if (u >= (uintptr_t)g_image_ddse && u < (uintptr_t)g_image_ddse + DDSE_SIZE)
        return (uint)(u - (uintptr_t)g_image_ddse) + DDSE_BASE;
    return 0;
}

static int load_pe(const char *path, unsigned char *img, uint base, uint size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;
    uint pe, nsec, optsz, opt, sec, i, relrva, relsize, text_lo = 0, text_hi = 0;

    if (!f) { rt_log("image: cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(len);
    if (!buf || fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); return 0; }
    fclose(f);

    pe = rd32(buf + 0x3c);
    if (rd32(buf + pe) != 0x4550) { rt_log("image: %s is not a PE file\n", path); free(buf); return 0; }
    nsec = rd16(buf + pe + 6);
    optsz = rd16(buf + pe + 20);
    opt = pe + 24;
    if (rd32(buf + opt + 28) != base || rd32(buf + opt + 56) != size) {
        rt_log("image: %s: unexpected ImageBase/SizeOfImage (wrong version? need v1.20E)\n", path);
        free(buf);
        return 0;
    }
    memset(img, 0, size);
    memcpy(img, buf, rd32(buf + opt + 60)); /* headers */
    sec = opt + optsz;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = buf + sec + i * 40;
        uint vsize = rd32(s + 8), va = rd32(s + 12), rsize = rd32(s + 16), rptr = rd32(s + 20);
        uint n = rsize < vsize ? rsize : vsize;
        if (va + n > size) n = size - va;
        memcpy(img + va, buf + rptr, n);
        if (memcmp(s, ".text", 5) == 0) { text_lo = base + va; text_hi = base + va + vsize; }
    }
    free(buf);

    /* base relocations; relocations inside .text are applied too: besides
     * code immediates (never executed here) .text holds data tables */
    relrva = rd32(img + opt + 96 + 5 * 8);
    relsize = rd32(img + opt + 96 + 5 * 8 + 4);
    {
        uint off = 0, nreloc = 0, ncode = 0;
        uintptr_t delta = (uintptr_t)img - base;
        while (off + 8 <= relsize) {
            uint page = rd32(img + relrva + off);
            uint bsize = rd32(img + relrva + off + 4);
            uint k;
            if (bsize < 8) break;
            for (k = 8; k + 2 <= bsize; k += 2) {
                uint e = rd16(img + relrva + off + k);
                uint loc = page + (e & 0xfff);
                uint *slot, v;
                void *fn;
                if ((e >> 12) != 3) continue;           /* IMAGE_REL_BASED_HIGHLOW only */
                slot = (uint *)(img + loc);
                v = *slot;
                if (v >= text_lo && v < text_hi && (fn = recomp_code_ptr(v)) != NULL) {
                    *slot = (uint)(uintptr_t)fn;
                    ncode++;
                } else if (v >= base && v < base + size) {
                    *slot = (uint)(v + delta);
                }
                nreloc++;
            }
            off += bsize;
        }
        rt_log("image: %s: %u relocations, %u to recompiled code\n", path, nreloc, ncode);
    }
    return 1;
}

int image_load(const char *path)
{
    return load_pe(path, g_image, IMAGE_BASE, IMAGE_SIZE);
}

int image_load_ddse(const char *path)
{
    return load_pe(path, g_image_ddse, DDSE_BASE, DDSE_SIZE);
}
