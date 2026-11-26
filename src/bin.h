/*
bin.h: the recycle bin. With Settings' Recycle bin on (7 days by default), a
delete moves the row into the bin on its own drive (a rename: instant, and
nothing's freed yet) instead of deleting it; the menu's Recycle bin puts it
back, deletes it for good, or empties the lot, and at start (or when a USB
drive's plugged in) entries older than the setting go for good.

  <drive>/<bin_rel>/<id>/<name>    an entry: what was deleted, its name kept
  <drive>/<bin_rel>/bin.txt        a line an entry: what it was and where

bin_rel is HDD Atlas's own USRDIR/bin on the HDD (game/HDAT00001/USRDIR/bin)
and HDDAtlas/bin on a USB drive: the same drive as what's in it, always.
The moves themselves are del.c's (del_bin_move, del_restore), behind the
same allow-list as deleting. bin.txt is only added to (bin.c says how), and
an entry's line is written before it's moved in: a crash at any point leaves
nothing that bin_tidy can't put right.
*/
#ifndef BIN_H
#define BIN_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "fsx.h"
#include "plat.h"

typedef struct {
	char id[32];					// its folder in the bin ("1794251143_3")
	char rel[FSX_PATH];				// where it was, under the drive
	char twin[FSX_PATH];			// and its twin with it ("" none)
	int64_t when;					// deleted at (time())
	uint64_t bytes;
	uint32_t files;
	int is_dir;
	int lost;						// its line was lost (a power cut part way): rel is only its name; it can't be put back
} bin_entry_t;

// the bin, under the drive; 0 if the path's too long (here: the scan needs it, without the rest)
static inline int bin_rel(const char *root, char *out, int size)
{
	const char *save = plat_save_dir();
	size_t n = strlen(root);
	out[0] = 0;
	// the HDD's: HDD Atlas's own USRDIR, when that's on it (always, on a PS3)
	if (!strncmp(save, root, n) && save[n] == '/' && save[n + 1]) return snprintf(out, size, "%s/bin", save + n + 1) < size;
	return snprintf(out, size, "HDDAtlas/bin") < size;
}
void bin_init(void);				// at start, before any worker uses it
int bin_days(void);				// Settings: days kept (> 0), -1 till emptied, 0 off (delete at once)
// a new entry's id and folder (under the drive: <bin_rel>/<id>); 0 if the path's too long
int bin_new_entry(const char *root, char *id, int idsize, char *entry_rel, int relsize);
int bin_entry_rel(const char *root, const char *id, char *out, int size);
int bin_load(const char *root, bin_entry_t **out);		// the entries, oldest first (malloc'd; free them); the count
int bin_add(const char *root, const bin_entry_t *e);
int bin_remove(const char *root, const char *id);		// its line gone from bin.txt
uint64_t bin_bytes(const char *root);					// all of it
// the list and the folders put back in step after a crash part way (a worker's: it walks folders);
// how many were changed
int bin_tidy(const char *root);

#endif
