/*
games_test.c: games.c's copies, from a made-up scan: what's a whole copy of a
game and what only sits beside one (webMAN's covers, a .dkey, a .cue), split
images and a disc's tracks as one copy, and game data that isn't a copy.
*/
#include <stdio.h>
#include <string.h>
#include "games.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static scan_item_t items[32];
static int n;

static void Item(int cat, int kind, const char *path, const char *tid, const char *code, uint64_t bytes)
{
	scan_item_t *it = &items[n++];
	memset(it, 0, sizeof(*it));
	snprintf(it->path, sizeof(it->path), "%s", path);
	snprintf(it->tid, sizeof(it->tid), "%s", tid);
	snprintf(it->code, sizeof(it->code), "%s", code);
	it->cat = (uint8_t)cat;
	it->kind = (uint8_t)kind;
	it->bytes = bytes;
	it->dir = -1;
}

static const game_t *Game(const games_t *g, const char *tid)
{
	for (int i = 0; i < g->ng; i++) if (!strcmp(g->g[i].tid, tid)) return &g->g[i];
	return NULL;
}

static int Spare(const games_t *g, const scan_t *s, const char *path)
{
	for (int i = 0; i < g->nspare; i++) if (!strcmp(s->items[g->spare[i].item].path, path)) return g->spare[i].why;
	return -1;
}

