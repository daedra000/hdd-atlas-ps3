/*
ui_kit.h: PS3 World's look, for HDD Atlas's screens.

The palette, the layout measures and the small pieces every screen uses
(button glyphs and the hints along the bottom, L1/R1 tabs, card frames,
chips, badges, the logo and crumb at the top), lifted from PS3 World's
src/ui.c as they are there, so the two apps look the same.
*/
#ifndef UI_KIT_H
#define UI_KIT_H

#include <stdint.h>
#include "plat.h"
#include "gfx.h"

// PS3 World's palette
#define C_BG		0xff05070d
#define C_TEXT		0xffffffff
// (the accent, and the dim and secondary text, follow Settings' Accent colour: kit_accent_set)
extern uint32_t kit_accent, kit_dim, kit_text2;
#define C_DIM		kit_dim
#define C_ACCENT	kit_accent
#define C_OK		0xff4cd08a
#define C_ERR		0xffff7a7a
#define C_WARN		0xffffc861
#define C_PANEL		0x8c0a0e18
#define C_ROW_ON	((kit_accent & 0x00ffffff) | 0x55000000)
#define C_CYAN		0xff4ad9ff
#define C_TEXT2		kit_text2
enum { ACCENT_BLUE, ACCENT_TEAL, ACCENT_PURPLE, ACCENT_PINK, ACCENT_ORANGE, ACCENT_GREEN, ACCENT_CONTRAST, NUM_ACCENTS };
void kit_accent_set(int which);		// ACCENT_*: CONTRAST is yellow, with the dim text made brighter
const char *kit_accent_name(int which);
#define C_BORDER	0xff1f2533

// and its measures (SCREEN_W is 1280, or 960 for the 4:3 layout)
#define LIST_X		60
#define LIST_W		(SCREEN_W - 2 * LIST_X)
#define GLYPH		28
#define TAB_H		34
#define CHIP_H		36
#define HINT_Y		(SCREEN_H - 58)

enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1, G_L2, G_R2, G_DPAD_UD, G_SELECT, G_START, G_R3, G_DPAD_LR, NUM_GLYPHS };

void kit_init(void);			// the glyphs, backdrop, logo and card background (USRDIR/data)
image_t *kit_load_image(const char *name, int w, int h);	// data/<name>, at the screen's resolution
void kit_backdrop(void);		// the whole screen's background
void kit_backdrop_alpha(int alpha);	// the same, over what's there (the intro fading out)

void kit_fmt_size(long long b, char *out, int size);	// "1.18 GB", "137.5 MB" (as PS3 World)
extern int kit_units_1000;		// sizes in 1000s rather than 1024s
void kit_fmt_size_in(long long b, char *out, int size, int local);	// local 0: the units in English (the report)
void kit_fmt_count(long long n, char *out, int size);	// "61,204"
// a name that isn't UTF-8 (a file named on Windows: "PlayStation\xaePlus"): each byte that isn't
// part of a whole UTF-8 character is Windows-1252's (® ™ é...)
uint32_t kit_cp1252(unsigned char c);
void kit_utf8_clean(const char *in, char *out, int size);
int kit_wrap(int font, int px, const char *s, int maxw, char (*lines)[256], int maxlines);

// the y to draw text at for its capitals to sit in the middle of a box (top, h tall)
int kit_text_y(int font, int px, int top, int h);
// a line drawn at px, or smaller (down to minpx) when it's wider than maxw, cut with "..." past that
int kit_draw_fit(int font, int px, int minpx, int x, int y, const char *s, uint32_t c, int maxw);
// the same, centred on cx
int kit_draw_fit_center(int font, int px, int minpx, int cx, int y, const char *s, uint32_t c, int maxw);
// a phrase that names X or Circle, in the language on screen: x_text as written, or o_text (the same
// with X and Circle swapped) on a console that confirms with O, where input.c swaps the two
const char *kit_btn(const char *x_text, const char *o_text);

void kit_glyph(int g, int x, int y);
int kit_hint(int x, int g, const char *label);			// "<glyph> label" along the bottom; the x after it
int kit_hint2(int x, int g1, int g2, const char *label);
// a row of hints along the bottom, from x, that must end before maxx: when they don't all fit,
// the least important go (the highest prio first, the later of equals first). g2 -1: one glyph
typedef struct { int g, g2; const char *label; int prio; } kit_hint_t;
int kit_hints(const kit_hint_t *h, int n, int x, int maxx);
int kit_tabs(int x, int y, int maxx, const char *const *labels, int n, int sel);
void kit_card_frame(int x, int y, int w, int h, int on);
int kit_chip(int x, int y, const char *text);
void kit_badge(int *rx, int y, const char *text, uint32_t c);
#define LOGO_W	180		// data/logo.png, on the screen (31 high)
void kit_top(const char *crumb, int maxw);		// the logo and the crumb pill beside it (maxw 0: no limit)

#endif
