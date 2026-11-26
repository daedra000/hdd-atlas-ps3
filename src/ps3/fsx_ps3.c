/*
fsx_ps3.c: fsx.h on the console. Read-only: open, list, stat, free space.

A folder is listed with readdir and a stat per entry (the calls PS3 World
uses). cellFsGetDirectoryEntries would give each entry's size with its name,
in batches, and be quicker; but PSL1GHT's sysFSDirectoryEntry (1,079 bytes:
a 1,025-byte name) isn't the firmware's layout (a 256-byte name), so the
entries it returns can't be read with it. So the first time a folder's
listed, that call is tried on the app's own data folder (whose entries are
known from readdir), every layout it could have is checked against them, and
whether its size argument counts bytes; only a layout that matches exactly is
used from then on. What was found is logged either way.

A path on an NTFS drive (/ntfsN:, which the system doesn't mount) goes to
ntfs_ps3.c instead.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-types.h>
#include <ppu-lv2.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include "../plat.h"
#include "../fsx.h"
#include "ntfs_ps3.h"

#define S_FMT	0170000
#define S_DIR	0040000
#define S_REG	0100000
#define DT_LINK	3		// an entry's d_type when it's a symbolic link (CELL_FS_TYPE_SYMLINK; 1 a folder, 2 a file)

#define PROBE_BUF	65536		// the probe's buffer: far more than the data folder's few entries need, any layout
#define BATCH_BYTES	(32 * 1024)

struct fsx_dir {
	s32 fd;
	char path[FSX_PATH];
	unsigned char *batch;
	ntfsx_dir_t *nd;		// (an NTFS drive's)
};

// the layout cellFsGetDirectoryEntries was found to use (stride 0: not used)
static int ge_stride, ge_stat;		// bytes an entry; bytes of its stat part (52 packed, 56 not)
static int ge_single;
static volatile int probe_lock;		// (a scan and a folder being opened can both be first)				// one entry a call (ge_stride is then the size asked for: one entry, whatever its size)
static volatile int ge_probed;

static int Type_Of(s32 mode)
{
	if ((mode & S_FMT) == S_DIR) return FSX_DIR;
	if ((mode & S_FMT) == S_REG) return FSX_FILE;
	return FSX_OTHER;
}

static int Dot(const char *n) { return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])); }

static u32 Be32(const unsigned char *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 Be64(const unsigned char *p) { return (u64)Be32(p) << 32 | Be32(p + 4); }

// entry i of a batch in the layout found: its name (NULL if it doesn't hold together), type and size
static const char *Ge_Entry(const unsigned char *b, int i, int stride, int st, int *type, u64 *size, u64 *blk, s64 *mtime)
{
	const unsigned char *e = b + (size_t)i * stride;
	int namlen = e[st + 1];
	const char *name = (const char *)e + st + 2;
	if (namlen == 0 || memchr(name, 0, 256) != name + namlen) return NULL;
	*type = e[st] == DT_LINK ? FSX_OTHER : Type_Of((s32)Be32(e));		// (a link: never a file or folder here, whatever its stat says)
	*size = Be64(e + st - 16);		// st_size, then st_blksize, end the stat part either way
	*blk = Be64(e + st - 8);
	*mtime = (s64)Be64(e + st - 32);		// (mode, uid, gid, then atime, mtime, ctime, size, blksize: mtime's 32 from the end)
	return name;
}

#define PROBE_SMALL	620		// the bytes-test size: 2 entries in the smallest layout, 0 in PSL1GHT's
#define CANARY		0xa5

static int Untouched(const unsigned char *p, size_t from, size_t to)
{
	for (size_t i = from; i < to; i++) if (p[i] != CANARY) return 0;
	return 1;
}

static void Probe(void)
{
	static const int strides[] = {310, 312, 314, 316, 320, 1079, 1080};
	static const int stats[] = {52, 56};
	enum { NS = sizeof(strides) / sizeof(strides[0]), NL = 2 * NS };
	static char names[64][FSX_NAME];		// (16 KB: not on a 64 KB thread's stack)
	char child[FSX_PATH];
	u64 sizes[64];
	int types[64], n = 0;
	const char *dir = plat_data_dir();
	s32 fd;
	// what readdir and stat say is there
	if (sysFsOpendir(dir, &fd) != 0) { plat_log("fsx: probe: can't open %s: readdir + stat", dir); return; }
	for (;;) {
		sysFSDirent ent;
		u64 nread = 0;
		if (sysFsReaddir(fd, &ent, &nread) != 0 || nread == 0 || n == 64) break;
		if (Dot(ent.d_name)) continue;
		snprintf(names[n], FSX_NAME, "%s", ent.d_name);
		snprintf(child, sizeof(child), "%s/%s", dir, ent.d_name);
		sysFSStat st;
		if (sysFsStat(child, &st) != 0) continue;
		types[n] = Type_Of(st.st_mode);
		sizes[n] = types[n] == FSX_FILE ? st.st_size : 0;
		n++;
	}
	sysFsClosedir(fd);
	if (n < 3 || n == 64) { plat_log("fsx: probe: %d entries in %s: readdir + stat", n, dir); return; }

	// the buffer's filled with a pattern and the call told PROBE_SMALL bytes: if it's a size in
	// bytes, nothing past them changes. If anything does, the size isn't what it's taken as, and
	// the call's never used (a listing could run past its buffer).
	unsigned char *buf = malloc(PROBE_BUF);
	if (!buf) return;
	memset(buf, CANARY, PROBE_BUF);
	u32 count = 0;
	s32 r = -1;
	if (sysFsOpendir(dir, &fd) == 0) {
		r = sysFsGetDirectoryEntries(fd, (sysFSDirectoryEntry *)buf, PROBE_SMALL, &count);
		sysFsClosedir(fd);
	}
	int kept = Untouched(buf, PROBE_SMALL, PROBE_BUF);
	u32 count_small = count;
	plat_log("fsx: probe: cellFsGetDirectoryEntries(%s, %d bytes) = 0x%08x, %u entries, %s past them (readdir: %d)",
		dir, PROBE_SMALL, (unsigned)r, (unsigned)count, kept ? "nothing written" : "WROTE", n);
	if (r != 0 || count == 0 || count > 2 || !kept) {
		plat_log("fsx: probe: %s: readdir + stat", r ? "refused" : !kept || count > 2 ? "its size isn't in bytes" : "nothing back");
		free(buf);
		return;
	}

	// the whole folder, however many calls it takes (one call needn't give everything that fits),
	// each call's entries decoded in every layout; a layout holds if it names each of readdir's
	// entries exactly once, with the same type and size, and nothing else
	int ok[NL];
	u64 got[NL];		// readdir's entries a layout has matched (a bit each)
	for (int l = 0; l < NL; l++) { ok[l] = 1; got[l] = 0; }
	int calls = 0, total = 0, most = 0;
	if (sysFsOpendir(dir, &fd) != 0) { free(buf); return; }
	for (; calls < 200; calls++) {
		memset(buf, CANARY, PROBE_BUF);
		count = 0;
		r = sysFsGetDirectoryEntries(fd, (sysFSDirectoryEntry *)buf, PROBE_BUF, &count);
		if (r != 0 || count == 0) break;
		total += count;
		if ((int)count > most) most = count;
		for (int l = 0; l < NL; l++) {
			int st = stats[l / NS], stride = strides[l % NS];
			if (!ok[l]) continue;
			if ((u64)count * stride > PROBE_BUF) { ok[l] = 0; continue; }
			for (u32 i = 0; i < count && ok[l]; i++) {
				int type;
				u64 size, blk;
				s64 mt;
				const char *nm = Ge_Entry(buf, i, stride, st, &type, &size, &blk, &mt);
				if (!nm) { ok[l] = 0; break; }
				if (Dot(nm)) continue;
				int m = -1;
				for (int j = 0; j < n; j++) if (!strcmp(names[j], nm)) m = j;
				if (m < 0 || (got[l] >> m & 1) || types[m] != type || (type == FSX_FILE && sizes[m] != size)) ok[l] = 0;
				else got[l] |= 1ULL << m;
			}
		}
	}
	sysFsClosedir(fd);
	free(buf);
	u64 all = (n == 64) ? ~0ULL : (1ULL << n) - 1;
	int found = 0, same_stat = 1;
	for (int l = 0; l < NL; l++)
		if (ok[l] && got[l] == all) {
			if (!found++) { ge_stride = strides[l % NS]; ge_stat = stats[l / NS]; }
			else if (stats[l / NS] != ge_stat) same_stat = 0;
		}
	plat_log("fsx: probe: the whole folder: %d calls, %d entries, at most %d a call (r = 0x%08x)", calls, total, most, (unsigned)r);
	if (found == 1) plat_log("fsx: probe: entries are %d bytes (stat %d) and match readdir: listing with cellFsGetDirectoryEntries", ge_stride, ge_stat);
	else if (found > 1 && same_stat && most == 1 && kept && count_small == 1) {
		// one entry a call, so every stride reads the first the same and which it is can't be
		// seen. It doesn't need to be: PROBE_SMALL bytes gave one entry, so one's more than half
		// of them, and asking for PROBE_SMALL bytes always gives back one whole entry.
		ge_single = 1;
		ge_stride = PROBE_SMALL;
		plat_log("fsx: probe: one entry a call (stat %d), %d layouts read it alike: listing with cellFsGetDirectoryEntries, %d bytes a call",
			ge_stat, found, PROBE_SMALL);
	} else {
		plat_log("fsx: probe: %d layouts matched: readdir + stat", found);
		ge_stride = 0;
	}
}

int fsx_opendir(const char *path, fsx_dir_t **out)
{
	*out = NULL;
	if (ntfsx_path(path)) {
		fsx_dir_t *d = calloc(1, sizeof(*d));
		if (!d) return -1;
		int r = ntfsx_opendir(path, &d->nd);
		if (r != 0) { free(d); return r; }
		d->fd = -1;
		*out = d;
		return 0;
	}
	if (!__atomic_load_n(&ge_probed, __ATOMIC_ACQUIRE)) {
		spin_lock(&probe_lock);
		if (!ge_probed) {
			Probe();
			__atomic_store_n(&ge_probed, 1, __ATOMIC_RELEASE);
		}
		spin_unlock(&probe_lock);
	}
	fsx_dir_t *d = calloc(1, sizeof(*d));
	if (!d) return -1;
	if (strlen(path) >= sizeof(d->path)) { free(d); return -2; }
	strcpy(d->path, path);
	s32 r = sysFsOpendir(path, &d->fd);
	if (r != 0) { free(d); return r; }
	*out = d;
	return 0;
}

static int Read_Batch(fsx_dir_t *d, fsx_ent_t *out, int max)
{
	if (!d->batch && !(d->batch = malloc(BATCH_BYTES))) return -1;
	int fit = ge_single ? 1 : BATCH_BYTES / ge_stride;
	if (fit > max) fit = max;
	for (;;) {
		u32 count = 0;
		s32 r = sysFsGetDirectoryEntries(d->fd, (sysFSDirectoryEntry *)d->batch, (u32)(fit * ge_stride), &count);
		if (r != 0) return r;
		if (count == 0) return 0;
		if ((int)count > fit) {		// (more than fit in what it was given: the layout's wrong, and the folder can't be read this way)
			plat_log("scan: %s: %u entries back, room for %d", d->path, (unsigned)count, fit);
			return -1;
		}
		int n = 0;
		for (u32 i = 0; i < count; i++) {
			int type;
			u64 size, blk;
			s64 mt;
			const char *nm = Ge_Entry(d->batch, i, ge_stride, ge_stat, &type, &size, &blk, &mt);
			if (!nm) { plat_log("scan: an entry in %s didn't decode: skipped", d->path); continue; }
			if (Dot(nm)) continue;
			fsx_ent_t *o = &out[n++];
			snprintf(o->name, sizeof(o->name), "%s", nm);
			o->type = type;
			o->size = type == FSX_FILE ? size : 0;
			o->blksize = blk;
			o->mtime = mt;
		}
		if (n) return n;		// (a batch of only "." and "..": the next)
	}
}

static int Read_Plain(fsx_dir_t *d, fsx_ent_t *out, int max)
{
	int n = 0;
	char child[FSX_PATH];
	while (n < max) {
		sysFSDirent ent;
		u64 nread = 0;
		s32 r = sysFsReaddir(d->fd, &ent, &nread);
		if (r != 0) return n ? n : r;
		if (nread == 0) break;
		if (Dot(ent.d_name)) continue;
		if (ent.d_namlen >= FSX_NAME) { plat_log("scan: name too long, skipped: %s/%.60s...", d->path, ent.d_name); continue; }
		fsx_ent_t *o = &out[n];
		snprintf(o->name, sizeof(o->name), "%s", ent.d_name);
		if (snprintf(child, sizeof(child), "%s/%s", d->path, ent.d_name) >= (int)sizeof(child)) continue;
		sysFSStat st;
		if (sysFsStat(child, &st) != 0) {		// (gone since, or locked: left out)
			o->type = FSX_OTHER;
			o->size = o->blksize = 0;
			o->mtime = 0;
		} else {
			// (a link: never a file or folder here, whatever stat says of what it points at: it's left,
			// never followed, by the scan and by deleting; other d_types go by stat, as before)
			o->type = ent.d_type == DT_LINK ? FSX_OTHER : Type_Of(st.st_mode);
			o->size = o->type == FSX_FILE ? st.st_size : 0;
			o->blksize = st.st_blksize;
			o->mtime = (int64_t)st.st_mtime;
		}
		n++;
	}
	return n;
}

int fsx_read(fsx_dir_t *d, fsx_ent_t *out, int max)
{
	if (d->nd) return ntfsx_readdir(d->nd, out, max);
	return ge_stride ? Read_Batch(d, out, max) : Read_Plain(d, out, max);
}

void fsx_closedir(fsx_dir_t *d)
{
	if (!d) return;
	if (d->nd) ntfsx_closedir(d->nd);
	if (d->fd >= 0) sysFsClosedir(d->fd);
	free(d->batch);
	free(d);
}

int fsx_stat(const char *path, int *type, uint64_t *size)
{
	if (ntfsx_path(path)) return ntfsx_stat(path, type, size, NULL);
	sysFSStat st;
	s32 r = sysFsStat(path, &st);
	if (r != 0) return r;
	if (type) *type = Type_Of(st.st_mode);
	if (size) *size = st.st_size;
	return 0;
}

/*
==================
Space. Free: cellFsGetFreeSize, as PS3 World reads it. The drive's size has no
PSL1GHT call: lv2's sys_fs_disk_free (840) is asked, and the storage driver's
device info (609) is logged beside it, so a hardware test can tell which
matches the XMB's System Information.
==================
*/
static s32 Disk_Free(const char *path, u64 *a, u64 *b)
{
	lv2syscall3(840, (u64)(uintptr_t)path, (u64)(uintptr_t)a, (u64)(uintptr_t)b);
	return_to_user_prog(s32);
}

