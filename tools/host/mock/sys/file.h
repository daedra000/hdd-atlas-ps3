// host mock of PSL1GHT's sys/file.h, the lv2 calls fsx_ps3.c uses (probe_test.c only)
#include <ppu-types.h>
s32 sysLv2FsOpen(const char *path, s32 oflags, s32 *fd, u32 mode, const void *arg, u64 argsize);
s32 sysLv2FsClose(s32 fd);
s32 sysLv2FsRead(s32 fd, void *ptr, u64 len, u64 *read);
s32 sysLv2FsLSeek64(s32 fd, u64 offset, s32 dir, u64 *pos);
