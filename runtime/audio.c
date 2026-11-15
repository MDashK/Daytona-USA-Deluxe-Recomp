/* audio.c — the mixer behind DirectSound and the CD-audio (MP3) emulation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO_OFF
#include "dr_mp3.h"
#include "audio.h"
#include "plat.h"
#include "audio_dump.h"
#include "prof.h"

#define OUT_RATE 44100

static AVoice *s_voices;
static drmp3 s_mp3;
static int s_mp3_open, s_mp3_paused, s_mp3_loop;
/* the whole MP3 in memory: reading the memory card while the audio thread
 * decodes made the music skip when a read was slow */
static unsigned char *s_mp3_mem;
static volatile int s_mp3_ended;
static void (*s_on_end)(void);
static int s_music_vol = 256;
#define MUSIC_GAIN 180   /* /256: headroom for the effects (the CD rips are mastered loud) */
/* the options menu's volumes, in % (0..200; menu.c) */
int g_opt_bgm_vol = 100, g_opt_sfx_vol = 100;

void audio_lock(void) { PROF_PHASE(PH_AUDIOLOCK); plat_audio_lock(); PROF_END(); }
void audio_unlock(void) { plat_audio_unlock(); }

void audio_voice_add(AVoice *v)
{
    AVoice *p;
    audio_lock();
    for (p = s_voices; p; p = p->next) if (p == v) break;
    if (!p) { v->next = s_voices; s_voices = v; }
    audio_unlock();
}

void audio_voice_remove(AVoice *v)
{
    AVoice **pp;
    audio_lock();
    for (pp = &s_voices; *pp; pp = &(*pp)->next) {
        if (*pp == v) { *pp = v->next; break; }
    }
    audio_unlock();
}

static inline int clamp16(int x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }

/* The effects (six looping engine layers in a race) and the CD music add up
 * well past full scale; on the PC the CD audio was not even in DirectSound's
 * mix.  Below LIM_KNEE the signal passes unchanged, above it is rounded off
 * towards full scale (tanh) instead of being clipped flat. */
#define LIM_KNEE 22000
static inline int soft_limit(int x)
{
    int a = x < 0 ? -x : x;
    double t, y;
    if (a <= LIM_KNEE) return x;
    t = (double)(a - LIM_KNEE) / (32767 - LIM_KNEE);
    y = t < 3.0 ? t * (27.0 + t * t) / (27.0 + 9.0 * t * t) : 1.0;   /* tanh(t) */
    a = LIM_KNEE + (int)(y * (32767 - LIM_KNEE));
    return x < 0 ? -a : a;
}

static void mix_voice(AVoice *v, int *acc, int frames)
{
    unsigned int step = (unsigned int)(((unsigned long long)v->freq << 16) / OUT_RATE);
    unsigned int bpf = (unsigned int)(v->bits / 8 * v->channels);
    unsigned int nframes = bpf ? v->len / bpf : 0;
    int lv = v->vol * (v->pan > 0 ? 256 - v->pan : 256) >> 8;
    int rv = v->vol * (v->pan < 0 ? 256 + v->pan : 256) >> 8;
    int i;
    if (!nframes || !v->data) { v->playing = 0; return; }
    for (i = 0; i < frames; i++) {
        unsigned int f = v->pos >> 16;
        int l, r;
        if (f >= nframes) {
            if (!v->looping) { v->playing = 0; v->pos = 0; return; }
            v->pos -= nframes << 16;
            f = v->pos >> 16;
        }
        {
            /* linear interpolation between this frame and the next (the
             * effects are 8.8/22 kHz, often pitch-shifted, mixed at 44.1 kHz) */
            unsigned int f2 = f + 1 < nframes ? f + 1 : (v->looping ? 0 : f);
            int t = (int)((v->pos >> 4) & 0xfff), l2, r2;
            if (v->bits == 16) {
                const short *s = (const short *)(v->data + f * bpf), *s2 = (const short *)(v->data + f2 * bpf);
                l = s[0]; l2 = s2[0];
                r = v->channels > 1 ? s[1] : l;
                r2 = v->channels > 1 ? s2[1] : l2;
            } else {
                const unsigned char *s = v->data + f * bpf, *s2 = v->data + f2 * bpf;
                l = ((int)s[0] - 128) << 8; l2 = ((int)s2[0] - 128) << 8;
                r = v->channels > 1 ? ((int)s[1] - 128) << 8 : l;
                r2 = v->channels > 1 ? ((int)s2[1] - 128) << 8 : l2;
            }
            l += (l2 - l) * t >> 12;
            r += (r2 - r) * t >> 12;
        }
        acc[i * 2] += l * lv >> 8;
        acc[i * 2 + 1] += r * rv >> 8;
        v->pos += step;
    }
}

