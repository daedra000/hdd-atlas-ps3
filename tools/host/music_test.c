/*
music_test.c: music.c on a PC: a one-second tone (made by host-test.sh with ffmpeg) played
three seconds through, so it loops; turned off (it fades to silence and keeps its place) and on
again; and data/music.ogg opens as 48 kHz stereo.

  music_test <tone.ogg> <data/music.ogg>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../../src/plat.h"
#include "../../src/music.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fflush(stdout); exit(1); } } while (0)

// frames of output, as the audio thread would take them (256 at a time, in real time-ish)
static double Pull(int frames, int *silent)
{
	float out[512];
	double e = 0;
	*silent = 0;
	for (int done = 0; done < frames; done += 256) {
		memset(out, 0, sizeof(out));
		music_mix(out, 256);
		for (int i = 0; i < 256; i++) {
			double v = out[i * 2];
			e += v * v;
			if (fabs(v) < 1e-6 && fabs(out[i * 2 + 1]) < 1e-6) (*silent)++;
		}
		usleep(2000);		// (faster than real time, slower than the decoder)
	}
	return sqrt(e / frames);
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	int silent;
	CHECK(music_start(argv[1]));
	usleep(100000);		// (the decoder fills the ring)
	double rms = Pull(3 * MUSIC_RATE, &silent);		// three times the file: it loops
	printf("tone: rms %.3f, %d silent frames of %d\n", rms, silent, 3 * MUSIC_RATE);
	CHECK(rms > 0.02 && silent < MUSIC_RATE / 10);		// (the fade-in's first frames are near silent)
	music_set_on(0);
	Pull(MUSIC_RATE / 2, &silent);					// the fade out
	rms = Pull(MUSIC_RATE / 2, &silent);
	printf("off: rms %g, %d silent of %d\n", rms, silent, MUSIC_RATE / 2);
	CHECK(rms == 0 && silent >= MUSIC_RATE / 2);		// (Pull goes in 256s: a few more)
	music_set_on(1);
	Pull(MUSIC_RATE / 2, &silent);
	rms = Pull(MUSIC_RATE / 2, &silent);
	CHECK(rms > 0.02);
	music_stop();
	CHECK(!music_start("/nonexistent.ogg"));
	CHECK(music_start(argv[2]));
	music_stop();
	printf("music_test: all PASS\n");
	return 0;
}
