/*
ui.c: HDD Atlas's screens, in PS3 World's look (ui_kit.c).

  overview   a drive's used and free space as a bar, then where it's gone:
             a ring of the categories beside a list of them, sizes and parts
             of the drive. Up/Down pick a category (it stands out on the ring),
             X opens it, Square lists the largest items, L1/R1 change drive
             (the HDD, and USB 000-007 or NTFS 0-7 when one's plugged in), START shows
             the internal HDD's health (ui_health.c: its SMART data).

START on the overview opens the menu (ui_menu.c): changes since the last
scan, export a report, Drive health, Settings, About. R3, anywhere, turns
the background music off or on. With deleting allowed (Settings), START on a
row in the lists deletes it (ui_delete.c).
  lists      ui_list.c: a category's items, the largest items and files, and
             a folder's contents, down to any depth.

Each drive's last scan is read from USRDIR, so the overview's there at once;
with none, the HDD's scan starts by itself (a USB drive's waits for Triangle).
A scan runs on its own thread (scan.c), one at a time, and the screen redraws
about ten times a second while it does, so it never waits on the drive.
Triangle scans again; Circle stops a scan, or quits (pressed twice, as in PS3
World).

Every screen lays itself out from SCREEN_W: 1280, or 960 in the 4:3 layout.
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
#include "fsx.h"
#include "scan.h"
#include "ring.h"
#include "icons.h"
#include "music.h"
#include "settings.h"
#include "history.h"
#include "ui_kit.h"
#include "bin.h"
#include "del.h"
#include "jobs.h"
#include "ui_int.h"
#include "version.h"
#include "ui.h"

#ifndef DRIVE
#define DRIVE			"/dev_hdd0"		// (tools/host/shot.c points it elsewhere)
#endif
#define PROGRESS_US		100000		// the screen redrawn this often while a scan runs
#define SPACE_US		5000000		// the drives' free space (and which are plugged in) read again this often
#define QUIT_US			4000000		// Circle again within this: quit
#define NOTE_US			3500000
#define CHANGED_BYTES	(512LL * 1024 * 1024)	// free space moved more than this since the scan: say so

// each category's colour (PS3 World's blue, cyan, green and yellow, and more in its manner; the
// system's partitions slate, Unaccounted rose, so neither can be taken for the free space's track)
const uint32_t cat_colors[NUM_CATS] = {
	0xff2b8fff, 0xff4ad9ff, 0xff4cd08a, 0xffb57bff, 0xffff8a5c, 0xffffc861, 0xff9aa4b8, 0xff5b6b8c, 0xffe0607e,
};

drive_t drives[NUM_DRIVES];
int cur_drive;

static int in_list;				// a list's on screen (ui_list.c), not the overview
static int in_health;			// the Drive health screen (ui_health.c)
static int stuck_drive = -1;	// a scan in trouble on the drive: its panel, till X or Circle
static int stuck_kind;			// what trouble (below)
static char stuck_at[192];		// (where it was stuck)
enum {
	STUCK_GIVEN_UP,			// stuck inside a call to the drive, and given up on
	STUCK_READ_ERRORS,		// finished, but the internal drive gave read errors: what it couldn't read was skipped
	STUCK_UNFINISHED,		// the internal drive's read errors stopped it
};
static int in_map;				// the map (ui_map.c)
static int from_menu;			// that screen was opened from the START menu: backing out goes back to it
static int in_home = 1;			// the start screen (ui_home.c): what the app opens on, after the intro
static int home_menu;			// the menu (or Settings) was opened from the start screen: closing it goes back there
static int sel;					// the category picked
static int dirty = 1;
static int scan_drive = -1;		// the drive a scan we started is of (-1: none running)
static volatile int hung_drive = -1;	// the drive whose scan was given up on, its call still out (-1: none): nothing
									// else is done on it (it'd wait behind that call, the screen too), till it's back
static uint64_t last_progress;
static image_t *ring;
static int ring_sel = -2;
static unsigned shown_gen = 1, ring_gen;	// (a new scan can be at the old one's address: a count tells them apart)
static int ring_drive = -1;
static int ring_d;
static char note[400];		// (a translation with a path in it, 3 bytes a character: not cut in two)
static uint64_t note_until, quit_asked, space_at;
static int sound_ok;
static uint64_t last_input;		// the screen dims after Settings' minutes without a button
static int dimmed;

void ui_dirty(void) { dirty = 1; }
const char *ui_drive_name(int i) { return tr(drives[i].name); }
int ui_scanning(void) { return scan_drive >= 0; }
// the drive, or (when it's an NTFS drive) any NTFS drive: every NTFS call takes one lock, and the
// stuck call holds it, so the others' would wait on it too
int ui_drive_hung(int i)
{
	int h = hung_drive;
	return i >= 0 && h >= 0 && (i == h || (i >= FIRST_NTFS && h >= FIRST_NTFS));
}

// (something asked of the drive whose scan was given up on, or of another NTFS drive then)
void ui_hung_note(void)
{
	char msg[300];
	int h = hung_drive;
	if (h < 0) return;
	if (cur_drive != h && ui_drive_hung(cur_drive))
		snprintf(msg, sizeof(msg), tr("%s stopped answering during its scan, and every NTFS drive waits on it: restart the console to use them again."), ui_drive_name(h));
	else snprintf(msg, sizeof(msg), tr("%s stopped answering during its scan: restart the console to use it again."), ui_drive_name(h));
	ui_note(msg);
	ui_sound(SND_BACK);
}

void ui_note(const char *msg)
{
	snprintf(note, sizeof(note), "%s", msg);
	note_until = plat_time_us() + NOTE_US;
	dirty = 1;
}

void ui_sound(int id) { if (sound_ok) plat_sound(id); }

void ui_fmt_pct(uint64_t v, uint64_t of, char *out, int size)
{
	double p = of ? 100.0 * v / of : 0;
	if (v && p < 0.1) snprintf(out, size, "<0.1%%");
	else snprintf(out, size, "%.1f%%", p);
}

static int Drives_Present(void)
{
	int n = 0;
	for (int i = 0; i < NUM_DRIVES; i++) n += drives[i].present;
	return n;
}
int ui_drives_present(void) { return Drives_Present(); }

static void Scan_Drive(int i);
static void Changes_Load(drive_t *d);

/*
==================
The recycle bin's old entries: gone for good at start, and when a USB drive's plugged in (Settings' days)
==================
*/
static job_t *expire_job[NUM_DRIVES];		// each drive's own (they queue on the delete lane: one at a time)

#define CLOCK_SANE	1700000000LL		// (Nov 2023: a console clock before this is one that's been reset)

static void Expire_Cleanup(job_t *j) { free(j->arg); j->arg = NULL; }

static void Expire_Run(job_t *j)
{
	bin_entry_t *e;
	bin_tidy(j->sval);		// (the list and the folders in step first, after any crash part way)
	int days = j->ival, n = days > 0 ? bin_load(j->sval, &e) : 0, gone = 0;
	long long bytes = 0;
	int64_t now = (int64_t)time(NULL), newest = 0;
	for (int i = 0; i < n; i++) if (e[i].when > newest) newest = e[i].when;
	// a clock that's been reset, or put back past what's in the bin: nothing goes on its word
	if (n && (now < CLOCK_SANE || now + 86400 < newest)) {
		plat_log("bin: the clock says %lld, before what's in the bin (%lld): nothing expired", (long long)now, (long long)newest);
		n = 0;
	}
	del_job_t *d = n ? calloc(1, sizeof(*d)) : NULL;
	for (int i = 0; d && i < n && !j->cancel; i++) {
		if (e[i].when + (int64_t)days * 86400 > now) continue;
		memset(d, 0, sizeof(*d));
		snprintf(d->root, sizeof(d->root), "%s", j->sval);
		if (!bin_entry_rel(j->sval, e[i].id, d->rel, sizeof(d->rel))) continue;
		d->is_dir = 1;
		int r = del_run(d);
		if (r == 0) { bin_remove(j->sval, e[i].id); gone++; }
		bytes += (long long)d->bytes_done;		// (what went, whether or not all of it did: the scan's bin row follows)
		if (j->arg) *(uint32_t *)j->arg += d->files_done;
	}
	free(d);
	if (days > 0) free(e);
	j->ival = gone;
	j->lval = bytes;
	j->ok = 1;
}

