/*
ui_health.c: the Drive health screen (START on the overview): the internal
drive's SMART data (health.h), read on a worker, as a verdict (good, caution,
failing) with why, the drive's model and size, the six figures that matter
(temperature, hours, power cycles, reallocated, pending and uncorrectable
sectors; on an SSD its life left instead of power cycles), and every attribute.

The first time, it says what it's about to do and waits for X; once the drive
has been read here without trouble it reads straight away. If the console
froze reading it once, it says so and never asks the drive again.
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
#include "jobs.h"
#include "health.h"
#include "ui_kit.h"
#include "ui_int.h"

enum { H_ASK, H_READING, H_FROZE, H_ERROR, H_DONE };

typedef struct {
	health_t h;
	int ok;
	char why[200];
} reading_t;

static int state = H_ASK;
static job_t *job;
static reading_t got;			// the last reading (kept for the session)
static int64_t got_when;
static int scroll;				// the attribute table's first row
static int show_history;		// the table shows the readings over time, not the attributes
static int show_help;			// SELECT: what it all means (1); X on a worn or failing drive: what to do (2)
static int help_scroll, help_max;	// (help_max: from the last drawing)
static health_rec_t hist[64];	// this drive's readings, oldest first
static int nhist;

static void History_Path(char *out, int size) { snprintf(out, size, "%s/smart_history.txt", plat_save_dir()); }

// a new reading: said if a wear count grew since this drive's last one, then kept
static void History_New(const health_t *h, int64_t when)
{
	char path[256], msg[300], d[32] = "";
	History_Path(path, sizeof(path));
	nhist = health_history_load(path, h, hist, 64);
	if (nhist) {
		const health_rec_t *p = &hist[nhist - 1];
		time_t t = (time_t)p->when;
		struct tm *tm = localtime(&t);
		if (tm) strftime(d, sizeof(d), "%Y-%m-%d", tm);
		static const char *const what[] = {N_("Reallocated sectors"), N_("Pending sectors"), N_("Uncorrectable sectors")};
		int64_t was[3] = {p->reallocated, p->pending, p->uncorrectable}, now[3] = {h->reallocated, h->pending, h->uncorrectable};
		for (int k = 0; k < 3; k++)
			if (was[k] >= 0 && now[k] > was[k]) {
				snprintf(msg, sizeof(msg), tr("%s went from %lld to %lld since %s."), tr(what[k]), (long long)was[k], (long long)now[k], d);
				ui_note(msg);
				plat_log("health: %s", msg);
				break;
			}
	}
	health_history_add(path, h, when);
	nhist = health_history_load(path, h, hist, 64);
}

static void Journal_Path(char *out, int size) { snprintf(out, size, "%s/smart_journal.txt", plat_save_dir()); }

// worn sectors, an SSD nearly worn out, or a drive that says it's failing: the reading offers what to do
static int Worn(const health_t *h)
{
	return h->status == HEALTH_FAILING || h->why_kind == WHY_WORN || h->why_kind == WHY_LIFE ||
		h->reallocated > 0 || h->pending > 0 || h->uncorrectable > 0;
}

int health_known_worn(void)
{
	if (got.ok) return Worn(&got.h);		// (read this session)
	char path[256];
	health_rec_t r;
	History_Path(path, sizeof(path));
	if (!health_history_last(path, &r)) return 0;		// (the last reading kept: on this drive, the only one ever read here)
	return r.status == HEALTH_FAILING || r.reallocated > 0 || r.pending > 0 || r.uncorrectable > 0 || (r.life >= 0 && r.life <= HEALTH_LIFE_LOW);
}

static void Read_Run(job_t *j)
{
	reading_t *r = calloc(1, sizeof(*r));
	j->result = r;
	if (!r) return;
	static uint8_t identify[512], data[512], thresh[512];		// (one reading at a time: the screen waits for it)
	int have_thresh = 0;
	char journal[256];
	Journal_Path(journal, sizeof(journal));
	if (health_read_drive(journal, identify, data, thresh, &have_thresh, r->why, sizeof(r->why)) != 0) return;
	if (health_parse(identify, data, have_thresh ? thresh : NULL, &r->h) != 0) {
		snprintf(r->why, sizeof(r->why), "%s", "The drive answered, but with nothing that reads as IDENTIFY (an emulator does this).");
		return;
	}
	r->ok = 1;
}

static void Read_Cleanup(job_t *j)
{
	free(j->result);
	j->result = NULL;
}

static void Start_Reading(void)
{
	if (job) return;
	job = job_new(Read_Run, Read_Cleanup);
	if (!job) { state = H_ERROR; snprintf(got.why, sizeof(got.why), "%s", "No memory to start."); return; }
	state = H_READING;
	plat_log("health: reading the internal HDD's SMART data");
	job_post(job, LANE_API);
	ui_dirty();
}

static void Log_Reading(const health_t *h)
{
	static const char *const st[] = {"good", "caution", "failing"};
	plat_log("health: %s, firmware %s, %.1f GB, %s%s, %d attributes%s", h->model, h->firmware, h->bytes / 1e9,
		h->ssd ? (h->rotation == 1 ? "SSD" : "SSD (by its attributes)") : "HDD", h->swapped ? ", words swapped" : "", h->count, h->data_checksum_ok ? "" : " (checksum bad)");
	if (h->ssd) plat_log("health: life left %d%% (attribute %d)", h->life_left, h->life_attr);
	plat_log("health: %s: %s", st[h->status], h->why);
	plat_log("health: %d C, %lld h, %lld power cycles, reallocated %lld, pending %lld, uncorrectable %lld, CRC %lld",
		h->temperature, (long long)h->power_on_hours, (long long)h->power_cycles, (long long)h->reallocated,
		(long long)h->pending, (long long)h->uncorrectable, (long long)h->crc_errors);
}

void health_open(void)
{
	char journal[256], froze[64];
	Journal_Path(journal, sizeof(journal));
	scroll = 0;
	if (job) return;		// (still reading from last time)
	if (health_journal_froze(journal, froze, sizeof(froze))) {
		state = H_FROZE;
		snprintf(got.why, sizeof(got.why), "%s", froze);
	} else if (got.ok) {
		state = H_DONE;
		char path[256];
		History_Path(path, sizeof(path));
		nhist = health_history_load(path, &got.h, hist, 64);
	} else if (health_journal_ok_before(journal)) {
		Start_Reading();
	} else {
		state = H_ASK;
	}
	ui_dirty();
}

static int Table_Rows(void);

int health_update(uint32_t p)
{
	if (job && job_done(job)) {
		reading_t *r = job->result;
		if (r) got = *r;		// (copied before the job's let go of: its cleanup frees r)
		else { memset(&got, 0, sizeof(got)); snprintf(got.why, sizeof(got.why), "%s", "No memory for the reading."); }
		job_release(job);
		job = NULL;
		got_when = (int64_t)time(NULL);
		if (got.ok) { state = H_DONE; Log_Reading(&got.h); ui_sound(SND_OK); History_New(&got.h, got_when); }
		else if (!strncmp(got.why, "froze:", 6)) { memmove(got.why, got.why + 6, strlen(got.why + 6) + 1); state = H_FROZE; }
		else { state = H_ERROR; plat_log("health: %s", got.why); }
		ui_dirty();
	}
	if (p & BTN_SELECT) { show_help = show_help == 1 ? 0 : 1; help_scroll = 0; ui_sound(SND_MOVE); ui_dirty(); return 1; }
	if (!show_help && state == H_DONE && Worn(&got.h) && (p & BTN_CROSS)) { show_help = 2; help_scroll = 0; ui_sound(SND_OK); ui_dirty(); return 1; }
	if (show_help) {
		int old = help_scroll;
		if (p & BTN_DOWN) help_scroll++;
		if (p & BTN_UP) help_scroll--;
		if (p & (BTN_RIGHT | BTN_R2)) help_scroll += 8;
		if (p & (BTN_LEFT | BTN_L2)) help_scroll -= 8;
		if (help_scroll > help_max) help_scroll = help_max;
		if (help_scroll < 0) help_scroll = 0;
		if (help_scroll != old) { ui_sound(SND_MOVE); ui_dirty(); }
		if (p & BTN_CIRCLE) { show_help = 0; ui_sound(SND_BACK); ui_dirty(); }
		return 1;
	}
	if (state == H_ASK && (p & BTN_CROSS)) { ui_sound(SND_OK); Start_Reading(); }
	if ((state == H_DONE || state == H_ERROR) && (p & BTN_TRIANGLE)) { ui_sound(SND_OK); Start_Reading(); }
	if (state == H_DONE && (p & BTN_SQUARE)) { show_history = !show_history; scroll = 0; ui_sound(SND_MOVE); ui_dirty(); }
	if (state == H_DONE) {
		int max = (show_history ? nhist : got.h.count) - Table_Rows();
		if (max < 0) max = 0;
		int old = scroll;
		if (p & BTN_DOWN) scroll++;
		if (p & BTN_UP) scroll--;
		if (p & (BTN_RIGHT | BTN_R2)) scroll += Table_Rows();
		if (p & (BTN_LEFT | BTN_L2)) scroll -= Table_Rows();
		if (scroll > max) scroll = max;
		if (scroll < 0) scroll = 0;
		if (scroll != old) { ui_sound(SND_MOVE); ui_dirty(); }
	}
	if (p & BTN_CIRCLE) {
		ui_sound(SND_BACK);
		return 0;		// (a reading still going carries on, and is there next time)
	}
	return 1;
}

void health_shutdown(void)
{
	if (job) {
		health_quitting = 1;		// (no new call to the drive starts)
		jobs_wait_lane(LANE_API, 3000000);		// (never cut off inside a drive command)
		if (!job_done(job)) {		// (still inside one: said, or the next start would think it froze the console)
			char journal[256];
			Journal_Path(journal, sizeof(journal));
			health_journal_quit(journal);
			plat_log("health: quitting during a read of the drive (journaled: not a freeze)");
		}
		job_release(job);
		job = NULL;
	}
}

/*
==================
Drawing
==================
*/
#define TOP_Y		100
#define TOP_H		150
#define TILE_Y		(TOP_Y + TOP_H + 12)
#define TILE_H		84
#define ROW_H2		30

