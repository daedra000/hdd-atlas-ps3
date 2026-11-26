/*
finds.h: two looks through a drive that the scan doesn't keep enough for, on
workers, read-only.

  Identical files   files of 1 MB or more that are the same, byte for byte as
                    far as it checks: the same size, and the same content (all
                    of it up to 16 MB; past that, 16 places spread through it).
                    Only where you keep things (backups, media, packages, your
                    own folders): never installed games, saves or the system's.
  A game's parts    its installed folders added up as the game, updates and
                    game data, and DLC (.edat/.sdat); its licences in exdata.
*/
#ifndef FINDS_H
#define FINDS_H

#include <stdint.h>

#define SAME_MIN	(1024 * 1024)	// files smaller than this aren't looked at

typedef struct { uint64_t size; uint32_t first, n; } same_group_t;	// paths [first, first + n)

typedef struct {
	char root[64];
	int64_t when;					// looked at (time())
	char **path;					// under the drive, each group's together
	uint32_t npath;
	same_group_t *g;				// the most wasted first
	int ng;
	uint32_t files_seen;			// files of SAME_MIN or more looked at
	int stopped;					// cut short (no memory, or stopped)
} same_t;

int same_start(const char *root);		// 0 if one's going already
int same_running(void);
void same_progress(uint32_t *listed, uint32_t *checked, uint32_t *to_check);
same_t *same_take(void);				// the finished look (yours to free), or NULL
void same_free(same_t *s);
void same_shutdown(void);

typedef struct { uint64_t game, updates, dlc, licences; } parts_t;
// the game's installed folders (under the drive; hg: an HDD game's own, else its data) and its ID
int parts_start(const char *root, const char *tid, const char *const *dirs, const int *hg, int n);
int parts_poll(parts_t *out);			// 1 once it's done (out filled), 0 meanwhile, -1 none going
void parts_shutdown(void);

#endif
