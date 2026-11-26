/*
ring.c: the overview's ring chart, made as a picture (ring.h).

gfx has no arcs, so the ring is drawn here into an image_t, pixel by pixel,
at the screen's own resolution (1.5x at 1080p), and drawn like any other
picture: the same on the RSX and the CPU renderer. Each pixel's angle and
distance from the middle are worked out once for the size; changing the
selection only colours the pixels again.
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "plat.h"
#include "gfx.h"
#include "ring.h"

#define GAP_PX	1.6f		// between segments, in the picture's pixels
#define POP		0.045f		// the selected segment stands out this much past the ring (of the diameter)
#define HOLE	0.64f		// the hole, of the ring's outer diameter

static int map_px;			// the size the maps are for
static float *ang, *rad;	// 0..1 clockwise from the top; distance from the middle (pixels)

static int Maps(int px)
{
	if (px == map_px && ang) return 1;
	free(ang);
	free(rad);
	ang = malloc((size_t)px * px * sizeof(float));
	rad = malloc((size_t)px * px * sizeof(float));
	if (!ang || !rad) { free(ang); free(rad); ang = rad = NULL; map_px = 0; return 0; }
	float c = px / 2.0f;
	for (int y = 0; y < px; y++)
		for (int x = 0; x < px; x++) {
			float dx = x + 0.5f - c, dy = y + 0.5f - c;
			float a = atan2f(dx, -dy) / (2 * (float)M_PI);		// 0 at the top, clockwise
			if (a < 0) a += 1;
			ang[y * px + x] = a;
			rad[y * px + x] = sqrtf(dx * dx + dy * dy);
		}
	map_px = px;
	return 1;
}

static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// per pixel, for the segments last laid out: which it's in (SEG_TRACK, SEG_NONE), and how
// much of it is covered, normally and when its segment's the one standing out
#define SEG_TRACK	254
#define SEG_NONE	255
static uint8_t *seg_of, *cov_n, *cov_p;
static int lay_px;
static ring_seg_t lay_segs[RING_MAX];
static int lay_n;
static uint64_t lay_total;

static int Lay_Out(int px, const ring_seg_t *segs, int n, uint64_t total)
{
	if (px == lay_px && n == lay_n && total == lay_total && !memcmp(segs, lay_segs, n * sizeof(*segs))) return 1;
	if (px != lay_px || !seg_of) {
		free(seg_of); free(cov_n); free(cov_p);
		size_t np = (size_t)px * px;
		seg_of = malloc(np);
		cov_n = malloc(np);
		cov_p = malloc(np);
		if (!seg_of || !cov_n || !cov_p) {
			free(seg_of); free(cov_n); free(cov_p);
			seg_of = cov_n = cov_p = NULL;
			lay_px = 0;
			return 0;
		}
	}
	lay_px = px;
	lay_n = n;
	lay_total = total;
	memcpy(lay_segs, segs, n * sizeof(*segs));

	// where each segment starts and ends (0..1), and what's left of the total (the track)
	float a0[RING_MAX + 1], a1[RING_MAX + 1];
	uint64_t sum = 0;
	for (int i = 0; i < n; i++) sum += segs[i].v;
	if (total < sum) total = sum;
	double at = 0;
	for (int i = 0; i < n; i++) {
		a0[i] = (float)at;
		at += total ? (double)segs[i].v / total : 0;
		a1[i] = (float)at;
	}
	float pop = px * POP, r_out = px / 2.0f - pop - 1, r_in = r_out * HOLE;
	float gap = GAP_PX * gfx_scale();
	for (int k = 0; k < px * px; k++) {
		float r = rad[k], a = ang[k];
		seg_of[k] = SEG_NONE;
		cov_n[k] = cov_p[k] = 0;
		if (r < r_in - 1 || r > r_out + pop + 1) continue;
		int s = -1;
		for (int i = 0; i < n; i++)
			if (a >= a0[i] && a < a1[i]) { s = i; break; }
		float in = Clamp01(r - r_in + 0.5f), edge;
		if (s < 0) {
			// (the track ends with a gap too, where the segments start and stop)
			edge = 1;
			if (n && sum) {
				float e = fminf(fabsf(a - (float)at), fminf(a, 1 - a));
				edge = Clamp01(e * 2 * (float)M_PI * r - gap * 0.5f + 0.5f);
			}
			seg_of[k] = SEG_TRACK;
			cov_n[k] = cov_p[k] = (uint8_t)(255 * in * edge * Clamp01(r_out - r + 0.5f) + 0.5f);
		} else {
			float e = fminf(a - a0[s], a1[s] - a) * 2 * (float)M_PI * r;	// to its nearer edge, in pixels
			edge = Clamp01(e - gap * 0.5f + 0.5f);
			seg_of[k] = (uint8_t)s;
			cov_n[k] = (uint8_t)(255 * in * edge * Clamp01(r_out - r + 0.5f) + 0.5f);
			cov_p[k] = (uint8_t)(255 * in * edge * Clamp01(r_out + pop - r + 0.5f) + 0.5f);
		}
	}
	return 1;
}

image_t *ring_build(image_t *img, int d, const ring_seg_t *segs, int n, uint64_t total, uint32_t track, int sel)
{
	int px = (int)(d * gfx_scale() + 0.5f);
	if (px < 16 || n > RING_MAX) return img;
	if (img && img->w != px) { image_free(img); img = NULL; }
	if (!img && !(img = image_new(px, px))) return NULL;
	if (!Maps(px) || !Lay_Out(px, segs, n, total)) return img;
	// the colours: a selection change only does this (integers, once a pixel)
	uint32_t col[256];
	uint8_t dim[256];
	for (int i = 0; i < 256; i++) { col[i] = 0; dim[i] = 0; }
	for (int i = 0; i < n; i++) {
		col[i] = segs[i].color;
		dim[i] = sel >= 0 && i != sel;		// the others, dimmed
	}
	col[SEG_TRACK] = track;
	uint32_t *p = img->px;
	for (int k = 0; k < px * px; k++) {
		int s = seg_of[k];
		uint32_t cov = s == sel ? cov_p[k] : cov_n[k];
		if (!cov) { p[k] = 0; continue; }
		uint32_t c = col[s], al = ((c >> 24) * cov + 127) / 255;
		if (dim[s]) al >>= 1;
		p[k] = al ? (al << 24) | (c & 0xffffff) : 0;
	}
	img->opaque = 0;
	image_touch(img);
	return img;
}
