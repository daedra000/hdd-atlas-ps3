/*
ui_list.c: HDD Atlas's lists, in PS3 World's list look (7 rows of 68, a
panel, the selection in blue, a scrollbar, the glyph legend).

  items     a category's items: a game's folder with its title and icon, a
            save, a package... L1/R1 change category.
  largest   every item on the drive, largest first (L1/R1: one category or
            all); Square shows the 200 largest files instead.
  folder    what's in a folder: its folders with their sizes (from the scan:
            no rescan to drill down) and its files, listed live on a worker
            (read-only), so a big folder never holds the screen up. X opens
            a folder, Circle goes up, and from the item's own folder back to
            the list it came from.

Every list: SELECT sorts (largest first, smallest first, A-Z), Triangle
searches by name with the system keyboard (Circle clears the search), L2/R2
or Left/Right page.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "lang.h"
#include "jobs.h"
#include "fsx.h"
#include "scan.h"
#include "icons.h"
#include "del.h"
#include "games.h"
#include "bin.h"
#include "finds.h"
#include "ui_kit.h"
#include "ui_int.h"

#define LIST_Y		150
#define ROW_H		68
#define ROWS		7
#define THUMB_W		96			// an ICON0's shape (320x176)
#define THUMB_H		53
#define MAX_ENTRIES	100000		// a folder's entries listed at most

enum { LS_ITEMS, LS_LARGEST, LS_FOLDER, LS_CHANGES, LS_GAMES, LS_DUPES, LS_GAME, LS_FREE, LS_TYPES, LS_FOLDERS, LS_OLD, LS_MARKS,
	LS_BIN, LS_FILTER, LS_SAME, LS_SAMEF, LS_RECENT };
enum { SORT_SIZE, SORT_SMALL, SORT_NAME, NUM_SORTS };
static const char *const sort_names[NUM_SORTS] = {N_("Largest first"), N_("Smallest first"), N_("A-Z")};

typedef struct {
	int *rows;				// indices into what the list shows (items, the largest files, a folder's entries)
	int count, cap;
	int sel, top;
	int sort;
	char query[128];		// "" no search
} view_t;

static int mode;
static int items_cat;
static int lg_tab;			// 0: every category; 1..: that category + 1
static int lg_files;		// the largest files, not items
static view_t v_items, v_largest, v_folder, v_changes, v_games, v_dupes, v_game, v_free, v_types, v_folders, v_old, v_marks;
static view_t v_bin, v_filter, v_same, v_samef, v_recent;
static bin_entry_t *bin_rows;			// Recycle bin: the drive's entries
static int nbin_rows;
static int flt_size = 2, flt_age;		// Size filter: its steps (over 1 GB, any age to start with)
static const uint64_t flt_sizes[] = {0, 100ULL << 20, 1ULL << 30, 4ULL << 30, 10ULL << 30};
static const int flt_days[] = {0, 182, 365, 730, 1095};
static const int rec_days[] = {7, 30, 91, 365};		// Recently added: how far back (Square)
static int rec_span = 1;
static same_t *same;					// Identical files: the last look (its drive's), and the group open
static int same_group;
static uint64_t same_drawn;				// (the progress redrawn now and then)
static int parts_open, parts_done;		// a game's parts (Square on its screen), and whether they're added up
static parts_t parts;
static int game_sel, game_parent;		// LS_GAME: the game (in the games list), and the list it was opened from
static char game_tid[12];				// (found again by its ID when the games are made again)
static char lg_ext[8];					// LS_LARGEST's files: only these (from File types; "" all)
static int lg_type = -1;				// opened from File types: that type's own largest files (the scan's tfiles),
										// lg_ext exactly ("" no extension, "*" the rest); -1 the drive's largest
static int lg_parent = -1;				// LS_LARGEST opened from File types (-1: from the overview)
static int ty_tab;						// File types: 0 every category, 1.. that category + 1
static int searching;

/*
==================
A folder's contents: listed by a job (LANE_API) into one block of entries and
one of their names; its folders' sizes are then looked up in the scan
==================
*/
typedef struct {
	uint32_t name;			// in names
	uint8_t type;			// FSX_FILE, FSX_DIR, FSX_OTHER
	uint8_t known;			// a folder the scan has the size of
	uint64_t bytes;
	uint32_t files, dirs;
} fent_t;

typedef struct {
	fent_t *e;
	char *names;
	int n, truncated, err;
} flist_t;

static flist_t *fl;				// the folder on screen (NULL: being read)
static job_t *f_job;
static char f_rel[FSX_PATH];	// it, under the drive's top ("game/BLUS30443/USRDIR"; "" the top)
static int f_base;				// f_rel's length at the item's own folder (Circle there: back to the list)
static int f_return;			// the list it was opened from
static char f_title[ITEM_TITLE];	// the item's name (the crumb)
static char f_pick[FSX_NAME];	// select this entry once it's listed (coming back up)
static int f_keep_sel = -1;		// the row to come back to after a delete (the folder's read again)
static int f_dir;				// the folder in the scan's sizes (-1: not kept, or not there when it scanned)
static uint64_t f_total;

static void Flist_Free(flist_t *l)
{
	if (!l) return;
	free(l->e);
	free(l->names);
	free(l);
}

static void List_Run(job_t *j)
{
	flist_t *l = calloc(1, sizeof(*l));
	j->result = l;
	if (!l) return;
	fsx_dir_t *d;
	int r = fsx_opendir((const char *)j->arg, &d);
	if (r != 0) { l->err = r; return; }
	fsx_ent_t *batch = malloc(64 * sizeof(fsx_ent_t));
	int cap = 0;
	size_t ncap = 0, nlen = 0;
	while (batch && !j->cancel && !l->truncated) {
		int k = fsx_read(d, batch, 64);
		if (k < 0) { l->err = k; break; }
		if (k == 0) break;
		for (int i = 0; i < k; i++) {
			if (l->n >= MAX_ENTRIES) { l->truncated = 1; break; }
			size_t len = strlen(batch[i].name) + 1;
			if (l->n == cap) {
				int c = cap ? cap * 2 : 256;
				fent_t *e = realloc(l->e, c * sizeof(*e));
				if (!e) { l->truncated = 1; break; }
				l->e = e;
				cap = c;
			}
			if (nlen + len > ncap) {
				size_t c = ncap ? ncap * 2 : 8192;
				while (c < nlen + len) c *= 2;
				char *nn = realloc(l->names, c);
				if (!nn) { l->truncated = 1; break; }
				l->names = nn;
				ncap = c;
			}
			fent_t *e = &l->e[l->n++];
			memset(e, 0, sizeof(*e));
			e->name = (uint32_t)nlen;
			memcpy(l->names + nlen, batch[i].name, len);
			nlen += len;
			e->type = (uint8_t)batch[i].type;
			if (e->type == FSX_FILE) {
				e->bytes = batch[i].size;
				e->known = 1;
			}
		}
	}
	if (!batch) l->err = -1;
	free(batch);
	fsx_closedir(d);
	j->ok = 1;
}

static void List_Cleanup(job_t *j)
{
	Flist_Free(j->result);
	j->result = NULL;
	free(j->arg);
	j->arg = NULL;
}

static const char *F_Name(int i) { return fl->names + fl->e[i].name; }

static const scan_t *kid_scan;
static int Kid_Cmp(const void *a, const void *b)
{
	return strcmp(kid_scan->names + kid_scan->dir[*(const uint32_t *)a].name, kid_scan->names + kid_scan->dir[*(const uint32_t *)b].name);
}

// the folders' sizes, from the scan: its folders sorted by name, each entry looked up in them
static void Folder_Sizes(void)
{
	const scan_t *s = DRV->shown;
	f_dir = s ? scan_dir_find(s, f_rel) : -1;
	uint32_t *kids = NULL;
	int nk = 0;
	if (f_dir >= 0 && s->first_child) {
		for (uint32_t c = s->first_child[f_dir]; c != UINT32_MAX; c = s->next_sibling[c]) nk++;
		kids = malloc((nk ? nk : 1) * sizeof(uint32_t));
		if (kids) {
			nk = 0;
			for (uint32_t c = s->first_child[f_dir]; c != UINT32_MAX; c = s->next_sibling[c]) kids[nk++] = c;
			kid_scan = s;
			qsort(kids, nk, sizeof(uint32_t), Kid_Cmp);
		} else nk = 0;
	}
	f_total = 0;
	for (int i = 0; i < fl->n; i++) {
		fent_t *e = &fl->e[i];
		if (e->type == FSX_DIR) {
			e->known = 0;
			e->bytes = 0;
			int lo = 0, hi = nk - 1;
			while (lo <= hi) {
				int mid = (lo + hi) / 2, c = strcmp(F_Name(i), s->names + s->dir[kids[mid]].name);
				if (!c) {
					const scan_dir_t *d = &s->dir[kids[mid]];
					e->known = 1;
					e->bytes = d->bytes;
					e->files = d->files;
					e->dirs = d->dirs;
					break;
				}
				if (c < 0) hi = mid - 1; else lo = mid + 1;
			}
		}
		f_total += e->bytes;
	}
	free(kids);
}

/*
==================
The rows: what's in the list on screen, searched and sorted
==================
*/
static view_t *Cur_View(void)
{
	switch (mode) {
	case LS_ITEMS: return &v_items;
	case LS_LARGEST: return &v_largest;
	case LS_CHANGES: return &v_changes;
	case LS_GAMES: return &v_games;
	case LS_DUPES: return &v_dupes;
	case LS_GAME: return &v_game;
	case LS_FREE: return &v_free;
	case LS_TYPES: return &v_types;
	case LS_FOLDERS: return &v_folders;
	case LS_OLD: return &v_old;
	case LS_MARKS: return &v_marks;
	case LS_BIN: return &v_bin;
	case LS_FILTER: return &v_filter;
	case LS_RECENT: return &v_recent;
	case LS_SAME: return &v_same;
	case LS_SAMEF: return &v_samef;
	}
	return &v_folder;
}

/*
==================
Games (games.c), made from the scan when wanted and again when it changes; file types,
merged across categories
==================
*/
static games_t *gm;
static const scan_t *gm_scan;
static int64_t gm_when;
static uint32_t gm_edits, gm_items;

static games_t *Games(void)
{
	const scan_t *s = DRV->shown;
	if (!s) return NULL;
	if (gm && gm_scan == s && gm_when == s->when && gm_edits == s->edits && gm_items == s->num_items) return gm;
	games_free(gm);
	gm = games_build(s);
	gm_scan = s;
	gm_when = s->when;
	gm_edits = s->edits;
	gm_items = s->num_items;
	return gm;
}

/*
==================
Largest folders: the folders that hold the most, at any depth, each one only if no
folder in it holds 90% of it or more (that one's the place, then)
==================
*/
#define FOLDERS_MAX	200
static uint32_t folders[FOLDERS_MAX];
static int nfolders;

