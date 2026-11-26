/*
scan.h: what's using a drive's space, found on a thread of its own.

The scanner walks the drive once, read-only, and keeps totals: for each
category, for each "item" (a game's folder, a save, a package: an entry
directly inside a category's folder, with its title and icon when it has a
PARAM.SFO), the 200 largest files, and every folder's size (the folders
only, never the files: a folder's files are listed live when it's opened,
and its folders' sizes come from here, so drilling down needs no rescan).
Memory stays small however many files there are; past DIRS_MAX folders the
deeper ones aren't kept, and are shown without a size.

No recursion: folders still to do are a list on the heap, and one folder is
open at a time, so deep trees can't run the thread's stack or lv2's handles
out. A folder that can't be read is skipped and counted.
*/
#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>
#include <string.h>

enum {
	CAT_GAMES,			// game/*
	CAT_BACKUPS,		// GAMES, PS3ISO, PS2ISO, PSXISO, PSPISO, BDISO, DVDISO
	CAT_SAVES,			// home/*, savedata, exdata
	CAT_THEMES,			// theme
	CAT_MEDIA,			// music, video, photo (a USB drive's MUSIC, VIDEO, PICTURE)
	CAT_PKGTMP,			// packages, tmp
	CAT_SYSTEM,			// every other folder, and the files at the top
	NUM_SCANNED_CATS,
	CAT_RESERVED = NUM_SCANNED_CATS,		// the HDD's other partitions (the system's own): the disk less /dev_hdd0
	CAT_UNACCOUNTED,	// used minus everything found: not a place, a difference
	NUM_CATS
};

#define ITEM_PATH	160			// an item's path under the drive ("game/BLUS30443")
#define ITEM_TITLE	96			// UTF-8
#define BIG_PATH	256
#define BIG_MAX		200			// the largest files kept
#ifndef ITEMS_MAX
#define ITEMS_MAX	50000		// past this, a category's rest is one "more items" row (a test sets it lower)
#endif
#ifndef DIRS_MAX
#define DIRS_MAX	262144		// folders whose sizes are kept (a test sets it lower)
#endif
#define NAMES_MAX	(16 << 20)	// bytes of their names

enum { ITEM_DIR, ITEM_FILE, ITEM_REST };
// the recycle bin's item (its code: no PARAM.SFO's): an item of its own, inside another's folder
#define SCAN_BIN_CODE	"~B"
#define SCAN_CACHE_CODE	"~C"		// an app's cache inside its game folder (IRISMAN's, Apollo's, multiMAN's)
#define SCAN_SONGS_CODE	"~S"		// SingStar's songs, FileSystem/Export, when FileSystem holds more (scan.c's Singstar)
#define SCAN_FIRMWARE_CODE	"~F"	// a system update: a folder backup's PS3_UPDATE, a file updater/<NN>/*.pup
#define scan_item_is_bin(it)	(!strcmp((it)->code, SCAN_BIN_CODE))
// <path>/ICON0.PNG, <path>/PS3_GAME/ICON0.PNG; inside a disc image: its PS3_GAME/ or PSP_GAME/ICON0.PNG
enum { ICON_NONE, ICON_HERE, ICON_PS3_GAME, ICON_ISO_PS3, ICON_ISO_PSP, NUM_ICON_KINDS };

typedef struct {
	char path[ITEM_PATH];
	char title[ITEM_TITLE];		// PARAM.SFO's TITLE (a save's with its SUB_TITLE), a trophy set's name; "" if none
	char tid[12];				// TITLE_ID
	char code[4];				// CATEGORY: HG a game, GD its data, SD a save...
	uint64_t bytes;
	int64_t newest;				// its most recently changed file's time (seconds since 1970; 0 unknown)
	int32_t dir;				// its folder in dirs (-1: a file, or not kept)
	uint32_t files, dirs, unreadable;
	uint8_t cat, kind, icon;
	uint8_t pad[5];
} scan_item_t;

