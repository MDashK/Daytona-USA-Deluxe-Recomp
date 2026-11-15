/* w32_mm.c — LZ32 (SZDD files), the game's _INMM.dll (winmm wrapper):
 * timer, MCI CD audio (played from <datadir>BGM/NN.mp3), mmio (RIFF/WAVE
 * reading) and the joystick API (fed from the platform pad). */
#include "w32.h"
#include "audio.h"

void *w32_file_fp(HANDLE h);

/* ---- LZ32 ----------------------------------------------------------------
 * LZInit on a file handle reads the whole file; SZDD files ("SZDD\x88\xf0\x27\x33")
 * are expanded (LZSS, 4 KB window filled with spaces, flag bit set = literal). */
typedef struct LzFile { unsigned char *data; unsigned int len, pos; } LzFile;
#define MAXLZ 16
static LzFile s_lz[MAXLZ];

static unsigned char *szdd_expand(const unsigned char *src, unsigned int srclen, unsigned int *outlen)
{
    unsigned int ulen = src[10] | (src[11] << 8) | (src[12] << 16) | ((unsigned int)src[13] << 24);
    unsigned char *out = (unsigned char *)host_malloc(ulen + 1);
    unsigned char win[4096];
    unsigned int ip = 14, op = 0, wp = 4096 - 16;
    memset(win, ' ', sizeof win);
    while (ip < srclen && op < ulen) {
        unsigned int flags = src[ip++], bit;
        for (bit = 0; bit < 8 && ip < srclen && op < ulen; bit++) {
            if (flags & (1u << bit)) {
                unsigned char c = src[ip++];
                out[op++] = c;
                win[wp] = c;
                wp = (wp + 1) & 4095;
            } else {
                unsigned int b1, b2, mpos, mlen, k;
                if (ip + 1 >= srclen) { ip = srclen; break; }
                b1 = src[ip++];
                b2 = src[ip++];
                mpos = b1 | ((b2 & 0xf0) << 4);
                mlen = (b2 & 0x0f) + 3;
                for (k = 0; k < mlen && op < ulen; k++) {
                    unsigned char c = win[(mpos + k) & 4095];
                    out[op++] = c;
                    win[wp] = c;
                    wp = (wp + 1) & 4095;
                }
            }
        }
    }
    *outlen = op;
    return out;
}

INT w32_LZInit(INT hf)
{
    void *fp = w32_file_fp((HANDLE)(recomp_uptr)hf);
    int i, start, end;
    unsigned char *raw;
    unsigned int rawlen;
    if (!fp) return -1;
    for (i = 0; i < MAXLZ && s_lz[i].data; i++) ;
    if (i == MAXLZ) return -1;
    start = host_ftell(fp);
    host_fseek(fp, 0, 2);
    end = host_ftell(fp);
    host_fseek(fp, start, 0);
    rawlen = (unsigned int)(end - start);
    raw = (unsigned char *)host_malloc(rawlen + 1);
    host_fread(fp, raw, rawlen);
    if (rawlen >= 14 && host_memcmp(raw, "SZDD\x88\xf0\x27\x33", 8) == 0) {
        s_lz[i].data = szdd_expand(raw, rawlen, &s_lz[i].len);
        host_free(raw);
    } else {
        s_lz[i].data = raw;
        s_lz[i].len = rawlen;
    }
    s_lz[i].pos = 0;
    return 0x400 + i;
}

/* a game data file, expanded when it is SZDD (runtime/menu.c reads the font) */
unsigned char *rt_load_data_file(const char *rel, unsigned int *len)
{
    char path[512];
    void *f;
    unsigned char *buf, tmp[4096];
    unsigned int n = 0, cap = 0, got;
    host_snprintf(path, sizeof path, "%s%s", rt_data_dir(), rel);
    plat_fix_case(path);
    if (!(f = host_fopen(path, "rb"))) return NULL;
    buf = NULL;
    while ((got = (unsigned int)host_fread(f, tmp, sizeof tmp)) > 0) {
        if (n + got > cap) {
            unsigned char *nb = (unsigned char *)host_malloc(cap = (n + got) * 2);
            if (buf) { memcpy(nb, buf, n); host_free(buf); }
            buf = nb;
        }
        memcpy(buf + n, tmp, got);
        n += got;
    }
    host_fclose(f);
    if (buf && n > 14 && buf[0] == 'S' && buf[1] == 'Z' && buf[2] == 'D' && buf[3] == 'D') {
        unsigned char *x = szdd_expand(buf, n, &n);
        host_free(buf);
        buf = x;
    }
    *len = n;
    return buf;
}

