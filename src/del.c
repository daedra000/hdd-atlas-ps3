/*
del.c: the allow-list and the deleting (del.h).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "plat.h"
#include "lang.h"
#include "del.h"
#include "bin.h"

#define MAX_PARTS	8

static const char *const iso_dirs[] = {"PS3ISO", "PS2ISO", "PSXISO", "PSPISO", "BDISO", "DVDISO"};
// never, whatever's under them (and installed games, media and saves, with their own reason)
static const char *const blocked[] = {"home", "vsh", "mms", "drm", "crash_report", "xmlhost", "hen", "plugins", "updater",
	"savedata", "exdata", "data", "vm"};

static int Ends(const char *s, const char *end)
{
	size_t a = strlen(s), b = strlen(end);
	return a >= b && !strcasecmp(s + a - b, end);
}

// a drive HDD Atlas may delete on (a host test build adds its fake drive)
static int Root_Ok(const char *root)
{
	if (!strcmp(root, "/dev_hdd0")) return 1;
	if (!strncmp(root, "/dev_usb", 8) && strlen(root) == 11 && root[8] == '0' && root[9] == '0' && root[10] >= '0' && root[10] <= '7')
		return 1;
	if (!strncmp(root, "/ntfs", 5) && strlen(root) == 7 && root[5] >= '0' && root[5] <= '7' && root[6] == ':')		// (an NTFS drive: ntfs_ps3.c)
		return 1;
#ifdef DEL_TEST_ROOT
	if (!strcmp(root, DEL_TEST_ROOT)) return 1;
#endif
	return 0;
}

// what webMAN (and prepISO) need to show and mount a game, never deleted from here though they're
// tiny: a game on an NTFS or exFAT drive is listed and mounted from its stub in tmp/wmtmp
// ("Game.iso.ntfs[PS3ISO]"), and a disc image's decryption key (".dkey") sits there or beside it
static int Webman_Keeps(const char *name, char *why, int why_size)
{
	size_t l = strlen(name);
	if (strstr(name, ".ntfs[") && l && name[l - 1] == ']') { snprintf(why, why_size, "%s", N_("webMAN needs this to mount a game from an NTFS drive.")); return 1; }
	if (Ends(name, ".dkey")) { snprintf(why, why_size, "%s", N_("A disc image's decryption key: webMAN needs it to mount it.")); return 1; }
	return 0;
}

// a name that stays where it is: not empty, ".", ".." or holding a "/"
static int Name_Ok(const char *n, size_t len)
{
	if (!len || len >= FSX_NAME) return 0;
	if ((len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) return 0;
	for (size_t i = 0; i < len; i++) if (n[i] == '/' || n[i] == '\\' || (unsigned char)n[i] < 0x20) return 0;
	return 1;
}

// rel split at "/" into parts; -1 if a part isn't a plain name
static int Split(const char *rel, char parts[MAX_PARTS][FSX_NAME])
{
	int n = 0;
	const char *p = rel;
	while (1) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (n == MAX_PARTS || !Name_Ok(p, len)) return -1;
		memcpy(parts[n], p, len);
		parts[n++][len] = 0;
		if (!slash) break;
		p = slash + 1;
	}
	return n;
}

int del_kind(const char *root, const char *rel, int is_dir, char *why, int why_size)
{
	char parts[MAX_PARTS][FSX_NAME];
	why[0] = 0;
	if (!Root_Ok(root) || !rel || strlen(rel) >= FSX_PATH) return DEL_NO;
	int n = Split(rel, parts);
	if (n < 1) return DEL_NO;
	// an entry of the recycle bin: exactly <bin>/<id>, a folder (before anything else: on the HDD it's under game/)
	char bin[FSX_PATH];
	size_t bl;
	if (is_dir && bin_rel(root, bin, sizeof(bin)) && (bl = strlen(bin)) && !strncmp(rel, bin, bl) && rel[bl] == '/' &&
		!strchr(rel + bl + 1, '/')) return DEL_BIN;
	const char *top = parts[0];
	if (n == 2 && is_dir && !strcmp(top, "game") && ((!strncmp(parts[1], "_INST_", 6) && parts[1][6]) || (!strncmp(parts[1], "_DEL_", 5) && parts[1][5])))
		return DEL_LEFTOVER;		// (the system's own leftovers: the XMB never shows them)
	// a system update waiting, updater/<NN>/<name>.pup: the file only, never a folder (PS3HEN makes
	// updater/01 again at each start; the verified-facts report), before the system's folders
	if (n == 3 && !is_dir && !strcmp(top, "updater") && Ends(parts[2], ".pup")) return DEL_FIRMWARE;
	if (!strcmp(top, "game")) { snprintf(why, why_size, "%s", N_("Installed games: delete them from the XMB (it keeps their icons).")); return DEL_NO; }
	if (!strcmp(top, "music") || !strcmp(top, "video") || !strcmp(top, "photo")) {
		snprintf(why, why_size, "%s", N_("Music, video and photos: delete them from the XMB (it keeps a list of them)."));
		return DEL_NO;
	}
	for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
		if (!strcmp(top, blocked[i])) {
			snprintf(why, why_size, "%s", !strcmp(top, "home") || !strcmp(top, "savedata") || !strcmp(top, "exdata")
				? N_("Saves, trophies and licences: delete them from the XMB.") : N_("The system's own: never deleted."));
			return DEL_NO;
		}
	if (n == 1 && !is_dir && Ends(parts[0], ".pkg")) return DEL_PKG;		// (a package at the top: HEN's installer, a copy dropped there)
	if (n == 1) return DEL_NO;		// (a whole top folder: never)
	if (n == 2 && !is_dir)
		for (size_t i = 0; i < sizeof(iso_dirs) / sizeof(iso_dirs[0]); i++)
			if (!strcmp(top, iso_dirs[i])) return Webman_Keeps(parts[1], why, why_size) ? DEL_NO : DEL_ISO;
	if (n == 2 && is_dir && (!strcmp(top, "GAMES") || !strcmp(top, "GAMEZ"))) return DEL_BACKUP;
	// system updates (never for HEN or CFW: installing one removes it): a backed-up disc's, one waiting
	if (n == 3 && is_dir && (!strcmp(top, "GAMES") || !strcmp(top, "GAMEZ")) && !strcmp(parts[2], "PS3_UPDATE")) return DEL_FIRMWARE;
	if (n == 2 && !is_dir && !strcmp(top, "packages") && Ends(parts[1], ".pkg")) return DEL_PKG;
	if (n == 3 && !is_dir && !strcmp(top, "ROMS")) return DEL_ROM;
	if (n == 3 && !strcmp(top, "tmp") && !strcmp(parts[1], "wmtmp")) return !is_dir && Webman_Keeps(parts[2], why, why_size) ? DEL_NO : DEL_CACHE;
	return DEL_NO;
}

const char *del_kind_name(int kind)
{
	static const char *const names[] = {"", "disc image", "folder backup", "package", "ROM", "webMAN cache file", "leftover install",
		"recycle bin entry", "system update"};		// (the log's)
	return kind > 0 && kind <= DEL_FIRMWARE ? names[kind] : "";
}

int del_twin(const char *root, const char *rel, char *out, int size)
{
	static const char *const exts[][2] = {{".bin", ".cue"}, {".cue", ".bin"}, {".iso", ".cue"}, {".img", ".cue"}};
	char why[8], abs[FSX_PATH];
	out[0] = 0;
	if (del_kind(root, rel, 0, why, sizeof(why)) != DEL_ISO) return 0;
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (!Ends(rel, exts[i][0])) continue;
		size_t base = strlen(rel) - strlen(exts[i][0]);
		const char *pick[2] = {exts[i][1], NULL};
		char up[8];
		snprintf(up, sizeof(up), "%s", exts[i][1]);
		for (char *c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
		pick[1] = up;
		for (int k = 0; k < 2; k++) {
			snprintf(out, size, "%.*s%s", (int)base, rel, pick[k]);
			snprintf(abs, sizeof(abs), "%s/%s", root, out);
			int type;
			uint64_t sz;
			if (strcmp(out, rel) && fsx_stat(abs, &type, &sz) == 0 && type == FSX_FILE) return 1;
		}
	}
	out[0] = 0;
	return 0;
}

/*
==================
Deleting
==================
*/
static char row_abs[FSX_PATH];		// (one delete at a time: del_run's)

