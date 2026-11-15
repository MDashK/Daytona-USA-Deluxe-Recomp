/* dx_sound_input.c — DirectSound (on audio.c) and DirectInput (on the pad). */
#include "dx.h"
#include "audio.h"
#include "audio_dump.h"

static void *com_new(METHOD *vtbl, int kind, unsigned int size)
{
    ComHdr *o = (ComHdr *)host_calloc(1, size);
    o->vtbl = vtbl; o->refs = 1; o->kind = kind;
    return o;
}
static uint X_AddRef(ComHdr *o) { return (uint)++o->refs; }
static uint X_QI(ComHdr *o, void *iid, void **out) { (void)iid; *out = o; o->refs++; return DD_OK; }
static uint X_Ok(void) { return DD_OK; }

/* ---- DirectSound --------------------------------------------------------------- */
typedef struct DSBuf {
    ComHdr h;
    AVoice v;
    uint flags;
    unsigned char wfx[18];
    int primary;
    int dsvol, dspan;          /* as last set (GetVolume/GetPan return them) */
} DSBuf;
extern METHOD vt_dsbuf[];

double pow(double, double);

/* DSBVOLUME / DSBPAN are attenuations in hundredths of a dB:
 * gain = 10^(dB/20) = 10^(db100/2000), as 0..256 */
static int gain_from_db(int db100)
{
    if (db100 >= 0) return 256;
    if (db100 <= -10000) return 0;
    return (int)(pow(10.0, (double)db100 / 2000.0) * 256.0 + 0.5);
}
static int vol_from_db(int db100) { return gain_from_db(db100); }