// at start, and when a USB drive's plugged in: tidied (always: the bin may be off now, with
// what was put in it before), and its old entries gone for good
static void Expire_Start(int drive)
{
	char rel[FSX_PATH], abs[FSX_PATH + 64];
	int type;
	uint64_t size;
	if (expire_job[drive] || !bin_rel(drives[drive].root, rel, sizeof(rel))) return;
	snprintf(abs, sizeof(abs), "%s/%s", drives[drive].root, rel);
	if (fsx_stat(abs, &type, &size) != 0 || type != FSX_DIR) return;		// (no bin on it)
	job_t *j = job_new(Expire_Run, Expire_Cleanup);
	if (!j) return;
	j->arg = calloc(1, sizeof(uint32_t));		// (the files that went: the scan's bin row follows)
	snprintf(j->sval, sizeof(j->sval), "%s", drives[drive].root);
	j->ival = bin_days();
	plat_log("bin: %s: tidied, and entries older than %d days go for good", drives[drive].root, j->ival);
	expire_job[drive] = j;
	job_post(j, LANE_DL);		// (after any delete going, and any other drive's: one at a time)
}

// a drive's scan follows its recycle bin: bytes and files moved in (> 0) or gone from it (< 0), with
// the free space now; saved. Not while it's being scanned (a rescan of a category reads the scan as
// it is; and the scan coming will have it right)
void ui_bin_counted(int drive, int64_t bytes, int32_t files)
{
	scan_t *s = drive >= 0 && drive < NUM_DRIVES ? drives[drive].shown : NULL;
	if (!s || scan_drive == drive || hung_drive == drive || (!bytes && !files)) return;
	uint64_t t, fr = 0, k;
	char how[64], cache[256];
	fsx_space(drives[drive].root, &t, &fr, &k, how, sizeof(how));
	scan_bin_change(s, bytes, files, fr);
	scan_cache_path(s->root, cache, sizeof(cache));
	scan_save(s, cache);
	shown_gen++;		// (the ring and the overview again)
	dirty = 1;
}

// the drive on screen's recycle bin is being tidied and expired now (nothing's put back meanwhile:
// an entry could be half deleted)
int ui_bin_busy(void) { return expire_job[cur_drive] != NULL; }

static void Expire_Poll(void)
{
	for (int i = 0; i < NUM_DRIVES; i++) {
		job_t *j = expire_job[i];
		if (!j || !job_done(j)) continue;
		if (j->ival) {
			char msg[200], a[32];
			kit_fmt_size(j->lval, a, sizeof(a));
			snprintf(msg, sizeof(msg), tr("Emptied %d old entries from the recycle bin: %s freed."), j->ival, a);
			ui_note(msg);
		}
		ui_bin_counted(i, -(int64_t)j->lval, -(int32_t)(j->arg ? *(uint32_t *)j->arg : 0));
		job_release(j);
		expire_job[i] = NULL;
		if (i == cur_drive && in_list) list_bin_changed();		// (the bin's list: what's in it now)
	}
}

/*
A port's scan, cache, changes and history are kept by the port's name (dev_usb000), not the drive
in it: another stick in the same port would be shown the last one's scan, and compared with it.
A scan says whose it is by its partition's size: one that doesn't match the drive there now (both
known) is another drive's, and isn't shown.
*/
static int Not_This_Drive(const scan_t *s, uint64_t total) { return s && s->total && total && s->total != total; }

// a USB drive's scan and changes let go of (another drive's): only when nothing's reading them
// (a scan or a rescan of it, a report, a delete panel); else it's tried again at the next read
static int Drop_Foreign(int i)
{
	drive_t *d = &drives[i];
	if (scan_drive == i || hung_drive == i || menu_exporting() || delete_active()) return 0;		// (hung: a rescan's may still read it)
	plat_log("ui: %s: the last scan's of another drive (%.2f GiB, this one %.2f GiB): not shown", d->name,
		d->shown->total / 1073741824.0, d->live_total / 1073741824.0);
	scan_free(d->shown);
	d->shown = NULL;
	scan_changes_free(d->changes);
	d->changes = NULL;
	shown_gen++;
	if (i == cur_drive) home_drive_changed();
	dirty = 1;
	return 1;
}

/*
Every drive's space is read on a thread of its own, every SPACE_US (or soon after space_want): a
USB drive that's stopped answering would freeze the screen at its next read. Space_Read takes what
it last read. The hung drive isn't asked (its last figures stay).
*/
static struct { int there; uint64_t t, f, k; } space_got[NUM_DRIVES];
static volatile int space_lock, space_want, space_quit, space_live;
static volatile unsigned space_seq;
static unsigned space_used;

static void Space_Poll(void)
{
	char how[64];
	for (int i = 0; i < NUM_DRIVES && !space_quit; i++) {
		uint64_t t = 0, f = 0, k = 0;
		if (i == hung_drive) continue;
		int there = fsx_space(drives[i].root, &t, &f, &k, how, sizeof(how)) == 0;
		spin_lock(&space_lock);
		space_got[i].there = there;
		space_got[i].t = t;
		space_got[i].f = f;
		space_got[i].k = k;
		spin_unlock(&space_lock);
	}
	__atomic_add_fetch(&space_seq, 1, __ATOMIC_ACQ_REL);
}

static void Space_Thread(void *arg)
{
	(void)arg;
	uint64_t last = plat_time_us();
	while (!space_quit) {
		if (space_want || plat_time_us() - last >= SPACE_US) {
			space_want = 0;
			Space_Poll();
			last = plat_time_us();
		}
		plat_sleep_us(100000);
	}
	space_live = 0;
}

// every drive's space (a USB drive that answers is plugged in), as the thread last read it; a drive
// just plugged in has its last scan read from the cache (if it's that drive's)
static void Space_Read(void)
{
	char path[256];
	unsigned seq = __atomic_load_n(&space_seq, __ATOMIC_ACQUIRE);
	if (seq == space_used) return;		// (nothing new)
	space_used = seq;
	for (int i = 0; i < NUM_DRIVES; i++) {
		drive_t *d = &drives[i];
		spin_lock(&space_lock);
		uint64_t t = space_got[i].t, f = space_got[i].f, k = space_got[i].k;
		int there = space_got[i].there;
		spin_unlock(&space_lock);
		if (i == 0) there = 1;		// (the HDD's always there, whatever it answers)
		if (there != d->present) {
			d->present = there;
			dirty = 1;
			if (i) plat_log("ui: %s %s", d->name, there ? "plugged in" : "gone");
			if (i && there && !plat_read_only(d->root)) Expire_Start(i);		// (its recycle bin's old entries; not on a drive that can't be written)
			if (there && !d->shown && i) {		// (the HDD's was read by ui_init)
				scan_cache_path(d->root, path, sizeof(path));
				d->shown = scan_load(path);
				if (Not_This_Drive(d->shown, t)) {		// (nothing has it yet: let go of at once)
					plat_log("ui: %s: the cached scan's of another drive (%.2f GiB, this one %.2f GiB): not shown", d->name,
						d->shown->total / 1073741824.0, t / 1073741824.0);
					scan_free(d->shown);
					d->shown = NULL;
					scan_changes_free(d->changes);
					d->changes = NULL;
				} else {
					Changes_Load(d);
					if (d->shown && !history_count(d->root)) history_add(d->shown);		// (its history starts with what's known)
				}
				shown_gen++;
			}
			if (there && i && setting_get("usbscan", 0) && scan_drive < 0 && space_at) {		// (not the ones there at start)
				plat_log("ui: %s plugged in: scanning it (Settings)", d->name);
				Scan_Drive(i);
			}
		}
		if (!there) continue;
		if (f != d->live_free || t != d->live_total) dirty = 1;
		d->live_total = t;
		d->live_free = f;
		d->live_disk = k;
		if (i && Not_This_Drive(d->shown, t)) Drop_Foreign(i);		// (a stick swapped for another while the app ran)
	}
	if (!DRV->present && !in_list) { cur_drive = 0; dirty = 1; }
	space_at = plat_time_us();
}