static s32 Storage_Info(u64 dev, void *info)
{
	lv2syscall2(609, dev, (u64)(uintptr_t)info);
	return_to_user_prog(s32);
}

// the HDD's whole disk (609), asked once: it doesn't change
static u64 Whole_Disk(void)
{
	static int asked;
	static u64 bytes;
	if (asked) return bytes;
	asked = 1;
	u8 info[256] __attribute__((aligned(16)));
	memset(info, 0, sizeof(info));
	s32 r = Storage_Info(0x101000000000007ULL, info);		// the internal HDD
	u64 sectors = 0;
	u32 sector_size = 0;
	memcpy(&sectors, info + 40, 8);
	memcpy(&sector_size, info + 48, 4);
	if (r == 0 && sector_size >= 512 && sector_size <= 65536) bytes = sectors * sector_size;
	plat_log("space: storage device info (609) = 0x%08x, %llu sectors of %u bytes = %llu bytes (the whole disk, every partition)",
		(unsigned)r, (unsigned long long)sectors, (unsigned)sector_size, (unsigned long long)bytes);
	return bytes;
}

int fsx_space(const char *dev, uint64_t *total, uint64_t *free_bytes, uint64_t *disk, char *how, int how_size)
{
	static char logged[12][16];		// the drives logged (once each: the screen asks every few seconds)
	u32 block = 0;
	u64 blocks = 0;
	*total = *free_bytes = *disk = 0;
	if (how_size > 0) how[0] = 0;
	if (ntfsx_path(dev)) {		// (the volume's own count, kept by the library: no call reads the drive)
		if (ntfsx_space(dev, total, free_bytes) != 0) return -1;
		snprintf(how, how_size, "NTFS");
		return 0;
	}
	s32 r = sysFsGetFreeSize(dev, &block, &blocks);
	if (r != 0) return -1;		// (not there: a USB port with nothing in it)
	*free_bytes = (u64)block * blocks;

	u64 a = 0, b = 0;
	s32 r840 = Disk_Free(dev, &a, &b);
	// a total smaller than what's free, or the same as it, isn't the drive's size
	if (r840 == 0 && a > *free_bytes && a > b) {
		*total = a;
		snprintf(how, how_size, "sys_fs_disk_free");
	}
	int seen = 0;
	for (int i = 0; i < 12 && logged[i][0] && !seen; i++) seen = !strcmp(logged[i], dev);
	if (!seen) {
		for (int i = 0; i < 12; i++) if (!logged[i][0]) { snprintf(logged[i], sizeof(logged[i]), "%s", dev); break; }
		plat_log("space %s: cellFsGetFreeSize = 0x%08x, block %u x %llu = %llu bytes free",
			dev, (unsigned)r, (unsigned)block, (unsigned long long)blocks, (unsigned long long)*free_bytes);
		plat_log("space %s: sys_fs_disk_free (840) = 0x%08x, first %llu, second %llu", dev, (unsigned)r840,
			(unsigned long long)a, (unsigned long long)b);
	}
	if (!strcmp(dev, "/dev_hdd0")) *disk = Whole_Disk();
	return 0;
}

