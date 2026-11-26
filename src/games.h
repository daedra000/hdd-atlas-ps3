/*
games.h: a scan's items by game (title ID), and what that shows: each game's
parts added up (installed, backups, saves, trophies), the games held more than
once, and what's likely safe to clear. Made from a scan when a screen wants it.
*/
#ifndef GAMES_H
#define GAMES_H

#include <stdint.h>
#include "scan.h"

enum { PART_INSTALLED, PART_BACKUP, PART_SAVE, PART_TROPHY, NUM_PARTS };

typedef struct {
	char tid[12];				// "BLUS30443"
	char title[ITEM_TITLE];
	uint64_t bytes[NUM_PARTS], total;
	uint32_t count[NUM_PARTS];
	uint32_t first, n;			// its items: members[first .. first + n)
	int icon_item;				// an item to take the icon from (-1: none)
	int copies;					// whole copies of it: disc images and folder backups, installed HDD games (HG)
	uint64_t copies_bytes;		// all of those (a split image's parts, a disc's tracks, counted as one)
	uint64_t could_free;		// those less the largest of them (duplicates)
	int32_t keep_item;			// an item of the copy that'd be kept (the largest); -1 none
	uint64_t data_bytes;		// of bytes[PART_INSTALLED], what's game data and updates (not HG): not a copy
	uint32_t data_count;
	int64_t newest;				// the last time anything of it changed (0 unknown)
	int64_t played;				// its saves' newest (the last time it was played, near enough), else newest
} game_t;

enum { WHY_PKG, WHY_CACHE, WHY_TMP /* (no longer given: 0.9.3) */, WHY_EXTRA_COPY, WHY_LONE_UPDATE, WHY_LONE_SAVE, WHY_LEFTOVER, WHY_NO_SFO, WHY_DOWNLOAD, WHY_QUEUED, WHY_PKG_NEW, WHY_TOOL_CACHE, WHY_FIRMWARE, NUM_WHYS };
// (LONE_UPDATE, LONE_SAVE and NO_SFO: to check before clearing)

typedef struct {
	uint32_t item;				// in the scan
	uint8_t why;				// WHY_*
} spare_t;

typedef struct {
	game_t *g;
	int ng;
	uint32_t *members;			// item indices, each game's together
	int32_t *game_of;			// [the scan's items]: each one's game (-1 none)
	spare_t *spare;				// likely safe to clear, largest first
	int nspare;
	uint64_t spare_bytes;
} games_t;

games_t *games_build(const scan_t *s);		// NULL: no memory
void games_free(games_t *g);
int games_part_of(const scan_item_t *it);	// PART_*, or -1 if it isn't any game's
// it's in a game's group: that game (-1 none)
int games_find(const games_t *g, const scan_t *s, uint32_t item);

#endif
