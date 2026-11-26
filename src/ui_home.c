/*
ui_home.c: what HDD Atlas opens on. First the intro (as PS3 World's: the logo
eases up and in over the backdrop, a glow passes behind it and a line draws
out under it, then it fades into the start screen; any button hurries it on).

Then the start screen: the logo, the drive (L1/R1 for another one), how full
it is and when it was last scanned, and four cards: Scan (or Rescan), Last
scan (straight to what the last one found), Tools (the START menu) and
Settings. Circle twice quits, from here. Nothing's scanned until asked
(unless Settings says to scan at start).
*/
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "lang.h"
#include "scan.h"
#include "music.h"
#include "ui_kit.h"
#include "ui_int.h"
#include "version.h"

enum { C_SCAN, C_LAST, C_TOOLS, C_SETTINGS, NUM_CARDS };

static int sel = -1;				// the card picked (-1: not yet: Last scan when there is one)
static image_t *icons[NUM_CARDS], *logo_big;
static int loaded;

#define INTRO_US		2100000		// the whole of it
#define INTRO_FADE_US	1700000		// where it starts fading into the start screen
#define INTRO_LOGO_W	540
#define INTRO_LOGO_H	93			// (logo.png is 540x93)
#define INTRO_Y			300			// the logo's middle
static uint64_t intro_t0;			// its first frame (0: not drawn yet)
static int intro_done;

void home_init(void)
{
	static const char *const files[NUM_CARDS] = {"menu/scan.png", "menu/last.png", "menu/tools.png", "menu/settings.png"};
	if (loaded) return;
	for (int i = 0; i < NUM_CARDS; i++) icons[i] = kit_load_image(files[i], 40, 40);
	logo_big = kit_load_image("logo.png", INTRO_LOGO_W, INTRO_LOGO_H);
	loaded = 1;
}

void home_shutdown(void)
{
	for (int i = 0; i < NUM_CARDS; i++) { if (icons[i]) image_free(icons[i]); icons[i] = NULL; }
	if (logo_big) image_free(logo_big);
	logo_big = NULL;
	loaded = 0;
}

/*
==================
The intro
==================
*/
static float Phase(float t, float a, float b) { return t <= a ? 0 : t >= b ? 1 : (t - a) / (b - a); }
static float Ease_Out(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

int home_intro_active(void) { return !intro_done; }

// (the screenshots: none of it, or a frame this far in)
void home_intro_at(int ms)
{
	if (ms < 0) { intro_done = 1; return; }
	intro_t0 = plat_time_us() - (uint64_t)ms * 1000;
}

void home_intro_hurry(void)
{
	uint64_t now = plat_time_us();
	if (intro_t0 && now - intro_t0 < INTRO_FADE_US) intro_t0 = now - INTRO_FADE_US;
}

// over the start screen, which is drawn underneath meanwhile
void home_intro_draw(void)
{
	uint64_t now = plat_time_us();
	if (!intro_t0) intro_t0 = now;
	if (now - intro_t0 >= INTRO_US) {
		intro_done = 1;
		ui_dirty();
		return;
	}
	float t = (now - intro_t0) / 1e6f, out = 1 - Phase(t, INTRO_FADE_US / 1e6f, INTRO_US / 1e6f);
	// the backdrop over the start screen, from black
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, with_alpha(C_BG, (int)(255 * out)));
	kit_backdrop_alpha((int)(255 * out));
	float in = Phase(t, 0, 0.45f);
	if (in < 1) gfx_rect(0, 0, SCREEN_W, SCREEN_H, with_alpha(0xff000000, (int)(255 * (1 - in) * out)));
	// a glow passing behind the logo, left to right
	float g = Phase(t, 0.7f, 1.6f);
	if (g > 0 && g < 1) {
		int gx = SCREEN_W / 2 - 420 + (int)(840 * Ease_Out(g)), ga = (int)(4 * g * (1 - g) * 5 * out + 0.5f);
		for (int r = 110; r >= 6; r -= 4) gfx_circle(gx, INTRO_Y, r, with_alpha(C_CYAN, ga));		// (many faint ones: soft, no rings)
	}
	// the logo, rising a little as it grows to its size and comes in
	float e = Ease_Out(Phase(t, 0.25f, 1.0f));
	int lw = (int)(INTRO_LOGO_W * (0.86f + 0.14f * e)), lh = lw * INTRO_LOGO_H / INTRO_LOGO_W;
	if (logo_big) gfx_image(logo_big, (SCREEN_W - lw) / 2, INTRO_Y - lh / 2 + (int)(14 * (1 - e)), lw, lh, 0, (int)(255 * e * out));
	// a line drawn out from the middle under it, then the version
	int half = (int)(150 * Ease_Out(Phase(t, 0.85f, 1.35f)));
	if (half > 0) gfx_rect(SCREEN_W / 2 - half, INTRO_Y + 62, half * 2, 2, with_alpha(C_CYAN, (int)(220 * out)));
	float a = Phase(t, 1.05f, 1.45f);
	if (a > 0) {
		char ver[64];
		snprintf(ver, sizeof(ver), tr("version %s"), APP_VERSION);
		font_draw_center(FONT_MEDIUM, 20, SCREEN_W / 2, INTRO_Y + 78, ver, with_alpha(C_TEXT2, (int)(255 * a * out)), 0);
	}
	ui_dirty();		// (the next frame of it)
}