static uint B_Release(DSBuf *b)
{
    if (--b->h.refs > 0) return (uint)b->h.refs;
    audio_voice_remove(&b->v);
    host_free(b->v.data);
    b->v.data = NULL;
    return 0;
}
static uint B_GetCaps(DSBuf *b, uint *caps) { caps[1] = b->flags; caps[2] = b->v.len; return DD_OK; }
static uint B_GetCurrentPosition(DSBuf *b, uint *play, uint *write)
{
    uint bpf = (uint)(b->v.bits / 8 * b->v.channels);
    uint p = (b->v.pos >> 16) * bpf;
    if (p > b->v.len) p = b->v.len;
    if (play) *play = p;
    if (write) *write = p;
    return DD_OK;
}
static uint B_GetFormat(DSBuf *b, void *wfx, uint sz, uint *got) { if (wfx) memcpy(wfx, b->wfx, sz < 18 ? sz : 18); if (got) *got = 18; return DD_OK; }
static uint B_GetVolume(DSBuf *b, int *v) { *v = b->dsvol; return DD_OK; }
static uint B_GetPan(DSBuf *b, int *p) { *p = b->dspan; return DD_OK; }
static uint B_GetFrequency(DSBuf *b, uint *f) { *f = b->v.freq; return DD_OK; }
static uint B_GetStatus(DSBuf *b, uint *st) { *st = (b->v.playing ? 1 : 0) | (b->v.looping && b->v.playing ? 4 : 0); return DD_OK; }
static uint B_Lock(DSBuf *b, uint off, uint n, void **p1, uint *n1, void **p2, uint *n2, uint flags)
{
    if (flags & 2) { off = 0; n = b->v.len; }       /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) { uint w; B_GetCurrentPosition(b, NULL, &w); off = w; }
    if (off >= b->v.len) off = 0;
    if (off + n <= b->v.len) {
        *p1 = b->v.data + off; *n1 = n;
        if (p2) *p2 = NULL;
        if (n2) *n2 = 0;
    } else {
        *p1 = b->v.data + off; *n1 = b->v.len - off;
        if (p2) *p2 = b->v.data;
        if (n2) *n2 = n - (b->v.len - off);
    }
    return DD_OK;
}
static int s_dslog = -1;
static int dslog(void) { if (s_dslog < 0) s_dslog = getenv_flag("DAYTONA_DSLOG"); return s_dslog; }
static uint B_Play(DSBuf *b, uint r1, uint r2, uint flags)
{
    (void)r1; (void)r2;
    if (dslog()) rt_log("ds play %p fl=%x len=%u fmt=%u/%u/%u freq=%u vol=%d pan=%d pos=%u playing=%d\n", (void *)b, flags, b->v.len,
                        b->v.channels, b->v.bits, b->wfx[4] | (b->wfx[5] << 8) | (b->wfx[6] << 16), b->v.freq, b->v.vol, b->v.pan, b->v.pos >> 16, b->v.playing);
    if (b->primary) return DD_OK;
    audio_dump_play(b, b->v.data, b->v.len, b->v.channels, b->v.bits, b->wfx[4] | (b->wfx[5] << 8) | (b->wfx[6] << 16),
                    b->v.freq, b->dsvol, b->dspan, (flags & 1) != 0, b->v.pos >> 16);
    b->v.looping = (flags & 1) != 0;
    b->v.playing = 1;
    audio_voice_add(&b->v);
    return DD_OK;
}
static uint B_SetCurrentPosition(DSBuf *b, uint pos)
{
    uint bpf = (uint)(b->v.bits / 8 * b->v.channels);
    b->v.pos = bpf ? (pos / bpf) << 16 : 0;
    return DD_OK;
}
static uint B_SetFormat(void) { return DD_OK; }
static uint B_SetVolume(DSBuf *b, int v) { b->v.vol = vol_from_db(v); b->dsvol = v; return DD_OK; }
static uint B_SetPan(DSBuf *b, int p)
{
    /* pan > 0 attenuates the left channel by p hundredths of a dB, pan < 0
     * the right one; v.pan stores the attenuated side's lost gain (0..256) */
    if (p > 10000) p = 10000;
    if (p < -10000) p = -10000;
    b->v.pan = p >= 0 ? 256 - gain_from_db(-p) : -(256 - gain_from_db(p));
    b->dspan = p;
    return DD_OK;
}
static uint B_SetFrequency(DSBuf *b, uint f)
{
    uint orig = b->wfx[4] | (b->wfx[5] << 8) | (b->wfx[6] << 16);
    b->v.freq = f ? f : orig;
    return DD_OK;
}
static uint B_Stop(DSBuf *b) { b->v.playing = 0; return DD_OK; }
static uint B_Unlock(void) { return DD_OK; }
static uint B_Restore(void) { return DD_OK; }
METHOD vt_dsbuf[] = {
    (METHOD)X_QI, (METHOD)X_AddRef, (METHOD)B_Release, (METHOD)B_GetCaps,
    (METHOD)B_GetCurrentPosition, (METHOD)B_GetFormat, (METHOD)B_GetVolume, (METHOD)B_GetPan,
    (METHOD)B_GetFrequency, (METHOD)B_GetStatus, (METHOD)X_Ok, (METHOD)B_Lock,
    (METHOD)B_Play, (METHOD)B_SetCurrentPosition, (METHOD)B_SetFormat, (METHOD)B_SetVolume,
    (METHOD)B_SetPan, (METHOD)B_SetFrequency, (METHOD)B_Stop, (METHOD)B_Unlock,
    (METHOD)B_Restore,
};

static DSBuf *dsbuf_new(uint flags, uint len, const unsigned char *wfx)
{
    DSBuf *b = (DSBuf *)com_new(vt_dsbuf, K_DSBUF, sizeof(DSBuf));
    b->flags = flags;
    if (wfx) memcpy(b->wfx, wfx, 18);
    else { b->wfx[0] = 1; b->wfx[2] = 2; b->wfx[4] = 0x44; b->wfx[5] = 0xac; b->wfx[14] = 16; }
    b->v.channels = b->wfx[2] | (b->wfx[3] << 8);
    b->v.freq = b->wfx[4] | (b->wfx[5] << 8) | (b->wfx[6] << 16);
    b->v.bits = b->wfx[14] | (b->wfx[15] << 8);
    if (!b->v.channels) b->v.channels = 1;
    if (b->v.bits != 8 && b->v.bits != 16) b->v.bits = 16;
    b->v.len = len;
    b->v.data = (unsigned char *)host_calloc(1, len + 4);
    /* a new buffer is silent; for unsigned 8-bit PCM silence is 0x80, not 0
     * (the game makes its stereo buffers channels*size bytes and fills only
     * the first half: zeros there were a full-scale DC block) */
    if (b->v.bits == 8 && b->v.data) memset(b->v.data, 0x80, len + 4);
    b->v.vol = 256;
    return b;
}

