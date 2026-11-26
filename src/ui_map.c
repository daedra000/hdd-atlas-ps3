/*
ui_map.c: the drive as a map (Map of the drive, in the menu): a folder's
folders as boxes sized by the space they take ("squarified": as near square
as they can be), coloured by their category; the files directly in it one
box, the smallest folders together another. The D-pad picks a box, X goes
into it, Circle comes back up (and from the top, back to the overview).
It's all from the scan's folder sizes: nothing's read from the drive.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "lang.h"
#include "fsx.h"
#include "scan.h"
#include "ui_kit.h"
#include "ui_int.h"

#define MAX_BOXES	40
#define MAP_Y		140
#define MAP_H		(HINT_Y - 60 - MAP_Y)

enum { BOX_DIR, BOX_FILES, BOX_REST };

typedef struct {
	int kind, dir, cat;
	uint64_t bytes;
	int x, y, w, h;
} box_t;

static box_t box[MAX_BOXES + 2];
static int nbox, msel;
static int map_dir;			// the folder shown (in the scan's folder sizes; 0 the top)
static const scan_t *map_of;
static int64_t map_when;

static int Bigger(const void *a, const void *b)
{
	uint64_t x = ((const box_t *)a)->bytes, y = ((const box_t *)b)->bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

// a folder's category: its top folder's
static int Cat_Of(const scan_t *s, int d)
{
	while (d > 0 && s->dir[d].parent) d = (int)s->dir[d].parent;
	return d > 0 ? scan_cat_of(s->names + s->dir[d].name, 1) : CAT_SYSTEM;
}

// the worst aspect ratio of a row of boxes laid along a side of length side
static double Worst(const box_t *b, int from, int to, double scale, double side)
{
	double sum = 0, mx = 0, mn = 1e300;
	for (int i = from; i < to; i++) {
		double a = b[i].bytes * scale;
		sum += a;
		if (a > mx) mx = a;
		if (a < mn) mn = a;
	}
	if (sum <= 0 || mn <= 0) return 1e300;
	double s2 = side * side, sum2 = sum * sum;
	double r1 = s2 * mx / sum2, r2 = sum2 / (s2 * mn);
	return r1 > r2 ? r1 : r2;
}

// boxes [from, n) laid out in the rectangle, rows added while they make the boxes squarer
static void Squarify(int from, int n, double x, double y, double w, double h, double scale)
{
	while (from < n && w > 1 && h > 1) {
		double side = w < h ? w : h;
		int to = from + 1;
		while (to < n && Worst(box, from, to + 1, scale, side) <= Worst(box, from, to, scale, side)) to++;
		double sum = 0;
		for (int i = from; i < to; i++) sum += box[i].bytes * scale;
		double thick = sum / side, at = 0;
		for (int i = from; i < to; i++) {
			double len = box[i].bytes * scale / thick;
			if (w >= h) { box[i].x = (int)x; box[i].y = (int)(y + at); box[i].w = (int)(x + thick) - (int)x; box[i].h = (int)(y + at + len) - (int)(y + at); }
			else { box[i].x = (int)(x + at); box[i].y = (int)y; box[i].w = (int)(x + at + len) - (int)(x + at); box[i].h = (int)(y + thick) - (int)y; }
			at += len;
		}
		if (w >= h) { x += thick; w -= thick; } else { y += thick; h -= thick; }
		from = to;
	}
}

static void Build(void)
{
	const scan_t *s = DRV->shown;
	nbox = 0;
	if (!s || !s->first_child || map_dir < 0 || (uint32_t)map_dir >= s->num_dirs) return;
	static box_t all[512];		// (20 KB: not on the screen thread's stack)
	int n = 0;
	uint64_t in_folders = 0, rest = 0;
	for (uint32_t c = s->first_child[map_dir]; c != UINT32_MAX; c = s->next_sibling[c]) {
		in_folders += s->dir[c].bytes;
		if (!s->dir[c].bytes) continue;
		if (n < 512) { all[n].kind = BOX_DIR; all[n].dir = (int)c; all[n].bytes = s->dir[c].bytes; all[n].cat = Cat_Of(s, (int)c); n++; }
		else rest += s->dir[c].bytes;
	}
	qsort(all, n, sizeof(box_t), Bigger);
	for (int i = 0; i < n; i++) {
		if (nbox < MAX_BOXES) box[nbox++] = all[i];
		else rest += all[i].bytes;
	}
	if (rest) box[nbox++] = (box_t){BOX_REST, -1, map_dir ? Cat_Of(s, map_dir) : CAT_SYSTEM, rest, 0, 0, 0, 0};
	uint64_t here = s->dir[map_dir].bytes > in_folders ? s->dir[map_dir].bytes - in_folders : 0;
	if (here) box[nbox++] = (box_t){BOX_FILES, -1, map_dir ? Cat_Of(s, map_dir) : CAT_SYSTEM, here, 0, 0, 0, 0};
	qsort(box, nbox, sizeof(box_t), Bigger);
	uint64_t total = 0;
	for (int i = 0; i < nbox; i++) total += box[i].bytes;
	if (!total) { nbox = 0; return; }
	double w = LIST_W + 24, h = MAP_H;
	Squarify(0, nbox, LIST_X - 12, MAP_Y, w, h, w * h / (double)total);
	if (msel >= nbox) msel = 0;
	map_of = s;
	map_when = s->when;
}

void map_open(void)
{
	map_dir = 0;
	msel = 0;
	Build();
	ui_dirty();
}

// the box nearest the picked one in a direction (dx, dy); the picked one if none
static int Neighbour(int dx, int dy)
{
	const box_t *a = &box[msel];
	int ax = a->x + a->w / 2, ay = a->y + a->h / 2, best = msel;
	long long best_d = -1;
	for (int i = 0; i < nbox; i++) {
		if (i == msel) continue;
		int bx = box[i].x + box[i].w / 2, by = box[i].y + box[i].h / 2;
		int along = (bx - ax) * dx + (by - ay) * dy, across = dx ? by - ay : bx - ax;
		if (along <= 0) continue;
		long long d = (long long)along * along + 4LL * across * across;		// (straight on beats off to the side)
		if (best_d < 0 || d < best_d) { best_d = d; best = i; }
	}
	return best;
}

int map_update(uint32_t p)
{
	const scan_t *s = DRV->shown;
	if (!s) return 0;
	if (map_of != s || map_when != s->when) { map_dir = 0; msel = 0; Build(); }
	int old = msel;
	if (nbox) {
		if (p & BTN_LEFT) msel = Neighbour(-1, 0);
		if (p & BTN_RIGHT) msel = Neighbour(1, 0);
		if (p & BTN_UP) msel = Neighbour(0, -1);
		if (p & BTN_DOWN) msel = Neighbour(0, 1);
	}
	if (msel != old) { ui_sound(SND_MOVE); ui_dirty(); }
	if ((p & BTN_CROSS) && nbox && box[msel].kind == BOX_DIR) {
		ui_sound(SND_OK);
		map_dir = box[msel].dir;
		msel = 0;
		Build();
		ui_dirty();
	}
	if (p & BTN_CIRCLE) {
		ui_sound(SND_BACK);
		if (!map_dir) return 0;
		int from = map_dir;
		map_dir = (int)s->dir[map_dir].parent;
		Build();
		for (int i = 0; i < nbox; i++) if (box[i].kind == BOX_DIR && box[i].dir == from) msel = i;		// (where it came up from)
		ui_dirty();
	}
	return 1;
}

static void Box_Name(const scan_t *s, const box_t *b, char *out, int size)
{
	if (b->kind == BOX_DIR) snprintf(out, size, "%s", s->names + s->dir[b->dir].name);
	else if (b->kind == BOX_FILES) snprintf(out, size, "%s", tr("Files here"));
	else snprintf(out, size, "%s", tr("Smaller folders"));
}

void map_render(void)
{
	const scan_t *s = DRV->shown;
	char path[FSX_PATH], name[FSX_NAME], size[32], line[400];
	kit_backdrop();
	kit_top(tr("Map of the drive"), 0);
	if (!s) return;
	// (a scan that finished while the screen was dimmed: no update's made the map again yet)
	if (map_of != s || map_when != s->when || map_dir < 0 || (uint32_t)map_dir >= s->num_dirs) { map_dir = 0; msel = 0; Build(); }
	// where: the folder's path, and its size
	path[0] = 0;
	{
		int chain[64], n = 0;
		for (int d = map_dir; d > 0 && n < 64; d = (int)s->dir[d].parent) chain[n++] = d;
		snprintf(path, sizeof(path), "%s", DRV->root);
		for (int i = n - 1; i >= 0; i--) {
			size_t l = strlen(path);
			snprintf(path + l, sizeof(path) - l, "/%s", s->names + s->dir[chain[i]].name);
		}
	}
	kit_fmt_size((long long)s->dir[map_dir].bytes, size, sizeof(size));
	font_draw_right(FONT_MEDIUM, 18, SCREEN_W - LIST_X, 44, size, C_TEXT);
	font_draw(FONT_REGULAR, 18, LIST_X, 104, path, C_TEXT2, LIST_W);
	if (!nbox) font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, MAP_Y + MAP_H / 2, tr("Nothing here."), C_DIM, LIST_W);
	for (int i = 0; i < nbox; i++) {
		const box_t *b = &box[i];
		if (b->w < 3 || b->h < 3) continue;
		uint32_t c = cat_colors[b->cat];
		int shade = 0x60 + (i % 3) * 0x28;		// (neighbours told apart)
		if (b->kind != BOX_DIR) shade = 0x38;
		if (i == msel) gfx_round_rect(b->x, b->y, b->w - 2, b->h - 2, 6, 0xffffffff);
		gfx_round_rect(b->x + (i == msel ? 3 : 0), b->y + (i == msel ? 3 : 0), b->w - 2 - (i == msel ? 6 : 0), b->h - 2 - (i == msel ? 6 : 0), 5,
			with_alpha(c, i == msel ? 0xff : shade));
		if (b->w > 70 && b->h > 26) {
			Box_Name(s, b, name, sizeof(name));
			font_draw(FONT_MEDIUM, 16, b->x + 8, b->y + 6, name, C_TEXT, b->w - 16);
			if (b->h > 48) {
				kit_fmt_size((long long)b->bytes, size, sizeof(size));
				font_draw(FONT_REGULAR, 14, b->x + 8, b->y + 26, size, 0xd0ffffff, b->w - 16);
			}
		}
	}
	// the box picked, in words
	if (nbox) {
		const box_t *b = &box[msel];
		char pct[16];
		Box_Name(s, b, name, sizeof(name));
		kit_fmt_size((long long)b->bytes, size, sizeof(size));
		ui_fmt_pct(b->bytes, s->dir[map_dir].bytes, pct, sizeof(pct));
		snprintf(line, sizeof(line), "%s  \xc2\xb7  %s  \xc2\xb7  %s", name, size, pct);
		if (b->kind == BOX_DIR) {
			char files[24];
			kit_fmt_count(s->dir[b->dir].files, files, sizeof(files));
			size_t l = strlen(line);
			snprintf(line + l, sizeof(line) - l, tr(s->dir[b->dir].files == 1 ? "  \xc2\xb7  %s file" : "  \xc2\xb7  %s files"), files);
		}
		font_draw(FONT_MEDIUM, 19, LIST_X, MAP_Y + MAP_H + 14, line, C_TEXT, LIST_W);
	}
	const kit_hint_t hints[] = {
		{G_CROSS, -1, nbox && box[msel].kind == BOX_DIR ? tr("Go in") : NULL, 0},
		{G_DPAD_LR, -1, tr("Pick a box"), 2},
		{G_CIRCLE, -1, map_dir ? tr("Up") : tr("Back"), 0},
	};
	kit_hints(hints, 3, LIST_X, ui_hints_max());
	ui_draw_strip();
}
