/* w32_reg.c — an in-memory registry, saved to <datadir>registry.dat.
 *
 * The game keeps its settings (display mode, D3D options, controls, ...)
 * under HKCU/HKLM\Software\SEGA\...; key paths are stored case-insensitively.
 */
#include "w32.h"

typedef struct RegVal { char key[200]; char name[64]; DWORD type; DWORD len; unsigned char *data; } RegVal;
#define MAXV 512
#define MAXK 64
static RegVal s_vals[MAXV];
static int s_nvals;
static char s_keys[MAXK][200];   /* open keys: handle = index + 0x1000 */
static int s_loaded;

static void reg_path(char *out, const char *root, const char *sub)
{
    int i;
    host_snprintf(out, 200, "%s%s%s", root, (sub && *sub) ? "\\" : "", sub ? sub : "");
    for (i = 0; out[i]; i++) if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
}

static const char *root_name(HKEY k)
{
    recomp_uptr v = (recomp_uptr)k;
    if (v == 0x80000000u) return "hkcr";
    if (v == 0x80000001u) return "hkcu";
    if (v == 0x80000002u) return "hklm";
    if (v == 0x80000003u) return "hku";
    if (v >= 0x1000 && v < 0x1000 + MAXK && s_keys[v - 0x1000][0]) return s_keys[v - 0x1000];
    return NULL;
}

static void reg_save(void)
{
    char p[512];
    void *f;
    int i;
    host_snprintf(p, sizeof p, "%sregistry.dat", rt_data_dir());
    f = host_fopen(p, "wb");
    if (!f) return;
    host_fwrite(f, "DREG", 4);
    host_fwrite(f, &s_nvals, 4);
    for (i = 0; i < s_nvals; i++) {
        host_fwrite(f, s_vals[i].key, 200);
        host_fwrite(f, s_vals[i].name, 64);
        host_fwrite(f, &s_vals[i].type, 4);
        host_fwrite(f, &s_vals[i].len, 4);
        host_fwrite(f, s_vals[i].data, s_vals[i].len);
    }
    host_fclose(f);
}

static void reg_load(void)
{
    char p[512], magic[4];
    void *f;
    int i, n = 0;
    if (s_loaded) return;
    s_loaded = 1;
    host_snprintf(p, sizeof p, "%sregistry.dat", rt_data_dir());
    f = host_fopen(p, "rb");
    if (!f) return;
    if (host_fread(f, magic, 4) == 4 && host_memcmp(magic, "DREG", 4) == 0 && host_fread(f, &n, 4) == 4) {
        for (i = 0; i < n && i < MAXV; i++) {
            RegVal *v = &s_vals[i];
            host_fread(f, v->key, 200);
            host_fread(f, v->name, 64);
            host_fread(f, &v->type, 4);
            host_fread(f, &v->len, 4);
            v->data = (unsigned char *)host_malloc(v->len + 1);
            host_fread(f, v->data, v->len);
        }
        s_nvals = i;
    }
    host_fclose(f);
}

static LSTATUS open_key(HKEY k, LPCSTR sub, PHKEY out)
{
    const char *root;
    char path[200];
    int i;
    reg_load();
    root = root_name(k);
    if (!root) return 6;   /* ERROR_INVALID_HANDLE */
    reg_path(path, root, sub);
    for (i = 0; i < MAXK; i++) {
        if (!s_keys[i][0]) {
            host_snprintf(s_keys[i], 200, "%s", path);
            *out = (HKEY)(recomp_uptr)(0x1000 + i);
            return 0;
        }
    }
    return 14;
}

LSTATUS w32_RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    (void)opt; (void)sam;
    return open_key(k, sub, out);
}

LSTATUS w32_RegCreateKeyExA(HKEY k, LPCSTR sub, DWORD r, LPSTR cls, DWORD opt, REGSAM sam,
                            LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    (void)r; (void)cls; (void)opt; (void)sam; (void)sa;
    if (disp) *disp = 2;   /* REG_OPENED_EXISTING_KEY */
    return open_key(k, sub, out);
}

LSTATUS w32_RegCreateKeyA(HKEY k, LPCSTR sub, PHKEY out) { return open_key(k, sub, out); }

LSTATUS w32_RegCloseKey(HKEY k)
{
    recomp_uptr v = (recomp_uptr)k;
    if (v >= 0x1000 && v < 0x1000 + MAXK) s_keys[v - 0x1000][0] = 0;
    return 0;
}

static RegVal *find_val(const char *key, const char *name)
{
    int i;
    for (i = 0; i < s_nvals; i++)
        if (host_strcmp(s_vals[i].key, key) == 0 && host_stricmp(s_vals[i].name, name ? name : "") == 0)
            return &s_vals[i];
    return NULL;
}

LSTATUS w32_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD len)
{
    const char *key = root_name(k);
    RegVal *v;
    (void)res;
    if (!key) return 6;
    v = find_val(key, name);
    if (!v) return 2;      /* ERROR_FILE_NOT_FOUND: the game uses its defaults */
    if (type) *type = v->type;
    if (data) {
        if (!len || *len < v->len) { if (len) *len = v->len; return 234; }
        memcpy(data, v->data, v->len);
    }
    if (len) *len = v->len;
    return 0;
}

LSTATUS w32_RegSetValueExA(HKEY k, LPCSTR name, DWORD r, DWORD type, BYTE *data, DWORD len)
{
    const char *key = root_name(k);
    RegVal *v;
    (void)r;
    if (!key) return 6;
    v = find_val(key, name);
    if (!v) {
        if (s_nvals >= MAXV) return 14;
        v = &s_vals[s_nvals++];
        host_snprintf(v->key, sizeof v->key, "%s", key);
        host_snprintf(v->name, sizeof v->name, "%s", name ? name : "");
        v->data = NULL;
    }
    host_free(v->data);
    v->data = (unsigned char *)host_malloc(len + 1);
    memcpy(v->data, data, len);
    v->type = type;
    v->len = len;
    reg_save();
    return 0;
}