static uint S_CreateSoundBuffer(ComHdr *ds, uint *desc, void **out, void *u)
{
    uint flags = desc[1], len = desc[2];
    const unsigned char *wfx = (const unsigned char *)(recomp_uptr)desc[4];
    DSBuf *b;
    (void)ds; (void)u;
    if (flags & 1) {                       /* DSBCAPS_PRIMARYBUFFER */
        b = dsbuf_new(flags, 4096, NULL);
        b->primary = 1;
    } else {
        b = dsbuf_new(flags, len, wfx);
    }
    if (dslog()) rt_log("ds create %p fl=%x len=%u fmt tag=%u ch=%u rate=%u bits=%u\n", (void *)b, flags, len,
                        b->wfx[0] | (b->wfx[1] << 8), b->v.channels, b->v.freq, b->v.bits);
    *out = b;
    return DD_OK;
}
static uint S_GetCaps(ComHdr *ds, uint *caps) { (void)ds; memset(caps + 1, 0, caps[0] > 4 ? caps[0] - 4 : 0); caps[1] = 0x1 | 0x4 | 0x10 | 0x20 | 0x400 | 0x800; caps[2] = 100; caps[3] = 100000; return DD_OK; }
static uint S_DuplicateSoundBuffer(ComHdr *ds, DSBuf *src, void **out)
{
    DSBuf *b = dsbuf_new(src->flags, src->v.len, src->wfx);
    (void)ds;
    memcpy(b->v.data, src->v.data, src->v.len);
    if (dslog()) rt_log("ds dup %p -> %p len=%u\n", (void *)src, (void *)b, src->v.len);
    *out = b;
    return DD_OK;
}
static uint S_GetSpeakerConfig(ComHdr *ds, uint *c) { (void)ds; *c = 4; return DD_OK; }
METHOD vt_dsound[] = { (METHOD)X_QI, (METHOD)X_AddRef, (METHOD)X_Ok, (METHOD)S_CreateSoundBuffer,
                       (METHOD)S_GetCaps, (METHOD)S_DuplicateSoundBuffer, (METHOD)X_Ok, (METHOD)X_Ok,
                       (METHOD)S_GetSpeakerConfig, (METHOD)X_Ok, (METHOD)X_Ok };

int w32_DirectSoundCreate(void *guid, void *out, void *outer)
{
    (void)guid; (void)outer;
    *(void **)out = com_new(vt_dsound, K_DSOUND, sizeof(ComHdr));
    rt_log("DirectSoundCreate\n");
    return DD_OK;
}

/* ---- DirectInput ------------------------------------------------------------ */
extern PlatPad g_pad;
typedef struct DIDev { ComHdr h; int keyboard; uint fmt_size; int rmin, rmax; } DIDev;

/* DIK scan codes for the pad (keyboard device), following the game's default
 * player-1 keys (FUN_0044131b): X accelerate, Z brake, C view, Up/Down gears,
 * Left/Right steer, Enter select, Esc back. */
static const struct { unsigned int bit; int dik; } s_dik[] = {
    { PAD_UP, 0xc8 }, { PAD_DOWN, 0xd0 }, { PAD_LEFT, 0xcb }, { PAD_RIGHT, 0xcd },
    { PAD_CROSS, 0x2d },                        /* X: accelerate */
    { PAD_SQUARE, 0x2c }, { PAD_CIRCLE, 0x2c }, /* Z: brake */
    { PAD_TRIANGLE, 0x2e }, { PAD_TRIANGLE, 0x0f }, /* C: change view, Tab: Records option */
    { PAD_R, 0xc8 }, { PAD_L, 0xd0 },           /* Up/Down: shift up/down */
    { PAD_START, 0x1c },                        /* Enter (Select is the port's menu; Circle = back) */
};