int fsx_read_at(const char *path, uint64_t offset, void *buf, int len)
{
	if (ntfsx_path(path)) {
		int h = ntfsx_open(path, 0, 0);
		if (h < 0) return -1;
		int r = ntfsx_read(h, buf, len, offset);
		ntfsx_close(h);
		return r;
	}
	s32 fd;
	u64 pos = 0, got = 0;
	if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) return -1;
	int r = -1;
	if (sysLv2FsLSeek64(fd, offset, 0, &pos) == 0 && pos == offset && sysLv2FsRead(fd, buf, len, &got) == 0) r = (int)got;
	sysLv2FsClose(fd);
	return r;
}

struct fsx_file {
	s32 fd;		// lv2's, or ntfs_ps3.c's handle
	int ntfs;
	uint64_t at;
};

int fsx_open(const char *path, fsx_file_t **out)
{
	fsx_file_t *f = calloc(1, sizeof(*f));
	*out = NULL;
	if (!f) return -1;
	int r;
	if ((f->ntfs = ntfsx_path(path))) r = (f->fd = ntfsx_open(path, 0, 0)) < 0 ? f->fd : 0;
	else r = sysLv2FsOpen(path, SYS_O_RDONLY, &f->fd, 0, NULL, 0);
	if (r != 0) { free(f); return r; }
	*out = f;
	return 0;
}

int fsx_fread(fsx_file_t *f, void *buf, int len)
{
	int r;
	if (f->ntfs) r = ntfsx_read(f->fd, buf, len, f->at);
	else {
		u64 got = 0;
		r = sysLv2FsRead(f->fd, buf, len, &got) == 0 ? (int)got : -1;
	}
	if (r > 0) f->at += r;
	return r;
}

int fsx_pread(fsx_file_t *f, uint64_t offset, void *buf, int len)
{
	int r = -1;
	if (f->ntfs) r = ntfsx_read(f->fd, buf, len, offset);
	else {
		u64 pos = 0, got = 0;
		if (sysLv2FsLSeek64(f->fd, offset, 0, &pos) == 0 && pos == offset && sysLv2FsRead(f->fd, buf, len, &got) == 0) r = (int)got;
	}
	f->at = r > 0 ? offset + r : offset;
	return r;
}

void fsx_fclose(fsx_file_t *f)
{
	if (!f) return;
	if (f->ntfs) ntfsx_close(f->fd);
	else sysLv2FsClose(f->fd);
	free(f);
}