static int Dir_Bigger(const void *a, const void *b)
{
	const scan_t *s = DRV->shown;
	uint64_t x = s->dir[*(const uint32_t *)a].bytes, y = s->dir[*(const uint32_t *)b].bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void Folders_Build(void)
{
	const scan_t *s = DRV->shown;
	nfolders = 0;
	if (!s || !s->first_child || s->num_dirs < 2) return;
	uint32_t *cand = malloc(s->num_dirs * sizeof(uint32_t));
	if (!cand) return;
	int n = 0;
	for (uint32_t d = 1; d < s->num_dirs; d++) {
		if (!s->dir[d].parent || !s->dir[d].bytes) continue;		// (the top's own folders: the categories show those)
		int one_holds = 0;
		for (uint32_t c = s->first_child[d]; c != UINT32_MAX && !one_holds; c = s->next_sibling[c])
			one_holds = s->dir[c].bytes * 10 >= s->dir[d].bytes * 9;
		if (!one_holds) cand[n++] = d;
	}
	qsort(cand, n, sizeof(uint32_t), Dir_Bigger);
	nfolders = n < FOLDERS_MAX ? n : FOLDERS_MAX;
	memcpy(folders, cand, nfolders * sizeof(uint32_t));
	free(cand);
}

// a folder's path under the drive, from the folder sizes; 0 (and "") if it can't be made whole
// (deeper than the chain, or longer than out): never part of one, as Delete would act on it
static int Dir_Path(const scan_t *s, uint32_t d, char *out, int size)
{
	uint32_t chain[128];
	int n = 0;
	uint32_t x = d;
	for (; x && n < 128; x = s->dir[x].parent) chain[n++] = x;
	out[0] = 0;
	if (x) return 0;
	for (int i = n - 1; i >= 0; i--) {
		size_t l = strlen(out);
		if (snprintf(out + l, size - l, "%s%s", l ? "/" : "", s->names + s->dir[chain[i]].name) >= (int)(size - l)) { out[0] = 0; return 0; }
	}
	return 1;
}

/*
==================
Bookmarks: folders kept in USRDIR/bookmarks.txt, a line each ("<drive>TAB<path>"); those
of the drive on screen are listed
==================
*/
#define MARKS_MAX	64
static struct { char root[64]; char rel[FSX_PATH]; } marks[MARKS_MAX];
static int nmarks, marks_loaded;
static int mark_rows[MARKS_MAX], nmark_rows;

static void Marks_Path(char *out, int size) { snprintf(out, size, "%s/bookmarks.txt", plat_save_dir()); }

static void Marks_Load(void)
{
	char path[256];
	size_t len;
	marks_loaded = 1;
	Marks_Path(path, sizeof(path));
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return;
	for (char *line = strtok(b, "\n"); line && nmarks < MARKS_MAX; line = strtok(NULL, "\n")) {
		char *tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0;
		snprintf(marks[nmarks].root, sizeof(marks[0].root), "%s", line);
		snprintf(marks[nmarks].rel, sizeof(marks[0].rel), "%s", tab + 1);
		nmarks++;
	}
	free(b);
}

static void Marks_Save(void)
{
	char path[256];
	size_t cap = (size_t)nmarks * (64 + FSX_PATH + 2) + 1, n = 0;
	char *b = malloc(cap);
	if (!b) return;
	for (int i = 0; i < nmarks; i++) n += snprintf(b + n, cap - n, "%s\t%s\n", marks[i].root, marks[i].rel);
	Marks_Path(path, sizeof(path));
	plat_write_file(path, b, n);
	free(b);
}

static int Mark_Find(const char *root, const char *rel)
{
	if (!marks_loaded) Marks_Load();
	for (int i = 0; i < nmarks; i++) if (!strcmp(marks[i].root, root) && !strcmp(marks[i].rel, rel)) return i;
	return -1;
}

static void Mark_Toggle(const char *root, const char *rel)
{
	int i = Mark_Find(root, rel);
	if (i >= 0) {
		memmove(&marks[i], &marks[i + 1], (nmarks - i - 1) * sizeof(marks[0]));
		nmarks--;
		ui_note(tr("Bookmark removed."));
	} else {
		if (nmarks == MARKS_MAX) { ui_note(tr("There are 64 bookmarks already: remove one first.")); return; }
		snprintf(marks[nmarks].root, sizeof(marks[0].root), "%s", root);
		snprintf(marks[nmarks].rel, sizeof(marks[0].rel), "%s", rel);
		nmarks++;
		ui_note(tr("Bookmarked: it's in the menu's Bookmarks."));
	}
	Marks_Save();
}

static void Marks_Rows(void)
{
	if (!marks_loaded) Marks_Load();
	nmark_rows = 0;
	for (int i = 0; i < nmarks; i++) if (!strcmp(marks[i].root, DRV->root)) mark_rows[nmark_rows++] = i;
}

/*
==================
Keep this: rows of Free up space (by path) and Duplicates (by game) marked never to be suggested;
USRDIR/keep.txt, a line each ("<drive>TAB<path>" or "<drive>TABgame:<ID>"). Kept rows stay
listed, last and greyed, and count in no total
==================
*/
#define KEEPS_MAX	512
static struct { char root[64]; char key[ITEM_PATH]; } *keeps;
static int nkeeps, keeps_loaded;

static void Keep_Path(char *out, int size) { snprintf(out, size, "%s/keep.txt", plat_save_dir()); }

static void Keep_Load(void)
{
	char path[256];
	size_t len;
	keeps_loaded = 1;
	if (!keeps && !(keeps = calloc(KEEPS_MAX, sizeof(*keeps)))) return;
	Keep_Path(path, sizeof(path));
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return;
	for (char *line = strtok(b, "\n"); line && nkeeps < KEEPS_MAX; line = strtok(NULL, "\n")) {
		char *tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0;
		snprintf(keeps[nkeeps].root, sizeof(keeps[0].root), "%s", line);
		snprintf(keeps[nkeeps].key, sizeof(keeps[0].key), "%s", tab + 1);
		nkeeps++;
	}
	free(b);
}

static int Keep_Find(const char *key)
{
	if (!keeps_loaded) Keep_Load();
	for (int i = 0; i < nkeeps; i++) if (!strcmp(keeps[i].root, DRV->root) && !strcmp(keeps[i].key, key)) return i;
	return -1;
}

static int Kept(const char *key) { return key && key[0] && Keep_Find(key) >= 0; }

static void Keep_Toggle(const char *key)
{
	int i = Keep_Find(key);
	if (!keeps) return;
	if (i >= 0) {
		memmove(&keeps[i], &keeps[i + 1], (nkeeps - i - 1) * sizeof(keeps[0]));
		nkeeps--;
		ui_note(tr("It's suggested again."));
	} else {
		if (nkeeps == KEEPS_MAX) { ui_note(tr("There are 512 kept already: unkeep one first.")); return; }
		snprintf(keeps[nkeeps].root, sizeof(keeps[0].root), "%s", DRV->root);
		snprintf(keeps[nkeeps].key, sizeof(keeps[0].key), "%s", key);
		nkeeps++;
		ui_note(tr("Kept: it won't be suggested again (Square here undoes it)."));
	}
	char path[256];
	size_t cap = (size_t)nkeeps * (64 + ITEM_PATH + 2) + 1, n = 0;
	char *b = malloc(cap);
	if (!b) return;
	for (int k = 0; k < nkeeps; k++) n += snprintf(b + n, cap - n, "%s\t%s\n", keeps[k].root, keeps[k].key);
	Keep_Path(path, sizeof(path));
	plat_write_file(path, b, n);
	free(b);
}

// a game's key in it
static const char *Game_Key(const game_t *g)
{
	static char k[24];
	snprintf(k, sizeof(k), "game:%s", g->tid);
	return k;
}

static uint64_t Mark_Bytes(int i)
{
	const scan_t *s = DRV->shown;
	int d = s ? scan_dir_find(s, marks[mark_rows[i]].rel) : -1;
	return d >= 0 ? s->dir[d].bytes : 0;
}

/*
==================
Duplicates across drives: the same game in another drive's last scan
==================
*/
static games_t *other_gm[NUM_DRIVES];
static const scan_t *other_scan[NUM_DRIVES];
static int64_t other_when[NUM_DRIVES];
static uint32_t other_edits[NUM_DRIVES];

// another drive holding copies of the game: its index, and their bytes; -1 none
static int Elsewhere(const game_t *g, uint64_t *bytes)
{
	for (int d = 0; d < NUM_DRIVES; d++) {
		const scan_t *s = drives[d].shown;
		if (d == cur_drive || !s) continue;
		if (!other_gm[d] || other_scan[d] != s || other_when[d] != s->when || other_edits[d] != s->edits) {
			games_free(other_gm[d]);
			other_gm[d] = games_build(s);
			other_scan[d] = s;
			other_when[d] = s->when;
			other_edits[d] = s->edits;
		}
		if (!other_gm[d]) continue;
		for (int x = 0; x < other_gm[d]->ng; x++)
			if (!strcmp(other_gm[d]->g[x].tid, g->tid) && other_gm[d]->g[x].copies) {
				*bytes = other_gm[d]->g[x].copies_bytes;
				return d;
			}
	}
	return -1;
}

// what removing copies would free here: the extra ones, or all of them when another drive has one
static uint64_t Dup_Free(const game_t *g)
{
	uint64_t b;
	if (Kept(Game_Key(g))) return 0;
	return Elsewhere(g, &b) >= 0 ? g->copies_bytes : g->could_free;
}

// a row of Free up space or Duplicates that's kept (last, greyed, in no total)
static int Row_Kept(int i)
{
	games_t *g = Games();
	if (!g) return 0;
	if (mode == LS_FREE) return Kept(DRV->shown->items[g->spare[i].item].path);
	if (mode == LS_DUPES) return Kept(Game_Key(&g->g[i]));
	return 0;
}

// Identical files: the look's of the drive on screen; a group's first file still there, and how many are
static int Same_Here(void) { return same && !strcmp(same->root, DRV->root); }
static const char *Same_First(int gi)
{
	for (uint32_t k = 0; k < same->g[gi].n; k++) if (same->path[same->g[gi].first + k]) return same->path[same->g[gi].first + k];
	return "";
}
// a group's copies checked against the drive now (each one gone or changed size since the look:
// off the group), so its count is what's there; and another copy than fi (in the group) that's
// still there, or NULL
static const char *Same_Refresh(int gi, int fi)
{
	const char *other = NULL;
	for (uint32_t k = 0; k < same->g[gi].n; k++) {
		char **pp = &same->path[same->g[gi].first + k], abs[FSX_PATH];
		int type;
		uint64_t sz;
		if (!*pp) continue;
		if (snprintf(abs, sizeof(abs), "%s/%s", DRV->root, *pp) >= (int)sizeof(abs) || fsx_stat(abs, &type, &sz) != 0 || type != FSX_FILE ||
			sz != same->g[gi].size) { free(*pp); *pp = NULL; continue; }
		if ((int)k != fi && !other) other = *pp;
	}
	return other;
}

static int Same_Left(int gi)
{
	int n = 0;
	for (uint32_t k = 0; k < same->g[gi].n; k++) n += same->path[same->g[gi].first + k] != NULL;
	return n;
}

// Largest files' rows: the drive's largest, or (from File types) each type's own
static const scan_big_t *Lg_File(const scan_t *s, int i) { return lg_type >= 0 ? &s->tfiles[i] : &s->big[i]; }

// the scan's item a row is (items, a game's parts, what's spare); -1 none
static int Row_Item(int i)
{
	games_t *g = Games();
	if (mode == LS_GAME) return g && game_sel < g->ng ? (int)g->members[g->g[game_sel].first + i] : -1;
	if (mode == LS_FREE) return g ? (int)g->spare[i].item : -1;
	if (mode == LS_ITEMS || mode == LS_FILTER || mode == LS_RECENT || (mode == LS_LARGEST && !lg_files)) return i;
	return -1;
}

typedef struct { char ext[8]; uint64_t bytes; uint32_t files; } trow_t;
static trow_t types[NUM_SCANNED_CATS * TYPES_PER_CAT];
static int ntypes;

static void Types_Build(void)
{
	const scan_t *s = DRV->shown;
	ntypes = 0;
	if (!s) return;
	for (int c = 0; c < NUM_SCANNED_CATS; c++) {
		if (ty_tab && c != ty_tab - 1) continue;
		for (int k = 0; k < s->num_types[c]; k++) {
			const scan_type_t *t = &s->types[c][k];
			int i;
			for (i = 0; i < ntypes && strcmp(types[i].ext, t->ext); i++);
			if (i == ntypes) { snprintf(types[i].ext, sizeof(types[i].ext), "%s", t->ext); types[i].bytes = 0; types[i].files = 0; ntypes++; }
			types[i].bytes += t->bytes;
			types[i].files += t->files;
		}
	}
}

// what a file type is, in words
static const char *Type_Name(const char *ext)
{
	static const struct { const char *exts, *name; } names[] = {
		// (psdevwiki's Template:File Formats for the console's own: pbp, rco/qrc/raf/cxml/gim, psv, vm2...)
		{"iso img pbp cso zso chd mdf nrg", N_("Disc images")}, {"bin", N_("Disc images and data")}, {"cue", N_("Disc image track lists")},
		{"pkg", N_("Packages")}, {"edat sdat", N_("Licensed game data")}, {"sprx self elf prx", N_("Programs")},
		{"psarc pak arc", N_("Game archives")}, {"dat rco qrc raf cxml gim", N_("Game data")}, {"mp3 aac m4a wav at3 flac ogg wma", N_("Music")},
		{"mp4 avi mkv m2ts mpg mpeg mts wmv m4v", N_("Video")}, {"jpg jpeg png bmp gif", N_("Pictures")}, {"p3t", N_("Themes")},
		{"rap rif act", N_("Licences")}, {"sfo pfd sfd psv vmc vm1 vm2 vme mcr", N_("Save and game info")}, {"trp", N_("Trophies")},
		{"txt xml ini cfg log", N_("Text and settings")}, {"db", N_("Databases")}, {"zip rar 7z gz", N_("Archives")},
		{"smc sfc nes gba gb gbc md gen n64 z64 sms gg pce", N_("ROMs")}, {"", N_("No extension")}, {"*", N_("Everything else")},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *e = names[i].exts;
		size_t n = strlen(ext);
		if (!n && !e[0]) return names[i].name;
		for (const char *p = e; n && *p; ) {
			const char *sp = strchr(p, ' ');
			size_t l = sp ? (size_t)(sp - p) : strlen(p);
			if (l == n && !strncmp(p, ext, n)) return names[i].name;
			p += l + (sp ? 1 : 0);
		}
	}
	return NULL;
}

static const scan_change_t *Change_At(int i) { return &DRV->changes->c[i]; }
static uint64_t Change_Size(const scan_change_t *c) { return c->now > c->was ? c->now - c->was : c->was - c->now; }

static const char *Base(const char *path)
{
	const char *b = strrchr(path, '/');
	return b ? b + 1 : path;
}

// a title the scan gave (scan.c's Split_All, System_Title), in English: shown in the language on screen
static int Known_Title(const char *t)
{
	static const char *const known[] = {N_("Recycle bin"), N_("SingStar songs"), N_("IRISMAN's cache"), N_("Apollo's cache"),
		N_("multiMAN's covers"), N_("Firmware update")};
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) if (!strcmp(t, known[i])) return 1;
	return 0;
}

static const char *Item_Name(const scan_item_t *it)
{
	if (it->kind == ITEM_REST) return tr("(more items)");
	if (Known_Title(it->title)) return tr(it->title);		// (the scan's own names: in the language on screen)
	return it->title[0] ? it->title : Base(it->path);
}

// Recently added: an item changed in the last days (its newest file's time); the bin, the
// "more items" rows and a time past now (a clock that was wrong then) left out
static int Recent(const scan_item_t *it, int days)
{
	int64_t now = (int64_t)time(NULL);
	return it->kind != ITEM_REST && !scan_item_is_bin(it) && it->bytes && it->newest > 0 && it->newest <= now + 86400 &&
		it->newest >= now - (int64_t)days * 86400;
}

uint64_t list_recent_bytes(int days)
{
	const scan_t *s = DRV->shown;
	uint64_t t = 0;
	for (uint32_t i = 0; s && i < s->num_items; i++) if (Recent(&s->items[i], days)) t += s->items[i].bytes;
	return t;
}

// a change's name (the recycle bin's, in the language on screen)
static const char *Change_Name(const scan_change_t *c)
{
	if (Known_Title(c->title)) return tr(c->title);
	return c->title[0] ? c->title : Base(c->path);
}

// needle in hay, ignoring case (ASCII; other characters as they are)
static int Match(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!n) return 1;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n)) return 1;
	return 0;
}

