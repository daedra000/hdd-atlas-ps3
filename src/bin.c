/*
bin.c: the recycle bin's places and its list (bin.h).

bin.txt is only ever added to, a line at a time (and each on the disk before
the next step): what can't be lost to a crash or a power cut part way.

  1 TAB id TAB when TAB bytes TAB files TAB is_dir TAB rel TAB twin [TAB lost]
                       an entry (lost 1: where it was isn't known: rel is its name)
  0 TAB id             that entry's gone (put back, or deleted for good)

Once it's mostly gone entries it's written again whole: to bin.txt.new first,
then that put in bin.txt's place, so one of the two is always all of it (a
bin.txt that's missing is bin.txt.new, put back first).

bin_tidy (on a worker: at start and when a USB drive's plugged in) puts the
list and the folders back in step after anything that stopped part way:
  a line whose folder isn't there     its line goes (a move that never happened)
  an entry's folder that's empty      it goes (made, but nothing moved in)
  a folder no line's for              a line's added: "lost", deletable, not put back
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "plat.h"
#include "settings.h"
#include "bin.h"
#include "del.h"

#define LINE_MAX_	(3 * FSX_PATH)

static plat_mutex_t *lock;		// (the UI thread and the delete lane both write the list)

void bin_init(void) { if (!lock) lock = plat_mutex_new(); }
static void Lock(void) { if (lock) plat_mutex_lock(lock); }
static void Unlock(void) { if (lock) plat_mutex_unlock(lock); }

int bin_days(void) { return setting_get("bin", 7); }

static void List_Path(const char *root, char *out, int size, const char *ext)
{
	char rel[FSX_PATH];
	bin_rel(root, rel, sizeof(rel));
	snprintf(out, size, "%s/%s/bin.txt%s", root, rel, ext);
}

int bin_entry_rel(const char *root, const char *id, char *out, int size)
{
	char rel[FSX_PATH];
	return bin_rel(root, rel, sizeof(rel)) && snprintf(out, size, "%s/%s", rel, id) < size;
}

int bin_new_entry(const char *root, char *id, int idsize, char *entry_rel, int relsize)
{
	static unsigned seq;
	snprintf(id, idsize, "%" PRId64 "_%u", (int64_t)time(NULL), ++seq % 100000);
	return bin_entry_rel(root, id, entry_rel, relsize);
}

// an id is a plain name: a folder of the bin's, and nothing else
static int Id_Ok(const char *id)
{
	return id[0] && !strchr(id, '/') && !strchr(id, '\t') && strcmp(id, ".") && strcmp(id, "..") && strlen(id) < sizeof(((bin_entry_t *)0)->id);
}

// a line split at its tabs (in place); the fields
static int Fields(char *line, char **f, int max)
{
	int n = 0;
	for (char *p = line; n < max; ) {
		f[n++] = p;
		char *tab = strchr(p, '\t');
		if (!tab) break;
		*tab = 0;
		p = tab + 1;
	}
	return n;
}

static int Parse(char **f, int n, bin_entry_t *e)
{
	if (n < 8) return 0;
	memset(e, 0, sizeof(*e));
	snprintf(e->id, sizeof(e->id), "%s", f[1]);
	e->when = strtoll(f[2], NULL, 10);
	e->bytes = strtoull(f[3], NULL, 10);
	e->files = (uint32_t)strtoul(f[4], NULL, 10);
	e->is_dir = atoi(f[5]) != 0;
	snprintf(e->rel, sizeof(e->rel), "%s", f[6]);
	snprintf(e->twin, sizeof(e->twin), "%s", f[7]);
	e->lost = n > 8 && atoi(f[8]) != 0;
	return Id_Ok(f[1]) && e->rel[0];
}

// bin.txt gone and bin.txt.new there: a rewrite stopped between the two; that's the list
static void Recover(const char *root)
{
	char path[FSX_PATH + 64], fresh[FSX_PATH + 64];
	List_Path(root, path, sizeof(path), "");
	List_Path(root, fresh, sizeof(fresh), ".new");
	if (plat_file_size(path) < 0 && plat_file_size(fresh) >= 0) {
		int ok = plat_rename(fresh, path) == 0;
		plat_log("bin: %s missing, %s put in its place: %s", path, fresh, ok ? "ok" : "failed");
	}
}

// the entries (malloc'd), and how many lines the list is; -1 if it's there but couldn't be read
static int Load(const char *root, bin_entry_t **out, int *lines_out)
{
	char path[FSX_PATH + 64];
	size_t len;
	*out = NULL;
	if (lines_out) *lines_out = 0;
	Recover(root);
	List_Path(root, path, sizeof(path), "");
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return plat_file_size(path) > 0 ? -1 : 0;
	int lines = 1, n = 0;
	for (size_t i = 0; i < len; i++) lines += b[i] == '\n';
	bin_entry_t *e = malloc(lines * sizeof(*e));
	if (!e) { free(b); return -1; }
	char *save = NULL, *f[9];
	for (char *line = strtok_r(b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		size_t l = strlen(line);
		if (l && line[l - 1] == '\r') line[l - 1] = 0;
		int nf = Fields(line, f, 9);
		if (!strcmp(f[0], "1")) {
			if (!Parse(f, nf, &e[n])) continue;
			for (int k = 0; k < n; k++) if (!strcmp(e[k].id, e[n].id)) { e[k] = e[n]; n--; break; }		// (the later line's it)
			n++;
		} else if (!strcmp(f[0], "0") && nf >= 2) {
			for (int k = 0; k < n; k++)
				if (!strcmp(e[k].id, f[1])) { memmove(&e[k], &e[k + 1], (n - k - 1) * sizeof(*e)); n--; break; }
		}
	}
	free(b);
	if (lines_out) *lines_out = lines - 1;
	if (!n) { free(e); return 0; }
	*out = e;
	return n;
}

int bin_load(const char *root, bin_entry_t **out)
{
	Lock();
	int n = Load(root, out, NULL);
	Unlock();
	return n < 0 ? 0 : n;
}

static int Line(const bin_entry_t *e, char *out, int size)
{
	if (!Id_Ok(e->id) || strchr(e->rel, '\t') || strchr(e->rel, '\n') || strchr(e->twin, '\t') || strchr(e->twin, '\n')) return 0;
	return snprintf(out, size, "1\t%s\t%" PRId64 "\t%" PRIu64 "\t%u\t%d\t%s\t%s\t%d", e->id, e->when, e->bytes, (unsigned)e->files,
		e->is_dir, e->rel, e->twin, e->lost ? 1 : 0) < size;
}

static int Append(const char *root, const char *line)
{
	char path[FSX_PATH + 64], rel[FSX_PATH], dir[FSX_PATH + 64];
	// the bin's folder first: a drive's first delete writes its line before anything's moved in
	// (0.9.7 on a console that had never had a bin: no folder, no list, every delete refused)
	if (!bin_rel(root, rel, sizeof(rel)) || snprintf(dir, sizeof(dir), "%s/%s", root, rel) >= (int)sizeof(dir) || !plat_mkdirs(dir)) return 0;
	Recover(root);
	List_Path(root, path, sizeof(path), "");
	return plat_append_line(path, line);
}

int bin_add(const char *root, const bin_entry_t *e)
{
	char line[LINE_MAX_];
	if (!Line(e, line, sizeof(line))) return 0;
	Lock();
	int ok = Append(root, line);
	Unlock();
	return ok;
}

// the list written again with only what's in it: bin.txt.new whole, then in bin.txt's place
static void Compact(const char *root)
{
	bin_entry_t *e;
	int lines, n = Load(root, &e, &lines);
	if (n < 0 || lines < 64 || lines < 2 * n + 32) { free(e); return; }		// (not worth it yet)
	char path[FSX_PATH + 64], fresh[FSX_PATH + 64], line[LINE_MAX_];
	size_t cap = (size_t)(n ? n : 1) * (sizeof(line) + 1), len = 0;
	char *b = malloc(cap);
	if (!b) { free(e); return; }
	b[0] = 0;
	for (int i = 0; i < n; i++)
		if (Line(&e[i], line, sizeof(line))) len += snprintf(b + len, cap - len, "%s\n", line);
	free(e);
	List_Path(root, path, sizeof(path), "");
	List_Path(root, fresh, sizeof(fresh), ".new");
	int ok = plat_write_file(fresh, b, len);
	free(b);
	if (!ok) { plat_remove(fresh); return; }
	plat_remove(path);
	ok = plat_rename(fresh, path) == 0;
	plat_log("bin: list of %s written again: %d lines to %d entries (%s)", root, lines, n, ok ? "ok" : "left as bin.txt.new");
}

int bin_remove(const char *root, const char *id)
{
	char line[64];
	if (!Id_Ok(id)) return 0;
	snprintf(line, sizeof(line), "0\t%s", id);
	Lock();
	int ok = Append(root, line);
	if (ok) Compact(root);
	Unlock();
	return ok;
}

uint64_t bin_bytes(const char *root)
{
	bin_entry_t *e;
	int n = bin_load(root, &e);
	uint64_t t = 0;
	for (int i = 0; i < n; i++) t += e[i].bytes;
	free(e);
	return t;
}

/*
==================
Tidy: the list and the folders put back in step (a worker's: it walks folders)
==================
*/
// a folder's size and files, walked with a list on the heap (no recursion); 0 if it couldn't all be read
static int Walk(const char *top, uint64_t *bytes, uint32_t *files)
{
	enum { PENDING = 4096, BATCH = 32 };
	char **todo = malloc(PENDING * sizeof(char *));
	fsx_ent_t *ents = malloc(BATCH * sizeof(fsx_ent_t));
	int n = 0, ok = 1;
	*bytes = 0;
	*files = 0;
	if (!todo || !ents || !(todo[n] = strdup(top))) { free(todo); free(ents); return 0; }
	n++;
	while (n) {
		char *dir = todo[--n];
		fsx_dir_t *d;
		if (fsx_opendir(dir, &d) != 0) { ok = 0; free(dir); continue; }
		int got;
		while ((got = fsx_read(d, ents, BATCH)) > 0) {
			for (int i = 0; i < got; i++) {
				if (ents[i].type == FSX_FILE) { *bytes += ents[i].size; (*files)++; }
				else if (ents[i].type == FSX_DIR) {
					char p[FSX_PATH];
					if (n >= PENDING || snprintf(p, sizeof(p), "%s/%s", dir, ents[i].name) >= (int)sizeof(p) || !(todo[n] = strdup(p))) ok = 0;
					else n++;
				}
			}
		}
		if (got < 0) ok = 0;
		fsx_closedir(d);
		free(dir);
	}
	free(todo);
	free(ents);
	return ok;
}