static LzFile *lz_get(INT h)
{
    int i = h - 0x400;
    if (i < 0 || i >= MAXLZ || !s_lz[i].data) return NULL;
    return &s_lz[i];
}

INT w32_LZRead(INT h, CHAR *buf, INT n)
{
    LzFile *f = lz_get(h);
    unsigned int k;
    if (!f || n < 0) return -1;
    k = f->len - f->pos;
    if ((unsigned int)n < k) k = (unsigned int)n;
    memcpy(buf, f->data + f->pos, k);
    f->pos += k;
    return (INT)k;
}

LONG w32_LZSeek(INT h, LONG off, INT whence)
{
    LzFile *f = lz_get(h);
    long p;
    if (!f) return -1;
    p = whence == 0 ? off : whence == 1 ? (long)f->pos + off : (long)f->len + off;
    if (p < 0 || (unsigned long)p > f->len) return -1;
    f->pos = (unsigned int)p;
    return p;
}

void w32_LZClose(INT h)
{
    LzFile *f = lz_get(h);
    if (!f) return;
    host_free(f->data);
    f->data = NULL;
}

/* ---- timer ------------------------------------------------------------- */
uint w32_imeGetTime(void) { return plat_time_ms(); }

/* ---- MCI CD audio --------------------------------------------------------
 * cdaudio device, TMSF time format; tracks 2.. are BGM/NN.mp3. */
#define MCI_OPEN 0x803
#define MCI_CLOSE 0x804
#define MCI_PLAY 0x806
#define MCI_SEEK 0x807
#define MCI_STOP 0x808
#define MCI_PAUSE 0x809
#define MCI_SET 0x80d
#define MCI_STATUS 0x814
#define MCI_RESUME 0x855
#define MCI_NOTIFY 1
#define MCI_FROM 4
#define MCI_TO 8
#define CD_DEVICE_ID 1

static HWND s_cd_notify;
static int s_cd_mode = 525;   /* MCI_MODE_STOP 525, PLAY 526, PAUSE 529 */
static int s_cd_track;
static int s_cd_stopped;      /* stopped mid-track: the track stays loaded */

/* A play with MCI_NOTIFY that is stopped or replaced gets its notification
 * (ABORTED 4 / SUPERSEDED 2) like on Windows.  The game counts the stops it
 * makes and swallows that many notifications; without them it also swallowed
 * the end of the next track and the menu music never repeated. */
static void cd_notify_cancel(uint code)
{
    if (s_cd_notify) {
        w32_post(s_cd_notify, 0x3b9 /* MM_MCINOTIFY */, code, CD_DEVICE_ID);
        s_cd_notify = NULL;
    }
}

static void cd_finished(void)
{
    rt_log("CD: track %d ended at %u ms\n", s_cd_track, plat_time_ms());
    s_cd_mode = 525;
    if (s_cd_notify) {
        w32_post(s_cd_notify, 0x3b9 /* MM_MCINOTIFY */, 1 /* MCI_NOTIFY_SUCCESSFUL */, CD_DEVICE_ID);
        s_cd_notify = NULL;
    }
}

