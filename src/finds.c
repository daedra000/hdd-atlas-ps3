/*
finds.c: finds.h. Both walk folders one at a time (a list of folders still to
open, no recursion), as the scanner does.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "plat.h"
#include "fsx.h"
#include "jobs.h"
#include "bin.h"
#include "finds.h"

#define SAME_FILES_MAX	60000			// files of SAME_MIN or more kept at most
#define WHOLE_MAX		(16 * 1024 * 1024)	// up to this, all of a file is compared
#define SAMPLES			16
#define SAMPLE			(64 * 1024)

typedef struct { char **v; int n, cap; } stack_t;

static int Push(stack_t *s, const char *path)
{
	if (s->n == s->cap) {
		int c = s->cap ? s->cap * 2 : 64;
		char **m = realloc(s->v, c * sizeof(*m));
		if (!m) return 0;
		s->v = m;
		s->cap = c;
	}
	return (s->v[s->n] = strdup(path)) ? (s->n++, 1) : 0;
}

static void Drop(stack_t *s)
{
	while (s->n) free(s->v[--s->n]);
	free(s->v);
	s->v = NULL;
	s->cap = 0;
}

/*
==================
Identical files
==================
*/
typedef struct { char *path; uint64_t size, fp; } sfile_t;

static job_t *same_job;
static volatile uint32_t p_listed, p_checked, p_to_check;

// a top folder that's looked in: where you keep things (not installed games, saves, the system's, nor the recycle bin)
static int Looked_In(const char *name, const char *bin_top)
{
	static const char *const never[] = {"game", "home", "savedata", "exdata", "vsh", "mms", "drm", "crash_report", "xmlhost", "hen",
		"plugins", "updater", "data", "vm"};
	for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) if (!strcmp(name, never[i])) return 0;
	return strcmp(name, bin_top) != 0;
}

static uint64_t Fnv(uint64_t h, const unsigned char *p, int n)
{
	for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
	return h;
}

// what's in a file, as a number (all of it up to WHOLE_MAX, else SAMPLES places); 0 if it couldn't be read
static uint64_t Fingerprint(const char *abs, uint64_t size, unsigned char *buf, volatile int *cancel)
{
	uint64_t h = 0xcbf29ce484222325ULL ^ size;
	if (size <= WHOLE_MAX) {
		for (uint64_t at = 0; at < size && !*cancel; ) {
			int n = size - at < 256 * 1024 ? (int)(size - at) : 256 * 1024;
			if (fsx_read_at(abs, at, buf, n) != n) return 0;
			h = Fnv(h, buf, n);
			at += n;
		}
		return h ? h : 1;
	}
	for (int i = 0; i < SAMPLES && !*cancel; i++) {
		uint64_t at = (size - SAMPLE) / (SAMPLES - 1) * i;
		if (fsx_read_at(abs, at, buf, SAMPLE) != SAMPLE) return 0;
		h = Fnv(h, buf, SAMPLE);
	}
	return h ? h : 1;
}

static int By_Size(const void *a, const void *b)
{
	const sfile_t *x = a, *y = b;
	return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}
static int By_Fp(const void *a, const void *b)
{
	const sfile_t *x = a, *y = b;
	return x->fp < y->fp ? -1 : x->fp > y->fp ? 1 : strcmp(x->path, y->path);
}
static int By_Waste(const void *a, const void *b)
{
	const same_group_t *x = a, *y = b;
	uint64_t wx = x->size * (x->n - 1), wy = y->size * (y->n - 1);
	return wx < wy ? 1 : wx > wy ? -1 : 0;
}