static void Scan_Drive(int i)
{
	char msg[240];
	if (hung_drive >= 0) {		// (the scan thread's still in the call: no other can start)
		if (ui_drive_hung(i) && i == cur_drive) ui_hung_note();
		else {
			snprintf(msg, sizeof(msg), tr("No scan can start till the console restarts: %s stopped answering during its scan."), ui_drive_name(hung_drive));
			ui_note(msg);
		}
		return;
	}
	if (scan_drive >= 0) {
		snprintf(msg, sizeof(msg), tr("A scan of %s is running: one at a time."), ui_drive_name(scan_drive));
		ui_note(msg);
		return;
	}
	if (scan_start(drives[i].root)) {
		scan_drive = i;
		last_progress = 0;
		if (i == 0 && health_known_worn()) {		// (its last health reading found it wearing out: a scan reads all of it)
			ui_note(tr("Drive health found this drive wearing out: back up what matters first."));
			plat_log("ui: the internal drive's last health reading found it wearing out: said before its scan");
		} else {
			ui_note(tr("Scanning: things may be slow until it's done. The menu's back after it."));
		}
		plat_log("ui: scan of %s started", drives[i].root);
	} else {
		ui_note(tr("The scan couldn't start (see the log)."));
	}
	dirty = 1;
}

static void Scan_Begin(void) { Scan_Drive(cur_drive); }
int ui_scan_drive(void) { return scan_drive; }

// how far the scan is, 0-99 (done is when the walk's done, not when the bytes say); -1 when it can't
// be told: the used space unknown, or the files already adding up to more than it (hard links, as
// webMAN's: a bar would sit at 99% for the rest of the scan and look stuck), so counts are shown
int ui_scan_pct(void)
{
	scan_progress_t *p = scan_progress();
	if (!p->used || p->bytes >= p->used) return -1;
	int pct = (int)(100.0 * p->bytes / p->used);
	return pct > 99 ? 99 : pct;
}

void ui_rescan_category(int cat)
{
	char msg[240];
	if (hung_drive >= 0) {
		if (ui_drive_hung(cur_drive)) ui_hung_note();
		else {
			snprintf(msg, sizeof(msg), tr("No scan can start till the console restarts: %s stopped answering during its scan."), ui_drive_name(hung_drive));
			ui_note(msg);
		}
		return;
	}
	if (scan_drive >= 0) {
		snprintf(msg, sizeof(msg), tr("A scan of %s is running: one at a time."), ui_drive_name(scan_drive));
		ui_note(msg);
		return;
	}
	if (!DRV->shown || !scan_start_partial(DRV->root, cat, DRV->shown)) {
		ui_note(tr("The scan couldn't start (see the log)."));
		return;
	}
	scan_drive = cur_drive;
	last_progress = 0;
	snprintf(msg, sizeof(msg), tr("Rescanning %s; the rest stays as it was."), tr(scan_cat_name(cat)));
	ui_note(msg);
	plat_log("ui: rescan of %s's %s started", DRV->root, scan_cat_name(cat));
}

// a drive's changes since its scan before, from USRDIR (kept until its next scan)
static void Changes_Load(drive_t *d)
{
	char path[256];
	scan_changes_path(d->root, path, sizeof(path));
	scan_changes_free(d->changes);
	d->changes = scan_changes_load(path);
}


static void Music_Begin(void)
{
	char path[256];
	menu_apply_settings();
	snprintf(path, sizeof(path), "%s/music.ogg", plat_data_dir());
	music_start(path);
}

void ui_init(void)
{
	char path[256];
	static const char *const usb[8] = {"000", "001", "002", "003", "004", "005", "006", "007"};
	bin_init();
	snprintf(drives[0].root, sizeof(drives[0].root), "%s", DRIVE);
	snprintf(drives[0].name, sizeof(drives[0].name), "%s", N_("Internal HDD"));
	for (int i = 1; i < FIRST_NTFS; i++) {
		snprintf(drives[i].root, sizeof(drives[i].root), "/dev_usb%s", usb[i - 1]);
		snprintf(drives[i].name, sizeof(drives[i].name), "USB %s", usb[i - 1]);
	}
	for (int i = FIRST_NTFS; i < NUM_DRIVES; i++) {		// (an NTFS drive: mounted by the app, a few seconds in)
		snprintf(drives[i].root, sizeof(drives[i].root), "/ntfs%d:", i - FIRST_NTFS);
		snprintf(drives[i].name, sizeof(drives[i].name), "NTFS %d", i - FIRST_NTFS);
	}
	kit_init();
	home_init();
	Music_Begin();
	sound_ok = 1;		// (main starts the sound after ui_init; plat_sound before that is ignored)
	scan_cache_path(drives[0].root, path, sizeof(path));
	drives[0].shown = scan_load(path);
	Changes_Load(&drives[0]);
	if (drives[0].shown && !history_count(drives[0].root)) history_add(drives[0].shown);
	Space_Poll();		// (the first read here: what's plugged in, before the first frame)
	Space_Read();
	space_live = plat_thread_start(Space_Thread, NULL, 1700, 64, "space");
	if (drives[0].shown) {
		plat_log("ui: the last scan, from the cache");
		long long moved = (long long)drives[0].live_free - (long long)drives[0].shown->free_bytes;
		if (setting_get("autoscan", 0) && (moved > CHANGED_BYTES || moved < -CHANGED_BYTES)) {
			plat_log("ui: free space moved %lld bytes since the scan: scanning again (Settings)", moved);
			Scan_Begin();
		}
	} else plat_log("ui: no cache: the start screen offers a scan");
	Expire_Start(0);
}

/*
==================
Update
==================
*/
// the internal drive gave read errors (bad sectors, usually): the panel that says so, and that Drive
// health shows why (Triangle)
static void Read_Errors(int kind)
{
	stuck_drive = 0;
	stuck_kind = kind;
	plat_log("ui: %s gave %llu read errors%s: Drive health suggested", ui_drive_name(0), (unsigned long long)scan_progress()->read_errors,
		kind == STUCK_UNFINISHED ? " and the scan didn't finish" : "");
	if (kind == STUCK_UNFINISHED) ui_sound(SND_BACK);
	dirty = 1;
}

