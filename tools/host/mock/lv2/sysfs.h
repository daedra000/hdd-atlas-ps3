// host mock of PSL1GHT's lv2/sysfs.h, the parts fsx_ps3.c uses (probe_test.c only)
#include <time.h>
#include <ppu-types.h>
#define SYS_O_RDONLY 0
typedef struct { s32 st_mode, st_uid, st_gid; time_t st_atime, st_mtime, st_ctime; u64 st_size, st_blksize; } __attribute__((packed)) sysFSStat;
typedef struct { u8 d_type, d_namlen; char d_name[1025]; } sysFSDirent;
typedef struct { sysFSStat attribute; sysFSDirent entry_name; } sysFSDirectoryEntry;
s32 sysFsOpendir(const char *path, s32 *fd);
s32 sysFsClosedir(s32 fd);
s32 sysFsReaddir(s32 fd, sysFSDirent *ent, u64 *nread);
s32 sysFsStat(const char *path, sysFSStat *st);
s32 sysFsGetDirectoryEntries(s32 fd, sysFSDirectoryEntry *e, u32 size, u32 *count);
s32 sysFsGetFreeSize(const char *path, u32 *block, u64 *blocks);
