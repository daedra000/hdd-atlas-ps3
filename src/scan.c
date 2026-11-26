/*
scan.c: the scanner (scan.h), its cache, and the log of what it found.

Read-only on the drive: it opens folders, lists them and asks what's free.
The only file it writes is its cache, in USRDIR (plat_save_dir).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stddef.h>
#include <errno.h>
#include "plat.h"
#include "lang.h"
#include "fsx.h"
#include "sfo.h"
#include "iso.h"
#include "scan.h"
#include "bin.h"

#define CACHE_MAGIC		"HDAT"
#define CACHE_VERSION	7		// 2: titles, icons, folder sizes, the whole disk's size; 3: file types, disc images' titles; 4: items' newest times, disc images' icons; 5: each type's largest files; 6: updater/'s .pup files their own items; 7: .p3t and .pkg files at the top in Themes and Packages
#define SMALL_FILE		(64 * 1024)		// a PARAM.SFO or TROPCONF.SFM bigger than this isn't read
#define BATCH			32			// entries asked for at a time
#define LOG_UNREADABLE	50			// folders that couldn't be read, logged at most
#define PROGRESS_US		50000		// the path on screen, refreshed at most this often
#ifndef DIR_ENTRIES_MAX
#define DIR_ENTRIES_MAX	200000		// entries listed in one folder at most: past it the folder's taken as damaged (a
									// FAT32 folder whose chain loops lists for ever), counted unreadable (a test sets it lower)
#endif

static scan_progress_t prog;
static scan_t *volatile done_scan;		// finished, waiting for scan_take
static volatile int stopped;
static volatile int thread_live;
static char scan_root[64];
static char scan_bin[FSX_PATH];		// its recycle bin, under it (its files: never among the largest)
static int scan_cat = -1;				// a partial scan's category (-1: the whole drive)
static const scan_t *scan_base;			// and the scan it's merged into

static const char *const cat_names[NUM_CATS] = {
	N_("Games & game data"), N_("Disc game backups"), N_("Saves, trophies & licences"), N_("Themes"),
	N_("Music, video & photos"), N_("Packages, tmp & caches"), N_("System & other folders"), N_("System partitions"), N_("Unaccounted"),
};
static const char *const cat_short[NUM_CATS] = {
	N_("Games"), N_("Backups"), N_("Saves"), N_("Themes"), N_("Media"), N_("Packages"), N_("System"), N_("Partitions"), N_("Unaccounted"),
};

const char *scan_cat_name(int cat) { return cat >= 0 && cat < NUM_CATS ? cat_names[cat] : "?"; }
const char *scan_cat_short(int cat) { return cat >= 0 && cat < NUM_CATS ? cat_short[cat] : "?"; }

/*
==================
The category map: a folder at the top of the drive -> its category. Folders not
here are System & other (each one an item); every scan logs what it found at the
top, so a folder this doesn't know shows up in the log rather than going missing.
==================
*/
static const struct { const char *name; int cat; } top_map[] = {
	{"game", CAT_GAMES},
	{"GAMES", CAT_BACKUPS}, {"GAMEZ", CAT_BACKUPS}, {"PSXGAMES", CAT_BACKUPS}, {"PS2DISC", CAT_BACKUPS}, {"PS3ISO", CAT_BACKUPS}, {"PS2ISO", CAT_BACKUPS}, {"PSXISO", CAT_BACKUPS},
	{"PSPISO", CAT_BACKUPS}, {"BDISO", CAT_BACKUPS}, {"DVDISO", CAT_BACKUPS}, {"ROMS", CAT_BACKUPS},
	{"home", CAT_SAVES}, {"savedata", CAT_SAVES}, {"exdata", CAT_SAVES},
	{"theme", CAT_THEMES},
	{"music", CAT_MEDIA}, {"video", CAT_MEDIA}, {"photo", CAT_MEDIA},
	{"MUSIC", CAT_MEDIA}, {"VIDEO", CAT_MEDIA}, {"PICTURE", CAT_MEDIA},		// (a USB drive's, where the XMB copies them)
	{"packages", CAT_PKGTMP}, {"tmp", CAT_PKGTMP},
};
#define NUM_TOP	((int)(sizeof(top_map) / sizeof(top_map[0])))

static int Ends_With(const char *s, const char *end)
{
	size_t a = strlen(s), b = strlen(end);
	return a >= b && !strcasecmp(s + a - b, end);
}

int scan_cat_of(const char *top, int is_dir)
{
	if (!is_dir) {		// (webMAN's screenshots land at the top of the drive; themes and packages copied there to install)
		if (!strncmp(top, "screenshot_", 11) && (Ends_With(top, ".bmp") || Ends_With(top, ".png") || Ends_With(top, ".jpg"))) return CAT_MEDIA;
		if (Ends_With(top, ".p3t")) return CAT_THEMES;
		if (Ends_With(top, ".pkg")) return CAT_PKGTMP;
		return CAT_SYSTEM;
	}
	for (int i = 0; i < NUM_TOP; i++)
		if (!strcmp(top_map[i].name, top)) return top_map[i].cat;
	return CAT_SYSTEM;
}

uint64_t scan_drive_total(const scan_t *s)
{
	return s->disk ? s->disk : s->total ? s->total : s->scanned + s->free_bytes;
}

uint64_t scan_drive_used(const scan_t *s)
{
	if (!s->disk && !s->total) return s->scanned;
	uint64_t t = scan_drive_total(s);
	return t > s->free_bytes ? t - s->free_bytes : 0;
}

/*
==================
Folder sizes: every folder found, its parent's index and its name; files add to
their own folder as they're found, and each folder's is added to its parent's at
the end (a folder always comes after its parent, so once, last to first).
==================
*/
int scan_dir_child(const scan_t *s, int dir, const char *name)
{
	if (!s->first_child || dir < 0 || (uint32_t)dir >= s->num_dirs) return -1;
	for (uint32_t c = s->first_child[dir]; c != UINT32_MAX; c = s->next_sibling[c])
		if (!strcmp(s->names + s->dir[c].name, name)) return (int)c;
	return -1;
}

int scan_dir_find(const scan_t *s, const char *rel)
{
	if (!s->num_dirs) return -1;
	int d = 0;
	char part[FSX_NAME];
	while (*rel && d >= 0) {
		const char *slash = strchr(rel, '/');
		size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
		if (n >= sizeof(part)) return -1;
		memcpy(part, rel, n);
		part[n] = 0;
		if (n) d = scan_dir_child(s, d, part);
		rel += n + (slash ? 1 : 0);
	}
	return d;
}

static int Under_Or_Is(const char *path, const char *rel)
{
	size_t n = strlen(rel);
	return !strncmp(path, rel, n) && (path[n] == 0 || path[n] == '/');
}

// what a path under the drive is counted in: its top folder's category (a file at the top: its own)
static int Cat_Of_Path(const char *rel)
{
	char top[FSX_NAME];
	const char *slash = strchr(rel, '/');
	size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
	if (n >= sizeof(top)) return CAT_SYSTEM;
	memcpy(top, rel, n);
	top[n] = 0;
	return scan_cat_of(top, slash != NULL);
}

// every folder under d emptied (walked through the index: no stack, no recursion)
static void Empty_Under(scan_t *s, uint32_t d)
{
	if (!s->first_child) return;
	uint32_t c = s->first_child[d];
	while (c != UINT32_MAX) {
		s->dir[c].bytes = 0;
		s->dir[c].files = s->dir[c].dirs = 0;
		if (s->first_child[c] != UINT32_MAX) { c = s->first_child[c]; continue; }
		while (c != d && s->next_sibling[c] == UINT32_MAX) c = s->dir[c].parent;
		c = c == d ? UINT32_MAX : s->next_sibling[c];
	}
}

void scan_forget(scan_t *s, const char *rel, uint64_t bytes, uint32_t files, int whole, uint64_t free_now)
{
	// its item: the one it is, or the one it's in; past ITEMS_MAX, its category's "more items" row
	// (the closest: the recycle bin's an item inside another's folder)
	int found = 0;
	uint32_t i = UINT32_MAX;
	size_t best = 0;
	for (uint32_t k = 0; k < s->num_items; k++) {
		const scan_item_t *it = &s->items[k];
		if (it->kind != ITEM_REST && Under_Or_Is(rel, it->path) && (i == UINT32_MAX || strlen(it->path) > best)) { i = k; best = strlen(it->path); }
	}
	if (i != UINT32_MAX) {
		scan_item_t *it = &s->items[i];
		found = 1;
		uint64_t b = bytes < it->bytes ? bytes : it->bytes;
		it->bytes -= b;
		it->files -= files < it->files ? files : it->files;
		s->cat_bytes[it->cat] -= b < s->cat_bytes[it->cat] ? b : s->cat_bytes[it->cat];
		if (!strcmp(it->path, rel)) {		// the item itself: gone
			s->cat_items[it->cat]--;
			memmove(it, it + 1, (s->num_items - i - 1) * sizeof(*it));
			s->num_items--;
		}
	}
	if (!found) {
		int cat = Cat_Of_Path(rel);
		for (uint32_t i = 0; i < s->num_items; i++) {
			scan_item_t *it = &s->items[i];
			if (it->kind != ITEM_REST || it->cat != cat) continue;
			uint64_t b = bytes < it->bytes ? bytes : it->bytes;
			it->bytes -= b;
			it->files -= files < it->files ? files : it->files;
			s->cat_bytes[cat] -= b < s->cat_bytes[cat] ? b : s->cat_bytes[cat];
			break;
		}
	}
	// its folders, from where it was up to the top (a folder deleted is left in the index, empty,
	// for the folder screen won't list it: what's there is listed live)
	char parent[FSX_PATH];
	snprintf(parent, sizeof(parent), "%s", rel);
	int d = scan_dir_find(s, parent);
	uint32_t gone_dirs = 0;		// (folders that went, whole: off the counts above)
	if (d > 0 && whole) {
		gone_dirs = s->dir[d].dirs + 1;
		Empty_Under(s, (uint32_t)d);
		s->dir[d].bytes = 0;
		s->dir[d].files = s->dir[d].dirs = 0;
		s->dirs -= gone_dirs < s->dirs ? gone_dirs : s->dirs;
		d = (int)s->dir[d].parent;
	} else if (d < 0) {		// a file: its folder
		char *slash = strrchr(parent, '/');
		if (slash) *slash = 0; else parent[0] = 0;
		d = scan_dir_find(s, parent);
	}
	for (int guard = 0; d >= 0 && guard < 4096; guard++) {
		scan_dir_t *dr = &s->dir[d];
		dr->bytes -= bytes < dr->bytes ? bytes : dr->bytes;
		dr->files -= files < dr->files ? files : dr->files;
		dr->dirs -= gone_dirs < dr->dirs ? gone_dirs : dr->dirs;
		if (d == 0) break;
		d = (int)dr->parent;
	}
	// the largest files that were it or in it
	uint32_t k = 0;
	for (uint32_t i = 0; i < s->num_big; i++)
		if (!Under_Or_Is(s->big[i].path, rel)) s->big[k++] = s->big[i];
	s->num_big = k;
	k = 0;
	for (uint32_t i = 0; i < s->num_tfiles; i++)
		if (!Under_Or_Is(s->tfiles[i].path, rel)) s->tfiles[k++] = s->tfiles[i];
	s->num_tfiles = k;
	s->scanned -= bytes < s->scanned ? bytes : s->scanned;
	s->files -= files < s->files ? files : s->files;
	// what's free now, so the used space and Unaccounted follow
	if (free_now) {
		s->free_bytes = free_now;
		s->used = s->total > free_now ? s->total - free_now : 0;
		uint64_t used = s->total ? s->used : scan_drive_used(s);
		s->cat_bytes[CAT_UNACCOUNTED] = used > s->scanned ? used - s->scanned : 0;
	}
	s->edits++;
}

