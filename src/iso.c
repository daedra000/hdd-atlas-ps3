/*
iso.c: disc images' titles (iso.h). Everything read is checked against what
was read: a damaged or encrypted image gives nothing, never a read past a buffer.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "fsx.h"
#include "sfo.h"
#include "iso.h"

#define DIR_MAX		(64 * 1024)		// a directory (or a file) read whole, at most
#define FILE_MAX	(1024 * 1024)	// a file read out of an image (an ICON0), at most

typedef struct {
	fsx_file_t *f;			// open once for the image: a raw .bin's read a sector at a time (opening it for each was
							// a hundred opens an image, slow on USB and NTFS)
	int sector, skip;		// 2048 and 0, or a raw .bin's 2352 and 24 (its data's start in a sector)
} img_t;

static uint32_t Le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// len bytes of the image's data from logical sector lba (a raw image's sector headers stepped over)
static int Read_Data(const img_t *im, uint32_t lba, unsigned char *out, int len)
{
	if (im->sector == 2048) return fsx_pread(im->f, (uint64_t)lba * 2048, out, len) == len;
	for (int done = 0; done < len; lba++) {
		int n = len - done < 2048 ? len - done : 2048;
		if (fsx_pread(im->f, (uint64_t)lba * im->sector + im->skip, out + done, n) != n) return 0;
		done += n;
	}
	return 1;
}

// the entry called name in the directory at lba (size bytes): its lba and size; 1 if found
static int Find(const img_t *im, uint32_t lba, uint32_t size, const char *name, int want_dir, uint32_t *flba, uint32_t *fsize)
{
	if (size > DIR_MAX) size = DIR_MAX;
	unsigned char *d = malloc(size);
	if (!d) return 0;
	int found = 0;
	if (Read_Data(im, lba, d, size)) {
		size_t nl = strlen(name);
		for (uint32_t at = 0; at < size && !found; ) {
			unsigned len = d[at];
			if (!len) { at = (at / 2048 + 1) * 2048; continue; }		// (records don't cross a sector)
			if (at + len > size || len < 34) break;
			const unsigned char *r = d + at;
			unsigned n = r[32];
			if (33 + n <= len && n >= nl && !strncasecmp((const char *)r + 33, name, nl) &&
				(n == nl || r[33 + nl] == ';') && !!(r[25] & 2) == want_dir) {
				*flba = Le32(r + 2);
				*fsize = Le32(r + 10);
				found = 1;
			}
			at += len;
		}
	}
	free(d);
	return found;
}

// a file at dir/name in the image, whole (up to max); NULL if it isn't there
static unsigned char *Read_File_Max(const img_t *im, uint32_t root_lba, uint32_t root_size, const char *dir, const char *name, uint32_t *len, uint32_t max)
{
	uint32_t lba = root_lba, size = root_size;
	if (dir && !Find(im, lba, size, dir, 1, &lba, &size)) return NULL;
	if (!Find(im, lba, size, name, 0, &lba, &size) || !size || size > max) return NULL;
	unsigned char *b = malloc(size + 1);
	if (!b || !Read_Data(im, lba, b, size)) { free(b); return NULL; }
	b[size] = 0;
	*len = size;
	return b;
}

static unsigned char *Read_File(const img_t *im, uint32_t rl, uint32_t rs, const char *dir, const char *name, uint32_t *len)
{
	return Read_File_Max(im, rl, rs, dir, name, len, DIR_MAX);
}

// the image opened, its kind (2048 or raw 2352) and its root directory; 0 if it isn't ISO9660
// (closed again then). Close_Image when it's 1
static int Open_Image(const char *path, img_t *im, uint32_t *rl, uint32_t *rs)
{
	static const int kinds[2][2] = {{2048, 0}, {2352, 24}};
	unsigned char pvd[2048];
	fsx_file_t *f;
	if (fsx_open(path, &f) != 0) return 0;
	for (int k = 0; k < 2; k++) {
		im->f = f;
		im->sector = kinds[k][0];
		im->skip = kinds[k][1];
		if (!Read_Data(im, 16, pvd, 2048) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) continue;
		*rl = Le32(pvd + 156 + 2);
		*rs = Le32(pvd + 156 + 10);
		return 1;
	}
	fsx_fclose(f);
	return 0;
}

static void Close_Image(img_t *im) { fsx_fclose(im->f); im->f = NULL; }

unsigned char *iso_read_file(const char *path, const char *dir, const char *name, uint32_t *len)
{
	img_t im;
	uint32_t rl, rs;
	if (!Open_Image(path, &im, &rl, &rs)) return NULL;
	unsigned char *b = Read_File_Max(&im, rl, rs, dir, name, len, FILE_MAX);
	Close_Image(&im);
	return b;
}

int iso_tid_in(const char *s, char *out, int size)
{
	for (const char *p = s; *p; p++) {
		if (!(isupper((unsigned char)p[0]) && isupper((unsigned char)p[1]) && isupper((unsigned char)p[2]) && isupper((unsigned char)p[3])))
			continue;
		const char *q = p + 4;
		char digits[6];
		int n = 0;
		if (*q == '_' || *q == '-') q++;
		while (n < 5 && (isdigit((unsigned char)*q) || (*q == '.' && n == 3))) { if (*q != '.') digits[n++] = *q; q++; }
		if (n == 5 && !isdigit((unsigned char)*q)) {
			snprintf(out, size, "%.4s%.5s", p, digits);
			return 1;
		}
	}
	return 0;
}

int iso_info(const char *path, char *title, int tsize, char *tid, int idsize, char *code, int csize, int *icon)
{
	img_t im;
	uint32_t rl, rs, len, il, is;
	title[0] = tid[0] = code[0] = 0;
	*icon = 0;
	if (Open_Image(path, &im, &rl, &rs)) {
		static const char *const sfo_dirs[] = {"PS3_GAME", "PSP_GAME"};
		for (int i = 0; i < 2; i++) {
			unsigned char *sfo = Read_File(&im, rl, rs, sfo_dirs[i], "PARAM.SFO", &len);
			if (!sfo) continue;
			sfo_get(sfo, len, "TITLE", title, tsize);
			sfo_get(sfo, len, "TITLE_ID", tid, idsize);
			sfo_get(sfo, len, "CATEGORY", code, csize);
			if (!code[0]) snprintf(code, csize, "%s", i ? "PP" : "DG");
			free(sfo);
			// its icon there too (looked up, not read: the screen reads it when it's shown)
			if (Find(&im, rl, rs, sfo_dirs[i], 1, &il, &is) && Find(&im, il, is, "ICON0.PNG", 0, &il, &is) && is && is <= FILE_MAX) *icon = i + 1;
			Close_Image(&im);
			return 1;
		}
		unsigned char *cnf = Read_File(&im, rl, rs, NULL, "SYSTEM.CNF", &len);
		if (cnf) {
			const char *boot = strstr((const char *)cnf, "BOOT2");
			int ps2 = boot != NULL;
			if (!boot) boot = strstr((const char *)cnf, "BOOT");
			if (boot && iso_tid_in(boot, tid, idsize)) snprintf(code, csize, "%s", ps2 ? "2P" : "1P");
			free(cnf);
		}
		Close_Image(&im);
		if (tid[0]) return 1;
	}
	const char *base = strrchr(path, '/');
	return iso_tid_in(base ? base + 1 : path, tid, idsize);
}