uint w32_ciSendCommandA(uint dev, uint msg, uint flags, uint param)
{
    uint *p = (uint *)(recomp_uptr)param;
    (void)dev;
    switch (msg) {
    case MCI_OPEN:
        if (p) p[1] = CD_DEVICE_ID;
        return 0;
    case MCI_CLOSE:
        cd_notify_cancel(4);
        audio_music_stop();
        s_cd_mode = 525;
        s_cd_stopped = 0;
        return 0;
    case MCI_SET:
        return 0;
    case MCI_STATUS:
        if (!p) return 0;
        switch (p[2]) {
        /* A virtual disc where track N starts at N:00:00 (MSF) / N:00:00:00
         * (TMSF): either way the low byte of a position is the track, which
         * is all MCI_PLAY needs to pick NN.mp3.  The game builds its track
         * table from these (status POSITION with MCI_TRACK). */
        case 1: p[1] = (flags & 0x10) ? 1u : 25u; break;                     /* length */
        /* stopped: no current track (the main menu restarts its music only
         * when track 23 is not the current one) */
        case 2: p[1] = (flags & 0x10) ? p[3] : (s_cd_mode == 525 ? 0u : (uint)s_cd_track); break;      /* position */
        case 3: p[1] = 24; break;                             /* number of tracks */
        case 4: p[1] = (uint)s_cd_mode; break;                /* mode */
        case 5: p[1] = 1; break;                              /* media present */
        case 6: p[1] = 10; break;                             /* time format: TMSF */
        case 7: p[1] = 1; break;                              /* ready */
        case 8: p[1] = s_cd_mode == 525 ? 0u : (uint)s_cd_track; break;   /* current track */
        case 0x80: p[1] = p[3] >= 2 ? 0 : 4; break;           /* MCI_CDA_STATUS_TYPE_TRACK: audio(0)/data */
        default: p[1] = 0; break;
        }
        return 0;
    case MCI_PLAY: {
        int track = (flags & MCI_FROM) && p ? (int)(p[1] & 0xff) : s_cd_track;
        char path[512];
        if (track < 2) track = 2;
        cd_notify_cancel(2);
        /* no start position after a stop: carry on from where it stopped (the
         * game "pauses" the music with stop + play; a CD keeps its position) */
        if (!(flags & MCI_FROM) && s_cd_stopped && track == s_cd_track && audio_music_is_open()) {
            s_cd_notify = (flags & MCI_NOTIFY) && p ? (HWND)(recomp_uptr)p[0] : NULL;
            s_cd_stopped = 0;
            audio_music_pause(0);
            s_cd_mode = 526;
            return 0;
        }
        s_cd_stopped = 0;
        s_cd_track = track;
        /* the game folder's CDDA_BGM/NN.mp3 (or BGM/NN.mp3) */
        host_snprintf(path, sizeof path, "%sCDDA_BGM/%02d.mp3", rt_data_dir(), track);
        plat_fix_case(path);
        if (!host_fexists(path)) {
            host_snprintf(path, sizeof path, "%sBGM/%02d.mp3", rt_data_dir(), track);
            plat_fix_case(path);
        }
        s_cd_notify = (flags & MCI_NOTIFY) && p ? (HWND)(recomp_uptr)p[0] : NULL;
        rt_log("CD: play track %d%s at %u ms\n", track, s_cd_notify ? " (notify)" : "", plat_time_ms());
        if (audio_music_play(path, 0, cd_finished)) s_cd_mode = 526;
        else { rt_log("CD: no music file %s\n", path); cd_finished(); }
        return 0;
    }
    case MCI_STOP:
        cd_notify_cancel(4);
        if (s_cd_mode == 526 && audio_music_is_open()) {
            audio_music_pause(1);       /* keep the position for a resume */
            s_cd_stopped = 1;
        } else {
            audio_music_stop();
            s_cd_stopped = 0;
        }
        s_cd_mode = 525;
        return 0;
    case MCI_PAUSE:
        audio_music_pause(1);
        s_cd_mode = 529;
        return 0;
    case MCI_RESUME:
        audio_music_pause(0);
        s_cd_mode = 526;
        return 0;
    case MCI_SEEK:
        return 0;
    }
    rt_log("MCI: unhandled command 0x%x\n", msg);
    return 0;
}

int w32_ciGetErrorStringA(uint err, char *buf, uint n)
{
    host_snprintf(buf, n, "MCI error %u", err);
    return 1;
}

/* ---- mmio (RIFF reading) ------------------------------------------------- */
typedef struct MmFile { unsigned char *data; unsigned int len, pos; } MmFile;
typedef struct MMCK { uint ckid, cksize, fccType, dwDataOffset, dwFlags; } MMCK;

/* The sound banks (Resource/*.bin, 0.3-2.5 MB) are opened once per sound
 * loaded: read each file once and keep it (about 10 MB for all of them);
 * re-reading the whole bank ~80 times took minutes on the Vita. */
#define MM_CACHE 16
static struct { char path[512]; unsigned char *data; unsigned int len; } s_mmcache[MM_CACHE];