static void Same_Run(job_t *j)
{
	const char *root = j->sval;
	same_t *r = calloc(1, sizeof(*r));
	j->result = r;
	if (!r) return;
	snprintf(r->root, sizeof(r->root), "%s", root);
	r->when = (int64_t)time(NULL);
	char bin[FSX_PATH] = "", bin_top[FSX_NAME] = "", child[FSX_PATH];
	bin_rel(root, bin, sizeof(bin));
	snprintf(bin_top, sizeof(bin_top), "%.*s", (int)strcspn(bin, "/"), bin);
	sfile_t *f = NULL;
	int nf = 0, fcap = 0;
	stack_t todo = {0};
	fsx_ent_t *ents = malloc(64 * sizeof(fsx_ent_t));
	unsigned char *buf = malloc(256 * 1024);
	size_t rl = strlen(root) + 1;
	if (!ents || !buf || !Push(&todo, root)) r->stopped = 1;
	// the files, from the folders looked in
	while (todo.n && !j->cancel && !r->stopped) {
		char *dir = todo.v[--todo.n];
		int top = !strcmp(dir, root);
		fsx_dir_t *d;
		if (fsx_opendir(dir, &d) == 0) {
			int k;
			while ((k = fsx_read(d, ents, 64)) > 0 && !r->stopped) {
				for (int i = 0; i < k; i++) {
					if (top && ents[i].type == FSX_DIR && !Looked_In(ents[i].name, bin_top)) continue;
					if (snprintf(child, sizeof(child), "%s/%s", dir, ents[i].name) >= (int)sizeof(child)) continue;
					if (ents[i].type == FSX_DIR) { if (!Push(&todo, child)) r->stopped = 1; continue; }
					if (ents[i].type != FSX_FILE || ents[i].size < SAME_MIN) continue;
					if (nf == SAME_FILES_MAX) { r->stopped = 1; break; }
					if (nf == fcap) {
						int c = fcap ? fcap * 2 : 256;
						sfile_t *m = realloc(f, c * sizeof(*m));
						if (!m) { r->stopped = 1; break; }
						f = m;
						fcap = c;
					}
					if (!(f[nf].path = strdup(child + rl))) { r->stopped = 1; break; }
					f[nf].size = ents[i].size;
					f[nf++].fp = 0;
					p_listed = nf;
				}
			}
			fsx_closedir(d);
		}
		free(dir);
	}
	Drop(&todo);
	r->files_seen = nf;
	// the same sizes, then the same content
	if (nf) qsort(f, nf, sizeof(*f), By_Size);
	uint32_t to_check = 0;
	for (int i = 0; i < nf; ) {
		int e = i;
		while (e < nf && f[e].size == f[i].size) e++;
		if (e - i > 1) to_check += e - i;
		i = e;
	}
	p_to_check = to_check;
	r->path = malloc((nf ? nf : 1) * sizeof(char *));
	r->g = malloc((nf / 2 + 1) * sizeof(same_group_t));
	if (!r->path || !r->g) r->stopped = 1;
	for (int i = 0; i < nf && !j->cancel && r->path && r->g; ) {
		int e = i;
		while (e < nf && f[e].size == f[i].size) e++;
		if (e - i > 1) {
			for (int k = i; k < e && !j->cancel; k++) {
				char abs[FSX_PATH];
				snprintf(abs, sizeof(abs), "%s/%s", root, f[k].path);
				f[k].fp = Fingerprint(abs, f[k].size, buf, &j->cancel);
				p_checked++;
			}
			qsort(f + i, e - i, sizeof(*f), By_Fp);
			for (int a = i; a < e; ) {
				int b = a;
				while (b < e && f[b].fp == f[a].fp) b++;
				if (b - a > 1 && f[a].fp) {
					same_group_t *g = &r->g[r->ng++];
					g->size = f[a].size;
					g->first = r->npath;
					g->n = b - a;
					for (int k = a; k < b; k++) { r->path[r->npath++] = f[k].path; f[k].path = NULL; }
				}
				a = b;
			}
		}
		i = e;
	}
	for (int i = 0; i < nf; i++) free(f[i].path);
	free(f);
	free(ents);
	free(buf);
	if (j->cancel) r->stopped = 1;
	if (r->ng) qsort(r->g, r->ng, sizeof(*r->g), By_Waste);
	plat_log("identical: %s: %u files of 1 MB or more, %d groups%s", root, (unsigned)r->files_seen, r->ng, r->stopped ? " (cut short)" : "");
	j->ok = 1;
}

static void Same_Cleanup(job_t *j) { same_free(j->result); j->result = NULL; }

void same_free(same_t *s)
{
	if (!s) return;
	for (uint32_t i = 0; i < s->npath; i++) free(s->path[i]);
	free(s->path);
	free(s->g);
	free(s);
}

int same_start(const char *root)
{
	if (same_job) return 0;
	same_job = job_new(Same_Run, Same_Cleanup);
	if (!same_job) return 0;
	snprintf(same_job->sval, sizeof(same_job->sval), "%s", root);
	p_listed = p_checked = p_to_check = 0;
	job_post(same_job, LANE_LONG);
	return 1;
}

int same_running(void) { return same_job != NULL; }

void same_progress(uint32_t *listed, uint32_t *checked, uint32_t *to_check)
{
	*listed = p_listed;
	*checked = p_checked;
	*to_check = p_to_check;
}

same_t *same_take(void)
{
	if (!same_job || !job_done(same_job)) return NULL;
	same_t *r = same_job->result;
	same_job->result = NULL;
	job_release(same_job);
	same_job = NULL;
	return r;
}

void same_shutdown(void)
{
	if (!same_job) return;
	jobs_cancel_lane(LANE_LONG);
	job_release(same_job);		// (stops between files; frees what it found)
	same_job = NULL;
}

/*
==================
A game's parts
==================
*/
typedef struct {
	char root[64], tid[12];
	char **dirs;
	int *hg, n;
	parts_t out;
} parts_arg_t;

static job_t *parts_job;