static void Scan_Tick(void)
{
	if (hung_drive >= 0 && !scan_progress()->running) {		// (the call given up on came back: the drive can be used again)
		plat_log("ui: %s answered again: usable", drives[hung_drive].name);
		hung_drive = -1;
		dirty = 1;
	}
	if (scan_drive < 0) return;
	if (menu_exporting()) return;		// (a report's reading the drive's scan on a worker: the new one waits till it's done)
	scan_t *s = scan_take();
	if (!s && !scan_progress()->running) s = scan_take();		// (finished between the two)
	if (s) {
		drive_t *d = &drives[scan_drive];
		if (d->shown && Not_This_Drive(d->shown, s->total)) {		// (the scan before was another drive's: nothing to compare)
			plat_log("ui: %s: the scan before was of another drive: no changes", d->name);
			scan_changes_free(d->changes);
			d->changes = NULL;
			char path[256];
			scan_changes_path(d->root, path, sizeof(path));
			remove(path);
		} else if (d->shown) {		// what changed since the scan before: kept until the next
			scan_changes_t *c = scan_diff(d->shown, s);
			if (c) {
				char path[256];
				scan_changes_path(d->root, path, sizeof(path));
				scan_changes_save(c, path);
				scan_changes_free(d->changes);
				d->changes = c;
				plat_log("ui: %u changes since the scan before", (unsigned)c->count);
			}
		}
		scan_free(d->shown);
		d->shown = s;
		shown_gen++;
		history_add(s);
		char msg[160];
		snprintf(msg, sizeof(msg), tr("Scan of %s finished in %.1f s."), ui_drive_name(scan_drive), s->duration_ms / 1000.0);
		ui_note(msg);
		ui_sound(SND_OK);
		if (scan_drive == 0 && scan_progress()->read_errors) Read_Errors(STUCK_READ_ERRORS);
		if (scan_drive == cur_drive && in_list) list_scan_changed();
		scan_drive = -1;
		space_want = 1;		// (its space now, soon)
		return;
	}
	if (!scan_progress()->running) {
		if (scan_drive == 0 && !scan_progress()->cancel && scan_progress()->read_errors) {
			Read_Errors(STUCK_UNFINISHED);
			scan_drive = -1;
			return;
		}
		scan_drive = -1;
		ui_note(scan_stopped() ? tr("Scan stopped. The last scan is shown.") : tr("The scan didn't finish (see the log)."));
		return;
	}
	if (scan_watch(stuck_at, sizeof(stuck_at))) {		// (nothing for 5 minutes: the drive never answered)
		stuck_drive = hung_drive = scan_drive;
		stuck_kind = STUCK_GIVEN_UP;
		scan_drive = -1;
		ui_sound(SND_BACK);
		dirty = 1;
		return;
	}
	if (plat_time_us() - last_progress > PROGRESS_US) {
		last_progress = plat_time_us();
		dirty = 1;
	}
}

/*
==================
What's this: the overview's two rows that aren't folders, explained (X on them). From the PS3 HDD
structure report: the disk's own parts outside dev_hdd0, and the file system's free-space reserve
(minfree: 8% as sold, 1% after webMAN's unlockhdd); files adding up to more than is used means some
are counted twice (webMAN can hard-link a game's files between GAMES and game, which stat can't see)
==================
*/
static int explain = -1;		// the row explained (-1: none)

static void Draw_Explain(const scan_t *s)
{
	char text[1200], lines[10][256];
	if (explain == CAT_RESERVED)
		snprintf(text, sizeof(text), "%s", tr("The disk's parts outside the drive HDD Atlas scans: a 2 GB game cache, the system's flash on most models, and the file system's records of every file (a few percent of the disk). None of it can be cleared."));
	else {
		snprintf(text, sizeof(text), "%s", tr("Used space no file accounts for. Most of it is what the file system keeps free for itself: 8 percent of the drive as sold, 1 percent after webMAN's unlockhdd. The rest is files' last blocks, rounded up. None of it can be cleared from here."));
		uint64_t used = scan_drive_used(s);
		if (used && s->scanned > used + (1ULL << 30)) {
			size_t l = strlen(text);
			snprintf(text + l, sizeof(text) - l, "\n\n%s", tr("Here the files add up to more than the used space: some are counted twice. webMAN can link a game's files between GAMES and game, one copy on the disk counted as two."));
		}
	}
	int w = screen_narrow() ? 760 : 900, n = 0;
	for (char *para = text, *next; para && n < 10; para = next) {		// (paragraphs, each wrapped)
		next = strstr(para, "\n\n");
		if (next) { *next = 0; next += 2; }
		n += kit_wrap(FONT_REGULAR, 19, para, w - 64, lines + n, 10 - n);
		if (next && n < 10) lines[n++][0] = 0;
	}
	int h = 96 + n * 28 + 24, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xa0000000);
	gfx_round_rect(x - 1, y - 1, w + 2, h + 2, 19, C_BORDER);
	gfx_round_rect(x, y, w, h, 18, 0xf20a0e18);
	font_draw(FONT_BOLD, 24, x + 32, y + 30, tr(scan_cat_name(explain)), C_TEXT, w - 64);
	for (int i = 0; i < n; i++) font_draw(FONT_REGULAR, 19, x + 32, y + 80 + i * 28, lines[i], C_TEXT2, w - 64);
}

static int Openable(int k) { return DRV->shown && k < NUM_SCANNED_CATS && DRV->shown->cat_items[k]; }

static void Switch_Drive(int dir)
{
	if (Drives_Present() < 2) return;
	int d = cur_drive;
	do d = (d + dir + NUM_DRIVES) % NUM_DRIVES; while (!drives[d].present);
	cur_drive = d;
	home_drive_changed();
	ui_sound(SND_MOVE);
	dirty = 1;
}

