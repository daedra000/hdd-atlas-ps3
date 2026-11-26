/*
fsx.h: the little of the filesystem the scanner needs, read-only.

src/ps3/fsx_ps3.c is the console's (lv2 / cellFs through PSL1GHT, and NTFS
drives through src/ps3/ntfs_ps3.c);
tools/host/fsx_host.c is POSIX, for testing the scanner on a PC.
Nothing here writes, renames or deletes.
*/
#ifndef FSX_H
#define FSX_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FSX_PATH	1024		// lv2's longest path (CELL_FS_MAX_FS_PATH_LENGTH)
#define FSX_NAME	256

enum { FSX_FILE, FSX_DIR, FSX_OTHER };		// OTHER: links and the like (never followed)

typedef struct {
	char name[FSX_NAME];
	int type;
	uint64_t size;			// bytes (files)
	uint64_t blksize;		// the filesystem's block size, as stat gives it (0: unknown)
	int64_t mtime;			// last changed (seconds since 1970; 0: unknown)
} fsx_ent_t;

typedef struct fsx_dir fsx_dir_t;

// 0 when it's open; otherwise the system's error (a folder that can't be read)
int fsx_opendir(const char *path, fsx_dir_t **out);
// up to max entries ("." and ".." left out); 0 at the end, < 0 an error part way
int fsx_read(fsx_dir_t *d, fsx_ent_t *out, int max);
void fsx_closedir(fsx_dir_t *d);

// a single path: 0 when it's there
int fsx_stat(const char *path, int *type, uint64_t *size);

// len bytes of a file from offset (64-bit: an ISO can be past 4 GB); bytes read, < 0 an error
int fsx_read_at(const char *path, uint64_t offset, void *buf, int len);

// a drive's name in HDD Atlas's own files (cache_<it>.bin...): "/dev_usb000" dev_usb000,
// "/ntfs0:" ntfs0 (no ":" in a file name)
static inline void fsx_drive_key(const char *root, char *out, int size)
{
	const char *d = strrchr(root, '/');
	d = d ? d + 1 : root;
	int n = (int)strlen(d);
	if (n && d[n - 1] == ':') n--;
	snprintf(out, size, "%.*s", n, d);
}

// a file read from start to end (the byte-for-byte check of identical files), or at offsets
typedef struct fsx_file fsx_file_t;
int fsx_open(const char *path, fsx_file_t **out);		// 0 when it's open
int fsx_fread(fsx_file_t *f, void *buf, int len);		// bytes read (0 at the end), < 0 an error
int fsx_pread(fsx_file_t *f, uint64_t offset, void *buf, int len);	// len bytes from offset (a disc image's sectors): as fsx_read_at, one open for many
void fsx_fclose(fsx_file_t *f);

// the drive path is on (as "/dev_hdd0"): its size and what's free, in bytes (0 when unknown),
// and for the HDD the whole disk's size, every partition (as the XMB gives it; 0 unknown).
// Returns 0 when free is known: a drive that isn't there gives -1. how gets which call gave
// the total ("sys_fs_disk_free"...)
int fsx_space(const char *dev, uint64_t *total, uint64_t *free_bytes, uint64_t *disk, char *how, int how_size);

#endif