// path is the row's own, or under it: checked before every call that removes anything
static int Inside(const char *path)
{
	size_t n = strlen(row_abs);
	if (strncmp(path, row_abs, n)) return 0;
	if (path[n] == 0) return 1;
	if (path[n] != '/') return 0;
	return !strstr(path + n, "/../") && !strstr(path + n, "/./") && !Ends(path, "/..") && !Ends(path, "/.");
}

static void Fail(del_job_t *d, const char *path, int r)
{
	if (d->result >= 0) {
		d->result = r < 0 ? r : -r ? -r : -1;
		snprintf(d->err_path, sizeof(d->err_path), "%s", path);
	}
	plat_log("delete: %s: 0x%08x", path, (unsigned)r);
}

static int Unlink(del_job_t *d, const char *path, uint64_t size)
{
	if (!Inside(path)) { Fail(d, path, -1); return 0; }
	int r = fsx_unlink(path);
	if (r != 0) { Fail(d, path, r); d->left++; return 0; }
	d->files_done++;
	d->bytes_done += size;
	return 1;
}

typedef struct { char **v; int n, cap; } dirs_t;

static int Add_Dir(del_job_t *d, dirs_t *ds, const char *path)
{
	if (ds->n == ds->cap) {
		int c = ds->cap ? ds->cap * 2 : 64;
		char **m = realloc(ds->v, c * sizeof(*m));
		if (!m) { Fail(d, path, -1); return 0; }
		ds->v = m;
		ds->cap = c;
	}
	if (!(ds->v[ds->n] = strdup(path))) { Fail(d, path, -1); return 0; }
	ds->n++;
	return 1;
}

