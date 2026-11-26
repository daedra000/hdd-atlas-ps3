/*
ntfs_ps3.h: NTFS drives on USB (ntfs_ps3.c, through libntfs_ext).

The system mounts only FAT32 drives (as /dev_usb00N). An NTFS drive is mounted
here as /ntfsN: (N its USB storage device, 0-7) and fsx_ps3.c, del_ps3.c and
main_ps3.c send any path under one to these calls. Each returns what the lv2
call it stands in for would: 0 done, < 0 an error.
*/
#ifndef NTFS_PS3_H
#define NTFS_PS3_H

#include <stdint.h>
#include "../fsx.h"

int ntfsx_path(const char *path);		// 1 if it's /ntfsN: or under it
void ntfsx_start(void);					// the watcher (mounts drives as they're plugged in, every 2 s)
void ntfsx_quit(void);					// every drive unmounted, written out (the app's last step)
int ntfsx_read_only(const char *path);	// mounted read-only (Windows didn't close it cleanly, or hibernated)

typedef struct ntfsx_dir ntfsx_dir_t;
int ntfsx_opendir(const char *path, ntfsx_dir_t **out);
int ntfsx_readdir(ntfsx_dir_t *d, fsx_ent_t *out, int max);
void ntfsx_closedir(ntfsx_dir_t *d);
int ntfsx_stat(const char *path, int *type, uint64_t *size, int64_t *mtime);
int ntfsx_space(const char *path, uint64_t *total, uint64_t *free_bytes);

int ntfsx_open(const char *path, int write, int append);	// a handle (>= 0); write truncates unless append
int ntfsx_read(int fd, void *buf, int len, uint64_t offset);	// bytes read at offset
int ntfsx_write(int fd, const void *buf, int len);
void ntfsx_close(int fd);

int ntfsx_unlink(const char *path);
int ntfsx_rmdir(const char *path);
int ntfsx_rename(const char *from, const char *to);
int ntfsx_mkdir(const char *path);
int ntfsx_replace(const char *from, const char *to);	// from renamed onto to (to deleted first, in the same hold of the lock)

#endif