static void mix_music(int *acc, int frames)
{
    short buf[2048 * 2];
    int done = 0, gain = s_music_vol * MUSIC_GAIN / 256 * g_opt_bgm_vol / 100;   /* /256 */
    if (!s_mp3_open || s_mp3_paused) return;
    while (done < frames) {
        int want = frames - done, got, i, ch = (int)s_mp3.channels;
        if (want > 2048) want = 2048;
        got = (int)drmp3_read_pcm_frames_s16(&s_mp3, (drmp3_uint64)want, buf);
        if (got <= 0) {
            if (s_mp3_loop) { drmp3_seek_to_pcm_frame(&s_mp3, 0); continue; }
            drmp3_uninit(&s_mp3);
            s_mp3_open = 0;
            s_mp3_ended = 1;
            return;
        }
        for (i = 0; i < got; i++) {
            int l = buf[i * ch], r = ch > 1 ? buf[i * ch + 1] : l;
            acc[(done + i) * 2] += l * gain >> 8;
            acc[(done + i) * 2 + 1] += r * gain >> 8;
        }
        done += got;
    }
}

static void audio_cb(short *out, int frames)
{
    static int acc[4096 * 2];
    AVoice *v;
    int i;
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(int) * 2 * frames);
    for (v = s_voices; v; v = v->next) if (v->playing) mix_voice(v, acc, frames);
    if (g_opt_sfx_vol != 100) {
        int g = g_opt_sfx_vol * 256 / 100;
        for (i = 0; i < frames * 2; i++) acc[i] = acc[i] * g >> 8;
    }
    mix_music(acc, frames);
    for (i = 0; i < frames * 2; i++) out[i] = (short)clamp16(soft_limit(acc[i]));
    audio_dump_mix(out, frames);
    {   /* DAYTONA_MIXREC: once a second, the voices that are playing */
        static int on = -1, acc_frames;
        if (on < 0) on = getenv_flag("DAYTONA_MIXREC");
        if (on && (acc_frames += frames) >= OUT_RATE) {
            acc_frames = 0;
            rt_log("voices:");
            for (v = s_voices; v; v = v->next)
                if (v->playing) rt_log(" [len=%u %dHz vol=%d pan=%d%s]", v->len, v->freq, v->vol, v->pan, v->looping ? " loop" : "");
            rt_log("\n");
        }
    }
}

void audio_init(void)
{
    plat_audio_start(audio_cb);
}

void audio_update(void)
{
    if (s_mp3_ended) {
        void (*cb)(void) = s_on_end;
        s_mp3_ended = 0;
        s_on_end = NULL;
        if (cb) cb();
    }
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    setvbuf(f, NULL, _IONBF, 0);       /* one big read straight from the file, no 1 KB steps */
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* The whole MP3 goes to RAM (streaming from the card skipped), which took
 * ~1.5 s for a 7 MB track on the Vita: done on a background thread, so the
 * game does not freeze (race start); the music starts once it is in. */
struct music_req { char path[512]; int loop, gen; void (*on_end)(void); };
static volatile int s_music_gen, s_music_pending;
static void music_load_job(void *p)
{
    struct music_req *r = (struct music_req *)p;
    size_t len = 0;
    unsigned char *mem = load_file(r->path, &len);
    plat_audio_lock();         /* not audio_lock(): the profiler phase belongs to the game thread */
    if (r->gen != s_music_gen) {                 /* stopped or replaced meanwhile */
        free(mem);
    } else {
        s_music_pending = 0;
        if (s_mp3_open) { drmp3_uninit(&s_mp3); s_mp3_open = 0; }
        free(s_mp3_mem);
        s_mp3_mem = mem;
        if (mem && drmp3_init_memory(&s_mp3, s_mp3_mem, len, NULL)) {
            s_mp3_open = 1;
            s_mp3_ended = 0;
            s_mp3_loop = r->loop;
            s_on_end = r->on_end;
        } else {                                  /* unreadable: report the end (main thread) */
            free(s_mp3_mem);
            s_mp3_mem = NULL;
            s_on_end = r->on_end;
            s_mp3_ended = 1;
        }
    }
    plat_audio_unlock();
    free(r);
}

int audio_music_play(const char *path, int loop, void (*on_end)(void))
{
    struct music_req *r;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    r = (struct music_req *)malloc(sizeof *r);
    if (!r) return 0;
    snprintf(r->path, sizeof r->path, "%s", path);
    r->loop = loop;
    r->on_end = on_end;
    audio_lock();
    if (s_mp3_open) { drmp3_uninit(&s_mp3); s_mp3_open = 0; }
    free(s_mp3_mem);
    s_mp3_mem = NULL;
    s_on_end = NULL;
    s_mp3_ended = 0;
    s_mp3_paused = 0;
    r->gen = ++s_music_gen;
    s_music_pending = 1;
    audio_unlock();
    plat_thread_start(music_load_job, r);
    return 1;
}

void audio_music_stop(void)
{
    audio_lock();
    s_music_gen++;
    s_music_pending = 0;
    if (s_mp3_open) { drmp3_uninit(&s_mp3); s_mp3_open = 0; }
    free(s_mp3_mem);
    s_mp3_mem = NULL;
    s_on_end = NULL;
    s_mp3_ended = 0;
    audio_unlock();
}

int audio_music_is_open(void) { return s_mp3_open || s_music_pending; }

void audio_music_pause(int paused) { s_mp3_paused = paused; }
void audio_music_volume(int vol) { s_music_vol = vol; }
