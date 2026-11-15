/* audio.h — software mixer: DirectSound voices + one streamed MP3 (CD music). */
#ifndef AUDIO_H
#define AUDIO_H

typedef struct AVoice {
    unsigned char *data;    /* owned by the DirectSound buffer */
    unsigned int len;       /* bytes */
    int bits, channels;
    unsigned int freq;      /* playback frequency (Hz) */
    unsigned int pos;       /* 16.16 fixed position in frames */
    int playing, looping;
    int vol;                /* 0..256 */
    int pan;                /* -256 (left) .. 256 (right) */
    struct AVoice *next;
} AVoice;

void audio_init(void);
void audio_update(void);                 /* main thread: deliver end-of-music events */
void audio_voice_add(AVoice *v);
void audio_voice_remove(AVoice *v);
void audio_lock(void);
void audio_unlock(void);

int  audio_music_play(const char *path, int loop, void (*on_end)(void));
void audio_music_stop(void);
void audio_music_pause(int paused);
int  audio_music_is_open(void);          /* a track is loaded (playing or paused) */
void audio_music_volume(int vol);        /* 0..256 */

#endif
