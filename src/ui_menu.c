/*
ui_menu.c: START on the overview: a menu of what's not on the overview itself.

  Free up space             ui_list.c's lists: what's likely safe to clear,
  Games, all together       each game added up, the games held twice,
  Duplicates                and the drive's files by type
  File types
  Recently added            ui_list.c's list of what changed lately, newest first
  Buttons                   what every button does, screen by screen
  Changes since last scan   ui_list.c's changes list
  Export a report           report.c, to USRDIR or a USB drive (a panel picks)
  Drive health              ui_health.c (the internal HDD only)
  Settings                  language, music, sound effects, sizes, scanning
                            by itself, allowing deletes (ui_delete.c's lock)
  About                     the version, what it's made with, the licences

The menu and the export panel sit over the overview; Settings and About are
screens of their own. Settings change as you go and are kept (settings.c;
the language in USRDIR/language.txt, as PS3 World keeps it).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "lang.h"
#include "i18n.h"
#include "jobs.h"
#include "music.h"
#include "report.h"
#include "settings.h"
#include "history.h"
#include "bin.h"
#include "health.h"
#include "ui_kit.h"
#include "ui_int.h"
#include "version.h"

enum { M_NONE, M_MENU, M_EXPORT, M_SETTINGS, M_ABOUT, M_BUTTONS, M_HISTORY };
// a grid of tiles, 5 across: two rows exploring the drive, two of the rest (the last place empty)
enum { E_FREE, E_GAMES, E_DUPES, E_SAME, E_OLD,
	E_FOLDERS, E_FILTER, E_TYPES, E_MAP, E_MARKS,
	E_CHANGES, E_HISTORY, E_RECENT, E_EXPORT, E_HEALTH,
	E_BIN, E_SETTINGS, E_BUTTONS, E_ABOUT, NUM_ENTRIES };
#define ACROSS	5
#define ROWS_OF	4
static const char *const icon_files[NUM_ENTRIES] = {"free", "games", "dupes", "same", "old", "folders", "filter", "types", "map", "marks",
	"changes", "history", "recent", "export", "health", "bin", "settings", "buttons", "about"};
static const uint32_t tile_colors[NUM_ENTRIES] = {
	0xff2fb57a, 0xff2b8fff, 0xffef8a2f, 0xffd9792b, 0xff9a6bff, 0xffe0a92e, 0xff2e9ed0, 0xff1fb5d6, 0xff1aa89a, 0xffe85d8f,
	0xff6c7cff, 0xff1fb5d6, 0xff2fb57a, 0xff6f86a8, 0xffe35d5d, 0xffd2564f, 0xff7b8496, 0xffc062e0, 0xff6f86a8};
static image_t *icons[NUM_ENTRIES];
static int icons_loaded;
// what the tiles say, worked out as the menu opens
static struct { uint64_t spare, bin, recent; int games, dupes, marks, scans, health; } fig;
enum { S_LANG, S_MUSIC, S_SFX, S_UNITS, S_ACCENT, S_AUTOSCAN, S_USBSCAN, S_DIM, S_DELETE, S_BIN, NUM_SETTINGS };
static const int bin_steps[] = {7, 30, -1, 0};		// days a deleted row's kept in the recycle bin; -1 till it's emptied, 0 none

static int screen, sel, set_sel, exp_sel;
static int exp_drives[NUM_DRIVES], exp_n;		// where a report can go: -1 USRDIR, else a USB drive
static job_t *exp_job;
static image_t *flags[LANG_COUNT];
static int flags_loaded;
static int buttons_top;
static int settings_alone;				// Settings opened on its own (the start screen): Circle closes the menu
static hist_t hist[HISTORY_MAX];		// Space over time: the drive's scans, oldest first
static int nhist, hist_sel;
static const int dim_steps[] = {0, 2, 5, 10, 30};

static const int music_steps[] = {0, 25, 50, 75, 100};

/*
==================
Settings: applied at start and as they change
==================
*/
void menu_apply_settings(void)
{
	int level = setting_get("music_level", 50);
	music_set_level(level / 100.0f);
	music_set_on(setting_get("music", 1) && level > 0);
	plat_sound_enable(setting_get("sfx", 1));
	kit_units_1000 = setting_get("units1000", 0);
	kit_accent_set(setting_get("accent", ACCENT_BLUE));
}

// R3 anywhere: the music off, or back on at its level
void menu_music_toggle(void)
{
	int on = !music_on();
	if (on && setting_get("music_level", 50) == 0) setting_set("music_level", 50);
	setting_set("music", on);
	menu_apply_settings();
	ui_note(on ? tr("Music on: Relaxing music by Piotr Witowski, from Pixabay") : tr("Music off"));
	plat_log("ui: music %s", on ? "on" : "off");
}

static int Music_Step(void)
{
	int level = setting_get("music", 1) ? setting_get("music_level", 50) : 0, best = 0;
	for (int i = 0; i < 5; i++) if (abs(music_steps[i] - level) < abs(music_steps[best] - level)) best = i;
	return best;
}