/*
==================
The start screen
==================
*/
// (Last scan once there's one; Tools not while scanning: the menu's screens would be slow)
static int Card_Ok(int c) { return c == C_LAST ? DRV->shown != NULL : c == C_TOOLS ? ui_scan_drive() < 0 && !ui_drive_hung(cur_drive) : 1; }
static const char *Card_Why(int c) { return c == C_TOOLS ? tr("Once the scan's done.") : tr("Scan this drive first."); }

int home_update(uint32_t p)
{
	if (sel < 0) sel = DRV->shown ? C_LAST : C_SCAN;
	int old = sel;
	if (p & BTN_LEFT) sel = (sel + NUM_CARDS - 1) % NUM_CARDS;
	if (p & BTN_RIGHT) sel = (sel + 1) % NUM_CARDS;
	if (sel != old) { ui_sound(SND_MOVE); ui_dirty(); }
	if (p & BTN_CIRCLE) return HOME_QUIT;
	if (p & BTN_CROSS) {
		if (!Card_Ok(sel)) { ui_sound(SND_BACK); ui_note(sel == C_TOOLS ? tr("The menu opens once the scan's done.") : Card_Why(sel)); return HOME_NONE; }
		ui_sound(SND_OK);
		return sel == C_SCAN ? HOME_SCAN : sel == C_LAST ? HOME_LAST : sel == C_TOOLS ? HOME_TOOLS : HOME_SETTINGS;
	}
	return HOME_NONE;
}

void home_drive_changed(void) { if (sel == C_LAST && !DRV->shown) sel = C_SCAN; }

static void Date(int64_t when, char *out, int size)
{
	time_t t = (time_t)when;
	struct tm *tm = localtime(&t);
	if (!tm || !strftime(out, size, "%Y-%m-%d %H:%M", tm)) snprintf(out, size, "-");
}

// the drive: its name (L1/R1 for another), how full, when it was last scanned
static void Draw_Drive(int y)
{
	char a[32], b[32], c[32], line[200];
	int x = LIST_X - 12, w = LIST_W + 24, h = 104, tx = LIST_X + 6, rx = SCREEN_W - LIST_X - 6, nx = tx;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	int many = ui_drives_present() > 1;
	if (many) { kit_glyph(G_L1, nx, y + 15); nx += GLYPH + 8; }
	nx += font_draw(FONT_BOLD, 24, nx, y + 16, ui_drive_name(cur_drive), C_TEXT, 0);
	if (many) kit_glyph(G_R1, nx + 8, y + 15);
	if (DRV->shown) {
		Date(DRV->shown->when, a, sizeof(a));
		snprintf(line, sizeof(line), tr("Last scanned %s"), a);
	} else snprintf(line, sizeof(line), "%s", tr("Not scanned yet"));
	font_draw_right(FONT_REGULAR, 18, rx, y + 20, line, DRV->shown ? C_TEXT2 : C_WARN);
	// how full (now, from the drive: as the XMB counts it on the HDD)
	uint64_t total = DRV->live_disk ? DRV->live_disk : DRV->live_total, fr = DRV->live_free;
	int by = y + 58, bw = rx - tx;
	gfx_round_rect(tx, by, bw, 12, 6, C_TRACK);
	if (total > fr) {
		double k = (double)(total - fr) / total;
		gfx_round_rect(tx, by, (int)(bw * k) > 12 ? (int)(bw * k) : 12, 12, 6, k > 0.9 ? C_WARN : C_ACCENT);
		kit_fmt_size((long long)(total - fr), a, sizeof(a));
		kit_fmt_size((long long)total, b, sizeof(b));
		kit_fmt_size((long long)fr, c, sizeof(c));
		snprintf(line, sizeof(line), tr("%s used of %s  \xc2\xb7  %s free"), a, b, c);
		font_draw(FONT_MEDIUM, 17, tx, by + 20, line, C_TEXT2, bw);
	}
}