// a folder: its parent (the drive's top is 0, its own parent), its name in names, and
// everything under it
typedef struct {
	uint32_t parent;
	uint32_t name;
	uint64_t bytes;
	uint32_t files, dirs;
} scan_dir_t;

#define TYPES_PER_CAT	20		// file types kept per category (the rest: one "other")
typedef struct {
	char ext[8];				// lower case, no dot ("iso"); "" no extension; "*" the rest
	uint64_t bytes;
	uint32_t files;
	uint32_t pad;
} scan_type_t;

typedef struct {
	char path[BIG_PATH];
	uint64_t bytes;
	uint8_t cat;
	uint8_t type;				// (a type's files: its index in types[cat])
	uint8_t pad[6];
} scan_big_t;
#define TYPE_FILES	25			// each kept file type's largest files

typedef struct {
	char root[64];				// "/dev_hdd0"
	int64_t when;				// time() at the end
	uint32_t duration_ms;
	uint32_t blksize;			// the block size stat gave (the first seen)
	uint64_t total, free_bytes;	// the drive's (the partition's), at the end (total 0: unknown)
	uint64_t disk;				// the whole disk, every partition, as the XMB counts it (HDD only; 0 unknown)
	uint64_t used;				// total - free (0 when the total's unknown)
	uint64_t scanned;			// bytes in every file found
	uint64_t alloc;				// the same rounded up to whole blocks (for comparing with the XMB)
	uint64_t files, dirs, unreadable;
	uint64_t cat_bytes[NUM_CATS];		// [CAT_UNACCOUNTED]: used - scanned, when that's more than 0
	uint32_t cat_items[NUM_CATS];
	uint32_t num_items;
	scan_item_t *items;
	uint32_t num_big;
	scan_big_t big[BIG_MAX];	// largest first
	uint32_t num_dirs, names_len;
	uint32_t dirs_dropped;		// folders past DIRS_MAX (or NAMES_MAX): not kept
	uint32_t edits;				// deletes made by HDD Atlas since it scanned (scan_forget)
	scan_type_t types[NUM_SCANNED_CATS][TYPES_PER_CAT];		// each category's files by type, largest first
	uint8_t num_types[NUM_SCANNED_CATS];
	uint8_t partial;			// the last scan rescanned this category + 1 (0: the whole drive)
	uint32_t num_tfiles;
	scan_big_t *tfiles;			// [num_tfiles]: each kept type's largest files (TYPE_FILES at most), by category and type, largest first
	scan_dir_t *dir;			// [num_dirs]; [0] is the drive's top
	char *names;				// [names_len]
	// made from dir when the scan's finished or loaded (not in the cache)
	uint32_t *first_child, *next_sibling;		// (UINT32_MAX: none)
} scan_t;

// what the screen shows while it runs (read without a lock: numbers that only go up; the
// path under progress_lock)
typedef struct {
	volatile int running;
	volatile int cancel;
	volatile uint64_t bytes, files, dirs, unreadable;
	volatile uint64_t used;			// the drive's used space, read as it started (0: unknown)
	volatile int progress_lock;		// spin flag for path and at
	char path[128];
	uint64_t t0;
	// where the scan is now, set before each call that can wait on the drive (a folder listed, a
	// file's header read), and a count of them: scan_watch's, to tell a stuck scan from a slow one
	char at[192];
	volatile uint32_t steps;
	volatile int given_up;			// scan_watch gave up on it (stuck): whatever it does after is thrown away
	// of the unreadable, those the drive itself failed to read (an I/O error: bad sectors, usually),
	// not a folder that's locked, gone or too deep: the internal drive's send the screen to Drive health
	volatile uint64_t read_errors;
} scan_progress_t;

