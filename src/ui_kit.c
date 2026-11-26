/*
ui_kit.c: PS3 World's drawing helpers (from its src/ui.c, unchanged but for
their names: Glyph is kit_glyph, Hint kit_hint and so on; and kit_top's
logo, HDD Atlas's own, is LOGO_W wide).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "lang.h"
#include "ui_kit.h"

static const char *glyph_files[NUM_GLYPHS] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "dpad_ud", "select", "start", "r3", "dpad_lr"};
static image_t *glyphs[NUM_GLYPHS];
static image_t *backdrop, *logo, *card_bg;

image_t *kit_load_image(const char *name, int w, int h)
{
	char path[512];
	size_t len;
	snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), name);
	unsigned char *data = plat_read_file(path, &len);
	if (!data) { plat_log("ui: %s missing", path); return NULL; }
	image_t *img = image_decode_screen(data, (int)len, w, h);
	free(data);
	return img;
}

void kit_init(void)
{
	char name[64];
	// (the backdrop is 16:9: on the 4:3 canvas, its middle; image_decode covers the box)
	backdrop = kit_load_image("backdrop.png", SCREEN_W, SCREEN_H);
	logo = kit_load_image("logo.png", LOGO_W, 31);
	card_bg = kit_load_image("cards/bg.png", 256, 256);
	for (int i = 0; i < NUM_GLYPHS; i++) {
		snprintf(name, sizeof(name), "glyphs/%s.png", glyph_files[i]);
		glyphs[i] = kit_load_image(name, GLYPH + 2, GLYPH + 2);		// (64x64: a 2-pixel clear margin round the art, tools/pad-glyphs.py)
	}
}

void kit_backdrop(void)
{
	if (backdrop) gfx_image(backdrop, 0, 0, SCREEN_W, SCREEN_H, 0, 255);
	else gfx_clear(C_BG);
}

uint32_t kit_accent = 0xff2b8fff, kit_dim = 0xff9aa4b8, kit_text2 = 0xffa2acc0;

void kit_accent_set(int which)
{
	static const uint32_t accents[NUM_ACCENTS] = {0xff2b8fff, 0xff16b3a6, 0xff8a5cff, 0xffe8508f, 0xffef8a2f, 0xff2fb57a, 0xffffd23f};
	if (which < 0 || which >= NUM_ACCENTS) which = ACCENT_BLUE;
	kit_accent = accents[which];
	kit_dim = which == ACCENT_CONTRAST ? 0xffd2d8e4 : 0xff9aa4b8;
	kit_text2 = which == ACCENT_CONTRAST ? 0xffe8ecf4 : 0xffa2acc0;
}

const char *kit_accent_name(int which)
{
	static const char *const names[NUM_ACCENTS] = {N_("Blue"), N_("Teal"), N_("Purple"), N_("Pink"), N_("Orange"), N_("Green"), N_("High contrast")};
	return names[which >= 0 && which < NUM_ACCENTS ? which : 0];
}

void kit_backdrop_alpha(int alpha)
{
	if (backdrop) gfx_image(backdrop, 0, 0, SCREEN_W, SCREEN_H, 0, alpha);
	else gfx_rect(0, 0, SCREEN_W, SCREEN_H, with_alpha(C_BG, alpha));
}

// word-wraps s into lines of at most maxw; returns the number of lines
// Chinese, Japanese and Korean go to a new line between any two characters, but not before
// closing punctuation (、。，）...): 0 not one of theirs, 1 one, 2 closing punctuation
static int Cjk(const char *s)
{
	const unsigned char *u = (const unsigned char *)s;
	uint32_t cp;
	if (u[0] < 0xe0 || u[0] >= 0xf0 || !u[1] || !u[2]) return 0;		// (all of them are 3 bytes in UTF-8; a cut-off one isn't)
	cp = ((uint32_t)(u[0] & 0x0f) << 12) | ((uint32_t)(u[1] & 0x3f) << 6) | (u[2] & 0x3f);
	if (cp < 0x2e80) return 0;
	static const uint32_t closing[] = {0x3001, 0x3002, 0xff0c, 0xff0e, 0xff1a, 0xff1b, 0xff09, 0x300d, 0x300f, 0xff01, 0xff1f, 0x30fc, 0x3009, 0x300b, 0x3011};
	for (size_t k = 0; k < sizeof(closing) / sizeof(closing[0]); k++) if (cp == closing[k]) return 2;
	return 1;
}

int kit_wrap(int font, int px, const char *s, int maxw, char (*lines)[256], int maxlines)
{
	int n = 0;
	while (*s && n < maxlines) {
		char line[256];
		int len = 0, last_space = -1, cl = 1;
		while (s[len] && s[len] != '\n') {
			// a whole UTF-8 character at a time: a line never ends inside one
			for (cl = 1; (s[len + cl] & 0xc0) == 0x80; cl++);
			if (len + cl > 255) break;
			memcpy(line, s, len + cl);
			line[len + cl] = 0;
			if (font_width(font, px, line) > maxw) break;
			if (s[len] == ' ' || (len > 0 && Cjk(s + len) == 1)) last_space = len;	// (a place the line can end)
			len += cl;
		}
		int cut = len;
		if (s[len] && s[len] != '\n' && Cjk(s + len) == 1) cut = len;		// (the next character can start a line)
		else if (s[len] && s[len] != '\n' && last_space > 0) cut = last_space;
		if (cut == 0) cut = len ? len : s[0] ? cl : 1;
		snprintf(lines[n++], 256, "%.*s", cut, s);
		s += cut;
		while (*s == ' ') s++;
		if (*s == '\n') {		// the line's own end; each newline after it, a blank line ("\n\n": a paragraph)
			s++;
			while (*s == '\n' && n < maxlines) { lines[n++][0] = 0; s++; }
			while (*s == ' ') s++;
		}
	}
	if (*s && n) {		// more than fits: the last line ends in ...
		size_t l = strlen(lines[n - 1]);
		if (l > 3) {
			l -= 3;
			while (l && (lines[n - 1][l] & 0xc0) == 0x80) l--;		// (not inside a character)
			strcpy(lines[n - 1] + l, "...");
		}
	}
	return n;
}

int kit_text_y(int font, int px, int top, int h)
{
	float y = top + h / 2.0f - font_cap_middle(font, px);
	return (int)(y + (y < 0 ? -0.5f : 0.5f));
}

int kit_draw_fit(int font, int px, int minpx, int x, int y, const char *s, uint32_t c, int maxw)
{
	int p = px;
	while (p > minpx && font_width(font, p, s) > maxw) p--;
	int dy = kit_text_y(font, p, 0, 40) - kit_text_y(font, px, 0, 40);	// (the capitals' middle stays put)
	return font_draw(font, p, x, y + dy, s, c, maxw);
}

int kit_draw_fit_center(int font, int px, int minpx, int cx, int y, const char *s, uint32_t c, int maxw)
{
	int p = px;
	while (p > minpx && font_width(font, p, s) > maxw) p--;
	int dy = kit_text_y(font, p, 0, 40) - kit_text_y(font, px, 0, 40);
	return font_draw_center(font, p, cx, y + dy, s, c, maxw);
}

const char *kit_btn(const char *x_text, const char *o_text) { return tr(plat_circle_confirms() ? o_text : x_text); }

void kit_glyph(int g, int x, int y)
{
	// on a console that confirms with O, the button that does "X" things is O (input.c swaps them)
	if (plat_circle_confirms() && (g == G_CROSS || g == G_CIRCLE)) g = g == G_CROSS ? G_CIRCLE : G_CROSS;
	if (glyphs[g]) gfx_image(glyphs[g], x - 1, y - 1, GLYPH + 2, GLYPH + 2, 0, 255);		// (the art itself fills GLYPH)
	else gfx_circle(x + GLYPH / 2, y + GLYPH / 2, GLYPH / 2 - 2, 0xff55585e);
}

int kit_hint(int x, int g, const char *label)
{
	int y = HINT_Y;
	kit_glyph(g, x, y);
	return x + GLYPH + 8 + font_draw(FONT_MEDIUM, 19, x + GLYPH + 8, y + 3, label, C_DIM, 0) + 28;
}

int kit_hint2(int x, int g1, int g2, const char *label)
{
	kit_glyph(g1, x, HINT_Y);
	return kit_hint(x + GLYPH + 4, g2, label);
}

static int Hint_W(const kit_hint_t *h)
{
	return (h->g2 >= 0 ? GLYPH + 4 : 0) + GLYPH + 8 + font_width(FONT_MEDIUM, 19, h->label) + 28;
}

int kit_hints(const kit_hint_t *h, int n, int x, int maxx)
{
	int keep[16], total = 0;
	if (n > 16) n = 16;
	for (int i = 0; i < n; i++) { keep[i] = h[i].label != NULL; if (keep[i]) total += Hint_W(&h[i]); }
	while (x + total - 28 > maxx) {		// (the last hint's trailing gap doesn't count)
		int drop = -1;
		for (int i = 0; i < n; i++)
			if (keep[i] && (drop < 0 || h[i].prio >= h[drop].prio)) drop = i;
		if (drop < 0 || h[drop].prio <= 0) break;		// (prio 0: always shown)
		keep[drop] = 0;
		total -= Hint_W(&h[drop]);
	}
	for (int i = 0; i < n; i++)
		if (keep[i]) x = h[i].g2 >= 0 ? kit_hint2(x, h[i].g, h[i].g2, h[i].label) : kit_hint(x, h[i].g, h[i].label);
	return x;
}

// L1, a row of pills (the one in use filled), R1: what L1/R1 move through. When they don't all
// fit before maxx, the ones around the one in use. Returns the x after R1.
int kit_tabs(int x, int y, int maxx, const char *const *labels, int n, int sel)
{
	int w[32];
	if (n > 32) n = 32;
	if (n <= 0) return x;
	if (sel < 0) sel = 0;
	if (sel >= n) sel = n - 1;
	for (int i = 0; i < n; i++) w[i] = font_width(FONT_MEDIUM, 20, labels[i]) + 30;
	int room = maxx - x - 2 * (GLYPH + 12), a = sel, b = sel, used = w[sel];
	for (int grew = 1; grew; ) {
		grew = 0;
		if (b + 1 < n && used + 6 + w[b + 1] <= room) { used += 6 + w[++b]; grew = 1; }
		if (a > 0 && used + 6 + w[a - 1] <= room) { used += 6 + w[--a]; grew = 1; }
	}
	kit_glyph(G_L1, x, y + (TAB_H - GLYPH) / 2);
	x += GLYPH + 12;
	for (int i = a; i <= b; i++) {
		if (i == sel) gfx_round_rect(x, y, w[i], TAB_H, TAB_H / 2, C_ROW_ON);
		font_draw(FONT_MEDIUM, 20, x + 15, kit_text_y(FONT_MEDIUM, 20, y, TAB_H), labels[i], i == sel ? C_TEXT : C_DIM, w[i] - 30);
		x += w[i] + 6;
	}
	kit_glyph(G_R1, x + 6, y + (TAB_H - GLYPH) / 2);
	return x + 6 + GLYPH;
}

// the card's frame: a blue ring when it's selected, the site's 1px border when not, and its gradient
void kit_card_frame(int x, int y, int w, int h, int on)
{
	if (on) gfx_round_rect(x - 3, y - 3, w + 6, h + 6, 21, C_ACCENT);
	else gfx_round_rect(x - 1, y - 1, w + 2, h + 2, 19, C_BORDER);
	if (card_bg) gfx_image_flip(card_bg, x, y, w, h, 18, 255);		// (its glow on the side the art's on)
	else gfx_round_rect(x, y, w, h, 18, 0xff0e131f);
}

int kit_chip(int x, int y, const char *text)
{
	int w = font_width(FONT_MEDIUM, 18, text) + 32;
	gfx_round_rect(x, y, w, CHIP_H, CHIP_H / 2, 0x1fffffff);
	gfx_round_rect(x + 1, y + 1, w - 2, CHIP_H - 2, CHIP_H / 2 - 1, 0xbf05070d);
	font_draw(FONT_MEDIUM, 18, x + 16, kit_text_y(FONT_MEDIUM, 18, y, CHIP_H), text, C_TEXT2, 0);
	return w;
}

void kit_badge(int *rx, int y, const char *text, uint32_t c)
{
	int w = font_width(FONT_MEDIUM, 15, text) + 16;
	*rx -= w;
	gfx_round_rect(*rx, y, w, 24, 12, with_alpha(c, 50));
	font_draw(FONT_MEDIUM, 15, *rx + 8, kit_text_y(FONT_MEDIUM, 15, y, 24), text, c, 0);
	*rx -= 8;
}

// the logo (HDD Atlas's: its icon and name, tools/make-art.py), and where we are (as the site's
// crumbs; cut short with "..." past maxw, 0: no limit)
void kit_top(const char *crumb, int maxw)
{
	if (logo) gfx_image(logo, LIST_X, 40, LOGO_W, 31, 0, 255);
	if (crumb) {
		int tw = font_width(FONT_BOLD, 18, crumb);
		if (maxw > 0 && tw > maxw) tw = maxw;
		int w = tw + 28;
		gfx_round_rect(LIST_X + LOGO_W + 16, 38, w, 34, 17, 0x404ad9ff);
		gfx_round_rect(LIST_X + LOGO_W + 17, 39, w - 2, 32, 16, 0xff0a1622);
		font_draw(FONT_BOLD, 18, LIST_X + LOGO_W + 30, kit_text_y(FONT_BOLD, 18, 38, 34), crumb, C_CYAN, maxw > 0 ? maxw : 0);
	}
}
