/*
report.c: the report and the CSV (report.h). English, for a PC; sizes as the
app shows them (kit_fmt_size: 1024s or 1000s, as set), and the CSV in bytes.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "scan.h"
#include "fsx.h"
#include "ui_kit.h"
#include "version.h"
#include "report.h"

static const scan_t *by;
static int Bigger(const void *a, const void *b)
{
	uint64_t x = by->items[*(const uint32_t *)a].bytes, y = by->items[*(const uint32_t *)b].bytes;
	return x < y ? 1 : x > y ? -1 : 0;
}

// a CSV field: quoted, quotes doubled
// a name as UTF-8 (one the PS3 was given in Windows-1252 too): four at a time, for one line's
static const char *U(const char *s)
{
	static char buf[4][1100];
	static int k;
	k = (k + 1) % 4;
	kit_utf8_clean(s, buf[k], sizeof(buf[k]));
	return buf[k];
}

static void Csv(FILE *f, const char *s)
{
	s = U(s);
	fputc('"', f);
	for (; *s; s++) { if (*s == '"') fputc('"', f); fputc(*s, f); }
	fputc('"', f);
}

int report_write(const scan_t *s, const scan_changes_t *ch, const char *drive_name, const char *dir, char *out, int size)
{
	char base[512], path[600], a[32], b[32], c[32], when[32];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char stamp[32] = "now";
	if (tm) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", tm);
	char dev[32];
	fsx_drive_key(s->root, dev, sizeof(dev));		// (no ":" in a file name: "/ntfs6:" is ntfs6)
	if (!plat_mkdirs(dir)) { plat_log("report: couldn't make %s", dir); return -1; }
	snprintf(base, sizeof(base), "%s/HDDAtlas_%s_%s", dir, dev[0] ? dev : "drive", stamp);
	uint32_t *ix = malloc((s->num_items ? s->num_items : 1) * sizeof(uint32_t));
	if (!ix) return -1;
	for (uint32_t i = 0; i < s->num_items; i++) ix[i] = i;
	by = s;
	qsort(ix, s->num_items, sizeof(uint32_t), Bigger);

	snprintf(path, sizeof(path), "%s.txt", base);
	FILE *f = fopen(path, "w");
	if (!f) { free(ix); plat_log("report: couldn't write %s", path); return -1; }
	time_t st = (time_t)s->when;
	struct tm *stm = localtime(&st);
	when[0] = 0;
	if (stm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", stm);
	uint64_t total = scan_drive_total(s), used = scan_drive_used(s);
	fprintf(f, "%s %s report: %s (%s)\n", APP_NAME, APP_VERSION, drive_name, s->root);
	fprintf(f, "Scanned %s, took %.0f s%s\n", when, s->duration_ms / 1000.0, s->edits ? " (deletes made since)" : "");
	kit_fmt_size_in((long long)total, a, sizeof(a), 0);
	kit_fmt_size_in((long long)used, b, sizeof(b), 0);
	kit_fmt_size_in((long long)s->free_bytes, c, sizeof(c), 0);
	fprintf(f, "Total %s, used %s (%.1f%%), free %s\n", a, b, total ? 100.0 * used / total : 0.0, c);
	fprintf(f, "%llu files in %llu folders, %llu couldn't be read\n\n", (unsigned long long)s->files, (unsigned long long)s->dirs,
		(unsigned long long)s->unreadable);
	uint64_t used_now = scan_drive_used(s);
	if (used_now && s->scanned > used_now + (1ULL << 30))		// (the PS3 HDD structure report: webMAN's hard links)
		fprintf(f, "Files add up to more than is used (by %.2f GB): some are counted twice (webMAN can link a game's files between GAMES and game).\n\n",
			(s->scanned - used_now) / 1073741824.0);
	fprintf(f, "Categories\n");
	for (int k = 0; k < NUM_CATS; k++) {
		kit_fmt_size_in((long long)s->cat_bytes[k], a, sizeof(a), 0);
		fprintf(f, "  %-28s %12s  %5.1f%%", scan_cat_name(k), a, total ? 100.0 * s->cat_bytes[k] / total : 0.0);
		if (k < NUM_SCANNED_CATS) fprintf(f, "  %u items", (unsigned)s->cat_items[k]);
		fputc('\n', f);
	}
	if (ch && ch->count) {
		long long d = (long long)ch->now_total - (long long)ch->was_total;
		kit_fmt_size_in(d < 0 ? -d : d, a, sizeof(a), 0);
		fprintf(f, "\nChanges since the scan before (%c%s found)\n", d < 0 ? '-' : '+', a);
		for (uint32_t i = 0; i < ch->count; i++) {
			const scan_change_t *x = &ch->c[i];
			long long dd = (long long)x->now - (long long)x->was;
			kit_fmt_size_in(dd < 0 ? -dd : dd, a, sizeof(a), 0);
			fprintf(f, "  %c%-12s %-5s %s%s%s\n", dd < 0 ? '-' : '+', a, !x->was_there ? "new" : !x->now_there ? "gone" : "",
				U(x->title[0] ? x->title : x->path), x->title[0] ? "  " : "", x->title[0] ? U(x->path) : "");
		}
	}
	fprintf(f, "\nItems, largest first\n");
	for (uint32_t r = 0; r < s->num_items; r++) {
		const scan_item_t *it = &s->items[ix[r]];
		kit_fmt_size_in((long long)it->bytes, a, sizeof(a), 0);
		fprintf(f, "  %12s  %-8s %s%s%s%s%s\n", a, scan_cat_short(it->cat), U(it->title[0] ? it->title : it->path),
			it->tid[0] ? "  [" : "", it->tid, it->tid[0] ? "]" : "", it->title[0] ? "" : "");
		if (it->title[0]) fprintf(f, "  %12s  %-8s   %s\n", "", "", U(it->path));
	}
	fprintf(f, "\nThe %u largest files\n", (unsigned)s->num_big);
	for (uint32_t i = 0; i < s->num_big; i++) {
		kit_fmt_size_in((long long)s->big[i].bytes, a, sizeof(a), 0);
		fprintf(f, "  %12s  %s\n", a, U(s->big[i].path));
	}
	int ok = fclose(f) == 0;

	snprintf(path, sizeof(path), "%s.csv", base);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "category,path,title,title_id,kind,bytes,files,folders\n");
		for (uint32_t r = 0; r < s->num_items; r++) {
			const scan_item_t *it = &s->items[ix[r]];
			Csv(f, scan_cat_name(it->cat)); fputc(',', f);
			Csv(f, it->path); fputc(',', f);
			Csv(f, it->title); fputc(',', f);
			Csv(f, it->tid); fputc(',', f);
			Csv(f, it->code); fputc(',', f);
			fprintf(f, "%llu,%u,%u\n", (unsigned long long)it->bytes, (unsigned)it->files, (unsigned)it->dirs);
		}
		ok = fclose(f) == 0 && ok;
	} else ok = 0;
	free(ix);
	snprintf(out, size, "%s.txt", base);
	plat_log("report: %s (and .csv) %s", base, ok ? "written" : "NOT written");
	return ok ? 0 : -1;
}
