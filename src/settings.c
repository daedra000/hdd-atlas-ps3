/*
settings.c: USRDIR/settings.txt (settings.h). Read whole, changed, written whole
(plat_write_file writes beside it and renames: a half-written file can't happen).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "settings.h"

#define MAX_KEYS	16

static struct { char key[24]; int value; } keys[MAX_KEYS];
static int nkeys, loaded;

static void Path(char *out, int size) { snprintf(out, size, "%s/settings.txt", plat_save_dir()); }

static void Load(void)
{
	char path[256];
	size_t len;
	loaded = 1;
	Path(path, sizeof(path));
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return;
	for (char *line = strtok(b, "\r\n"); line && nkeys < MAX_KEYS; line = strtok(NULL, "\r\n")) {
		char k[24];
		int v;
		if (sscanf(line, "%23[^=]=%d", k, &v) == 2) {
			snprintf(keys[nkeys].key, sizeof(keys[0].key), "%s", k);
			keys[nkeys++].value = v;
		}
	}
	free(b);
}

int setting_get(const char *key, int def)
{
	if (!loaded) Load();
	for (int i = 0; i < nkeys; i++) if (!strcmp(keys[i].key, key)) return keys[i].value;
	return def;
}

void setting_set(const char *key, int value)
{
	char path[256], buf[MAX_KEYS * 40];
	int i, n = 0;
	if (!loaded) Load();
	for (i = 0; i < nkeys && strcmp(keys[i].key, key); i++);
	if (i == nkeys) {
		if (nkeys == MAX_KEYS) return;
		snprintf(keys[nkeys++].key, sizeof(keys[0].key), "%s", key);
	}
	keys[i].value = value;
	for (int k = 0; k < nkeys; k++) n += snprintf(buf + n, sizeof(buf) - n, "%s=%d\n", keys[k].key, keys[k].value);
	Path(path, sizeof(path));
	if (!plat_write_file(path, buf, n)) plat_log("settings: couldn't write %s", path);
}
