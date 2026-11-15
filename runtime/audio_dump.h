/* audio_dump.h — debug recordings of the game's sound (see audio_dump.c) */
#ifndef AUDIO_DUMP_H
#define AUDIO_DUMP_H
void audio_dump_play(const void *buf, const unsigned char *data, unsigned int len, int channels, int bits,
                     unsigned int rate, unsigned int freq, int db_vol, int db_pan, int looping, unsigned int pos);
void audio_dump_mix(const short *out, int frames);
#endif