static int Row_Wanted(int i)
{
	const scan_t *s = DRV->shown;
	view_t *v = Cur_View();
	if (mode == LS_FOLDER) return Match(F_Name(i), v->query);
	if (mode == LS_BIN) return Match(bin_rows[i].rel, v->query);
	if (mode == LS_SAME) return same->g[i].n >= 2 && Same_Left(i) >= 2 && Match(Same_First(i), v->query);
	if (mode == LS_SAMEF) return same->path[same->g[same_group].first + i] && Match(same->path[same->g[same_group].first + i], v->query);
	if (mode == LS_FILTER) {		// over the size, and untouched as long (an item whose time isn't known: not, then)
		const scan_item_t *it = &s->items[i];
		if (it->kind == ITEM_REST || scan_item_is_bin(it) || it->bytes < flt_sizes[flt_size]) return 0;		// (the bin's its own tile)
		if (flt_days[flt_age] && (it->newest <= 0 || it->newest > (int64_t)time(NULL) - (int64_t)flt_days[flt_age] * 86400)) return 0;
		return Match(it->title, v->query) || Match(it->path, v->query);
	}
	if (mode == LS_RECENT) {		// (changed in the span: a time past now is a clock that was wrong, not new)
		const scan_item_t *it = &s->items[i];
		if (!Recent(it, rec_days[rec_span])) return 0;
		return Match(it->title, v->query) || Match(it->path, v->query);
	}
	if (mode == LS_CHANGES) return Match(Change_At(i)->title, v->query) || Match(Change_At(i)->path, v->query);
	if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_OLD) {
		const game_t *g = &Games()->g[i];
		uint64_t b;
		if (mode == LS_DUPES && g->copies < 2 && !(g->copies && Elsewhere(g, &b) >= 0)) return 0;
		if (mode == LS_OLD && (!g->copies && !g->bytes[PART_INSTALLED])) return 0;		// (a game that's here, not only its saves)
		return Match(g->title, v->query) || Match(g->tid, v->query);
	}
	if (mode == LS_FOLDERS) { char p[FSX_PATH]; Dir_Path(s, folders[i], p, sizeof(p)); return Match(p, v->query); }
	if (mode == LS_MARKS) return Match(marks[mark_rows[i]].rel, v->query);
	if (mode == LS_TYPES) return Match(types[i].ext, v->query) || (Type_Name(types[i].ext) && Match(tr(Type_Name(types[i].ext)), v->query));
	if (mode == LS_GAME || mode == LS_FREE) {
		const scan_item_t *it = &s->items[Row_Item(i)];
		return Match(it->title, v->query) || Match(it->path, v->query);
	}
	if (mode == LS_LARGEST && lg_files) {
		const scan_big_t *b = Lg_File(s, i);
		if (lg_type >= 0 && (b->type >= s->num_types[b->cat] || strcmp(s->types[b->cat][b->type].ext, lg_ext))) return 0;	// (a type's)
		return (!lg_tab || b->cat == lg_tab - 1) && Match(b->path, v->query);
	}
	const scan_item_t *it = &s->items[i];
	int cat = mode == LS_ITEMS ? items_cat : lg_tab - 1;
	if (cat >= 0 && it->cat != cat) return 0;
	return Match(it->title, v->query) || Match(it->path, v->query);
}

static uint64_t Row_Bytes(int i)
{
	const scan_t *s = DRV->shown;
	if (mode == LS_FOLDER) return fl->e[i].bytes;
	if (mode == LS_BIN) return bin_rows[i].bytes;
	if (mode == LS_SAME) return same->g[i].size * (uint64_t)(Same_Left(i) - 1);
	if (mode == LS_SAMEF) return same->g[same_group].size;
	if (mode == LS_CHANGES) return Change_Size(Change_At(i));
	if (mode == LS_GAMES) return Games()->g[i].total;
	if (mode == LS_DUPES) return Dup_Free(&Games()->g[i]);
	if (mode == LS_OLD) return Games()->g[i].bytes[PART_INSTALLED] + Games()->g[i].bytes[PART_BACKUP];
	if (mode == LS_FOLDERS) return s->dir[folders[i]].bytes;
	if (mode == LS_MARKS) return Mark_Bytes(i);
	if (mode == LS_TYPES) return types[i].bytes;
	if (mode == LS_GAME || mode == LS_FREE) return s->items[Row_Item(i)].bytes;
	if (mode == LS_LARGEST && lg_files) return Lg_File(s, i)->bytes;
	return s->items[i].bytes;
}

static const char *Row_Name(int i)
{
	const scan_t *s = DRV->shown;
	if (mode == LS_FOLDER) return F_Name(i);
	if (mode == LS_BIN) return Base(bin_rows[i].rel);
	if (mode == LS_SAME) return Base(Same_First(i));
	if (mode == LS_SAMEF) return Base(same->path[same->g[same_group].first + i]);
	if (mode == LS_CHANGES) return Change_Name(Change_At(i));
	if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_OLD) return Games()->g[i].title;
	if (mode == LS_FOLDERS) return s->names + s->dir[folders[i]].name;
	if (mode == LS_MARKS) { const char *r = marks[mark_rows[i]].rel, *b = strrchr(r, '/'); return b ? b + 1 : r; }
	if (mode == LS_TYPES) return types[i].ext;
	if (mode == LS_GAME || mode == LS_FREE) return Item_Name(&s->items[Row_Item(i)]);
	if (mode == LS_LARGEST && lg_files) return Base(Lg_File(s, i)->path);
	return Item_Name(&s->items[i]);
}

// a sort's name (Not played lately's first is oldest first)
static const char *Sort_Label(int sort)
{
	if (mode == LS_RECENT && sort == SORT_SIZE) return tr("Newest first");
	if (mode == LS_RECENT && sort == SORT_SMALL) return tr("Largest first");
	if (mode == LS_OLD && sort == SORT_SIZE) return tr("Oldest first");
	if (mode == LS_OLD && sort == SORT_SMALL) return tr("Largest first");
	return tr(sort_names[sort]);
}

static int sort_by;
static int Row_Cmp(const void *pa, const void *pb)
{
	int a = *(const int *)pa, b = *(const int *)pb;
	if (mode == LS_FREE || mode == LS_DUPES) {		// (kept ones last)
		int ka = Row_Kept(a), kb = Row_Kept(b);
		if (ka != kb) return ka - kb;
	}
	if (sort_by == SORT_NAME) {
		// (folders first, as the XMB lists them)
		if (mode == LS_FOLDER && (fl->e[a].type == FSX_DIR) != (fl->e[b].type == FSX_DIR))
			return fl->e[a].type == FSX_DIR ? -1 : 1;
		int c = strcasecmp(Row_Name(a), Row_Name(b));
		return c ? c : a - b;
	}
	if (mode == LS_OLD && sort_by == SORT_SIZE) {		// (its first sort: the longest untouched first)
		int64_t x = Games()->g[a].played, y = Games()->g[b].played;
		if (x != y) return x < y ? -1 : 1;
		return a - b;
	}
	if (mode == LS_RECENT && sort_by == SORT_SIZE) {		// (its first sort: the newest first)
		int64_t x = DRV->shown->items[a].newest, y = DRV->shown->items[b].newest;
		if (x != y) return x > y ? -1 : 1;
		return a - b;
	}
	uint64_t x = Row_Bytes(a), y = Row_Bytes(b);
	if (x != y) return (x < y) == (sort_by == SORT_SIZE || mode == LS_OLD || mode == LS_RECENT) ? 1 : -1;
	return a - b;
}

// the rows again (after a search, a sort, a tab, a new scan); the selection kept where it was
static void Rebuild(void)
{
	view_t *v = Cur_View();
	const scan_t *s = DRV->shown;
	int n = 0;
	if (mode == LS_FOLDER) n = fl ? fl->n : 0;
	else if (mode == LS_CHANGES) n = DRV->changes ? (int)DRV->changes->count : 0;
	else if (mode == LS_BIN) { free(bin_rows); bin_rows = NULL; nbin_rows = bin_load(DRV->root, &bin_rows); n = nbin_rows; }
	else if (mode == LS_SAME) n = Same_Here() ? same->ng : 0;
	else if (mode == LS_SAMEF) n = Same_Here() && same_group < same->ng ? (int)same->g[same_group].n : 0;
	else if (!s) n = 0;
	else if (mode == LS_FOLDERS) { Folders_Build(); n = nfolders; }
	else if (mode == LS_MARKS) { Marks_Rows(); n = nmark_rows; }
	else if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_GAME || mode == LS_FREE || mode == LS_OLD) {
		games_t *g = Games();
		if (g && mode == LS_GAME) {		// (the game, found again by its ID: the games may have been made again)
			game_sel = -1;
			for (int x = 0; x < g->ng && game_sel < 0; x++) if (!strcmp(g->g[x].tid, game_tid)) game_sel = x;
			if (game_sel < 0) { mode = game_parent; Rebuild(); return; }
		}
		n = !g ? 0 : mode == LS_FREE ? g->nspare : mode == LS_GAME ? (int)g->g[game_sel].n : g->ng;
	}
	else if (mode == LS_TYPES) { Types_Build(); n = ntypes; }
	else if (mode == LS_FILTER || mode == LS_RECENT) n = (int)s->num_items;
	else if (mode == LS_LARGEST && lg_files) n = (int)(lg_type >= 0 ? s->num_tfiles : s->num_big);
	else n = (int)s->num_items;
	if (n > v->cap) {
		int *r = realloc(v->rows, n * sizeof(int));
		if (!r) n = v->cap;
		else { v->rows = r; v->cap = n; }
	}
	v->count = 0;
	for (int i = 0; i < n; i++)
		if (Row_Wanted(i)) v->rows[v->count++] = i;
	sort_by = v->sort;
	if (v->count > 1) qsort(v->rows, v->count, sizeof(int), Row_Cmp);
	if (v->sel >= v->count) v->sel = v->count ? v->count - 1 : 0;
	if (v->sel < 0) v->sel = 0;
	ui_dirty();
}

static void Clamp_Top(view_t *v)
{
	if (v->sel < v->top) v->top = v->sel;
	if (v->sel >= v->top + ROWS) v->top = v->sel - ROWS + 1;
	if (v->top > v->count - ROWS) v->top = v->count - ROWS;
	if (v->top < 0) v->top = 0;
}

/*
==================
Opening
==================
*/
void list_open_items(int cat)
{
	if (mode != LS_ITEMS || items_cat != cat) {
		v_items.sel = v_items.top = 0;
		v_items.query[0] = 0;
	}
	mode = LS_ITEMS;
	items_cat = cat;
	Rebuild();
}

void list_open_games(int dupes)
{
	mode = dupes ? LS_DUPES : LS_GAMES;
	view_t *v = Cur_View();
	v->sel = v->top = 0;
	v->query[0] = 0;
	Rebuild();
}

void list_open_mode(int which)		// LIST_FOLDERS, LIST_OLD, LIST_MARKS, LIST_BIN, LIST_FILTER, LIST_SAME
{
	mode = which == LIST_FOLDERS ? LS_FOLDERS : which == LIST_OLD ? LS_OLD : which == LIST_MARKS ? LS_MARKS
		: which == LIST_BIN ? LS_BIN : which == LIST_FILTER ? LS_FILTER : which == LIST_RECENT ? LS_RECENT : LS_SAME;
	if (mode == LS_SAME && !Same_Here() && !same_running()) same_start(DRV->root);		// (looked for once a drive, till asked again)
	view_t *v = Cur_View();
	v->sel = v->top = 0;
	v->query[0] = 0;
	Rebuild();
}

void list_open_free(void)
{
	mode = LS_FREE;
	v_free.sel = v_free.top = 0;
	v_free.query[0] = 0;
	Rebuild();
}

void list_open_types(void)
{
	mode = LS_TYPES;
	v_types.sel = v_types.top = 0;
	v_types.query[0] = 0;
	Rebuild();
}

// what Free up space found: everything, and what of it the delete rules let go
// the START menu's figures for this drive: what's likely safe to clear, the games, those held
// more than once (as Duplicates lists them), the bookmarks
void list_figures(uint64_t *spare, int *games, int *dupes, int *bookmarks)
{
	games_t *g = Games();
	*spare = g ? g->spare_bytes : 0;
	*games = *dupes = *bookmarks = 0;
	for (int i = 0; g && i < g->ng; i++) {
		const game_t *x = &g->g[i];
		uint64_t b;
		if (x->copies || x->bytes[PART_INSTALLED]) ++*games;
		if ((x->copies >= 2 || (x->copies && Elsewhere(x, &b) >= 0)) && !Kept(Game_Key(x))) ++*dupes;
	}
	if (g) {		// (what's kept isn't suggested)
		*spare = 0;
		for (int i = 0; i < g->nspare; i++)
			if (!Kept(DRV->shown->items[g->spare[i].item].path)) *spare += DRV->shown->items[g->spare[i].item].bytes;
	}
	if (!marks_loaded) Marks_Load();
	for (int i = 0; i < nmarks; i++) if (!strcmp(marks[i].root, DRV->root)) ++*bookmarks;
}

uint64_t list_spare_bytes(uint64_t *deletable)
{
	games_t *g = Games();
	char why[8];
	*deletable = 0;
	if (!g) return 0;
	uint64_t all = 0;
	for (int i = 0; i < g->nspare; i++) {
		const scan_item_t *it = &DRV->shown->items[g->spare[i].item];
		if (Kept(it->path)) continue;		// (kept: in no total)
		all += it->bytes;
		if (del_kind(DRV->root, it->path, it->kind == ITEM_DIR, why, sizeof(why)) != DEL_NO) *deletable += it->bytes;
	}
	return all;
}

void list_open_changes(void)
{
	mode = LS_CHANGES;
	v_changes.sel = v_changes.top = 0;
	v_changes.query[0] = 0;
	Rebuild();
}

void list_open_largest(void)
{
	mode = LS_LARGEST;
	lg_ext[0] = 0;
	lg_type = -1;
	lg_parent = -1;
	v_largest.sel = v_largest.top = 0;
	Rebuild();
}

static void Folder_List(void)
{
	char abs[FSX_PATH];
	if (f_job) job_release(f_job);		// (one still reading: it's let go of, and frees what it read)
	f_job = NULL;
	Flist_Free(fl);
	fl = NULL;
	v_folder.count = v_folder.sel = v_folder.top = 0;
	v_folder.query[0] = 0;
	snprintf(abs, sizeof(abs), "%s%s%s", DRV->root, f_rel[0] ? "/" : "", f_rel);
	job_t *j = job_new(List_Run, List_Cleanup);
	if (!j || !(j->arg = strdup(abs))) {
		if (j) job_release(j);
		fl = calloc(1, sizeof(*fl));
		if (fl) fl->err = -1;
		return;
	}
	f_job = j;
	job_post(j, LANE_API);
	ui_dirty();
}