typedef struct { char name[FSX_NAME]; uint64_t size; } file_t;

// a folder and everything in it: one folder open at a time, each listed in full and closed before
// its files go (no deleting under an open listing); the folders removed last, deepest first
static void Remove_Tree(del_job_t *d, const char *top)
{
	dirs_t ds = {0};
	fsx_ent_t *ents = malloc(64 * sizeof(fsx_ent_t));
	char child[FSX_PATH];
	if (!ents || !Add_Dir(d, &ds, top)) { if (!ents) Fail(d, top, -1); free(ents); free(ds.v); return; }
	for (int next = 0; next < ds.n && !d->cancel && d->result >= 0; next++) {
		const char *dir = ds.v[next];
		fsx_dir_t *h;
		int r = fsx_opendir(dir, &h);
		if (r != 0) { Fail(d, dir, r); d->left++; continue; }
		file_t *files = NULL;
		int nf = 0, fcap = 0, k, ok = 1;
		while (ok && (k = fsx_read(h, ents, 64)) > 0) {
			for (int i = 0; i < k && ok; i++) {
				if (!Name_Ok(ents[i].name, strlen(ents[i].name)) ||
					snprintf(child, sizeof(child), "%s/%s", dir, ents[i].name) >= (int)sizeof(child)) { d->left++; continue; }
				if (ents[i].type == FSX_DIR) { ok = Add_Dir(d, &ds, child); continue; }
				if (ents[i].type != FSX_FILE) { d->left++; continue; }		// (a link or the like: left, never followed)
				if (nf == fcap) {
					int c = fcap ? fcap * 2 : 64;
					file_t *m = realloc(files, c * sizeof(*m));
					if (!m) { Fail(d, dir, -1); ok = 0; break; }
					files = m;
					fcap = c;
				}
				snprintf(files[nf].name, FSX_NAME, "%s", ents[i].name);
				files[nf++].size = ents[i].size;
			}
		}
		if (ok && k < 0) Fail(d, dir, k);
		fsx_closedir(h);
		for (int i = 0; ok && i < nf && !d->cancel; i++) {
			snprintf(child, sizeof(child), "%s/%s", dir, files[i].name);
			Unlink(d, child, files[i].size);
		}
		free(files);
	}
	// the folders, deepest first (one that isn't empty, because something was left in it, stays)
	for (int i = ds.n - 1; i >= 0 && !d->cancel && d->result >= 0; i--) {
		if (!Inside(ds.v[i])) { Fail(d, ds.v[i], -1); break; }
		int r = fsx_rmdir(ds.v[i]);
		if (r == 0) d->dirs_done++;
		else if (!d->left) { Fail(d, ds.v[i], r); d->left++; }
	}
	for (int i = 0; i < ds.n; i++) free(ds.v[i]);
	free(ds.v);
	free(ents);
}

