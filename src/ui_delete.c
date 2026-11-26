/*
ui_delete.c: deleting a row, and the lock that allows it (Phase 4; del.h has
the rules). Both are panels over whatever's on screen.

  the lock   deleting starts off. SELECT + START on the overview turns it
             on, after a panel saying what it can and can't delete (X to
             allow); the same again turns it off at once. Kept in settings.
  delete     START on a row the rules allow: a panel with what it is, where,
             its size and files, and the free space before and after.
             Cancel is picked; Right picks Delete, and X has to be HELD for
             two seconds (a bar fills; letting go early stops it). Then it
             runs on a worker with a progress bar: Circle stops it between
             files, and the panel says what was left. Afterwards the scan
             forgets what went (no rescan), is saved, and the delete is
             written to USRDIR/deletions.log.

One row at a time: there's no way to pick several.
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
#include "fsx.h"
#include "del.h"
#include "bin.h"
#include "scan.h"
#include "settings.h"
#include "ui_kit.h"
#include "ui_int.h"

#define HOLD_US		2000000		// X held this long deletes

enum { P_NONE, P_LOCK, P_CONFIRM, P_RUNNING, P_DONE };

static int panel;
static int pick_delete;			// Delete picked (not Cancel)
static uint64_t hold_from;		// when X went down on Delete (0: it isn't down)
static job_t *job;
static del_job_t *d;			// the job's (it frees it); kept here once it's done
static char name[FSX_NAME];
static int kind, drive;
static uint64_t bytes, twin_bytes, free_before;
static uint32_t files;
static char done_msg[300];
static const char *done_title = "";
static int to_bin;					// this delete's a move to the recycle bin (Settings)
static bin_entry_t bin_e;			// and its entry (its folder: bin_entry, under the drive)
static char bin_entry[FSX_PATH];
static char same_as_next[FSX_PATH];	// Identical files: the copy the next delete_open's row must be the same as
static int empty_all;				// the recycle bin emptied, every entry (a job of its own)
static bin_entry_t *empty_e;
static int empty_n;

int delete_enabled(void) { return setting_get("delete", 0); }
int delete_active(void) { return panel != P_NONE; }

void delete_lock_toggle(void)
{
	if (delete_enabled()) {
		setting_set("delete", 0);
		plat_log("delete: turned off");
		ui_note(tr("Deleting is off."));
		return;
	}
	panel = P_LOCK;
	ui_dirty();
}

static void Log_Line(const char *how);
static void Log_Entry(const del_job_t *dj, const bin_entry_t *e);

static void Run(job_t *j)
{
	del_job_t *dj = (del_job_t *)j->arg;
	if (empty_all) {		// every entry of the bin, one after the other (each its own del_run)
		uint32_t files = 0, dirs = 0;
		uint64_t bytes = 0;
		int worst = 0;
		for (int i = 0; i < empty_n && !dj->cancel; i++) {
			if (!bin_entry_rel(dj->root, empty_e[i].id, dj->rel, sizeof(dj->rel))) continue;
			dj->is_dir = 1;
			dj->twin[0] = 0;
			del_run(dj);
			Log_Entry(dj, &empty_e[i]);		// (each its own line, by what it was: not one line for them all)
			files += dj->files_done;
			dirs += dj->dirs_done;
			bytes += dj->bytes_done;
			if (dj->result == 0) bin_remove(dj->root, empty_e[i].id);
			else if (dj->result < 0 && !worst) worst = dj->result;
			dj->files_done = files;		// (the running total, for the bar)
			dj->bytes_done = bytes;
		}
		dj->files_done = files;
		dj->dirs_done = dirs;
		dj->bytes_done = bytes;
		dj->result = worst ? worst : dj->cancel ? 1 : 0;
		return;
	}
	// a leftover install: not one an install is still writing (the same twice, 3 seconds apart)
	if (kind == DEL_LEFTOVER && del_settled(dj, 3000000) != 0) return;
	// Identical files: the same as a copy that stays, every byte, before anything goes
	if (dj->same_as[0] && del_same_check(dj) != 0) return;
	if (to_bin) {		// (its line first: a crash part way leaves a line bin_tidy drops, never a lost entry)
		bin_e.when = (int64_t)time(NULL);
		bin_e.bytes = dj->bytes_total;
		bin_e.files = dj->files_total + (dj->twin[0] ? 1 : 0);
		bin_e.is_dir = dj->is_dir;
		snprintf(bin_e.rel, sizeof(bin_e.rel), "%s", dj->rel);
		snprintf(bin_e.twin, sizeof(bin_e.twin), "%s", dj->twin);
		if (!bin_add(dj->root, &bin_e)) { plat_log("bin: its list couldn't be written: not moved"); dj->result = -1; return; }
		if (del_bin_move(dj, bin_entry) != 0) bin_remove(dj->root, bin_e.id);
	} else del_run(dj);
}
static void Cleanup(job_t *j) { free(j->arg); j->arg = NULL; }

void delete_same_as(const char *other_rel) { snprintf(same_as_next, sizeof(same_as_next), "%s", other_rel ? other_rel : ""); }

int delete_open(const char *rel, int is_dir, const char *shown_name, uint64_t size, uint32_t nfiles)
{
	char why[160], same_as[FSX_PATH];
	snprintf(same_as, sizeof(same_as), "%s", same_as_next);		// (this delete's only: never the next one's)
	same_as_next[0] = 0;
	if (ui_drive_hung(cur_drive)) { ui_hung_note(); return 0; }
	if (!delete_enabled()) {
		ui_note(tr("Deleting is off: turn it on in Settings (START on the overview)."));
		return 0;
	}
	if (plat_read_only(DRV->root)) {		// (an NTFS drive mounted read-only: nothing's written to it)
		ui_note(tr("This NTFS drive is read-only: Windows didn't close it cleanly."));
		ui_sound(SND_BACK);
		return 0;
	}
	int k = del_kind(DRV->root, rel, is_dir, why, sizeof(why));
	if (k == DEL_NO) {
		ui_note(why[0] ? tr(why) : tr("This can't be deleted here."));
		ui_sound(SND_BACK);
		return 0;
	}
	if (job) return 0;		// (one at a time)
	if (k == DEL_LEFTOVER && del_installing(DRV->root)) {		// (PS3HEN installing a package now: it may be this one)
		ui_note(tr("A package is being installed: delete this once that's done."));
		ui_sound(SND_BACK);
		return 0;
	}
	if (ui_scanning()) {		// (a scan going on would count what's being deleted, or trip over it)
		ui_note(tr("Wait for the scan to finish, then delete."));
		return 0;
	}
	d = calloc(1, sizeof(*d));
	if (!d) return 0;
	snprintf(d->root, sizeof(d->root), "%s", DRV->root);
	snprintf(d->rel, sizeof(d->rel), "%s", rel);
	snprintf(d->same_as, sizeof(d->same_as), "%s", is_dir ? "" : same_as);
	d->is_dir = is_dir;
	d->files_total = nfiles;
	d->bytes_total = size;
	empty_all = 0;
	// with the recycle bin on, it's moved there (its own entries are deleted for good)
	to_bin = k != DEL_BIN && bin_days() != 0;
	if (to_bin) {
		memset(&bin_e, 0, sizeof(bin_e));
		if (!bin_new_entry(DRV->root, bin_e.id, sizeof(bin_e.id), bin_entry, sizeof(bin_entry))) to_bin = 0;
	}
	twin_bytes = 0;
	if (del_twin(d->root, rel, d->twin, sizeof(d->twin))) {
		char abs[FSX_PATH];
		int type;
		snprintf(abs, sizeof(abs), "%s/%s", d->root, d->twin);
		if (fsx_stat(abs, &type, &twin_bytes) != 0) twin_bytes = 0;
	}
	snprintf(name, sizeof(name), "%s", shown_name);
	kind = k;
	drive = cur_drive;
	bytes = size + twin_bytes;
	d->bytes_total = bytes;
	files = nfiles + (d->twin[0] ? 1 : 0);
	uint64_t t, k2;
	free_before = 0;
	fsx_space(DRV->root, &t, &free_before, &k2, why, sizeof(why));
	pick_delete = 0;
	hold_from = 0;
	panel = P_CONFIRM;
	ui_sound(SND_OK);
	ui_dirty();
	return 1;
}

// the whole recycle bin of the drive on screen, for good
int delete_open_empty_bin(void)
{
	if (ui_drive_hung(cur_drive)) { ui_hung_note(); return 0; }
	if (!delete_enabled()) { ui_note(tr("Deleting is off: turn it on in Settings (START on the overview).")); return 0; }
	if (plat_read_only(DRV->root)) { ui_note(tr("This NTFS drive is read-only: Windows didn't close it cleanly.")); return 0; }
	if (job) return 0;
	if (ui_scanning()) { ui_note(tr("Wait for the scan to finish, then delete.")); return 0; }
	free(empty_e);
	empty_n = bin_load(DRV->root, &empty_e);
	if (!empty_n) { ui_note(tr("The recycle bin's empty.")); return 0; }
	d = calloc(1, sizeof(*d));
	if (!d) return 0;
	snprintf(d->root, sizeof(d->root), "%s", DRV->root);
	bin_rel(DRV->root, d->rel, sizeof(d->rel));
	d->is_dir = 1;
	empty_all = 1;
	to_bin = 0;
	kind = DEL_BIN;
	drive = cur_drive;
	bytes = 0;
	files = 0;
	for (int i = 0; i < empty_n; i++) { bytes += empty_e[i].bytes; files += empty_e[i].files; }
	d->files_total = files;
	d->bytes_total = bytes;
	snprintf(name, sizeof(name), "%s", tr("The recycle bin"));
	uint64_t t, k2;
	char how[64];
	free_before = 0;
	fsx_space(DRV->root, &t, &free_before, &k2, how, sizeof(how));
	pick_delete = 0;
	hold_from = 0;
	panel = P_CONFIRM;
	ui_sound(SND_OK);
	ui_dirty();
	return 1;
}

static void Start(void)
{
	job = job_new(Run, Cleanup);
	if (!job) {
		done_title = N_("Not deleted");
		snprintf(done_msg, sizeof(done_msg), "%s", tr("No memory to start."));
		free(d);		// (never posted: still ours)
		d = NULL;
		panel = P_DONE;
		ui_dirty();
		return;
	}
	job->arg = d;		// (the job frees it, whenever both sides are done)
	panel = P_RUNNING;
	job_post(job, LANE_DL);		// (its own lane: a folder can take a while, and listings go on)
	ui_dirty();
}

// the delete's line in USRDIR/deletions.log (what it was, what went so far, how it ended)
static void Log_Line(const char *how)
{
	char line[1400], when[32];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	when[0] = 0;
	if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
	snprintf(line, sizeof(line), "%s  %s/%s%s%s  %llu bytes  %u files  %s", when, d->root, d->rel, d->twin[0] ? " + " : "", d->twin,
		(unsigned long long)d->bytes_done, (unsigned)d->files_done, how);
	char log[256];
	snprintf(log, sizeof(log), "%s/deletions.log", plat_save_dir());
	plat_append_line(log, line);
}

// an entry of the bin emptied, as it goes: where it is, what it was
static void Log_Entry(const del_job_t *dj, const bin_entry_t *e)
{
	char line[1400], when[32];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	when[0] = 0;
	if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
	snprintf(line, sizeof(line), "%s  %s/%s (%s%s%s)  %llu bytes  %u files  %s", when, dj->root, dj->rel, e->rel, e->twin[0] ? " + " : "", e->twin,
		(unsigned long long)dj->bytes_done, (unsigned)dj->files_done,
		dj->result == 0 ? "deleted from the recycle bin" : dj->result > 0 ? "partly" : "failed");
	char log[256];
	snprintf(log, sizeof(log), "%s/deletions.log", plat_save_dir());
	plat_append_line(log, line);
}

// the job's done: said, logged, and the scan follows (quitting: no list to follow, nothing said)
static void Finished(int quitting)
{
	char size[32];
	uint64_t t, fr = 0, k;
	char how[64];
	fsx_space(d->root, &t, &fr, &k, how, sizeof(how));
	kit_fmt_size((long long)d->bytes_done, size, sizeof(size));
	done_title = d->result == 0 ? N_("Deleted") : d->result > 0 ? N_("Stopped part way") : N_("Not deleted");
	const char *other = strrchr(d->err_path, '/');
	other = other ? other + 1 : d->err_path;
	if (d->result == DEL_NOT_SAME)		// (Identical files: checked every byte first, and it wasn't)
		snprintf(done_msg, sizeof(done_msg), tr("Not deleted: it isn't the same as %s, byte for byte."), other);
	else if (d->result == DEL_BUSY)		// (a leftover install still changing: an install may be writing to it)
		snprintf(done_msg, sizeof(done_msg), "%s", tr("Not deleted: it's still changing, so an install may be writing to it. Try again later."));
	else if (d->result == DEL_RECENT)		// (an _INST_ written to in the last day)
		snprintf(done_msg, sizeof(done_msg), "%s", tr("Not deleted: an install wrote to it in the last day and may still need it. Try again tomorrow."));
	else if (d->result == DEL_NO_OTHER)
		snprintf(done_msg, sizeof(done_msg), tr("Not deleted: the other copy (%s) isn't there any more, so this is the last one."), other);
	else if (to_bin && d->result == 0) {		// (moved: nothing's freed till it's emptied)
		done_title = N_("In the recycle bin");
		snprintf(done_msg, sizeof(done_msg), tr("Moved to the recycle bin (%s). It's freed when the bin's emptied: the menu's Recycle bin."), size);
	} else if (d->result == 0)
		snprintf(done_msg, sizeof(done_msg), tr("Deleted: %s freed."), size);
	else if (d->result > 0 && d->cancel)		// (each message its own call: their conversions differ)
		snprintf(done_msg, sizeof(done_msg), tr("Stopped part way: %s freed, the rest is still there."), size);
	else if (d->result > 0)
		snprintf(done_msg, sizeof(done_msg), tr("Deleted, but %u entries were left (links, or ones that wouldn't go): %s freed."),
			(unsigned)d->left, size);
	else
		snprintf(done_msg, sizeof(done_msg), tr("Not deleted: error 0x%08x at %s. %s freed."), (unsigned)d->result,
			d->err_path[0] ? d->err_path : d->rel, size);
	if (!empty_all)		// (the bin emptied: each entry's line was written as it went)
		Log_Line(to_bin ? (d->result == 0 ? "to the recycle bin" : "not moved to the recycle bin") : d->result == 0 ? "deleted" : d->result > 0 ? "partly" : "failed");
	if (kind == DEL_BIN) {		// (the bin's own: its line goes; the scan didn't count it where it is)
		if (!empty_all && d->result == 0) {
			const char *id = strrchr(d->rel, '/');
			if (id) bin_remove(d->root, id + 1);
		}
		empty_all = 0;
		free(empty_e);
		empty_e = NULL;
		empty_n = 0;
		ui_bin_counted(drive, -(int64_t)d->bytes_done, -(int32_t)d->files_done);		// (the scan's Recycle bin row: what went for good)
		if (!quitting) list_after_delete();
		if (quitting) return;
		ui_sound(d->result == 0 ? SND_OK : SND_BACK);
		panel = P_DONE;
		return;
	}
	// the scan forgets what went, and is saved (what went is gone, whether or not all of it did;
	// a folder of only folders goes with no files at all)
	scan_t *s = drives[drive].shown;
	int whole = d->is_dir && d->result == 0;		// (the folder and everything under it: gone)
	if (s && (d->files_done || d->dirs_done)) {
		if (d->twin[0] && d->files_done > 1 && d->result == 0) {		// (the row, then its twin: each its own)
			uint64_t tb = twin_bytes < d->bytes_done ? twin_bytes : d->bytes_done;
			scan_forget(s, d->twin, tb, 1, 0, 0);
			scan_forget(s, d->rel, d->bytes_done - tb, d->files_done - 1, 0, fr);
		} else {
			scan_forget(s, d->rel, d->bytes_done, d->files_done, whole, fr);
		}
		// moved to the recycle bin: what left its place is the bin's now (ui_bin_counted saves too)
		if (to_bin && d->result == 0) ui_bin_counted(drive, (int64_t)d->bytes_done, (int32_t)d->files_done);
		char cache[256];
		scan_cache_path(s->root, cache, sizeof(cache));
		scan_save(s, cache);
		if (!quitting) list_after_delete();
	}
	if (quitting) return;
	ui_sound(d->result == 0 ? SND_OK : SND_BACK);
	panel = P_DONE;
}

void delete_update(uint32_t p)
{
	uint32_t held = input_held();
	if (panel == P_LOCK) {
		if (p & BTN_CROSS) {
			setting_set("delete", 1);
			plat_log("delete: turned on");
			ui_note(tr("Deleting is on: START on a row that can go."));
			ui_sound(SND_OK);
			panel = P_NONE;
		} else if (p & BTN_CIRCLE) {
			ui_sound(SND_BACK);
			panel = P_NONE;
		}
		ui_dirty();
		return;
	}
	if (panel == P_CONFIRM) {
		if (p & (BTN_LEFT | BTN_RIGHT)) { pick_delete = !!(p & BTN_RIGHT); hold_from = 0; ui_sound(SND_MOVE); }
		if ((p & BTN_CIRCLE) || ((p & BTN_CROSS) && !pick_delete)) {
			ui_sound(SND_BACK);
			free(d);
			d = NULL;
			panel = P_NONE;
		} else if (pick_delete && (held & BTN_CROSS)) {
			if (!hold_from) hold_from = plat_time_us();
			else if (plat_time_us() - hold_from >= HOLD_US) { hold_from = 0; Start(); }
		} else {
			hold_from = 0;
		}
		ui_dirty();		// (the bar fills while X is held)
		return;
	}
	if (panel == P_RUNNING) {
		if ((p & BTN_CIRCLE) && d) d->cancel = 1;
		if (job_done(job)) {
			Finished(0);
			job_release(job);		// (frees d)
			job = NULL;
			d = NULL;
		}
		ui_dirty();
		return;
	}
	if (panel == P_DONE && (p & (BTN_CROSS | BTN_CIRCLE))) {
		panel = P_NONE;
		ui_dirty();
	}
}

void delete_shutdown(void)
{
	if (job) {
		d->cancel = 1;
		jobs_wait_lane(LANE_DL, 3000000);		// (stops between files)
		// quitting part way: logged as every delete is, and the scan saved with what went
		if (job_done(job)) Finished(1);
		else {		// (still inside a call to the drive after the wait: what went so far, logged)
			Log_Line("interrupted (the app quit while deleting)");
			plat_log("delete: %s/%s: quit while deleting, %u files gone", d->root, d->rel, (unsigned)d->files_done);
		}
		job_release(job);		// (frees d, whenever both sides are done)
		job = NULL;
	} else {
		free(d);
	}
	d = NULL;
	panel = P_NONE;
}

/*
==================
Drawing
==================
*/
static void Box(int w, int h, int *x, int *y)
{
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xb0000000);
	gfx_rect(0, HINT_Y - 14, SCREEN_W, SCREEN_H - HINT_Y + 14, 0xf205070d);		// (the screen's own hints: the panel's go here)
	*x = (SCREEN_W - w) / 2;
	*y = (SCREEN_H - h) / 2 - 10;
	gfx_round_rect(*x - 1, *y - 1, w + 2, h + 2, 19, C_BORDER);
	gfx_round_rect(*x, *y, w, h, 18, 0xf20a0e18);
}

