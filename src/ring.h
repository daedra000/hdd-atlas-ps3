#ifndef RING_H
#define RING_H

#include <stdint.h>
#include "gfx.h"

#define RING_MAX	16

typedef struct {
	uint64_t v;
	uint32_t color;		// ARGB
} ring_seg_t;

// the ring, d canvas units across (made at the screen's resolution), into img (made, or
// remade when the size changed; returned): the segments clockwise from the top, as parts of
// total (what's left of it is the track colour). sel stands out and the others dim (-1: none).
image_t *ring_build(image_t *img, int d, const ring_seg_t *segs, int n, uint64_t total, uint32_t track, int sel);

#endif