/*
==================
A leftover install still changing: the verified-facts report's guard (game/_INST_<x> is what the
system unpacks a package into; one an install is still writing mustn't go)
==================
*/
// a folder's files: how many, their sizes and the newest time (-1 when it can't be read through)
static int Tally(const char *top, uint64_t *files, uint64_t *bytes, int64_t *newest)
{
	char **todo = NULL, child[FSX_PATH];
	int n = 0, cap = 0, r = 0;
	fsx_ent_t *ents = malloc(64 * sizeof(fsx_ent_t));
	*files = *bytes = 0;
	*newest = 0;
	if (!ents || !(todo = malloc(sizeof(char *) * (cap = 64))) || !(todo[n++] = strdup(top))) { free(ents); free(todo); return -1; }
	while (n > 0) {
		char *dir = todo[--n];
		fsx_dir_t *h;
		int k;
		if (r == 0 && fsx_opendir(dir, &h) == 0) {
			while (r == 0 && (k = fsx_read(h, ents, 64)) != 0) {
				if (k < 0) { r = -1; break; }
				for (int i = 0; i < k && r == 0; i++) {
					if (ents[i].mtime > *newest) *newest = ents[i].mtime;
					if (ents[i].type == FSX_FILE) { (*files)++; *bytes += ents[i].size; continue; }
					if (ents[i].type != FSX_DIR || !Name_Ok(ents[i].name, strlen(ents[i].name)) ||
						snprintf(child, sizeof(child), "%s/%s", dir, ents[i].name) >= (int)sizeof(child)) continue;
					if (n == cap) {
						char **m = realloc(todo, sizeof(char *) * (cap *= 2));
						if (!m) { r = -1; break; }
						todo = m;
					}
					if (!(todo[n] = strdup(child))) r = -1;
					else n++;
				}
			}
			fsx_closedir(h);
		} else r = -1;
		free(dir);
	}
	free(todo);
	free(ents);
	return r;
}

int del_settled(del_job_t *d, unsigned wait_us)
{
	char path[FSX_PATH];
	uint64_t f1, b1, f2, b2;
	int64_t t1, t2;
	if (snprintf(path, sizeof(path), "%s/%s", d->root, d->rel) >= (int)sizeof(path) || Tally(path, &f1, &b1, &t1) != 0) {
		Fail(d, path, -1);
		return d->result;
	}
	for (unsigned waited = 0; waited < wait_us && !d->cancel; waited += 100000) plat_sleep_us(100000);
	if (d->cancel) { d->result = 1; return 1; }
	if (Tally(path, &f2, &b2, &t2) != 0) { Fail(d, path, -1); return d->result; }
	if (f1 != f2 || b1 != b2 || t1 != t2) {
		d->result = DEL_BUSY;
		snprintf(d->err_path, sizeof(d->err_path), "%s", path);
		plat_log("delete: %s: still changing (%llu -> %llu files, %llu -> %llu bytes): left", path, (unsigned long long)f1,
			(unsigned long long)f2, (unsigned long long)b1, (unsigned long long)b2);
		return d->result;
	}
	// an install's (_INST_) written to in the last day: one going on may still need it, however it's
	// reached (Free up space waits a day too). A clock that's wrong only makes it wait.
	const char *base = strrchr(d->rel, '/');
	int64_t now = (int64_t)time(NULL);
	if (base && !strncmp(base + 1, "_INST_", 6) && t2 > now - 86400) {
		d->result = DEL_RECENT;
		snprintf(d->err_path, sizeof(d->err_path), "%s", path);
		plat_log("delete: %s: written to %lld s ago (under a day): left", path, (long long)(now - t2));
		return d->result;
	}
	return 0;
}