static int Lines(int x, int y, int w, int px, const char *text, uint32_t c, int max)
{
	char lines[8][256];
	int n = kit_wrap(FONT_REGULAR, px, text, w, lines, max < 8 ? max : 8);
	for (int i = 0; i < n; i++) font_draw(FONT_REGULAR, px, x, y + i * (px + 8), lines[i], c, w);
	return y + n * (px + 8);
}

static void Button(int x, int y, int w, const char *label, int on, uint32_t c)
{
	gfx_round_rect(x, y, w, 44, 22, on ? with_alpha(c, 0x50) : 0x18ffffff);
	font_draw_center(FONT_BOLD, 20, x + w / 2, kit_text_y(FONT_BOLD, 20, y, 44), label, on ? c : C_DIM, w - 16);
}

void delete_render(void)
{
	int w = screen_narrow() ? 780 : 860, h, x, y;
	char line[600], a[32], b[32];
	if (panel == P_LOCK) {
		h = screen_narrow() ? 300 : 270;
		Box(w, h, &x, &y);
		font_draw(FONT_BOLD, 26, x + 32, y + 28, tr("Allow deleting?"), C_TEXT, w - 64);
		int ty = Lines(x + 32, y + 76, w - 64, 19, kit_btn(N_("HDD Atlas will be able to delete disc images (PS3ISO, PS2ISO...), "
			"folder backups in GAMES, leftover packages, ROMs and webMAN's cache: one row at a time, each after you hold X "
			"to confirm. Deleted files don't come back."), N_("HDD Atlas will be able to delete disc images (PS3ISO, PS2ISO...), "
			"folder backups in GAMES, leftover packages, ROMs and webMAN's cache: one row at a time, each after you hold Circle "
			"to confirm. Deleted files don't come back.")), C_TEXT2, 5);
		Lines(x + 32, ty + 10, w - 64, 17, tr("Installed games, saves, trophies, music, video and photos stay with the XMB "
			"(it keeps a list of them). The system's folders are never touched. Settings turns this off again."), C_DIM, 4);
		const kit_hint_t hints[] = {{G_CROSS, -1, tr("Allow deleting"), 0}, {G_CIRCLE, -1, tr("Cancel"), 0}};
		kit_hints(hints, 2, LIST_X, SCREEN_W - LIST_X);
		return;
	}
	h = panel == P_DONE ? 250 : 340;
	Box(w, h, &x, &y);
	uint32_t red = C_ERR;
	if (panel == P_CONFIRM) {
		static const char *const asks[] = {"", N_("Delete this disc image?"), N_("Delete this folder backup?"), N_("Delete this package?"),
			N_("Delete this ROM?"), N_("Delete this webMAN cache file?"), N_("Delete this leftover install?"), N_("Delete it for good?"), N_("Delete this system update?")};
		font_draw(FONT_BOLD, 26, x + 32, y + 26, empty_all ? tr("Empty the recycle bin?") : to_bin ? tr("Move it to the recycle bin?")
			: tr(asks[kind > 0 && kind <= DEL_FIRMWARE ? kind : 1]), C_TEXT, w - 64);
	} else {
		font_draw(FONT_BOLD, 26, x + 32, y + 26, panel == P_RUNNING ? tr("Deleting...") : tr(done_title), C_TEXT, w - 64);
	}
	font_draw(FONT_MEDIUM, 22, x + 32, y + 74, name, C_TEXT, w - 64);
	if (d) {
		snprintf(line, sizeof(line), "%s/%s", drives[drive].root, d->rel);
		font_draw(FONT_REGULAR, 17, x + 32, y + 106, line, C_DIM, w - 64);
		if (d->twin[0]) {
			const char *tn = strrchr(d->twin, '/');
			snprintf(line, sizeof(line), tr("and %s with it"), tn ? tn + 1 : d->twin);
			font_draw(FONT_REGULAR, 17, x + 32, y + 130, line, C_DIM, w - 64);
		}
	}
	int iy = y + 166;
	if (panel == P_CONFIRM) {
		char files_s[24];
		kit_fmt_size((long long)bytes, a, sizeof(a));
		kit_fmt_count(files, files_s, sizeof(files_s));
		snprintf(line, sizeof(line), files > 1 ? tr("%s  \xc2\xb7  %s files") : "%s", a, files_s);
		font_draw(FONT_BOLD, 24, x + 32, iy, line, C_TEXT, w - 64);
		if (free_before && to_bin) {
			snprintf(line, sizeof(line), "%s", tr("Nothing's freed till the recycle bin's emptied."));
			font_draw(FONT_REGULAR, 18, x + 32, iy + 36, line, C_TEXT2, w - 64);
		} else if (free_before) {
			kit_fmt_size((long long)free_before, a, sizeof(a));
			kit_fmt_size((long long)(free_before + bytes), b, sizeof(b));
			snprintf(line, sizeof(line), tr("Free: %s  \xe2\x86\x92  about %s"), a, b);
			font_draw(FONT_REGULAR, 18, x + 32, iy + 36, line, C_TEXT2, w - 64);
		}
		if (kind == DEL_LEFTOVER && d && strstr(d->rel, "/_INST_"))		// (an install that stopped part way: an update's half there)
			font_draw(FONT_REGULAR, 17, x + 32, iy + 64, tr("If it was a game update, install that update again afterwards."), C_WARN, w - 64);
		int bw = 180, by = y + h - 76, bx = x + w - 32 - 2 * bw - 16;
		char foot[160];
		int bd = bin_days();
		if (!to_bin) snprintf(foot, sizeof(foot), "%s", tr("It can't be undone."));
		else if (bd > 0) snprintf(foot, sizeof(foot), tr("It waits in the recycle bin for %d days first."), bd);
		else snprintf(foot, sizeof(foot), "%s", tr("It waits in the recycle bin till that's emptied."));
		font_draw(FONT_REGULAR, 16, x + 32, by + 12, foot, to_bin ? C_TEXT2 : C_WARN, bx - x - 48);
		Button(bx, by, bw, tr("Cancel"), !pick_delete, C_TEXT);
		Button(bx + bw + 16, by, bw, tr("Delete"), pick_delete, red);
		if (pick_delete && hold_from) {		// the hold, filling the Delete button
			double f = (double)(plat_time_us() - hold_from) / HOLD_US;
			if (f > 1) f = 1;
			gfx_round_rect(bx + bw + 16, by + 40, (int)(bw * f), 4, 2, red);
		}
		const kit_hint_t hints[] = {
			{G_CROSS, -1, pick_delete ? tr("Hold to delete") : tr("Cancel"), 0},
			{G_DPAD_LR, -1, pick_delete ? tr("Back to Cancel") : tr("Pick Delete"), 0},
			{G_CIRCLE, -1, tr("Cancel"), 0},
		};
		kit_hints(hints, 3, LIST_X, SCREEN_W - LIST_X);
	} else if (panel == P_RUNNING && d && d->checking) {		// (Identical files: compared first, every byte)
		kit_fmt_size((long long)d->bytes_done, a, sizeof(a));
		kit_fmt_size((long long)d->bytes_total, b, sizeof(b));
		snprintf(line, sizeof(line), tr("Checking it's the same as the other copy: %s of %s"), a, b);
		font_draw(FONT_MEDIUM, 20, x + 32, iy, line, C_TEXT2, w - 64);
		double f = d->bytes_total ? (double)d->bytes_done / d->bytes_total : 0;
		if (f > 1) f = 1;
		gfx_round_rect(x + 32, iy + 44, w - 64, 12, 6, C_TRACK);
		gfx_round_rect(x + 32, iy + 44, (int)((w - 64) * f) > 12 ? (int)((w - 64) * f) : 12, 12, 6, C_ACCENT);
		const kit_hint_t hints[] = {{G_CIRCLE, -1, d->cancel ? tr("Stopping...") : tr("Stop"), 0}};
		kit_hints(hints, 1, LIST_X, SCREEN_W - LIST_X);
	} else if (panel == P_RUNNING && d) {
		char done_s[24], tot_s[24];
		kit_fmt_count(d->files_done, done_s, sizeof(done_s));
		kit_fmt_count(d->files_total, tot_s, sizeof(tot_s));
		kit_fmt_size((long long)d->bytes_done, a, sizeof(a));
		if (d->files_total > 1) snprintf(line, sizeof(line), tr("%s of %s files  \xc2\xb7  %s"), done_s, tot_s, a);
		else snprintf(line, sizeof(line), "%s", a);
		font_draw(FONT_MEDIUM, 20, x + 32, iy, line, C_TEXT2, w - 64);
		double f = d->files_total ? (double)d->files_done / d->files_total : 0;
		if (f > 1) f = 1;
		gfx_round_rect(x + 32, iy + 44, w - 64, 12, 6, C_TRACK);
		gfx_round_rect(x + 32, iy + 44, (int)((w - 64) * f) > 12 ? (int)((w - 64) * f) : 12, 12, 6, red);
		const kit_hint_t hints[] = {{G_CIRCLE, -1, d->cancel ? tr("Stopping...") : tr("Stop"), 0}};
		kit_hints(hints, 1, LIST_X, SCREEN_W - LIST_X);
	} else {
		Lines(x + 32, iy, w - 64, 20, done_msg, C_TEXT, 4);
		const kit_hint_t hints[] = {{G_CROSS, -1, tr("OK"), 0}};
		kit_hints(hints, 1, LIST_X, SCREEN_W - LIST_X);
	}
}
