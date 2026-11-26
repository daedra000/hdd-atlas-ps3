/*
music.c: the background music (music.h).

The whole file is read into memory at the start (9 MB): a scan keeps the HDD
so busy that a read of the next bit of the file could wait longer than the
ring lasts, and the music stuttered. From then on it never touches the disk.

A thread decodes the Ogg Vorbis file (stb_vorbis, public domain) into a ring
of about a second of stereo samples, a little at a time, and starts it again
at the end. The audio thread takes from the ring as it plays: one writer,
one reader, so no lock, only the two positions (each written by its own side).
Turning it off or on fades over a quarter second rather than clicking; while
it's off the decoder waits, so it costs nothing.

The file's made by tools/make-music.sh: 48 kHz (the audio port's rate) and
loudness-normalised quiet, so it sits under the UI's sounds.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "music.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "thirdparty/stb_vorbis.c"

#define RING		65536		// stereo frames (1.4 s at 48 kHz); a power of two
#define CHUNK		1024		// frames decoded at a time
#define FADE		(MUSIC_RATE / 4)
static volatile float level = 0.5f;		// (the file's -24 LUFS, 6 dB down again at 0.5: under the UI's sounds)
#define LEVEL		level

static float ring[RING * 2];
static volatile uint32_t wpos, rpos;		// frames written (decoder) and read (audio thread), counting up
static volatile int running, want_on = 1, live;
static stb_vorbis *vf;			// the decoder thread's alone once it's started (nothing else may move it)
static char vf_path[256];
static unsigned char *file;		// the whole .ogg (vf reads from it; freed after vf's closed)
static size_t file_len;
static float gain;				// the fade: 0..1 (the audio thread's)
static float chunk[CHUNK * 2];

static void Decoder(void *arg)
{
	(void)arg;
	int empty = 0;		// passes from the start that gave nothing: a file that won't decode
	while (running) {
		uint32_t room = RING - (wpos - __atomic_load_n(&rpos, __ATOMIC_ACQUIRE));
		if (room < CHUNK || (!want_on && gain <= 0)) { plat_sleep_us(10000); continue; }
		int n = stb_vorbis_get_samples_float_interleaved(vf, 2, chunk, CHUNK * 2);
		if (n <= 0) {		// the end: from the top again (or, if it won't go back, the file opened again)
			if (++empty >= 3) { plat_log("music: nothing decodes from %s (stb_vorbis %d): stopped", vf_path, stb_vorbis_get_error(vf)); break; }
			if (stb_vorbis_seek_start(vf)) continue;
			int err = 0;
			stb_vorbis_close(vf);
			vf = stb_vorbis_open_memory(file, (int)file_len, &err, NULL);
			if (!vf) { plat_log("music: couldn't open %s again (stb_vorbis %d): stopped", vf_path, err); break; }
			plat_log("music: back to the start by opening it again");
			continue;
		}
		empty = 0;
		for (int i = 0; i < n; i++) {
			uint32_t at = (wpos + i) & (RING - 1);
			ring[at * 2] = chunk[i * 2];
			ring[at * 2 + 1] = chunk[i * 2 + 1];
		}
		__atomic_store_n(&wpos, wpos + n, __ATOMIC_RELEASE);
	}
	live = 0;
}

int music_start(const char *path)
{
	int err = 0;
	if (running) return 1;
	if (live) return 0;		// (an old decoder's still stuck: it has the file)
	free(file);
	file = plat_read_file(path, &file_len);
	if (!file || file_len < 64 || file_len > 64u * 1024 * 1024) {
		plat_log("music: %s couldn't be read", path);
		free(file);
		file = NULL;
		return 0;
	}
	vf = stb_vorbis_open_memory(file, (int)file_len, &err, NULL);
	if (!vf) { plat_log("music: %s couldn't be opened (stb_vorbis %d)", path, err); free(file); file = NULL; return 0; }
	stb_vorbis_info info = stb_vorbis_get_info(vf);
	if (info.channels != 2 || info.sample_rate != MUSIC_RATE) {
		plat_log("music: %s is %u Hz, %d channels; wanted %d Hz stereo", path, info.sample_rate, info.channels, MUSIC_RATE);
		stb_vorbis_close(vf);
		vf = NULL;
		free(file);
		file = NULL;
		return 0;
	}
	// (everything that reads the file is done before the decoder starts: from then on it's the
	// decoder's, and a seek from here would move it under the decoder's feet)
	float secs = stb_vorbis_stream_length_in_seconds(vf);
	if (!stb_vorbis_seek_start(vf)) {
		plat_log("music: %s won't go back to its start", path);
		stb_vorbis_close(vf);
		vf = NULL;
		free(file);
		file = NULL;
		return 0;
	}
	snprintf(vf_path, sizeof(vf_path), "%s", path);
	plat_log("music: %s, %.0f s, %u KB in memory, %s", path, secs, (unsigned)(file_len / 1024), want_on ? "on" : "off");
	wpos = rpos = 0;
	running = live = 1;
	// (below the screen's thread: a ring of a second and more covers any wait; stb_vorbis's
	// scratch is on its stack)
	if (!plat_thread_start(Decoder, NULL, 1200, 256, "music")) {
		running = live = 0;
		stb_vorbis_close(vf);
		vf = NULL;
		free(file);
		file = NULL;
		return 0;
	}
	return 1;
}

void music_stop(void)
{
	if (!running) return;
	running = 0;
	for (int i = 0; i < 100 && live; i++) plat_sleep_us(10000);
	if (!live) {		// (a decoder still going keeps them)
		if (vf) stb_vorbis_close(vf);
		vf = NULL;
		free(file);
		file = NULL;
	}
}

void music_set_on(int on) { want_on = on; }
void music_set_level(float l) { level = l < 0 ? 0 : l > 1 ? 1 : l; }
int music_on(void) { return want_on; }

void music_mix(float *out, int frames)
{
	if (!running && gain <= 0) return;
	uint32_t have = __atomic_load_n(&wpos, __ATOMIC_ACQUIRE) - rpos;
	uint32_t r = rpos;
	for (int i = 0; i < frames; i++) {
		float target = want_on ? 1.0f : 0.0f;
		if (gain < target) gain += 1.0f / FADE;
		else if (gain > target) gain -= 1.0f / FADE;
		if (gain < 0) gain = 0;
		if (gain > 1) gain = 1;
		if (gain <= 0 || !have) continue;		// (off: the ring waits where it was, for coming back)
		uint32_t at = r & (RING - 1);
		out[i * 2] += ring[at * 2] * gain * LEVEL;
		out[i * 2 + 1] += ring[at * 2 + 1] * gain * LEVEL;
		r++;
		have--;
	}
	__atomic_store_n(&rpos, r, __ATOMIC_RELEASE);
}