void scan_bin_change(scan_t *s, int64_t bytes, int32_t files, uint64_t free_now)
{
	char bin[FSX_PATH];
	if (!bytes && !files) return;
	if (!bin_rel(s->root, bin, sizeof(bin)) || strlen(bin) >= ITEM_PATH) return;
	uint64_t ub = (uint64_t)(bytes < 0 ? -bytes : bytes);
	uint32_t uf = (uint32_t)(files < 0 ? -files : files);
	int add = bytes > 0 || (!bytes && files > 0);
	// its item (made the first time something's moved in: the bin wasn't there when it scanned)
	int it = -1;
	for (uint32_t i = 0; i < s->num_items && it < 0; i++) if (scan_item_is_bin(&s->items[i])) it = (int)i;
	if (it < 0 && add && s->num_items < ITEMS_MAX + NUM_CATS) {
		scan_item_t *more = realloc(s->items, (s->num_items + 1) * sizeof(*more));
		if (more) {
			char top[FSX_NAME];
			snprintf(top, sizeof(top), "%.*s", (int)strcspn(bin, "/"), bin);
			s->items = more;
			scan_item_t *b = &s->items[s->num_items];
			memset(b, 0, sizeof(*b));
			snprintf(b->path, sizeof(b->path), "%s", bin);
			snprintf(b->title, sizeof(b->title), "%s", "Recycle bin");
			snprintf(b->code, sizeof(b->code), "%s", SCAN_BIN_CODE);
			b->cat = (uint8_t)scan_cat_of(top, 1);
			b->kind = ITEM_DIR;
			b->icon = ICON_NONE;
			b->dir = scan_dir_find(s, bin);
			if (b->dir == 0) b->dir = -1;
			it = (int)s->num_items++;
			s->cat_items[b->cat]++;
		}
	}
	if (it >= 0) {
		scan_item_t *x = &s->items[it];
		if (add) { x->bytes += ub; x->files += uf; s->cat_bytes[x->cat] += ub; }
		else {
			uint64_t b = ub < x->bytes ? ub : x->bytes;
			x->bytes -= b;
			x->files -= uf < x->files ? uf : x->files;
			s->cat_bytes[x->cat] -= b < s->cat_bytes[x->cat] ? b : s->cat_bytes[x->cat];
		}
	}
	// its folder (or the nearest above it that's kept) and every one above
	char p[FSX_PATH];
	snprintf(p, sizeof(p), "%s", bin);
	int d = scan_dir_find(s, p);
	while (d < 0 && p[0]) { char *sl = strrchr(p, '/'); if (sl) *sl = 0; else p[0] = 0; d = scan_dir_find(s, p); }
	for (int guard = 0; d >= 0 && guard < 4096; guard++) {
		scan_dir_t *dr = &s->dir[d];
		if (add) { dr->bytes += ub; dr->files += uf; }
		else { dr->bytes -= ub < dr->bytes ? ub : dr->bytes; dr->files -= uf < dr->files ? uf : dr->files; }
		if (d == 0) break;
		d = (int)dr->parent;
	}
	if (add) { s->scanned += ub; s->files += uf; }
	else { s->scanned -= ub < s->scanned ? ub : s->scanned; s->files -= uf < s->files ? uf : s->files; }
	if (free_now) {
		s->free_bytes = free_now;
		s->used = s->total > free_now ? s->total - free_now : 0;
		uint64_t used = s->total ? s->used : scan_drive_used(s);
		s->cat_bytes[CAT_UNACCOUNTED] = used > s->scanned ? used - s->scanned : 0;
	}
	if (!add) s->edits++;		// (gone for good: a delete; a move in is counted by scan_forget's)
}

// first_child / next_sibling from the parents (children in the order they were found)
static int Index_Dirs(scan_t *s)
{
	free(s->first_child);
	free(s->next_sibling);
	s->first_child = s->next_sibling = NULL;
	if (!s->num_dirs) return 1;
	s->first_child = malloc(s->num_dirs * sizeof(uint32_t));
	s->next_sibling = malloc(s->num_dirs * sizeof(uint32_t));
	if (!s->first_child || !s->next_sibling) {
		free(s->first_child);
		free(s->next_sibling);
		s->first_child = s->next_sibling = NULL;
		return 0;
	}
	memset(s->first_child, 0xff, s->num_dirs * sizeof(uint32_t));
	memset(s->next_sibling, 0xff, s->num_dirs * sizeof(uint32_t));
	for (uint32_t i = s->num_dirs - 1; i >= 1; i--) {
		uint32_t p = s->dir[i].parent;
		s->next_sibling[i] = s->first_child[p];
		s->first_child[p] = i;
	}
	return 1;
}

/*
==================
The recycle bin: an item of its own, not the one whose folder it's in (on the HDD,
HDD Atlas's: what's in the bin isn't HDD Atlas's size), and its files not among the
largest files (they're deleted; File types still counts them: they're there). Once
the folder index is made; again does nothing.
==================
*/
static int In_Bin(const char *path, const char *bin) { return Under_Or_Is(path, bin) && strcmp(path, bin); }

// path, a folder inside another item's, made an item of its own (code: what it is); nothing if it's
// one already, isn't there, is empty, or there's no room
static void Split_At(scan_t *s, const char *path, const char *title, const char *code)
{
	if (strlen(path) >= ITEM_PATH || s->num_items >= ITEMS_MAX) return;
	int owner = -1;
	size_t best = 0;
	for (uint32_t i = 0; i < s->num_items; i++) {
		const scan_item_t *it = &s->items[i];
		if (it->kind == ITEM_REST) continue;
		if (!strcmp(it->path, path)) return;		// (split already)
		if (it->kind == ITEM_DIR && In_Bin(path, it->path) && strlen(it->path) >= best) { owner = (int)i; best = strlen(it->path); }
	}
	int d = scan_dir_find(s, path);
	if (owner < 0 || d <= 0 || !s->dir[d].bytes) return;
	scan_item_t *more = realloc(s->items, (s->num_items + 1) * sizeof(*more));
	if (!more) return;
	s->items = more;
	scan_item_t *o = &s->items[owner], *b = &s->items[s->num_items];
	memset(b, 0, sizeof(*b));
	snprintf(b->path, sizeof(b->path), "%s", path);
	snprintf(b->title, sizeof(b->title), "%s", title);
	snprintf(b->code, sizeof(b->code), "%s", code);
	b->bytes = s->dir[d].bytes < o->bytes ? s->dir[d].bytes : o->bytes;
	b->files = s->dir[d].files < o->files ? s->dir[d].files : o->files;
	b->dirs = s->dir[d].dirs + 1 < o->dirs ? s->dir[d].dirs + 1 : o->dirs;
	b->dir = d;
	b->cat = o->cat;
	b->kind = ITEM_DIR;
	b->icon = ICON_NONE;
	o->bytes -= b->bytes;
	o->files -= b->files;
	o->dirs -= b->dirs;
	s->num_items++;
	s->cat_items[b->cat]++;
}

/*
SingStar's songs, copied from its discs: FileSystem/Export, known by the config.xml and covers.xml
SingStar writes there (songs_*.xml, a folder a song). FileSystem with nothing else in it is called
that whole; with other things too (another console's FileSystem may hold more: only SingStar's was
seen), only Export is, an item of its own, and the rest stays "FileSystem" with its own size.
*/
static void Singstar(scan_t *s)
{
	char p[FSX_PATH + 64], q[FSX_PATH + 64];
	int it = -1, t;
	uint64_t z;
	for (uint32_t i = 0; i < s->num_items; i++) if (s->items[i].kind == ITEM_DIR && !strcmp(s->items[i].path, "FileSystem")) it = (int)i;
	if (it < 0) return;
	snprintf(p, sizeof(p), "%s/FileSystem/Export/config.xml", s->root);
	snprintf(q, sizeof(q), "%s/FileSystem/Export/covers.xml", s->root);
	if (fsx_stat(p, &t, &z) != 0 || t != FSX_FILE || fsx_stat(q, &t, &z) != 0 || t != FSX_FILE) return;
	int d = scan_dir_find(s, "FileSystem"), e = scan_dir_find(s, "FileSystem/Export");
	if (d > 0 && e > 0 && s->dir[e].bytes < s->dir[d].bytes) Split_At(s, "FileSystem/Export", "SingStar songs", SCAN_SONGS_CODE);
	else snprintf(s->items[it].title, sizeof(s->items[it].title), "%s", "SingStar songs");
}

/*
==================
Folders inside others' that are their own thing, an item each (their titles in English: the screens
show them in the language on screen). From the PS3 HDD structure report's sources:
  the recycle bin            HDD Atlas's own (its files not among the largest: they're deleted)
  apps' caches               IRISMAN's joined big files (cache/<ID>, GBs a game), Apollo's
                             downloads, multiMAN's covers: made again by their apps
  firmware                   a folder backup's PS3_UPDATE (the disc's system update): not for HEN
                             or CFW (an update waiting in updater/ is a file: Updater())
==================
*/
static void Split_All(scan_t *s)
{
	char bin[FSX_PATH];
	if (bin_rel(s->root, bin, sizeof(bin)) && strlen(bin) < ITEM_PATH) {
		uint32_t keep = 0;
		for (uint32_t i = 0; i < s->num_big; i++) if (!In_Bin(s->big[i].path, bin)) s->big[keep++] = s->big[i];
		s->num_big = keep;
		keep = 0;		// (and each type's largest files: a cache from before has the bin's among them)
		for (uint32_t i = 0; i < s->num_tfiles; i++) if (!In_Bin(s->tfiles[i].path, bin)) s->tfiles[keep++] = s->tfiles[i];
		s->num_tfiles = keep;
		Split_At(s, bin, "Recycle bin", SCAN_BIN_CODE);
	}
	static const struct { const char *path, *title; } caches[] = {
		{"game/IRISMAN00/cache", "IRISMAN's cache"},		// (seen on the console: the app's own folder, not USRDIR)
		{"game/NP0APOLLO/USRDIR/CACHE", "Apollo's cache"},
		{"game/BLES80608/USRDIR/covers", "multiMAN's covers"}, {"game/BLES80608/USRDIR/covers_retro", "multiMAN's covers"},
	};
	for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); i++) Split_At(s, caches[i].path, caches[i].title, SCAN_CACHE_CODE);
	Singstar(s);
	// (the items' count changes as they're split: the ones there at the start. Split_At moves the
	// items in memory: each one's read by its index, its path copied, before it)
	for (uint32_t i = 0, n = s->num_items; i < n; i++) {
		char p[FSX_PATH];
		if (s->items[i].kind != ITEM_DIR || s->items[i].code[0] == '~') continue;
		if (strncmp(s->items[i].path, "GAMES/", 6) && strncmp(s->items[i].path, "GAMEZ/", 6)) continue;
		if (snprintf(p, sizeof(p), "%s/PS3_UPDATE", s->items[i].path) < (int)sizeof(p)) Split_At(s, p, "Firmware update", SCAN_FIRMWARE_CODE);
	}
}