// a folder (rel under the drive), opened from a list: Circle at base goes back to it
static void Folder_Open(const char *rel, int base, const char *pick, const char *title)
{
	f_return = mode;
	mode = LS_FOLDER;
	snprintf(f_rel, sizeof(f_rel), "%s", rel);
	f_base = base;
	snprintf(f_pick, sizeof(f_pick), "%s", pick);
	snprintf(f_title, sizeof(f_title), "%s", title);
	Folder_List();
}

// the folder's arrived from the job
static void Folder_Arrived(void)
{
	fl = f_job->result;
	f_job->result = NULL;
	job_release(f_job);
	f_job = NULL;
	if (!fl) { fl = calloc(1, sizeof(*fl)); if (fl) fl->err = -1; }
	if (!fl) return;
	Folder_Sizes();
	Rebuild();
	if (f_pick[0])
		for (int r = 0; r < v_folder.count; r++)
			if (!strcmp(F_Name(v_folder.rows[r]), f_pick)) { v_folder.sel = r; break; }
	f_pick[0] = 0;
	if (f_keep_sel >= 0) {		// (after a delete: where it was, or the last row)
		v_folder.sel = f_keep_sel < v_folder.count ? f_keep_sel : v_folder.count ? v_folder.count - 1 : 0;
		f_keep_sel = -1;
	}
	Clamp_Top(&v_folder);
}

// a path's parent and last part ("game/X/a.bin": "game/X", "a.bin")
static void Split(const char *path, char *parent, int psize, const char **name)
{
	const char *slash = strrchr(path, '/');
	*name = slash ? slash + 1 : path;
	int n = slash ? (int)(slash - path) : 0;
	if (n >= psize) n = psize - 1;
	memcpy(parent, path, n);
	parent[n] = 0;
}

// the selected row as something to delete: its path under the drive, what it is, its size
static int Row_Target(char *rel, int size, int *is_dir, uint64_t *bytes, uint32_t *files, const char **name)
{
	view_t *v = Cur_View();
	const scan_t *s = DRV->shown;
	if (!v->count || (!s && mode != LS_BIN)) return 0;
	int i = v->rows[v->sel];
	if (mode == LS_FOLDER) {
		if (!fl) return 0;
		const fent_t *e = &fl->e[i];
		if (e->type != FSX_FILE && e->type != FSX_DIR) return 0;
		if (snprintf(rel, size, "%s%s%s", f_rel, f_rel[0] ? "/" : "", F_Name(i)) >= size) return 0;
		*is_dir = e->type == FSX_DIR;
		*bytes = e->bytes;
		*files = e->type == FSX_DIR ? e->files : 1;
		*name = F_Name(i);
		return 1;
	}
	if (mode == LS_CHANGES || mode == LS_GAMES || mode == LS_DUPES || mode == LS_TYPES || mode == LS_OLD || mode == LS_MARKS || mode == LS_SAME) return 0;		// (not a place on the drive)
	if (mode == LS_BIN) {		// an entry of the bin: for good
		const bin_entry_t *e = &bin_rows[i];
		if (!bin_entry_rel(DRV->root, e->id, rel, size)) return 0;
		*is_dir = 1;
		*bytes = e->bytes;
		*files = e->files;
		*name = Base(e->rel);
		return 1;
	}
	if (mode == LS_SAMEF) {
		const char *pth = same->path[same->g[same_group].first + i];
		if (!pth || snprintf(rel, size, "%s", pth) >= size) return 0;
		*is_dir = 0;
		*bytes = same->g[same_group].size;
		*files = 1;
		*name = Base(pth);
		return 1;
	}
	if (mode == LS_FOLDERS) {
		if (!Dir_Path(s, folders[i], rel, size)) return 0;
		*is_dir = 1;
		*bytes = s->dir[folders[i]].bytes;
		*files = s->dir[folders[i]].files;
		*name = s->names + s->dir[folders[i]].name;
		return 1;
	}
	if (mode == LS_GAME || mode == LS_FREE) i = Row_Item(i);
	if (mode == LS_LARGEST && lg_files) {
		snprintf(rel, size, "%s", Lg_File(s, i)->path);
		*is_dir = 0;
		*bytes = Lg_File(s, i)->bytes;
		*files = 1;
		*name = Base(Lg_File(s, i)->path);
		return 1;
	}
	const scan_item_t *it = &s->items[i];
	if (it->kind == ITEM_REST) return 0;
	snprintf(rel, size, "%s", it->path);
	*is_dir = it->kind == ITEM_DIR;
	*bytes = it->bytes;
	*files = it->kind == ITEM_DIR ? it->files : 1;
	*name = Item_Name(it);
	return 1;
}

static int Row_Deletable(void)
{
	char rel[FSX_PATH], why[8];
	int is_dir;
	uint64_t bytes;
	uint32_t files;
	const char *name;
	return delete_enabled() && Row_Target(rel, sizeof(rel), &is_dir, &bytes, &files, &name) &&
		del_kind(DRV->root, rel, is_dir, why, sizeof(why)) != DEL_NO;
}

void list_bin_changed(void) { if (mode == LS_BIN) Rebuild(); }

void list_after_delete(void)
{
	if (mode == LS_GAME || mode == LS_FREE) { Rebuild(); return; }
	if (mode == LS_SAMEF && Same_Here()) {		// (the file gone: off its group)
		view_t *v = &v_samef;
		if (v->count) {
			char abs[FSX_PATH];
			int type;
			uint64_t sz;
			char **pp = &same->path[same->g[same_group].first + v->rows[v->sel]];
			snprintf(abs, sizeof(abs), "%s/%s", DRV->root, *pp ? *pp : "");
			if (*pp && fsx_stat(abs, &type, &sz) != 0) { free(*pp); *pp = NULL; }
		}
	}
	if (mode == LS_FOLDER) {
		f_keep_sel = v_folder.sel;
		Folder_List();
	} else {
		Rebuild();
	}
}

static void Open_Selected(void)
{
	view_t *v = Cur_View();
	if (!v->count) return;
	int i = v->rows[v->sel];
	const scan_t *s = DRV->shown;
	char parent[FSX_PATH];
	const char *name;
	if (mode == LS_FOLDER) {
		if (!fl || fl->e[i].type != FSX_DIR) return;
		size_t l = strlen(f_rel);
		if (l + 1 + strlen(F_Name(i)) + strlen(DRV->root) + 2 >= sizeof(f_rel)) return;
		snprintf(f_rel + l, sizeof(f_rel) - l, "%s%s", l ? "/" : "", F_Name(i));
		f_pick[0] = 0;
		ui_sound(SND_OK);
		Folder_List();
		return;
	}
	if (mode == LS_BIN) {		// put it back where it was
		const bin_entry_t *e = &bin_rows[i];
		char entry[FSX_PATH];
		if (e->lost) {
			ui_note(tr("Where it was wasn't saved: it can't be put back, only deleted for good."));
			ui_sound(SND_BACK);
			return;
		}
		if (plat_read_only(DRV->root)) {
			ui_note(tr("This NTFS drive is read-only: Windows didn't close it cleanly."));
			ui_sound(SND_BACK);
			return;
		}
		if (ui_bin_busy()) {		// (old entries going for good just now: this one could be half gone)
			ui_note(tr("The recycle bin's being tidied: try again in a moment."));
			ui_sound(SND_BACK);
			return;
		}
		int r = bin_entry_rel(DRV->root, e->id, entry, sizeof(entry)) ? del_restore(DRV->root, entry, e->rel, e->is_dir) : -1;
		if (r == 0) {
			if (e->twin[0] && del_restore(DRV->root, entry, e->twin, 0) != 0) {
				// its twin (a .bin's .cue) couldn't go back: the entry stays, as the twin alone (the
				// later line for an id is the one that counts), to be put back or deleted on its own
				bin_entry_t t = *e;
				char tw[FSX_PATH], abs[FSX_PATH];
				int type;
				uint64_t sz = 0;
				snprintf(tw, sizeof(tw), "%s", e->twin);
				snprintf(t.rel, sizeof(t.rel), "%s", tw);
				t.twin[0] = 0;
				t.is_dir = 0;
				t.files = 1;
				const char *base = strrchr(tw, '/');
				if (snprintf(abs, sizeof(abs), "%s/%s/%s", DRV->root, entry, base ? base + 1 : tw) < (int)sizeof(abs) && fsx_stat(abs, &type, &sz) == 0) t.bytes = sz;
				bin_add(DRV->root, &t);
				plat_log("bin: %s put back; its twin %s couldn't be: it stays in the bin", e->rel, tw);
				ui_bin_counted(cur_drive, -(int64_t)(e->bytes > t.bytes ? e->bytes - t.bytes : 0), -(int32_t)(e->files > 1 ? e->files - 1 : 0));
				ui_note(tr("Put back, but not its .cue/.bin: that's still in the recycle bin."));
				ui_sound(SND_OK);
				Rebuild();
				return;
			}
			del_bin_drop(DRV->root, entry);
			bin_remove(DRV->root, e->id);
			plat_log("bin: %s put back", e->rel);
			ui_bin_counted(cur_drive, -(int64_t)e->bytes, -(int32_t)e->files);		// (the bin's row: what's out of it; where it went counts at the next scan)
			ui_note(tr("Put back where it was. Its size counts again after the next scan."));
			ui_sound(SND_OK);
			Rebuild();
		} else {
			ui_note(r == -17 ? tr("Something's where it was now: not put back.") : tr("It couldn't be put back (see the log)."));
			ui_sound(SND_BACK);
		}
		return;
	}
	if (mode == LS_SAME) {		// a group: its files
		ui_sound(SND_OK);
		Same_Refresh(i, -1);		// (what's gone since the look: off it)
		same_group = i;
		mode = LS_SAMEF;
		v_samef.sel = v_samef.top = 0;
		v_samef.query[0] = 0;
		Rebuild();
		return;
	}
	if (mode == LS_SAMEF) {		// a file: its folder, with it picked
		const char *pth = same->path[same->g[same_group].first + i];
		if (!pth) return;
		Split(pth, parent, sizeof(parent), &name);
		ui_sound(SND_OK);
		Folder_Open(parent, (int)strlen(parent), name, Base(parent[0] ? parent : DRV->root));
		return;
	}
	if (mode == LS_CHANGES) {		// what's there now: its folder (or the folder it's in)
		const scan_change_t *c = Change_At(i);
		if (!c->now_there) { ui_note(tr("It's gone since the scan before.")); ui_sound(SND_BACK); return; }
		ui_sound(SND_OK);
		if (c->kind == ITEM_FILE) {
			Split(c->path, parent, sizeof(parent), &name);
			Folder_Open(parent, (int)strlen(parent), name, Change_Name(c));
		} else Folder_Open(c->path, (int)strlen(c->path), "", Change_Name(c));
		return;
	}
	if (mode == LS_FOLDERS || mode == LS_MARKS) {		// a folder: its contents
		char rel[FSX_PATH];
		if (mode == LS_FOLDERS && !Dir_Path(s, folders[i], rel, sizeof(rel))) return;
		if (mode == LS_MARKS) snprintf(rel, sizeof(rel), "%s", marks[mark_rows[i]].rel);
		ui_sound(SND_OK);
		Folder_Open(rel, (int)strlen(rel), "", Base(rel[0] ? rel : DRV->root));
		return;
	}
	if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_OLD) {		// a game: its parts
		ui_sound(SND_OK);
		game_parent = mode;
		game_sel = i;
		snprintf(game_tid, sizeof(game_tid), "%s", Games()->g[i].tid);
		mode = LS_GAME;
		v_game.sel = v_game.top = 0;
		v_game.query[0] = 0;
		Rebuild();
		return;
	}
	if (mode == LS_TYPES) {		// a type: the largest files of it
		ui_sound(SND_OK);
		snprintf(lg_ext, sizeof(lg_ext), "%s", types[i].ext);
		lg_type = 1;
		lg_parent = LS_TYPES;
		lg_tab = ty_tab;
		lg_files = 1;
		mode = LS_LARGEST;
		v_largest.sel = v_largest.top = 0;
		v_largest.query[0] = 0;
		Rebuild();
		return;
	}
	if (mode == LS_GAME || mode == LS_FREE) i = Row_Item(i);		// (then as an item)
	if (mode == LS_LARGEST && lg_files) {		// a file: its folder, with it picked
		Split(Lg_File(s, i)->path, parent, sizeof(parent), &name);
		ui_sound(SND_OK);
		Folder_Open(parent, (int)strlen(parent), name, Base(parent[0] ? parent : DRV->root));
		return;
	}
	const scan_item_t *it = &s->items[i];
	if (it->kind == ITEM_REST) {
		ui_note(tr("These are past the 50,000 items a scan keeps apart: their folders aren't listed one by one."));
		return;
	}
	ui_sound(SND_OK);
	if (it->kind == ITEM_FILE) {
		Split(it->path, parent, sizeof(parent), &name);
		Folder_Open(parent, (int)strlen(parent), name, Item_Name(it));
	} else {
		Folder_Open(it->path, (int)strlen(it->path), "", Item_Name(it));
	}
}

// Circle in a folder: up one, or back to the list from the item's own
static void Folder_Up(void)
{
	if ((int)strlen(f_rel) <= f_base) {
		if (f_job) job_release(f_job);
		f_job = NULL;
		Flist_Free(fl);
		fl = NULL;
		mode = f_return;
		Rebuild();
		return;
	}
	char *slash = strrchr(f_rel, '/');
	snprintf(f_pick, sizeof(f_pick), "%s", slash ? slash + 1 : f_rel);
	if (slash) *slash = 0; else f_rel[0] = 0;
	char keep[FSX_NAME];
	snprintf(keep, sizeof(keep), "%s", f_pick);
	Folder_List();
	snprintf(f_pick, sizeof(f_pick), "%s", keep);
}

/*
==================
Update
==================
*/
static void Search_Open(void)
{
	char title[96];
	view_t *v = Cur_View();
	snprintf(title, sizeof(title), "%s", tr("Search by name"));
	if (plat_osk_open(title, v->query)) searching = 1;
}

void list_scan_changed(void)
{
	if (mode == LS_FOLDER) {
		if (fl) { Folder_Sizes(); Rebuild(); }
	} else {
		Rebuild();
	}
}

