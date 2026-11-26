/*
del_test.c: del.c on a PC: the allow-list, and real deletes in a scratch drive
(build/host/deltest/dev_hdd0, DEL_TEST_ROOT) made fresh each run: an ISO with its
twin, folder backups (one of 2,000 files; one holding a link out of the drive,
which must not be followed; one stopped part way), a package, a ROM, a cache
entry, and the scan forgetting what went (it must match a rescan).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../src/plat.h"
#include "../../src/del.h"
#include "../../src/bin.h"
#include "../../src/scan.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fflush(stdout); exit(1); } } while (0)
#define R DEL_TEST_ROOT

static void Mk(const char *rel, int size)
{
	char p[1024], cmd[1100];
	snprintf(p, sizeof(p), "%s/%s", R, rel);
	char *slash = strrchr(p, '/');
	*slash = 0;
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", p);
	CHECK(system(cmd) == 0);
	*slash = '/';
	FILE *f = fopen(p, "wb");
	CHECK(f);
	for (int i = 0; i < size; i++) fputc('x', f);
	fclose(f);
}

static int Exists(const char *rel)
{
	char p[1024];
	struct stat st;
	snprintf(p, sizeof(p), "%s/%s", R, rel);
	return lstat(p, &st) == 0;
}

static int Kind(const char *rel, int dir)
{
	char why[160];
	return del_kind(R, rel, dir, why, sizeof(why));
}

static del_job_t job;
static int Delete(const char *rel, int dir)
{
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "%s", rel);
	job.is_dir = dir;
	del_twin(R, rel, job.twin, sizeof(job.twin));
	return del_run(&job);
}

static scan_t *Scan(void)
{
	CHECK(scan_start(R));
	scan_t *s;
	while (!(s = scan_take()) && scan_progress()->running) usleep(10000);
	if (!s) s = scan_take();
	CHECK(s);
	return s;
}

static void Grow_Soon(void *arg) { (void)arg; usleep(150000); Mk("game/_INST_00000007/USRDIR/b.dat", 900); }

static void Cancel_Soon(void *arg) { (void)arg; while (job.files_done < 500) usleep(100); job.cancel = 1; }

int main(void)
{
	char why[160];
	// the allow-list: shapes, depths, and what's never deleted
	CHECK(Kind("PS3ISO/Game.iso", 0) == DEL_ISO && Kind("PSXISO/Game.bin", 0) == DEL_ISO && Kind("DVDISO/Film.iso", 0) == DEL_ISO);
	CHECK(Kind("PS3ISO/Folder", 1) == DEL_NO && Kind("PS3ISO", 1) == DEL_NO && Kind("PS3ISO/a/b.iso", 0) == DEL_NO);
	// an NTFS drive (src/ps3/ntfs_ps3.c mounts it as /ntfsN:): the same rules as a USB drive's, its bin
	// at its top, and HDD Atlas's own files for it named without the ":"
	{
		char w[160], key[32], b[FSX_PATH];
		CHECK(del_kind("/ntfs0:", "PS3ISO/Game.iso", 0, w, sizeof(w)) == DEL_ISO && del_kind("/ntfs7:", "PS3ISO/Game.iso", 0, w, sizeof(w)) == DEL_ISO);
		CHECK(del_kind("/ntfs8:", "PS3ISO/Game.iso", 0, w, sizeof(w)) == DEL_NO && del_kind("/ntfs0", "PS3ISO/Game.iso", 0, w, sizeof(w)) == DEL_NO);
		CHECK(del_kind("/ntfs0:/PS3ISO", "Game.iso", 0, w, sizeof(w)) == DEL_NO && del_kind("/ntfs:", "PS3ISO/Game.iso", 0, w, sizeof(w)) == DEL_NO);
		CHECK(del_kind("/ntfs0:", "game/BLUS30443", 1, w, sizeof(w)) == DEL_NO);
		CHECK(bin_rel("/ntfs2:", b, sizeof(b)) && !strcmp(b, "HDDAtlas/bin") && del_kind("/ntfs2:", "HDDAtlas/bin/123_1", 1, w, sizeof(w)) == DEL_BIN);
		fsx_drive_key("/ntfs3:", key, sizeof(key));
		CHECK(!strcmp(key, "ntfs3"));
		fsx_drive_key("/dev_usb001", key, sizeof(key));
		CHECK(!strcmp(key, "dev_usb001"));
	}
	CHECK(Kind("GAMES/Uncharted 3", 1) == DEL_BACKUP && Kind("GAMES/Uncharted 3", 0) == DEL_NO && Kind("GAMES/U/PS3_GAME", 1) == DEL_NO);
	CHECK(Kind("packages/update.pkg", 0) == DEL_PKG && Kind("packages/update.PKG", 0) == DEL_PKG && Kind("packages/notes.txt", 0) == DEL_NO);
	CHECK(Kind("ROMS/snes/game.smc", 0) == DEL_ROM && Kind("ROMS/snes", 1) == DEL_NO && Kind("ROMS/game.smc", 0) == DEL_NO);
	CHECK(Kind("GAMES/X/PS3_UPDATE", 1) == DEL_FIRMWARE && Kind("GAMEZ/X/PS3_UPDATE", 1) == DEL_FIRMWARE && Kind("updater/01", 1) == DEL_NO);
	CHECK(Kind("GAMEZ/X", 1) == DEL_BACKUP && Kind("updater", 1) == DEL_NO && Kind("GAMES/X/PS3_GAME", 1) == DEL_NO);
	// (a system update waiting: the .pup file only, never updater/ or its folders: HEN makes updater/01 again)
	CHECK(Kind("updater/01/PS3UPDAT.PUP", 0) == DEL_FIRMWARE && Kind("updater/02/hfw.pup", 0) == DEL_FIRMWARE && Kind("updater/01/PS3UPDAT.PUP", 1) == DEL_NO);
	CHECK(Kind("updater/PS3UPDAT.PUP", 0) == DEL_NO && Kind("updater/01/notes.txt", 0) == DEL_NO && Kind("updater/01/x/PS3UPDAT.PUP", 0) == DEL_NO);
	CHECK(Kind("Latest_HEN_Installer_signed.pkg", 0) == DEL_PKG && Kind("game.pkg", 1) == DEL_NO && Kind("PS3ISO", 1) == DEL_NO);		// (a package at the top; never a top folder)
	CHECK(Kind("tmp/wmtmp/cache.bin", 0) == DEL_CACHE && Kind("tmp/wmtmp/sub", 1) == DEL_CACHE && Kind("tmp/other.bin", 0) == DEL_NO);
	// what webMAN needs to list and mount a game: an NTFS/exFAT game's stub, a decryption key (kept, with why)
	{
		char w[160];
		CHECK(del_kind(R, "tmp/wmtmp/Uncharted.iso.ntfs[PS3ISO]", 0, w, sizeof(w)) == DEL_NO && strstr(w, "NTFS"));
		CHECK(Kind("tmp/wmtmp/Crash.bin.ntfs[PSXISO]", 0) == DEL_NO && Kind("tmp/wmtmp/Film.iso.ntfs[BDISO]", 0) == DEL_NO);
		CHECK(del_kind(R, "tmp/wmtmp/Uncharted.dkey", 0, w, sizeof(w)) == DEL_NO && strstr(w, "key") && Kind("tmp/wmtmp/X.DKEY", 0) == DEL_NO);
		CHECK(Kind("PS3ISO/Uncharted.dkey", 0) == DEL_NO && Kind("PS3ISO/Uncharted.iso", 0) == DEL_ISO);
		CHECK(Kind("tmp/wmtmp/cover.ntfs.png", 0) == DEL_CACHE && Kind("tmp/wmtmp/x.ntfs[", 0) == DEL_CACHE);
	}
	CHECK(Kind("game/BLUS30443", 1) == DEL_NO && del_kind(R, "game/BLUS30443", 1, why, sizeof(why)) == DEL_NO && strstr(why, "XMB"));
	CHECK(Kind("game/HDAT00001", 1) == DEL_NO && Kind("home/00000001/savedata/X", 1) == DEL_NO && Kind("music/a.mp3", 0) == DEL_NO);
	CHECK(Kind("vsh/task", 1) == DEL_NO && Kind("hen/x", 0) == DEL_NO && Kind("GAMES", 1) == DEL_NO);
	CHECK(Kind("PS3ISO/../game/x.iso", 0) == DEL_NO && Kind("GAMES/..", 1) == DEL_NO && Kind("GAMES/.", 1) == DEL_NO);
	CHECK(Kind("/PS3ISO/a.iso", 0) == DEL_NO && Kind("PS3ISO//a.iso", 0) == DEL_NO && Kind("PS3ISO/a.iso/", 0) == DEL_NO && Kind("", 0) == DEL_NO);
	CHECK(del_kind("/dev_hdd0", "PS3ISO/a.iso", 0, why, sizeof(why)) == DEL_ISO && del_kind("/dev_usb007", "GAMES/X", 1, why, sizeof(why)) == DEL_BACKUP);
	CHECK(del_kind("/dev_flash", "PS3ISO/a.iso", 0, why, sizeof(why)) == DEL_NO && del_kind("/dev_usb008", "GAMES/X", 1, why, sizeof(why)) == DEL_NO);
	CHECK(del_kind("/dev_hdd1", "PS3ISO/a.iso", 0, why, sizeof(why)) == DEL_NO && del_kind("/", "PS3ISO/a.iso", 0, why, sizeof(why)) == DEL_NO);

	// a fresh scratch drive
	CHECK(system("rm -rf '" R "' '" R "/../outside' && mkdir -p '" R "/../outside'") == 0);
	Mk("PS3ISO/Game.iso", 3000);
	Mk("PSXISO/Old.bin", 2000); Mk("PSXISO/Old.cue", 100);
	for (int i = 0; i < 2000; i++) { char p[128]; snprintf(p, sizeof(p), "GAMES/Big/PS3_GAME/USRDIR/d%d/f%d.dat", i % 20, i); Mk(p, 10); }
	for (int i = 0; i < 5000; i++) { char p[128]; snprintf(p, sizeof(p), "GAMES/Huge/f%d.dat", i); Mk(p, 1); }
	Mk("GAMES/Linked/real.dat", 5);
	Mk("../outside/keep.txt", 7);
	CHECK(symlink("../../../outside", R "/GAMES/Linked/out") == 0);
	Mk("packages/update.pkg", 400); Mk("ROMS/snes/game.smc", 300); Mk("tmp/wmtmp/cache.bin", 200);
	Mk("game/BLUS30443/EBOOT.BIN", 50);

	// the scan forgets what went, and matches a rescan
	scan_t *s = Scan();
	CHECK(Delete("PS3ISO/Game.iso", 0) == 0 && job.files_done == 1 && job.bytes_done == 3000 && !Exists("PS3ISO/Game.iso"));
	scan_forget(s, "PS3ISO/Game.iso", job.bytes_done, job.files_done, 0, 0);
	CHECK(Delete("GAMES/Big", 1) == 0 && job.files_done == 2000 && job.bytes_done == 20000 && !Exists("GAMES/Big"));
	scan_forget(s, "GAMES/Big", job.bytes_done, job.files_done, 1, 0);
	scan_t *s2 = Scan();
	CHECK(s->scanned == s2->scanned && s->files == s2->files && s->num_items == s2->num_items && s->edits == 2 && !s2->edits);
	for (int k = 0; k < NUM_SCANNED_CATS; k++) CHECK(s->cat_bytes[k] == s2->cat_bytes[k] && s->cat_items[k] == s2->cat_items[k]);
	CHECK(s->dir[0].bytes == s2->dir[0].bytes && s->dir[scan_dir_find(s, "GAMES")].bytes == s2->dir[scan_dir_find(s2, "GAMES")].bytes);
	for (uint32_t i = 0; i < s->num_big; i++) CHECK(strcmp(s->big[i].path, "PS3ISO/Game.iso") && strncmp(s->big[i].path, "GAMES/Big/", 10));
	// the folders under the one that went: emptied, not left holding their old sizes (Largest folders)
	CHECK(s->dirs == s2->dirs && s->dir[0].dirs == s2->dir[0].dirs);
	CHECK(s->dir[scan_dir_find(s, "GAMES")].dirs == s2->dir[scan_dir_find(s2, "GAMES")].dirs);
	CHECK(s->dir[scan_dir_find(s, "GAMES/Big/PS3_GAME/USRDIR")].bytes == 0 && s->dir[scan_dir_find(s, "GAMES/Big/PS3_GAME/USRDIR/d7")].files == 0);
	for (uint32_t i = 1; i < s->num_dirs; i++) {
		uint32_t x = i;
		while (x && x != (uint32_t)scan_dir_find(s, "GAMES/Big")) x = s->dir[x].parent;
		if (x) CHECK(!s->dir[i].bytes && !s->dir[i].files && !s->dir[i].dirs);
	}
	scan_free(s);
	scan_free(s2);

	// a .bin goes with its .cue
	CHECK(Delete("PSXISO/Old.bin", 0) == 0 && job.files_done == 2 && !Exists("PSXISO/Old.bin") && !Exists("PSXISO/Old.cue"));
	// a link inside a backup: left, never followed (what it points at stays); said so
	CHECK(Delete("GAMES/Linked", 1) == 1 && job.left >= 1 && Exists("../outside/keep.txt") && Exists("GAMES/Linked/out") && !Exists("GAMES/Linked/real.dat"));
	// stopped part way: some gone, the rest there, said so
	plat_thread_start(Cancel_Soon, NULL, 1000, 64, "cancel");
	CHECK(Delete("GAMES/Huge", 1) == 1 && job.files_done >= 500 && job.files_done < 5000 && Exists("GAMES/Huge"));
	// the others
	CHECK(Delete("packages/update.pkg", 0) == 0 && Delete("ROMS/snes/game.smc", 0) == 0 && Delete("tmp/wmtmp/cache.bin", 0) == 0);
	CHECK(!Exists("packages/update.pkg") && !Exists("ROMS/snes/game.smc") && !Exists("tmp/wmtmp/cache.bin") && Exists("ROMS/snes"));
	// refused, and nothing touched
	CHECK(Delete("game/BLUS30443", 1) < 0 && Exists("game/BLUS30443/EBOOT.BIN"));
	CHECK(Delete("PS3ISO", 1) < 0 && Exists("PS3ISO"));
	CHECK(Delete("GAMES/../game", 1) < 0 && Exists("game/BLUS30443"));
	CHECK(Delete("PSXISO/missing.bin", 0) < 0);
	CHECK(Delete("ROMS/snes", 0) < 0 && Exists("ROMS/snes"));		// (a folder said to be a file)
	// leftover installs: game/_INST_* and game/_DEL_* folders, nothing else under game/
	CHECK(Kind("game/_INST_00001", 1) == DEL_LEFTOVER && Kind("game/_DEL_00000002", 1) == DEL_LEFTOVER);
	CHECK(Kind("game/_INST_", 1) == DEL_NO && Kind("game/_INST_x/USRDIR", 1) == DEL_NO && Kind("game/_INST_00001", 0) == DEL_NO);
	// (one an install is still writing: left; settled, it goes. PS3HEN's tmp/installer.active: installing now)
	Mk("game/_INST_00000007/USRDIR/a.dat", 500);
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "%s", "game/_INST_00000007");
	plat_thread_start(Grow_Soon, NULL, 1000, 64, "grow");
	CHECK(del_settled(&job, 400000) == DEL_BUSY && Exists("game/_INST_00000007/USRDIR/a.dat"));
	job.result = 0;
	// settled, but written to just now: an _INST_ waits a day, however it's reached
	CHECK(del_settled(&job, 200000) == DEL_RECENT && Exists("game/_INST_00000007/USRDIR/a.dat"));
	{
		char cmd[1100];		// (its files two days old: it goes)
		snprintf(cmd, sizeof(cmd), "find '%s/game/_INST_00000007' -exec touch -d '2 days ago' {} +", R);
		CHECK(system(cmd) == 0);
	}
	job.result = 0;
	CHECK(del_settled(&job, 200000) == 0 && Delete("game/_INST_00000007", 1) == 0 && !Exists("game/_INST_00000007"));
	// a _DEL_ one (a delete the system didn't finish) needn't wait the day
	Mk("game/_DEL_00000008/x.dat", 10);
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "%s", "game/_DEL_00000008");
	CHECK(del_settled(&job, 100000) == 0 && Delete("game/_DEL_00000008", 1) == 0);
	CHECK(!del_installing(R));
	Mk("tmp/installer.active", 1);
	CHECK(del_installing(R) && Delete("tmp/installer.active", 0) < 0);
	Mk("updater/01/PS3UPDAT.PUP", 600); Mk("updater/01/keep.txt", 3);
	CHECK(Delete("updater/01/PS3UPDAT.PUP", 0) == 0 && !Exists("updater/01/PS3UPDAT.PUP") && Exists("updater/01/keep.txt"));
	CHECK(Delete("updater/01", 1) < 0 && Exists("updater/01"));
	Mk("game/_DEL_00000009/USRDIR/a.dat", 500);
	CHECK(Delete("game/_DEL_00000009", 1) == 0 && !Exists("game/_DEL_00000009") && Exists("game/BLUS30443/EBOOT.BIN"));

	// the recycle bin: a row moved in, put back, moved in again, and its entry deleted for good
	char bin[FSX_PATH], entry[FSX_PATH], id[32];
	CHECK(bin_rel(R, bin, sizeof(bin)) && !strcmp(bin, "HDDAtlas/bin"));
	CHECK(Kind("HDDAtlas/bin/123_1", 1) == DEL_BIN && Kind("HDDAtlas/bin/123_1", 0) == DEL_NO && Kind("HDDAtlas/bin", 1) == DEL_NO);
	CHECK(Kind("HDDAtlas/bin/123_1/x.iso", 0) == DEL_NO && Kind("HDDAtlas/bin/..", 1) == DEL_NO && Kind("HDDAtlas/other", 1) == DEL_NO);
	Mk("PS3ISO/Binned.iso", 4000);
	CHECK(bin_new_entry(R, id, sizeof(id), entry, sizeof(entry)));
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "PS3ISO/Binned.iso");
	job.files_total = 1;
	job.bytes_total = 4000;
	bin_entry_t e = {0};
	snprintf(e.id, sizeof(e.id), "%s", id);
	snprintf(e.rel, sizeof(e.rel), "PS3ISO/Binned.iso");
	e.bytes = 4000;
	e.files = 1;
	e.when = 1000;
	// in the app's order, on a drive that's never had a bin (the 0.9.7 console: every delete refused):
	// its line first, the bin's folder made for it, then the move
	CHECK(system("rm -rf '" R "/HDDAtlas'") == 0 && !Exists("HDDAtlas"));
	CHECK(bin_add(R, &e) && Exists("HDDAtlas/bin/bin.txt"));
	CHECK(del_bin_move(&job, entry) == 0 && job.bytes_done == 4000 && !Exists("PS3ISO/Binned.iso"));
	char inside[FSX_PATH + 32];
	snprintf(inside, sizeof(inside), "%s/Binned.iso", entry);
	CHECK(Exists(inside));
	bin_entry_t *got;
	CHECK(bin_load(R, &got) == 1 && !strcmp(got[0].id, id) && got[0].bytes == 4000 && !strcmp(got[0].rel, "PS3ISO/Binned.iso"));
	free(got);
	CHECK(del_bin_move(&job, entry) < 0);		// (the same entry twice: never)
	Mk("PS3ISO/Binned.iso", 10);
	CHECK(del_restore(R, entry, "PS3ISO/Binned.iso", 0) == -17 && Exists(inside));		// (something's there: not over it)
	CHECK(system("rm -f '" R "/PS3ISO/Binned.iso'") == 0);
	CHECK(del_restore(R, entry, "PS3ISO/Binned.iso", 0) == 0 && Exists("PS3ISO/Binned.iso") && !Exists(inside));
	CHECK(del_restore(R, entry, "game/BLUS30443", 1) < 0 && del_restore(R, "PS3ISO", "PS3ISO/Binned.iso", 0) < 0);	// (only the allowed, from the bin)
	CHECK(del_bin_drop(R, entry) == 0 && !Exists(entry) && bin_remove(R, id) && bin_load(R, &got) == 0);
	CHECK(bin_new_entry(R, id, sizeof(id), entry, sizeof(entry)) && del_bin_move(&job, entry) == 0);
	CHECK(Delete(entry, 1) == 0 && !Exists(entry) && !Exists("PS3ISO/Binned.iso"));		// (for good)
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "game/BLUS30443");
	job.is_dir = 1;
	CHECK(bin_new_entry(R, id, sizeof(id), entry, sizeof(entry)) && del_bin_move(&job, entry) < 0 && Exists("game/BLUS30443/EBOOT.BIN"));

	// the list's only added to: many in and out, written again whole now and then, and the same after
	CHECK(system("rm -rf '" R "/HDDAtlas/bin'") == 0);
	Mk("HDDAtlas/bin/keep", 1);
	CHECK(system("rm -f '" R "/HDDAtlas/bin/keep'") == 0);
	for (int i = 0; i < 100; i++) {
		bin_entry_t x = {0};
		snprintf(x.id, sizeof(x.id), "%d_%d", 1700000000 + i, i);
		snprintf(x.rel, sizeof(x.rel), "PS3ISO/G%d.iso", i);
		x.bytes = 1000 + i;
		x.when = 1700000000 + i;
		CHECK(bin_add(R, &x));
		if (i % 4) CHECK(bin_remove(R, x.id));
	}
	CHECK(bin_load(R, &got) == 25 && !strcmp(got[0].rel, "PS3ISO/G0.iso") && !strcmp(got[24].rel, "PS3ISO/G96.iso") && got[24].bytes == 1096);
	free(got);
	CHECK(plat_file_size(R "/HDDAtlas/bin/bin.txt") < 100 * 60);		// (written again whole: not every line kept)
	// a rewrite stopped between its two steps: bin.txt.new is the list
	CHECK(system("mv '" R "/HDDAtlas/bin/bin.txt' '" R "/HDDAtlas/bin/bin.txt.new'") == 0);
	CHECK(bin_load(R, &got) == 25 && Exists("HDDAtlas/bin/bin.txt") && !Exists("HDDAtlas/bin/bin.txt.new"));
	free(got);
	// a line with a tab or a newline in a path: not written (it'd be read back as something else)
	bin_entry_t bad = {0};
	snprintf(bad.id, sizeof(bad.id), "1_1");
	snprintf(bad.rel, sizeof(bad.rel), "PS3ISO/a\tb.iso");
	CHECK(!bin_add(R, &bad));
	snprintf(bad.id, sizeof(bad.id), "../x");
	snprintf(bad.rel, sizeof(bad.rel), "PS3ISO/a.iso");
	CHECK(!bin_add(R, &bad) && !bin_remove(R, "../x"));

	// tidy: none of the 25 has a folder (lines go); an empty folder goes; a folder with no line is kept, lost
	Mk("HDDAtlas/bin/1700000500_7/Lost.iso", 3000);
	Mk("HDDAtlas/bin/1700000600_8/Folder/a.dat", 100);
	Mk("HDDAtlas/bin/1700000600_8/Folder/sub/b.dat", 200);
	CHECK(system("mkdir -p '" R "/HDDAtlas/bin/1700000700_9'") == 0);
	CHECK(bin_tidy(R) == 28);
	CHECK(bin_load(R, &got) == 2 && !Exists("HDDAtlas/bin/1700000700_9"));
	for (int i = 0; i < 2; i++) {
		CHECK(got[i].lost);
		if (!strcmp(got[i].id, "1700000500_7")) CHECK(!strcmp(got[i].rel, "Lost.iso") && !got[i].is_dir && got[i].bytes == 3000 && got[i].when == 1700000500);
		else CHECK(!strcmp(got[i].rel, "Folder") && got[i].is_dir && got[i].bytes == 300 && got[i].files == 2);
	}
	free(got);
	CHECK(bin_tidy(R) == 0);		// (in step now: nothing more)
	CHECK(del_restore(R, "HDDAtlas/bin/1700000500_7", "Lost.iso", 0) < 0);		// (a lost one has no place to go back to)
	CHECK(Delete("HDDAtlas/bin/1700000500_7", 1) == 0 && bin_remove(R, "1700000500_7") && bin_load(R, &got) == 1);
	free(got);
	// Identical files: before one goes, it's compared with a copy that stays, every byte
	Mk("PS3ISO/Same A.iso", 300000); Mk("PS3ISO/Same B.iso", 300000); Mk("PS3ISO/Diff.iso", 300000); Mk("PS3ISO/Short.iso", 299999);
	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/PS3ISO/Diff.iso", R);
		FILE *f = fopen(p, "r+b");
		CHECK(f && fseek(f, 250001, SEEK_SET) == 0 && fputc('y', f) != EOF);		// (one byte, past where a sample would look)
		fclose(f);
	}
	memset(&job, 0, sizeof(job));
	snprintf(job.root, sizeof(job.root), "%s", R);
	snprintf(job.rel, sizeof(job.rel), "%s", "PS3ISO/Same A.iso");
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Same B.iso");
	CHECK(del_same_check(&job) == 0 && !job.checking);
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Diff.iso");
	CHECK(del_same_check(&job) == DEL_NOT_SAME);
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Short.iso");
	CHECK(del_same_check(&job) == DEL_NOT_SAME);
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Gone.iso");
	CHECK(del_same_check(&job) == DEL_NO_OTHER);
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Same A.iso");
	CHECK(del_same_check(&job) < 0);		// (never against itself)
	job.cancel = 1;
	snprintf(job.same_as, sizeof(job.same_as), "%s", "PS3ISO/Same B.iso");
	CHECK(del_same_check(&job) == 1 && !job.checking);		// (stopped: nothing compared, nothing deleted)
	// and then deleted: only the one, the other copy kept
	CHECK(Delete("PS3ISO/Same A.iso", 0) == 0 && !Exists("PS3ISO/Same A.iso") && Exists("PS3ISO/Same B.iso") && Exists("PS3ISO/Diff.iso"));
	printf("del_test: all PASS\n");
	return 0;
}