/*
==================
Building a scan
==================
*/
typedef struct {
	scan_t *s;
	uint32_t cap;
	int rest[NUM_CATS];			// each category's "more items" row, once ITEMS_MAX is reached (-1: none yet)
	fsx_ent_t *ents;			// BATCH of them
	// folders still to list: malloc'd paths, their index in dir (-1: not kept), and the kept folder
	// their files count in (their own, or the nearest above it that's kept: its size stays right)
	struct todo { char *path; int dir, acct; } *todo;
	int ntodo, captodo;
	uint32_t dir_cap, names_cap;
	int logged_unreadable;
	uint64_t last_progress;
	int out_of_memory;
	scan_type_t (*tt)[64];		// [NUM_SCANNED_CATS][64]: each category's file types while scanning
	int ntt[NUM_SCANNED_CATS];
	scan_big_t **th;			// [NUM_SCANNED_CATS * 64]: each of those types' largest files, a min-heap (made when it has one)
	uint32_t *thn;
} build_t;

static int Cancelled(void) { return prog.cancel; }

// where the scan is about to wait on the drive (scan_watch's)
static void At(const char *path)
{
	spin_lock(&prog.progress_lock);
	snprintf(prog.at, sizeof(prog.at), "%s", path);
	spin_unlock(&prog.progress_lock);
	prog.steps++;
}

static void Progress_Path(build_t *b, const char *path)
{
	uint64_t now = plat_time_us();
	if (now - b->last_progress < PROGRESS_US) return;
	b->last_progress = now;
	spin_lock(&prog.progress_lock);
	snprintf(prog.path, sizeof(prog.path), "%s", path);
	spin_unlock(&prog.progress_lock);
}

// a read the drive itself failed (EIO: lv2's CELL_FS_EIO, or -EIO from NTFS and the PC's), not a folder
// that's locked, went, or is deeper than lv2 can name
static int Read_Error(int err)
{
	return err == -EIO || (uint32_t)err == 0x8001002bu;
}

static void Unreadable(build_t *b, int item, const char *path, int err)
{
	b->s->unreadable++;
	prog.unreadable = b->s->unreadable;
	if (Read_Error(err)) prog.read_errors++;
	if (item >= 0) b->s->items[item].unreadable++;
	if (b->logged_unreadable++ < LOG_UNREADABLE)
		plat_log("scan: couldn't read %s (0x%08x): skipped", path, (unsigned)err);
	else if (b->logged_unreadable == LOG_UNREADABLE + 1)
		plat_log("scan: (more folders couldn't be read: only the first %d are logged)", LOG_UNREADABLE);
}

// a folder that listed DIR_ENTRIES_MAX entries and still had more: what was listed is kept, the
// rest isn't (counted unreadable: damaged, a FAT32 folder whose chain loops back on itself, usually)
static void Too_Many(build_t *b, int item, const char *path)
{
	plat_log("scan: %s listed %d entries and wasn't done: taken as damaged, the rest skipped", path, DIR_ENTRIES_MAX);
	Unreadable(b, item, path, -3);
}

// a folder found under parent (-1: parent isn't kept, nor is this); its index, or -1 when there's
// no more room (it, and what's under it, are then not kept: counted in dirs_dropped)
static int Dir_Add(build_t *b, int parent, const char *name)
{
	scan_t *s = b->s;
	if (parent < 0) return -1;
	size_t nl = strlen(name) + 1;
	if (s->num_dirs >= DIRS_MAX || s->names_len + nl > NAMES_MAX) { s->dirs_dropped++; return -1; }
	if (s->num_dirs == b->dir_cap) {
		uint32_t cap = b->dir_cap ? b->dir_cap * 2 : 1024;
		if (cap > DIRS_MAX) cap = DIRS_MAX;
		scan_dir_t *n = realloc(s->dir, (size_t)cap * sizeof(*n));
		if (!n) { s->dirs_dropped++; return -1; }
		s->dir = n;
		b->dir_cap = cap;
	}
	if (s->names_len + nl > b->names_cap) {
		uint32_t cap = b->names_cap ? b->names_cap : 64 * 1024;
		while (cap < s->names_len + nl) cap *= 2;
		if (cap > NAMES_MAX) cap = NAMES_MAX;
		char *n = realloc(s->names, cap);
		if (!n) { s->dirs_dropped++; return -1; }
		s->names = n;
		b->names_cap = cap;
	}
	memcpy(s->names + s->names_len, name, nl);
	scan_dir_t *d = &s->dir[s->num_dirs];
	d->parent = (uint32_t)parent;
	d->name = s->names_len;
	d->bytes = 0;
	d->files = d->dirs = 0;
	s->names_len += nl;
	return (int)s->num_dirs++;
}

// a new item (or the category's "more items" row once there are ITEMS_MAX); -1 if there's no memory
static int New_Item(build_t *b, int cat, int kind, const char *path, int dir)
{
	scan_t *s = b->s;
	if (s->num_items >= ITEMS_MAX) {
		if (b->rest[cat] >= 0) return b->rest[cat];
		kind = ITEM_REST;
		path = "(more items)";
	}
	if (s->num_items == b->cap) {
		uint32_t cap = b->cap ? b->cap * 2 : 256;
		if (cap > ITEMS_MAX + NUM_CATS) cap = ITEMS_MAX + NUM_CATS;
		scan_item_t *n = realloc(s->items, (size_t)cap * sizeof(*n));
		if (!n) { b->out_of_memory = 1; return -1; }
		s->items = n;
		b->cap = cap;
	}
	int i = (int)s->num_items++;
	scan_item_t *it = &s->items[i];
	memset(it, 0, sizeof(*it));
	snprintf(it->path, sizeof(it->path), "%s", path);
	it->cat = (uint8_t)cat;
	it->kind = (uint8_t)kind;
	it->dir = kind == ITEM_DIR ? dir : -1;
	if (kind == ITEM_REST) b->rest[cat] = i;
	return i;
}

/*
==================
Titles and icons: a game's (game/<ID>, GAMES/<name>/PS3_GAME) and a save's
PARAM.SFO, a trophy set's TROPCONF.SFM; whether there's an ICON0.PNG. Small
files, read once each, only by the items that should have them.
==================
*/
static unsigned char *Read_Small(const char *path, size_t *len)
{
	int type;
	uint64_t size;
	At(path);
	if (fsx_stat(path, &type, &size) != 0 || type != FSX_FILE || size > SMALL_FILE) return NULL;
	return plat_read_file(path, len);
}

static int Is_File(const char *path)
{
	int type;
	uint64_t size;
	At(path);
	return fsx_stat(path, &type, &size) == 0 && type == FSX_FILE;
}

static void Item_Sfo(scan_item_t *x, const char *dir, int save)
{
	char p[FSX_PATH], sub[64];
	size_t len;
	snprintf(p, sizeof(p), "%s/PARAM.SFO", dir);
	unsigned char *sfo = Read_Small(p, &len);
	if (!sfo) return;
	sfo_get(sfo, len, "TITLE", x->title, sizeof(x->title));
	sfo_get(sfo, len, "TITLE_ID", x->tid, sizeof(x->tid));
	sfo_get(sfo, len, "CATEGORY", x->code, sizeof(x->code));
	if (save && sfo_get(sfo, len, "SUB_TITLE", sub, sizeof(sub)) && x->title[0]) {
		size_t l = strlen(x->title);
		snprintf(x->title + l, sizeof(x->title) - l, " \xc2\xb7 %s", sub);
		sfo_clean(x->title, sizeof(x->title));
	}
	free(sfo);
}

// the item it is (parent: the folder it's in, under the drive) has a title and icon, if its kind has
/*
==================
A download's title: vsh/task/<n>/ (a background download) and vsh/game_pkg/<n>/ (a package waiting to be
installed, the verified-facts report) have a d0.pdb: a 4-byte 0, then entries of an ID, a length, the length
again and that many bytes, big-endian; entry 0x69 is the title (psdevwiki's Project Database (PDB))
==================
*/
static void Pdb_Title(scan_item_t *x, const char *abs)
{
	char p[FSX_PATH], t[ITEM_TITLE] = "", file[ITEM_TITLE] = "";
	size_t len;
	snprintf(p, sizeof(p), "%s/d0.pdb", abs);
	unsigned char *d = Read_Small(p, &len);
	if (!d) return;
	for (size_t at = 4; at + 12 <= len; ) {
		uint32_t id = (uint32_t)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
		uint32_t n = (uint32_t)d[at + 4] << 24 | d[at + 5] << 16 | d[at + 6] << 8 | d[at + 7];
		if (n > len - at - 12) break;		// (not a PDB as described: no title)
		int k = (int)(n < sizeof(t) ? n : sizeof(t) - 1);
		if (id == 0x69 && n > 1 && !t[0]) snprintf(t, sizeof(t), "%.*s", k, (const char *)d + at + 12);
		if (id == 0xCB && n > 1 && !file[0]) snprintf(file, sizeof(file), "%.*s", k, (const char *)d + at + 12);
		at += 12 + n;
	}
	free(d);
	// (the title as pkgi and IRISMAN write it, the verified-facts report: "\u2605 Download "<name>"" or
	// "\u2605 Install "<name>"": the name; no title, the package's file name, its ".pkg" left off)
	char *q = t;
	if (!strncmp(q, "\xE2\x98\x85", 3)) for (q += 3; *q == ' '; q++) {}
	size_t ql;
	if ((!strncmp(q, "Download \"", 10) || !strncmp(q, "Install \"", 9)) && (ql = strlen(q)) > 0 && q[ql - 1] == '"') {
		q = strchr(q, '"') + 1;
		q[strlen(q) - 1] = 0;
	}
	if (!q[0] && file[0]) {
		size_t fl = strlen(file);
		if (fl > 4 && !strcasecmp(file + fl - 4, ".pkg")) file[fl - 4] = 0;
		q = file;
	}
	if (q[0]) snprintf(x->title, sizeof(x->title), "%s", q);
}

static void Item_Info(build_t *b, int it, const char *abs, const char *parent)
{
	if (it < 0) return;
	scan_item_t *x = &b->s->items[it];
	if (x->kind != ITEM_DIR) return;
	const char *base = strrchr(parent, '/');
	base = base ? base + 1 : parent;
	char p[FSX_PATH];
	// (minis_savedata: PSP Minis' and Remasters'; ps2emu2_savedata: PS2 Classics', its PARAM.SFO the game's)
	int save = !strcmp(base, "savedata") || !strcmp(base, "minis_savedata") || !strcmp(base, "ps2emu2_savedata");
	if (!strcmp(parent, "game") || save) {
		Item_Sfo(x, abs, save);
		snprintf(p, sizeof(p), "%s/ICON0.PNG", abs);
		if (Is_File(p)) x->icon = ICON_HERE;
	} else if (!strcmp(parent, "vsh/task") || !strcmp(parent, "vsh/game_pkg")) {
		Pdb_Title(x, abs);
		snprintf(p, sizeof(p), "%s/ICON_FILE", abs);
		if (Is_File(p)) x->icon = ICON_HERE;		// (a PNG, as the XMB shows it)
	} else if (!strcmp(parent, "GAMES") || !strcmp(parent, "GAMEZ")) {
		snprintf(p, sizeof(p), "%s/PS3_GAME", abs);
		Item_Sfo(x, p, 0);
		snprintf(p, sizeof(p), "%s/PS3_GAME/ICON0.PNG", abs);
		if (Is_File(p)) x->icon = ICON_PS3_GAME;
	} else if (!strcmp(base, "trophy")) {
		size_t len;
		snprintf(p, sizeof(p), "%s/TROPCONF.SFM", abs);
		unsigned char *t = Read_Small(p, &len);
		if (t) sfo_trophy_title(t, len, x->title, sizeof(x->title));
		free(t);
		snprintf(p, sizeof(p), "%s/ICON0.PNG", abs);
		if (Is_File(p)) x->icon = ICON_HERE;
	}
}