static uint DD_GetCapabilities(DIDev *d, uint *caps)
{
    memset(caps + 1, 0, caps[0] > 4 ? caps[0] - 4 : 0);
    caps[1] = 0x5;              /* attached, polled-data */
    caps[2] = d->keyboard ? 0x103 : 0x104;   /* DIDEVTYPE_KEYBOARD / JOYSTICK */
    caps[3] = d->keyboard ? 0 : 3;           /* axes */
    caps[4] = d->keyboard ? 128 : 12;        /* buttons */
    return DD_OK;
}
static uint DD_EnumObjects(void) { return DD_OK; }
/* DIPROP_RANGE is MAKEDIPROP(4): the guid argument is the number 4 */
static uint DD_GetProperty(DIDev *d, void *guid, uint *ph)
{
    if ((recomp_uptr)guid == 4 && ph && ph[0] >= 24) { ph[4] = (uint)d->rmin; ph[5] = (uint)d->rmax; }
    return DD_OK;
}
static uint DD_SetProperty(DIDev *d, void *guid, uint *ph)
{
    if ((recomp_uptr)guid == 4 && ph && ph[0] >= 24) { d->rmin = (int)ph[4]; d->rmax = (int)ph[5]; }
    return DD_OK;
}
/* -1.0..1.0 (as -32768..32767) to the device's range */
static int axis_scale(DIDev *d, int v)
{
    long long span = (long long)d->rmax - d->rmin;
    return d->rmin + (int)(((long long)(v + 32768) * span) / 65535);
}
static uint DD_GetDeviceState(DIDev *d, uint n, unsigned char *buf)
{
    unsigned int i;
    memset(buf, 0, n);
    if (d->keyboard) {
        static int joyonly = -1;   /* DAYTONA_JOYONLY=1: drive through the joystick only (testing) */
        if (joyonly < 0) joyonly = getenv_flag("DAYTONA_JOYONLY");
        for (i = 0; i < sizeof s_dik / sizeof s_dik[0]; i++)
            if ((g_pad.buttons & s_dik[i].bit) && (unsigned)s_dik[i].dik < n &&
                (!joyonly || (s_dik[i].bit & (PAD_START | PAD_SELECT)))) buf[s_dik[i].dik] = 0x80;
    } else if (n >= 80) {
        /* DIJOYSTATE: lX lY lZ lRx lRy lRz rglSlider[2] rgdwPOV[4] rgbButtons[32]
         * laid out for the game's "SEGA SATURN PAD" profile (FUN_004411a2):
         * X = steering (stick, or D-pad at full lock), Y = gears (D-pad
         * up/down, R/L; never the stick), button 1 accelerate, 2 brake,
         * 3 view, 9 start */
        int *ax = (int *)buf, x, y = 0;
        unsigned int b = g_pad.buttons;
        x = g_pad.lx * 256;
        if (b & PAD_LEFT) x = -32768;
        if (b & PAD_RIGHT) x = 32767;
        if (b & (PAD_UP | PAD_R)) y = -32768;
        if (b & (PAD_DOWN | PAD_L)) y = 32767;
        ax[0] = axis_scale(d, x);
        ax[1] = axis_scale(d, y);
        ax[2] = ax[3] = ax[4] = ax[5] = axis_scale(d, 0);
        ((uint *)buf)[8] = ((uint *)buf)[9] = ((uint *)buf)[10] = ((uint *)buf)[11] = 0xffffffffu;
        buf[48 + 0] = (b & PAD_CROSS) ? 0x80 : 0;
        buf[48 + 1] = (b & (PAD_SQUARE | PAD_CIRCLE)) ? 0x80 : 0;
        buf[48 + 2] = (b & PAD_TRIANGLE) ? 0x80 : 0;
        buf[48 + 8] = (b & PAD_START) ? 0x80 : 0;
    }
    return DD_OK;
}
static uint DD_GetDeviceData(DIDev *d, uint sz, void *data, uint *n, uint fl) { (void)d; (void)sz; (void)data; (void)fl; if (n) *n = 0; return DD_OK; }
static uint DD_SetDataFormat(DIDev *d, uint *fmt) { d->fmt_size = fmt ? fmt[3] : 0; return DD_OK; }
static uint DD_GetObjectInfo(void) { return DDERR_UNSUPPORTED; }
static uint DD_GetDeviceInfo(DIDev *d, uint *inst)
{
    memset(inst + 1, 0, inst[0] > 4 ? inst[0] - 4 : 0);
    inst[9] = d->keyboard ? 0x103 : 0x104;
    host_snprintf((char *)(inst + 10), 260, d->keyboard ? "Keyboard" : "SEGA SATURN PAD");
    host_snprintf((char *)(inst + 75), 260, d->keyboard ? "Keyboard" : "SEGA SATURN PAD");
    return DD_OK;
}
static uint DD_NoEffect(void) { return DDERR_UNSUPPORTED; }
METHOD vt_didev[] = {
    (METHOD)X_QI, (METHOD)X_AddRef, (METHOD)X_Ok, (METHOD)DD_GetCapabilities,
    (METHOD)DD_EnumObjects, (METHOD)DD_GetProperty, (METHOD)DD_SetProperty, (METHOD)X_Ok /* Acquire */,
    (METHOD)X_Ok /* Unacquire */, (METHOD)DD_GetDeviceState, (METHOD)DD_GetDeviceData, (METHOD)DD_SetDataFormat,
    (METHOD)X_Ok /* SetEventNotification */, (METHOD)X_Ok /* SetCooperativeLevel */, (METHOD)DD_GetObjectInfo, (METHOD)DD_GetDeviceInfo,
    (METHOD)X_Ok /* RunControlPanel */, (METHOD)X_Ok /* Initialize */,
    /* IDirectInputDevice2 */
    (METHOD)DD_NoEffect /* CreateEffect */, (METHOD)X_Ok /* EnumEffects */, (METHOD)DD_NoEffect /* GetEffectInfo */,
    (METHOD)DD_NoEffect /* GetForceFeedbackState */, (METHOD)DD_NoEffect /* SendForceFeedbackCommand */,
    (METHOD)X_Ok /* EnumCreatedEffectObjects */, (METHOD)DD_NoEffect /* Escape */, (METHOD)X_Ok /* Poll */,
    (METHOD)X_Ok /* SendDeviceData */,
};

