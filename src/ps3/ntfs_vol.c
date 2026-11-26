/*
ntfs_vol.c: what libntfs_ext's own calls don't give, read from its volume
(built with its flags and headers: Makefile.ps3's NTFS_CFLAGS). ntfs_ps3.c is
the only caller, holding its lock.

  hddatlas_ntfs_stat   an entry without following it: ps3ntfs_stat follows a
                       junction or a symbolic link to what it points at, and
                       HDD Atlas never follows links (the scan leaves them)
  hddatlas_ntfs_space  the volume's size and free space; the free clusters are
                       counted from its bitmap only when asked (once a mount:
                       the library keeps the count up to date after), where
                       ps3ntfs_statvfs reads the whole bitmap every call
  hddatlas_usb_*       any USB storage device, 0-127: the library's own 8
                       interfaces (__io_ntfs_usb000-007) are fixed to devices
                       0-7, and the PS3 numbers a drive plugged in again past
                       them (0.9.10 on the console: pulled out and back in, and
                       never seen again). These are its 8 slots, each pointed
                       at whichever device it's given, with its sector calls.
*/
#include <string.h>
#include <sys/stat.h>
#include "ntfsinternal.h"
#include "reparse.h"
#include "storage.h"

bool PS3_NTFS_Startup(u64 id, int fd);
bool PS3_NTFS_ReadSectors(int fd, sec_t sector, sec_t numSectors, void *buffer);
bool PS3_NTFS_WriteSectors(int fd, sec_t sector, sec_t numSectors, const void *buffer);

#define SLOTS	8

// USB storage device n's id, as lv2 numbers them (webMAN's USB_MASS_STORAGE): 0-5 from 0x...0A, 6-127 from 0x...1F
unsigned long long hddatlas_usb_id(int n)
{
	return n < 6 ? 0x010300000000000AULL + (unsigned)n : 0x010300000000001FULL + (unsigned)(n - 6);
}

// 1 if device n is plugged in (the system knows it): its info, nothing read from it
int hddatlas_usb_present(int n)
{
	device_info_t info;
	memset(&info, 0, sizeof(info));
	info.unknown03 = 0x12345678;		// (as the library asks: its "disc-less" hack)
	return sys_storage_get_device_info(hddatlas_usb_id(n), &info) == 0;
}

static u64 slot_dev[SLOTS];		// (the device id each slot reads)

#define SLOT(k) \
	static bool Startup##k(void) { return PS3_NTFS_Startup(slot_dev[k], k); } \
	static bool Inserted##k(void) { device_info_t i; memset(&i, 0, sizeof(i)); i.unknown03 = 0x12345678; return sys_storage_get_device_info(slot_dev[k], &i) == 0; } \
	static bool Read##k(sec_t s, sec_t n, void *b) { return PS3_NTFS_ReadSectors(k, s, n, b); } \
	static bool Write##k(sec_t s, sec_t n, const void *b) { return PS3_NTFS_WriteSectors(k, s, n, b); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
static bool Clear(void) { return true; }
static bool Down(void) { return true; }		// (the device's closed by ntfs_ps3.c: PS3_NTFS_Shutdown(slot))

#define IFACE(k) { ('H' << 24) | ('D' << 16) | ('A' << 8) | ('0' + k), \
	FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_PS3_USB, \
	(FN_MEDIUM_STARTUP)Startup##k, (FN_MEDIUM_ISINSERTED)Inserted##k, (FN_MEDIUM_READSECTORS)Read##k, \
	(FN_MEDIUM_WRITESECTORS)Write##k, (FN_MEDIUM_CLEARSTATUS)Clear, (FN_MEDIUM_SHUTDOWN)Down }
static const DISC_INTERFACE slot_if[SLOTS] = { IFACE(0), IFACE(1), IFACE(2), IFACE(3), IFACE(4), IFACE(5), IFACE(6), IFACE(7) };

// slot k pointed at USB device n: the interface to find its partitions and mount it with
const DISC_INTERFACE *hddatlas_usb_slot(int k, int n)
{
	if (k < 0 || k >= SLOTS) return NULL;
	slot_dev[k] = hddatlas_usb_id(n);
	return &slot_if[k];
}

// "/ntfsN:/a/b" as the library's calls take it, and the volume's; NULL if it isn't mounted
static ntfs_vd *Volume(const char *path, const char **in)
{
	if (path[0] == '/') path++;
	ntfs_vd *vd = ntfsGetVolume(path);
	if (!vd || !vd->vol) return NULL;
	const char *p = ntfsRealPath(path);
	if (!p) return NULL;
	*in = p[0] ? p : "/";
	return vd;
}

// 0 a file, 1 a folder, 2 a link (never followed), -1 not there
int hddatlas_ntfs_stat(const char *path, unsigned long long *size, long long *mtime)
{
	const char *in;
	ntfs_vd *vd = Volume(path, &in);
	int type = -1;
	*size = 0;
	*mtime = 0;
	if (!vd || in[0] != '/') return -1;
	ntfsLock(vd);
	ntfs_inode *ni = ntfs_pathname_to_inode(vd->vol, NULL, in);
	if (ni) {
		struct stat st;
		if ((ni->flags & FILE_ATTR_REPARSE_POINT) && ntfs_possible_symlink(ni)) type = 2;
		else if (ntfsStat(vd, ni, &st) == 0) {
			type = S_ISDIR(st.st_mode) ? 1 : 0;
			*size = type ? 0 : (unsigned long long)st.st_size;
			*mtime = (long long)st.st_mtime;
		}
		ntfsCloseEntry(vd, ni);
	}
	ntfsUnlock(vd);
	return type;
}

// 0 when it's known; count: the free clusters counted again from the bitmap
int hddatlas_ntfs_space(const char *path, int count, unsigned long long *total, unsigned long long *free_bytes)
{
	const char *in;
	ntfs_vd *vd = Volume(path, &in);
	*total = *free_bytes = 0;
	if (!vd) return -1;
	ntfsLock(vd);
	int r = 0;
	if (count && ntfs_volume_get_free_space(vd->vol) < 0) r = -1;
	if (r == 0) {
		s64 fc = vd->vol->free_clusters;
		*total = (unsigned long long)vd->vol->nr_clusters * vd->vol->cluster_size;
		*free_bytes = fc > 0 ? (unsigned long long)fc * vd->vol->cluster_size : 0;
	}
	ntfsUnlock(vd);
	return r;
}