static void Change(int row, int dir)
{
	switch (row) {
	case S_LANG: {
		int l = i18n_choice() + dir;		// -1 (the PS3's) .. LANG_COUNT - 1
		if (l < -1) l = LANG_COUNT - 1;
		if (l >= LANG_COUNT) l = -1;
		i18n_set(l);
		plat_log("ui: language %s", l < 0 ? "auto" : lang_code(l));
		break;
	}
	case S_MUSIC: {		// Off, 25, 50, 75, 100% (off keeps the level it had, for R3)
		int i = (Music_Step() + dir + 5) % 5;
		if (music_steps[i]) setting_set("music_level", music_steps[i]);
		setting_set("music", music_steps[i] > 0);
		break;
	}
	case S_SFX: setting_set("sfx", !setting_get("sfx", 1)); break;
	case S_UNITS: setting_set("units1000", !setting_get("units1000", 0)); break;
	case S_AUTOSCAN: setting_set("autoscan", !setting_get("autoscan", 0)); break;
	case S_USBSCAN: setting_set("usbscan", !setting_get("usbscan", 0)); break;
	case S_DIM: {
		int cur = setting_get("dim", 5), i = 0;
		for (int k = 0; k < 5; k++) if (dim_steps[k] == cur) i = k;
		setting_set("dim", dim_steps[(i + dir + 5) % 5]);
		break;
	}
	case S_DELETE: delete_lock_toggle(); break;		// (on: its warning first; off: at once)
	case S_ACCENT: setting_set("accent", (setting_get("accent", ACCENT_BLUE) + dir + NUM_ACCENTS) % NUM_ACCENTS); break;
	case S_BIN: {
		int cur = setting_get("bin", 7), i = 0;
		for (int k = 0; k < 4; k++) if (bin_steps[k] == cur) i = k;
		setting_set("bin", bin_steps[(i + dir + 4) % 4]);
		break;
	}
	}
	menu_apply_settings();
	ui_dirty();
}

/*
==================
The menu, the export panel
==================
*/
static int Entry_Ok(int e)
{
	if (e == E_CHANGES) return DRV->changes != NULL;
	if (e == E_EXPORT || e == E_FREE || e == E_GAMES || e == E_DUPES || e == E_TYPES || e == E_OLD || e == E_FOLDERS || e == E_MAP ||
		e == E_SAME || e == E_FILTER || e == E_RECENT) return DRV->shown != NULL;
	if (e == E_HEALTH) return cur_drive == 0;
	return 1;
}

// the drive health verdict last read (smart_history.txt's last line ends in it); -1 none
static int Last_Health(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/smart_history.txt", plat_save_dir());
	char *t = (char *)plat_read_file(path, NULL);
	if (!t) return -1;
	int n = (int)strlen(t);
	while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) t[--n] = 0;
	char *last = strrchr(t, ' ');
	int v = last ? atoi(last + 1) : -1;
	free(t);
	return v >= HEALTH_GOOD && v <= HEALTH_FAILING ? v : -1;
}

void menu_open_settings(void)
{
	screen = M_SETTINGS;
	set_sel = 0;
	settings_alone = 1;
	ui_dirty();
}

void menu_open(void)
{
	screen = M_MENU;
	settings_alone = 0;
	memset(&fig, 0, sizeof(fig));
	if (DRV->shown) list_figures(&fig.spare, &fig.games, &fig.dupes, &fig.marks);
	static hist_t tmp[HISTORY_MAX];
	fig.scans = DRV->shown ? history_load_of(DRV->shown, tmp, HISTORY_MAX) : history_count(DRV->root);
	fig.health = cur_drive == 0 ? Last_Health() : -1;
	fig.bin = bin_bytes(DRV->root);
	fig.recent = DRV->shown ? list_recent_bytes(30) : 0;
	ui_dirty();
}

int menu_active(void) { return screen != M_NONE; }
int menu_fullscreen(void) { return screen == M_MENU || screen == M_EXPORT || screen == M_SETTINGS || screen == M_ABOUT || screen == M_BUTTONS || screen == M_HISTORY; }

// the report's drive and folder, fixed when it's asked for (the drive on screen can change while
// it's written); the drive's scan and changes stay put till it's done (menu_exporting: a scan
// that finishes meanwhile waits, and deletes queue behind it on the same lane)
typedef struct { int drive; char dir[256]; } export_t;

static void Export_Run(job_t *j)
{
	const export_t *a = j->arg;
	const drive_t *d = &drives[a->drive];
	j->ok = d->shown && report_write(d->shown, d->changes, d->name, a->dir, j->sval, sizeof(j->sval)) == 0;
}
static void Export_Cleanup(job_t *j) { free(j->arg); j->arg = NULL; }

int menu_exporting(void) { return exp_job != NULL; }

static void Export_Start(int where)
{
	if (exp_job || !DRV->shown) return;
	export_t *a = malloc(sizeof(*a));
	exp_job = a ? job_new(Export_Run, Export_Cleanup) : NULL;
	if (!exp_job) { free(a); ui_note(tr("No memory to write the report.")); return; }
	a->drive = cur_drive;
	if (where < 0) snprintf(a->dir, sizeof(a->dir), "%s", plat_save_dir());
	else snprintf(a->dir, sizeof(a->dir), "%s/HDDAtlas", drives[where].root);
	exp_job->arg = a;
	job_post(exp_job, LANE_DL);
	ui_note(tr("Writing the report..."));
}

// (each frame, whatever's on screen: a report being written)
static void Export_Poll(void)
{
	if (!exp_job || !job_done(exp_job)) return;
	char msg[400];
	// (in HDD Atlas's own folder: "USRDIR/..." says where; the whole path is too long for a note at 4:3)
	const char *where = exp_job->sval, *save = plat_save_dir();
	size_t sl = strlen(save);
	char shown[300];
	if (!strncmp(where, save, sl) && where[sl] == '/') { snprintf(shown, sizeof(shown), "USRDIR%s", where + sl); where = shown; }
	if (exp_job->ok) snprintf(msg, sizeof(msg), tr("Report saved: %s (and .csv)"), where);
	else snprintf(msg, sizeof(msg), "%s", tr("The report couldn't be written (see the log)."));
	ui_note(msg);
	ui_sound(exp_job->ok ? SND_OK : SND_BACK);
	job_release(exp_job);
	exp_job = NULL;
}