int list_update(uint32_t p)
{
	view_t *v = Cur_View();
	if (searching) {
		char q[128];
		int r = plat_osk_poll(q, sizeof(q));
		if (!r) return 1;
		searching = 0;
		if (r > 0) {
			snprintf(v->query, sizeof(v->query), "%s", q);
			v->sel = v->top = 0;
			Rebuild();
		}
		ui_dirty();
		return 1;
	}
	if (mode == LS_FOLDER && f_job && job_done(f_job)) Folder_Arrived();
	if (same_running()) {
		same_t *r = same_take();
		if (r) { same_free(same); same = r; if (mode == LS_SAME || mode == LS_SAMEF) { mode = LS_SAME; Rebuild(); } ui_sound(SND_OK); }
		else if (mode == LS_SAME && plat_time_us() - same_drawn > 500000) { same_drawn = plat_time_us(); ui_dirty(); }
	}
	if (parts_open && !parts_done && parts_poll(&parts) != 0) { parts_done = 1; ui_dirty(); }
	if (parts_open) {		// a game's parts, over its screen: Circle or Square closes it
		if (p & (BTN_CIRCLE | BTN_SQUARE | BTN_CROSS)) { parts_open = 0; parts_shutdown(); ui_sound(SND_BACK); ui_dirty(); }
		return 1;
	}
	if (!DRV->shown && mode != LS_BIN) return 0;		// (the drive went: back to the overview; the bin's there without a scan)
	if (ui_drive_hung(cur_drive)) return 0;		// (its scan was given up on: what's done from a list would wait on the drive)

	int old = v->sel;
	if (p & BTN_UP) v->sel--;
	if (p & BTN_DOWN) v->sel++;
	if (p & (BTN_RIGHT | BTN_R2)) v->sel += ROWS;
	if (p & (BTN_LEFT | BTN_L2)) v->sel -= ROWS;
	if (v->sel >= v->count) v->sel = (p & (BTN_DOWN)) && old == v->count - 1 ? 0 : v->count - 1;		// (Down at the end: the top)
	if (v->sel < 0) v->sel = (p & BTN_UP) && old == 0 && v->count ? v->count - 1 : 0;
	if (v->sel != old) { ui_sound(SND_MOVE); ui_dirty(); }
	Clamp_Top(v);

	if (p & (BTN_L1 | BTN_R1)) {
		int d = (p & BTN_R1) ? 1 : -1;
		if (mode == LS_ITEMS) {
			items_cat = (items_cat + d + NUM_SCANNED_CATS) % NUM_SCANNED_CATS;
			v->sel = v->top = 0;
			Rebuild();
			ui_sound(SND_MOVE);
		} else if (mode == LS_LARGEST) {
			lg_tab = (lg_tab + d + NUM_SCANNED_CATS + 1) % (NUM_SCANNED_CATS + 1);
			v->sel = v->top = 0;
			Rebuild();
			ui_sound(SND_MOVE);
		} else if (mode == LS_TYPES) {
			ty_tab = (ty_tab + d + NUM_SCANNED_CATS + 1) % (NUM_SCANNED_CATS + 1);
			v->sel = v->top = 0;
			Rebuild();
			ui_sound(SND_MOVE);
		} else if (mode == LS_FILTER) {
			flt_size = (flt_size + d + 5) % 5;
			v->sel = v->top = 0;
			Rebuild();
			ui_sound(SND_MOVE);
		}
	}
	if ((p & BTN_SQUARE) && mode == LS_FOLDER) { ui_sound(SND_OK); Mark_Toggle(DRV->root, f_rel); }
	if ((p & BTN_SQUARE) && mode == LS_MARKS && v->count) {
		ui_sound(SND_BACK);
		Mark_Toggle(DRV->root, marks[mark_rows[v->rows[v->sel]]].rel);
		Rebuild();
	}
	if ((p & BTN_SQUARE) && (mode == LS_FREE || mode == LS_DUPES) && v->count && Games()) {		// keep this (or not)
		int i = v->rows[v->sel];
		ui_sound(SND_OK);
		Keep_Toggle(mode == LS_FREE ? DRV->shown->items[Games()->spare[i].item].path : Game_Key(&Games()->g[i]));
		Rebuild();
	}
	if ((p & BTN_SQUARE) && mode == LS_GAME && Games() && game_sel >= 0) {		// what it's made of: its folders added up
		games_t *g = Games();
		const game_t *gm = &g->g[game_sel];
		const char *dirs[64];
		int hg[64], n = 0;
		for (uint32_t k = 0; k < gm->n && n < 64; k++) {
			const scan_item_t *it = &DRV->shown->items[g->members[gm->first + k]];
			if (games_part_of(it) == PART_INSTALLED && it->kind == ITEM_DIR) { dirs[n] = it->path; hg[n++] = !strcmp(it->code, "HG"); }
		}
		memset(&parts, 0, sizeof(parts));
		parts_done = 0;
		parts_open = parts_start(DRV->root, gm->tid, dirs, hg, n);
		ui_sound(SND_OK);
		ui_dirty();
		return 1;
	}
	if ((p & BTN_SQUARE) && mode == LS_RECENT) { rec_span = (rec_span + 1) % 4; v->sel = v->top = 0; Rebuild(); ui_sound(SND_MOVE); }
	if ((p & BTN_SQUARE) && mode == LS_FILTER) { flt_age = (flt_age + 1) % 5; v->sel = v->top = 0; Rebuild(); ui_sound(SND_MOVE); }
	if ((p & BTN_SQUARE) && mode == LS_BIN) { ui_sound(SND_OK); delete_open_empty_bin(); }
	if ((p & BTN_SQUARE) && mode == LS_SAME && !same_running()) { ui_sound(SND_OK); same_start(DRV->root); ui_dirty(); }
	if ((p & BTN_SQUARE) && mode == LS_ITEMS) {		// this category again, the rest kept
		ui_sound(SND_OK);
		ui_rescan_category(items_cat);
	}
	if ((p & BTN_SQUARE) && mode == LS_LARGEST && lg_type < 0) {
		lg_files = !lg_files;
		v->sel = v->top = 0;
		Rebuild();
		ui_sound(SND_OK);
	}
	if (p & BTN_SELECT) {
		v->sort = (v->sort + 1) % NUM_SORTS;
		v->sel = v->top = 0;
		Rebuild();
		ui_sound(SND_MOVE);
	}
	if (p & BTN_TRIANGLE) { ui_sound(SND_OK); Search_Open(); }
	if (p & BTN_START) {
		char rel[FSX_PATH];
		int is_dir;
		uint64_t bytes;
		uint32_t files;
		const char *name;
		if (mode == LS_SAMEF && Same_Here() && v->count) {		// (Identical files: only while another copy's there, and it's checked against that one)
			const char *other = Same_Refresh(same_group, v->rows[v->sel]);
			if (!same->path[same->g[same_group].first + v->rows[v->sel]]) { Rebuild(); ui_note(tr("It isn't there any more.")); ui_sound(SND_BACK); }
			else if (!other) { Rebuild(); ui_note(tr("It's the only copy left: it isn't deleted from here.")); ui_sound(SND_BACK); }
			else if (Row_Target(rel, sizeof(rel), &is_dir, &bytes, &files, &name)) { delete_same_as(other); delete_open(rel, is_dir, name, bytes, files); }
		} else if (Row_Target(rel, sizeof(rel), &is_dir, &bytes, &files, &name)) delete_open(rel, is_dir, name, bytes, files);
	}
	if (p & BTN_CROSS) Open_Selected();
	if (p & BTN_CIRCLE) {
		ui_sound(SND_BACK);
		if (v->query[0]) {		// (a search is cleared first)
			v->query[0] = 0;
			v->sel = v->top = 0;
			Rebuild();
		} else if (mode == LS_FOLDER) {
			Folder_Up();
		} else if (mode == LS_GAME) {
			mode = game_parent;
			Rebuild();
		} else if (mode == LS_SAMEF) {
			mode = LS_SAME;
			Rebuild();
		} else if (mode == LS_LARGEST && lg_parent >= 0) {
			mode = lg_parent;
			lg_parent = -1;
			lg_ext[0] = 0;
			lg_type = -1;
			Rebuild();
		} else {
			ui_dirty();
			return 0;
		}
	}
	return 1;
}

void list_shutdown(void)
{
	if (f_job) job_release(f_job);
	f_job = NULL;
	Flist_Free(fl);
	fl = NULL;
	free(v_items.rows);
	free(v_largest.rows);
	free(v_folder.rows);
	free(v_changes.rows);
	memset(&v_changes, 0, sizeof(v_changes));
	view_t *more[] = {&v_games, &v_dupes, &v_game, &v_free, &v_types, &v_folders, &v_old, &v_marks};
	for (int i = 0; i < 8; i++) { free(more[i]->rows); memset(more[i], 0, sizeof(view_t)); }
	for (int d = 0; d < NUM_DRIVES; d++) { games_free(other_gm[d]); other_gm[d] = NULL; }
	games_free(gm);
	gm = NULL;
	free(keeps);
	keeps = NULL;
	free(bin_rows);
	bin_rows = NULL;
	nbin_rows = 0;
	same_shutdown();
	same_free(same);
	same = NULL;
	parts_shutdown();
	parts_open = 0;
	view_t *newer[] = {&v_bin, &v_filter, &v_same, &v_samef, &v_recent};
	for (int i = 0; i < 5; i++) { free(newer[i]->rows); memset(newer[i], 0, sizeof(view_t)); }
	nkeeps = keeps_loaded = 0;
	memset(&v_items, 0, sizeof(v_items));
	memset(&v_largest, 0, sizeof(v_largest));
	memset(&v_folder, 0, sizeof(v_folder));
}

/*
==================
Drawing
==================
*/
// a row's picture when there's no icon: a folder in the category's colour, or a file's type
static void Tile(int x, int y, int is_dir, const char *name, uint32_t c)
{
	gfx_round_rect(x, y, THUMB_W, THUMB_H, 6, 0xff141a26);
	if (is_dir) {
		int fw = 38, fh = 26, fx = x + (THUMB_W - fw) / 2, fy = y + (THUMB_H - fh) / 2 + 3;
		gfx_round_rect(fx, fy - 6, 16, 10, 3, with_alpha(c, 0xc0));
		gfx_round_rect(fx, fy, fw, fh, 4, c);
		return;
	}
	char ext[8] = "FILE";
	const char *dot = strrchr(name, '.');
	if (dot && dot != name && strlen(dot + 1) >= 1 && strlen(dot + 1) <= 5) {
		int k = 0;
		for (const char *q = dot + 1; *q && k < 5; q++) ext[k++] = (*q >= 'a' && *q <= 'z') ? *q - 32 : *q;
		ext[k] = 0;
	}
	font_draw_center(FONT_BOLD, 16, x + THUMB_W / 2, y + THUMB_H / 2 - 9, ext, c, THUMB_W - 8);
}

// an item's icon, or its tile
static void Item_Thumb(const scan_item_t *it, int x, int y)
{
	image_t *img = NULL;
	if (it->icon != ICON_NONE) {
		char path[FSX_PATH];
		static const char *const where[NUM_ICON_KINDS] = {"", "/ICON0.PNG", "/PS3_GAME/ICON0.PNG", "::PS3_GAME/ICON0.PNG", "::PSP_GAME/ICON0.PNG"};
		snprintf(path, sizeof(path), "%s/%s%s", DRV->root, it->path, where[it->icon]);
		img = icon_get(path, THUMB_W, THUMB_H);
	}
	if (img) gfx_image(img, x, y, THUMB_W, THUMB_H, 6, 255);
	else Tile(x, y, it->kind != ITEM_FILE, it->path, with_alpha(cat_colors[it->cat], 0xd0));
}

// a save's or trophy's user: home/<id>/localusername (read once per user)
static const char *User_Of(const char *path)
{
	static struct { char id[16]; char name[40]; } users[16];
	static int nusers;
	if (strncmp(path, "home/", 5)) return NULL;
	const char *id = path + 5, *end = strchr(id, '/');
	size_t n = end ? (size_t)(end - id) : strlen(id);
	if (!n || n >= sizeof(users[0].id)) return NULL;
	for (int i = 0; i < nusers; i++)
		if (strlen(users[i].id) == n && !strncmp(users[i].id, id, n)) return users[i].name[0] ? users[i].name : NULL;
	if (nusers == 16) return NULL;
	char p[256];
	snprintf(users[nusers].id, sizeof(users[0].id), "%.*s", (int)n, id);
	snprintf(p, sizeof(p), "%s/home/%s/localusername", DRV->root, users[nusers].id);
	users[nusers].name[0] = 0;
	size_t len;
	unsigned char *b = plat_file_size(p) > 0 && plat_file_size(p) < 256 ? plat_read_file(p, &len) : NULL;
	if (b) {
		snprintf(users[nusers].name, sizeof(users[0].name), "%.*s", (int)len, (const char *)b);
		for (char *q = users[nusers].name; *q; q++) if ((unsigned char)*q < 0x20) *q = 0;
		free(b);
	}
	return users[nusers++].name[0] ? users[nusers - 1].name : NULL;
}

static const char *Kind_Of(const char *code)
{
	static const struct { const char *c, *name; } kinds[] = {
		{"HG", N_("Game")}, {"GD", N_("Game data")}, {"DG", N_("Disc game")}, {"SD", N_("Save")}, {"AP", N_("App")}, {"AT", N_("App")},
		{"2P", N_("PS2 game")}, {"1P", N_("PS1 game")}, {"PP", N_("PSP game")}, {"MN", N_("Minis")}, {"HM", N_("Home")}, {"CB", N_("Network")},
		// (the rest of PARAM.SFO's CATEGORY codes, as psdevwiki's PS3 HDD Game Structure lists them)
		{"AM", N_("App")}, {"AV", N_("App")}, {"BV", N_("App")}, {"WT", N_("App")}, {"AS", N_("App")}, {"SF", N_("App")},
		{"2G", N_("PS2 game")}, {"PE", N_("PSP game")}, {"2D", N_("Game data")}, {"MS", N_("Save")},
	};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) if (!strcmp(code, kinds[i].c)) return kinds[i].name;
	return code;
}