// a PKG's title ID, from its header: magic 7F 'PKG' at 0, the content ID ("UP0001-NPUB30001_00-...")
// at 0x30 (psdevwiki's PKG files); the ID in it is what the package installs, as game/<ID>
static void Pkg_Info(scan_item_t *x, const char *abs)
{
	unsigned char h[0x60];
	At(abs);
	if (fsx_read_at(abs, 0, h, sizeof(h)) != (int)sizeof(h) || memcmp(h, "\x7fPKG", 4)) return;
	const unsigned char *id = h + 0x30 + 7;
	if (h[0x30 + 6] != '-' || h[0x30 + 16] != '_') return;
	for (int i = 0; i < 9; i++) if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !(id[i] >= '0' && id[i] <= '9')) return;
	snprintf(x->tid, sizeof(x->tid), "%.9s", (const char *)id);
}

// the largest files: a min-heap of cap while scanning (sorted at the end)
static void Heap_Offer(scan_big_t *h, uint32_t *num, int cap, const char *rel, uint64_t bytes, int cat)
{
	int n = (int)*num, i;
	if (n < cap) {
		i = n;
		++*num;
		while (i > 0 && h[(i - 1) / 2].bytes > bytes) { h[i] = h[(i - 1) / 2]; i = (i - 1) / 2; }
	} else {
		if (bytes <= h[0].bytes) return;
		i = 0;		// replaces the smallest, which sinks to its place
		for (;;) {
			int c = 2 * i + 1;
			if (c >= n) break;
			if (c + 1 < n && h[c + 1].bytes < h[c].bytes) c++;
			if (h[c].bytes >= bytes) break;
			h[i] = h[c];
			i = c;
		}
	}
	snprintf(h[i].path, sizeof(h[i].path), "%s", rel);
	h[i].bytes = bytes;
	h[i].cat = (uint8_t)cat;
	h[i].type = 0;
	memset(h[i].pad, 0, sizeof(h[i].pad));
}

static void Big_Offer(scan_t *s, const char *rel, uint64_t bytes, int cat)
{
	if (scan_bin[0] && In_Bin(rel, scan_bin)) return;		// (deleted, in the recycle bin)
	Heap_Offer(s->big, &s->num_big, BIG_MAX, rel, bytes, cat);
}

