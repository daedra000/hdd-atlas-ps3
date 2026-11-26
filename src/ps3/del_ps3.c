/*
del_ps3.c: the only calls in HDD Atlas that remove or move anything (del.h):
lv2's unlink and rmdir, and the recycle bin's rename and mkdir. Only del.c calls
them, after its allow-list and its checks. On an NTFS drive (/ntfsN:) they're
ntfs_ps3.c's, which refuses each on a drive mounted read-only.
*/
#include <ppu-types.h>
#include <sys/file.h>
#include "../del.h"
#include "ntfs_ps3.h"

int fsx_unlink(const char *path) { return ntfsx_path(path) ? ntfsx_unlink(path) : sysLv2FsUnlink(path); }
int fsx_rmdir(const char *path) { return ntfsx_path(path) ? ntfsx_rmdir(path) : sysLv2FsRmdir(path); }
// (and the recycle bin's moves: a rename on one drive, and its folders)
int fsx_rename(const char *from, const char *to) { return ntfsx_path(from) || ntfsx_path(to) ? ntfsx_rename(from, to) : sysLv2FsRename(from, to); }
int fsx_mkdir(const char *path) { return ntfsx_path(path) ? ntfsx_mkdir(path) : sysLv2FsMkdir(path, 0777); }