void *w32_mioOpenA(char *name, void *info, uint flags)
{
    char path[512];
    void *fp;
    MmFile *m;
    int len, i;
    (void)info; (void)flags;
    rt_map_path(name, path, sizeof path);
    m = (MmFile *)host_calloc(1, sizeof *m);
    for (i = 0; i < MM_CACHE; i++)
        if (s_mmcache[i].data && host_strcmp(s_mmcache[i].path, path) == 0) {
            m->data = s_mmcache[i].data;
            m->len = s_mmcache[i].len;
            return m;
        }
    fp = host_fopen(path, "rb");
    if (!fp) { rt_log("mmioOpen: cannot open %s\n", path); host_free(m); return NULL; }
    host_fseek(fp, 0, 2);
    len = host_ftell(fp);
    host_fseek(fp, 0, 0);
    m->data = (unsigned char *)host_malloc(len + 8);
    m->len = (unsigned int)host_fread(fp, m->data, len);
    host_fclose(fp);
    for (i = 0; i < MM_CACHE; i++)
        if (!s_mmcache[i].data) {
            host_snprintf(s_mmcache[i].path, sizeof s_mmcache[i].path, "%s", path);
            s_mmcache[i].data = m->data;
            s_mmcache[i].len = m->len;
            break;
        }
    return m;
}

static int mm_cached(const unsigned char *data)
{
    int i;
    for (i = 0; i < MM_CACHE; i++) if (s_mmcache[i].data == data) return 1;
    return 0;
}

uint w32_mioClose(void *h, uint fl)
{
    MmFile *m = (MmFile *)h;
    (void)fl;
    if (!m) return 0;
    if (!mm_cached(m->data)) host_free(m->data);
    host_free(m);
    return 0;
}

int w32_mioRead(void *h, char *buf, int n)
{
    MmFile *m = (MmFile *)h;
    unsigned int k;
    if (!m || n <= 0) return 0;
    k = m->len - m->pos;
    if ((unsigned int)n < k) k = (unsigned int)n;
    memcpy(buf, m->data + m->pos, k);
    m->pos += k;
    return (int)k;
}

int w32_mioSeek(void *h, int off, int whence)
{
    MmFile *m = (MmFile *)h;
    long p;
    if (!m) return -1;
    p = whence == 0 ? off : whence == 1 ? (long)m->pos + off : (long)m->len + off;
    if (p < 0 || (unsigned long)p > m->len) { rt_log("mmioSeek: %ld outside %u" "\n", p, m->len); return -1; }
    m->pos = (unsigned int)p;
    return (int)p;
}

static uint rd4(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint)p[3] << 24); }

/* MMIO_FINDCHUNK 0x10, MMIO_FINDRIFF 0x20, MMIO_FINDLIST 0x40 */
uint w32_mioDescend(void *h, void *ckv, void *parentv, uint flags)
{
    MmFile *m = (MmFile *)h;
    MMCK *ck = (MMCK *)ckv, *parent = (MMCK *)parentv;
    unsigned int end = parent ? parent->dwDataOffset + parent->cksize : m->len;
    uint want = ck->ckid, wanttype = ck->fccType;
    unsigned int start;
    if (!m) return 0x101;
    start = m->pos;
    while (m->pos + 8 <= end) {
        uint id = rd4(m->data + m->pos), size = rd4(m->data + m->pos + 4);
        uint type = 0;
        unsigned int data = m->pos + 8;
        if (id == 0x46464952 || id == 0x5453494c) {   /* RIFF / LIST */
            type = m->pos + 12 <= m->len ? rd4(m->data + m->pos + 8) : 0;
            data += 4;
        }
        if ((flags & 0x10 && id == want) || (flags & 0x20 && id == 0x46464952 && type == wanttype) ||
            (flags & 0x40 && id == 0x5453494c && type == wanttype) || !(flags & 0x70)) {
            ck->ckid = id;
            ck->cksize = (id == 0x46464952 || id == 0x5453494c) ? size - 4 : size;
            ck->fccType = type;
            ck->dwDataOffset = data;
            ck->dwFlags = 0;
            m->pos = data;
            return 0;
        }
        /* the pad byte follows an odd-sized chunk (not an odd file offset:
         * the sound banks are WAV files concatenated at any offset) */
        m->pos = data + ((id == 0x46464952 || id == 0x5453494c) ? size - 4 : size) + (size & 1);
    }
    rt_log("mmioDescend: %.4s not found (from %u, end %u, len %u)" "\n", (const char *)&want, start, end, m->len);
    return 0x104;   /* MMIOERR_CHUNKNOTFOUND */
}

