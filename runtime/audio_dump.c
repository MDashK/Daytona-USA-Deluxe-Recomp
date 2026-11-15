/* audio_dump.c — debug recordings of the game's sound (host testing).
 *
 *   DAYTONA_WAVDUMP=1  every sound buffer the game plays is written once to
 *                      snd/NNNN_<len>.wav (its own format), and each Play is
 *                      logged ("snd play ...") with volume, pan and frequency
 *   DAYTONA_MIXREC=1   the mixer output (44.1 kHz stereo) goes to mix.raw
 */
#include "dx.h"
#include "audio_dump.h"

static int s_wav = -1, s_mix = -1, s_seq;
static void *s_mixf;

typedef struct { unsigned int hash, len; int id; } Seen;
static Seen s_seen[1024];
static int s_nseen;

static void put32(unsigned char *p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put16(unsigned char *p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static unsigned int fnv(const unsigned char *d, unsigned int n)
{
    unsigned int h = 2166136261u, i;
    for (i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

void audio_dump_play(const void *buf, const unsigned char *data, unsigned int len, int channels, int bits,
                     unsigned int rate, unsigned int freq, int db_vol, int db_pan, int looping, unsigned int pos)
{
    unsigned int h;
    int i, id = -1;
    if (s_wav < 0) { s_wav = getenv_flag("DAYTONA_WAVDUMP"); if (s_wav) host_mkdir("snd"); }
    if (!s_wav || !data) return;
    h = fnv(data, len);
    for (i = 0; i < s_nseen; i++) if (s_seen[i].hash == h && s_seen[i].len == len) { id = s_seen[i].id; break; }
    if (id < 0 && s_nseen < 1024) {
        char name[64];
        unsigned char hdr[44];
        void *f;
        id = s_seq++;
        s_seen[s_nseen].hash = h; s_seen[s_nseen].len = len; s_seen[s_nseen].id = id; s_nseen++;
        host_snprintf(name, sizeof name, "snd/%04d_%u.wav", id, len);
        memcpy(hdr, "RIFF", 4); put32(hdr + 4, 36 + len); memcpy(hdr + 8, "WAVEfmt ", 8);
        put32(hdr + 16, 16); put16(hdr + 20, 1); put16(hdr + 22, (unsigned)channels);
        put32(hdr + 24, rate); put32(hdr + 28, rate * (unsigned)channels * (unsigned)bits / 8);
        put16(hdr + 32, (unsigned)(channels * bits / 8)); put16(hdr + 34, (unsigned)bits);
        memcpy(hdr + 36, "data", 4); put32(hdr + 40, len);
        if ((f = host_fopen(name, "wb")) != NULL) { host_fwrite(f, hdr, 44); host_fwrite(f, data, len); host_fclose(f); }
    }
    rt_log("snd play id=%d buf=%p len=%u fmt=%dch/%dbit/%uHz freq=%u vol=%d pan=%d loop=%d pos=%u\n",
           id, buf, len, channels, bits, rate, freq, db_vol, db_pan, looping, pos);
}

void audio_dump_mix(const short *out, int frames)
{
    if (s_mix < 0) { s_mix = getenv_flag("DAYTONA_MIXREC"); if (s_mix) s_mixf = host_fopen("mix.raw", "wb"); }
    if (s_mix && s_mixf) host_fwrite(s_mixf, out, (unsigned)(frames * 4));
}
