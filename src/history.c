/*
history.c: history.h. A line a scan: "1 when total used free partial c0 .. c8";
once there are twice HISTORY_MAX lines, the file's written again with the last
HISTORY_MAX.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "plat.h"
#include "history.h"
#include "fsx.h"

static void Path(const char *root, char *out, int size)
{
	char key[32];
	fsx_drive_key(root, key, sizeof(key));
	snprintf(out, size, "%s/history_%s.txt", plat_save_dir(), key);
}

static int Parse(const char *line, hist_t *h)
{
	unsigned long long v[6 + NUM_CATS];
	int n = 0, ver;
	const char *p = line;
	if (sscanf(p, "%d", &ver) != 1 || ver != 1) return 0;
	while (*p && *p != ' ') p++;
	for (; n < 6 + NUM_CATS; n++) {
		char *end;
		while (*p == ' ') p++;
		if (!*p) break;
		v[n] = strtoull(p, &end, 10);
		if (end == p) break;
		p = end;
	}
	if (n != 6 + NUM_CATS - 1 && n != 5 + NUM_CATS) return 0;
	memset(h, 0, sizeof(*h));
	h->when = (int64_t)v[0];
	h->total = v[1];
	h->used = v[2];
	h->free_bytes = v[3];
	h->partial = (int)v[4];
	for (int k = 0; k < NUM_CATS && 5 + k < n; k++) h->cat[k] = v[5 + k];
	return h->when > 0;
}

int history_load(const char *root, hist_t *out, int max)
{
	char path[256];
	size_t len;
	Path(root, path, sizeof(path));
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return 0;
	// the last max lines
	int lines = 0;
	for (char *c = b; *c; c++) lines += *c == '\n';
	int skip = lines > max ? lines - max : 0, n = 0;
	for (char *line = strtok(b, "\n"); line && n < max; line = strtok(NULL, "\n")) {
		if (skip > 0) { skip--; continue; }
		if (Parse(line, &out[n])) n++;
	}
	free(b);
	return n;
}

int history_load_of(const scan_t *s, hist_t *out, int max)
{
	int n = history_load(s->root, out, max);
	// (a USB drive's partition size names it, FAT32 or NTFS; the HDD's port only ever holds the HDD)
	if (!strcmp(s->root, "/dev_hdd0") || !s->total) return n;
	uint64_t total = scan_drive_total(s);
	int k = 0;
	for (int i = 0; i < n; i++) if (out[i].total == total) out[k++] = out[i];
	return k;
}

int history_count(const char *root)
{
	static hist_t tmp[HISTORY_MAX];		// (main thread only)
	return history_load(root, tmp, HISTORY_MAX);
}

void history_add(const scan_t *s)
{
	char path[256], line[512];
	int n = snprintf(line, sizeof(line), "1 %" PRId64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %d", s->when, scan_drive_total(s),
		scan_drive_used(s), s->free_bytes, (int)s->partial);
	for (int k = 0; k < NUM_CATS && n < (int)sizeof(line); k++) n += snprintf(line + n, sizeof(line) - n, " %" PRIu64, s->cat_bytes[k]);
	Path(s->root, path, sizeof(path));
	plat_append_line(path, line);
	// too long: the last HISTORY_MAX again
	size_t len;
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return;
	int lines = 0;
	for (char *c = b; *c; c++) lines += *c == '\n';
	if (lines > 2 * HISTORY_MAX) {
		char *c = b;
		for (int skip = lines - HISTORY_MAX; skip > 0 && *c; c++) if (*c == '\n') skip--;
		plat_write_file(path, c, strlen(c));
	}
	free(b);
}
