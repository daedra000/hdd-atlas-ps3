/*
music.h: the background music (data/music.ogg, looped), decoded on a thread of
its own and mixed under the UI's sounds by the audio thread.
*/
#ifndef MUSIC_H
#define MUSIC_H

#define MUSIC_RATE	48000		// the file's rate, the audio port's: no resampling

int music_start(const char *path);	// opens it and starts decoding; 0 if it can't
void music_stop(void);
void music_set_on(int on);			// fades in or out (it keeps its place)
void music_set_level(float level);	// 0..1, how loud at most (the file's already quiet; 0.5 by default)
int music_on(void);
// adds frames of music (stereo, interleaved) onto out; the audio thread's call, never blocks
// (what isn't decoded yet is left out: silence, not a wait)
void music_mix(float *out, int frames);

#endif