int del_installing(const char *root)
{
	char p[128];
	int type;
	uint64_t size;
	snprintf(p, sizeof(p), "%s/tmp/installer.active", root);		// (PS3HEN's, while it installs a package)
	return fsx_stat(p, &type, &size) == 0;
}

/*
==================
The recycle bin's moves: renames on the one drive, each end checked
==================
*/
static const char *Base_Of(const char *rel)
{
	const char *b = strrchr(rel, '/');
	return b ? b + 1 : rel;
}

// <root>/<rel>, or 0 if it's too long
static int Abs(const char *root, const char *rel, char *out, int size) { return snprintf(out, size, "%s/%s", root, rel) < size; }

static int Exists(const char *path, int *type)
{
	uint64_t size;
	int t;
	if (fsx_stat(path, &t, &size) != 0) return 0;
	if (type) *type = t;
	return 1;
}

// the bin's folders up to the entry's own, made (each must not be anything else already)
static int Make_Entry(const char *root, const char *entry_rel)
{
	char path[FSX_PATH];
	int type;
	size_t n = strlen(root) + 1;
	if (!Abs(root, entry_rel, path, sizeof(path))) return -1;
	for (char *c = path + n; ; c++) {		// (each folder on the way, from the drive's top down)
		int end = !*c;
		if (*c != '/' && !end) continue;
		*c = 0;
		if (!Exists(path, &type)) { int r = fsx_mkdir(path); if (r != 0) return r; }
		else if (type != FSX_DIR) return -1;
		if (end) break;
		*c = '/';
	}
	return 0;
}

static int Move_In(del_job_t *d, const char *rel, const char *entry_abs, int is_dir)
{
	char src[FSX_PATH], dst[FSX_PATH];
	int type;
	if (!Abs(d->root, rel, src, sizeof(src)) || snprintf(dst, sizeof(dst), "%s/%s", entry_abs, Base_Of(rel)) >= (int)sizeof(dst)) return -1;
	if (!Exists(src, &type) || type != (is_dir ? FSX_DIR : FSX_FILE)) return -2;		// (what it is now, not what the screen thought)
	if (Exists(dst, NULL)) return -17;
	return fsx_rename(src, dst);
}

int del_bin_move(del_job_t *d, const char *entry_rel)
{
	char why[160], entry_abs[FSX_PATH];
	d->result = 0;
	d->left = 0;
	d->files_done = d->dirs_done = 0;
	d->bytes_done = 0;
	d->err_path[0] = 0;
	int kind = del_kind(d->root, d->rel, d->is_dir, why, sizeof(why));
	if (kind == DEL_NO || kind == DEL_BIN || (d->is_dir && kind != DEL_BACKUP && kind != DEL_CACHE && kind != DEL_LEFTOVER && kind != DEL_FIRMWARE) ||
		del_kind(d->root, entry_rel, 1, why, sizeof(why)) != DEL_BIN || !Abs(d->root, entry_rel, entry_abs, sizeof(entry_abs)) ||
		Exists(entry_abs, NULL)) {
		plat_log("bin: refused %s/%s into %s", d->root, d->rel, entry_rel);
		d->result = -1;
		return -1;
	}
	int r = Make_Entry(d->root, entry_rel);
	if (r == 0) r = Move_In(d, d->rel, entry_abs, d->is_dir);
	if (r != 0) {
		Fail(d, d->rel, r);
		fsx_rmdir(entry_abs);		// (its folder, empty, made just now: del_kind said it's the bin's)
		return d->result;
	}
	d->files_done = d->files_total;
	d->bytes_done = d->bytes_total;
	if (d->twin[0] && del_kind(d->root, d->twin, 0, why, sizeof(why)) == DEL_ISO && Move_In(d, d->twin, entry_abs, 0) == 0) d->files_done++;
	plat_log("bin: %s/%s into %s", d->root, d->rel, entry_rel);
	return 0;
}