static int Tiles_Rows(void) { return screen_narrow() ? 2 : 1; }
static int Table_Y(void) { return TILE_Y + Tiles_Rows() * (TILE_H + 10) + 2; }
static int Table_Rows(void) { return (HINT_Y - 18 - Table_Y() - 16 - ROW_H2) / ROW_H2; }

static void Fmt_Count(int64_t v, char *out, int size)
{
	if (v < 0) snprintf(out, size, "-");
	else kit_fmt_count(v, out, size);
}

// why a reading failed, in the language on screen: the platform's reasons come in English, some with
// lv2's error code in them ("... (0x80010002)."): each is matched with its phrase and said again in it
static const char *Why_Local(const char *why, char *out, int size)
{
	static const char *const known[] = {
		N_("The journal couldn't be written in USRDIR, so the drive wasn't asked."),
		N_("lv2 wouldn't open the drive (0x%08x)."),
		N_("The drive didn't answer IDENTIFY (0x%08x)."),
		N_("The drive didn't give its SMART data (0x%08x)."),
		N_("The drive answered, but with nothing that reads as IDENTIFY (an emulator does this)."),
		N_("No memory to start."),
		N_("No memory for the reading."),
	};
	for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
		const char *f = known[k], *spec = strstr(f, "0x%08x");
		if (!spec) {
			if (!strcmp(why, f)) return tr(f);
			continue;
		}
		size_t pre = (size_t)(spec - f), post = strlen(spec + 6), wl = strlen(why);
		unsigned code;
		char *end;
		if (wl < pre + post + 3 || strncmp(why, f, pre) || strcmp(why + wl - post, spec + 6) || strncmp(why + pre, "0x", 2)) continue;
		code = (unsigned)strtoul(why + pre + 2, &end, 16);
		if (end != why + wl - post) continue;
		snprintf(out, size, tr(f), code);
		return out;
	}
	return why;		// (one it doesn't know: as it came)
}