int menu_update(uint32_t p)
{
	Export_Poll();
	if (screen == M_HISTORY) {
		int old = hist_sel;
		if ((p & BTN_LEFT) && hist_sel > 0) hist_sel--;
		if ((p & BTN_RIGHT) && hist_sel < nhist - 1) hist_sel++;
		if (hist_sel != old) ui_sound(SND_MOVE);
		if (p & (BTN_CIRCLE | BTN_CROSS)) { ui_sound(SND_BACK); screen = M_MENU; }
		ui_dirty();
		return 0;
	}
	if (screen == M_BUTTONS) {
		if (p & BTN_DOWN) buttons_top++;
		if (p & BTN_UP) buttons_top--;
		if (p & (BTN_CIRCLE | BTN_CROSS)) { ui_sound(SND_BACK); screen = M_MENU; }
		ui_dirty();
		return 0;
	}
	if (screen == M_MENU) {
		int old = sel, col = sel % ACROSS, row = sel / ACROSS;
		if (p & BTN_UP) row = (row + ROWS_OF - 1) % ROWS_OF;
		if (p & BTN_DOWN) row = (row + 1) % ROWS_OF;
		if (p & BTN_LEFT) col = (col + ACROSS - 1) % ACROSS;
		if (p & BTN_RIGHT) col = (col + 1) % ACROSS;
		sel = row * ACROSS + col;		// (one that can't open yet can be picked: it says why)
		if (sel >= NUM_ENTRIES) sel = (p & BTN_RIGHT) ? row * ACROSS : NUM_ENTRIES - 1;		// (the empty place: past it, or the last)
		if (sel != old) ui_sound(SND_MOVE);
		if (p & BTN_CIRCLE) { ui_sound(SND_BACK); screen = M_NONE; }
		if ((p & BTN_CROSS) && Entry_Ok(sel)) {
			ui_sound(SND_OK);
			screen = M_NONE;
			switch (sel) {
			case E_CHANGES: ui_dirty(); return MENU_GO_CHANGES;
			case E_FREE: ui_dirty(); return MENU_GO_FREE;
			case E_GAMES: ui_dirty(); return MENU_GO_GAMES;
			case E_DUPES: ui_dirty(); return MENU_GO_DUPES;
			case E_TYPES: ui_dirty(); return MENU_GO_TYPES;
			case E_RECENT: ui_dirty(); return MENU_GO_RECENT;
			case E_OLD: ui_dirty(); return MENU_GO_OLD;
			case E_FOLDERS: ui_dirty(); return MENU_GO_FOLDERS;
			case E_MARKS: ui_dirty(); return MENU_GO_MARKS;
			case E_MAP: ui_dirty(); return MENU_GO_MAP;
			case E_SAME: ui_dirty(); return MENU_GO_SAME;
			case E_FILTER: ui_dirty(); return MENU_GO_FILTER;
			case E_BIN: ui_dirty(); return MENU_GO_BIN;
			case E_HISTORY:
				nhist = DRV->shown ? history_load_of(DRV->shown, hist, HISTORY_MAX) : history_load(DRV->root, hist, HISTORY_MAX);
				hist_sel = nhist - 1;
				screen = M_HISTORY;
				break;
			case E_BUTTONS: screen = M_BUTTONS; buttons_top = 0; break;
			case E_HEALTH: ui_dirty(); return MENU_GO_HEALTH;
			case E_SETTINGS: screen = M_SETTINGS; set_sel = 0; break;
			case E_ABOUT: screen = M_ABOUT; break;
			case E_EXPORT:
				exp_n = 0;
				exp_drives[exp_n++] = -1;
				for (int i = 1; i < FIRST_NTFS; i++) if (drives[i].present) exp_drives[exp_n++] = i;		// (FAT32 drives: a report's written with stdio, which an NTFS drive isn't)
				exp_sel = 0;
				screen = M_EXPORT;
				break;
			}
		}
	} else if (screen == M_EXPORT) {
		if (p & BTN_UP) { exp_sel = (exp_sel + exp_n - 1) % exp_n; ui_sound(SND_MOVE); }
		if (p & BTN_DOWN) { exp_sel = (exp_sel + 1) % exp_n; ui_sound(SND_MOVE); }
		if (p & BTN_CIRCLE) { ui_sound(SND_BACK); screen = M_MENU; }
		if (p & BTN_CROSS) { ui_sound(SND_OK); Export_Start(exp_drives[exp_sel]); screen = M_NONE; }
	} else if (screen == M_SETTINGS) {
		int old = set_sel;
		if (p & BTN_UP) set_sel = (set_sel + NUM_SETTINGS - 1) % NUM_SETTINGS;
		if (p & BTN_DOWN) set_sel = (set_sel + 1) % NUM_SETTINGS;
		if (set_sel != old) ui_sound(SND_MOVE);
		if (p & (BTN_LEFT | BTN_RIGHT | BTN_CROSS)) {
			ui_sound(SND_MOVE);
			Change(set_sel, (p & BTN_LEFT) ? -1 : 1);
		}
		if (p & BTN_CIRCLE) { ui_sound(SND_BACK); screen = settings_alone ? M_NONE : M_MENU; settings_alone = 0; }
	} else if (screen == M_ABOUT) {
		if (p & (BTN_CIRCLE | BTN_CROSS)) { ui_sound(SND_BACK); screen = M_MENU; }
	}
	ui_dirty();
	return 0;
}

void menu_poll(void) { Export_Poll(); }

void menu_shutdown(void)
{
	if (exp_job) {
		jobs_wait_lane(LANE_DL, 3000000);
		job_release(exp_job);
		exp_job = NULL;
	}
	for (int i = 0; i < LANG_COUNT; i++) { if (flags[i]) image_free(flags[i]); flags[i] = NULL; }
	for (int i = 0; i < NUM_ENTRIES; i++) { if (icons[i]) image_free(icons[i]); icons[i] = NULL; }
	icons_loaded = 0;
}

/*
==================
Drawing
==================
*/
static void Dim_Box(int w, int h, int *x, int *y)
{
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xa0000000);
	gfx_rect(0, HINT_Y - 14, SCREEN_W, SCREEN_H - HINT_Y + 14, 0xf205070d);
	*x = (SCREEN_W - w) / 2;
	*y = (SCREEN_H - h) / 2 - 10;
	gfx_round_rect(*x - 1, *y - 1, w + 2, h + 2, 19, C_BORDER);
	gfx_round_rect(*x, *y, w, h, 18, 0xf20a0e18);
}