int del_restore(const char *root, const char *entry_rel, const char *rel, int is_dir)
{
	char why[160], src[FSX_PATH], dst[FSX_PATH], parent[FSX_PATH];
	int kind = del_kind(root, rel, is_dir, why, sizeof(why)), type;
	if (del_kind(root, entry_rel, 1, why, sizeof(why)) != DEL_BIN || kind == DEL_NO || kind == DEL_BIN) return -1;
	if (snprintf(src, sizeof(src), "%s/%s/%s", root, entry_rel, Base_Of(rel)) >= (int)sizeof(src) || !Abs(root, rel, dst, sizeof(dst))) return -1;
	if (!Exists(src, &type) || type != (is_dir ? FSX_DIR : FSX_FILE)) return -2;
	if (Exists(dst, NULL)) return -17;		// (something's there now: never over it)
	// its folder (PS3ISO, ROMS/<system>...) made again if it went meanwhile
	snprintf(parent, sizeof(parent), "%s", rel);
	char *slash = strrchr(parent, '/');
	if (slash) {
		*slash = 0;
		char p[FSX_PATH];
		if (!Abs(root, parent, p, sizeof(p))) return -1;
		if (!Exists(p, &type) && Make_Entry(root, parent) != 0) return -1;
	}
	int r = fsx_rename(src, dst);
	plat_log("bin: %s/%s back from %s: 0x%08x", root, rel, entry_rel, (unsigned)r);
	return r;
}

// the entry's folder, once it's empty (what's in it put back)
int del_bin_drop(const char *root, const char *entry_rel)
{
	char why[8], path[FSX_PATH];
	if (del_kind(root, entry_rel, 1, why, sizeof(why)) != DEL_BIN || !Abs(root, entry_rel, path, sizeof(path))) return -1;
	return fsx_rmdir(path);
}

/*
==================
Identical files: before one goes, it's compared with a copy that stays, all of it (the
search compares big files at 16 places only: here nothing's taken on that)
==================
*/
// want bytes, however many reads it takes; fewer only at the end or on an error
static size_t Read_Full(fsx_file_t *f, unsigned char *p, size_t want)
{
	size_t got = 0;
	while (got < want) {
		int r = fsx_fread(f, p + got, (int)(want - got));
		if (r <= 0) break;
		got += (size_t)r;
	}
	return got;
}

