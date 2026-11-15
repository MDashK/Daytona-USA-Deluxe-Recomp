/* res.c — Win32 resources (string tables, ...) from the exe image and Resource.dll. */
#include "w32.h"

static unsigned char *s_resdll;     /* Resource.dll mapped by sections */
static unsigned int s_resdll_size;

static uint rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint)p[3] << 24); }
static uint rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static void load_resdll(void)
{
    char path[512];
    void *f;
    unsigned char *file;
    int len;
    uint pe, nsec, optsz, sec, i, size;
    if (s_resdll) return;
    host_snprintf(path, sizeof path, "%sResource.dll", rt_data_dir());
    plat_fix_case(path);
    f = host_fopen(path, "rb");
    if (!f) { rt_log("Resource.dll not found in %s\n", rt_data_dir()); return; }
    host_fseek(f, 0, 2);
    len = host_ftell(f);
    host_fseek(f, 0, 0);
    file = (unsigned char *)host_malloc(len);
    host_fread(f, file, len);
    host_fclose(f);
    pe = rd32(file + 0x3c);
    nsec = rd16(file + pe + 6);
    optsz = rd16(file + pe + 20);
    size = rd32(file + pe + 24 + 56);
    s_resdll = (unsigned char *)host_calloc(1, size);
    s_resdll_size = size;
    memcpy(s_resdll, file, rd32(file + pe + 24 + 60));
    sec = pe + 24 + optsz;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = file + sec + i * 40;
        uint va = rd32(s + 12), rsize = rd32(s + 16), rptr = rd32(s + 20), vsize = rd32(s + 8);
        uint n = rsize < vsize ? rsize : vsize;
        if (va + n <= size && rptr + n <= (uint)len) memcpy(s_resdll + va, file + rptr, n);
    }
    host_free(file);
}

HINSTANCE res_dll_handle(void) { load_resdll(); return (HINSTANCE)s_resdll; }
const unsigned char *res_dll_image(void) { load_resdll(); return s_resdll; }

static const unsigned char *image_of(HINSTANCE inst)
{
    if ((const unsigned char *)inst == s_resdll && s_resdll) return s_resdll;
    if (inst == NULL || (const unsigned char *)inst == g_image) return g_image;
    return g_image;
}

/* find entry `id` (integer id or name string) in a resource directory */
static const unsigned char *dir_find(const unsigned char *base, const unsigned char *dir, uint id, const char *name)
{
    uint nnamed = rd16(dir + 12), nid = rd16(dir + 14), i;
    const unsigned char *e = dir + 16;
    for (i = 0; i < nnamed + nid; i++, e += 8) {
        uint nm = rd32(e), off = rd32(e + 4);
        int match = 0;
        if (nm & 0x80000000u) {
            if (name) {
                const unsigned char *s = base + (nm & 0x7fffffff);
                uint len = rd16(s), k;
                match = 1;
                for (k = 0; k < len; k++) {
                    int a = rd16(s + 2 + k * 2), b = (unsigned char)name[k];
                    if (!b || ((a | 32) != (b | 32))) { match = 0; break; }
                }
                if (match && name[len]) match = 0;
            }
        } else if (!name) {
            match = (id == nm || id == 0xffffffffu);
        }
        if (match) return e;
    }
    return NULL;
}

const unsigned char *res_find(HINSTANCE inst, int type, const char *name_or_id, unsigned int *size)
{
    const unsigned char *img = image_of(inst);
    uint pe, rva, i;
    const unsigned char *base, *e;
    const char *nm = ((recomp_uptr)name_or_id > 0xffff) ? name_or_id : NULL;
    uint id = nm ? 0 : (uint)(recomp_uptr)name_or_id;
    if (!img) return NULL;
    pe = rd32(img + 0x3c);
    rva = rd32(img + pe + 24 + 96 + 2 * 8);
    if (!rva) return NULL;
    base = img + rva;
    e = dir_find(base, base, (uint)type, NULL);
    if (!e) return NULL;
    e = dir_find(base, base + (rd32(e + 4) & 0x7fffffff), id, nm);
    if (!e) return NULL;
    /* language: take the first */
    {
        const unsigned char *d = base + (rd32(e + 4) & 0x7fffffff);
        uint off;
        if (rd16(d + 12) + rd16(d + 14) == 0) return NULL;
        off = rd32(d + 16 + 4);
        for (i = 0; (off & 0x80000000u) && i < 4; i++) {
            d = base + (off & 0x7fffffff);
            off = rd32(d + 16 + 4);
        }
        d = base + off;   /* IMAGE_RESOURCE_DATA_ENTRY */
        if (size) *size = rd32(d + 4);
        return img + rd32(d);
    }
}

int w32_LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int max)
{
    unsigned int sz;
    const unsigned char *p = res_find(inst, 6, (const char *)(recomp_uptr)((id >> 4) + 1), &sz);
    uint k, len, j;
    if (!p && inst != GAME_HINSTANCE) p = res_find(GAME_HINSTANCE, 6, (const char *)(recomp_uptr)((id >> 4) + 1), &sz);
    if (!p && s_resdll) p = res_find((HINSTANCE)s_resdll, 6, (const char *)(recomp_uptr)((id >> 4) + 1), &sz);
    if (!p || max <= 0) { if (max > 0) buf[0] = 0; return 0; }
    for (k = 0; k < (id & 15); k++) p += 2 + rd16(p) * 2;
    len = rd16(p);
    for (j = 0; j < len && (int)j < max - 1; j++) {
        uint c = rd16(p + 2 + j * 2);
        buf[j] = (char)(c < 256 ? c : '?');
    }
    buf[j] = 0;
    return (int)j;
}