void ui_update(void)
{
	input_update();
	uint32_t p = input_pressed();
	uint64_t now = plat_time_us();
	// the dimmer: any button wakes the screen (and does nothing else); idle long enough, it dims
	if (p || input_held()) {
		last_input = now;
		if (dimmed) { dimmed = 0; dirty = 1; return; }
	}
	int dim_min = setting_get("dim", 5);
	if (!last_input) last_input = now;
	if (dim_min > 0 && !dimmed && !plat_osk_active() && now - last_input > (uint64_t)dim_min * 60000000ULL) { dimmed = 1; dirty = 1; }
	if (dimmed) {
		static uint64_t moved;		// (its little name moves once a minute: nothing stays put)
		if (now - moved > 60000000ULL) { moved = now; dirty = 1; }
		Scan_Tick();
		return;
	}
	Scan_Tick();
	Space_Read();
	if (note[0] && now > note_until) { note[0] = 0; dirty = 1; }
	if (icons_poll()) dirty = 1;
	Expire_Poll();
	if (p & BTN_R3) menu_music_toggle();
	menu_poll();

	if (stuck_drive >= 0) {		// (the stuck scan's panel takes the buttons till it's read)
		if (p & (BTN_CROSS | BTN_CIRCLE)) { stuck_drive = -1; ui_sound(SND_OK); dirty = 1; }
		else if (stuck_drive == 0 && (p & BTN_TRIANGLE)) {		// (the internal drive: its health, to see why)
			stuck_drive = -1;
			in_home = in_list = in_map = 0;
			health_open();
			in_health = 1;
			ui_sound(SND_OK);
			dirty = 1;
		}
		return;
	}
	if (home_intro_active()) {		// (any button hurries the intro on)
		if (p) home_intro_hurry();
		dirty = 1;
		return;
	}
	if (delete_active()) {		// (a delete panel takes the buttons, over any screen)
		delete_update(p);
		return;
	}
	if (menu_active()) {
		int go = menu_update(p);
		if (go == MENU_GO_CHANGES) { list_open_changes(); in_list = 1; }
		else if (go == MENU_GO_GAMES || go == MENU_GO_DUPES) { list_open_games(go == MENU_GO_DUPES); in_list = 1; }
		else if (go == MENU_GO_FREE) { list_open_free(); in_list = 1; }
		else if (go == MENU_GO_TYPES) { list_open_types(); in_list = 1; }
		else if (go == MENU_GO_OLD) { list_open_mode(LIST_OLD); in_list = 1; }
		else if (go == MENU_GO_FOLDERS) { list_open_mode(LIST_FOLDERS); in_list = 1; }
		else if (go == MENU_GO_MARKS) { list_open_mode(LIST_MARKS); in_list = 1; }
		else if (go == MENU_GO_SAME) { list_open_mode(LIST_SAME); in_list = 1; }
		else if (go == MENU_GO_FILTER) { list_open_mode(LIST_FILTER); in_list = 1; }
		else if (go == MENU_GO_RECENT) { list_open_mode(LIST_RECENT); in_list = 1; }
		else if (go == MENU_GO_BIN) { list_open_mode(LIST_BIN); in_list = 1; }
		else if (go == MENU_GO_MAP) { map_open(); in_map = 1; }
		else if (go == MENU_GO_HEALTH) { health_open(); in_health = 1; }
		from_menu = in_list || in_map || in_health;
		if (home_menu && !menu_active() && !from_menu) { home_menu = 0; in_home = 1; }		// (closed: back to the start screen)
		return;
	}
	if (in_home) {
		if (p & BTN_L1) Switch_Drive(-1);
		if (p & BTN_R1) Switch_Drive(1);
		int a = home_update(p);
		if (a == HOME_SCAN) { in_home = 0; if (scan_drive < 0) Scan_Begin(); }
		else if (a == HOME_LAST) in_home = 0;
		else if (a == HOME_TOOLS && ui_drive_hung(cur_drive)) ui_hung_note();
		else if (a == HOME_TOOLS) { in_home = 0; home_menu = 1; menu_open(); }
		else if (a == HOME_SETTINGS) { in_home = 0; home_menu = 1; menu_open_settings(); }
		else if (a == HOME_QUIT) {
			ui_sound(SND_BACK);
			if (quit_asked && now - quit_asked < QUIT_US) plat_quit();
			else { quit_asked = now; ui_note(kit_btn(N_("Press Circle again to quit."), N_("Press X again to quit."))); }
		}
		if (a != HOME_NONE) dirty = 1;
		return;
	}
	if (in_list || in_map || in_health) {
		int stay = in_list ? list_update(p) : in_map ? map_update(p) : health_update(p);
		if (!stay) {
			in_list = in_map = in_health = 0;
			dirty = 1;
			if (from_menu && scan_drive < 0 && !ui_drive_hung(cur_drive)) menu_open();		// (back where it was opened from; not while a scan's going)
			else if (home_menu) { home_menu = 0; in_home = 1; }
			from_menu = 0;
		}
		return;
	}
	if (explain >= 0) {		// (any button closes it)
		if (p) { explain = -1; ui_sound(SND_BACK); dirty = 1; }
		return;
	}
	if ((p & (BTN_START | BTN_CROSS | BTN_SQUARE)) && ui_drive_hung(cur_drive)) {		// (its menu, lists and deletes: they'd wait on it)
		ui_hung_note();
		return;
	}
	if (p & BTN_START) {
		if (scan_drive >= 0) {		// (not while scanning: the menu's screens would be slow, and of a scan that's about to change)
			ui_sound(SND_BACK);
			ui_note(tr("The menu opens once the scan's done."));
			return;
		}
		ui_sound(SND_OK);
		menu_open();
		return;
	}
	if (p & BTN_UP) { sel = (sel + NUM_CATS - 1) % NUM_CATS; ui_sound(SND_MOVE); dirty = 1; }
	if (p & BTN_DOWN) { sel = (sel + 1) % NUM_CATS; ui_sound(SND_MOVE); dirty = 1; }
	if (p & BTN_L1) Switch_Drive(-1);
	if (p & BTN_R1) Switch_Drive(1);
	if ((p & BTN_CROSS) && DRV->shown && sel >= NUM_SCANNED_CATS) { explain = sel; ui_sound(SND_OK); dirty = 1; return; }
	if ((p & BTN_CROSS) && Openable(sel)) {
		ui_sound(SND_OK);
		list_open_items(sel);
		in_list = 1;
		return;
	}
	if ((p & BTN_SQUARE) && DRV->shown) {
		ui_sound(SND_OK);
		list_open_largest();
		in_list = 1;
		return;
	}
	if (p & BTN_TRIANGLE) {
		ui_sound(SND_OK);
		Scan_Begin();
	}
	if (p & BTN_CIRCLE) {
		ui_sound(SND_BACK);
		if (scan_drive >= 0) {
			scan_cancel();
			plat_log("ui: scan stopped by the user");
		} else {
			in_home = 1;		// (the start screen: quitting's from there)
			dirty = 1;
		}
	}
}

int ui_needs_redraw(void) { return dirty; }
void ui_force_redraw(void) { dirty = 1; }

void ui_shutdown(void)
{
	if (scan_drive >= 0) {
		scan_cancel();
		scan_wait(3000000);		// (the thread's out before the process goes)
	}
	space_quit = 1;
	for (int i = 0; i < 30 && space_live; i++) plat_sleep_us(50000);		// (not for ever: a drive's read can be stuck)
	delete_shutdown();
	for (int i = 0; i < NUM_DRIVES; i++) if (expire_job[i]) { job_release(expire_job[i]); expire_job[i] = NULL; }		// (its lane waited for just now)
	menu_shutdown();
	home_shutdown();
	music_stop();
	health_shutdown();
	list_shutdown();
	icons_shutdown();
	for (int i = 0; i < NUM_DRIVES; i++) {
		if (i == hung_drive || (scan_drive == i && scan_progress()->running)) continue;		// (a scan still out may read it: left, the process is going)
		scan_free(drives[i].shown);
		drives[i].shown = NULL;
		scan_changes_free(drives[i].changes);
		drives[i].changes = NULL;
	}
}

/*
==================
Drawing
==================
*/
static void Fmt_When(int64_t t, char *out, int size)
{
	time_t tt = (time_t)t;
	struct tm *tm = t > 0 ? localtime(&tt) : NULL;
	out[0] = 0;
	if (tm) strftime(out, size, "%Y-%m-%d %H:%M", tm);
}

// the overview's status line, top right: how wide it may be (the drive panel's title is left of it)
static int Status_W(void) { return SCREEN_W - 2 * LIST_X - 420; }
// a note fits in it (else it's shown in the pill, as on the other screens: at 4:3 most do)
static int Note_In_Status(void) { return note[0] && font_width(FONT_REGULAR, 18, note) <= Status_W(); }

// top right, under the version: when the scan was, and whether the drive's changed since
static void Draw_Status(void)
{
	char line[200], when[48], d[32];
	uint32_t c = C_DIM;
	scan_t *shown = DRV->shown;
	if (Note_In_Status()) {
		snprintf(line, sizeof(line), "%s", note);
		c = C_WARN;
	} else if (!shown) {
		snprintf(line, sizeof(line), "%s", tr("Not scanned yet"));
	} else {
		long long moved = (long long)DRV->live_free - (long long)shown->free_bytes;
		if (DRV->live_free && (moved > CHANGED_BYTES || moved < -CHANGED_BYTES)) {
			kit_fmt_size(moved < 0 ? -moved : moved, d, sizeof(d));
			snprintf(line, sizeof(line), moved < 0 ? tr("Free space has dropped by %s since this scan") : tr("Free space has grown by %s since this scan"), d);
			c = C_WARN;
		} else {
			Fmt_When(shown->when, when, sizeof(when));
			if (shown->edits)
				snprintf(line, sizeof(line), tr(shown->edits == 1 ? "Scanned %s  \xc2\xb7  1 delete since" : "Scanned %s  \xc2\xb7  %u deletes since"),
					when, (unsigned)shown->edits);
			else snprintf(line, sizeof(line), tr("Scanned %s  \xc2\xb7  took %.0f s"), when, shown->duration_ms / 1000.0);
		}
	}
	int maxw = Status_W();
	int w = font_width(FONT_REGULAR, 18, line);
	if (w > maxw) w = maxw;
	font_draw(FONT_REGULAR, 18, SCREEN_W - LIST_X - w, 68, line, c, maxw);
}

