/*
icons.c: ICON0.PNGs for the lists (icons.h).

A small table of slots, each a path and its picture (or the job reading it).
Reading and decoding run on LANE_ART, newest first, so the rows on screen come
first however far the list was scrolled. A slot that hasn't been drawn for a
few frames can be taken by a new icon; one being read is never taken (its job
is let go of, and frees what it made). A path that couldn't be read is
remembered, so it's not tried again every frame.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "gfx.h"
#include "jobs.h"
#include "iso.h"
#include "icons.h"

#define SLOTS		64
#define MAX_PNG		(1 << 20)		// an ICON0 bigger than this isn't read

typedef struct {
	char path[256];
	int w, h;
	image_t *img;
	job_t *job;
	int failed;
	uint64_t used;			// the frame it was last asked for
} slot_t;

static slot_t slots[SLOTS];
static uint64_t frame;

static void Read_Run(job_t *j)
{
	size_t len;
	unsigned char *data;
	char *inside = strstr(j->sval, "::");		// "<image>::PS3_GAME/ICON0.PNG": read out of a disc image
	if (inside) {
		char img[512], dir[32];
		uint32_t l = 0;
		const char *name = strchr(inside + 2, '/');
		snprintf(img, sizeof(img), "%.*s", (int)(inside - j->sval), j->sval);
		snprintf(dir, sizeof(dir), "%.*s", name ? (int)(name - inside - 2) : 0, inside + 2);
		data = iso_read_file(img, name ? dir : NULL, name ? name + 1 : inside + 2, &l);
		len = l;
	} else {
		long long n = plat_file_size(j->sval);
		if (n <= 0 || n > MAX_PNG) return;
		data = plat_read_file(j->sval, &len);
	}
	if (!data) return;
	j->result = image_decode_screen(data, (int)len, j->ival, (int)j->lval);
	free(data);
	j->ok = j->result != NULL;
}

static void Read_Cleanup(job_t *j)
{
	if (j->result) image_free(j->result);
	j->result = NULL;
}

image_t *icon_get(const char *path, int w, int h)
{
	slot_t *free_slot = NULL;
	for (int i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[i];
		if (s->path[0] && s->w == w && s->h == h && !strcmp(s->path, path)) {
			s->used = frame;
			return s->img;
		}
	}
	// a slot: an empty one, or the one unused longest (not one still being read, nor one drawn
	// in the last few frames: its picture may still be on its way to the screen)
	for (int i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[i];
		if (!s->path[0]) { free_slot = s; break; }
		if (s->job || s->used + 3 > frame) continue;
		if (!free_slot || s->used < free_slot->used) free_slot = s;
	}
	if (!free_slot || strlen(path) >= sizeof(free_slot->path)) return NULL;
	slot_t *s = free_slot;
	if (s->img) image_free(s->img);
	memset(s, 0, sizeof(*s));
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->w = w;
	s->h = h;
	s->used = frame;
	job_t *j = job_new(Read_Run, Read_Cleanup);
	if (!j) { s->failed = 1; return NULL; }
	snprintf(j->sval, sizeof(j->sval), "%s", path);
	j->ival = w;
	j->lval = h;
	s->job = j;
	job_post(j, LANE_ART);
	return NULL;
}

int icons_poll(void)
{
	int arrived = 0;
	frame++;
	for (int i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[i];
		if (!s->job || !job_done(s->job)) continue;
		s->img = s->job->result;		// (taken: the job's cleanup won't free it)
		s->job->result = NULL;
		s->failed = !s->img;
		job_release(s->job);
		s->job = NULL;
		arrived = 1;
	}
	return arrived;
}

void icons_shutdown(void)
{
	jobs_cancel_lane(LANE_ART);
	for (int i = 0; i < SLOTS; i++) {
		slot_t *s = &slots[i];
		if (s->job) job_release(s->job);		// (it frees its picture itself, whenever it's done)
		if (s->img) image_free(s->img);
		memset(s, 0, sizeof(*s));
	}
}