// what's in an entry's folder: how many, and the first's name and type
static int Inside(const char *path, char *name, int size, int *type)
{
	fsx_dir_t *d;
	fsx_ent_t ents[4];
	int n = 0, got;
	name[0] = 0;
	if (fsx_opendir(path, &d) != 0) return -1;
	while ((got = fsx_read(d, ents, 4)) > 0) {
		if (!n) { snprintf(name, size, "%s", ents[0].name); *type = ents[0].type; }
		n += got;
	}
	fsx_closedir(d);
	return got < 0 ? -1 : n;
}

int bin_tidy(const char *root)
{
	char rel[FSX_PATH], dir[FSX_PATH + 64];
	if (!bin_rel(root, rel, sizeof(rel)) || snprintf(dir, sizeof(dir), "%s/%s", root, rel) >= (int)sizeof(dir)) return 0;
	fsx_dir_t *d;
	if (fsx_opendir(dir, &d) != 0) return 0;		// (no bin on this drive)
	// the bin's folders, all of them: if the listing's cut short, nothing's changed
	int cap = 64, nf = 0, got = 0, fine = 1;
	char (*folder)[32] = malloc(cap * sizeof(*folder));
	fsx_ent_t ents[16];
	while (folder && fine && (got = fsx_read(d, ents, 16)) > 0) {
		for (int i = 0; i < got; i++) {
			if (ents[i].type != FSX_DIR || !Id_Ok(ents[i].name)) continue;
			if (nf == cap) {
				void *more = realloc(folder, 2 * cap * sizeof(*folder));
				if (!more) { fine = 0; break; }
				folder = more;
				cap *= 2;
			}
			snprintf(folder[nf++], sizeof(folder[0]), "%s", ents[i].name);
		}
	}
	if (got < 0) fine = 0;
	fsx_closedir(d);
	if (!folder || !fine) { free(folder); plat_log("bin: %s couldn't all be read: not tidied", dir); return 0; }

	Lock();
	bin_entry_t *e;
	int n = Load(root, &e, NULL), changed = 0;
	Unlock();
	if (n < 0) { free(folder); plat_log("bin: %s's list couldn't be read: not tidied", dir); return 0; }
	// a line whose folder isn't there: it went, or was never made
	for (int i = 0; i < n; i++) {
		int there = 0;
		for (int k = 0; k < nf && !there; k++) there = !strcmp(folder[k], e[i].id);
		if (!there) { plat_log("bin: %s: no folder for it: its line goes", e[i].id); bin_remove(root, e[i].id); changed++; }
	}
	// a folder with no line
	for (int k = 0; k < nf; k++) {
		int listed = 0;
		for (int i = 0; i < n && !listed; i++) listed = !strcmp(folder[k], e[i].id);
		if (listed) continue;
		char path[FSX_PATH + 64], name[FSX_NAME];
		int type = FSX_OTHER, inside;
		snprintf(path, sizeof(path), "%s/%s", dir, folder[k]);
		if ((inside = Inside(path, name, sizeof(name), &type)) < 0) continue;
		if (inside == 0) {
			char entry[FSX_PATH];
			plat_log("bin: %s: empty, no line: it goes", folder[k]);
			if (bin_entry_rel(root, folder[k], entry, sizeof(entry))) del_bin_drop(root, entry);
			changed++;
			continue;
		}
		bin_entry_t a;
		memset(&a, 0, sizeof(a));
		snprintf(a.id, sizeof(a.id), "%s", folder[k]);
		snprintf(a.rel, sizeof(a.rel), "%s", name);
		a.lost = 1;
		a.is_dir = type == FSX_DIR;
		Walk(path, &a.bytes, &a.files);
		// when it went in: its id says (the time, then the count), else now (it's kept from now)
		int64_t t = strtoll(a.id, NULL, 10), now = (int64_t)time(NULL);
		a.when = t > 1000000000 && t <= now ? t : now;
		plat_log("bin: %s: no line (\"%s\", %" PRIu64 " bytes): kept as lost", a.id, name, a.bytes);
		bin_add(root, &a);
		changed++;
	}
	free(e);
	free(folder);
	return changed;
}