// "A  ·  B" onto line
static void Add(char *line, int size, const char *part)
{
	if (!part || !part[0]) return;
	size_t l = strlen(line);
	snprintf(line + l, size - l, "%s%s", l ? "  \xc2\xb7  " : "", part);
}

// the size on the right, the line under it; returns the width they take
static int Right(int rx, int y, const char *size, const char *under, uint32_t uc)
{
	int a = font_width(FONT_MEDIUM, 22, size), b = under[0] ? font_width(FONT_REGULAR, 17, under) : 0;
	font_draw_right(FONT_MEDIUM, 22, rx, y + 8, size, C_TEXT);
	if (under[0]) font_draw_right(FONT_REGULAR, 17, rx, y + 37, under, uc);
	return a > b ? a : b;
}

static void Draw_Item_Row(const scan_item_t *it, int y, int show_cat)
{
	const scan_t *s = DRV->shown;
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	Item_Thumb(it, LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2);
	char size[32], under[96], pct[16], line[320];
	kit_fmt_size((long long)it->bytes, size, sizeof(size));
	ui_fmt_pct(it->bytes, show_cat ? scan_drive_total(s) : s->cat_bytes[it->cat], pct, sizeof(pct));
	if (show_cat == 2) {		// (Recently added: its category and when)
		char when[32] = "?";
		time_t t = (time_t)it->newest;
		struct tm *tm = localtime(&t);
		if (tm) strftime(when, sizeof(when), "%Y-%m-%d", tm);
		snprintf(under, sizeof(under), "%s  \xc2\xb7  %s", tr(scan_cat_short(it->cat)), when);
	} else if (show_cat) snprintf(under, sizeof(under), "%s", tr(scan_cat_short(it->cat)));
	else if (it->kind == ITEM_FILE || it->kind == ITEM_REST) snprintf(under, sizeof(under), "%s", pct);
	else {
		char files[24];
		kit_fmt_count(it->files, files, sizeof(files));
		snprintf(under, sizeof(under), tr(it->files == 1 ? "%s  \xc2\xb7  %s file" : "%s  \xc2\xb7  %s files"), pct, files);
	}
	int rw = Right(rx, y, size, under, show_cat ? cat_colors[it->cat] : C_DIM);
	int maxw = rx - rw - 24 - tx;
	font_draw(FONT_MEDIUM, 22, tx, y + 8, Item_Name(it), C_TEXT, maxw);
	line[0] = 0;
	if (it->cat == CAT_SAVES) Add(line, sizeof(line), User_Of(it->path));
	if (it->code[0]) Add(line, sizeof(line), tr(Kind_Of(it->code)));
	Add(line, sizeof(line), it->tid);
	Add(line, sizeof(line), it->kind == ITEM_REST ? "" : it->path);
	if (it->unreadable) {
		char u[64];
		snprintf(u, sizeof(u), tr("%u couldn't be read"), (unsigned)it->unreadable);
		Add(line, sizeof(line), u);
	}
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, it->unreadable ? C_WARN : C_DIM, maxw);
}

static void Draw_Change_Row(const scan_change_t *c, int y)
{
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	scan_item_t as_item;		// (its thumbnail: drawn as the item it is)
	memset(&as_item, 0, sizeof(as_item));
	snprintf(as_item.path, sizeof(as_item.path), "%s", c->path);
	as_item.cat = c->cat;
	as_item.kind = c->kind;
	as_item.icon = c->icon;
	Item_Thumb(&as_item, LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2);
	char d[40], size[32], under[96], line[320];
	int grew = c->now > c->was;
	kit_fmt_size((long long)Change_Size(c), size, sizeof(size));
	snprintf(d, sizeof(d), "%s%s", grew ? "+" : "\xe2\x88\x92", size);
	if (!c->was_there) snprintf(under, sizeof(under), "%s", tr("New"));
	else if (!c->now_there) snprintf(under, sizeof(under), "%s", tr("Gone"));
	else {
		char was[32], now[32];
		kit_fmt_size((long long)c->was, was, sizeof(was));
		kit_fmt_size((long long)c->now, now, sizeof(now));
		snprintf(under, sizeof(under), "%s \xe2\x86\x92 %s", was, now);
	}
	int a = font_width(FONT_MEDIUM, 22, d), b = font_width(FONT_REGULAR, 17, under), rw = a > b ? a : b;
	font_draw_right(FONT_MEDIUM, 22, rx, y + 8, d, grew ? C_WARN : C_OK);
	font_draw_right(FONT_REGULAR, 17, rx, y + 37, under, C_DIM);
	int maxw = rx - rw - 24 - tx;
	font_draw(FONT_MEDIUM, 22, tx, y + 8, Change_Name(c), c->now_there ? C_TEXT : C_DIM, maxw);
	line[0] = 0;
	Add(line, sizeof(line), tr(scan_cat_short(c->cat)));
	Add(line, sizeof(line), c->path);
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, maxw);
}

static void Draw_Big_Row(const scan_big_t *b, int y)
{
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 0, b->path, with_alpha(cat_colors[b->cat], 0xd0));
	char size[32], parent[BIG_PATH];
	const char *name;
	kit_fmt_size((long long)b->bytes, size, sizeof(size));
	int rw = Right(rx, y, size, tr(scan_cat_short(b->cat)), cat_colors[b->cat]);
	Split(b->path, parent, sizeof(parent), &name);
	font_draw(FONT_MEDIUM, 22, tx, y + 8, name, C_TEXT, rx - rw - 24 - tx);
	font_draw(FONT_REGULAR, 17, tx, y + 37, parent[0] ? parent : DRV->root, C_DIM, rx - rw - 24 - tx);
}

static void Draw_Entry_Row(int i, int y)
{
	const fent_t *e = &fl->e[i];
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	uint32_t c = 0xd09aa4b8;
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, e->type == FSX_DIR, F_Name(i), e->type == FSX_DIR ? 0xd0ffc861 : c);
	char size[32], pct[16], line[96] = "";
	if (e->known) kit_fmt_size((long long)e->bytes, size, sizeof(size));
	else snprintf(size, sizeof(size), "-");
	ui_fmt_pct(e->bytes, f_total, pct, sizeof(pct));
	int rw = Right(rx, y, size, e->known ? pct : "", C_DIM);
	int maxw = rx - rw - 24 - tx;
	if (e->type == FSX_DIR) {
		if (e->known) {
			char files[24], dirs[24];
			kit_fmt_count(e->files, files, sizeof(files));
			kit_fmt_count(e->dirs, dirs, sizeof(dirs));
			snprintf(line, sizeof(line), tr(e->files == 1 ? "%s file" : "%s files"), files);
			if (e->dirs) {
				size_t l = strlen(line);
				snprintf(line + l, sizeof(line) - l, tr(e->dirs == 1 ? "  \xc2\xb7  %s folder" : "  \xc2\xb7  %s folders"), dirs);
			}
		} else {
			// (its parent kept, it not: new since the scan; the parent not kept either: past the folders a scan keeps)
			snprintf(line, sizeof(line), "%s", f_dir >= 0 || !DRV->shown->dirs_dropped ? tr("New since the scan: rescan for its size")
				: tr("Past the folders a scan keeps sizes for"));
		}
		font_draw(FONT_MEDIUM, 22, tx, y + 8, F_Name(i), C_TEXT, maxw);
		font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, maxw);
	} else {
		font_draw(FONT_MEDIUM, 22, tx, y + 21, F_Name(i), e->type == FSX_FILE ? C_TEXT : C_DIM, maxw);
	}
}

static void Draw_Message(const char *msg)
{
	font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 200, msg, C_DIM, LIST_W - 80);
}

static void Draw_Game_Row(const game_t *g, int y, int dupes)
{
	const scan_t *s = DRV->shown;
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	if (g->icon_item >= 0) Item_Thumb(&s->items[g->icon_item], LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2);
	else Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 1, "", with_alpha(cat_colors[CAT_GAMES], 0xd0));
	char size[32], under[64], line[400], part[64];
	static const char *const parts[NUM_PARTS] = {N_("Installed %s"), N_("Backup %s"), N_("Saves %s"), N_("Trophies %s")};
	uint64_t there = 0;
	int other = dupes ? Elsewhere(g, &there) : -1;
	kit_fmt_size((long long)(dupes ? Dup_Free(g) : g->total), size, sizeof(size));
	int kept = dupes && Kept(Game_Key(g));
	if (kept) snprintf(under, sizeof(under), "%s", tr("Kept"));
	else if (dupes) snprintf(under, sizeof(under), tr(g->copies == 1 ? "%d copy" : "%d copies"), g->copies);
	else snprintf(under, sizeof(under), "%s", g->tid);
	int rw = Right(rx, y, size, under, kept ? C_DIM : dupes ? C_WARN : C_DIM);
	int maxw = rx - rw - 24 - tx;
	font_draw(FONT_MEDIUM, 22, tx, y + 8, g->title, kept ? C_DIM : C_TEXT, maxw);
	line[0] = 0;
	for (int k = 0; k < NUM_PARTS; k++) {
		char sz[32];
		uint64_t bytes = g->bytes[k];
		uint32_t count = g->count[k];
		if (k == PART_INSTALLED) { bytes -= g->data_bytes; count -= g->data_count; }		// (game data and updates: their own)
		if (count) {
			kit_fmt_size((long long)bytes, sz, sizeof(sz));
			snprintf(part, sizeof(part), tr(parts[k]), sz);
			Add(line, sizeof(line), part);
		}
		if (k == PART_INSTALLED && g->data_count) {
			kit_fmt_size((long long)g->data_bytes, sz, sizeof(sz));
			snprintf(part, sizeof(part), tr("Game data %s"), sz);
			Add(line, sizeof(line), part);
		}
	}
	if (other >= 0) {
		char sz[32];
		kit_fmt_size((long long)there, sz, sizeof(sz));
		snprintf(part, sizeof(part), tr("Also on %s (%s)"), ui_drive_name(other), sz);
		Add(line, sizeof(line), part);
	}
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, other >= 0 ? C_CYAN : C_DIM, maxw);
}

// "3 months ago", from seconds
static void Ago(int64_t when, char *out, int size)
{
	int64_t d = ((int64_t)time(NULL) - when) / 86400;
	if (when <= 0) snprintf(out, size, "%s", tr("never"));
	else if (d < 1) snprintf(out, size, "%s", tr("today"));
	else if (d < 60) snprintf(out, size, tr("%d days ago"), (int)d);
	else if (d < 730) snprintf(out, size, tr("%d months ago"), (int)(d / 30));
	else snprintf(out, size, tr("%d years ago"), (int)(d / 365));
}

static void Draw_Old_Row(const game_t *g, int y)
{
	const scan_t *s = DRV->shown;
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	if (g->icon_item >= 0) Item_Thumb(&s->items[g->icon_item], LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2);
	else Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 1, "", with_alpha(cat_colors[CAT_GAMES], 0xd0));
	char size[32], when[32] = "-", ago[48], line[200];
	kit_fmt_size((long long)(g->bytes[PART_INSTALLED] + g->bytes[PART_BACKUP]), size, sizeof(size));		// (installed and backed up: an HDD game's both, once)
	time_t t = (time_t)g->played;
	struct tm *tm = g->played > 0 ? localtime(&t) : NULL;
	if (tm) strftime(when, sizeof(when), "%Y-%m-%d", tm);
	Ago(g->played, ago, sizeof(ago));
	int rw = Right(rx, y, size, ago, C_WARN);
	int maxw = rx - rw - 24 - tx;
	font_draw(FONT_MEDIUM, 22, tx, y + 8, g->title, C_TEXT, maxw);
	snprintf(line, sizeof(line), tr(g->count[PART_SAVE] ? "Last played %s (its newest save)" : "Last changed %s"), when);
	Add(line, sizeof(line), g->tid);
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, maxw);
}

// a folder row: Largest folders' (d in the folder sizes) or a bookmark's (rel; d -1 if not in the scan)
static void Draw_Folder_Row(const char *rel, int d, int y)
{
	const scan_t *s = DRV->shown;
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 1, "", 0xd0ffc861);
	char size[32] = "-", under[48] = "", files[24];
	if (d >= 0) {
		kit_fmt_size((long long)s->dir[d].bytes, size, sizeof(size));
		kit_fmt_count(s->dir[d].files, files, sizeof(files));
		snprintf(under, sizeof(under), tr(s->dir[d].files == 1 ? "%s file" : "%s files"), files);
	}
	int rw = Right(rx, y, size, under, C_DIM);
	int maxw = rx - rw - 24 - tx;
	const char *base = strrchr(rel, '/');
	font_draw(FONT_MEDIUM, 22, tx, y + 8, base ? base + 1 : rel[0] ? rel : DRV->root, C_TEXT, maxw);
	char where[FSX_PATH + 16];
	snprintf(where, sizeof(where), "%s/%s", DRV->root, rel);
	font_draw(FONT_REGULAR, 17, tx, y + 37, where, C_DIM, maxw);
}

static void Draw_Spare_Row(const spare_t *sp, int y)
{
	static const char *const whys[NUM_WHYS] = {N_("Leftover package (already installed)"), N_("webMAN's cache (made again when needed)"),
		N_("Temporary files"), N_("Extra copy of a game held more than once"), N_("Update for a game that isn't here (keep it if you play the disc)"),
		N_("Save for a game that isn't here (keep it if you play the disc)"),
		N_("Left by an install or a delete the system didn't finish"),
		N_("No PARAM.SFO: a broken install, or another app's data (check it first)"),
		N_("Downloaded, not installed yet: install or delete it from the XMB"),
		N_("Waiting to be installed: install or delete it from the XMB"),
		N_("A package that isn't installed: install it, or delete it if it's not needed"),
		N_("An app's cache: clear it in that app (it makes it again)"),
		N_("A system update: not for HEN or CFW (installing it removes them)")};
	const scan_item_t *it = &DRV->shown->items[sp->item];
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	Item_Thumb(it, LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2);
	char size[32], why[8], line[400];
	kit_fmt_size((long long)it->bytes, size, sizeof(size));
	int can = del_kind(DRV->root, it->path, it->kind == ITEM_DIR, why, sizeof(why)) != DEL_NO, kept = Kept(it->path);
	int rw = Right(rx, y, size, kept ? tr("Kept") : can ? tr("Can be deleted") : tr("From the XMB"), kept ? C_DIM : can ? C_OK : C_DIM);
	int maxw = rx - rw - 24 - tx;
	font_draw(FONT_MEDIUM, 22, tx, y + 8, Item_Name(it), kept ? C_DIM : C_TEXT, maxw);
	line[0] = 0;
	Add(line, sizeof(line), tr(whys[sp->why]));
	Add(line, sizeof(line), it->path);
	int check = sp->why == WHY_LONE_UPDATE || sp->why == WHY_LONE_SAVE || sp->why == WHY_NO_SFO || sp->why == WHY_DOWNLOAD || sp->why == WHY_QUEUED || sp->why == WHY_PKG_NEW || sp->why == WHY_TOOL_CACHE;
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, check && !kept ? C_WARN : C_DIM, maxw);
}

