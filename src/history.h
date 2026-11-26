/*
history.h: each drive's scans over time, a line each in USRDIR/history_<drive>.txt
(the last HISTORY_MAX kept): when, the drive's size, what was used and free, and
each category's bytes. For the Space over time chart.
*/
#ifndef HISTORY_H
#define HISTORY_H

#include <stdint.h>
#include "scan.h"

#define HISTORY_MAX	200

typedef struct {
	int64_t when;
	uint64_t total, used, free_bytes;
	uint64_t cat[NUM_CATS];
	int partial;			// a category's rescan (that category + 1), 0 the whole drive
} hist_t;

void history_add(const scan_t *s);
int history_load(const char *root, hist_t *out, int max);	// oldest first; how many
int history_count(const char *root);
// s's drive's scans (a USB port's history can hold another stick's too: those of another size are
// left out); oldest first; how many
int history_load_of(const scan_t *s, hist_t *out, int max);

#endif