// the drive: its name (L1/R1 either side when there's more than one), used of total, a bar of
// the categories, and what's free
#define USE_Y	100
#define USE_H	112
static void Draw_Usage(void)
{
	char a[32], b[32], line[160];
	int x = LIST_X - 12, w = LIST_W + 24, narrow = screen_narrow();
	scan_t *shown = DRV->shown;
	gfx_round_rect(x, USE_Y, w, USE_H, 14, C_PANEL);
	int tx = LIST_X + 6, rx = SCREEN_W - LIST_X - 6, nx = tx;
	int many = Drives_Present() > 1;
	if (many) { kit_glyph(G_L1, nx, USE_Y + 15); nx += GLYPH + 8; }
	nx += font_draw(FONT_BOLD, 24, nx, USE_Y + 16, ui_drive_name(cur_drive), C_TEXT, 0);
	if (many) { kit_glyph(G_R1, nx + 8, USE_Y + 15); nx += GLYPH + 16; }
	font_draw(FONT_REGULAR, 17, nx + 12, USE_Y + 21, DRV->root, C_DIM, (rx - tx) / 2 - (nx - tx) - 24);	// (the used line's on the right half)
	int bx = tx, by = USE_Y + 56, bw = rx - tx, bh = 20;
	gfx_round_rect(bx, by, bw, bh, 6, C_TRACK);
	if (!shown) {
		uint64_t total = DRV->live_disk ? DRV->live_disk : DRV->live_total;
		if (total > DRV->live_free) {
			kit_fmt_size(total - DRV->live_free, a, sizeof(a));
			kit_fmt_size(total, b, sizeof(b));
			snprintf(line, sizeof(line), tr("%s used of %s"), a, b);
			font_draw_right(FONT_MEDIUM, 20, rx, USE_Y + 19, line, C_TEXT2);
			gfx_round_rect(bx, by, (int)((double)bw * (total - DRV->live_free) / total), bh, 6, 0x60ffffff);
		}
		return;
	}
	uint64_t total = scan_drive_total(shown), used = scan_drive_used(shown);
	char pct[16];
	kit_fmt_size(used, a, sizeof(a));
	kit_fmt_size(total, b, sizeof(b));
	ui_fmt_pct(used, total, pct, sizeof(pct));
	snprintf(line, sizeof(line), tr("%s used of %s  \xc2\xb7  %s"), a, b, pct);
	font_draw_right(FONT_MEDIUM, narrow ? 19 : 20, rx, USE_Y + 19, line, C_TEXT);
	// the categories along the bar, in order, a hair apart (the picked one brighter)
	double at = 0;
	for (int k = 0; k < NUM_CATS && total; k++) {
		uint64_t v = shown->cat_bytes[k];
		if (!v) continue;
		int x0 = bx + (int)(bw * at / total), x1 = bx + (int)(bw * (at + v) / total);
		at += v;
		if (x1 - x0 < 2) continue;
		uint32_t c = k == sel ? cat_colors[k] : with_alpha(cat_colors[k], 0xb0);
		gfx_round_rect(x0, by, x1 - x0 - 2, bh, (x1 - x0) > 12 ? 6 : 1, c);
	}
	// under it: what was found, and what's free
	char files[24], dirs[24], fr[32];
	kit_fmt_count((long long)shown->files, files, sizeof(files));
	kit_fmt_count((long long)shown->dirs, dirs, sizeof(dirs));
	snprintf(line, sizeof(line), tr("%s files in %s folders"), files, dirs);
	if (shown->unreadable) {
		size_t l = strlen(line);
		snprintf(line + l, sizeof(line) - l, tr("  \xc2\xb7  %llu couldn't be read (skipped)"), (unsigned long long)shown->unreadable);
	}
	font_draw(FONT_REGULAR, 17, tx, by + bh + 10, line, shown->unreadable ? C_WARN : C_DIM, bw - 220);
	kit_fmt_size(shown->free_bytes, fr, sizeof(fr));
	snprintf(line, sizeof(line), shown->total || shown->disk ? tr("%s free") : tr("%s free  \xc2\xb7  drive size unknown"), fr);
	font_draw_right(FONT_MEDIUM, 17, rx, by + bh + 10, line, C_TEXT2);
}

// a category with no size because the drive wouldn't say (its partitions, or what's unaccounted)
static int Unknown(const scan_t *s, int k)
{
	if (k == CAT_RESERVED) return !s->disk || !s->total;
	if (k == CAT_UNACCOUNTED) return !s->total && !s->disk;
	return 0;
}

// the ring, with the picked category's size in its middle
#define LOW_Y	228
#define LOW_H	(HINT_Y - 18 - LOW_Y)
static void Draw_Ring(int x, int w)
{
	scan_t *shown = DRV->shown;
	gfx_round_rect(x, LOW_Y, w, LOW_H, 14, C_PANEL);
	int d = w - 40;
	if (d > LOW_H - 30) d = LOW_H - 30;
	int rx = x + (w - d) / 2, ry = LOW_Y + (LOW_H - d) / 2;
	if (ring_gen != shown_gen || ring_drive != cur_drive || ring_sel != sel || ring_d != d) {
		ring_seg_t segs[NUM_CATS];
		for (int k = 0; k < NUM_CATS; k++) {
			segs[k].v = shown->cat_bytes[k];
			segs[k].color = cat_colors[k];
		}
		ring = ring_build(ring, d, segs, NUM_CATS, scan_drive_total(shown), C_TRACK, sel);
		ring_gen = shown_gen;
		ring_drive = cur_drive;
		ring_sel = sel;
		ring_d = d;
	}
	if (ring) gfx_image(ring, rx, ry, d, d, 0, 255);
	// the middle: the category, its size, its part of the drive
	char lines[2][256], size[32], pct[16], line[64];
	int cx = rx + d / 2, inner = (int)(d * 0.64f * 0.80f);
	int n = kit_wrap(FONT_MEDIUM, 17, tr(scan_cat_name(sel)), inner, lines, 2);
	kit_fmt_size(shown->cat_bytes[sel], size, sizeof(size));
	if (Unknown(shown, sel)) snprintf(size, sizeof(size), "-");
	ui_fmt_pct(shown->cat_bytes[sel], scan_drive_total(shown), pct, sizeof(pct));
	int y = ry + d / 2 - (n * 22 + 44 + 26) / 2;
	for (int i = 0; i < n; i++) font_draw_center(FONT_MEDIUM, 17, cx, y + i * 22, lines[i], C_TEXT2, inner);
	y += n * 22 + 4;
	font_draw_center(FONT_BOLD, 30, cx, y, size, C_TEXT, inner);
	y += 42;
	if (sel >= NUM_SCANNED_CATS) snprintf(line, sizeof(line), "%s", Unknown(shown, sel) ? "" : pct);
	else snprintf(line, sizeof(line), tr(shown->cat_items[sel] == 1 ? "%s  \xc2\xb7  %u item" : "%s  \xc2\xb7  %u items"), pct, (unsigned)shown->cat_items[sel]);
	kit_draw_fit_center(FONT_MEDIUM, 17, 13, cx, y, line, cat_colors[sel], inner);		// (smaller rather than cut: "203 elementos")
}