static void Draw_Type_Row(const trow_t *t, int y)
{
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	char file[16], size[32], pct[16], name[96], line[64], files[24];
	snprintf(file, sizeof(file), "x.%s", t->ext[0] && strcmp(t->ext, "*") ? t->ext : "...");
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 0, file, 0xd04ad9ff);
	kit_fmt_size((long long)t->bytes, size, sizeof(size));
	ui_fmt_pct(t->bytes, DRV->shown->scanned, pct, sizeof(pct));
	int rw = Right(rx, y, size, pct, C_DIM);
	const char *what = Type_Name(t->ext);
	if (what && t->ext[0] && strcmp(t->ext, "*")) snprintf(name, sizeof(name), "%s  (.%s)", tr(what), t->ext);
	else if (what) snprintf(name, sizeof(name), "%s", tr(what));
	else snprintf(name, sizeof(name), tr(".%s files"), t->ext);
	font_draw(FONT_MEDIUM, 22, tx, y + 8, name, C_TEXT, rx - rw - 24 - tx);
	kit_fmt_count(t->files, files, sizeof(files));
	snprintf(line, sizeof(line), tr(t->files == 1 ? "%s file" : "%s files"), files);
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, rx - rw - 24 - tx);
}

static void Draw_Bin_Row(const bin_entry_t *e, int y)
{
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, e->is_dir, e->rel, 0xd0ff7a7a);
	char size[32], under[64], line[400], when[32] = "-", parent[FSX_PATH];
	const char *name;
	kit_fmt_size((long long)e->bytes, size, sizeof(size));
	int days = bin_days();
	if (days > 0) {
		long long left = ((long long)e->when + (long long)days * 86400 - (long long)time(NULL) + 86399) / 86400;
		if (left <= 1) snprintf(under, sizeof(under), "%s", tr("Goes for good tomorrow"));
		else snprintf(under, sizeof(under), tr("Goes for good in %d days"), (int)left);
	} else snprintf(under, sizeof(under), "%s", tr("Till the bin's emptied"));
	int rw = Right(rx, y, size, under, C_DIM);
	int maxw = rx - rw - 24 - tx;
	Split(e->rel, parent, sizeof(parent), &name);
	font_draw(FONT_MEDIUM, 22, tx, y + 8, name, C_TEXT, maxw);
	time_t t = (time_t)e->when;
	struct tm *tm = localtime(&t);
	if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
	if (e->lost) snprintf(line, sizeof(line), tr("Where it was isn't known  \xc2\xb7  deleted %s"), when);
	else snprintf(line, sizeof(line), tr("Was in %s  \xc2\xb7  deleted %s"), parent[0] ? parent : DRV->root, when);
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, maxw);
}

static void Draw_Same_Row(int gi, int y, int as_file, int fi)
{
	int tx = LIST_X + 6 + THUMB_W + 16, rx = LIST_X + LIST_W - 12;
	const same_group_t *g = &same->g[gi];
	const char *pth = as_file ? same->path[g->first + fi] : Same_First(gi), *name;
	char size[32], under[64], line[400], parent[FSX_PATH];
	Tile(LIST_X + 6, y + (ROW_H - 4 - THUMB_H) / 2, 0, pth ? pth : "", 0xd04ad9ff);
	int left = Same_Left(gi);
	kit_fmt_size((long long)(as_file ? g->size : g->size * (uint64_t)(left - 1)), size, sizeof(size));
	if (as_file) under[0] = 0;
	else snprintf(under, sizeof(under), tr("%d copies"), left);
	int rw = Right(rx, y, size, under, as_file ? C_DIM : C_WARN);
	int maxw = rx - rw - 24 - tx;
	Split(pth ? pth : "", parent, sizeof(parent), &name);
	font_draw(FONT_MEDIUM, 22, tx, y + 8, name, C_TEXT, maxw);
	if (as_file) snprintf(line, sizeof(line), "%s/%s", DRV->root, parent);
	else {
		char each[32];
		kit_fmt_size((long long)g->size, each, sizeof(each));
		snprintf(line, sizeof(line), tr("%s each  \xc2\xb7  in %s"), each, parent[0] ? parent : DRV->root);
	}
	font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, maxw);
}

// a game's parts, over its screen
static void Draw_Parts(const game_t *gm)
{
	int w = screen_narrow() ? 780 : 860, h = 420, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 10;
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xb0000000);
	gfx_rect(0, HINT_Y - 14, SCREEN_W, SCREEN_H - HINT_Y + 14, 0xf205070d);
	gfx_round_rect(x - 1, y - 1, w + 2, h + 2, 19, C_BORDER);
	gfx_round_rect(x, y, w, h, 18, 0xf20a0e18);
	char title[200], a[32];
	snprintf(title, sizeof(title), tr("What %s is made of"), gm->title);
	font_draw(FONT_BOLD, 24, x + 32, y + 24, title, C_TEXT, w - 64);
	static const char *const labels[] = {N_("The game (with its updates)"), N_("Updates and game data"), N_("DLC and add-ons"),
		N_("Disc backups"), N_("Saves"), N_("Trophies"), N_("Licences")};
	uint64_t v[7] = {parts.game, parts.updates, parts.dlc, gm->bytes[PART_BACKUP], gm->bytes[PART_SAVE], gm->bytes[PART_TROPHY], parts.licences};
	uint64_t total = 0, top = 1;
	for (int i = 0; i < 7; i++) { total += v[i]; if (v[i] > top) top = v[i]; }
	for (int i = 0; i < 7; i++) {
		int ry = y + 74 + i * 40, from_folders = i < 3 || i == 6;
		uint32_t c = v[i] ? C_TEXT : C_DIM;
		font_draw(FONT_MEDIUM, 19, x + 32, ry, tr(labels[i]), c, 300);
		if (from_folders && !parts_done) snprintf(a, sizeof(a), "%s", "...");
		else if (v[i]) kit_fmt_size((long long)v[i], a, sizeof(a));
		else snprintf(a, sizeof(a), "-");
		font_draw_right(FONT_MEDIUM, 19, x + w - 32, ry, a, c);
		int bx = x + 340, bw = w - 340 - 140;
		gfx_round_rect(bx, ry + 6, bw, 10, 5, C_TRACK);
		if (v[i]) gfx_round_rect(bx, ry + 6, (int)(bw * (double)v[i] / top) > 10 ? (int)(bw * (double)v[i] / top) : 10, 10, 5, C_ACCENT);
	}
	kit_fmt_size((long long)total, a, sizeof(a));
	if (parts_done) snprintf(title, sizeof(title), tr("All of it: %s"), a);
	else snprintf(title, sizeof(title), tr("Adding up its folders... %s so far"), a);
	font_draw(FONT_MEDIUM, 19, x + 32, y + h - 50, title, parts_done ? C_CYAN : C_DIM, w - 64);
	const kit_hint_t hints[] = {{G_CIRCLE, -1, tr("Back"), 0}};
	kit_hints(hints, 1, LIST_X, ui_hints_max());
}

