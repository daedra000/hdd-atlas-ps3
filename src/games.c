/*
games.c: games.h. Items are matched by their title ID's first 9 characters:
installed games and their data (game/<ID>), backups (GAMES/<x>, and disc images
by the ID inside them), saves (savedata/<ID...>); a trophy set by its name, when
it's exactly a game's. An item with no ID isn't any game's.

"Spare" is what's likely safe to clear, with why: leftover packages, webMAN's
cache, tmp, a backup also held another way, an update with neither its game
nor a backup of it here (it may be for a disc), a save for a game that isn't
here (the same).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include "iso.h"
#include "games.h"

static int Is_Tid(const char *s)
{
	for (int i = 0; i < 4; i++) if (!isupper((unsigned char)s[i])) return 0;
	for (int i = 4; i < 9; i++) if (!isdigit((unsigned char)s[i])) return 0;
	return 1;
}

int games_part_of(const scan_item_t *it)
{
	if (it->kind == ITEM_REST) return -1;
	if (it->cat == CAT_GAMES) return PART_INSTALLED;
	if (it->cat == CAT_BACKUPS) return PART_BACKUP;
	if (it->cat == CAT_MEDIA && it->kind == ITEM_FILE && !strncmp(it->path, "video/", 6) && it->tid[0]) return PART_BACKUP;		// (a game's ISO in webMAN's "stealth" video/)
	if (it->cat == CAT_SAVES && (strstr(it->path, "/savedata/") || strstr(it->path, "/minis_savedata/") || strstr(it->path, "/ps2emu2_savedata/")))
		return PART_SAVE;		// (minis_savedata: PSP Minis' and Remasters'; ps2emu2_savedata: PS2 Classics')
	if (it->cat == CAT_SAVES && strstr(it->path, "/trophy/")) return PART_TROPHY;
	return -1;
}

// the item's title ID (9 characters), from what the scan read or its folder's name; 0 if none
static int Key_Of(const scan_item_t *it, char *out)
{
	const char *base = strrchr(it->path, '/');
	base = base ? base + 1 : it->path;
	if (strlen(it->tid) >= 9 && Is_Tid(it->tid)) { snprintf(out, 10, "%.9s", it->tid); return 1; }
	if (strlen(base) >= 9 && Is_Tid(base)) { snprintf(out, 10, "%.9s", base); return 1; }
	return iso_tid_in(base, out, 10);
}

// a save's title without its " · subtitle"
static void Plain_Title(const char *t, char *out, int size)
{
	snprintf(out, size, "%s", t);
	char *dot = strstr(out, " \xc2\xb7 ");
	if (dot) *dot = 0;
}

typedef struct { char key[10]; uint32_t item; } keyed_t;
static int Keyed_Cmp(const void *a, const void *b)
{
	int c = strcmp(((const keyed_t *)a)->key, ((const keyed_t *)b)->key);
	return c ? c : (((const keyed_t *)a)->item > ((const keyed_t *)b)->item) - (((const keyed_t *)a)->item < ((const keyed_t *)b)->item);
}

static int Game_Cmp(const void *a, const void *b)
{
	uint64_t x = ((const game_t *)a)->total, y = ((const game_t *)b)->total;
	return x < y ? 1 : x > y ? -1 : 0;
}
static const scan_t *sort_s;
static int Spare_Cmp(const void *a, const void *b)
{
	uint64_t x = sort_s->items[((const spare_t *)a)->item].bytes, y = sort_s->items[((const spare_t *)b)->item].bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

/*
A whole copy of a game: a disc image or a folder backup, or an installed HDD
game (HG). What sits beside an image with the game's ID in its name (webMAN's
cover .png/.jpg, a .dkey, a .cue) is part of the backup, not another copy, and
a split image's parts (.iso.0, .iso.1) or a disc's tracks ("(Track 2).bin")
are one copy between them: its stem names it. 0: not a copy
*/
// a name of exactly 8 hex digits (a PDB folder's: vsh/task/<id>, vsh/game_pkg/<id>)
static int Hex8(const char *n)
{
	for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)n[i])) return 0;
	return !n[8];
}

static int Ends_Ci(const char *s, size_t n, const char *ext)
{
	size_t e = strlen(ext);
	return n > e && !strncasecmp(s + n - e, ext, e);
}