int main(void)
{
	// one ISO with its webMAN cover and dkey, and the disc's update data: one copy, not a duplicate
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Uncharted 3 [BCES01175].iso", "BCES01175", "DG", 25000);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Uncharted 3 [BCES01175].png", "", "", 300);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Uncharted 3 [BCES01175].dkey", "", "", 16);
	Item(CAT_GAMES, ITEM_DIR, "game/BCES01175", "BCES01175", "GD", 3000);
	// a split ISO (one copy) and a folder backup of the same game: two copies
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Big [BLUS30001].iso.0", "BLUS30001", "DG", 4000);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Big [BLUS30001].iso.1", "", "", 4000);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Big [BLUS30001].jpg", "", "", 100);
	Item(CAT_BACKUPS, ITEM_DIR, "GAMES/BLUS30001-[Big]", "BLUS30001", "DG", 7000);
	// a PS1 disc's tracks and its cue: one copy
	Item(CAT_BACKUPS, ITEM_FILE, "PSXISO/Classic [SCUS94426] (Track 1).bin", "", "", 500);
	Item(CAT_BACKUPS, ITEM_FILE, "PSXISO/Classic [SCUS94426] (Track 2).bin", "", "", 50);
	Item(CAT_BACKUPS, ITEM_FILE, "PSXISO/Classic [SCUS94426].cue", "", "", 1);
	// an installed HDD game and an ISO of it: two copies
	Item(CAT_GAMES, ITEM_DIR, "game/NPEB00001", "NPEB00001", "HG", 900);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Psn [NPEB00001].ISO", "", "", 800);

	// an install the system didn't finish, and a folder in game/ with no PARAM.SFO
	Item(CAT_GAMES, ITEM_DIR, "game/_INST_00001", "", "", 5000);
	Item(CAT_GAMES, ITEM_DIR, "game/MYAPPDATA", "", "", 700);
	// tmp: webMAN's cache is spare; the rest of tmp isn't (homebrew's settings and plugins live there)
	Item(CAT_PKGTMP, ITEM_DIR, "tmp/wmtmp", "", "", 900);
	Item(CAT_PKGTMP, ITEM_FILE, "tmp/wm_config.bin", "", "", 10);
	Item(CAT_PKGTMP, ITEM_FILE, "tmp/FuryV2MW3.sprx", "", "", 90);
	// the system's downloads: one not installed is a check; its own task 00000001 and small ones aren't
	// packages: installed when their header's ID is an installed game here; else a check (and at the drive's top too)
	Item(CAT_PKGTMP, ITEM_FILE, "packages/psn.pkg", "NPEB00001", "", 4000);
	Item(CAT_PKGTMP, ITEM_FILE, "packages/other.pkg", "NPUB99999", "", 4000);
	Item(CAT_SYSTEM, ITEM_FILE, "Latest_HEN_Installer_signed.pkg", "", "", 4000);
	Item(CAT_SYSTEM, ITEM_DIR, "vsh/task/00000002", "", "", 5 << 20);
	// the scan's splits: an app's cache (a check), a disc's system update (deletable); a stealth ISO is a backup copy
	Item(CAT_GAMES, ITEM_DIR, "game/IRISMAN00/cache", "", SCAN_CACHE_CODE, 3 << 20);
	Item(CAT_BACKUPS, ITEM_DIR, "GAMES/X/PS3_UPDATE", "", SCAN_FIRMWARE_CODE, 3 << 20);
	Item(CAT_BACKUPS, ITEM_FILE, "PS3ISO/Stealthy [NPUB55555].iso", "NPUB55555", "DG", 700);
	Item(CAT_MEDIA, ITEM_FILE, "video/Stealthy [NPUB55555].iso", "NPUB55555", "DG", 700);
	Item(CAT_SYSTEM, ITEM_DIR, "vsh/task/00000001", "", "", 5 << 20);
	Item(CAT_SYSTEM, ITEM_DIR, "vsh/game_pkg/80000000", "", "", 3 << 20);
	Item(CAT_SYSTEM, ITEM_DIR, "vsh/task/notatask", "", "", 3 << 20); Item(CAT_SYSTEM, ITEM_DIR, "vsh/game_pkg/8000000", "", "", 3 << 20);

	scan_t s;
	memset(&s, 0, sizeof(s));
	s.items = items;
	s.num_items = (uint32_t)n;
	games_t *g = games_build(&s);
	CHECK(g != NULL);
	if (!g) return 1;

	const game_t *u = Game(g, "BCES01175");
	CHECK(u && u->copies == 1);
	CHECK(u && u->copies_bytes == 25000);
	CHECK(u && u->could_free == 0);
	CHECK(u && u->data_count == 1 && u->data_bytes == 3000);
	CHECK(u && u->bytes[PART_BACKUP] == 25316);		// (the cover and dkey are still the backup's)
	CHECK(Spare(g, &s, "PS3ISO/Uncharted 3 [BCES01175].png") < 0);

	const game_t *b = Game(g, "BLUS30001");
	CHECK(b && b->copies == 2);
	CHECK(b && b->copies_bytes == 15000);
	CHECK(b && b->could_free == 7000);		// (the split ISO, 8000, is the one kept)
	CHECK(Spare(g, &s, "GAMES/BLUS30001-[Big]") == WHY_EXTRA_COPY);
	CHECK(Spare(g, &s, "PS3ISO/Big [BLUS30001].iso.0") < 0);
	CHECK(Spare(g, &s, "PS3ISO/Big [BLUS30001].iso.1") < 0);
	CHECK(Spare(g, &s, "PS3ISO/Big [BLUS30001].jpg") < 0);

	const game_t *c = Game(g, "SCUS94426");
	CHECK(c && c->copies == 1 && c->copies_bytes == 550);

	const game_t *p = Game(g, "NPEB00001");
	CHECK(p && p->copies == 2 && p->could_free == 800);
	CHECK(p && p->data_count == 0);

	CHECK(Spare(g, &s, "game/_INST_00001") == WHY_LEFTOVER);
	CHECK(Spare(g, &s, "game/MYAPPDATA") == WHY_NO_SFO);
	CHECK(Spare(g, &s, "packages/psn.pkg") == WHY_PKG && Spare(g, &s, "packages/other.pkg") == WHY_PKG_NEW &&
		Spare(g, &s, "Latest_HEN_Installer_signed.pkg") == WHY_PKG_NEW);
	CHECK(Spare(g, &s, "game/IRISMAN00/cache") == WHY_TOOL_CACHE && Spare(g, &s, "GAMES/X/PS3_UPDATE") == WHY_FIRMWARE);
	CHECK(Game(g, "NPUB55555") && Game(g, "NPUB55555")->bytes[PART_BACKUP] == 1400 && Game(g, "NPUB55555")->copies == 2);		// (the ISO in video/ a copy, with the one in PS3ISO)
	CHECK(Spare(g, &s, "vsh/task/00000002") == WHY_DOWNLOAD && Spare(g, &s, "vsh/task/00000001") < 0 && Spare(g, &s, "vsh/game_pkg/80000000") == WHY_QUEUED &&
		Spare(g, &s, "vsh/task/notatask") < 0 && Spare(g, &s, "vsh/game_pkg/8000000") < 0);
	CHECK(Spare(g, &s, "tmp/wmtmp") == WHY_CACHE && Spare(g, &s, "tmp/wm_config.bin") < 0 && Spare(g, &s, "tmp/FuryV2MW3.sprx") < 0);
	CHECK(Spare(g, &s, "game/BCES01175") < 0);		// (a game's data with its PARAM.SFO and a copy here: not spare)

	games_free(g);
	printf(fails ? "games_test: %d FAILED\n" : "games_test: all PASS\n", fails);
	return fails != 0;
}
