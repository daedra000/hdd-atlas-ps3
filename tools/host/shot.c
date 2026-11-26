/*
shot.c: the screens drawn on a PC by the CPU renderer (the same gfx.c, font.c, ui.c and
ui_list.c), saved as a PPM, to look at layouts (16:9 and the 4:3 one) without a PS3.

  shot <out.ppm> <canvas width: 1280|960> [keys] [scan_ms]

HOST_SAVE holds the cache it shows (cache_dev_hdd0.bin); HOST_DATA the app's data folder.
keys: buttons pressed in turn, comma separated: U D L R X O S(quare) T(riangle) L1 R1 L2 R2
SEL START, SELSTART (SELECT held, START pressed), HOLD<ms> (X held that long, then let go),
HELD<ms> (X held that long and still held for the shot); a number alone presses Down that many times. The workers (icons, folder listings)
are waited for after each. scan_ms: a scan is started and the shot taken after that long.
HOST_HOME: start on the start screen (otherwise X there opens the last scan); HOST_INTRO=<ms>: the intro, that far in.
HOST_QUERY: what the system keyboard "types" when Triangle opens it.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "../../src/plat.h"
#include "../../src/gfx.h"
#include "../../src/font.h"
#include "../../src/input.h"
#include "../../src/jobs.h"
#include "../../src/ui_int.h"
#include "../../src/ui.h"
#include "../../src/i18n.h"

static uint32_t pad;
static int held_x;		// X still held for the shot (HELD)
uint32_t plat_input_held(void) { return pad; }
const char *plat_data_dir(void) { const char *d = getenv("HOST_DATA"); return d ? d : "data"; }
// HOST_O_CONFIRM: a console that confirms with O (Japan, Asia); the keys given still mean the app's
// X and O, so the other button is the one pressed (input.c swaps them back)
int plat_circle_confirms(void) { return getenv("HOST_O_CONFIRM") != NULL; }
static uint32_t Physical(uint32_t b)
{
	if (plat_circle_confirms() && !(b & BTN_CROSS) != !(b & BTN_CIRCLE)) b ^= BTN_CROSS | BTN_CIRCLE;
	return b;
}
void plat_sound(int id) { (void)id; }
void plat_sound_enable(int on) { (void)on; }
void plat_quit(void) {}
int plat_system_lang(void) { return 1; }
static int osk_up;
int plat_osk_open(const char *title, const char *initial) { (void)title; (void)initial; osk_up = getenv("HOST_QUERY") != NULL; return osk_up; }
int plat_osk_active(void) { return 0; }
int plat_osk_poll(char *out, int size)
{
	if (!osk_up) return -1;
	osk_up = 0;
	snprintf(out, size, "%s", getenv("HOST_QUERY"));
	return 1;
}

static void Settle(void)
{
	for (int i = 0; i < 40; i++) { ui_update(); usleep(5000); }
	if (held_x) pad = Physical(BTN_CROSS);
}

static void Press(uint32_t b)
{
	pad = Physical(b); ui_update();		// (ui_update reads the pad itself)
	pad = 0; ui_update();
	Settle();
}

static uint32_t Button(const char *k)
{
	static const struct { const char *n; uint32_t b; } names[] = {
		{"U", BTN_UP}, {"D", BTN_DOWN}, {"L", BTN_LEFT}, {"R", BTN_RIGHT}, {"X", BTN_CROSS}, {"O", BTN_CIRCLE},
		{"S", BTN_SQUARE}, {"T", BTN_TRIANGLE}, {"L1", BTN_L1}, {"R1", BTN_R1}, {"L2", BTN_L2}, {"R2", BTN_R2}, {"SEL", BTN_SELECT}, {"START", BTN_START},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (!strcmp(names[i].n, k)) return names[i].b;
	fprintf(stderr, "shot: no button %s\n", k);
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	g_screen_w = atoi(argv[2]);
	int scan_ms = argc > 4 ? atoi(argv[4]) : 0;
	uint32_t *canvas = calloc(SCREEN_W * SCREEN_H, 4);
	gfx_init(canvas, SCREEN_W, SCREEN_H);
	font_init();
	jobs_init();
	i18n_init();		// (USRDIR's language.txt: HOST_SAVE's)
	ui_init();
	// the intro: none of it (HOST_INTRO: a frame that many ms in); then the start screen's X opens the
	// last scan, unless HOST_HOME (the shot's of the start screen)
	home_intro_at(getenv("HOST_INTRO") ? atoi(getenv("HOST_INTRO")) : -1);
	Settle();
	if (!getenv("HOST_HOME") && !getenv("HOST_INTRO")) Press(BTN_CROSS);
	if (argc > 3) {
		char keys[512], *save = NULL;
		snprintf(keys, sizeof(keys), "%s", argv[3]);
		for (char *k = strtok_r(keys, ",", &save); k; k = strtok_r(NULL, ",", &save)) {
			if (isdigit((unsigned char)k[0])) { for (int i = atoi(k); i > 0; i--) Press(BTN_DOWN); }
			else if (!strcmp(k, "SELSTART")) Press(BTN_SELECT | BTN_START);
			else if (!strncmp(k, "HOLD", 4) || !strncmp(k, "HELD", 4)) {
				uint64_t until = plat_time_us() + (uint64_t)atoi(k + 4) * 1000;
				pad = Physical(BTN_CROSS);
				while (plat_time_us() < until) { ui_update(); usleep(5000); }
				if (k[2] == 'L' && k[3] == 'D' && k[1] == 'O') { pad = 0; ui_update(); Settle(); }		// HOLD: let go
				else held_x = 1;
			}
			else Press(Button(k));
		}
	}
	if (getenv("HOST_WAIT")) {		// (HOST_WAIT=<ms>: a worker's long look given that long, the screen updated meanwhile)
		uint64_t until = plat_time_us() + (uint64_t)atoi(getenv("HOST_WAIT")) * 1000;
		while (plat_time_us() < until) { ui_update(); usleep(20000); }
	}
	if (scan_ms) {
		Press(BTN_TRIANGLE);
		usleep(scan_ms * 1000);
		ui_update();
		// HOST_AFTER: buttons pressed while it's scanning (comma separated, as keys)
		char after[128], *save = NULL;
		snprintf(after, sizeof(after), "%s", getenv("HOST_AFTER") ? getenv("HOST_AFTER") : "");
		for (char *k = strtok_r(after, ",", &save); k; k = strtok_r(NULL, ",", &save)) Press(Button(k));
	}
	// drawn twice: the first asks for the icons on screen, which the workers read meanwhile
	for (int pass = 0; pass < 2; pass++) {
		Settle();
		gfx_init(canvas, SCREEN_W, SCREEN_H);
		ui_render();
	}
	FILE *f = fopen(argv[1], "wb");
	fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { uint32_t c = canvas[i]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); }
	fclose(f);
	ui_shutdown();
	return 0;
}

// the GPU path isn't used here (the CPU renderer draws)
#include "../../src/gpu.h"
int gpu_tex_alloc(gtex_t *t, int w, int h) { (void)t; (void)w; (void)h; return 0; }
void gpu_tex_free(gtex_t *t) { (void)t; }
void gpu_frame_begin(void) {}
void gpu_frame_submit(const gquad_t *q, int nq, const gbatch_t *b, int nb) { (void)q; (void)nq; (void)b; (void)nb; }
