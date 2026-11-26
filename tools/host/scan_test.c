/*
scan_test.c: the scanner on a PC, against a folder standing for /dev_hdd0.

  scan_test <root> [cancel_after_ms]

Scans root as the console would, checks the result (the categories add up,
the largest files are in order, the cache reads back the same), and with
cancel_after_ms stops part way and checks nothing is handed over.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "../../src/plat.h"
#include "../../src/scan.h"
#include "../../src/sfo.h"
#include "../../src/report.h"
#include "../../src/bin.h"
#include "../../src/iso.h"
#include "../../src/fsx.h"
#ifndef DIR_ENTRIES_MAX
#define DIR_ENTRIES_MAX 200000
#endif
#include <dirent.h>
#include <sys/stat.h>

// the recycle bin's files of a type (ext, lower case; "*": those whose type isn't one of the
// category's kept ones), walked on the PC: the scan counts them in their type, but never offers
// them among its largest files
static uint32_t Bin_Files(const char *dir, const scan_t *s, int c, const char *ext)
{
	uint32_t n = 0;
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d) return 0;
	while ((e = readdir(d))) {
		char p[2048];
		struct stat st;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (stat(p, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) { n += Bin_Files(p, s, c, ext); continue; }
		const char *dot = strrchr(e->d_name, '.');
		char x[8] = "";
		if (dot && dot != e->d_name && dot[1] && strlen(dot + 1) < sizeof(x)) for (int i = 0; dot[1 + i]; i++) x[i] = (char)(dot[1 + i] | 0x20);
		if (strcmp(ext, "*")) { n += !strcmp(x, ext); continue; }
		int kept = 0;		// (the rest: its type isn't one of the category's kept ones)
		for (int k = 0; k < s->num_types[c]; k++) kept |= !strcmp(s->types[c][k].ext, x);
		n += !kept;
	}
	closedir(d);
	return n;
}

static const scan_item_t *Item(const scan_t *s, const char *path)
{
	for (uint32_t i = 0; i < s->num_items; i++) if (!strcmp(s->items[i].path, path)) return &s->items[i];
	return NULL;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: scan_test <root> [cancel_after_ms]\n"); return 2; }
	int cancel_ms = argc > 2 ? atoi(argv[2]) : 0, plain = argc > 2 && !strcmp(argv[2], "plain");		// (plain: scan and keep the cache, no checks)
	if (argc > 2 && !strcmp(argv[2], "stuck")) {		// (built with short SCAN_QUIET_US and SCAN_GIVE_UP_US)
		extern volatile int host_unstick;
		char at[192] = "";
		setenv("HOST_STUCK", "/home", 1);		// (home's listing never comes back)
		CHECK(scan_start(argv[1]));
		uint64_t t0 = plat_time_us();
		while (!scan_watch(at, sizeof(at))) {
			CHECK(scan_progress()->running && plat_time_us() - t0 < 30000000ULL);
			usleep(20000);
		}
		CHECK(plat_time_us() - t0 >= SCAN_GIVE_UP_US && strstr(at, "/home"));
		CHECK(!scan_watch(at, sizeof(at)));		// (said once)
		CHECK(!scan_start(argv[1]));		// (the stuck one's thread is still in its call)
		host_unstick = 1;		// the call comes back: the scan stops there, keeping nothing
		scan_wait(5000000);
		CHECK(!scan_progress()->running && !scan_take());
		unsetenv("HOST_STUCK");
		CHECK(scan_start(argv[1]));		// and the next one runs as ever
		scan_t *n = NULL;
		while (!(n = scan_take()) && scan_progress()->running) usleep(20000);
		if (!n) n = scan_take();
		CHECK(n && n->files > 0 && !scan_progress()->given_up);
		scan_free(n);
		printf("PASS (stuck: given up after %.1f s at %s; nothing kept; the next scan ran)\n", SCAN_GIVE_UP_US / 1e6, at);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[2], "folder")) {		// Stop in the middle of a big folder's listing: it stops there, not at its end
		setenv("HOST_SLOW_READ", "/tmp/many", 1);		// (20,000 entries, 5 ms a batch of 32: over 3 s to list)
		CHECK(scan_start(argv[1]));
		uint64_t t0 = plat_time_us();
		for (;;) {
			char at[192];
			spin_lock(&scan_progress()->progress_lock);
			snprintf(at, sizeof(at), "%s", scan_progress()->at);
			spin_unlock(&scan_progress()->progress_lock);
			if (strstr(at, "/tmp/many")) break;
			CHECK(scan_progress()->running && plat_time_us() - t0 < 30000000ULL);
			usleep(5000);
		}
		usleep(200000);
		uint64_t t1 = plat_time_us();
		scan_cancel();
		scan_wait(5000000);
		double took = (plat_time_us() - t1) / 1e6;
		CHECK(!scan_progress()->running && !scan_take() && took < 0.5);
		printf("PASS (stopped inside a big folder's listing, in %.2f s)\n", took);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[2], "entries")) {		// built with DIR_ENTRIES_MAX 5000: tmp/many (20,000) is cut off there
		CHECK(scan_start(argv[1]));
		scan_t *n = NULL;
		while (!(n = scan_take()) && scan_progress()->running) usleep(20000);
		if (!n) n = scan_take();
		const scan_item_t *t = n ? Item(n, "tmp/many") : NULL;
		if (DIR_ENTRIES_MAX >= 20000) {		// (a folder with just that many: it ended there, not cut off)
			CHECK(n && t && t->unreadable == 0 && t->files == 20000);
			printf("PASS (a folder of exactly %d entries, the cap: all counted, not taken as damaged)\n", DIR_ENTRIES_MAX);
			scan_free(n);
			return 0;
		}
		CHECK(n && t && t->unreadable == 1 && t->files >= 5000 && t->files < 5000 + 64 && n->unreadable >= 1);
		printf("PASS (a folder past %d entries: %u of its files counted, the rest skipped as unreadable; the scan finished)\n", 5000, (unsigned)t->files);
		scan_free(n);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[2], "eio")) {		// home/ gives a read error (bad sectors): skipped, counted as the drive's
		setenv("HOST_EIO", "/home", 1);
		CHECK(scan_start(argv[1]));
		scan_t *n = NULL;
		while (!(n = scan_take()) && scan_progress()->running) usleep(20000);
		if (!n) n = scan_take();
		CHECK(n && n->files > 0 && scan_progress()->read_errors == 1 && n->unreadable > scan_progress()->read_errors);
		printf("PASS (a read error: the scan finished, %llu unreadable, %llu of them the drive's)\n",
			(unsigned long long)n->unreadable, (unsigned long long)scan_progress()->read_errors);
		scan_free(n);
		return 0;
	}
	CHECK(scan_start(argv[1]));
	CHECK(!scan_start(argv[1]));		// (one at a time)
	uint64_t t0 = plat_time_us();
	scan_t *s = NULL;
	while (!(s = scan_take())) {
		scan_progress_t *p = scan_progress();
		if (!p->running) break;
		if (cancel_ms && plat_time_us() - t0 > (uint64_t)cancel_ms * 1000) scan_cancel();
		usleep(20000);
	}
	if (!s) s = scan_take();		// (it can finish between the take and the running check)
	if (cancel_ms) {
		scan_wait(5000000);
		CHECK(!s && !scan_take());
		CHECK(scan_stopped());
		printf("PASS (stopped part way: nothing handed over)\n");
		return 0;
	}
	CHECK(s);
	if (plain) { scan_free(s); printf("PASS (scanned: %s)\n", argv[1]); return 0; }
	uint64_t sum = 0, items = 0;
	for (int k = 0; k < NUM_SCANNED_CATS; k++) { sum += s->cat_bytes[k]; items += s->cat_items[k]; }
	CHECK(sum == s->scanned);
	CHECK(items == s->num_items);
	for (uint32_t i = 1; i < s->num_big; i++) CHECK(s->big[i - 1].bytes >= s->big[i].bytes);
	// each kept type's own largest files: every type with files has some (at most TYPE_FILES, largest
	// first), each its type's, and none of them past what the type holds
	for (int c = 0; c < NUM_SCANNED_CATS; c++)
		for (int k = 0; k < s->num_types[c]; k++) {
			uint32_t n = 0;
			uint64_t prev = UINT64_MAX, sum_t = 0;
			for (uint32_t i = 0; i < s->num_tfiles; i++) {
				const scan_big_t *t = &s->tfiles[i];
				if (t->cat != c || t->type != k) continue;
				n++;
				sum_t += t->bytes;
				CHECK(t->bytes <= prev);
				prev = t->bytes;
				const char *dot = strrchr(t->path, '.'), *sl = strrchr(t->path, '/');
				if (strcmp(s->types[c][k].ext, "*") && s->types[c][k].ext[0]) CHECK(dot && (!sl || dot > sl) && !strcasecmp(dot + 1, s->types[c][k].ext));
			}
			// (the bin's files of the type count in it, but aren't among its largest)
			char bin[FSX_PATH], bin_abs[FSX_PATH + 64];
			uint32_t in_bin = 0, offered;
			if (bin_rel(s->root, bin, sizeof(bin))) {
				snprintf(bin_abs, sizeof(bin_abs), "%s/%s", s->root, bin);
				char top[FSX_NAME];		// (its files are its top folder's category's)
				snprintf(top, sizeof(top), "%.*s", (int)strcspn(bin, "/"), bin);
				if (scan_cat_of(top, 1) == c) in_bin = Bin_Files(bin_abs, s, c, s->types[c][k].ext);
			}
			offered = s->types[c][k].files - (in_bin < s->types[c][k].files ? in_bin : s->types[c][k].files);
			CHECK(n <= TYPE_FILES && n == (offered < TYPE_FILES ? offered : TYPE_FILES));
			CHECK(sum_t <= s->types[c][k].bytes);
		}
	printf("types: %u files kept for them\n", (unsigned)s->num_tfiles);
	if (s->used) CHECK(s->cat_bytes[CAT_UNACCOUNTED] == (s->used > s->scanned ? s->used - s->scanned : 0));
	// the folders' sizes: the top holds everything; each kept item's folder holds what the item does
	// (and still, when some weren't kept: their files count in the nearest kept folder above them)
	{
		CHECK(s->num_dirs > 1 && s->dir[0].bytes == s->scanned && s->dir[0].files == s->files);
		// (the scan's splits: an item of their own inside another's folder, the recycle bin, apps' caches, system updates)
		for (uint32_t i = 0; i < s->num_items; i++)
			if (s->items[i].dir >= 0) {
				const scan_dir_t *d = &s->dir[s->items[i].dir];
				uint64_t sb = 0, sf = 0;
				for (uint32_t j = 0; j < s->num_items; j++) {
					const scan_item_t *x = &s->items[j];
					size_t l = strlen(s->items[i].path);
					if (j != i && x->code[0] == '~' && !strncmp(x->path, s->items[i].path, l) && x->path[l] == '/') { sb += x->bytes; sf += x->files; }
				}
				CHECK(d->bytes == s->items[i].bytes + sb && d->files == s->items[i].files + sf);
			}
		uint64_t kids = 0;
		for (uint32_t c = s->first_child[0]; c != UINT32_MAX; c = s->next_sibling[c]) kids += s->dir[c].bytes;
		CHECK(kids <= s->scanned);		// (the rest: files at the top)
	}
	CHECK(scan_dir_find(s, "") == 0);
	CHECK(scan_cat_of("MUSIC", 1) == CAT_MEDIA && scan_cat_of("PICTURE", 1) == CAT_MEDIA && scan_cat_of("VIDEO", 1) == CAT_MEDIA);	// (a USB drive's)
	CHECK(scan_cat_of("PS3", 1) == CAT_SYSTEM && scan_cat_of("MUSIC", 0) == CAT_SYSTEM);
	CHECK(scan_cat_of("14157-ATARISlideShow.p3t", 0) == CAT_THEMES && scan_cat_of("Latest_HEN_Installer_signed.PKG", 0) == CAT_PKGTMP && scan_cat_of("x.p3t", 1) == CAT_SYSTEM);
	if (!s->dirs_dropped && Item(s, "game/BLUS30001")) {
		int d = scan_dir_find(s, "game/BLUS30001/USRDIR");
		CHECK(d > 0 && !strcmp(scan_dir_name(s, d), "USRDIR") && scan_dir_child(s, d, "d3") > 0);
		CHECK(scan_dir_find(s, "game/BLUS30001/nope") == -1);
		// titles and icons; categories for what the console's HDD showed
		const scan_item_t *g = Item(s, "game/BLUS30001");
		CHECK(!strcmp(g->title, "Fake Game 1 Subtitle on a second line") && !strcmp(g->tid, "BLUS30001") && !strcmp(g->code, "HG") && g->icon == ICON_HERE);
		CHECK(Item(s, "game/BLUS30002")->icon == ICON_NONE && Item(s, "game/BLUS30020")->title[0] == 0);
		CHECK(!strcmp(Item(s, "game/BCES01585")->title, "The Last of Us\xe2\x84\xa2"));
		const scan_item_t *u = Item(s, "GAMES/Uncharted 3");
		CHECK(u && u->icon == ICON_PS3_GAME && !strcmp(u->tid, "BCES01175") && u->cat == CAT_BACKUPS);
		CHECK(!strcmp(Item(s, "home/00000001/savedata/BLUS30002-SAVE")->title, "Fake Game 2 \xc2\xb7 Slot 3"));
		CHECK(!strcmp(Item(s, "home/00000001/trophy/NPWR00003_00")->title, "Trophies & Things 3"));
		const scan_item_t *ms = Item(s, "home/00000001/minis_savedata/NPUZ00001DATA");		// (a Minis save: its own item, titled)
		if (Item(s, "game/IRISMAN00/cache")) {		// (an app's cache, a disc's system update, an update waiting, a stealth ISO)
			CHECK(!strcmp(Item(s, "game/IRISMAN00/cache")->code, SCAN_CACHE_CODE) && Item(s, "game/IRISMAN00/cache")->bytes == 2 << 20);
			CHECK(!strcmp(Item(s, "GAMES/Uncharted 3/PS3_UPDATE")->code, SCAN_FIRMWARE_CODE) && Item(s, "updater/01/PS3UPDAT.PUP") &&
				Item(s, "updater/01/PS3UPDAT.PUP")->kind == ITEM_FILE && !strcmp(Item(s, "updater/01/PS3UPDAT.PUP")->code, SCAN_FIRMWARE_CODE) &&
				!Item(s, "updater/01") && Item(s, "updater") && !Item(s, "updater")->code[0] && Item(s, "updater")->bytes == 30 && Item(s, "updater")->dirs == 3);
			CHECK(!strcmp(Item(s, "video/Stealth.iso")->tid, "BLUS30003") && Item(s, "video/Stealth.iso")->cat == CAT_MEDIA);
		}
		if (Item(s, "packages/Fake1.pkg")) CHECK(!strcmp(Item(s, "packages/Fake1.pkg")->tid, "BLUS30001") && !strcmp(Item(s, "packages/NotYet.pkg")->tid, "NPUB99999"));		// (from their headers)
		const scan_item_t *dl = Item(s, "vsh/task/00000002"), *hh = Item(s, "vsh/game_pkg/80000000");		// (downloads: their own items, titled from d0.pdb)
		if (scan_dir_find(s, "vsh/task/00000002") > 0) CHECK(dl && dl->cat == CAT_SYSTEM && !strcmp(dl->title, "Fake Download") && hh && !strcmp(hh->title, "Fake Queued Game") &&
			Item(s, "vsh/task/10000003") && !strcmp(Item(s, "vsh/task/10000003")->title, "UP0001-NPUB31111_00-NOTITLE000000001"));
		const scan_item_t *p2 = Item(s, "home/00000001/ps2emu2_savedata/NPEB00001");		// (a PS2 Classic's save: its own item, titled)
		if (scan_dir_find(s, "home/00000001/ps2emu2_savedata") > 0) CHECK(p2 && p2->cat == CAT_SAVES && !strcmp(p2->title, "Fake PS2 Classic") && p2->bytes > 8000);
		if (scan_dir_find(s, "home/00000001/minis_savedata") > 0) CHECK(ms && ms->cat == CAT_SAVES && !strcmp(ms->title, "Fake Minis \xc2\xb7 Progress") && !strcmp(ms->tid, "NPUZ00001"));
		CHECK(Item(s, "ROMS/snes")->cat == CAT_BACKUPS);
	}
	if (!s->dirs_dropped && scan_dir_find(s, "HDDAtlas/bin") > 0 && Item(s, "HDDAtlas")) {		// (the fake HDD's recycle bin, when HDDAtlas has an item of its own)
		const scan_item_t *bin = Item(s, "HDDAtlas/bin"), *own = Item(s, "HDDAtlas");
		CHECK(bin && scan_item_is_bin(bin) && bin->bytes > 3ULL << 30 && bin->files == 2 && bin->cat == CAT_SYSTEM && bin->dir > 0);
		CHECK(own && own->bytes == 100 && own->files == 1);
		// SingStar's songs, known by their files: FileSystem with nothing else is called that whole; with
		// other things too (host-test.sh's "many" drive), only FileSystem/Export is, and the rest is FileSystem
		const scan_item_t *ss = Item(s, "FileSystem"), *ex = Item(s, "FileSystem/Export");
		int fd = scan_dir_find(s, "FileSystem"), ed = scan_dir_find(s, "FileSystem/Export");
		CHECK(ss && ss->cat == CAT_SYSTEM && fd > 0 && ed > 0);
		printf("singstar: %s\n", s->dir[ed].bytes == s->dir[fd].bytes ? "FileSystem is SingStar's songs, whole" : "FileSystem/Export split out as SingStar's songs");
		if (s->dir[ed].bytes == s->dir[fd].bytes) CHECK(!strcmp(ss->title, "SingStar songs") && !ex);
		else CHECK(ex && !strcmp(ex->title, "SingStar songs") && strcmp(ss->title, "SingStar songs") && ex->cat == CAT_SYSTEM &&
			ex->bytes == s->dir[ed].bytes && ss->bytes + ex->bytes == s->dir[fd].bytes);
		for (uint32_t i = 0; i < s->num_big; i++) CHECK(strncmp(s->big[i].path, "HDDAtlas/bin/", 13));		// (deleted: not among the largest)
		// the cache keeps it, and splitting again changes nothing
		CHECK(scan_save(s, "scan_test_bin.bin"));
		scan_t *l = scan_load("scan_test_bin.bin");
		CHECK(l && l->num_items == s->num_items && Item(l, "HDDAtlas")->bytes == 100 && Item(l, "HDDAtlas/bin")->bytes == bin->bytes);
		CHECK(l->cat_items[CAT_SYSTEM] == s->cat_items[CAT_SYSTEM]);
		scan_free(l);
		remove("scan_test_bin.bin");
		CHECK(Item(s, "screenshot_2026_09_25_12_19_14.bmp")->cat == CAT_MEDIA && Item(s, "boot_plugins.txt")->cat == CAT_SYSTEM);
	}
	// disc images' titles from inside them (and an ID from a name)
	if (!s->dirs_dropped && s->num_items < ITEMS_MAX) {
		const scan_item_t *i1 = Item(s, "PS3ISO/Game One.iso"), *i2 = Item(s, "PS2ISO/Old PS2.iso"), *i3 = Item(s, "PSXISO/Classic.bin"),
			*i4 = Item(s, "PS3ISO/Other Game [BCES00001].iso");
		CHECK(i1 && !strcmp(i1->title, "Fake Game 3") && !strcmp(i1->tid, "BLUS30003") && !strcmp(i1->code, "DG") && i1->icon == ICON_ISO_PS3);
		CHECK(Item(s, "game/BLUS30011")->newest == 1672531200 && Item(s, "home/00000001/savedata/BLUS30005-SAVE")->newest == 1717200000);
		CHECK(i2 && !strcmp(i2->tid, "SLUS20312") && !strcmp(i2->code, "2P") && !i2->title[0]);
		CHECK(i3 && !strcmp(i3->tid, "SCUS94426") && !strcmp(i3->code, "1P"));
		{	// a raw .bin read a sector at a time, through one open (a hundred before 1.0.1)
			extern int host_opens, host_preads;
			char p[FSX_PATH], t[64], id[16], c[8];
			int icon, o = host_opens, r = host_preads;
			snprintf(p, sizeof(p), "%s/PSXISO/Classic.bin", s->root);
			CHECK(iso_info(p, t, sizeof(t), id, sizeof(id), c, sizeof(c), &icon) && !strcmp(id, "SCUS94426"));
			CHECK(host_opens - o == 1 && host_preads - r > 1);
		}
		CHECK(i4 && !strcmp(i4->tid, "BCES00001"));
		// file types: every file counted once, in its category
		uint64_t tb = 0, tf = 0;
		for (int c = 0; c < NUM_SCANNED_CATS; c++) for (int k = 0; k < s->num_types[c]; k++) { tb += s->types[c][k].bytes; tf += s->types[c][k].files; }
		CHECK(tb == s->scanned && tf == s->files);
		CHECK(!strcmp(s->types[CAT_MEDIA][0].ext, "mp3") && s->types[CAT_MEDIA][0].files == 3000);
	}
	// the whole disk past the partition: the system's partitions; the partition's rest: Unaccounted
	if (s->disk) CHECK(s->cat_bytes[CAT_RESERVED] == s->disk - s->total && scan_drive_total(s) == s->disk);
	printf("largest: %s %llu\n", s->num_big ? s->big[0].path : "-", s->num_big ? (unsigned long long)s->big[0].bytes : 0ULL);
	// the cache
	char path[1024];
	scan_cache_path(argv[1], path, sizeof(path));
	scan_t *c = scan_load(path);
	CHECK(c);
	CHECK(c->num_items == s->num_items && c->scanned == s->scanned && c->files == s->files && c->num_big == s->num_big);
	CHECK(!memcmp(c->items, s->items, s->num_items * sizeof(scan_item_t)));
	CHECK(!memcmp(c->big, s->big, sizeof(s->big)));
	CHECK(c->num_tfiles == s->num_tfiles && (!s->num_tfiles || !memcmp(c->tfiles, s->tfiles, s->num_tfiles * sizeof(scan_big_t))));
	CHECK(c->num_dirs == s->num_dirs && c->names_len == s->names_len && c->disk == s->disk);
	CHECK(!s->num_dirs || (!memcmp(c->dir, s->dir, s->num_dirs * sizeof(scan_dir_t)) && !memcmp(c->names, s->names, s->names_len)));
	CHECK(scan_dir_find(c, "game") == scan_dir_find(s, "game"));
	// what changed: an item grown by 2 MB, one gone, one new, one changed by less than 1 MB (left out)
	{
		uint32_t g = 0, gone = 1, small = 2, add = c->num_items - 1;
		while (c->items[g].kind == ITEM_REST) g++;
		uint64_t was_g = c->items[g].bytes;
		c->items[g].bytes += 2 * 1024 * 1024;
		c->items[small].bytes += 1000;
		char gone_path[ITEM_PATH];
		snprintf(gone_path, sizeof(gone_path), "%s", c->items[gone].path);
		snprintf(c->items[gone].path, sizeof(c->items[gone].path), "zz-new-item");		// (gone under its name, new under this)
		(void)add;
		scan_changes_t *ch = scan_diff(s, c);
		CHECK(ch);
		int saw_g = 0, saw_gone = 0, saw_new = 0;
		for (uint32_t i = 0; i < ch->count; i++) {
			scan_change_t *x = &ch->c[i];
			if (!strcmp(x->path, c->items[g].path)) saw_g = x->was == was_g && x->now == was_g + 2 * 1024 * 1024;
			if (!strcmp(x->path, gone_path)) saw_gone = x->was_there && !x->now_there;
			if (!strcmp(x->path, "zz-new-item")) saw_new = !x->was_there && x->now_there;
			CHECK(strcmp(x->path, c->items[small].path));
		}
		CHECK(saw_g && saw_gone && saw_new && ch->count == 3);
		char cp[1100];
		scan_changes_path(argv[1], cp, sizeof(cp));
		CHECK(scan_changes_save(ch, cp));
		scan_changes_t *back = scan_changes_load(cp);
		CHECK(back && back->count == ch->count && !memcmp(back->c, ch->c, ch->count * sizeof(scan_change_t)));
		// a report of it: both files there, and the text names the largest item
		char out[1100];
		CHECK(report_write(s, ch, "Internal HDD", plat_save_dir(), out, sizeof(out)) == 0);
		size_t rl;
		char *rep = (char *)plat_read_file(out, &rl);
		CHECK(rep && strstr(rep, "Categories") && strstr(rep, "Changes since the scan before") && strstr(rep, " largest files"));
		// a name the PS3 was given in Windows-1252 ("\xae" for ®): the report's is UTF-8
		if (Item(s, "theme/PlayStation\xaePlus Theme.p3t")) CHECK(strstr(rep, "theme/PlayStation\xc2\xaePlus Theme.p3t") && !strstr(rep, "PlayStation\xaePlus"));
		free(rep);
		scan_changes_free(back);
		scan_changes_free(ch);
	}
	scan_free(c);
	// each category rescanned on its own, merged into the whole: the same as the whole scan
	if (!s->dirs_dropped && s->num_items < ITEMS_MAX && !cancel_ms) {
		for (int cat = 0; cat < NUM_SCANNED_CATS; cat++) {
			CHECK(scan_start_partial(argv[1], cat, s));
			scan_t *p = NULL;
			while (!(p = scan_take()) && scan_progress()->running) usleep(10000);
			if (!p) p = scan_take();
			CHECK(p && p->partial == cat + 1);
			CHECK(p->scanned == s->scanned && p->files == s->files && p->num_items == s->num_items && p->unreadable == s->unreadable);
			for (int k = 0; k < NUM_CATS; k++) CHECK(p->cat_bytes[k] == s->cat_bytes[k] && p->cat_items[k] == s->cat_items[k]);
			CHECK(p->dir[0].bytes == s->dir[0].bytes && p->num_dirs == s->num_dirs && p->num_big == s->num_big);
			for (uint32_t i = 0; i < s->num_big; i++) CHECK(p->big[i].bytes == s->big[i].bytes);
			CHECK(p->num_tfiles == s->num_tfiles);		// (each type's files: the other categories' kept, this one's found again)
			for (uint32_t i = 0; i < s->num_items; i++) {
				const scan_item_t *a = &s->items[i], *b = Item(p, a->path);
				CHECK(b && b->bytes == a->bytes && b->files == a->files && !strcmp(b->title, a->title));
				if (a->dir >= 0) CHECK(b->dir >= 0 && p->dir[b->dir].bytes == s->dir[a->dir].bytes);
			}
			static const char *const some[] = {"game/BCES01585/USRDIR", "home/00000001/savedata", "music/014", "GAMES/Uncharted 3/PS3_GAME"};
			for (int k = 0; k < 4; k++) {
				int da = scan_dir_find(s, some[k]), db = scan_dir_find(p, some[k]);
				CHECK(da > 0 && db > 0 && s->dir[da].bytes == p->dir[db].bytes && s->dir[da].files == p->dir[db].files);
			}
			CHECK(!memcmp(p->types, s->types, sizeof(s->types)) && !memcmp(p->num_types, s->num_types, sizeof(s->num_types)));
			scan_free(p);
		}
		printf("partial rescans: each category merged matches the whole\n");
	}
	// a damaged cache is refused, not trusted
	size_t len;
	unsigned char *b = plat_read_file(path, &len);
	CHECK(b && len > 100);
	char bad[1100];
	snprintf(bad, sizeof(bad), "%s.bad", path);
	plat_write_file(bad, b, len - 7);
	CHECK(!scan_load(bad));
	b[0] = 'X';
	plat_write_file(bad, b, len);
	CHECK(!scan_load(bad));
	remove(bad);
	free(b);
	// the recycle bin's row follows what's moved in and out (ui_bin_counted): its item, its category,
	// its folders and the totals; back to exactly as it was
	{
		char bin[FSX_PATH];
		CHECK(bin_rel(s->root, bin, sizeof(bin)));
		int bi = -1;
		for (uint32_t i = 0; i < s->num_items; i++) if (scan_item_is_bin(&s->items[i])) bi = (int)i;
		if (bi < 0) {		// (no row for it: past the items a scan keeps, or a drive with no bin then) made by the first move in
			uint32_t n0 = s->num_items;
			uint64_t sc0 = s->scanned;
			scan_bin_change(s, 7000, 1, 0);
			CHECK(s->num_items == n0 + 1 && scan_item_is_bin(&s->items[n0]) && s->items[n0].bytes == 7000 && s->scanned == sc0 + 7000);
			bi = (int)n0;
		}
		int bd = scan_dir_find(s, bin);
		uint64_t ib = s->items[bi].bytes, cb = s->cat_bytes[s->items[bi].cat], sc = s->scanned, top = s->dir[0].bytes, db = bd > 0 ? s->dir[bd].bytes : 0;
		uint32_t nitems = s->num_items;
		scan_bin_change(s, 5000000, 3, 0);
		CHECK(s->items[bi].bytes == ib + 5000000 && s->items[bi].files >= 3 && s->cat_bytes[s->items[bi].cat] == cb + 5000000);
		CHECK(s->scanned == sc + 5000000 && s->dir[0].bytes == top + 5000000 && (bd <= 0 || s->dir[bd].bytes == db + 5000000));
		CHECK(s->num_items == nitems);		// (its row: the one there)
		scan_bin_change(s, -5000000, -3, 0);
		CHECK(s->items[bi].bytes == ib && s->cat_bytes[s->items[bi].cat] == cb && s->scanned == sc && s->dir[0].bytes == top);
		scan_bin_change(s, -(int64_t)ib - 999, -1000000, 0);		// (more than it holds: down to nothing, never under)
		CHECK(s->items[bi].bytes == 0 && s->items[bi].files == 0 && s->cat_bytes[s->items[bi].cat] == cb - ib);
		// a scan with no bin row (none there when it scanned): made the first time something's moved in
		memset(s->items[bi].code, 0, sizeof(s->items[bi].code));
		scan_bin_change(s, 7000, 1, 0);
		CHECK(s->num_items == nitems + 1 && scan_item_is_bin(&s->items[nitems]) && s->items[nitems].bytes == 7000 && !strcmp(s->items[nitems].path, bin));
	}
	scan_free(s);
	// a damaged PARAM.SFO gives nothing, and never reads past its end
	static const unsigned char bad_sfo[] = {0, 'P', 'S', 'F', 1, 1, 0, 0, 0x24, 0, 0, 0, 0xff, 0xff, 0, 0, 1, 0, 0, 0,
		0, 0, 4, 2, 0x40, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 'T', 'I', 'T', 'L', 'E', 0};
	char out[64];
	for (size_t n = 0; n <= sizeof(bad_sfo); n++) CHECK(!sfo_get(bad_sfo, n, "TITLE", out, sizeof(out)));
	CHECK(!sfo_trophy_title((const unsigned char *)"<title-name>open", 16, out, sizeof(out)));
	printf("PASS\n");
	return 0;
}