static int Big_Cmp(const void *a, const void *b)
{
	uint64_t x = ((const scan_big_t *)a)->bytes, y = ((const scan_big_t *)b)->bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

// the file's type (its extension, lower case) counted in its category; past 63 types, "*". Its
// index there (-1 none)
static int Type_Add(build_t *b, int cat, const char *abs, uint64_t size)
{
	if (!b->tt || cat < 0 || cat >= NUM_SCANNED_CATS) return -1;
	char ext[8] = "";
	const char *base = strrchr(abs, '/'), *dot = strrchr(base ? base : abs, '.');
	if (dot && dot[1] && strlen(dot + 1) < sizeof(ext) && dot != (base ? base + 1 : abs)) {
		int k = 0;
		for (const char *c = dot + 1; *c; c++) ext[k++] = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
		ext[k] = 0;
	}
	scan_type_t *t = b->tt[cat];
	int n = b->ntt[cat], i;
	for (i = 0; i < n && strcmp(t[i].ext, ext); i++);
	if (i == n) {
		if (n < 63) { b->ntt[cat]++; snprintf(t[i].ext, sizeof(t[i].ext), "%s", ext); t[i].bytes = 0; t[i].files = 0; }
		else {
			for (i = 0; i < n && strcmp(t[i].ext, "*"); i++);
			if (i == n) { b->ntt[cat]++; snprintf(t[i].ext, sizeof(t[i].ext), "*"); t[i].bytes = 0; t[i].files = 0; }
		}
	}
	t[i].bytes += size;
	t[i].files++;
	return i;
}

// and among that type's largest files
static void Type_File(build_t *b, int cat, int ti, const char *rel, uint64_t size)
{
	if (!b->th || ti < 0 || ti >= 64) return;
	if (scan_bin[0] && In_Bin(rel, scan_bin)) return;		// (deleted, in the recycle bin: counted in its type, not offered among its files)
	scan_big_t **h = &b->th[cat * 64 + ti];
	if (!*h && !(*h = calloc(TYPE_FILES, sizeof(scan_big_t)))) return;		// (no memory: that type's files aren't kept)
	Heap_Offer(*h, &b->thn[cat * 64 + ti], TYPE_FILES, rel, size, cat);
}

// a type's largest files, largest first, to the scan's (as type k of cat)
static void Tfiles_Add(scan_t *s, int cat, int k, const scan_big_t *h, uint32_t n)
{
	if (!n) return;
	scan_big_t *t = realloc(s->tfiles, (s->num_tfiles + n) * sizeof(scan_big_t));
	if (!t) return;
	s->tfiles = t;
	scan_big_t *at = t + s->num_tfiles;
	memcpy(at, h, n * sizeof(scan_big_t));
	qsort(at, n, sizeof(scan_big_t), Big_Cmp);
	for (uint32_t i = 0; i < n; i++) { at[i].cat = (uint8_t)cat; at[i].type = (uint8_t)k; }
	s->num_tfiles += n;
}

static int Type_Cmp(const void *a, const void *b)
{
	uint64_t x = ((const scan_type_t *)a)->bytes, y = ((const scan_type_t *)b)->bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

// the scan's types: each category's largest TYPES_PER_CAT - 1, the rest one "*"
static void Types_Keep(build_t *b)
{
	scan_t *s = b->s;
	if (!b->tt) return;
	for (int c = 0; c < NUM_SCANNED_CATS; c++) {
		if (scan_cat >= 0 && c != scan_cat) continue;		// (a partial scan: the others come from its base)
		scan_type_t *t = b->tt[c];
		int n = b->ntt[c], idx[64];
		for (int i = 0; i < n; i++) idx[i] = i;
		for (int i = 1; i < n; i++)		// (largest first, by index: the heaps stay where they are)
			for (int j = i; j > 0 && Type_Cmp(&t[idx[j - 1]], &t[idx[j]]) > 0; j--) { int x = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = x; }
		int keep = n <= TYPES_PER_CAT ? n : TYPES_PER_CAT - 1;
		memset(s->types[c], 0, sizeof(s->types[c]));
		for (int k = 0; k < keep; k++) {
			s->types[c][k] = t[idx[k]];
			if (b->th) Tfiles_Add(s, c, k, b->th[c * 64 + idx[k]], b->thn[c * 64 + idx[k]]);
		}
		if (n > keep) {
			scan_type_t *rest = &s->types[c][keep];
			snprintf(rest->ext, sizeof(rest->ext), "*");
			static scan_big_t rh[TYPE_FILES];		// (the rest's largest files, from all of theirs)
			uint32_t rn = 0;
			memset(rh, 0, sizeof(rh));
			for (int i = keep; i < n; i++) {
				rest->bytes += t[idx[i]].bytes;
				rest->files += t[idx[i]].files;
				const scan_big_t *h = b->th ? b->th[c * 64 + idx[i]] : NULL;
				for (uint32_t q = 0; h && q < b->thn[c * 64 + idx[i]]; q++) Heap_Offer(rh, &rn, TYPE_FILES, h[q].path, h[q].bytes, c);
			}
			Tfiles_Add(s, c, keep, rh, rn);
			keep++;
		}
		s->num_types[c] = (uint8_t)keep;
	}
}

static void File_Found(build_t *b, int item, int dir, const char *abs, uint64_t size, uint64_t blksize, int64_t mtime)
{
	scan_t *s = b->s;
	if (dir >= 0) {
		s->dir[dir].bytes += size;
		s->dir[dir].files++;
	}
	s->files++;
	s->scanned += size;
	if (blksize && !s->blksize) s->blksize = (uint32_t)blksize;
	s->alloc += blksize ? (size + blksize - 1) / blksize * blksize : size;
	if (item >= 0) {
		s->items[item].files++;
		s->items[item].bytes += size;
		if (mtime > s->items[item].newest) s->items[item].newest = mtime;
		Big_Offer(s, abs + strlen(scan_root) + 1, size, s->items[item].cat);
		int ti = Type_Add(b, s->items[item].cat, abs, size);
		Type_File(b, s->items[item].cat, ti, abs + strlen(scan_root) + 1, size);
	}
	prog.files = s->files;
	prog.bytes = s->scanned;
}

static int Push(build_t *b, const char *path, int dir, int acct)
{
	if (b->ntodo == b->captodo) {
		int cap = b->captodo ? b->captodo * 2 : 64;
		struct todo *n = realloc(b->todo, cap * sizeof(*n));
		if (!n) return 0;
		b->todo = n;
		b->captodo = cap;
	}
	char *p = strdup(path);
	if (!p) return 0;
	b->todo[b->ntodo].path = p;
	b->todo[b->ntodo].dir = dir;
	b->todo[b->ntodo++].acct = dir >= 0 ? dir : acct;
	return 1;
}

// everything under path (a folder, dir in the sizes; acct the kept folder its files count in when
// it isn't kept), into item: one folder open at a time, the rest waiting in todo
static void Walk(build_t *b, int item, const char *path, int path_dir, int acct)
{
	char child[FSX_PATH];
	if (!Push(b, path, path_dir, acct)) { b->out_of_memory = 1; return; }
	while (b->ntodo > 0 && !Cancelled()) {
		b->ntodo--;
		char *dir = b->todo[b->ntodo].path;
		int di = b->todo[b->ntodo].dir, ac = b->todo[b->ntodo].acct;
		fsx_dir_t *d;
		At(dir);
		int r = fsx_opendir(dir, &d);
		if (r != 0) {
			Unreadable(b, item, dir, r);
			free(dir);
			continue;
		}
		Progress_Path(b, dir);
		// (Stop and memory checked every batch, not only between folders: a big folder listed an entry
		// a call takes a while, and one that never ends is cut off at DIR_ENTRIES_MAX)
		for (int listed = 0; !Cancelled() && !b->out_of_memory;) {
			int n = fsx_read(d, b->ents, BATCH);
			prog.steps++;
			if (listed >= DIR_ENTRIES_MAX && n > 0) { Too_Many(b, item, dir); break; }		// (more after the cap: damaged; none: it ended there)
			if (n < 0) { Unreadable(b, item, dir, n); break; }
			if (n == 0) break;
			listed += n;
			for (int i = 0; i < n; i++) {
				const fsx_ent_t *e = &b->ents[i];
				if (snprintf(child, sizeof(child), "%s/%s", dir, e->name) >= (int)sizeof(child)) {
					Unreadable(b, item, dir, -2);		// (deeper than lv2 can name)
					continue;
				}
				if (e->type == FSX_DIR) {
					b->s->dirs++;
					prog.dirs = b->s->dirs;
					if (item >= 0) b->s->items[item].dirs++;
					if (!Push(b, child, Dir_Add(b, di, e->name), ac)) b->out_of_memory = 1;
				} else if (e->type == FSX_FILE) {
					File_Found(b, item, ac, child, e->size, e->blksize, e->mtime);
				}
			}
		}
		fsx_closedir(d);
		free(dir);
		if (b->out_of_memory) break;
	}
	// (a stop leaves folders waiting: let them go)
	while (b->ntodo > 0) free(b->todo[--b->ntodo].path);
}

// a folder's entries, all of them (a copy: the folder's closed before they're walked); -1 if it can't be read
static int List_All(build_t *b, const char *path, fsx_ent_t **out)
{
	fsx_dir_t *d;
	*out = NULL;
	At(path);
	int r = fsx_opendir(path, &d), n = 0, cap = 0;
	if (r != 0) return r < 0 ? r : -r;
	while (!Cancelled() && !b->out_of_memory) {
		int k = fsx_read(d, b->ents, BATCH);
		prog.steps++;
		if (n >= DIR_ENTRIES_MAX && k > 0) { Too_Many(b, -1, path); break; }
		if (k <= 0) { if (k < 0) Unreadable(b, -1, path, k); break; }
		if (n + k > cap) {
			cap = (n + k) * 2;
			fsx_ent_t *m = realloc(*out, cap * sizeof(*m));
			if (!m) { b->out_of_memory = 1; break; }
			*out = m;
		}
		memcpy(*out + n, b->ents, k * sizeof(fsx_ent_t));
		n += k;
	}
	fsx_closedir(d);
	return n;
}

// each entry of rel (a folder under the root, dir in the sizes; acct the kept folder its files
// count in if it isn't kept) is an item of cat
static void Children_As_Items(build_t *b, const char *rel, int cat, int dir, int acct)
{
	if (dir >= 0) acct = dir;
	char abs[FSX_PATH], crel[FSX_PATH];
	fsx_ent_t *ents;
	snprintf(abs, sizeof(abs), "%s/%s", scan_root, rel);
	int n = List_All(b, abs, &ents);
	if (n < 0) {
		int it = New_Item(b, cat, ITEM_DIR, rel, dir);
		Unreadable(b, it, abs, n);
		return;
	}
	b->s->dirs++;
	for (int i = 0; i < n && !Cancelled() && !b->out_of_memory; i++) {
		snprintf(crel, sizeof(crel), "%s/%s", rel, ents[i].name);
		snprintf(abs, sizeof(abs), "%s/%s", scan_root, crel);
		if (ents[i].type == FSX_DIR) {
			int cd = Dir_Add(b, dir, ents[i].name);
			int it = New_Item(b, cat, ITEM_DIR, crel, cd);
			Item_Info(b, it, abs, rel);
			b->s->dirs++;
			Walk(b, it, abs, cd, acct);
		} else if (ents[i].type == FSX_FILE) {
			int it = New_Item(b, cat, ITEM_FILE, crel, -1);
			if (it >= 0 && cat == CAT_PKGTMP && Ends_With(crel, ".pkg")) Pkg_Info(&b->s->items[it], abs);
			// (and webMAN's "stealth" folder: PS3 ISOs kept in video/, psdevwiki's webMAN Game Paths)
			if (it >= 0 && ((cat == CAT_BACKUPS && (Ends_With(crel, ".iso") || Ends_With(crel, ".bin") || Ends_With(crel, ".img"))) ||
					(cat == CAT_MEDIA && !strncmp(crel, "video/", 6) && Ends_With(crel, ".iso")))) {
				scan_item_t *x = &b->s->items[it];
				int icon = 0;
				At(abs);
				iso_info(abs, x->title, sizeof(x->title), x->tid, sizeof(x->tid), x->code, sizeof(x->code), &icon);
				x->icon = icon == 1 ? ICON_ISO_PS3 : icon == 2 ? ICON_ISO_PSP : ICON_NONE;
			}
			File_Found(b, it, acct, abs, ents[i].size, ents[i].blksize, ents[i].mtime);
		}
	}
	free(ents);
}

// home/<user>/: savedata, minis_savedata, ps2emu2_savedata and trophy one item per save / trophy set; the rest (exdata, friendim...) an item each
static void Home(build_t *b, int home_dir)
{
	int hacct = home_dir >= 0 ? home_dir : 0;
	char abs[FSX_PATH], rel[FSX_PATH];
	fsx_ent_t *users, *ents;
	snprintf(abs, sizeof(abs), "%s/home", scan_root);
	int nu = List_All(b, abs, &users);
	if (nu < 0) { Unreadable(b, New_Item(b, CAT_SAVES, ITEM_DIR, "home", home_dir), abs, nu); return; }
	b->s->dirs++;
	for (int u = 0; u < nu && !Cancelled() && !b->out_of_memory; u++) {
		snprintf(rel, sizeof(rel), "home/%s", users[u].name);
		snprintf(abs, sizeof(abs), "%s/%s", scan_root, rel);
		if (users[u].type == FSX_FILE) {
			File_Found(b, New_Item(b, CAT_SAVES, ITEM_FILE, rel, -1), hacct, abs, users[u].size, users[u].blksize, users[u].mtime);
			continue;
		}
		if (users[u].type != FSX_DIR) continue;
		int ud = Dir_Add(b, home_dir, users[u].name), uacct = ud >= 0 ? ud : hacct;
		int n = List_All(b, abs, &ents);
		if (n < 0) { Unreadable(b, New_Item(b, CAT_SAVES, ITEM_DIR, rel, ud), abs, n); continue; }
		b->s->dirs++;
		for (int i = 0; i < n && !Cancelled() && !b->out_of_memory; i++) {
			char crel[FSX_PATH];
			snprintf(crel, sizeof(crel), "%s/%s", rel, ents[i].name);
			snprintf(abs, sizeof(abs), "%s/%s", scan_root, crel);
			if (ents[i].type == FSX_DIR && (!strcmp(ents[i].name, "savedata") || !strcmp(ents[i].name, "minis_savedata") || !strcmp(ents[i].name, "ps2emu2_savedata") ||
				!strcmp(ents[i].name, "trophy"))) {
				Children_As_Items(b, crel, CAT_SAVES, Dir_Add(b, ud, ents[i].name), uacct);
			} else if (ents[i].type == FSX_DIR) {
				int cd = Dir_Add(b, ud, ents[i].name);
				int it = New_Item(b, CAT_SAVES, ITEM_DIR, crel, cd);
				b->s->dirs++;
				Walk(b, it, abs, cd, uacct);
			} else if (ents[i].type == FSX_FILE) {
				File_Found(b, New_Item(b, CAT_SAVES, ITEM_FILE, crel, -1), uacct, abs, ents[i].size, ents[i].blksize, ents[i].mtime);
			}
		}
		free(ents);
	}
	free(users);
}

// vsh/: the system's; task/ and game_pkg/ one item a download (they can be a whole game's package), the
// rest an item each
static void Vsh(build_t *b, int vsh_dir)
{
	int acct = vsh_dir >= 0 ? vsh_dir : 0;
	char abs[FSX_PATH], crel[FSX_PATH];
	fsx_ent_t *ents;
	snprintf(abs, sizeof(abs), "%s/vsh", scan_root);
	int n = List_All(b, abs, &ents);
	if (n < 0) { Unreadable(b, New_Item(b, CAT_SYSTEM, ITEM_DIR, "vsh", vsh_dir), abs, n); return; }
	b->s->dirs++;
	for (int i = 0; i < n && !Cancelled() && !b->out_of_memory; i++) {
		snprintf(crel, sizeof(crel), "vsh/%s", ents[i].name);
		snprintf(abs, sizeof(abs), "%s/%s", scan_root, crel);
		if (ents[i].type == FSX_DIR && (!strcmp(ents[i].name, "task") || !strcmp(ents[i].name, "game_pkg"))) {
			Children_As_Items(b, crel, CAT_SYSTEM, Dir_Add(b, vsh_dir, ents[i].name), acct);
		} else if (ents[i].type == FSX_DIR) {
			int cd = Dir_Add(b, vsh_dir, ents[i].name);
			int it = New_Item(b, CAT_SYSTEM, ITEM_DIR, crel, cd);
			b->s->dirs++;
			Walk(b, it, abs, cd, acct);
		} else if (ents[i].type == FSX_FILE) {
			File_Found(b, New_Item(b, CAT_SYSTEM, ITEM_FILE, crel, -1), acct, abs, ents[i].size, ents[i].blksize, ents[i].mtime);
		}
	}
	free(ents);
}

// updater/: the system's, one item (PS3HEN makes updater/01 again at each start: never offered); an
// update waiting, updater/<NN>/<name>.pup, an item of its own (the verified-facts report: the Update
// Manager's layout, only the files)
static void Updater(build_t *b, int up_dir)
{
	int acct = up_dir >= 0 ? up_dir : 0;
	char abs[FSX_PATH], crel[FSX_PATH];
	fsx_ent_t *ents, *sub;
	int it = New_Item(b, CAT_SYSTEM, ITEM_DIR, "updater", up_dir);
	snprintf(abs, sizeof(abs), "%s/updater", scan_root);
	int n = List_All(b, abs, &ents);
	if (n < 0) { Unreadable(b, it, abs, n); return; }
	b->s->dirs++;
	for (int i = 0; i < n && !Cancelled() && !b->out_of_memory; i++) {
		snprintf(crel, sizeof(crel), "updater/%s", ents[i].name);
		snprintf(abs, sizeof(abs), "%s/%s", scan_root, crel);
		if (ents[i].type == FSX_FILE) { File_Found(b, it, acct, abs, ents[i].size, ents[i].blksize, ents[i].mtime); continue; }
		if (ents[i].type != FSX_DIR) continue;
		int cd = Dir_Add(b, up_dir, ents[i].name), cacct = cd >= 0 ? cd : acct;
		b->s->dirs++;
		if (it >= 0) b->s->items[it].dirs++;
		int m = List_All(b, abs, &sub);
		if (m < 0) { Unreadable(b, it, abs, m); continue; }
		for (int k = 0; k < m && !Cancelled() && !b->out_of_memory; k++) {
			char frel[FSX_PATH], fabs[FSX_PATH];
			if (snprintf(frel, sizeof(frel), "%s/%s", crel, sub[k].name) >= (int)sizeof(frel)) continue;
			snprintf(fabs, sizeof(fabs), "%s/%s", scan_root, frel);
			if (sub[k].type == FSX_FILE && Ends_With(sub[k].name, ".pup")) {
				int pi = New_Item(b, CAT_SYSTEM, ITEM_FILE, frel, -1);
				if (pi >= 0 && b->s->items[pi].kind == ITEM_FILE) {
					snprintf(b->s->items[pi].code, sizeof(b->s->items[pi].code), "%s", SCAN_FIRMWARE_CODE);
					snprintf(b->s->items[pi].title, sizeof(b->s->items[pi].title), "%s", "Firmware update");
				}
				File_Found(b, pi, cacct, fabs, sub[k].size, sub[k].blksize, sub[k].mtime);
			} else if (sub[k].type == FSX_FILE) {
				File_Found(b, it, cacct, fabs, sub[k].size, sub[k].blksize, sub[k].mtime);
			} else if (sub[k].type == FSX_DIR) {
				if (it >= 0) b->s->items[it].dirs++;
				b->s->dirs++;
				Walk(b, it, fabs, Dir_Add(b, cd, sub[k].name), cacct);
			}
		}
		free(sub);
	}
	free(ents);
}

static void Fmt_GB(uint64_t v, char *out, int size) { snprintf(out, size, "%.2f GiB", v / 1073741824.0); }

// the whole drive: each folder at its top by its category; what each held, logged (the path probe)
static void Scan_Root(build_t *b)
{
	fsx_ent_t *top;
	int n = List_All(b, scan_root, &top);
	if (n < 0) { Unreadable(b, -1, scan_root, n); return; }
	plat_log("probe %s: %d entries at the top", scan_root, n);
	// the folders at the top are kept first, whatever comes under them
	int *top_dir = malloc((n ? n : 1) * sizeof(int));
	if (!top_dir) { b->out_of_memory = 1; free(top); return; }
	for (int i = 0; i < n; i++)		// (a partial scan: only its own)
		top_dir[i] = top[i].type == FSX_DIR && (scan_cat < 0 || scan_cat_of(top[i].name, 1) == scan_cat) ? Dir_Add(b, 0, top[i].name) : -1;
	for (int i = 0; i < n && !Cancelled() && !b->out_of_memory; i++) {
		const fsx_ent_t *e = &top[i];
		char abs[FSX_PATH], gb[32];
		if (scan_cat >= 0 && (e->type != FSX_DIR && e->type != FSX_FILE)) continue;
		if (scan_cat >= 0 && scan_cat_of(e->name, e->type == FSX_DIR) != scan_cat) continue;		// (a partial scan: its own folders and files)
		uint64_t before = b->s->scanned, files0 = b->s->files, unr0 = b->s->unreadable;
		snprintf(abs, sizeof(abs), "%s/%s", scan_root, e->name);
		int cat = scan_cat_of(e->name, e->type == FSX_DIR);
		if (e->type == FSX_FILE) {
			int it = New_Item(b, cat, ITEM_FILE, e->name, -1);
			if (it >= 0 && Ends_With(e->name, ".pkg")) Pkg_Info(&b->s->items[it], abs);		// (a package dropped at the top: HEN's installer, a copy)
			File_Found(b, it, 0, abs, e->size, e->blksize, e->mtime);
		} else if (e->type != FSX_DIR) {
			plat_log("probe   %-14s (not a file or folder: left out)", e->name);
			continue;
		} else if (!strcmp(e->name, "home")) {
			Home(b, top_dir[i]);
		} else if (!strcmp(e->name, "vsh")) {
			Vsh(b, top_dir[i]);
		} else if (!strcmp(e->name, "updater")) {
			Updater(b, top_dir[i]);
		} else if (cat != CAT_SYSTEM) {
			Children_As_Items(b, e->name, cat, top_dir[i], 0);
		} else {
			int d = top_dir[i];
			int it = New_Item(b, CAT_SYSTEM, ITEM_DIR, e->name, d);
			b->s->dirs++;
			Walk(b, it, abs, d, 0);
		}
		Fmt_GB(b->s->scanned - before, gb, sizeof(gb));
		plat_log("probe   %-14s %-5s %-28s %12s  %7llu files%s", e->name, e->type == FSX_DIR ? "dir" : "file", scan_cat_name(cat), gb,
			(unsigned long long)(b->s->files - files0), b->s->unreadable > unr0 ? "  (some unreadable)" : "");
	}
	// the mapped folders this drive hasn't got (expected: most backups' folders)
	char missing[512] = "";
	for (int k = 0; k < NUM_TOP; k++) {
		int found = 0;
		for (int i = 0; i < n && !found; i++) found = !strcmp(top[i].name, top_map[k].name);
		if (!found) {
			size_t l = strlen(missing);
			snprintf(missing + l, sizeof(missing) - l, "%s%s", l ? ", " : "", top_map[k].name);
		}
	}
	if (missing[0] && scan_cat < 0) plat_log("probe   not on this drive: %s", missing);
	free(top_dir);
	free(top);
}

// the folders' sizes: each one's added to its parent's, last to first
static void Roll_Up(scan_t *s)
{
	for (uint32_t i = s->num_dirs; i-- > 1; ) {
		scan_dir_t *d = &s->dir[i], *p = &s->dir[d->parent];
		p->bytes += d->bytes;
		p->files += d->files;
		p->dirs += d->dirs + 1;
	}
}

// a path in the base (is_file: a file) that the partial scan found again: under, or being, a top
// folder of its category, or a file at the top of the drive of its category
static int Rescanned(const char *path, int cat, int is_file)
{
	if (strchr(path, '/')) return Cat_Of_Path(path) == scan_cat;		// (its top folder's)
	return is_file ? cat == scan_cat : scan_cat_of(path, 1) == scan_cat;
}

// a partial scan's result (its category's folders, rolled up) made whole from the base: the
// base's items, folders, largest files and types from everywhere else, its totals less what was
// rescanned. A top folder of the category is "replaced": what was under it in the base goes. (Each
// path's top folder decides, by its name: however many of them a drive has.)
static void Merge(build_t *b)
{
	scan_t *s = b->s;
	const scan_t *o = scan_base;
	int nrep = 0;
	// the base's folders not under a replaced top, renumbered (parents before children, as they were)
	int32_t *map = malloc((o->num_dirs ? o->num_dirs : 1) * sizeof(int32_t));
	uint8_t *gone = calloc(o->num_dirs ? o->num_dirs : 1, 1);
	if (!map || !gone) { free(map); free(gone); b->out_of_memory = 1; return; }
	map[0] = 0;
	for (uint32_t i = 1; i < o->num_dirs; i++) {
		uint32_t p = o->dir[i].parent;
		gone[i] = p ? gone[p] : scan_cat_of(o->names + o->dir[i].name, 1) == scan_cat;
		if (!p && gone[i]) nrep++;
		map[i] = -1;
		if (gone[i] || map[p] < 0) continue;
		int ni = Dir_Add(b, map[p], o->names + o->dir[i].name);
		if (ni < 0) continue;
		map[i] = ni;
		s->dir[ni].bytes = o->dir[i].bytes;		// (already rolled up: everything under it)
		s->dir[ni].files = o->dir[i].files;
		s->dir[ni].dirs = o->dir[i].dirs;
	}
	// the base's items from elsewhere (an item is replaced if it's under, or is, a replaced top)
	uint64_t rep_bytes = 0, rep_files = 0, rep_unr = 0;
	for (uint32_t i = 0; i < o->num_items && !b->out_of_memory; i++) {
		const scan_item_t *it = &o->items[i];
		int replaced = it->kind == ITEM_REST ? it->cat == scan_cat : Rescanned(it->path, it->cat, it->kind == ITEM_FILE);
		if (replaced) { rep_bytes += it->bytes; rep_files += it->files; rep_unr += it->unreadable; continue; }
		int ni = New_Item(b, it->cat, it->kind, it->path, -1);
		if (ni < 0) break;
		scan_item_t *x = &s->items[ni];
		*x = *it;
		x->dir = it->dir >= 0 && (uint32_t)it->dir < o->num_dirs ? map[it->dir] : -1;
	}
	// the largest files: the base's from elsewhere with the new ones, the largest BIG_MAX
	for (uint32_t i = 0; i < o->num_big; i++) {
		if (!Rescanned(o->big[i].path, o->big[i].cat, 1)) Big_Offer(s, o->big[i].path, o->big[i].bytes, o->big[i].cat);
	}
	for (int c = 0; c < NUM_SCANNED_CATS; c++)
		if (c != scan_cat) { memcpy(s->types[c], o->types[c], sizeof(s->types[c])); s->num_types[c] = o->num_types[c]; }
	for (uint32_t i = 0; i < o->num_tfiles; i++)		// (and their largest files)
		if (o->tfiles[i].cat != scan_cat) Tfiles_Add(s, o->tfiles[i].cat, o->tfiles[i].type, &o->tfiles[i], 1);
	// the totals: the base's less what was replaced, and what was found now
	s->scanned += o->scanned > rep_bytes ? o->scanned - rep_bytes : 0;
	s->files += o->files > rep_files ? o->files - rep_files : 0;
	s->unreadable += o->unreadable > rep_unr ? o->unreadable - rep_unr : 0;
	s->alloc = s->scanned + (o->alloc > o->scanned ? o->alloc - o->scanned : 0);
	if (!s->blksize) s->blksize = o->blksize;
	s->dirs_dropped += o->dirs_dropped;
	s->dirs = s->num_dirs > 1 ? s->num_dirs - 1 : 0;
	s->dir[0].bytes = s->scanned;		// (the top: everything, the files at the top included)
	s->dir[0].files = (uint32_t)s->files;
	s->dir[0].dirs = s->num_dirs - 1;
	free(map);
	free(gone);
	plat_log("scan %s: %s rescanned, %d folders replaced, the rest from the last scan", scan_root, scan_cat_name(scan_cat), nrep);
}

static void Finish(build_t *b)
{
	scan_t *s = b->s;
	char how[64];
	At("(the drive's size and free space, after the scan)");
	fsx_space(scan_root, &s->total, &s->free_bytes, &s->disk, how, sizeof(how));
	// a drive holds at least what's free and what was found: a "total" under that isn't its size
	// (lv2's 840 may give the free space with the reserve rather than the capacity: unverified)
	if (s->total && s->total < s->free_bytes + s->scanned) {
		plat_log("scan %s: %s gave %.2f GiB, less than free + found (%.2f GiB): not the drive's size; size unknown",
			scan_root, how, s->total / 1073741824.0, (s->free_bytes + s->scanned) / 1073741824.0);
		s->total = 0;
		how[0] = 0;
	}
	if (s->disk && s->total && s->disk < s->total) s->disk = 0;		// (a disk smaller than its partition isn't the disk)
	s->used = s->total > s->free_bytes ? s->total - s->free_bytes : 0;
	for (uint32_t i = 0; i < s->num_items; i++) {
		s->cat_bytes[s->items[i].cat] += s->items[i].bytes;
		s->cat_items[s->items[i].cat]++;
	}
	// the XMB's total is the whole disk: the system's partitions are what it has past this one
	if (s->disk && s->total) s->cat_bytes[CAT_RESERVED] = s->disk - s->total;
	uint64_t used = s->total ? s->used : scan_drive_used(s);
	if (used > s->scanned) s->cat_bytes[CAT_UNACCOUNTED] = used - s->scanned;
	if (!Index_Dirs(s)) plat_log("scan: no memory for the folder index: folders shown without sizes");
	Split_All(s);
	qsort(s->big, s->num_big, sizeof(s->big[0]), Big_Cmp);
	s->when = (int64_t)time(NULL);
	s->duration_ms = (uint32_t)((plat_time_us() - prog.t0) / 1000);

	char a[32], c[32], d[32], e[32], f[32];
	Fmt_GB(s->total, a, sizeof(a));
	Fmt_GB(s->free_bytes, c, sizeof(c));
	Fmt_GB(s->used, d, sizeof(d));
	Fmt_GB(s->scanned, e, sizeof(e));
	Fmt_GB(s->alloc, f, sizeof(f));
	plat_log("scan %s: %.1f s, %llu files, %llu folders, %llu unreadable, %u items", scan_root, s->duration_ms / 1000.0,
		(unsigned long long)s->files, (unsigned long long)s->dirs, (unsigned long long)s->unreadable, (unsigned)s->num_items);
	plat_log("scan %s: total %s (%s), free %s, used %s; the whole disk %.2f GiB", scan_root, a, how[0] ? how : "unknown", c, d,
		s->disk / 1073741824.0);
	plat_log("scan %s: %u folders' sizes kept (%u KB of names)%s", scan_root, (unsigned)s->num_dirs, (unsigned)(s->names_len / 1024),
		s->dirs_dropped ? "; some past the limit, not kept" : "");
	if (s->blksize) plat_log("scan %s: files add up to %s; rounded to %u-byte blocks %s", scan_root, e, (unsigned)s->blksize, f);
	else plat_log("scan %s: files add up to %s (no block size given)", scan_root, e);		// (an NTFS drive's)
	if (s->used && s->scanned > s->used)
		plat_log("scan %s: files add up to more than is used (by %.2f GiB): no Unaccounted", scan_root, (s->scanned - s->used) / 1073741824.0);
	for (int k = 0; k < NUM_CATS; k++) {
		Fmt_GB(s->cat_bytes[k], a, sizeof(a));
		plat_log("scan   %-28s %12s  %u items", scan_cat_name(k), a, (unsigned)s->cat_items[k]);
	}
}

static void Scan_Thread(void *arg)
{
	(void)arg;
	build_t b;
	memset(&b, 0, sizeof(b));
	for (int k = 0; k < NUM_CATS; k++) b.rest[k] = -1;
	b.s = calloc(1, sizeof(scan_t));
	b.ents = malloc(BATCH * sizeof(fsx_ent_t));
	if (!b.s || !b.ents) {
		plat_log("scan: no memory to start");
		free(b.s);
		free(b.ents);
		stopped = 1;
		prog.running = 0;
		thread_live = 0;
		return;
	}
	snprintf(b.s->root, sizeof(b.s->root), "%s", scan_root);
	b.tt = calloc(NUM_SCANNED_CATS, sizeof(*b.tt));		// (types: if there's no memory, none are kept)
	b.th = calloc(NUM_SCANNED_CATS * 64, sizeof(*b.th));
	b.thn = calloc(NUM_SCANNED_CATS * 64, sizeof(*b.thn));
	if (!b.th || !b.thn) { free(b.th); free(b.thn); b.th = NULL; b.thn = NULL; }
	plat_log("scan %s: started%s%s", scan_root, scan_cat >= 0 ? ", only " : "", scan_cat >= 0 ? scan_cat_name(scan_cat) : "");
	// what's used, for the progress bar (the total from the drive, as the result will have it). Asked
	// here, not by scan_start: that's the screen's thread, and a drive that's stopped answering would freeze it
	uint64_t total, fr, disk;
	char how[64];
	At("(the drive's size and free space, before the scan)");
	if (fsx_space(scan_root, &total, &fr, &disk, how, sizeof(how)) == 0 && total > fr) prog.used = total - fr;
	Dir_Add(&b, 0, "");		// the top (its own parent)
	Scan_Root(&b);
	free(b.ents);
	free(b.todo);
	if (b.out_of_memory) plat_log("scan %s: ran out of memory: stopped", scan_root);
	// a USB drive pulled out during its scan: what's left of it is folders that couldn't be read,
	// not the drive, so it's dropped and the last scan stays (0.9.10: one saved with 1 TB "Unaccounted")
	int gone = 0;
	if (!Cancelled() && !b.out_of_memory && !b.s->files && b.s->unreadable && b.s->num_dirs <= 1) {		// (0.9.11: an NTFS drive mounted but unreadable)
		plat_log("scan %s: the drive's top couldn't be read: not kept, the last scan stays", scan_root);
		gone = 1;
	} else if (!Cancelled() && !b.out_of_memory && strcmp(scan_root, "/dev_hdd0")) {
		uint64_t t, f, k;
		char how[32];
		if ((gone = fsx_space(scan_root, &t, &f, &k, how, sizeof(how)) != 0))
			plat_log("scan %s: the drive went during the scan: not kept, the last scan stays", scan_root);
	}
	if (Cancelled() || b.out_of_memory || gone) {
		if (prog.given_up) plat_log("scan %s: came back %.1f s after it started, after it was given up: stopped, not kept", scan_root, (plat_time_us() - prog.t0) / 1e6);
		else if (Cancelled()) plat_log("scan %s: stopped after %.1f s", scan_root, (plat_time_us() - prog.t0) / 1e6);
		scan_free(b.s);
		stopped = 1;
	} else {
		Roll_Up(b.s);
		Types_Keep(&b);
		b.s->partial = (uint8_t)(scan_cat + 1);
		if (scan_cat >= 0) Merge(&b);
		Finish(&b);
		char path[256];
		scan_cache_path(scan_root, path, sizeof(path));
		At(path);
		if (prog.given_up) {		// (stuck in Finish, and given up on: kept nowhere, as the screen's told)
			plat_log("scan %s: came back after it was given up: not kept", scan_root);
			scan_free(b.s);
		} else {
			if (!scan_save(b.s, path)) plat_log("scan: couldn't save the cache %s", path);
			__atomic_store_n(&done_scan, b.s, __ATOMIC_RELEASE);
		}
	}
	free(b.tt);
	for (int i = 0; b.th && i < NUM_SCANNED_CATS * 64; i++) free(b.th[i]);
	free(b.th);
	free(b.thn);
	scan_cat = -1;
	scan_base = NULL;
	__atomic_store_n(&prog.running, 0, __ATOMIC_RELEASE);
	thread_live = 0;
}

// USRDIR/cache_dev_hdd0.bin: named for the drive (the last part of its path)
void scan_cache_path(const char *root, char *out, int size)
{
	char key[32];
	fsx_drive_key(root, key, sizeof(key));
	snprintf(out, size, "%s/cache_%s.bin", plat_save_dir(), key);
}

int scan_start_partial(const char *root, int cat, const scan_t *base)
{
	if (thread_live || cat < 0 || cat >= NUM_SCANNED_CATS || !base || !base->num_dirs || !base->first_child) return 0;
	scan_cat = cat;
	scan_base = base;
	int ok = scan_start(root);
	if (!ok) { scan_cat = -1; scan_base = NULL; }
	return ok;
}

int scan_start(const char *root)
{
	if (thread_live) {
		if (prog.given_up) plat_log("scan: the scan given up on is still stuck inside a call to %s: restart the console to scan again", scan_root);
		return 0;
	}
	if (!scan_base) scan_cat = -1;		// (a whole scan, unless scan_start_partial set one up)
	snprintf(scan_root, sizeof(scan_root), "%s", root);
	if (!bin_rel(root, scan_bin, sizeof(scan_bin))) scan_bin[0] = 0;
	scan_t *old = __atomic_exchange_n(&done_scan, NULL, __ATOMIC_ACQ_REL);
	if (old) scan_free(old);
	memset((void *)&prog, 0, sizeof(prog));
	stopped = 0;
	prog.t0 = plat_time_us();
	prog.running = 1;
	thread_live = 1;
	// below the screen's thread (1000) and PS3 World's workers (1700); no recursion, so a small stack
	if (!plat_thread_start(Scan_Thread, NULL, 1750, 256, "scan")) {		// (256 KB: an NTFS drive's calls go deeper than lv2's)
		prog.running = 0;
		thread_live = 0;
		return 0;
	}
	return 1;
}

scan_progress_t *scan_progress(void) { return &prog; }

static uint64_t watch_sig, watch_at;		// (scan_start's memset of prog starts them again: watch_at 0)
static int watch_logged;

int scan_watch(char *at, int size)
{
	if (!prog.running || prog.given_up) return 0;
	uint64_t now = plat_time_us(), sig = prog.files + prog.dirs + prog.unreadable + prog.steps;
	if (!watch_at || prog.t0 > watch_at || sig != watch_sig) {		// (moving: or a new scan)
		if (watch_logged && prog.t0 <= watch_at) plat_log("scan %s: moving again after %.0f s", scan_root, (now - watch_at) / 1e6);
		watch_sig = sig;
		watch_at = now;
		watch_logged = 0;
		return 0;
	}
	if (now - watch_at < SCAN_QUIET_US) return 0;
	spin_lock(&prog.progress_lock);
	snprintf(at, size, "%s", prog.at);
	spin_unlock(&prog.progress_lock);
	if (!watch_logged) {
		plat_log("scan %s: nothing new for %.0f s, %llu files in; waiting on %s", scan_root, SCAN_QUIET_US / 1e6, (unsigned long long)prog.files, at);
		watch_logged = 1;
	}
	if (now - watch_at < SCAN_GIVE_UP_US) return 0;
	prog.given_up = 1;
	prog.cancel = 1;		// (if the call ever comes back, it stops there)
	plat_log("scan %s: nothing new for %.0f minutes, stuck on %s: given up. The drive never answered: failing to read there?",
		scan_root, SCAN_GIVE_UP_US / 60e6, at);
	return 1;
}
scan_t *scan_take(void) { return __atomic_exchange_n(&done_scan, NULL, __ATOMIC_ACQ_REL); }
void scan_cancel(void) { prog.cancel = 1; }

int scan_stopped(void)
{
	int s = stopped;
	stopped = 0;
	return s;
}

void scan_wait(unsigned max_us)
{
	uint64_t until = plat_time_us() + max_us;
	while (thread_live && plat_time_us() < until) plat_sleep_us(10000);
}

void scan_free(scan_t *s)
{
	if (!s) return;
	free(s->items);
	free(s->tfiles);
	free(s->dir);
	free(s->names);
	free(s->first_child);
	free(s->next_sibling);
	free(s);
}

/*
==================
The cache: a header, the scan_t as it is in memory (its pointers meaningless),
the items, the folders and their names. Only ever read back by the same build
of this app on the same machine (the version and the struct sizes are
checked), so no byte swapping; everything that indexes something is checked
before it's trusted.
==================
*/
typedef struct {
	char magic[4];
	uint32_t version, scan_size, item_size, num_items;
	uint32_t num_dirs, names_len;
	uint32_t dir_size;
	uint32_t num_tfiles, tfile_size;	// (40 bytes: the scan_t after it stays 8-byte aligned)
} cache_hdr_t;

// (each type's largest files go after the items: they're 8-byte aligned there too)
static size_t Cache_Len(uint32_t items, uint32_t tfiles, uint32_t dirs, uint32_t names)
{
	return sizeof(cache_hdr_t) + sizeof(scan_t) + (size_t)items * sizeof(scan_item_t) + (size_t)tfiles * sizeof(scan_big_t) +
		(size_t)dirs * sizeof(scan_dir_t) + names;
}

int scan_save(const scan_t *s, const char *path)
{
	size_t len = Cache_Len(s->num_items, s->num_tfiles, s->num_dirs, s->names_len);
	unsigned char *buf = malloc(len), *p = buf;
	if (!buf) return 0;
	cache_hdr_t h;
	memcpy(h.magic, CACHE_MAGIC, 4);
	h.version = CACHE_VERSION;
	h.scan_size = sizeof(scan_t);
	h.item_size = sizeof(scan_item_t);
	h.num_items = s->num_items;
	h.num_dirs = s->num_dirs;
	h.names_len = s->names_len;
	h.dir_size = sizeof(scan_dir_t);
	h.num_tfiles = s->num_tfiles;
	h.tfile_size = sizeof(scan_big_t);
	memcpy(p, &h, sizeof(h));
	p += sizeof(h);
	memcpy(p, s, sizeof(scan_t));		// (scan_t is ~55 KB: never a copy on the thread's stack)
	scan_t *c = (scan_t *)p;
	c->items = NULL;
	c->tfiles = NULL;
	c->dir = NULL;
	c->names = NULL;
	c->first_child = c->next_sibling = NULL;
	p += sizeof(scan_t);
	if (s->num_items) memcpy(p, s->items, (size_t)s->num_items * sizeof(scan_item_t));
	p += (size_t)s->num_items * sizeof(scan_item_t);
	if (s->num_tfiles) memcpy(p, s->tfiles, (size_t)s->num_tfiles * sizeof(scan_big_t));
	p += (size_t)s->num_tfiles * sizeof(scan_big_t);
	if (s->num_dirs) memcpy(p, s->dir, (size_t)s->num_dirs * sizeof(scan_dir_t));
	p += (size_t)s->num_dirs * sizeof(scan_dir_t);
	if (s->names_len) memcpy(p, s->names, s->names_len);
	int ok = plat_write_file(path, buf, len);
	free(buf);
	if (ok) plat_log("scan: cache saved, %s (%u KB)", path, (unsigned)(len / 1024));
	return ok;
}

static const char *Check(scan_t *s)
{
	if (s->num_big > BIG_MAX) return "damaged";
	for (uint32_t i = 0; i < s->num_items; i++) {
		scan_item_t *it = &s->items[i];
		it->path[ITEM_PATH - 1] = it->title[ITEM_TITLE - 1] = it->tid[sizeof(it->tid) - 1] = it->code[sizeof(it->code) - 1] = 0;
		if (it->cat >= NUM_SCANNED_CATS || it->kind > ITEM_REST || it->icon >= NUM_ICON_KINDS) return "damaged";
		if (it->dir < -1 || (it->dir >= 0 && (uint32_t)it->dir >= s->num_dirs)) return "damaged";
	}
	for (uint32_t i = 0; i < s->num_big; i++) {
		s->big[i].path[BIG_PATH - 1] = 0;
		if (s->big[i].cat >= NUM_SCANNED_CATS) return "damaged";
	}
	for (int c = 0; c < NUM_SCANNED_CATS; c++) {
		if (s->num_types[c] > TYPES_PER_CAT) return "damaged";
		for (int k = 0; k < TYPES_PER_CAT; k++) s->types[c][k].ext[sizeof(s->types[c][k].ext) - 1] = 0;
	}
	for (uint32_t i = 0; i < s->num_tfiles; i++) {
		s->tfiles[i].path[BIG_PATH - 1] = 0;
		if (s->tfiles[i].cat >= NUM_SCANNED_CATS || s->tfiles[i].type >= s->num_types[s->tfiles[i].cat]) return "damaged";
	}
	if (s->num_dirs && (!s->names_len || s->names[s->names_len - 1])) return "damaged";
	for (uint32_t i = 0; i < s->num_dirs; i++)
		if ((i ? s->dir[i].parent >= i : s->dir[i].parent != 0) || s->dir[i].name >= s->names_len) return "damaged";
	return NULL;
}

scan_t *scan_load(const char *path)
{
	size_t len = 0;
	unsigned char *buf = plat_read_file(path, &len);
	if (!buf) return NULL;
	const char *why = NULL;
	cache_hdr_t h;
	scan_t *s = NULL;
	if (len < sizeof(h) + sizeof(scan_t)) why = "too short";
	else {
		memcpy(&h, buf, sizeof(h));
		if (memcmp(h.magic, CACHE_MAGIC, 4) || h.version != CACHE_VERSION || h.scan_size != sizeof(scan_t) ||
			h.item_size != sizeof(scan_item_t) || h.dir_size != sizeof(scan_dir_t) || h.tfile_size != sizeof(scan_big_t))
			why = "another version's";
		else if (h.num_items > ITEMS_MAX + NUM_CATS || h.num_dirs > DIRS_MAX || h.names_len > NAMES_MAX ||
				h.num_tfiles > NUM_SCANNED_CATS * TYPES_PER_CAT * TYPE_FILES ||
				len != Cache_Len(h.num_items, h.num_tfiles, h.num_dirs, h.names_len))
			why = "the wrong size";
	}
	if (!why && !(s = calloc(1, sizeof(scan_t)))) why = "no memory";
	if (!why) {
		const unsigned char *p = buf + sizeof(h);
		memcpy(s, p, sizeof(scan_t));
		p += sizeof(scan_t);
		s->items = NULL;
		s->tfiles = NULL;
		s->dir = NULL;
		s->names = NULL;
		s->first_child = s->next_sibling = NULL;
		s->root[sizeof(s->root) - 1] = 0;
		size_t ib = (size_t)h.num_items * sizeof(scan_item_t), db = (size_t)h.num_dirs * sizeof(scan_dir_t);
		size_t tb = (size_t)h.num_tfiles * sizeof(scan_big_t);
		if (s->num_items != h.num_items || s->num_dirs != h.num_dirs || s->names_len != h.names_len || s->num_tfiles != h.num_tfiles) why = "damaged";
		else if ((ib && !(s->items = malloc(ib))) || (tb && !(s->tfiles = malloc(tb))) || (db && !(s->dir = malloc(db))) ||
				(h.names_len && !(s->names = malloc(h.names_len))))
			why = "no memory";
		else {
			if (ib) memcpy(s->items, p, ib);
			p += ib;
			if (tb) memcpy(s->tfiles, p, tb);
			p += tb;
			if (db) memcpy(s->dir, p, db);
			p += db;
			if (h.names_len) memcpy(s->names, p, h.names_len);
			why = Check(s);
			if (!why && !Index_Dirs(s)) why = "no memory";
			if (!why) Split_All(s);		// (a cache from before has them in their owners')
		}
	}
	free(buf);
	if (why) {
		plat_log("scan: cache %s is %s: not used", path, why);
		scan_free(s);
		return NULL;
	}
	plat_log("scan: cache %s from %lld, %u items, %u folders", path, (long long)s->when, (unsigned)s->num_items, (unsigned)s->num_dirs);
	return s;
}

/*
==================
Changes between two scans: both scans' items sorted by path and walked side by side.
==================
*/
static const scan_t *cmp_scan;
static int Item_Path_Cmp(const void *a, const void *b)
{
	return strcmp(cmp_scan->items[*(const uint32_t *)a].path, cmp_scan->items[*(const uint32_t *)b].path);
}

static uint32_t *Sorted(const scan_t *s)
{
	uint32_t *ix = malloc((s->num_items ? s->num_items : 1) * sizeof(uint32_t)), n = 0;
	if (!ix) return NULL;
	for (uint32_t i = 0; i < s->num_items; i++) if (s->items[i].kind != ITEM_REST) ix[n++] = i;
	cmp_scan = s;
	qsort(ix, n, sizeof(uint32_t), Item_Path_Cmp);
	if (n < s->num_items) ix[n] = UINT32_MAX;		// (the rest rows left out: the end)
	return ix;
}

static int Add_Change(scan_changes_t *c, uint32_t *cap, const scan_item_t *was, const scan_item_t *now)
{
	if (c->count >= CHANGES_MAX) return 1;
	uint64_t w = was ? was->bytes : 0, n = now ? now->bytes : 0;
	if (was && now && (w > n ? w - n : n - w) < CHANGE_MIN) return 1;
	if (c->count == *cap) {
		uint32_t nc = *cap ? *cap * 2 : 64;
		scan_change_t *m = realloc(c->c, nc * sizeof(*m));
		if (!m) return 0;
		c->c = m;
		*cap = nc;
	}
	const scan_item_t *it = now ? now : was;
	scan_change_t *x = &c->c[c->count++];
	memset(x, 0, sizeof(*x));
	snprintf(x->path, sizeof(x->path), "%s", it->path);
	snprintf(x->title, sizeof(x->title), "%s", it->title);
	x->was = w;
	x->now = n;
	x->was_there = was != NULL;
	x->now_there = now != NULL;
	x->cat = it->cat;
	x->kind = it->kind;
	x->icon = now ? now->icon : ICON_NONE;
	return 1;
}

scan_changes_t *scan_diff(const scan_t *was, const scan_t *now)
{
	scan_changes_t *c = calloc(1, sizeof(*c));
	uint32_t *a = Sorted(was), *b = Sorted(now), cap = 0;
	if (!c || !a || !b) { free(a); free(b); free(c); return NULL; }
	snprintf(c->root, sizeof(c->root), "%s", now->root);
	c->was_when = was->when;
	c->now_when = now->when;
	c->was_total = was->scanned;
	c->now_total = now->scanned;
	uint32_t na = 0, nb = 0, i = 0, j = 0;
	while (na < was->num_items && a[na] != UINT32_MAX) na++;
	while (nb < now->num_items && b[nb] != UINT32_MAX) nb++;
	int ok = 1;
	while (ok && (i < na || j < nb)) {
		int k = i >= na ? 1 : j >= nb ? -1 : strcmp(was->items[a[i]].path, now->items[b[j]].path);
		if (k < 0) ok = Add_Change(c, &cap, &was->items[a[i++]], NULL);
		else if (k > 0) ok = Add_Change(c, &cap, NULL, &now->items[b[j++]]);
		else ok = Add_Change(c, &cap, &was->items[a[i++]], &now->items[b[j++]]);
	}
	free(a);
	free(b);
	if (!ok) { scan_changes_free(c); return NULL; }
	return c;
}

void scan_changes_free(scan_changes_t *c)
{
	if (!c) return;
	free(c->c);
	free(c);
}

void scan_changes_path(const char *root, char *out, int size)
{
	char key[32];
	fsx_drive_key(root, key, sizeof(key));
	snprintf(out, size, "%s/changes_%s.bin", plat_save_dir(), key);
}

typedef struct { char magic[4]; uint32_t version, head_size, change_size; } changes_hdr_t;

int scan_changes_save(const scan_changes_t *c, const char *path)
{
	size_t len = sizeof(changes_hdr_t) + sizeof(*c) + (size_t)c->count * sizeof(scan_change_t);
	unsigned char *buf = malloc(len);
	if (!buf) return 0;
	changes_hdr_t h = {{'H', 'D', 'C', 'H'}, 1, sizeof(*c), sizeof(scan_change_t)};
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), c, sizeof(*c));
	((scan_changes_t *)(buf + sizeof(h)))->c = NULL;
	if (c->count) memcpy(buf + sizeof(h) + sizeof(*c), c->c, (size_t)c->count * sizeof(scan_change_t));
	int ok = plat_write_file(path, buf, len);
	free(buf);
	return ok;
}