int del_same_check(del_job_t *d)
{
	char a[FSX_PATH], b[FSX_PATH];
	int ta, tb;
	uint64_t sa, sb;
	d->result = 0;
	d->bytes_done = 0;
	if (!Abs(d->root, d->rel, a, sizeof(a)) || !Abs(d->root, d->same_as, b, sizeof(b)) || !strcmp(a, b)) { d->result = -1; return -1; }
	if (fsx_stat(b, &tb, &sb) != 0 || tb != FSX_FILE) {
		snprintf(d->err_path, sizeof(d->err_path), "%s", d->same_as);
		plat_log("delete: %s: the other copy %s isn't there: not deleted", d->rel, d->same_as);
		return d->result = DEL_NO_OTHER;
	}
	if (fsx_stat(a, &ta, &sa) != 0 || ta != FSX_FILE) { Fail(d, a, -2); return d->result; }
	if (sa != sb) {
		snprintf(d->err_path, sizeof(d->err_path), "%s", d->same_as);
		plat_log("delete: %s: %llu bytes, %s %llu: not the same", d->rel, (unsigned long long)sa, d->same_as, (unsigned long long)sb);
		return d->result = DEL_NOT_SAME;
	}
	enum { CHUNK = 256 * 1024 };
	unsigned char *x = malloc(CHUNK), *y = malloc(CHUNK);
	fsx_file_t *fa = NULL, *fb = NULL;		// (fsx's: an NTFS drive's files too)
	if (x && y && fsx_open(a, &fa) == 0) fsx_open(b, &fb);
	if (!fa || !fb) {
		fsx_fclose(fa);
		free(x);
		free(y);
		Fail(d, fa ? b : a, -5);
		return d->result;
	}
	d->checking = 1;
	for (uint64_t at = 0; at < sa && !d->result; ) {
		if (d->cancel) { d->result = 1; break; }
		size_t want = sa - at < CHUNK ? (size_t)(sa - at) : CHUNK;
		if (Read_Full(fa, x, want) != want || Read_Full(fb, y, want) != want) { Fail(d, a, -5); break; }
		if (memcmp(x, y, want)) {
			snprintf(d->err_path, sizeof(d->err_path), "%s", d->same_as);
			plat_log("delete: %s differs from %s at byte %llu: not deleted", d->rel, d->same_as, (unsigned long long)at);
			d->result = DEL_NOT_SAME;
			break;
		}
		at += want;
		d->bytes_done = at;
	}
	d->checking = 0;
	fsx_fclose(fa);
	fsx_fclose(fb);
	free(x);
	free(y);
	if (!d->result) plat_log("delete: %s is the same as %s, every byte (%llu)", d->rel, d->same_as, (unsigned long long)sa);
	d->bytes_done = 0;
	return d->result;
}

int del_run(del_job_t *d)
{
	char why[160];
	d->result = 0;
	d->left = 0;
	d->files_done = d->dirs_done = 0;
	d->bytes_done = 0;
	d->err_path[0] = 0;
	int kind = del_kind(d->root, d->rel, d->is_dir, why, sizeof(why));
	if (kind == DEL_NO || (d->is_dir && kind != DEL_BACKUP && kind != DEL_CACHE && kind != DEL_LEFTOVER && kind != DEL_BIN && kind != DEL_FIRMWARE)) {
		plat_log("delete: refused %s/%s", d->root, d->rel);
		d->result = -1;
		return d->result;
	}
	if (snprintf(row_abs, sizeof(row_abs), "%s/%s", d->root, d->rel) >= (int)sizeof(row_abs)) { d->result = -1; return -1; }
	// what it is now, not what the screen thought
	int type;
	uint64_t size;
	int r = fsx_stat(row_abs, &type, &size);
	if (r != 0 || type != (d->is_dir ? FSX_DIR : FSX_FILE)) {
		Fail(d, row_abs, r ? r : -1);
		return d->result;
	}
	plat_log("delete: %s (%s)", row_abs, del_kind_name(kind));
	if (d->is_dir) Remove_Tree(d, row_abs);
	else Unlink(d, row_abs, size);
	// its twin (a .bin's .cue): checked as a row of its own
	if (d->twin[0] && d->result == 0 && !d->cancel) {
		char twin_abs[FSX_PATH];
		if (del_kind(d->root, d->twin, 0, why, sizeof(why)) == DEL_ISO &&
			snprintf(twin_abs, sizeof(twin_abs), "%s/%s", d->root, d->twin) < (int)sizeof(twin_abs) &&
			fsx_stat(twin_abs, &type, &size) == 0 && type == FSX_FILE) {
			snprintf(row_abs, sizeof(row_abs), "%s", twin_abs);
			Unlink(d, twin_abs, size);
		}
	}
	if (d->result == 0 && (d->cancel || d->left)) d->result = 1;
	plat_log("delete: %s: %u files, %u folders, %llu bytes%s%s", d->rel, (unsigned)d->files_done, (unsigned)d->dirs_done,
		(unsigned long long)d->bytes_done, d->cancel ? ", stopped" : "", d->left ? ", some left" : "");
	return d->result;
}