static int Copy_Stem(const scan_item_t *it, char *out, int size)
{
	int part = games_part_of(it);
	if (part == PART_INSTALLED) {
		if (strcmp(it->code, "HG")) return 0;
		snprintf(out, size, "%s", it->path);
		return 1;
	}
	if (part != PART_BACKUP) return 0;
	if (it->kind == ITEM_DIR) { snprintf(out, size, "%s", it->path); return 1; }
	if (it->kind != ITEM_FILE) return 0;
	static const char *const images[] = {".iso", ".bin", ".img", ".mdf", ".nrg", ".cso", ".zso", ".pbp", ".chd"};
	size_t n = strlen(it->path), m = n;
	while (m > 0 && isdigit((unsigned char)it->path[m - 1])) m--;		// (a split part: name.iso.3)
	if (m < n && m > 1 && it->path[m - 1] == '.') m--;
	else m = n;
	int image = 0;
	for (size_t k = 0; k < sizeof(images) / sizeof(images[0]); k++) if (Ends_Ci(it->path, m, images[k])) image = 1;
	if (!image) return 0;
	const char *track = strstr(it->path, " (Track ");
	if (track && (size_t)(track - it->path) < m) m = (size_t)(track - it->path);
	snprintf(out, size, "%.*s", (int)m, it->path);
	return 1;
}