void list_render(void)
{
	view_t *v = Cur_View();
	const scan_t *s = DRV->shown;
	char crumb[160], r1[160], r2[200], a[32], b[32];
	kit_backdrop();
	if (!s && mode != LS_BIN) return;
	Clamp_Top(v);
	int narrow = screen_narrow();

	// the header: where we are, and what's in it
	r1[0] = r2[0] = 0;
	if (mode == LS_ITEMS) {
		snprintf(crumb, sizeof(crumb), "%s", tr(scan_cat_name(items_cat)));
		kit_fmt_size((long long)s->cat_bytes[items_cat], a, sizeof(a));
		kit_fmt_count(s->cat_items[items_cat], b, sizeof(b));
		snprintf(r1, sizeof(r1), tr(s->cat_items[items_cat] == 1 ? "%s  \xc2\xb7  %s item" : "%s  \xc2\xb7  %s items"), a, b);
	} else if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_GAME || mode == LS_FREE) {
		games_t *g = Games();
		if (mode == LS_GAMES) {
			snprintf(crumb, sizeof(crumb), "%s", tr("Games, all together"));
			if (g) snprintf(r1, sizeof(r1), tr("%d games"), g->ng);
		} else if (mode == LS_DUPES) {
			uint64_t could = 0;
			for (int r = 0; g && r < v->count; r++) could += Dup_Free(&g->g[v->rows[r]]);
			kit_fmt_size((long long)could, a, sizeof(a));
			snprintf(crumb, sizeof(crumb), "%s", tr("Duplicates"));
			snprintf(r1, sizeof(r1), tr("Up to %s could be freed"), a);
		} else if (mode == LS_GAME && g && game_sel >= 0 && game_sel < g->ng) {
			snprintf(crumb, sizeof(crumb), "%s", g->g[game_sel].title);
			kit_fmt_size((long long)g->g[game_sel].total, a, sizeof(a));
			snprintf(r1, sizeof(r1), "%s  \xc2\xb7  %s", g->g[game_sel].tid, a);
		} else {
			snprintf(crumb, sizeof(crumb), "%s", tr("Free up space"));
			uint64_t del;
			kit_fmt_size((long long)list_spare_bytes(&del), a, sizeof(a));
			snprintf(r1, sizeof(r1), tr("Up to %s"), a);
		}
	} else if (mode == LS_BIN) {
		uint64_t all = 0;
		for (int i = 0; i < nbin_rows; i++) all += bin_rows[i].bytes;
		kit_fmt_size((long long)all, a, sizeof(a));
		snprintf(crumb, sizeof(crumb), "%s", tr("Recycle bin"));
		snprintf(r1, sizeof(r1), tr("%s  \xc2\xb7  %d deleted"), a, nbin_rows);
	} else if (mode == LS_FILTER) {
		static const char *const ages[] = {N_("Any age"), N_("Untouched 6 months or more"), N_("Untouched a year or more"),
			N_("Untouched 2 years or more"), N_("Untouched 3 years or more")};
		snprintf(crumb, sizeof(crumb), "%s", tr("Size filter"));
		if (flt_sizes[flt_size]) { kit_fmt_size((long long)flt_sizes[flt_size], b, sizeof(b)); snprintf(a, sizeof(a), tr("Over %s"), b); }
		else snprintf(a, sizeof(a), "%s", tr("Any size"));
		snprintf(r1, sizeof(r1), "%s  \xc2\xb7  %s", a, tr(ages[flt_age]));
	} else if (mode == LS_RECENT) {
		static const char *const spans[] = {N_("In the last week"), N_("In the last month"), N_("In the last 3 months"), N_("In the last year")};
		uint64_t all = 0;
		for (int r = 0; r < v->count; r++) all += s->items[v->rows[r]].bytes;
		kit_fmt_size((long long)all, a, sizeof(a));
		snprintf(crumb, sizeof(crumb), "%s", tr("Recently added"));
		snprintf(r1, sizeof(r1), "%s  \xc2\xb7  %s", tr(spans[rec_span]), a);
	} else if (mode == LS_SAME || mode == LS_SAMEF) {
		snprintf(crumb, sizeof(crumb), "%s", tr("Identical files"));
		if (Same_Here()) {
			uint64_t w = 0;
			for (int i = 0; i < same->ng; i++) if (Same_Left(i) >= 2) w += same->g[i].size * (uint64_t)(Same_Left(i) - 1);
			kit_fmt_size((long long)w, a, sizeof(a));
			snprintf(r1, sizeof(r1), tr("Up to %s could be freed"), a);
		}
	} else if (mode == LS_FOLDERS) {
		snprintf(crumb, sizeof(crumb), "%s", tr("Largest folders"));
		snprintf(r1, sizeof(r1), tr("The %d largest, at any depth"), nfolders);
	} else if (mode == LS_OLD) {
		snprintf(crumb, sizeof(crumb), "%s", tr("Not played lately"));
		if (Games()) snprintf(r1, sizeof(r1), tr("%d games"), v->count);
	} else if (mode == LS_MARKS) {
		snprintf(crumb, sizeof(crumb), "%s", tr("Bookmarks"));
		snprintf(r1, sizeof(r1), tr("%d on %s"), nmark_rows, ui_drive_name(cur_drive));
	} else if (mode == LS_TYPES) {
		snprintf(crumb, sizeof(crumb), "%s", tr("File types"));
		snprintf(r1, sizeof(r1), tr("%d types"), ntypes);
	} else if (mode == LS_LARGEST) {
		snprintf(crumb, sizeof(crumb), "%s", lg_files ? tr("Largest files") : tr("Largest items"));
		if (lg_type >= 0 && lg_ext[0] && strcmp(lg_ext, "*")) snprintf(r1, sizeof(r1), tr("The largest .%s files"), lg_ext);
		else if (lg_type >= 0) snprintf(r1, sizeof(r1), "%s", tr(Type_Name(lg_ext) ? Type_Name(lg_ext) : "Everything else"));
		else if (lg_files) snprintf(r1, sizeof(r1), tr("The %d largest files on the drive"), BIG_MAX);
		else snprintf(r1, sizeof(r1), "%s", tr(lg_tab ? scan_cat_name(lg_tab - 1) : "Every category"));
	} else if (mode == LS_CHANGES) {
		snprintf(crumb, sizeof(crumb), "%s", tr("Changes since last scan"));
		const scan_changes_t *ch = DRV->changes;
		if (ch) {
			long long d = (long long)ch->now_total - (long long)ch->was_total;
			kit_fmt_size(d < 0 ? -d : d, a, sizeof(a));
			snprintf(r1, sizeof(r1), tr(d < 0 ? "\xe2\x88\x92%s in files since the scan before" : "+%s in files since the scan before"), a);
		}
	} else {
		snprintf(crumb, sizeof(crumb), "%s", f_title);
		if (fl && !fl->err) {
			kit_fmt_size((long long)f_total, a, sizeof(a));
			kit_fmt_count(fl->n, b, sizeof(b));
			snprintf(r1, sizeof(r1), tr(fl->n == 1 ? "%s  \xc2\xb7  %s entry" : "%s  \xc2\xb7  %s entries"), a, b);
		}
	}
	snprintf(r2, sizeof(r2), "%s", Sort_Label(v->sort));
	if (v->query[0]) {
		size_t l = strlen(r2);
		snprintf(r2 + l, sizeof(r2) - l, tr("  \xc2\xb7  search: \"%s\""), v->query);
	}
	int r1w = font_width(FONT_MEDIUM, 18, r1);
	kit_top(crumb, SCREEN_W - 2 * LIST_X - LOGO_W - 16 - 28 - r1w - 30);
	font_draw_right(FONT_MEDIUM, 18, SCREEN_W - LIST_X, 44, r1, C_TEXT);
	font_draw_right(FONT_REGULAR, 17, SCREEN_W - LIST_X, 104, r2, v->query[0] ? C_CYAN : C_DIM);
	int r2w = font_width(FONT_REGULAR, 17, r2), tab_max = SCREEN_W - LIST_X - r2w - 24;

	// tabs, or the folder's path
	if (mode == LS_ITEMS || mode == LS_LARGEST || mode == LS_TYPES) {
		const char *labels[NUM_SCANNED_CATS + 1];
		int n = 0;
		if (mode != LS_ITEMS) labels[n++] = tr("All");
		for (int k = 0; k < NUM_SCANNED_CATS; k++) labels[n++] = tr(scan_cat_short(k));
		kit_tabs(LIST_X, 96, tab_max, labels, n, mode == LS_ITEMS ? items_cat : mode == LS_TYPES ? ty_tab : lg_tab);
	} else if (mode == LS_BIN || mode == LS_FILTER || mode == LS_SAME || mode == LS_SAMEF || mode == LS_RECENT) {
		static const char *const says[] = {N_("What's deleted waits here: X puts it back, START deletes it for good, Square empties the bin."),
			N_("Everything over a size, untouched for a while: L1/R1 change the size, Square how long."),
			N_("Files the same, byte for byte, where you keep things (never installed games or saves): X shows them."),
			N_("The same file in each place: X opens its folder, START deletes one."),
			N_("What was added or changed lately, newest first: Square changes how far back.")};
		static const char *const says_o[] = {N_("What's deleted waits here: Circle puts it back, START deletes it for good, Square empties the bin."), NULL,
			N_("Files the same, byte for byte, where you keep things (never installed games or saves): Circle shows them."),
			N_("The same file in each place: Circle opens its folder, START deletes one."), NULL};		// (a console that confirms with Circle)
		char said[300];
		int k = mode == LS_BIN ? 0 : mode == LS_FILTER ? 1 : mode == LS_SAME ? 2 : mode == LS_SAMEF ? 3 : 4;
		snprintf(said, sizeof(said), "%s", says_o[k] ? kit_btn(says[k], says_o[k]) : tr(says[k]));
		if (mode == LS_SAME && Same_Here()) {		// (when it looked)
			char when[32] = "";
			time_t t = (time_t)same->when;
			struct tm *tm = localtime(&t);
			if (tm) strftime(when, sizeof(when), "%H:%M", tm);
			if (same->stopped) snprintf(said, sizeof(said), tr("%u files of 1 MB or more looked at, at %s, cut short (Square: again)"), (unsigned)same->files_seen, when);
			else snprintf(said, sizeof(said), tr("%u files of 1 MB or more looked at, at %s (Square: again)"), (unsigned)same->files_seen, when);
		}
		kit_draw_fit(FONT_REGULAR, 18, 14, LIST_X, 104, said, C_TEXT2, tab_max - LIST_X);
	} else if (mode == LS_GAMES || mode == LS_DUPES || mode == LS_FREE || mode == LS_GAME || mode == LS_FOLDERS || mode == LS_OLD || mode == LS_MARKS) {
		static const char *const says[] = {N_("Each game's installed data, backups, saves and trophies, added up."),
			N_("Games held more than once: installed and backed up, backed up twice, or also on another drive."),
			N_("Likely safe to clear: check each one. START deletes those that can go (Settings)."),
			N_("Everything of this game's on the drive: X opens one, START deletes one that can go."),
			N_("Where the space really is: a folder that's nearly all one folder is left for that one."),
			N_("Games here, the longest unplayed first: when their newest save (or file) was written."),
			N_("Square in a folder bookmarks it; here, Square removes one.")};
		int k = mode == LS_GAMES ? 0 : mode == LS_DUPES ? 1 : mode == LS_FREE ? 2 : mode == LS_GAME ? 3 : mode == LS_FOLDERS ? 4 : mode == LS_OLD ? 5 : 6;
		const char *say = k == 3 ? kit_btn(says[3], N_("Everything of this game's on the drive: Circle opens one, START deletes one that can go."))
			: tr(says[k]);
		kit_draw_fit(FONT_REGULAR, 18, 14, LIST_X, 104, say, C_TEXT2, tab_max - LIST_X);
	} else if (mode == LS_CHANGES) {
		if (DRV->changes) {
			char w1[48] = "", w2[48] = "", line[160];
			time_t t1 = (time_t)DRV->changes->was_when, t2 = (time_t)DRV->changes->now_when;
			struct tm *tm = localtime(&t1);
			if (tm) strftime(w1, sizeof(w1), "%Y-%m-%d %H:%M", tm);
			tm = localtime(&t2);
			if (tm) strftime(w2, sizeof(w2), "%Y-%m-%d %H:%M", tm);
			snprintf(line, sizeof(line), tr("The scan of %s against the one of %s; 1 MB or more"), w2, w1);
			kit_draw_fit(FONT_REGULAR, 18, 14, LIST_X, 104, line, C_TEXT2, tab_max - LIST_X);
		}
	} else {
		char abs[FSX_PATH];
		snprintf(abs, sizeof(abs), "%s%s%s", DRV->root, f_rel[0] ? "/" : "", f_rel);
		font_draw(FONT_REGULAR, 18, LIST_X, 104, abs, C_TEXT2, tab_max - LIST_X);
	}

	// the rows
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (mode == LS_FOLDER && !fl) Draw_Message(tr("Reading the folder..."));
	else if (mode == LS_FOLDER && fl->err && !fl->n) {
		char msg[128];
		snprintf(msg, sizeof(msg), tr("This folder couldn't be read (0x%08x)."), (unsigned)fl->err);
		Draw_Message(msg);
	} else if ((mode == LS_SAME || mode == LS_SAMEF) && same_running() && !v->count) {
		uint32_t li, ch, to;
		char msg[200];
		same_progress(&li, &ch, &to);
		if (to) snprintf(msg, sizeof(msg), tr("Comparing files of the same size: %u of %u..."), (unsigned)ch, (unsigned)to);
		else snprintf(msg, sizeof(msg), tr("Looking for identical files: %u of 1 MB or more so far..."), (unsigned)li);
		Draw_Message(msg);
	} else if (!v->count && mode == LS_BIN) {
		Draw_Message(tr("The recycle bin's empty."));
	} else if (!v->count && mode == LS_RECENT && !v->query[0]) {
		Draw_Message(tr("Nothing was added or changed in that time: Square goes further back."));
	} else if (!v->count && mode == LS_FILTER && !v->query[0]) {
		Draw_Message(tr("Nothing's that big and that old: L1/R1 or Square widen it."));
	} else if (!v->count && (mode == LS_SAME || mode == LS_SAMEF) && !v->query[0]) {
		Draw_Message(tr("No identical files found."));
	} else if (!v->count) {
		if (v->query[0]) Draw_Message(kit_btn(N_("Nothing matches the search. Press Circle to clear it."), N_("Nothing matches the search. Press X to clear it.")));
		else Draw_Message(mode == LS_FOLDER ? tr("This folder is empty.") : mode == LS_CHANGES ? tr("Nothing changed by 1 MB or more.")
			: mode == LS_DUPES ? tr("No game is here more than once.") : mode == LS_FREE ? tr("Nothing obvious to clear.")
			: mode == LS_MARKS ? tr("No bookmarks on this drive yet: Square in a folder adds one.") : tr("Nothing here."));
	}
	for (int r = 0; r < ROWS && v->top + r < v->count; r++) {
		int row = v->top + r, i = v->rows[row], y = LIST_Y + r * ROW_H;
		if (row == v->sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		if (mode == LS_FOLDER) Draw_Entry_Row(i, y);
		else if (mode == LS_BIN) Draw_Bin_Row(&bin_rows[i], y);
		else if (mode == LS_SAME) Draw_Same_Row(i, y, 0, 0);
		else if (mode == LS_SAMEF) Draw_Same_Row(same_group, y, 1, i);
		else if (mode == LS_FILTER) Draw_Item_Row(&s->items[i], y, 1);
		else if (mode == LS_RECENT) Draw_Item_Row(&s->items[i], y, 2);
		else if (mode == LS_CHANGES) Draw_Change_Row(Change_At(i), y);
		else if (mode == LS_GAMES || mode == LS_DUPES) Draw_Game_Row(&Games()->g[i], y, mode == LS_DUPES);
		else if (mode == LS_OLD) Draw_Old_Row(&Games()->g[i], y);
		else if (mode == LS_FOLDERS) { char p[FSX_PATH]; Dir_Path(s, folders[i], p, sizeof(p)); Draw_Folder_Row(p, (int)folders[i], y); }
		else if (mode == LS_MARKS) Draw_Folder_Row(marks[mark_rows[i]].rel, scan_dir_find(s, marks[mark_rows[i]].rel), y);
		else if (mode == LS_FREE) Draw_Spare_Row(&Games()->spare[i], y);
		else if (mode == LS_TYPES) Draw_Type_Row(&types[i], y);
		else if (mode == LS_GAME) Draw_Item_Row(&s->items[Row_Item(i)], y, 1);
		else if (mode == LS_LARGEST && lg_files) Draw_Big_Row(Lg_File(s, i), y);
		else Draw_Item_Row(&s->items[i], y, mode == LS_LARGEST);
	}
	if (v->count > ROWS) {
		int h = ROWS * ROW_H, bh = h * ROWS / v->count;
		if (bh < 24) bh = 24;
		int by = LIST_Y + (int)((long long)(h - bh) * v->sel / (v->count - 1));
		gfx_round_rect(LIST_X + LIST_W + 2, by, 4, bh, 2, 0x99ffffff);
	}
	if (mode == LS_FOLDER && fl && (fl->truncated || (fl->err && fl->n))) {
		font_draw_right(FONT_REGULAR, 16, SCREEN_W - LIST_X, LIST_Y + ROWS * ROW_H + 12,
			fl->truncated ? tr("Only the first 100,000 entries are listed.") : tr("Some of this folder couldn't be read."), C_WARN);
	}

	// the legend, and where in the list
	int can_open = 0;
	if (v->count) {
		int i = v->rows[v->sel];
		int item = Row_Item(i);
		can_open = mode == LS_FOLDER ? fl->e[i].type == FSX_DIR : mode == LS_CHANGES ? Change_At(i)->now_there
			: mode == LS_BIN ? !bin_rows[i].lost : item >= 0 ? s->items[item].kind != ITEM_REST : 1;
	}
	const kit_hint_t hints[] = {
		{G_CROSS, -1, can_open ? (mode == LS_BIN ? tr("Put back") : tr("Open")) : NULL, 0},
		{G_L1, G_R1, mode == LS_FILTER ? tr("Size") : NULL, 1},
		{G_SQUARE, -1, mode == LS_BIN ? (v->count ? tr("Empty the bin") : NULL) : mode == LS_FILTER ? tr("How long") : mode == LS_RECENT ? tr("How far back")
			: mode == LS_SAME ? (same_running() ? NULL : tr("Look again")) : mode == LS_GAME ? tr("What it's made of")
			: mode == LS_LARGEST && lg_type < 0 ? (lg_files ? tr("Items") : tr("Files")) : mode == LS_ITEMS ? (narrow ? tr("Rescan") : tr("Rescan this"))
			: mode == LS_FOLDER ? (Mark_Find(DRV->root, f_rel) >= 0 ? tr("Unbookmark") : tr("Bookmark")) : mode == LS_MARKS && v->count ? tr("Remove")
			: (mode == LS_FREE || mode == LS_DUPES) && v->count && Games() ? (Row_Kept(v->rows[v->sel]) ? tr("Unkeep") : tr("Keep")) : NULL,
			mode == LS_ITEMS || mode == LS_FOLDER || mode == LS_FREE || mode == LS_DUPES ? 2 : 0},
		{G_TRIANGLE, -1, tr("Search"), 1},
		{G_SELECT, -1, Sort_Label((v->sort + 1) % NUM_SORTS), 2},
		{G_START, -1, Row_Deletable() ? (mode == LS_BIN ? tr("Delete for good") : tr("Delete")) : NULL, 1},
		{G_CIRCLE, -1, v->query[0] ? tr("Clear search") : mode == LS_FOLDER && (int)strlen(f_rel) > f_base ? tr("Up") : tr("Back"), 0},
	};
	int pos_w = 0;
	char pos[48];
	snprintf(pos, sizeof(pos), "%d / %d", v->sel + 1, v->count);
	if (!ui_scanning() && v->count && !narrow) pos_w = font_width(FONT_MEDIUM, 19, pos) + 24;
	kit_hints(hints, sizeof(hints) / sizeof(hints[0]), LIST_X, ui_hints_max() - pos_w);
	if (ui_scanning()) ui_draw_strip();
	else if (pos_w) font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, SCREEN_H - 55, pos, C_DIM);
	if (parts_open && mode == LS_GAME && Games() && game_sel >= 0 && game_sel < Games()->ng) Draw_Parts(&Games()->g[game_sel]);
}