// a tile's figure: what's there, at a glance ("" none)
static uint32_t Figure(int e, char *out, int size)
{
	static const char *const verdicts[] = {N_("GOOD"), N_("CAUTION"), N_("FAILING")};
	char a[32];
	out[0] = 0;
	if (!Entry_Ok(e)) return C_DIM;
	switch (e) {
	case E_FREE: if (fig.spare) kit_fmt_size((long long)fig.spare, out, size); return C_OK;
	case E_GAMES: if (fig.games) snprintf(out, size, tr(fig.games == 1 ? "%d game" : "%d games"), fig.games); break;
	case E_DUPES: if (fig.dupes) snprintf(out, size, tr(fig.dupes == 1 ? "%d game" : "%d games"), fig.dupes); return C_WARN;
	case E_MARKS: if (fig.marks) snprintf(out, size, tr(fig.marks == 1 ? "%d bookmark" : "%d bookmarks"), fig.marks); break;
	case E_HISTORY: if (fig.scans) snprintf(out, size, tr(fig.scans == 1 ? "%d scan" : "%d scans"), fig.scans); break;
	case E_MAP:
		if (DRV->shown) { kit_fmt_size((long long)scan_drive_used(DRV->shown), a, sizeof(a)); snprintf(out, size, tr("%s used"), a); }
		break;
	case E_RECENT: if (fig.recent) { kit_fmt_size((long long)fig.recent, a, sizeof(a)); snprintf(out, size, tr("%s in 30 days"), a); } break;
	case E_HEALTH:
		if (fig.health >= 0) snprintf(out, size, "%s", tr(verdicts[fig.health]));
		return fig.health == HEALTH_FAILING ? C_ERR : fig.health == HEALTH_CAUTION ? C_WARN : C_OK;
	case E_ABOUT: snprintf(out, size, "%s", APP_VERSION); break;
	case E_BIN: if (fig.bin) kit_fmt_size((long long)fig.bin, out, size); return C_WARN;
	}
	return C_TEXT2;
}

static void Draw_Menu(void)
{
	static const char *const names[NUM_ENTRIES] = {N_("Free up space"), N_("Games, all together"), N_("Duplicates"), N_("Identical files"),
		N_("Not played lately"), N_("Largest folders"), N_("Size filter"), N_("File types"), N_("Map of the drive"), N_("Bookmarks"),
		N_("Changes since last scan"), N_("Space over time"), N_("Recently added"), N_("Export a report"), N_("Drive health"),
		N_("Recycle bin"), N_("Settings"), N_("Buttons"), N_("About")};
	static const char *const descs[NUM_ENTRIES] = {
		N_("What's likely safe to clear: leftover packages, caches, extra copies of games, unfinished installs."),
		N_("Each game's installed data, backups, saves and trophies, added up."),
		N_("Games held more than once, here or on another drive, and what removing the extra copies would free."),
		N_("Files the same, byte for byte, where you keep things: disc images, media, packages, your own folders."),
		N_("Games here, the longest unplayed first: good ones to clear if you're done with them."),
		N_("The folders holding the most, at any depth."),
		N_("Everything over a size, untouched for a while."),
		N_("The drive's files by type: disc images, packages, video, saves..."),
		N_("The drive as boxes sized by the space they take: X goes into one."),
		N_("Folders you've bookmarked (Square in a folder)."),
		N_("What grew, shrank, appeared or went since the scan before."),
		N_("Used space scan by scan, by category."),
		N_("What's been added or changed lately, newest first: what's taken your space."),
		N_("The scan as a text file and a spreadsheet (CSV), on the HDD or a USB drive."),
		N_("The internal HDD's own record of its wear (SMART)."),
		N_("What's been deleted, waiting to go for good: put it back, or empty the bin."),
		N_("Language, music, sounds, sizes, the colour, scanning, the dimmer, deleting."),
		N_("What every button does, screen by screen."),
		N_("Version, credits and licences."),
	};
	if (!icons_loaded) {
		char name[64];
		for (int i = 0; i < NUM_ENTRIES; i++) {
			snprintf(name, sizeof(name), "menu/%s.png", icon_files[i]);
			icons[i] = kit_load_image(name, 32, 32);
		}
		icons_loaded = 1;
	}
	kit_backdrop();
	kit_top(tr("Menu"), 0);
	font_draw_right(FONT_MEDIUM, 18, SCREEN_W - LIST_X, 44, ui_drive_name(cur_drive), C_TEXT);
	// two groups of two rows of tiles; under them, what the one picked is
	const int gap = 14, head_h = 30, th = 100;
	int tw = (LIST_W - (ACROSS - 1) * gap) / ACROSS, y = 92;
	const char *heads[2] = {tr("Explore"), tr("Tools")};
	for (int r = 0; r < ROWS_OF; r++) {
		if (r % 2 == 0) { font_draw(FONT_BOLD, 17, LIST_X + 2, y + 4, heads[r / 2], C_CYAN, LIST_W); y += head_h; }
		for (int c = 0; c < ACROSS; c++) {
			int e = r * ACROSS + c, x = LIST_X + c * (tw + gap), ok = e < NUM_ENTRIES && Entry_Ok(e), on = e == sel;
			if (e >= NUM_ENTRIES) continue;		// (the empty place)
			if (on) gfx_round_rect(x - 3, y - 3, tw + 6, th + 6, 17, C_ACCENT);
			gfx_round_rect(x, y, tw, th, 14, on ? 0xf2142a48 : 0xd00c1220);
			// the icon on its colour, the figure beside it, the name under them
			uint32_t col = ok ? tile_colors[e] : 0xff3a4150;
			gfx_round_rect(x + 14, y + 14, 44, 44, 12, col);
			if (icons[e]) gfx_image(icons[e], x + 14 + 6, y + 14 + 6, 32, 32, 0, ok ? 255 : 110);
			char f[64];
			uint32_t fc = Figure(e, f, sizeof(f));
			if (f[0]) kit_draw_fit(FONT_MEDIUM, 17, 12, x + 70, kit_text_y(FONT_MEDIUM, 17, y + 14, 44), f, fc, tw - 70 - 12);
			// (a name too long for one line at 16 takes two)
			const char *nm = tr(names[e]);
			char two[2][256];
			int nl = font_width(FONT_MEDIUM, 16, nm) > tw - 28 ? kit_wrap(FONT_MEDIUM, 15, nm, tw - 28, two, 2) : 0;
			if (nl == 2) {		// (a word that doesn't fit a line isn't cut in two: the name's made smaller instead)
				char word[128];
				for (const char *w = nm; *w && nl == 2; ) {
					size_t l = strcspn(w, " ");
					snprintf(word, sizeof(word), "%.*s", (int)(l < sizeof(word) - 1 ? l : sizeof(word) - 1), w);
					if (font_width(FONT_MEDIUM, 15, word) > tw - 28) nl = 1;
					w += l + (w[l] ? 1 : 0);
				}
			}
			if (nl == 2) {
				font_draw(FONT_MEDIUM, 15, x + 16, y + 60, two[0], ok ? C_TEXT : C_DIM, tw - 28);
				font_draw(FONT_MEDIUM, 15, x + 16, y + 78, two[1], ok ? C_TEXT : C_DIM, tw - 28);
			} else kit_draw_fit(FONT_MEDIUM, 19, nl ? 11 : 16, x + 16, y + 68, nm, ok ? C_TEXT : C_DIM, tw - 28);
		}
		y += th + gap;
	}
	const char *d = sel == E_MAP ? kit_btn(descs[E_MAP], N_("The drive as boxes sized by the space they take: Circle goes into one."))
		: tr(descs[sel]);
	if (!Entry_Ok(sel))
		d = sel == E_CHANGES ? tr("After the next scan of this drive.") : sel == E_HEALTH ? tr("The internal HDD only.") : tr("Scan this drive first.");
	char lines[2][256];
	int n = kit_wrap(FONT_REGULAR, 18, d, LIST_W, lines, 2);
	for (int i = 0; i < n; i++) font_draw(FONT_REGULAR, 18, LIST_X + 2, y + 2 + i * 24, lines[i], C_TEXT2, LIST_W);
	const kit_hint_t hints[] = {{G_CROSS, -1, tr("Open"), 0}, {G_DPAD_UD, -1, tr("Move"), 1}, {G_CIRCLE, -1, tr("Close"), 0}};
	kit_hints(hints, 3, LIST_X, ui_hints_max());
	ui_draw_strip();
}

