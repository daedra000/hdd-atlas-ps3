/*
fsx_host.c: fsx.h on a PC (POSIX), for tools/host/scan_test.c.

Tests run as root in WSL, where permissions don't stop anything: a folder
named LOCKED stands for one the PS3 won't open (EACCES). HOST_STUCK: a folder
whose path has it in it never answers (a failing drive), till host_unstick.
HOST_EIO: one whose path has it in it gives a read error (bad sectors).
*/
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "../../src/fsx.h"

struct fsx_dir { DIR *d; char path[FSX_PATH]; };

volatile int host_unstick;
int host_opens, host_preads;		// files opened, and reads at offsets in them (a raw .bin: one open, many reads)

static int Locked(const char *path)
{
	const char *b = strrchr(path, '/');
	return b && !strcmp(b + 1, "LOCKED");
}

int fsx_opendir(const char *path, fsx_dir_t **out)
{
	if (getenv("HOST_SLOW")) usleep(1000);		// (a scan slow enough for a shot of it running)
	if (getenv("HOST_STUCK") && strstr(path, getenv("HOST_STUCK"))) while (!host_unstick) usleep(10000);
	*out = NULL;
	if (Locked(path)) return -EACCES;
	if (getenv("HOST_EIO") && strstr(path, getenv("HOST_EIO"))) return -EIO;
	if (strlen(path) >= FSX_PATH) return -ENAMETOOLONG;
	DIR *d = opendir(path);
	if (!d) return -errno;
	fsx_dir_t *x = calloc(1, sizeof(*x));
	x->d = d;
	strcpy(x->path, path);
	*out = x;
	return 0;
}

int fsx_read(fsx_dir_t *d, fsx_ent_t *out, int max)
{
	int n = 0;
	char child[FSX_PATH * 2];
	struct dirent *e;
	if (getenv("HOST_SLOW_READ") && strstr(d->path, getenv("HOST_SLOW_READ"))) usleep(5000);		// (a big folder, listed slowly)
	while (n < max && (e = readdir(d->d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (strlen(e->d_name) >= FSX_NAME) continue;
		fsx_ent_t *o = &out[n++];
		snprintf(o->name, sizeof(o->name), "%s", e->d_name);
		snprintf(child, sizeof(child), "%s/%s", d->path, e->d_name);
		struct stat st;
		if (lstat(child, &st) != 0) { o->type = FSX_OTHER; o->size = o->blksize = 0; o->mtime = 0; continue; }
		o->type = S_ISDIR(st.st_mode) ? FSX_DIR : S_ISREG(st.st_mode) ? FSX_FILE : FSX_OTHER;
		o->size = o->type == FSX_FILE ? (uint64_t)st.st_size : 0;
		o->blksize = st.st_blksize;
		o->mtime = (int64_t)st.st_mtime;
	}
	return n;
}

void fsx_closedir(fsx_dir_t *d) { if (d) { closedir(d->d); free(d); } }

int fsx_stat(const char *path, int *type, uint64_t *size)
{
	struct stat st;
	if (lstat(path, &st) != 0) return -errno;
	if (type) *type = S_ISDIR(st.st_mode) ? FSX_DIR : S_ISREG(st.st_mode) ? FSX_FILE : FSX_OTHER;
	if (size) *size = st.st_size;
	return 0;
}

// HOST_TOTAL / HOST_FREE (bytes) stand in for the console's drive when set
int fsx_space(const char *dev, uint64_t *total, uint64_t *free_bytes, uint64_t *disk, char *how, int how_size)
{
	const char *t = getenv("HOST_TOTAL"), *f = getenv("HOST_FREE"), *d = getenv("HOST_DISK");
	*disk = 0;
	// the USB stick: the fake one scanned (build/host/fake/dev_usb000), and /dev_usb000 when HOST_USB
	// plugs it in, are the same drive, the same size (HOST_USB_TOTAL: another stick in its port)
	size_t dl = strlen(dev);
	int fake_usb = dl >= 10 && !strcmp(dev + dl - 10, "dev_usb000") && strncmp(dev, "/dev_", 5);
	if (fake_usb || (!strcmp(dev, "/dev_usb000") && getenv("HOST_USB") && *getenv("HOST_USB"))) {
		*total = getenv("HOST_USB_TOTAL") ? strtoull(getenv("HOST_USB_TOTAL"), NULL, 10) : 64ULL << 30;
		*free_bytes = 20ULL << 30;
		snprintf(how, how_size, "HOST_USB");
		return 0;
	}
	if (strncmp(dev, "/dev_", 5) && t && f) {		// (the fake drive; a /dev_usb... path isn't there)
		*total = strtoull(t, NULL, 10);
		*free_bytes = strtoull(f, NULL, 10);
		if (d) *disk = strtoull(d, NULL, 10);
		snprintf(how, how_size, "HOST_TOTAL");
		return 0;
	}
	struct statvfs v;
	if (statvfs(dev, &v) != 0) { *total = *free_bytes = 0; return -1; }
	*total = (uint64_t)v.f_blocks * v.f_frsize;
	*free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
	snprintf(how, how_size, "statvfs");
	return 0;
}

int fsx_read_at(const char *path, uint64_t offset, void *buf, int len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	host_opens++;
	ssize_t n = pread(fd, buf, len, (off_t)offset);
	close(fd);
	return n < 0 ? -1 : (int)n;
}

struct fsx_file { FILE *f; };

int fsx_open(const char *path, fsx_file_t **out)
{
	*out = NULL;
	FILE *f = fopen(path, "rb");
	if (!f) return -2;
	host_opens++;
	if (!(*out = malloc(sizeof(**out)))) { fclose(f); return -1; }
	(*out)->f = f;
	return 0;
}

int fsx_fread(fsx_file_t *f, void *buf, int len)
{
	size_t n = fread(buf, 1, len, f->f);
	return n == 0 && ferror(f->f) ? -1 : (int)n;
}

int fsx_pread(fsx_file_t *f, uint64_t offset, void *buf, int len)
{
	host_preads++;
	ssize_t n = pread(fileno(f->f), buf, len, (off_t)offset);
	return n < 0 ? -1 : (int)n;
}

void fsx_fclose(fsx_file_t *f) { if (f) { fclose(f->f); free(f); } }
