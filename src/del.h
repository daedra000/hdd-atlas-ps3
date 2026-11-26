/*
del.h: deleting, one row at a time, only what the allow-list lets through
(as agreed for 0.4.0, 9 Nov 2026).

A row may go only if its path under a drive has exactly one of these shapes:
  PS3ISO|PS2ISO|PSXISO|PSPISO|BDISO|DVDISO/<file>   an ISO (and its .cue/.bin twin)
  GAMES|GAMEZ/<folder>                              a folder backup, all of it
  GAMES|GAMEZ/<folder>/PS3_UPDATE                   the system update on a backed-up disc
  updater/<NN>/<file>.pup                           a system update waiting (the file: HEN makes updater/01 again)
  packages/<file>.pkg | <file>.pkg                  a leftover package (in packages/, or at the top)
  ROMS/<system>/<file>                              a ROM
  tmp/wmtmp/<entry>                                 webMAN's cache
  game/_INST_<x> | game/_DEL_<x>                    an install or a delete the system didn't finish
  <the recycle bin>/<entry>                         an entry of HDD Atlas's own recycle bin (bin.h)
on /dev_hdd0 or /dev_usb000-007. Everything else is refused, and some with a
reason to show: installed games, saves, music, video and photos belong to
the XMB's database (delete them there), system folders are never touched.

del_run checks again, on the worker, before every single unlink and rmdir,
that the path is the row's or inside it, built from names that can't climb
out (no "/", ".", ".."). Links and anything that isn't a file or a folder
are never followed or removed: they're left, and said so.
*/
#ifndef DEL_H
#define DEL_H

#include <stdint.h>
#include "fsx.h"

enum { DEL_NO, DEL_ISO, DEL_BACKUP, DEL_PKG, DEL_ROM, DEL_CACHE, DEL_LEFTOVER, DEL_BIN, DEL_FIRMWARE };

// what rel (under root: "PS3ISO/Game.iso") is, if it may be deleted; DEL_NO otherwise, with why
// ("" when there's nothing worth saying, as for a file deep in a game's folder)
int del_kind(const char *root, const char *rel, int is_dir, char *why, int why_size);
const char *del_kind_name(int kind);
// an ISO's twin, deleted with it: Game.bin's Game.cue and the other way round (rel under root);
// 1 if there's one
int del_twin(const char *root, const char *rel, char *out, int size);

typedef struct {
	char root[64];
	char rel[FSX_PATH];				// the row
	char twin[FSX_PATH];			// and its twin ("" none)
	int is_dir;
	uint32_t files_total;			// from the scan, for the progress bar (0: not known)
	uint64_t bytes_total;			// and its size (a move to the recycle bin moves this much)
	volatile int cancel;			// stop between files
	volatile uint32_t files_done, dirs_done;
	volatile uint64_t bytes_done;
	uint32_t left;					// entries left behind (links and the like, or what failed)
	int result;						// 0 all gone; 1 stopped part way; < 0 the system's error (or DEL_NOT_SAME, DEL_NO_OTHER)
	char err_path[256];				// where it went wrong
	char same_as[FSX_PATH];			// Identical files: another copy (under the drive) it must be the same as, byte for byte, first ("" none)
	volatile int checking;			// comparing it with same_as now (bytes_done: how far)
} del_job_t;

#define DEL_NOT_SAME	(-1001)		// not deleted: it isn't the same as same_as after all
#define DEL_NO_OTHER	(-1002)		// not deleted: same_as isn't there any more (it'd be the last copy)
#define DEL_BUSY		(-1003)		// not deleted: still changing (a leftover install an install is still writing)
#define DEL_RECENT		(-1004)		// not deleted: a leftover install written to in the last day (one going on may need it)

// d->rel against d->same_as, every byte (on the worker: a big file takes a while; stops on d->cancel);
// 0 when they're the same, else d->result set (DEL_NOT_SAME, DEL_NO_OTHER, 1 stopped, or an error)
int del_same_check(del_job_t *d);

// a leftover install (game/_INST_<x>) still being written? Its files counted, sized and timed twice,
// wait_us apart (on the worker; stops on d->cancel), and an _INST_ one not written to in the last day:
// 0 when it's settled, else d->result set (DEL_BUSY, DEL_RECENT, 1 stopped, or an error)
int del_settled(del_job_t *d, unsigned wait_us);
// a package being installed now (PS3HEN's tmp/installer.active there): no leftover install deleted
int del_installing(const char *root);

// deletes it (on a worker: it can take a while); sets d->result and returns it
int del_run(del_job_t *d);

// the recycle bin's (bin.h): the row (and its twin) moved into the entry <entry_rel> (a folder
// that mustn't be there yet; it and the bin are made); sets d->result and returns it
int del_bin_move(del_job_t *d, const char *entry_rel);
// rel (is_dir) put back from the entry; 0 when it's back (-17: something's there now)
int del_restore(const char *root, const char *entry_rel, const char *rel, int is_dir);
int del_bin_drop(const char *root, const char *entry_rel);		// the entry's folder, empty, removed

// the platform's calls (src/ps3/del_ps3.c; tools/host/del_host.c). 0 when it's done
int fsx_unlink(const char *path);
int fsx_rmdir(const char *path);
int fsx_rename(const char *from, const char *to);		// (the recycle bin's: on one drive)
int fsx_mkdir(const char *path);

#endif