scan_changes_t *scan_changes_load(const char *path)
{
	size_t len = 0;
	unsigned char *buf = plat_read_file(path, &len);
	if (!buf) return NULL;
	changes_hdr_t h;
	scan_changes_t *c = NULL;
	if (len >= sizeof(h) + sizeof(*c)) {
		memcpy(&h, buf, sizeof(h));
		if (!memcmp(h.magic, "HDCH", 4) && h.version == 1 && h.head_size == sizeof(*c) && h.change_size == sizeof(scan_change_t)) {
			c = malloc(sizeof(*c));
			if (c) {
				memcpy(c, buf + sizeof(h), sizeof(*c));
				c->c = NULL;
				c->root[sizeof(c->root) - 1] = 0;
				size_t want = sizeof(h) + sizeof(*c) + (size_t)c->count * sizeof(scan_change_t);
				if (c->count > CHANGES_MAX || len != want || (c->count && !(c->c = malloc((size_t)c->count * sizeof(scan_change_t))))) {
					free(c);
					c = NULL;
				} else if (c->count) {
					memcpy(c->c, buf + sizeof(h) + sizeof(*c), (size_t)c->count * sizeof(scan_change_t));
					for (uint32_t i = 0; i < c->count; i++) {
						scan_change_t *x = &c->c[i];
						x->path[ITEM_PATH - 1] = x->title[ITEM_TITLE - 1] = 0;
						if (x->cat >= NUM_SCANNED_CATS) x->cat = CAT_SYSTEM;
						if (x->icon >= NUM_ICON_KINDS) x->icon = ICON_NONE;
					}
				}
			}
		}
	}
	free(buf);
	if (!c) plat_log("scan: changes %s not used", path);
	return c;
}