uint w32_mioAscend(void *h, void *ckv, uint fl)
{
    MmFile *m = (MmFile *)h;
    MMCK *ck = (MMCK *)ckv;
    (void)fl;
    if (!m) return 0x101;
    m->pos = ck->dwDataOffset + ck->cksize + (ck->cksize & 1);   /* pad after an odd size */
    if (m->pos > m->len) m->pos = m->len;
    return 0;
}

/* MMIOINFO: dwFlags, fccIOProc, pIOProc, wErrorRet, htask, cchBuffer, pchBuffer,
 * pchNext, pchEndRead, pchEndWrite, lBufOffset, lDiskOffset, ... (we expose the
 * whole file as the I/O buffer so the game can read through pchNext). */
uint w32_mioGetInfo(void *h, void *infov, uint fl)
{
    MmFile *m = (MmFile *)h;
    uint *info = (uint *)infov;
    (void)fl;
    memset(info, 0, 72);
    info[5] = m->len;                                          /* cchBuffer */
    info[6] = (uint)(recomp_uptr)m->data;                      /* pchBuffer */
    info[7] = (uint)(recomp_uptr)(m->data + m->pos);           /* pchNext */
    info[8] = (uint)(recomp_uptr)(m->data + m->len);           /* pchEndRead */
    info[9] = (uint)(recomp_uptr)(m->data + m->len);
    info[10] = 0;                                              /* lBufOffset */
    info[11] = m->pos;                                         /* lDiskOffset */
    return 0;
}

uint w32_mioSetInfo(void *h, void *infov, uint fl)
{
    MmFile *m = (MmFile *)h;
    uint *info = (uint *)infov;
    (void)fl;
    m->pos = (unsigned int)((unsigned char *)(recomp_uptr)info[7] - m->data);
    if (m->pos > m->len) m->pos = m->len;
    return 0;
}

uint w32_mioAdvance(void *h, void *infov, uint fl)
{
    /* the whole file is already "buffered": nothing more to read */
    w32_mioSetInfo(h, infov, fl);
    return w32_mioGetInfo(h, infov, fl);
}

/* ---- joystick (joyGetPosEx) ---------------------------------------------- */
extern PlatPad g_pad;

uint w32_oyGetNumDevs(void) { return 1; }

uint w32_oyGetDevCapsA(uint id, void *capsv, uint cb)
{
    uint *c = (uint *)capsv;
    if (id != 0) return 167;   /* JOYERR_UNPLUGGED */
    memset(capsv, 0, cb);
    ((ushort *)c)[0] = 0x45e;  /* wMid */
    ((ushort *)c)[1] = 1;      /* wPid */
    host_snprintf((char *)c + 4, 32, "PS Vita");
    /* wXmin.. at offset 36: x 0..65535, y, z */
    c[9] = 0; c[10] = 65535; c[11] = 0; c[12] = 65535; c[13] = 0; c[14] = 65535;
    c[15] = 12;   /* wNumButtons */
    c[16] = 10; c[17] = 1000;   /* period min/max */
    return 0;
}

uint w32_oyGetPosEx(uint id, void *jiv)
{
    uint *ji = (uint *)jiv;
    if (id != 0) return 167;
    ji[2] = (uint)((g_pad.lx + 128) * 257);     /* dwXpos */
    ji[3] = (uint)((g_pad.ly + 128) * 257);     /* dwYpos */
    ji[4] = (uint)(32767 + g_pad.rtrig * 128 - g_pad.ltrig * 128);  /* dwZpos */
    ji[5] = 32767; ji[6] = 32767; ji[7] = 32767;
    ji[8] = g_pad.buttons;                      /* dwButtons */
    ji[9] = 0;
    ji[10] = 0xffff;                            /* dwPOV centered */
    return 0;
}

uint w32_oySetCapture(void *hwnd, uint id, uint period, int changed) { (void)hwnd; (void)id; (void)period; (void)changed; return 0; }
uint w32_oyReleaseCapture(uint id) { (void)id; return 0; }

uint w32_acmMetrics(void *hao, uint metric, void *out) { (void)hao; (void)metric; if (out) *(uint *)out = 0; return 0; }