#define ROW_H	((LOW_H - 16) / NUM_CATS)
static void Draw_Cats(int x, int w)
{
	scan_t *shown = DRV->shown;
	gfx_round_rect(x, LOW_Y, w, LOW_H, 14, C_PANEL);
	int narrow = screen_narrow(), px = narrow ? 19 : 20;
	int rx = x + w - 16, pct_w = narrow ? 64 : 74;
	uint64_t total = scan_drive_total(shown);
	for (int k = 0; k < NUM_CATS; k++) {
		int y = LOW_Y + 8 + k * ROW_H;
		if (k == sel) gfx_round_rect(x + 6, y, w - 12, ROW_H - 2, 10, C_ROW_ON);
		int cy = y + (ROW_H - 2) / 2;
		gfx_round_rect(x + 22, cy - 7, 14, 14, 4, cat_colors[k]);
		char size[32], pct[16];
		uint64_t v = shown->cat_bytes[k];
		kit_fmt_size(v, size, sizeof(size));
		ui_fmt_pct(v, total, pct, sizeof(pct));
		if (Unknown(shown, k)) { snprintf(size, sizeof(size), "-"); pct[0] = 0; }
		font_draw_right(FONT_REGULAR, px - 2, rx, cy - (px - 2) / 2, pct, C_DIM);
		int sw = font_draw_right(FONT_MEDIUM, px, rx - pct_w, cy - px / 2, size, k == sel ? C_TEXT : C_TEXT2);
		int nx = x + 50;
		font_draw(FONT_MEDIUM, px, nx, cy - px / 2, tr(scan_cat_name(k)), v || k == sel ? C_TEXT : C_DIM, rx - pct_w - sw - 20 - nx);
	}
}

// no scan of this drive to show yet: how its first one's going, filling the lower half
static void Draw_First_Scan(void)
{
	int x = LIST_X - 12, w = LIST_W + 24;
	gfx_round_rect(x, LOW_Y, w, LOW_H, 14, C_PANEL);
	scan_progress_t *p = scan_progress();
	int cx = SCREEN_W / 2, y = LOW_Y + LOW_H / 2 - 90;
	char path[160], line[200], a[32], b[32], files[24];
	if (scan_drive != cur_drive) {
		snprintf(line, sizeof(line), tr("No scan of %s yet. Press Triangle to scan it."), ui_drive_name(cur_drive));
		font_draw_center(FONT_MEDIUM, 22, cx, y + 60, line, C_DIM, w - 80);
		if (scan_drive >= 0) {
			snprintf(line, sizeof(line), tr("(%s is being scanned: one at a time.)"), ui_drive_name(scan_drive));
			font_draw_center(FONT_REGULAR, 18, cx, y + 100, line, C_DIM, w - 80);
		}
		return;
	}
	snprintf(line, sizeof(line), tr("Scanning %s"), ui_drive_name(cur_drive));
	font_draw_center(FONT_BOLD, 30, cx, y, line, C_TEXT, w - 80);
	spin_lock(&p->progress_lock);
	snprintf(path, sizeof(path), "%s", p->path);
	spin_unlock(&p->progress_lock);
	font_draw_center(FONT_REGULAR, 18, cx, y + 50, path, C_DIM, w - 80);
	int bw = w - 160, bx = cx - bw / 2, by = y + 90;
	int pct = ui_scan_pct();
	double frac = pct < 0 ? 0 : pct / 100.0;
	gfx_round_rect(bx, by, bw, 14, 7, C_TRACK);
	if (pct >= 0) gfx_round_rect(bx, by, (int)(bw * frac) > 14 ? (int)(bw * frac) : 14, 14, 7, C_ACCENT);
	kit_fmt_size((long long)p->bytes, a, sizeof(a));
	kit_fmt_size((long long)p->used, b, sizeof(b));
	kit_fmt_count((long long)p->files, files, sizeof(files));
	if (pct >= 0) snprintf(line, sizeof(line), tr("%d%%  \xc2\xb7  %s of %s  \xc2\xb7  %s files"), pct, a, b, files);
	else snprintf(line, sizeof(line), tr("%s  \xc2\xb7  %s files"), a, files);
	font_draw_center(FONT_MEDIUM, 20, cx, by + 30, line, C_TEXT2, w - 80);
	font_draw_center(FONT_REGULAR, 17, cx, by + 66, tr("The first scan of a full HDD can take a few minutes."), C_DIM, w - 80);
}

// a scan going on, bottom right (as PS3 World's download strip): its text, and where it starts
static double Strip(char *line, int size, int *left)
{
	scan_progress_t *p = scan_progress();
	int right = SCREEN_W - LIST_X, bw = screen_narrow() ? 90 : 120;
	int pct = ui_scan_pct();
	char files[24], count[48], head[64];
	const char *name = scan_drive ? ui_drive_name(scan_drive) : "HDD";
	kit_fmt_count((long long)p->files, files, sizeof(files));
	if (pct >= 0 && screen_narrow()) snprintf(line, size, tr("Scanning  %d%%"), pct);
	else if (pct >= 0) snprintf(line, size, tr("Scanning %s  %d%%  \xc2\xb7  %s files"), name, pct, files);
	else {		// (no percentage to give: the files found so far)
		snprintf(count, sizeof(count), tr("%s files"), files);
		if (screen_narrow()) snprintf(line, size, "%s", count);
		else {
			snprintf(head, sizeof(head), tr("Scanning %s"), name);
			snprintf(line, size, "%s  \xc2\xb7  %s", head, count);
		}
	}
	*left = right - bw - 14 - font_width(FONT_MEDIUM, 19, line);
	return pct < 0 ? 0 : pct / 100.0;
}

int ui_hints_max(void)
{
	char line[96];
	int left;
	if (scan_drive < 0) return SCREEN_W - LIST_X;
	Strip(line, sizeof(line), &left);
	return left - 24;
}

void ui_draw_strip(void)
{
	if (scan_drive < 0) return;
	char line[96];
	int left, right = SCREEN_W - LIST_X, bw = screen_narrow() ? 90 : 120, y = HINT_Y + 3;
	double frac = Strip(line, sizeof(line), &left);
	gfx_round_rect(right - bw, y + 9, bw, 8, 4, 0x40ffffff);
	gfx_round_rect(right - bw, y + 9, (int)(bw * frac), 8, 4, C_ACCENT);
	font_draw_right(FONT_MEDIUM, 19, right - bw - 14, y, line, C_TEXT);
}

static void Render_Screen(void);