// starts a scan of root ("/dev_hdd0") on a thread; 0 if one's running already or it couldn't start
int scan_start(const char *root);
// the same for one category's folders only, the rest taken from base (the drive's last scan, which
// mustn't change or go while it runs): the result is a whole scan, as if the drive had been scanned
int scan_start_partial(const char *root, int cat, const scan_t *base);
scan_progress_t *scan_progress(void);
/*
A scan stuck inside a call to the drive (a failing disk or SSD that never answers: the call never
comes back, and nothing can cut it short): the screen asks this every frame while one runs. Nothing
new for SCAN_QUIET_US, where it is goes in the log; for SCAN_GIVE_UP_US, it's given up: 1, and
where it was stuck in at. The thread stays where it is; if the call ever comes back, the scan stops
there and keeps nothing. Until it does, no other scan starts (scan_start says so in the log).
*/
#ifndef SCAN_QUIET_US		// (the tests' are shorter)
#define SCAN_QUIET_US	30000000ULL
#define SCAN_GIVE_UP_US	300000000ULL
#endif
int scan_watch(char *at, int size);
// a finished scan (the caller owns it, and scan_free's it), once; NULL while it runs or after a stop
scan_t *scan_take(void);
int scan_stopped(void);			// the last one was stopped (once: cleared when read)
void scan_cancel(void);
void scan_wait(unsigned max_us);	// at quit: until the thread's out, or max_us
void scan_free(scan_t *s);

// what changed between two scans of a drive: items that appeared or went, or grew or shrank by at
// least CHANGE_MIN; kept in USRDIR/changes_<drive>.bin until the next scan
#define CHANGE_MIN		(1024 * 1024)
#define CHANGES_MAX		10000
typedef struct {
	char path[ITEM_PATH];
	char title[ITEM_TITLE];
	uint64_t was, now;			// bytes (0 and 0 can't both be)
	uint8_t was_there, now_there, cat, kind, icon;
	uint8_t pad[3];
} scan_change_t;

typedef struct {
	char root[64];
	int64_t was_when, now_when;		// the two scans
	uint64_t was_total, now_total;	// what each found
	uint32_t count;
	uint32_t pad;
	scan_change_t *c;
} scan_changes_t;

scan_changes_t *scan_diff(const scan_t *was, const scan_t *now);	// NULL: no memory
void scan_changes_free(scan_changes_t *c);
void scan_changes_path(const char *root, char *out, int size);
int scan_changes_save(const scan_changes_t *c, const char *path);
scan_changes_t *scan_changes_load(const char *path);

// the cache: USRDIR/cache_<drive>.bin
void scan_cache_path(const char *root, char *out, int size);
int scan_save(const scan_t *s, const char *path);
scan_t *scan_load(const char *path);

const char *scan_cat_name(int cat);
const char *scan_cat_short(int cat);	// for tabs: "Games", "Backups"...
int scan_cat_of(const char *top, int is_dir);	// the category of an entry at the top of a drive

// the drive's size as the XMB gives it: the whole disk if known, else the partition, else what
// was found plus what's free
uint64_t scan_drive_total(const scan_t *s);
uint64_t scan_drive_used(const scan_t *s);

// rel (a file or a folder under the drive) was deleted: bytes and files (what went) taken off its
// item (or its category's "more items" row), its category, its folders and the totals; the item
// itself goes if it was the item, and any of the largest files that were it or in it. whole: rel
// is a folder and all of it went, so every folder under it is emptied too (none of them is there
// any more). free is the drive's free space now. edits++.
void scan_forget(scan_t *s, const char *rel, uint64_t bytes, uint32_t files, int whole, uint64_t free_now);
// the recycle bin's row follows what's moved in (bytes, files > 0: a move to the bin, after scan_forget
// took it off where it was) or out (< 0: put back, or gone for good): its item (made the first
// time), its category, its folders and the totals. free is the drive's free space now (0: as it was)
void scan_bin_change(scan_t *s, int64_t bytes, int32_t files, uint64_t free_now);

// the folder at rel ("game/BLUS30443/USRDIR", "" the top) in s's folder sizes; -1 if it isn't kept
int scan_dir_find(const scan_t *s, const char *rel);
// its folder called name; -1 if none
int scan_dir_child(const scan_t *s, int dir, const char *name);
static inline const char *scan_dir_name(const scan_t *s, int d) { return s->names + s->dir[d].name; }

#endif