static void Draw_History(void)
{
	kit_backdrop();
	kit_top(tr("Space over time"), 0);
	font_draw_right(FONT_MEDIUM, 18, SCREEN_W - LIST_X, 44, ui_drive_name(cur_drive), C_TEXT);
	int x = LIST_X - 12, w = LIST_W + 24, y = 100, h = HINT_Y - 18 - y;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	if (nhist < 2) {
		font_draw_center(FONT_MEDIUM, 21, SCREEN_W / 2, y + h / 2 - 12,
			nhist ? tr("Only one scan so far: the chart grows with each scan.") : tr("No scans kept yet."), C_DIM, w - 80);
	}
	// the last scans that fit, each a bar of its categories, up to the drive's size
	int show = nhist < 30 ? nhist : 30, first = nhist - show;
	if (hist_sel < first) first = hist_sel;
	if (hist_sel >= first + show) first = hist_sel - show + 1;
	int cx = LIST_X + 10, cy = y + 24, ch = h - 190, cwid = LIST_W - 20;
	uint64_t top = 1;
	for (int i = first; i < first + show; i++) if (hist[i].total > top) top = hist[i].total;
	int bw = show ? cwid / (show > 0 ? show : 1) : cwid;
	for (int i = first; nhist >= 1 && i < first + show; i++) {
		const hist_t *e = &hist[i];
		int bx = cx + (i - first) * bw + 2, bww = bw - 4 > 4 ? bw - 4 : 4, base = cy + ch;
		gfx_round_rect(bx, cy + ch - (int)((double)ch * e->total / top), bww, (int)((double)ch * e->total / top), 4, i == hist_sel ? 0x30ffffff : 0x14ffffff);
		double at = 0;
		for (int k = 0; k < NUM_CATS; k++) {
			double hh = (double)ch * e->cat[k] / top;
			int y1 = base - (int)(at + hh), y0 = base - (int)at;
			at += hh;
			if (y0 - y1 < 1) continue;
			gfx_rect(bx, y1, bww, y0 - y1, i == hist_sel ? cat_colors[k] : with_alpha(cat_colors[k], 0xa0));
		}
	}
	// the scan picked: when, used, free, and the change from the one before
	if (nhist >= 1) {
		const hist_t *e = &hist[hist_sel];
		char when[32] = "", a[32], b[32], c[32], line[300];
		time_t t = (time_t)e->when;
		struct tm *tm = localtime(&t);
		if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
		kit_fmt_size((long long)e->used, a, sizeof(a));
		kit_fmt_size((long long)e->total, b, sizeof(b));
		kit_fmt_size((long long)e->free_bytes, c, sizeof(c));
		int iy = cy + ch + 20;
		font_draw(FONT_BOLD, 22, LIST_X + 10, iy, when, C_TEXT, 300);
		snprintf(line, sizeof(line), tr("%s used of %s  \xc2\xb7  %s free"), a, b, c);
		font_draw(FONT_MEDIUM, 19, LIST_X + 10, iy + 36, line, C_TEXT2, LIST_W - 20);
		if (hist_sel > 0) {
			long long d = (long long)e->used - (long long)hist[hist_sel - 1].used;
			char dd[32], pw[32] = "";
			time_t pt = (time_t)hist[hist_sel - 1].when;
			struct tm *ptm = localtime(&pt);
			if (ptm) strftime(pw, sizeof(pw), "%Y-%m-%d", ptm);
			kit_fmt_size(d < 0 ? -d : d, dd, sizeof(dd));
			snprintf(line, sizeof(line), tr(d < 0 ? "\xe2\x88\x92%s used since %s" : "+%s used since %s"), dd, pw);
			font_draw(FONT_MEDIUM, 19, LIST_X + 10, iy + 66, line, d > 0 ? C_WARN : C_OK, LIST_W - 20);
		}
		if (e->partial) {
			snprintf(line, sizeof(line), tr("(%s rescanned)"), tr(scan_cat_name(e->partial - 1)));
			font_draw_right(FONT_REGULAR, 17, SCREEN_W - LIST_X - 10, iy + 6, line, C_DIM);
		}
	}
	const kit_hint_t hints[] = {{G_DPAD_LR, -1, nhist > 1 ? tr("Choose a scan") : NULL, 1}, {G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 2, LIST_X, ui_hints_max());
	ui_draw_strip();
}

// a line of the button guide: a heading (no glyph), or one or two glyphs and what they do
typedef struct { int g1, g2; const char *text; } bline_t;
#define HEAD -1
static const bline_t guide[] = {
	{HEAD, -1, N_("Start screen")},
	{G_DPAD_LR, -1, N_("Pick Scan, Last scan, Tools or Settings")},
	{G_L1, G_R1, N_("Change drive (when a USB drive is plugged in)")},
	{G_CIRCLE, -1, N_("Twice: quit")},
	{HEAD, -1, N_("Overview")},
	{G_DPAD_UD, -1, N_("Pick a category (its part of the ring stands out)")},
	{G_CROSS, -1, N_("Open the category's items")},
	{G_SQUARE, -1, N_("The largest items on the drive")},
	{G_TRIANGLE, -1, N_("Scan the drive again")},
	{G_L1, G_R1, N_("Change drive (when a USB drive is plugged in)")},
	{G_START, -1, N_("The menu (not while scanning)")},
	{G_CIRCLE, -1, N_("Stop a scan, or back to the start screen")},
	{HEAD, -1, N_("Lists")},
	{G_DPAD_UD, -1, N_("Move")},
	{G_L2, G_R2, N_("A page at a time (or Left and Right)")},
	{G_CROSS, -1, N_("Open: a folder, a game's parts, the files of a type")},
	{G_TRIANGLE, -1, N_("Search by name")},
	{G_SELECT, -1, N_("Sort: largest, smallest, A-Z")},
	{G_L1, G_R1, N_("Change tab")},
	{G_SQUARE, -1, N_("A category's list: scan just that category again. Largest: items or files")},
	{G_START, -1, N_("Delete the row (when deleting is allowed in Settings)")},
	{G_CIRCLE, -1, N_("Back (a search is cleared first; in a folder, up one)")},
	{HEAD, -1, N_("Deleting")},
	{G_DPAD_LR, -1, N_("Pick Delete or Cancel")},
	{G_CROSS, -1, N_("Hold on Delete for 2 seconds to delete")},
	{G_CIRCLE, -1, N_("Cancel, or stop between files")},
	{HEAD, -1, N_("Drive health and Settings")},
	{G_TRIANGLE, -1, N_("Read the drive's health again")},
	{G_SQUARE, -1, N_("Drive health: its readings over time")},
	{G_SELECT, -1, N_("Drive health: what it all means")},
	{G_DPAD_LR, -1, N_("Change a setting")},
	{HEAD, -1, N_("Keeping, the bin, filters")},
	{G_SQUARE, -1, N_("Free up space, Duplicates: keep a row off them (or back on)")},
	{G_SQUARE, -1, N_("A game's screen: what it's made of")},
	{G_CROSS, -1, N_("Recycle bin: put it back (Square there empties the bin)")},
	{G_L1, G_R1, N_("Size filter: the size (Square there: how long)")},
	{G_SQUARE, -1, N_("Recently added: how far back")},
	{HEAD, -1, N_("Folders, Map, Space over time")},
	{G_SQUARE, -1, N_("In a folder: bookmark it (or remove the bookmark)")},
	{G_CROSS, -1, N_("Map: go into the box picked")},
	{G_DPAD_LR, -1, N_("Map: pick a box. Space over time: pick a scan")},
	{HEAD, -1, N_("Anywhere")},
	{G_R3, -1, N_("Music off or on")},
	{G_CROSS, -1, N_("When the screen's dimmed: any button wakes it")},
	{HEAD, -1, N_("When HDD Atlas starts")},
	{G_SELECT, -1, N_("Held: the 4:3 layout on a widescreen TV")},
	{G_L1, -1, N_("Held: drawn by the CPU instead of the graphics chip")},
};

static void Draw_Buttons(void)
{
	kit_backdrop();
	kit_top(tr("Buttons"), 0);
	int x = LIST_X - 12, w = LIST_W + 24, y = 100, h = HINT_Y - 18 - y, lh = 34;
	int n = sizeof(guide) / sizeof(guide[0]), rows = (h - 20) / lh;
	if (buttons_top > n - rows) buttons_top = n - rows;
	if (buttons_top < 0) buttons_top = 0;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	for (int r = 0; r < rows && buttons_top + r < n; r++) {
		const bline_t *b = &guide[buttons_top + r];
		int ly = y + 12 + r * lh;
		if (b->g1 == HEAD) { font_draw(FONT_BOLD, 19, LIST_X + 6, kit_text_y(FONT_BOLD, 19, ly, lh), tr(b->text), C_CYAN, LIST_W - 12); continue; }
		int gx = LIST_X + 22, gy = ly + (lh - GLYPH) / 2;
		kit_glyph(b->g1, gx, gy);
		if (b->g2 >= 0) kit_glyph(b->g2, gx + GLYPH + 4, gy);
		font_draw(FONT_REGULAR, 18, LIST_X + 22 + 2 * GLYPH + 20, kit_text_y(FONT_REGULAR, 18, ly, lh), tr(b->text), C_TEXT2, LIST_W - 2 * GLYPH - 60);
	}
	if (n > rows) {
		int bh = (h - 20) * rows / n, by = y + 10 + (h - 20 - bh) * buttons_top / (n - rows);
		gfx_round_rect(x + w - 6, by, 4, bh, 2, 0x99ffffff);
	}
	const kit_hint_t hints[] = {{G_DPAD_UD, -1, n > rows ? tr("Scroll") : NULL, 1}, {G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 2, LIST_X, ui_hints_max());
	ui_draw_strip();
}

static void Draw_Export(void)
{
	int w = screen_narrow() ? 700 : 760, rh = 60, h = exp_n * rh + 110, x, y;
	Dim_Box(w, h, &x, &y);
	font_draw(FONT_BOLD, 24, x + 32, y + 22, tr("Save the report where?"), C_TEXT, w - 64);
	for (int i = 0; i < exp_n; i++) {
		int ry = y + 70 + i * rh;
		char line[300], sub[300];
		if (exp_drives[i] < 0) {
			snprintf(line, sizeof(line), "%s", tr("On the HDD, in HDD Atlas's folder"));
			snprintf(sub, sizeof(sub), "%s", plat_save_dir());
		} else {
			snprintf(line, sizeof(line), tr("On %s"), ui_drive_name(exp_drives[i]));
			snprintf(sub, sizeof(sub), "%s/HDDAtlas", drives[exp_drives[i]].root);
		}
		if (i == exp_sel) gfx_round_rect(x + 12, ry, w - 24, rh - 6, 12, C_ROW_ON);
		font_draw(FONT_MEDIUM, 20, x + 32, ry + 6, line, C_TEXT, w - 64);
		font_draw(FONT_REGULAR, 15, x + 32, ry + 32, sub, C_DIM, w - 64);
	}
	const kit_hint_t hints[] = {{G_CROSS, -1, tr("Save"), 0}, {G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 2, LIST_X, SCREEN_W - LIST_X);
}

static void Load_Flags(void)
{
	char name[64];
	if (flags_loaded) return;
	flags_loaded = 1;
	for (int i = 0; i < LANG_COUNT; i++) {
		snprintf(name, sizeof(name), "flags/%s.png", lang_flag(i));
		flags[i] = kit_load_image(name, 32, 24);
	}
}

static void Draw_Settings(void)
{
	static const char *const labels[NUM_SETTINGS] = {N_("Language"), N_("Music"), N_("Sound effects"), N_("Sizes"), N_("Accent colour"),
		N_("Rescan at start"), N_("Scan USB drives"), N_("Dim the screen"), N_("Allow deleting"), N_("Recycle bin")};
	static const char *const helps[NUM_SETTINGS] = {
		N_("Automatic follows the PS3's own language. Translations are by HDD Atlas, not checked by native speakers yet."),
		N_("How loud the background music is under the click sounds. R3 turns it off or on anywhere."),
		N_("The clicks when you move and press."),
		N_("1024 is how the XMB counts (931 GB for a 1 TB drive); 1000 is how drive boxes count (1 TB)."),
		N_("The colour of what's picked. High contrast makes it yellow and the grey text brighter."),
		N_("When the free space has moved since the last scan, scan again by itself at start."),
		N_("Scan a USB drive by itself when it's plugged in (when no other scan is running)."),
		N_("After this long without a button, the screen goes dark (any button wakes it): kind to plasma and OLED TVs."),
		N_("Lets START delete a row in the lists: disc images, folder backups, leftover packages, ROMs and webMAN's cache."),
		N_("What's deleted waits in the recycle bin on its drive (the menu's Recycle bin) for this long, so it can be put back. Off deletes at once."),
	};
	Load_Flags();
	kit_backdrop();
	kit_top(tr("Settings"), 0);
	font_draw_right(FONT_REGULAR, 16, SCREEN_W - LIST_X, 40, APP_NAME " " APP_VERSION, C_DIM);
	int py = 96, rh = 44, x = LIST_X - 12, w = LIST_W + 24;
	gfx_round_rect(x, py, w, NUM_SETTINGS * rh + 16, 14, C_PANEL);
	for (int r = 0; r < NUM_SETTINGS; r++) {
		int y = py + 8 + r * rh, rx = SCREEN_W - LIST_X - 10;
		if (r == set_sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, rh - 6, 10, C_ROW_ON);
		font_draw(FONT_MEDIUM, 21, LIST_X + 10, kit_text_y(FONT_MEDIUM, 21, y, rh - 6), tr(labels[r]), C_TEXT, LIST_W / 2);
		char v[160];
		int lang_flag_i = -1;
		switch (r) {
		case S_LANG:
			if (i18n_choice() < 0) { snprintf(v, sizeof(v), tr("Automatic: %s"), lang_name(i18n_system())); lang_flag_i = i18n_system(); }
			else { snprintf(v, sizeof(v), "%s", lang_name(i18n_choice())); lang_flag_i = i18n_choice(); }
			break;
		case S_MUSIC: {
			int st = Music_Step();
			if (!music_steps[st]) snprintf(v, sizeof(v), "%s", tr("Off"));
			else snprintf(v, sizeof(v), "%d%%", music_steps[st]);
			break;
		}
		case S_SFX: snprintf(v, sizeof(v), "%s", setting_get("sfx", 1) ? tr("On") : tr("Off")); break;
		case S_UNITS: snprintf(v, sizeof(v), "%s", setting_get("units1000", 0) ? tr("1 GB = 1000 MB (as on the box)") : tr("1 GB = 1024 MB (as the XMB)")); break;
		case S_AUTOSCAN: snprintf(v, sizeof(v), "%s", setting_get("autoscan", 0) ? tr("When free space has moved") : tr("Off")); break;
		case S_USBSCAN: snprintf(v, sizeof(v), "%s", setting_get("usbscan", 0) ? tr("When plugged in") : tr("Off")); break;
		case S_DIM: {
			int m = setting_get("dim", 5);
			if (m) snprintf(v, sizeof(v), tr("After %d minutes"), m); else snprintf(v, sizeof(v), "%s", tr("Off"));
			break;
		}
		case S_DELETE: snprintf(v, sizeof(v), "%s", delete_enabled() ? tr("On") : tr("Off")); break;
		case S_ACCENT: {
			const char *an = kit_accent_name(setting_get("accent", ACCENT_BLUE));
			snprintf(v, sizeof(v), "%s", tr(an));
			break;
		}
		case S_BIN: {
			int b = setting_get("bin", 7);
			if (b > 0) snprintf(v, sizeof(v), tr("Keep %d days"), b);
			else snprintf(v, sizeof(v), "%s", b < 0 ? tr("Until it's emptied") : tr("Off: delete at once"));
			break;
		}
		}
		uint32_t vc = r == S_DELETE && delete_enabled() ? C_WARN : C_CYAN;
		int cy = kit_text_y(FONT_MEDIUM, 20, y, rh - 6);
		int vw = font_draw_right(FONT_MEDIUM, 20, rx - 22, cy, v, vc);
		if (r == set_sel) {		// the arrows: Left/Right change it
			font_draw_right(FONT_BOLD, 20, rx, cy, "\xe2\x80\xba", vc);
			font_draw_right(FONT_BOLD, 20, rx - 22 - vw - 10, cy, "\xe2\x80\xb9", vc);
		}
		if (lang_flag_i >= 0 && flags[lang_flag_i])
			gfx_image(flags[lang_flag_i], rx - 22 - vw - (r == set_sel ? 32 : 12) - 32, y + (rh - 6 - 24) / 2, 32, 24, 3, 255);
	}
	char lines[2][256];
	int hy = py + NUM_SETTINGS * rh + 26, n = kit_wrap(FONT_REGULAR, 17, tr(helps[set_sel]), LIST_W, lines, 2);
	for (int i = 0; i < n; i++) font_draw(FONT_REGULAR, 17, LIST_X, hy + i * 24, lines[i], C_TEXT2, LIST_W);
	const kit_hint_t hints[] = {{G_DPAD_LR, -1, tr("Change"), 0}, {G_DPAD_UD, -1, tr("Choose"), 1}, {G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 3, LIST_X, ui_hints_max());
	ui_draw_strip();
}

static void Draw_About(void)
{
	// as PS3 World's credits: who made it, then who it's thanks to
	static const char *const thanks[] = {
		N_("Fonts: Poppins (SIL Open Font License) and DejaVu Sans."),
		N_("Button pictures: Kenney (CC0). Flags: flag-icons (MIT)."),
		N_("The hard disk icon: \"Internal Clear\" by Rick Patrick."),
		N_("Menu icons: Tanah Basah, from Flaticon."),
		N_("Music: \"Relaxing Music (Part 1)\" by Piotr Witowski, from Pixabay."),
		N_("Pictures, fonts and music decoded with stb_image, stb_truetype and stb_vorbis (public domain)."),
		N_("NTFS drives: libntfs_ext by Estwald, from NTFS-3G (GPL-2.0)."),
		N_("Built with PSL1GHT and the ps3toolchain."),
	};
	kit_backdrop();
	kit_top(tr("About"), 0);
	int x = LIST_X - 12, w = LIST_W + 24, y = 100, h = HINT_Y - 18 - y, cx = SCREEN_W / 2, tw = LIST_W - 80;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	char ver[64], lines[3][256];
	snprintf(ver, sizeof(ver), tr("version %s"), APP_VERSION);
	int nw = font_width(FONT_BOLD, 30, APP_NAME), vw = font_width(FONT_MEDIUM, 19, ver), nx = cx - (nw + 14 + vw) / 2;
	font_draw(FONT_BOLD, 30, nx, y + 20, APP_NAME, C_TEXT, 0);
	font_draw(FONT_MEDIUM, 19, nx + nw + 14, y + 29, ver, C_CYAN, 0);
	font_draw_center(FONT_REGULAR, 18, cx, y + 64, tr("What's using your PS3's space, drive by drive, down to the last file."), C_TEXT2, LIST_W);

	font_draw_center(FONT_MEDIUM, 19, cx, y + 106, tr("Made by"), C_CYAN, 0);
	font_draw_center(FONT_BOLD, 30, cx, y + 132, "daedra000  \xc2\xb7  Claude (Anthropic)", C_TEXT, LIST_W - 40);
	gfx_rect(cx - 260, y + 186, 520, 1, C_BORDER);

	font_draw_center(FONT_MEDIUM, 19, cx, y + 202, tr("Thanks to"), C_CYAN, 0);
	int ty = y + 234;
	for (size_t i = 0; i < sizeof(thanks) / sizeof(thanks[0]); i++) {
		int n = kit_wrap(FONT_REGULAR, 17, tr(thanks[i]), tw, lines, 2);
		for (int k = 0; k < n; k++, ty += 25) font_draw_center(FONT_REGULAR, 17, cx, ty, lines[k], C_TEXT2, tw);
	}
	int n = kit_wrap(FONT_REGULAR, 16, tr("HDD Atlas is free software under the GPL-2.0. The licences are in USRDIR/data/licenses."), tw, lines, 2);
	for (int k = 0; k < n; k++) font_draw_center(FONT_REGULAR, 16, cx, y + h - 26 - (n - k) * 23, lines[k], C_DIM, tw);
	const kit_hint_t hints[] = {{G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 1, LIST_X, ui_hints_max());
	ui_draw_strip();
}

void menu_render(void)
{
	switch (screen) {
	case M_MENU: Draw_Menu(); break;
	case M_EXPORT: Draw_Menu(); Draw_Export(); break;		// (over the menu it came from)
	case M_SETTINGS: Draw_Settings(); break;
	case M_ABOUT: Draw_About(); break;
	case M_BUTTONS: Draw_Buttons(); break;
	case M_HISTORY: Draw_History(); break;
	}
}