// DLC: .edat/.sdat, but not the ones that are the game itself (psdevwiki's EDAT files: a PS1 or PS2
// Classic's ISO.BIN.EDAT, a Minis' or PSP Remaster's MINIS.EDAT and MINIS2.EDAT)
static int Is_Dlc(const char *name)
{
	size_t n = strlen(name);
	if (!strncasecmp(name, "ISO.BIN.", 8) || !strncasecmp(name, "MINIS", 5)) return 0;
	return n > 5 && (!strcasecmp(name + n - 5, ".edat") || !strcasecmp(name + n - 5, ".sdat"));
}

// everything under a folder: added to the game (hg) or its updates, its .edat/.sdat to the DLC
static void Add_Up(parts_arg_t *a, const char *top, int hg, fsx_ent_t *ents, volatile int *cancel)
{
	stack_t todo = {0};
	char child[FSX_PATH];
	if (!Push(&todo, top)) return;
	while (todo.n && !*cancel) {
		char *dir = todo.v[--todo.n];
		fsx_dir_t *d;
		if (fsx_opendir(dir, &d) == 0) {
			int k;
			while ((k = fsx_read(d, ents, 64)) > 0)
				for (int i = 0; i < k; i++) {
					if (snprintf(child, sizeof(child), "%s/%s", dir, ents[i].name) >= (int)sizeof(child)) continue;
					if (ents[i].type == FSX_DIR) Push(&todo, child);
					else if (ents[i].type == FSX_FILE) {
						if (Is_Dlc(ents[i].name)) a->out.dlc += ents[i].size;
						else if (hg) a->out.game += ents[i].size;
						else a->out.updates += ents[i].size;
					}
				}
			fsx_closedir(d);
		}
		free(dir);
	}
	Drop(&todo);
}

static void Parts_Run(job_t *j)
{
	parts_arg_t *a = j->arg;
	char path[FSX_PATH];
	fsx_ent_t *ents = malloc(64 * sizeof(fsx_ent_t));
	if (!ents) return;
	for (int i = 0; i < a->n && !j->cancel; i++) {
		snprintf(path, sizeof(path), "%s/%s", a->root, a->dirs[i]);
		Add_Up(a, path, a->hg[i], ents, &j->cancel);
	}
	// its licences: home/<user>/exdata/ files with its ID in their name (EP9000-BLES01234_00-...)
	snprintf(path, sizeof(path), "%s/home", a->root);
	fsx_dir_t *h;
	if (!j->cancel && fsx_opendir(path, &h) == 0) {
		char users[16][FSX_NAME];
		int nu = 0, k;
		while ((k = fsx_read(h, ents, 64)) > 0)
			for (int i = 0; i < k && nu < 16; i++) if (ents[i].type == FSX_DIR) snprintf(users[nu++], FSX_NAME, "%s", ents[i].name);
		fsx_closedir(h);
		for (int u = 0; u < nu; u++) {
			fsx_dir_t *x;
			snprintf(path, sizeof(path), "%s/home/%s/exdata", a->root, users[u]);
			if (fsx_opendir(path, &x) != 0) continue;
			while ((k = fsx_read(x, ents, 64)) > 0)
				for (int i = 0; i < k; i++) if (ents[i].type == FSX_FILE && strstr(ents[i].name, a->tid)) a->out.licences += ents[i].size;
			fsx_closedir(x);
		}
	}
	free(ents);
	j->ok = 1;
}

static void Parts_Cleanup(job_t *j)
{
	parts_arg_t *a = j->arg;
	if (!a) return;
	for (int i = 0; i < a->n; i++) free(a->dirs[i]);
	free(a->dirs);
	free(a->hg);
	free(a);
	j->arg = NULL;
}

int parts_start(const char *root, const char *tid, const char *const *dirs, const int *hg, int n)
{
	if (parts_job) { job_release(parts_job); parts_job = NULL; }		// (another game's: let go of)
	parts_arg_t *a = calloc(1, sizeof(*a));
	if (!a) return 0;
	a->dirs = calloc(n ? n : 1, sizeof(char *));
	a->hg = calloc(n ? n : 1, sizeof(int));
	snprintf(a->root, sizeof(a->root), "%s", root);
	snprintf(a->tid, sizeof(a->tid), "%s", tid);
	if (!a->dirs || !a->hg) { free(a->dirs); free(a->hg); free(a); return 0; }
	for (int i = 0; i < n; i++) if ((a->dirs[a->n] = strdup(dirs[i]))) a->hg[a->n++] = hg[i];
	parts_job = job_new(Parts_Run, Parts_Cleanup);
	if (!parts_job) { job_t tmp = {0}; tmp.arg = a; Parts_Cleanup(&tmp); return 0; }
	parts_job->arg = a;
	job_post(parts_job, LANE_API);
	return 1;
}

int parts_poll(parts_t *out)
{
	if (!parts_job) return -1;
	if (!job_done(parts_job)) return 0;
	*out = ((parts_arg_t *)parts_job->arg)->out;
	job_release(parts_job);
	parts_job = NULL;
	return 1;
}

void parts_shutdown(void)
{
	if (parts_job) job_release(parts_job);
	parts_job = NULL;
}