static uint I_CreateDevice(ComHdr *di, void *guid, void **out, void *u)
{
    DIDev *d = (DIDev *)com_new(vt_didev, K_DIDEV, sizeof(DIDev));
    (void)di; (void)u;
    d->keyboard = guid_is(guid, "6F1D2B61-D5A0-11CF-BFC7-444553540000");
    d->rmin = 0; d->rmax = 65535;
    rt_log("DirectInput: CreateDevice (%s)\n", d->keyboard ? "keyboard" : "joystick/mouse");
    *out = d;
    return DD_OK;
}
static uint I_EnumDevices(ComHdr *di, uint type, int (*cb)(), void *ctx, uint fl)
{
    static uint inst[146];
    (void)di; (void)fl;
    if (type == 0 || type == 4) {           /* DIDEVTYPE_JOYSTICK */
        memset(inst, 0, sizeof inst);
        inst[0] = 580;
        inst[1] = 0x6f1d2b70; inst[2] = 0x11cfd5a0; inst[3] = 0x4445bfc7; inst[4] = 0x00005453;  /* instance guid */
        inst[9] = 0x104;
        host_snprintf((char *)(inst + 10), 260, "SEGA SATURN PAD");
        host_snprintf((char *)(inst + 75), 260, "SEGA SATURN PAD");
        cb(inst, ctx);
    }
    return DD_OK;
}
static uint I_GetDeviceStatus(void) { return DD_OK; }
METHOD vt_dinput[] = { (METHOD)X_QI, (METHOD)X_AddRef, (METHOD)X_Ok, (METHOD)I_CreateDevice,
                       (METHOD)I_EnumDevices, (METHOD)I_GetDeviceStatus, (METHOD)X_Ok, (METHOD)X_Ok };

int w32_DirectInputCreateA(void *hinst, uint ver, void *out, void *outer)
{
    (void)hinst; (void)outer;
    rt_log("DirectInputCreateA(0x%x)\n", ver);
    *(void **)out = com_new(vt_dinput, K_DINPUT, sizeof(ComHdr));
    return DD_OK;
}

/* ---- DirectPlay / COM: no network play ---------------------------------- */
int w32_DirectPlayCreate(void *g, void *out, void *u) { (void)g; (void)u; if (out) *(void **)out = NULL; return (int)DDERR_UNSUPPORTED; }
int w32_DirectPlayLobbyCreateA(void *g, void *out, void *u, void *d, uint sz) { (void)g; (void)u; (void)d; (void)sz; if (out) *(void **)out = NULL; return (int)DDERR_UNSUPPORTED; }
HRESULT w32_CoInitialize(LPVOID r) { (void)r; return 0; }
void w32_CoUninitialize(void) { }
HRESULT w32_CoCreateInstance(IID *clsid, LPUNKNOWN outer, DWORD ctx, IID *iid, LPVOID *ppv)
{
    (void)clsid; (void)outer; (void)ctx; (void)iid;
    if (ppv) *ppv = NULL;
    rt_log("CoCreateInstance: class not available\n");
    return (HRESULT)0x80040154;   /* REGDB_E_CLASSNOTREG */
}