// a card's figure: how the scan's going or how long the last took, the last scan's date
static void Figure(int c, char *out, int size, uint32_t *col)
{
	out[0] = 0;
	*col = C_TEXT2;
	if (c == C_SCAN) {
		scan_progress_t *pr = scan_progress();
		if (ui_scan_drive() == cur_drive) {
			int pct = ui_scan_pct();
			if (pct >= 0) snprintf(out, size, tr("Scanning  %d%%"), pct);
			else {
				char files[24], count[48];
				kit_fmt_count((long long)pr->files, files, sizeof(files));
				snprintf(count, sizeof(count), tr("%s files"), files);
				snprintf(out, size, tr("Scanning %s"), count);
			}
			*col = C_CYAN;
		} else if (DRV->shown && DRV->shown->duration_ms) {
			unsigned s = (DRV->shown->duration_ms + 500) / 1000;
			if (s >= 90) snprintf(out, size, tr("Last time: %u min"), (s + 30) / 60);
			else snprintf(out, size, tr("Last time: %u s"), s ? s : 1);
		}
	} else if (c == C_LAST && DRV->shown) {
		Date(DRV->shown->when, out, size);
		*col = C_OK;
	}
}

void home_render(void)
{
	static const char *const names[NUM_CARDS] = {N_("Scan"), N_("Last scan"), N_("Tools"), N_("Settings")};
	static const char *const says[NUM_CARDS] = {
		N_("Go through every folder on the drive and add up what's there."),
		N_("What the last scan found, straight away."),
		N_("Free up space, duplicates, the map, drive health and more."),
		N_("Language, music, sounds, sizes, scanning, the dimmer, deleting."),
	};
	static const uint32_t colors[NUM_CARDS] = {0xff2b8fff, 0xff2fb57a, 0xff9a6bff, 0xff7b8496};
	if (sel < 0) sel = DRV->shown ? C_LAST : C_SCAN;
	kit_backdrop();
	// the logo, big, and what it's for
	const int lw = 330, lh = lw * INTRO_LOGO_H / INTRO_LOGO_W;
	if (logo_big) gfx_image(logo_big, (SCREEN_W - lw) / 2, 52, lw, lh, 0, 255);
	font_draw_center(FONT_REGULAR, 18, SCREEN_W / 2, 52 + lh + 14, tr("What's using your PS3's space, drive by drive, down to the last file."), C_TEXT2, LIST_W);
	Draw_Drive(178);
	// the cards
	const int gap = 16, y = 302, ch = 272;
	int cw = (LIST_W - (NUM_CARDS - 1) * gap) / NUM_CARDS;
	for (int c = 0; c < NUM_CARDS; c++) {
		int x = LIST_X + c * (cw + gap), on = c == sel, ok = Card_Ok(c);
		if (on) gfx_round_rect(x - 3, y - 3, cw + 6, ch + 6, 19, C_ACCENT);
		gfx_round_rect(x, y, cw, ch, 16, on ? 0xf2142a48 : 0xd00c1220);
		gfx_round_rect(x + 18, y + 18, 60, 60, 15, ok ? colors[c] : 0xff3a4150);
		if (icons[c]) gfx_image(icons[c], x + 18 + 10, y + 18 + 10, 40, 40, 0, ok ? 255 : 110);
		const char *nm = c == C_SCAN && DRV->shown ? tr("Rescan") : tr(names[c]);
		kit_draw_fit(FONT_BOLD, 24, 17, x + 18, y + 96, nm, ok ? C_TEXT : C_DIM, cw - 36);
		char lines[4][256];
		const char *d = ok ? tr(says[c]) : Card_Why(c);
		int dp = screen_narrow() ? 15 : 16, n = kit_wrap(FONT_REGULAR, dp, d, cw - 36, lines, 4);
		for (int i = 0; i < n; i++) font_draw(FONT_REGULAR, dp, x + 18, y + 134 + i * (dp + 6), lines[i], C_TEXT2, cw - 36);
		char f[64];
		uint32_t fc;
		Figure(c, f, sizeof(f), &fc);
		if (f[0]) kit_draw_fit(FONT_MEDIUM, 17, 13, x + 18, y + ch - 34, f, fc, cw - 36);
	}
	const kit_hint_t hints[] = {
		{G_CROSS, -1, tr("Open"), 0},
		{G_DPAD_LR, -1, tr("Choose"), 2},
		{G_L1, G_R1, ui_drives_present() > 1 ? tr("Drive") : NULL, 1},
		{G_R3, -1, music_on() ? tr("Music off") : tr("Music on"), 3},
		{G_CIRCLE, -1, tr("Quit"), 0},
	};
	kit_hints(hints, sizeof(hints) / sizeof(hints[0]), LIST_X, ui_hints_max());
	ui_draw_strip();
}