static void Draw_Panel_Text(const char *title, const char *body, uint32_t tc)
{
	int x = LIST_X - 12, w = LIST_W + 24, y = TOP_Y, h = HINT_Y - 18 - TOP_Y;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	int cx = SCREEN_W / 2, tw = w - 160;
	char lines[10][256];
	int n = kit_wrap(FONT_REGULAR, 20, body, tw, lines, 10);
	int ty = y + h / 2 - (44 + n * 30) / 2;
	font_draw_center(FONT_BOLD, 28, cx, ty, title, tc, tw);
	for (int i = 0; i < n; i++) font_draw_center(FONT_REGULAR, 20, cx, ty + 50 + i * 30, lines[i], C_TEXT2, tw);
}

static void Tile(int x, int y, int w, const char *label, const char *value, const char *sub, uint32_t vc)
{
	gfx_round_rect(x, y, w, TILE_H, 12, C_PANEL);
	kit_draw_fit(FONT_REGULAR, 16, 13, x + 16, y + 12, label, C_DIM, w - 32);
	font_draw(FONT_BOLD, 26, x + 16, y + 34, value, vc, w - 32);
	if (sub && sub[0]) font_draw_right(FONT_REGULAR, 15, x + w - 14, y + 44, sub, C_DIM);
}

// a column's header, right-aligned at rx, made smaller (to 12 px) if it's wider than its column
static void Head_Right(int rx, int y, const char *s, int room)
{
	int p = 15;
	while (p > 12 && font_width(FONT_MEDIUM, p, s) > room) p--;
	font_draw_right(FONT_MEDIUM, p, rx, y + kit_text_y(FONT_MEDIUM, p, 0, 40) - kit_text_y(FONT_MEDIUM, 15, 0, 40), s, C_DIM);
}

