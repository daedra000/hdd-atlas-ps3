/*
probe_test.c: fsx_ps3.c's probe and batch listing, on the host, against mock
firmware calls that behave each way cellFsGetDirectoryEntries might:
RPCS3's (a size in bytes, one entry a call), all that fits a call, other
layouts, a size that's really a count (must be refused), and refusing.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../src/ps3/fsx_ps3.c"

// (no NTFS drives here: ntfs_ps3.c is the console's)
int ntfsx_path(const char *path) { (void)path; return 0; }
int ntfsx_opendir(const char *path, ntfsx_dir_t **out) { (void)path; *out = NULL; return -1; }
int ntfsx_readdir(ntfsx_dir_t *d, fsx_ent_t *out, int max) { (void)d; (void)out; (void)max; return -1; }
void ntfsx_closedir(ntfsx_dir_t *d) { (void)d; }
int ntfsx_stat(const char *path, int *type, uint64_t *size, int64_t *mtime) { (void)path; (void)type; (void)size; (void)mtime; return -1; }
int ntfsx_space(const char *path, uint64_t *total, uint64_t *free_bytes) { (void)path; (void)total; (void)free_bytes; return -1; }
int ntfsx_open(const char *path, int write, int append) { (void)path; (void)write; (void)append; return -1; }
int ntfsx_read(int fd, void *buf, int len, uint64_t offset) { (void)fd; (void)buf; (void)len; (void)offset; return -1; }
void ntfsx_close(int fd) { (void)fd; }

#define DATA "/dev_hdd0/game/HDAT00001/USRDIR/data"
#define BIG "/big"
#define NBIG 700

// how the mock call behaves
static int m_stride, m_stat, m_bytes, m_cap, m_refuse;
static char data_names[8][32] = {".", "..", "fonts", "glyphs", "backdrop.png", "logo.png", "cards", "licences"};
static int data_dir[8] = {1, 1, 1, 1, 0, 0, 1, 1};
static u64 data_size[8] = {0, 0, 0, 0, 51234, 2201, 0, 0};

typedef struct { int used, big, pos; } mdir_t;
static mdir_t fds[16];

static int Count(int big) { return big ? NBIG + 2 : 8; }
static void Entry(int big, int i, char *name, int *dir, u64 *size)
{
	if (!big) { strcpy(name, data_names[i]); *dir = data_dir[i]; *size = data_size[i]; return; }
	if (i < 2) { strcpy(name, i ? ".." : "."); *dir = 1; *size = 0; return; }
	sprintf(name, "file_%04d.bin", i - 2); *dir = 0; *size = (u64)(i - 2) * 1000 + 7;
}

s32 sysFsOpendir(const char *path, s32 *fd)
{
	int big = !strcmp(path, BIG);
	if (!big && strcmp(path, DATA)) return -1;
	for (int i = 3; i < 16; i++) if (!fds[i].used) { fds[i] = (mdir_t){1, big, 0}; *fd = i; return 0; }
	return -1;
}
s32 sysFsClosedir(s32 fd) { fds[fd].used = 0; return 0; }
s32 sysFsReaddir(s32 fd, sysFSDirent *ent, u64 *nread)
{
	mdir_t *d = &fds[fd];
	*nread = 0;
	if (d->pos >= Count(d->big)) return 0;
	int dir; u64 size;
	Entry(d->big, d->pos++, ent->d_name, &dir, &size);
	ent->d_namlen = strlen(ent->d_name);
	*nread = sizeof(*ent);
	return 0;
}
s32 sysFsStat(const char *path, sysFSStat *st)
{
	const char *nm = strrchr(path, '/') + 1;
	int big = !strncmp(path, BIG "/", strlen(BIG) + 1);
	for (int i = 0; i < Count(big); i++) {
		char name[64]; int dir; u64 size;
		Entry(big, i, name, &dir, &size);
		if (strcmp(name, nm)) continue;
		memset(st, 0, sizeof(*st));
		st->st_mode = dir ? S_DIR | 0777 : S_REG | 0666;
		st->st_size = size; st->st_blksize = 4096;
		return 0;
	}
	return -1;
}
static void Put32(unsigned char *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void Put64(unsigned char *p, u64 v) { Put32(p, v >> 32); Put32(p + 4, (u32)v); }
s32 sysFsGetDirectoryEntries(s32 fd, sysFSDirectoryEntry *e, u32 size, u32 *count)
{
	if (m_refuse) return 0x80010002;
	mdir_t *d = &fds[fd];
	u32 max = m_bytes ? size / m_stride : size, k = 0;
	unsigned char *b = (unsigned char *)e;
	for (; k < max && (!m_cap || k < (u32)m_cap) && d->pos < Count(d->big); k++) {
		unsigned char *p = b + (size_t)k * m_stride;
		char name[64]; int dir; u64 sz;
		Entry(d->big, d->pos++, name, &dir, &sz);
		memset(p, 0, m_stride);
		Put32(p, dir ? S_DIR | 0777 : S_REG | 0666);
		Put64(p + m_stat - 16, sz);
		Put64(p + m_stat - 8, 4096);
		p[m_stat] = dir ? 1 : 2;
		p[m_stat + 1] = strlen(name);
		strcpy((char *)p + m_stat + 2, name);
	}
	*count = k;
	return 0;
}
s32 sysFsGetFreeSize(const char *path, u32 *block, u64 *blocks) { (void)path; *block = 4096; *blocks = 1; return 0; }

const char *plat_data_dir(void) { return DATA; }
void plat_sleep_us(unsigned us) { (void)us; }
s32 sysLv2FsOpen(const char *path, s32 oflags, s32 *fd, u32 mode, const void *arg, u64 argsize) { (void)path; (void)oflags; (void)fd; (void)mode; (void)arg; (void)argsize; return -1; }
s32 sysLv2FsClose(s32 fd) { (void)fd; return 0; }
s32 sysLv2FsRead(s32 fd, void *ptr, u64 len, u64 *read) { (void)fd; (void)ptr; (void)len; (void)read; return -1; }
s32 sysLv2FsLSeek64(s32 fd, u64 offset, s32 dir, u64 *pos) { (void)fd; (void)offset; (void)dir; (void)pos; return -1; }
void plat_log(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	printf("    "); vprintf(fmt, ap); printf("\n");
	va_end(ap);
}

static int fails;

static void Case(const char *what, int stride, int stat, int bytes, int cap, int refuse, int want_stride, int want_stat)
{
	printf("%s\n", what);
	m_stride = stride; m_stat = stat; m_bytes = bytes; m_cap = cap; m_refuse = refuse;
	ge_probed = 0; ge_stride = ge_stat = 0;
	memset(fds, 0, sizeof(fds));
	// listing a big folder (the probe's run by the first opendir); everything must come back right
	fsx_dir_t *d;
	if (fsx_opendir(BIG, &d) != 0) { printf("  FAIL: opendir\n"); fails++; return; }
	static fsx_ent_t ents[64];
	static char seen[NBIG];
	memset(seen, 0, sizeof(seen));
	int total = 0, bad = 0, got;
	while ((got = fsx_read(d, ents, 64)) > 0)
		for (int i = 0; i < got; i++) {
			int k;
			if (sscanf(ents[i].name, "file_%d.bin", &k) != 1 || k < 0 || k >= NBIG || seen[k]++ ||
				ents[i].type != FSX_FILE || ents[i].size != (u64)k * 1000 + 7) bad++;
			total++;
		}
	fsx_closedir(d);
	int pass = ge_stride == want_stride && (!want_stride || ge_stat == want_stat) && total == NBIG && !bad;
	printf("  %s: layout %d/%d (wanted %d/%d), listed %d of %d, %d wrong\n", pass ? "PASS" : "FAIL",
		ge_stride, ge_stat, want_stride, want_stat, total, NBIG, bad);
	if (!pass) fails++;
}

int main(void)
{
	// (one entry a call: which stride can't be seen, and needn't be; asked for 620 bytes, one whole entry comes back)
	Case("RPCS3: a size in bytes, 312-byte entries, one a call", 312, 52, 1, 1, 0, 620, 52);
	Case("one a call, 320-byte entries, 56-byte stat", 320, 56, 1, 1, 0, 620, 56);
	Case("one a call whatever it's told (a count or bytes)", 312, 52, 0, 1, 0, 620, 52);
	Case("a size in bytes, 312-byte entries, all that fit", 312, 52, 1, 0, 0, 312, 52);
	Case("a size in bytes, 310-byte entries (packed)", 310, 52, 1, 0, 0, 310, 52);
	Case("a size in bytes, 320-byte entries, 56-byte stat", 320, 56, 1, 0, 0, 320, 56);
	Case("a size in bytes, PSL1GHT's 1079-byte entries", 1079, 52, 1, 0, 0, 0, 0);
	Case("the size is a count (must not be used)", 312, 52, 0, 0, 0, 0, 0);
	Case("the call refused", 312, 52, 1, 0, 1, 0, 0);
	printf(fails ? "probe_test: %d FAILED\n" : "probe_test: all PASS\n", fails);
	return fails != 0;
}