games_t *games_build(const scan_t *s)
{
	uint32_t ni = s->num_items;
	games_t *g = calloc(1, sizeof(*g));
	keyed_t *k = malloc((ni ? ni : 1) * sizeof(*k));
	int32_t *trophy_of = malloc((ni ? ni : 1) * sizeof(int32_t));
	if (!g || !k || !trophy_of) { free(k); free(trophy_of); free(g); return NULL; }
	g->game_of = malloc((ni ? ni : 1) * sizeof(int32_t));
	g->g = calloc(ni ? ni : 1, sizeof(game_t));
	g->members = malloc((ni ? ni : 1) * sizeof(uint32_t));
	g->spare = malloc((ni ? ni : 1) * sizeof(spare_t));
	if (!g->game_of || !g->g || !g->members || !g->spare) { free(k); free(trophy_of); games_free(g); return NULL; }
	for (uint32_t i = 0; i < ni; i++) g->game_of[i] = trophy_of[i] = -1;
	// the items with an ID, grouped by it
	int nk = 0;
	for (uint32_t i = 0; i < ni; i++) {
		const scan_item_t *it = &s->items[i];
		int part = games_part_of(it);
		if (part < 0 || part == PART_TROPHY) continue;
		if (Key_Of(it, k[nk].key)) k[nk++].item = i;
	}
	qsort(k, nk, sizeof(*k), Keyed_Cmp);
	for (int i = 0; i < nk; ) {
		game_t *gm = &g->g[g->ng];
		snprintf(gm->tid, sizeof(gm->tid), "%s", k[i].key);
		gm->icon_item = -1;
		int j = i, title_rank = 9;		// (the title from: an installed HDD game, a backup, installed data, a save)
		enum { MAX_COPIES = 32 };
		char stems[MAX_COPIES][ITEM_PATH];
		uint64_t stem_bytes[MAX_COPIES];
		uint32_t stem_item[MAX_COPIES];
		int ns = 0;
		gm->keep_item = -1;
		for (; j < nk && !strcmp(k[j].key, k[i].key); j++) {
			const scan_item_t *it = &s->items[k[j].item];
			int part = games_part_of(it), rank = part == PART_INSTALLED ? (!strcmp(it->code, "HG") ? 0 : 2) : part == PART_BACKUP ? 1 : 3;
			g->game_of[k[j].item] = g->ng;
			gm->bytes[part] += it->bytes;
			gm->count[part]++;
			if (it->title[0] && rank < title_rank) { Plain_Title(it->title, gm->title, sizeof(gm->title)); title_rank = rank; }
			if (it->icon != ICON_NONE && gm->icon_item < 0) gm->icon_item = (int)k[j].item;
			if (it->newest > gm->newest) gm->newest = it->newest;
			if (part == PART_SAVE && it->newest > gm->played) gm->played = it->newest;
			if (part == PART_INSTALLED && strcmp(it->code, "HG")) { gm->data_bytes += it->bytes; gm->data_count++; }
			char stem[ITEM_PATH];
			if (Copy_Stem(it, stem, sizeof(stem))) {
				int c = 0;
				while (c < ns && strcmp(stems[c], stem)) c++;
				if (c == ns && ns < MAX_COPIES) { snprintf(stems[ns], ITEM_PATH, "%s", stem); stem_bytes[ns] = 0; stem_item[ns] = k[j].item; ns++; }
				if (c < ns) stem_bytes[c] += it->bytes;
				gm->copies_bytes += it->bytes;
			}
		}
		uint64_t biggest = 0;
		for (int c = 0; c < ns; c++)
			if (gm->keep_item < 0 || stem_bytes[c] > biggest) { biggest = stem_bytes[c]; gm->keep_item = (int32_t)stem_item[c]; }
		gm->copies = ns;
		gm->could_free = ns >= 2 ? gm->copies_bytes - biggest : 0;
		if (!gm->played) gm->played = gm->newest;
		if (!gm->title[0]) snprintf(gm->title, sizeof(gm->title), "%s", gm->tid);
		g->ng++;
		i = j;
	}
	free(k);
	// trophy sets: the game whose title is exactly theirs
	for (uint32_t i = 0; i < ni; i++) {
		const scan_item_t *it = &s->items[i];
		if (games_part_of(it) != PART_TROPHY || !it->title[0]) continue;
		for (int x = 0; x < g->ng; x++)
			if (!strcasecmp(g->g[x].title, it->title)) {
				g->g[x].bytes[PART_TROPHY] += it->bytes;
				g->g[x].count[PART_TROPHY]++;
				g->game_of[i] = trophy_of[i] = x;
				break;
			}
	}
	free(trophy_of);
	for (int x = 0; x < g->ng; x++) {
		game_t *gm = &g->g[x];
		gm->total = gm->bytes[PART_INSTALLED] + gm->bytes[PART_BACKUP] + gm->bytes[PART_SAVE] + gm->bytes[PART_TROPHY];
	}
	// the games largest first, and each one's items together, in members (game_of follows the sort)
	int32_t *order = malloc((g->ng ? g->ng : 1) * sizeof(int32_t));
	game_t *sorted = malloc((g->ng ? g->ng : 1) * sizeof(game_t));
	if (!order || !sorted) { free(order); free(sorted); games_free(g); return NULL; }
	memcpy(sorted, g->g, g->ng * sizeof(game_t));
	for (int x = 0; x < g->ng; x++) sorted[x].first = (uint32_t)x;		// (its old index, for now)
	qsort(sorted, g->ng, sizeof(game_t), Game_Cmp);
	for (int x = 0; x < g->ng; x++) order[sorted[x].first] = x;
	for (uint32_t i = 0; i < ni; i++) if (g->game_of[i] >= 0) g->game_of[i] = order[g->game_of[i]];
	// each game's items together in members: counted, then placed (one pass each)
	for (int x = 0; x < g->ng; x++) sorted[x].n = 0;
	for (uint32_t i = 0; i < ni; i++) if (g->game_of[i] >= 0) sorted[g->game_of[i]].n++;
	uint32_t at = 0;
	for (int x = 0; x < g->ng; x++) { sorted[x].first = at; at += sorted[x].n; sorted[x].n = 0; }
	for (uint32_t i = 0; i < ni; i++) {
		int x = g->game_of[i];
		if (x >= 0) g->members[sorted[x].first + sorted[x].n++] = i;
	}
	free(g->g);
	g->g = sorted;
	free(order);
	// spare: what's likely safe to clear
	for (uint32_t i = 0; i < ni; i++) {
		const scan_item_t *it = &s->items[i];
		int why = -1, x = g->game_of[i], part = games_part_of(it);
		if (it->kind == ITEM_REST || !it->bytes) continue;
		const char *in_game = it->cat == CAT_GAMES && it->kind == ITEM_DIR && !strncmp(it->path, "game/", 5) && !strchr(it->path + 5, '/') ? it->path + 5 : NULL;
		// (folders split off their owners by the scan: apps' caches, system updates)
		if (!strcmp(it->code, SCAN_CACHE_CODE) && it->bytes >= (1 << 20)) why = WHY_TOOL_CACHE;
		else if (!strcmp(it->code, SCAN_FIRMWARE_CODE) && it->bytes >= (1 << 20)) why = WHY_FIRMWARE;
		if (why >= 0) { g->spare[g->nspare].item = i; g->spare[g->nspare++].why = (uint8_t)why; g->spare_bytes += it->bytes; continue; }
		int pkg = it->kind == ITEM_FILE && (!strncmp(it->path, "packages/", 9) || !strchr(it->path, '/')) && strlen(it->path) > 4 &&
			!strcasecmp(it->path + strlen(it->path) - 4, ".pkg");
		if (in_game && !strncmp(in_game, "_INST_", 6) && it->newest > (int64_t)time(NULL) - 86400) continue;		// (an install maybe still going: a day first)
		if (in_game && (!strncmp(in_game, "_INST_", 6) || !strncmp(in_game, "_DEL_", 5))) why = WHY_LEFTOVER;		// (an install or a delete that didn't finish)
		else if (pkg) {		// (installed when its header's ID is a game installed here; else a check)
			int installed = 0;
			for (int x2 = 0; it->tid[0] && x2 < g->ng && !installed; x2++) installed = !strcmp(g->g[x2].tid, it->tid) && g->g[x2].bytes[PART_INSTALLED] > 0;
			why = installed ? WHY_PKG : WHY_PKG_NEW;
		}
		else if (in_game && !it->code[0] && !it->title[0]) why = WHY_NO_SFO;		// (no PARAM.SFO: a broken install, or something else's)
		else if (it->cat == CAT_PKGTMP && !strncmp(it->path, "packages/", 9)) why = WHY_PKG_NEW;		// (anything else in packages/: not known to be installed)
		// (the system's downloads and installs waiting, psdevwiki's Project Database (PDB): a folder of 8 hex
		// digits each; task/00000001 is its own, always there)
		else if (it->kind == ITEM_DIR && !strncmp(it->path, "vsh/task/", 9) && Hex8(it->path + 9) && strcmp(it->path, "vsh/task/00000001") &&
			it->bytes >= (1 << 20)) why = WHY_DOWNLOAD;
		else if (it->kind == ITEM_DIR && !strncmp(it->path, "vsh/game_pkg/", 13) && Hex8(it->path + 13) && it->bytes >= (1 << 20)) why = WHY_QUEUED;
		else if (it->cat == CAT_PKGTMP && !strcmp(it->path, "tmp/wmtmp")) why = WHY_CACHE;
		// (the rest of tmp isn't suggested: homebrew keeps its settings and plugins there, webMAN's
		// wm_config.bin, a game's mod .sprx, PKGi's and Apollo's files: seen on a console, 0.9.2)
		else if (x >= 0) {
			const game_t *gm = &g->g[x];
			int has_copy = gm->copies > 0;
			char stem[ITEM_PATH], kept[ITEM_PATH];
			if (part == PART_BACKUP && gm->copies >= 2 && gm->keep_item >= 0 && Copy_Stem(it, stem, sizeof(stem))) {
				// a copy other than the one kept (the largest; a split image's parts go together)
				if (!Copy_Stem(&s->items[gm->keep_item], kept, sizeof(kept)) || strcmp(stem, kept)) why = WHY_EXTRA_COPY;
			} else if (part == PART_INSTALLED && !strcmp(it->code, "GD") && !has_copy) why = WHY_LONE_UPDATE;
			else if (part == PART_SAVE && !has_copy && !gm->bytes[PART_INSTALLED]) why = WHY_LONE_SAVE;
		}
		if (why >= 0) {
			g->spare[g->nspare].item = i;
			g->spare[g->nspare++].why = (uint8_t)why;
			g->spare_bytes += it->bytes;
		}
	}
	sort_s = s;
	qsort(g->spare, g->nspare, sizeof(spare_t), Spare_Cmp);
	return g;
}

int games_find(const games_t *g, const scan_t *s, uint32_t item)
{
	(void)s;
	return item < s->num_items ? g->game_of[item] : -1;
}

void games_free(games_t *g)
{
	if (!g) return;
	free(g->g);
	free(g->members);
	free(g->spare);
	free(g->game_of);
	free(g);
}