static void Draw_Reading(void)
{
	const health_t *h = &got.h;
	char line[256], size[32], a[48], b[48];
	int x = LIST_X - 12, w = LIST_W + 24;
	// the verdict, why, and the drive
	gfx_round_rect(x, TOP_Y, w, TOP_H, 14, C_PANEL);
	static const char *const words[] = {N_("GOOD"), N_("CAUTION"), N_("FAILING")};
	const uint32_t colors[] = {C_OK, C_WARN, C_ERR};
	uint32_t c = colors[h->status];
	const char *word = tr(words[h->status]);
	int pw = font_width(FONT_BOLD, 26, word) + 48;
	gfx_round_rect(LIST_X + 6, TOP_Y + 20, pw, 50, 25, with_alpha(c, 0x40));
	font_draw_center(FONT_BOLD, 26, LIST_X + 6 + pw / 2, kit_text_y(FONT_BOLD, 26, TOP_Y + 20, 50), word, c, 0);
	int tx = LIST_X + 6 + pw + 20, tw = SCREEN_W - LIST_X - 6 - tx;
	char lines[2][256];
	char why[300];
	switch (h->why_kind) {
	case WHY_FAILING:
		snprintf(why, sizeof(why), tr("%s is at %d, at or below its threshold of %d: the drive says it's failing."), tr(health_attr_name(h->why_attr, h->ssd)), h->why_value, h->why_thresh);
		break;
	case WHY_PAST:
		snprintf(why, sizeof(why), tr("%s is at %d, at or below its threshold of %d."), tr(health_attr_name(h->why_attr, h->ssd)), h->why_value, h->why_thresh);
		break;
	case WHY_WORN:
		snprintf(why, sizeof(why), h->ssd ? tr("%lld %s: the flash is wearing. Keep a backup of what matters.") :
			tr("%lld %s: the surface is wearing. Keep a backup of what matters."), (long long)h->why_count, tr(health_worn_what(h->why_attr)));
		break;
	case WHY_LIFE:
		snprintf(why, sizeof(why), tr("Life left is at %d%%: the flash is nearly worn out. Keep a backup of what matters."), h->why_value);
		break;
	case WHY_HOT:
		snprintf(why, sizeof(why), tr("Running hot (%d \xc2\xb0""C). Check the console's vents."), h->why_value);
		break;
	default:
		snprintf(why, sizeof(why), "%s", tr("No reallocated, pending or uncorrectable sectors, and nothing past its threshold."));
	}
	int n = kit_wrap(FONT_MEDIUM, 19, why, tw, lines, 2);
	for (int i = 0; i < n; i++) font_draw(FONT_MEDIUM, 19, tx, TOP_Y + (n == 1 ? 34 : 22) + i * 26, lines[i], C_TEXT, tw);
	kit_fmt_size((long long)h->bytes, size, sizeof(size));
	health_mask_serial(h->serial, a, sizeof(a));
	if (h->ssd) snprintf(b, sizeof(b), "%s", tr("SSD"));
	else if (h->rotation > 1) snprintf(b, sizeof(b), tr("%d rpm"), h->rotation);
	else b[0] = 0;
	font_draw(FONT_BOLD, 22, LIST_X + 6, TOP_Y + 90, h->model, C_TEXT, LIST_W - 12);
	snprintf(line, sizeof(line), tr("%s  \xc2\xb7  %s%s%s  \xc2\xb7  firmware %s  \xc2\xb7  serial %s"), size, b, b[0] ? "  \xc2\xb7  " : "", h->ssd ? tr("internal drive") : tr("internal HDD"), h->firmware, a);
	if (!h->data_checksum_ok) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", tr("  \xc2\xb7  (the data's checksum is off)"));
	font_draw(FONT_REGULAR, 17, LIST_X + 6, TOP_Y + 120, line, C_DIM, LIST_W - 12);

	// the six figures
	int per = screen_narrow() ? 3 : 6, gap = 10, tw2 = (w - gap * (per - 1)) / per;
	char v[6][48], s[6][48];
	const char *labels[6] = {tr("Temperature"), tr("Powered on"), h->ssd ? tr("Life left") : tr("Power cycles"), tr("Reallocated"), tr("Pending"), tr("Uncorrectable")};
	uint32_t vc[6];
	for (int i = 0; i < 6; i++) { s[i][0] = 0; vc[i] = C_TEXT; }
	if (h->temperature >= 0) snprintf(v[0], sizeof(v[0]), "%d \xc2\xb0""C", h->temperature); else snprintf(v[0], sizeof(v[0]), "-");
	if (health_hot(h)) vc[0] = C_WARN;
	Fmt_Count(h->power_on_hours, a, sizeof(a));
	snprintf(v[1], sizeof(v[1]), h->power_on_hours >= 0 ? tr("%s h") : "%s", a);
	if (h->power_on_hours >= 48) snprintf(s[1], sizeof(s[1]), tr("%lld days"), (long long)(h->power_on_hours / 24));
	if (!h->ssd) Fmt_Count(h->power_cycles, v[2], sizeof(v[2]));
	else if (h->life_left < 0) { snprintf(v[2], sizeof(v[2]), "-"); vc[2] = C_DIM; }
	else {		// (an SSD: its own estimate of the wear its flash has left; power cycles are in the table)
		snprintf(v[2], sizeof(v[2]), "%d%%", h->life_left);
		vc[2] = h->life_left <= HEALTH_LIFE_LOW ? C_WARN : C_OK;
	}
	int64_t bad[3] = {h->reallocated, h->pending, h->uncorrectable};
	for (int k = 0; k < 3; k++) {
		Fmt_Count(bad[k], v[3 + k], sizeof(v[3 + k]));
		snprintf(s[3 + k], sizeof(s[3 + k]), "%s", tr("sectors"));
		vc[3 + k] = bad[k] > 0 ? C_WARN : bad[k] == 0 ? C_OK : C_DIM;
	}
	for (int i = 0; i < 6; i++) {
		int row = i / per, col = i % per;
		Tile(x + col * (tw2 + gap), TILE_Y + row * (TILE_H + 10), tw2, labels[i], v[i], s[i], vc[i]);
	}

	// every attribute, or the readings over time
	int ty = Table_Y(), th = HINT_Y - 18 - ty, rows = Table_Rows();
	if (show_history) {
		gfx_round_rect(x, ty, w, th, 14, C_PANEL);
		int hy = ty + 10, cols = 6;
		static const char *const heads[] = {N_("Read on"), N_("Powered on"), N_("Temperature"), N_("Reallocated"), N_("Pending"), N_("Uncorrectable")};
		int cw = (LIST_W - 160) / (cols - 1);
		font_draw(FONT_MEDIUM, 15, LIST_X + 6, hy, tr(heads[0]), C_DIM, 0);
		for (int c = 1; c < cols; c++)		// (an SSD: its life left, not the temperature)
			font_draw_right(FONT_MEDIUM, 15, LIST_X + 160 + c * cw - 10, hy, c == 2 && h->ssd ? tr("Life left") : tr(heads[c]), C_DIM);
		for (int r = 0; r < rows && scroll + r < nhist; r++) {
			const health_rec_t *e = &hist[nhist - 1 - scroll - r], *older = nhist - 2 - scroll - r >= 0 ? &hist[nhist - 2 - scroll - r] : NULL;		// (newest first)
			int y = hy + ROW_H2 + r * ROW_H2 - 4;
			if (r % 2 == 0) gfx_rect(x + 8, y - 4, w - 16, ROW_H2, 0x0affffff);
			char t[48] = "-";
			time_t tt = (time_t)e->when;
			struct tm *tm = localtime(&tt);
			if (tm) strftime(t, sizeof(t), "%Y-%m-%d %H:%M", tm);
			font_draw(FONT_REGULAR, 17, LIST_X + 6, y, t, C_TEXT2, 150);
			int64_t v[5] = {e->hours, h->ssd ? e->life : e->temperature, e->reallocated, e->pending, e->uncorrectable};
			int64_t o[5] = {older ? older->hours : -1, -1, older ? older->reallocated : -1, older ? older->pending : -1, older ? older->uncorrectable : -1};
			for (int c = 0; c < 5; c++) {
				if (v[c] < 0) snprintf(t, sizeof(t), "-");
				else if (c == 1 && h->ssd) snprintf(t, sizeof(t), "%lld%%", (long long)v[c]);
				else if (c == 1) snprintf(t, sizeof(t), "%lld \xc2\xb0""C", (long long)v[c]);
				else kit_fmt_count(v[c], t, sizeof(t));
				uint32_t col = c >= 2 && older && o[c] >= 0 && v[c] > o[c] ? C_WARN : C_TEXT2;		// (a wear count that grew)
				font_draw_right(FONT_REGULAR, 17, LIST_X + 160 + (c + 1) * cw - 10, y, t, col);
			}
		}
		if (!nhist) font_draw(FONT_REGULAR, 17, LIST_X + 6, hy + ROW_H2, tr("No readings kept yet."), C_DIM, 0);
		return;
	}
	gfx_round_rect(x, ty, w, th, 14, C_PANEL);
	int narrow = screen_narrow();
	int cx_id = LIST_X + 6, cx_name = LIST_X + 56, rx_raw = SCREEN_W - LIST_X - 10;
	// (Worst and Threshold get their headers' room, in every language: the name has room to spare, a raw count doesn't)
	int rx_th = rx_raw - (narrow ? 150 : 210), rx_worst = rx_th - (narrow ? 80 : 100), rx_val = rx_worst - (narrow ? 80 : 100);
	int hy = ty + 10;
	font_draw(FONT_MEDIUM, 15, cx_id, hy, "ID", C_DIM, 0);
	font_draw(FONT_MEDIUM, 15, cx_name, hy, tr("Attribute"), C_DIM, 0);
	Head_Right(rx_val, hy, tr("Value"), rx_val - 70 - cx_name);
	Head_Right(rx_worst, hy, tr("Worst"), rx_worst - rx_val - 10);
	Head_Right(rx_th, hy, narrow ? tr("Thresh.") : tr("Threshold"), rx_th - rx_worst - 10);
	Head_Right(rx_raw, hy, tr("Raw"), rx_raw - rx_th - 10);
	for (int r = 0; r < rows && scroll + r < h->count; r++) {
		const health_attr_t *at = &h->a[scroll + r];
		int y = hy + ROW_H2 + r * ROW_H2 - 4;
		int past = at->thresh && at->thresh != 0xFE && at->value <= at->thresh;
		if (r % 2 == 0) gfx_rect(x + 8, y - 4, w - 16, ROW_H2, 0x0affffff);
		uint32_t tc = past ? (at->flags & 1 ? C_ERR : C_WARN) : C_TEXT2;
		char t[32];
		snprintf(t, sizeof(t), "%u", at->id);
		font_draw(FONT_REGULAR, 17, cx_id, y, t, C_DIM, 0);
		font_draw(FONT_MEDIUM, 17, cx_name, y, tr(health_attr_name(at->id, h->ssd)), tc, rx_val - 70 - cx_name);
		if (at->flags & 1) font_draw_right(FONT_REGULAR, 13, rx_val - 52, y + 3, tr("pre-fail"), C_DIM);
		snprintf(t, sizeof(t), "%u", at->value);
		font_draw_right(FONT_REGULAR, 17, rx_val, y, t, tc);
		snprintf(t, sizeof(t), "%u", at->worst);
		font_draw_right(FONT_REGULAR, 17, rx_worst, y, t, C_TEXT2);
		if (h->have_thresh) snprintf(t, sizeof(t), "%u", at->thresh); else snprintf(t, sizeof(t), "-");
		font_draw_right(FONT_REGULAR, 17, rx_th, y, t, C_TEXT2);
		if (at->id == 194 || at->id == 190) snprintf(t, sizeof(t), "%u \xc2\xb0""C", (unsigned)(at->raw & 0xff));
		else kit_fmt_count((long long)at->raw, t, sizeof(t));
		font_draw_right(FONT_REGULAR, 17, rx_raw, y, t, C_TEXT2);
	}
	if (h->count > rows) {
		int bh = (th - 20) * rows / h->count, by = ty + 10 + (int)((long long)(th - 20 - bh) * scroll / (h->count - rows));
		gfx_round_rect(x + w - 6, by, 4, bh, 2, 0x99ffffff);
	}
}

// what it all means: a heading, then its paragraph, for each part of the screen
static const char *const help_text[] = {
	N_("The verdict"),
	N_("Good: no worn sectors, nothing past its threshold, and not running hot. "
	"Caution: the drive works, but some sectors have worn out, an old-age attribute is past its threshold, or it's above 55 \xc2\xb0""C: keep a backup of what matters. "
	"Failing: a pre-fail attribute is at or below its threshold, so the drive itself expects to fail soon: copy off what you want to keep."),
	N_("The six figures"),
	N_("Temperature: under 50 \xc2\xb0""C is comfortable; above 55 \xc2\xb0""C, check the console's vents and dust. "
	"Powered on: the hours the drive has run in its life. Power cycles: how many times it's been switched on."),
	N_("Bad sectors"),
	N_("Reallocated: sectors that went bad and were swapped for spares. "
	"Pending: sectors the drive couldn't read, waiting to be rewritten or swapped. "
	"Uncorrectable: reads that failed even after error correction, so what was there may be lost. "
	"On a healthy drive all three are 0. A few isn't fatal, but a count that keeps growing is the surest sign a drive is on its way out."),
	N_("The table"),
	N_("The drive scores each attribute. Value is the score now (usually 100 or 200 when new; lower is worse), Worst is the lowest it's ever been, "
	"and Threshold is the maker's limit: at or below it, that attribute has failed. "
	"Raw is the drive's own count behind the score (hours, sectors, degrees). What it means differs from maker to maker, "
	"so compare a drive with its own earlier readings, not with another drive."),
	N_("Pre-fail and old-age"),
	N_("The drive marks every attribute as one or the other. "
	"Pre-fail attributes (marked pre-fail in the table) watch for the faults that come before a breakdown: bad sectors, slow spin-up, seek errors. "
	"One at or below its threshold means the drive expects to fail soon. "
	"The rest are old-age attributes: they follow ordinary wear over the years, like hours and power cycles. "
	"One past its threshold means the drive is worn, not that it's about to stop."),
	N_("SSDs"),
	N_("An SSD shows its life left in place of power cycles: its own estimate of how much of its flash's rated wear is left, 100% when new. "
	"At 10% or under the verdict says Caution. On an SSD, reallocated counts worn-out blocks of flash swapped for spares, and the verdict waits for 70 \xc2\xb0""C, as SSDs run warmer. "
	"A failing SSD can stop answering instead of reporting errors: if a scan freezes at the same place every time, suspect the drive even when SMART looks good."),
	N_("History"),
	N_("Every reading is kept: Square shows them, newest first. A reallocated, pending or uncorrectable count that grew since the reading before shows in yellow. "
	"Read the drive now and then (Triangle) so there's something to compare."),
	N_("It only reads"),
	N_("SMART is the drive's own record: HDD Atlas only asks for it, and nothing on the drive changes. "
	"It can't mend a worn drive: if it's failing, the answer is a backup and a new drive."),
};

/*
What to do when the drive's wearing out (X on such a reading): HDD Atlas runs only on PS3HEN or CFW, so
it's the homebrew way, not Sony's. File by file first (one unreadable file loses only itself; Backup
Utility's one image of everything can fail on it, takes hours of reading the failing disk, and keeps no
trophies); a PC can't copy the drive (it's encrypted to this console). The swap itself, step by step:
ps3.world's guide (only named: HDD Atlas has no network).
*/
static const char *const todo_text[] = {
	N_("1. Copy what matters first"),
	N_("Your saves are in /dev_hdd0/home: copy them to a USB drive or a PC with webMAN's FTP or multiMAN. "
	"With Apollo Save Tool, export your licences (RAPs), act.dat and trophies too. Keep it all off the console."),
	N_("2. Delete what you don't need"),
	N_("Old disc images, leftover packages, caches: then there's less to copy, and less to read from a failing drive. "
	"Deleting a file hardly reads it."),
	N_("3. Don't scan again and again"),
	N_("A scan reads every folder on the drive. Where the drive can't read, a scan can stop (HDD Atlas gives up after 5 minutes), "
	"and a drive that keeps retrying wears faster."),
	N_("4. Replace the drive"),
	N_("A worn drive can't be mended, and it only gets worse. The PS3's Backup Utility is only a second safety net: it keeps no trophies, "
	"takes hours, and can fail on a damaged file. Copying the drive on a PC doesn't work: it's encrypted to this console. "
	"The whole swap, step by step: ps3.world/upgrade-storage"),
};

static void Draw_Help(void)
{
	static char lines[120][256];
	static unsigned char head[120];
	int x = LIST_X - 12, w = LIST_W + 24, y = TOP_Y, h = HINT_Y - 18 - TOP_Y, tw = w - 72, n = 0;
	const char *const *text = show_help == 2 ? todo_text : help_text;
	size_t count = show_help == 2 ? sizeof(todo_text) / sizeof(todo_text[0]) : sizeof(help_text) / sizeof(help_text[0]);
	for (size_t k = 0; k < count && n < 110; k += 2) {
		if (n) { lines[n][0] = 0; head[n++] = 0; }		// (a gap between parts)
		snprintf(lines[n], 256, "%s", tr(text[k]));
		head[n++] = 1;
		int m = kit_wrap(FONT_REGULAR, 18, tr(text[k + 1]), tw, lines + n, 110 - n);
		for (int i = 0; i < m; i++) head[n + i] = 0;
		n += m;
	}
	const int lh = 27, rows = (h - 76) / lh;
	help_max = n > rows ? n - rows : 0;
	if (help_scroll > help_max) help_scroll = help_max;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	font_draw(FONT_BOLD, 24, x + 36, y + 16, show_help == 2 ? tr("If the drive is wearing out") : tr("What it means"), C_TEXT, tw);
	for (int r = 0; r < rows && help_scroll + r < n; r++) {
		int i = help_scroll + r, ly = y + 60 + r * lh;
		if (head[i]) font_draw(FONT_BOLD, 19, x + 36, ly, lines[i], C_CYAN, tw);
		else font_draw(FONT_REGULAR, 18, x + 36, ly, lines[i], C_TEXT2, tw);
	}
	if (help_max) {
		int track = h - 76, bh = track * rows / n, by = y + 60 + (int)((long long)(track - bh) * help_scroll / help_max);
		gfx_round_rect(x + w - 10, by, 4, bh, 2, 0x99ffffff);
	}
}

void health_render(void)
{
	char line[400];
	kit_backdrop();
	kit_top(tr("Drive health"), 0);
	if (state == H_DONE) {
		char when[48] = "";
		time_t t = (time_t)got_when;
		struct tm *tm = got_when > 0 ? localtime(&t) : NULL;
		if (tm) strftime(when, sizeof(when), "%H:%M", tm);
		snprintf(line, sizeof(line), tr("SMART, read at %s"), when);
		font_draw_right(FONT_REGULAR, 17, SCREEN_W - LIST_X, 44, line, C_DIM);
	}
	int x = LIST_X;
	if (show_help) {
		Draw_Help();
		const kit_hint_t hh[] = {
			{G_DPAD_UD, -1, help_max ? tr("Scroll") : NULL, 1},
			{G_CIRCLE, -1, tr("Back"), 0},
		};
		kit_hints(hh, 2, x, ui_hints_max());
		ui_draw_strip();
		return;
	}
	switch (state) {
	case H_ASK:
		Draw_Panel_Text(tr("Read the internal HDD's health?"),
			tr("HDD Atlas will ask the drive for its SMART data: its own count of hours, errors and worn-out sectors. "
			"It only reads: nothing on the drive changes.\n\nIt asks through the PS3's storage syscalls, which haven't been "
			"tried on every console. If it ever freezes this one, HDD Atlas "
			"notices on the next start and won't ask the drive again."), C_TEXT);
		break;
	case H_READING:
		Draw_Panel_Text(tr("Reading the drive's health..."), tr("Asking the drive for its SMART data."), C_TEXT);
		break;
	case H_FROZE:
		snprintf(line, sizeof(line), tr("Reading the drive's health froze this console last time (at \"%s\"), so HDD Atlas "
			"won't ask the drive again.\n\nTo try anyway, delete smart_journal.txt in /dev_hdd0/game/HDAT00001/USRDIR."), got.why);
		Draw_Panel_Text(tr("Not read: it froze the console before"), line, C_WARN);
		break;
	case H_ERROR:
		Draw_Panel_Text(tr("The drive's health couldn't be read"), Why_Local(got.why, line, sizeof(line)), C_WARN);
		break;
	default:
		Draw_Reading();
		break;
	}
	const kit_hint_t hints[] = {
		{G_CROSS, -1, state == H_ASK ? tr("Read") : state == H_DONE && Worn(&got.h) ? tr("What to do") : NULL, 0},
		{G_TRIANGLE, -1, state == H_ERROR ? tr("Try again") : state == H_DONE ? tr("Read again") : NULL, 0},
		{G_SQUARE, -1, state == H_DONE ? (show_history ? tr("Attributes") : tr("History")) : NULL, 1},
		{G_SELECT, -1, state != H_READING ? tr("What it means") : NULL, 1},
		{G_DPAD_UD, -1, state == H_DONE && (show_history ? nhist : got.h.count) > Table_Rows() ? tr("Scroll") : NULL, 2},
		{G_CIRCLE, -1, tr("Back"), 0},
	};
	kit_hints(hints, sizeof(hints) / sizeof(hints[0]), x, ui_hints_max());
	ui_draw_strip();
}