// the internal drive's read errors: what happened, and that Drive health shows why
static void Draw_Read_Errors(void)
{
	char lines[8][256], body[400];
	int w = LIST_W + 24, x = (SCREEN_W - w) / 2, tw = w - 96, cx = SCREEN_W / 2;
	if (stuck_kind == STUCK_READ_ERRORS)
		snprintf(body, sizeof(body), tr("%s gave read errors, so the scan skipped what it couldn't read. That's usually a sign of bad sectors."),
			ui_drive_name(0));
	else snprintf(body, sizeof(body), tr("%s gave read errors, so the scan couldn't finish. That's usually a sign of bad sectors."), ui_drive_name(0));
	int n1 = kit_wrap(FONT_REGULAR, 20, body, tw, lines, 4);
	int n2 = kit_wrap(FONT_REGULAR, 20, tr("Drive health shows whether the drive has bad sectors, and what to do if it has."), tw, lines + n1, 8 - n1);
	int h = 70 + n1 * 30 + 14 + n2 * 30 + 30, y = (SCREEN_H - h) / 2 - 20;
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	font_draw_center(FONT_BOLD, 28, cx, y + 22, stuck_kind == STUCK_READ_ERRORS ? tr("Some of the drive couldn't be read") : tr("The scan didn't finish"), C_WARN, tw);
	int ly = y + 76;
	for (int i = 0; i < n1; i++, ly += 30) font_draw_center(FONT_REGULAR, 20, cx, ly, lines[i], C_TEXT2, tw);
	ly += 14;
	for (int i = 0; i < n2; i++, ly += 30) font_draw_center(FONT_REGULAR, 20, cx, ly, lines[n1 + i], C_CYAN, tw);
	const kit_hint_t hints[] = {{G_CROSS, -1, tr("OK"), 0}, {G_TRIANGLE, -1, tr("Drive health"), 0}};
	kit_hints(hints, 2, LIST_X, ui_hints_max());
}

// a scan given up on: what happened, where, and where to send the log (ps3.world's contact page: only
// said, HDD Atlas has no network)
static void Draw_Stuck(void)
{
	char lines[12][256], body[600];
	int w = LIST_W + 24, x = (SCREEN_W - w) / 2, tw = w - 96, cx = SCREEN_W / 2;
	kit_backdrop();		// (the screen under it, hints and all, out of the way)
	if (stuck_kind != STUCK_GIVEN_UP) { Draw_Read_Errors(); return; }
	snprintf(body, sizeof(body), tr("%s didn't answer for 5 minutes, so the scan was given up. That's usually the drive failing to read here:"),
		ui_drive_name(stuck_drive));
	int n1 = kit_wrap(FONT_REGULAR, 20, body, tw, lines, 4);
	int n2 = kit_wrap(FONT_REGULAR, 20, tr("Please send hddatlas_log.txt, in /dev_hdd0/game/HDAT00001/USRDIR, to ps3.world/contact "
		"(hddatlas_log_prev.txt if HDD Atlas has been started again since). Another scan may not start till the console's restarted."),
		tw, lines + n1, 12 - n1 - 1);
	int h = 70 + n1 * 30 + 44 + n2 * 30 + 14 + 2 * 30 + 30, y = (SCREEN_H - h) / 2 - 20;		// (and up to 2 lines of why)
	gfx_round_rect(x, y, w, h, 14, C_PANEL);
	font_draw_center(FONT_BOLD, 28, cx, y + 22, tr("The scan got stuck"), C_WARN, tw);
	int ly = y + 76;
	for (int i = 0; i < n1; i++, ly += 30) font_draw_center(FONT_REGULAR, 20, cx, ly, lines[i], C_TEXT2, tw);
	font_draw_center(FONT_MEDIUM, 18, cx, ly + 4, stuck_at, C_TEXT, tw);
	ly += 44;
	for (int i = 0; i < n2; i++, ly += 30) font_draw_center(FONT_REGULAR, 20, cx, ly, lines[n1 + i], C_TEXT2, tw);
	// why: the internal drive's health (Triangle opens it); a USB drive's isn't read here
	int n3 = kit_wrap(FONT_REGULAR, 20, stuck_drive == 0 ? tr("Drive health shows whether the drive has bad sectors, and what to do if it has.") :
		tr("Check this drive's health on a PC (its SMART data) before trusting it with more."), tw, lines, 3);
	for (int i = 0; i < n3; i++) font_draw_center(FONT_REGULAR, 20, cx, ly + 14 + i * 30, lines[i], C_CYAN, tw);
	const kit_hint_t hints[] = {{G_CROSS, -1, tr("OK"), 0}, {G_TRIANGLE, -1, stuck_drive == 0 ? tr("Drive health") : NULL, 0}};
	kit_hints(hints, 2, LIST_X, ui_hints_max());
}

void ui_render(void)
{
	dirty = 0;
	gfx_mirror(lang_current() == LANG_AR);		// Arabic: the whole screen right to left (as PS3 World)
	if (menu_fullscreen()) menu_render();
	else {
		if (in_home) home_render();
		else if (in_list) list_render();
		else if (in_map) map_render();
		else if (in_health) health_render();
		else Render_Screen();
		if (menu_active()) menu_render();
	}
	// a note, on the lists, Drive health and Settings (the overview has it in its status line, if it fits there)
	int overview = !in_home && !in_list && !in_map && !in_health && !menu_active();
	if ((in_home || in_list || in_health || menu_fullscreen() || (overview && !Note_In_Status())) && note[0] && !delete_active()) {
		int w = font_width(FONT_MEDIUM, 19, note) + 44, maxw = SCREEN_W - 2 * LIST_X;
		if (w > maxw) w = maxw;
		int x = (SCREEN_W - w) / 2, y = HINT_Y - 62;
		gfx_round_rect(x, y, w, 42, 21, 0xf00a1622);
		gfx_round_rect(x, y, w, 42, 21, 0x30ffc861);
		font_draw_center(FONT_MEDIUM, 19, SCREEN_W / 2, kit_text_y(FONT_MEDIUM, 19, y, 42), note, C_WARN, w - 40);
	}
	if (delete_active()) delete_render();
	if (home_intro_active()) home_intro_draw();		// (over the start screen, fading into it)
	if (stuck_drive >= 0 && !home_intro_active()) Draw_Stuck();
	if (dimmed) {
		gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xf4000000);
		uint64_t m = plat_time_us() / 60000000ULL;
		int x = 60 + (int)((m * 389) % (uint64_t)(SCREEN_W - 260)), y = 60 + (int)((m * 241) % (uint64_t)(SCREEN_H - 140));
		font_draw(FONT_MEDIUM, 18, x, y, APP_NAME, 0x60ffffff, 0);
	}
}

static void Render_Screen(void)
{
	scan_t *shown = DRV->shown;
	kit_backdrop();
	kit_top(tr("Overview"), 0);		// (the logo says HDD Atlas)
	font_draw_right(FONT_REGULAR, 16, SCREEN_W - LIST_X, 40, APP_NAME " " APP_VERSION, C_DIM);
	Draw_Status();
	Draw_Usage();
	if (shown) {
		int ring_w = screen_narrow() ? 312 : 392;
		Draw_Ring(LIST_X - 12, ring_w);
		Draw_Cats(LIST_X - 12 + ring_w + 16, LIST_W + 24 - ring_w - 16);
	} else {
		Draw_First_Scan();
	}
	int narrow = screen_narrow();
	const kit_hint_t hints[] = {
		{G_CROSS, -1, Openable(sel) ? tr("Open") : shown && sel >= NUM_SCANNED_CATS ? tr("What's this") : NULL, 0},
		{G_SQUARE, -1, shown ? (narrow ? tr("Largest") : tr("Largest items")) : NULL, 1},
		{G_TRIANGLE, -1, scan_drive < 0 ? (shown ? tr("Rescan") : tr("Scan")) : NULL, 0},
		{G_CIRCLE, -1, scan_drive >= 0 ? tr("Stop scan") : tr("Back"), 0},
		{G_START, -1, scan_drive < 0 ? tr("Menu") : NULL, 0},
		{G_R3, -1, music_on() ? tr("Music off") : tr("Music on"), 3},
		{G_L1, G_R1, Drives_Present() > 1 ? tr("Drive") : NULL, 1},
	};
	kit_hints(hints, sizeof(hints) / sizeof(hints[0]), LIST_X, ui_hints_max());
	ui_draw_strip();
	if (explain >= 0 && shown) Draw_Explain(shown);
}
