/*
ntfs_ps3.c: NTFS drives on USB, through libntfs_ext (ntfs_ps3.h).

The watcher (a thread of its own, every 2 s) looks at every USB storage device
the system knows (0-127: a drive plugged in again gets a new number). One that's
been there for two passes (so the system's had time to mount a FAT32 drive
first) is looked at: its first NTFS partition, if it has one, is mounted in the
first free slot as /ntfsN: (N 0-7, the slot; ntfs_vol.c's interfaces). Nothing's
written to find out: the partition table and boot sectors are only read, and a
drive with no NTFS partition is let go (looked at again only if it couldn't be
read, up to 5 times, or once it's been plugged in again). A volume that mounts
but whose free space or top folder can't then be read is let go and tried again
(0.9.11 at start: mounted with 0 GiB free, its top unreadable; fine once plugged
in again).

  mounted read and write    a volume Windows closed cleanly
  mounted read-only         one it didn't (pulled out without "safely remove",
                            or Windows hibernated or fast-started on it): it's
                            scanned, but nothing's deleted (ntfsx_read_only)

Every call into the library holds one lock: the library keeps its errno and a
current folder per volume, and the watcher mounts and unmounts. Whether a
mounted drive's still there is asked without it (a call can hold it a while on
a drive that's going), and as soon as a call on it fails: once it's gone, every
call on it fails at once, its device is closed (the library's reads stop
retrying it), and it's unmounted once nothing has a folder or file of it open,
so a listing part way never reads a volume that's been freed. Its space is kept
by the watcher, so the screen never waits on the lock for it. At the end
(ntfsx_quit) each is unmounted, which writes out what the library has cached.

A call on a drive that's stopped answering can hold the lock for good (a scan
given up on), and a mount holds it a few seconds. So the screen's thread waits
for it at most a second, and its call then fails (E_BUSY): the app doesn't
freeze behind a drive. The watcher waits a second too, and tries again next pass
(it still sees a drive pulled out meanwhile: that's asked without the lock).
A drive plugged in isn't mounted while other NTFS drives are being read (a scan,
a delete): mounting holds the lock, and they'd wait on it.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ntfs.h>
#include <sys/thread.h>
#include "../plat.h"
#include "ntfs_ps3.h"

extern int partition_type[];		// (ntfs.c: what ntfsFindPartitions found, 0 NTFS, 1 ext2/3/4)
bool PS3_NTFS_Shutdown(int fd);
int hddatlas_ntfs_stat(const char *path, unsigned long long *size, long long *mtime);
int hddatlas_ntfs_space(const char *path, int count, unsigned long long *total, unsigned long long *free_bytes);
unsigned long long hddatlas_usb_id(int n);
int hddatlas_usb_present(int n);
const DISC_INTERFACE *hddatlas_usb_slot(int k, int n);

#define PORTS		8		// the slots: /ntfs0: to /ntfs7:
#define DEVICES		128		// USB storage devices the system numbers
#define TRIES		5		// a drive that couldn't be read, looked at again (2 s apart)
#define MAX_FILES	16
#define E_GONE		-19		// (ENODEV: not mounted, or pulled out)
#define E_RO		-30		// (EROFS: mounted read-only)
#define E_BUSY		-16		// (EBUSY: the screen's call, the lock held by another past UI_WAIT_US)
#define UI_WAIT_US	1000000		// the screen's thread waits this long for the lock, at most
#define UI_RETRY_US	50000		// and only this long for 5 s after it last didn't get it (one action makes
								// several calls: each waiting a second would add up)
#define UI_BUSY_US	5000000
#define WATCH_WAIT_US	1000000	// and the watcher, each pass
#define QUIET_US	3000000		// a drive's mounted once no other call's been made for this long

static struct {
	int mounted, read_only;
	volatile int gone;	// pulled out while mounted: every call fails; unmounted when users is 0
	int dev;			// the USB device in it
	int users;			// folders and files open on it
	volatile uint64_t total, free_bytes;		// (the watcher's, each pass: read without the lock)
} port[PORTS];
static unsigned char seen[DEVICES], tries[DEVICES], present[DEVICES], waited[DEVICES];

static struct { int fd, port; } files[MAX_FILES];
static plat_mutex_t *lock;
static volatile int stopping, started;
static sys_ppu_thread_t ui_thread;		// the screen's (ntfsx_start's caller)
static int ui_known;
static volatile uint64_t last_call;		// when a call other than the watcher's last had the lock
static int busy_logged;
static uint64_t ui_busy_until;		// (the screen's thread's: till then it hardly waits)

struct ntfsx_dir {
	DIR_ITER *it;
	int port;
	char path[FSX_PATH];
};

int ntfsx_path(const char *p)
{
	return p && p[0] == '/' && !strncmp(p + 1, "ntfs", 4) && p[5] >= '0' && p[5] < '0' + PORTS && p[6] == ':' && (!p[7] || p[7] == '/');
}

static int Port_Of(const char *p) { return p[5] - '0'; }

// the library's form of a path: the volume's top needs its "/" ("/ntfs0:" is "/ntfs0:/")
static const char *Lib(const char *p, char *buf, int size)
{
	if (p[7]) return p;
	snprintf(buf, size, "%s/", p);
	return buf;
}

static int On_Ui(void)
{
	sys_ppu_thread_t me;
	return ui_known && sysThreadGetId(&me) == 0 && me == ui_thread;
}

// the lock, for a call: 1 when it's held. A worker waits for it; the screen's thread only UI_WAIT_US
// (0 then: the call fails with E_BUSY, and the screen goes on)
static int Lock(void)
{
	if (!lock) return 1;
	if (On_Ui()) {
		uint64_t now = plat_time_us();
		unsigned wait = now < ui_busy_until ? UI_RETRY_US : UI_WAIT_US;
		if (!plat_mutex_lock_for(lock, wait)) {
			if (busy_logged++ < 20) plat_log("ntfs: a drive call held the lock over %.2f s: the screen's call fails (busy)", wait / 1e6);
			ui_busy_until = plat_time_us() + UI_BUSY_US;
			return 0;
		}
		ui_busy_until = 0;
	} else plat_mutex_lock(lock);
	last_call = plat_time_us();
	return 1;
}
// the lock, for closing what was opened: a worker waits however long; the screen's thread as long as
// Lock's (0: it's left open, its slot used, rather than the screen waiting for good on a stuck call)
static int Lock_Close(void)
{
	if (Lock()) return 1;
	plat_log("ntfs: a folder or file the screen opened is left open (a drive call's holding the lock)");
	return 0;
}
// the watcher's: WATCH_WAIT_US at most (0: not this pass), and not counted as a call
static int Lock_Watch(void) { return !lock || plat_mutex_lock_for(lock, WATCH_WAIT_US); }
static void Unlock(void) { if (lock) plat_mutex_unlock(lock); }

// mounted and still plugged in
static int Usable(int n) { return port[n].mounted && !port[n].gone; }

static void Name(int n, char *out) { snprintf(out, 8, "ntfs%d", n); }

// (any thread, no lock) a mounted drive that isn't there any more: from now on every call on it
// fails at once, and its device is closed so a read the library's making gives up
static int Check_Gone(int n)
{
	if (!port[n].mounted || port[n].gone) return port[n].gone;
	if (hddatlas_usb_present(port[n].dev)) return 0;
	port[n].gone = 1;
	PS3_NTFS_Shutdown(n);
	plat_log("ntfs: /ntfs%d: USB device %d pulled out", n, port[n].dev);
	return 1;
}

static void Unmount(int n)
{
	char name[8];
	Name(n, name);
	ntfsUnmount(name, true);
	PS3_NTFS_Shutdown(n);
	plat_log("ntfs: /%s: unmounted%s", name, port[n].gone ? " (pulled out)" : "");
	port[n].mounted = port[n].read_only = port[n].gone = 0;
	port[n].dev = -1;
	port[n].total = port[n].free_bytes = 0;
}

// USB device d into slot n; 1 when that's settled (mounted, or nothing to mount), 0 to try again
static int Mount(int n, int d)
{
	char name[8], root[16];
	sec_t *parts = NULL;
	Name(n, name);
	snprintf(root, sizeof(root), "/%s:/", name);
	const DISC_INTERFACE *dif = hddatlas_usb_slot(n, d);
	PS3_NTFS_Shutdown(n);		// (the slot's last device closed, if it wasn't)
	int np = ntfsFindPartitions(dif, &parts);
	int j = 0;
	while (j < np && partition_type[j]) j++;		// (the first NTFS one: ext2/3/4 isn't looked at)
	int ok = 0, ro = 0, why = 0;
	if (parts && j < np) {
		ok = ntfsMount(name, dif, parts[j], CACHE_DEFAULT_PAGE_COUNT, CACHE_DEFAULT_PAGE_SIZE, NTFS_DEFAULT | NTFS_SHOW_HIDDEN_FILES);
		if (!ok) {
			why = errno;		// (EDIRTY: not closed cleanly; EHIBERNATED: Windows hibernated on it)
			ok = ro = ntfsMount(name, dif, parts[j], CACHE_DEFAULT_PAGE_COUNT, CACHE_DEFAULT_PAGE_SIZE, NTFS_READ_ONLY | NTFS_SHOW_HIDDEN_FILES);
		}
	}
	if (!ok) {
		int e = errno;
		PS3_NTFS_Shutdown(n);
		free(parts);
		if (np < 0) { plat_log("ntfs: USB device %d: couldn't be read (errno %d)", d, e); return 0; }
		if (j < np) plat_log("ntfs: USB device %d: an NTFS partition that couldn't be mounted (errno %d)", d, e);
		else plat_log("ntfs: USB device %d: %d partitions, none NTFS", d, np);
		return 1;
	}
	// read before it's used: its free clusters (counted once: the library keeps the count after), and its top
	unsigned long long total = 0, fr = 0;
	int sr = hddatlas_ntfs_space(root, 1, &total, &fr);
	DIR_ITER *top = sr == 0 ? ps3ntfs_diropen(root) : NULL;
	if (top) ps3ntfs_dirclose(top);
	if (!top) {
		int e = errno;
		ntfsUnmount(name, true);
		PS3_NTFS_Shutdown(n);
		plat_log("ntfs: USB device %d: mounted, but its %s couldn't be read (errno %d): let go, to try again", d, sr ? "free space" : "top folder", e);
		free(parts);
		return 0;
	}
	port[n].dev = d;
	port[n].read_only = ro;
	port[n].gone = 0;
	port[n].total = total;
	port[n].free_bytes = fr;
	port[n].mounted = 1;
	const char *label = ntfsGetVolumeName(name);
	plat_log("ntfs: USB device %d: partition %d of %d (sector %u) mounted as /%s: \"%s\", %.2f GiB, %.2f GiB free%s",
		d, j + 1, np, (unsigned)parts[j], name, label ? label : "", total / 1073741824.0, fr / 1073741824.0,
		ro ? (why == EHIBERNATED ? "; read-only: Windows hibernated on it" : why == EDIRTY ? "; read-only: not closed cleanly" : "; read-only") : "");
	free(parts);
	return 1;
}

static int In_Slot(int d)
{
	for (int n = 0; n < PORTS; n++) if (port[n].mounted && !port[n].gone && port[n].dev == d) return 1;
	return 0;
}

static void Watch(void *arg)
{
	(void)arg;
	while (!stopping) {
		// the mounted ones: still there? (no lock: a call may be holding it on a drive that's going)
		for (int n = 0; n < PORTS; n++) Check_Gone(n);
		int held = Lock_Watch();		// (not got: a call's holding it, stuck maybe; next pass)
		for (int n = 0; held && n < PORTS; n++) {
			if (port[n].mounted && port[n].gone && !port[n].users) Unmount(n);
			else if (Usable(n)) {		// (its space, for the screen: the library's count, no reads)
				char root[16];
				unsigned long long t = 0, f = 0;
				snprintf(root, sizeof(root), "/ntfs%d:/", n);
				if (hddatlas_ntfs_space(root, 0, &t, &f) == 0) { port[n].total = t; port[n].free_bytes = f; }
			}
		}
		if (held) Unlock();
		// every USB storage device: one plugged in for two passes, and not in a slot, looked at
		for (int d = 0; d < DEVICES && !stopping; d++) {
			int in = hddatlas_usb_present(d);
			if (in != present[d]) plat_log("ntfs: USB device %d %s", d, in ? "plugged in" : "gone");
			present[d] = (unsigned char)in;
			if (!in) { seen[d] = tries[d] = 0; continue; }
			if (seen[d] < 2) { seen[d]++; continue; }
			if (tries[d] >= TRIES || In_Slot(d)) continue;
			int n = 0;
			while (n < PORTS && port[n].mounted) n++;
			if (n == PORTS) { if (!tries[d]) plat_log("ntfs: USB device %d: all %d NTFS slots in use", d, PORTS); tries[d] = TRIES; continue; }
			if (plat_time_us() - last_call < QUIET_US) {		// (a scan or delete reading NTFS: it'd wait on the mount)
				if (!(waited[d]++ % 30)) plat_log("ntfs: USB device %d: waiting for the NTFS drive being read to be done, to mount it", d);
				continue;
			}
			if (!Lock_Watch()) continue;
			waited[d] = 0;
			tries[d] = Mount(n, d) ? TRIES : tries[d] + 1;
			Unlock();
		}
		for (int i = 0; i < 20 && !stopping; i++) usleep(100000);
	}
	started = 0;
}

void ntfsx_start(void)
{
	if (!lock) lock = plat_mutex_new();
	ui_known = sysThreadGetId(&ui_thread) == 0;
	for (int i = 0; i < MAX_FILES; i++) files[i].fd = -1;
	for (int n = 0; n < PORTS; n++) port[n].dev = -1;
	started = plat_thread_start(Watch, NULL, 1800, 256, "ntfs");
}

void ntfsx_quit(void)
{
	stopping = 1;
	for (int i = 0; i < 40 && started; i++) usleep(50000);		// (the watcher out of a pass first)
	// (a scan given up on can be stuck inside the library, holding the lock, for good: then the drives
	// are left as they are, rather than the app never quitting. Windows checks such a drive at its next plug-in)
	if (lock && !plat_mutex_lock_for(lock, 5000000)) {
		plat_log("ntfs: a call to a drive is still stuck after 5 s: quitting without unmounting");
		return;
	}
	for (int n = 0; n < PORTS; n++) if (port[n].mounted) Unmount(n);
	Unlock();
}

int ntfsx_read_only(const char *path)
{
	if (!ntfsx_path(path)) return 0;
	return port[Port_Of(path)].read_only;
}

static int Type_Of(int t) { return t == 0 ? FSX_FILE : t == 1 ? FSX_DIR : FSX_OTHER; }

int ntfsx_stat(const char *path, int *type, uint64_t *size, int64_t *mtime)
{
	char b[FSX_PATH + 2];
	unsigned long long sz = 0;
	long long mt = 0;
	int n = Port_Of(path), t = -1;
	if (!Lock()) return E_BUSY;
	if (Usable(n)) t = hddatlas_ntfs_stat(Lib(path, b, sizeof(b)), &sz, &mt);
	Unlock();
	if (t < 0) return Check_Gone(n) || !Usable(n) ? E_GONE : -2;
	if (type) *type = Type_Of(t);
	if (size) *size = sz;
	if (mtime) *mtime = mt;
	return 0;
}

// (no lock: the watcher's figures, so the screen never waits behind a scan)
int ntfsx_space(const char *path, uint64_t *total, uint64_t *free_bytes)
{
	int n = Port_Of(path);
	*total = *free_bytes = 0;
	if (!Usable(n) || !port[n].total) return -1;
	*total = port[n].total;
	*free_bytes = port[n].free_bytes;
	return 0;
}

int ntfsx_opendir(const char *path, ntfsx_dir_t **out)
{
	char b[FSX_PATH + 2];
	int n = Port_Of(path);
	*out = NULL;
	ntfsx_dir_t *d = calloc(1, sizeof(*d));
	if (!d) return -1;
	if (strlen(path) >= sizeof(d->path)) { free(d); return -2; }
	strcpy(d->path, path);
	d->port = n;
	if (!Lock()) { free(d); return E_BUSY; }
	if (Usable(n)) d->it = ps3ntfs_diropen(Lib(path, b, sizeof(b)));
	if (d->it) port[n].users++;
	int gone = !Usable(n);
	Unlock();
	if (!d->it) { free(d); return gone || Check_Gone(n) ? E_GONE : -2; }
	*out = d;
	return 0;
}

int ntfsx_readdir(ntfsx_dir_t *d, fsx_ent_t *out, int max)
{
	char name[1024], child[FSX_PATH];
	int n = 0;
	while (n < max) {
		if (!Lock()) return n ? n : E_BUSY;
		int ok = Usable(d->port);
		int r = ok ? ps3ntfs_dirnext(d->it, name, NULL) : -1;		// (names only: each is stat'ed by its own path, links not followed)
		Unlock();
		if (!ok) return n ? n : E_GONE;
		if (r != 0) break;		// (the end)
		if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
		if (strlen(name) >= FSX_NAME) { plat_log("scan: name too long, skipped: %s/%.60s...", d->path, name); continue; }
		if (snprintf(child, sizeof(child), "%s/%s", d->path, name) >= (int)sizeof(child)) continue;
		fsx_ent_t *o = &out[n++];
		snprintf(o->name, sizeof(o->name), "%s", name);
		int64_t mt = 0;
		uint64_t size = 0;
		int type;
		int sr = ntfsx_stat(child, &type, &size, &mt);
		if (sr == E_GONE || sr == E_BUSY) return sr;		// (gone: the folder can't be read, not a list of nothings; busy: the screen's call)
		if (sr != 0) { type = FSX_OTHER; size = 0; mt = 0; }		// (gone since: left out)
		o->type = type;
		o->size = type == FSX_FILE ? size : 0;
		o->blksize = 0;
		o->mtime = mt;
	}
	return n;
}

void ntfsx_closedir(ntfsx_dir_t *d)
{
	if (!d || !Lock_Close()) return;
	ps3ntfs_dirclose(d->it);		// (still mounted: a volume with a folder open isn't unmounted)
	port[d->port].users--;
	Unlock();
	free(d);
}

int ntfsx_open(const char *path, int write, int append)
{
	char b[FSX_PATH + 2];
	int n = Port_Of(path), slot = -1, fd = -1;
	if (!Lock()) return E_BUSY;
	for (int i = 0; i < MAX_FILES && slot < 0; i++) if (files[i].fd < 0) slot = i;
	if (slot >= 0 && Usable(n) && !(write && port[n].read_only)) {
		int flags = write ? (O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)) : O_RDONLY;
		fd = ps3ntfs_open(Lib(path, b, sizeof(b)), flags, 0777);
		if (fd >= 0) { files[slot].fd = fd; files[slot].port = n; port[n].users++; }
	}
	int gone = !Usable(n) || (fd < 0 && Check_Gone(n)), ro = write && port[n].read_only;
	Unlock();
	return fd >= 0 ? slot : gone ? E_GONE : ro ? E_RO : -2;
}

int ntfsx_read(int h, void *buf, int len, uint64_t offset)
{
	int r = -1;
	if (h < 0 || h >= MAX_FILES || !Lock()) return -1;
	if (files[h].fd >= 0 && Usable(files[h].port) && ps3ntfs_seek64(files[h].fd, (s64)offset, SEEK_SET) == (s64)offset)
		r = ps3ntfs_read(files[h].fd, buf, len);
	Unlock();
	return r;
}

int ntfsx_write(int h, const void *buf, int len)
{
	int r = -1;
	if (h < 0 || h >= MAX_FILES || !Lock()) return -1;
	if (files[h].fd >= 0 && Usable(files[h].port)) r = ps3ntfs_write(files[h].fd, buf, len);
	Unlock();
	return r;
}

void ntfsx_close(int h)
{
	if (h < 0 || h >= MAX_FILES || !Lock_Close()) return;
	if (files[h].fd >= 0) {
		ps3ntfs_close(files[h].fd);		// (written out to the drive: the library syncs a file it closes)
		port[files[h].port].users--;
		files[h].fd = -1;
	}
	Unlock();
}

// a change: refused on a read-only volume
static int Change(const char *path, const char *to, int what)
{
	char a[FSX_PATH + 2], b[FSX_PATH + 2];
	int n = Port_Of(path), r;
	if (to && (!ntfsx_path(to) || Port_Of(to) != n)) return -18;		// (EXDEV: one volume only)
	if (!Lock()) return E_BUSY;
	if (!Usable(n)) r = E_GONE;
	else if (port[n].read_only) r = E_RO;
	else if (what == 0) r = ps3ntfs_unlink(Lib(path, a, sizeof(a)));		// (a file, or an empty folder: the library's unlink takes both)
	else if (what == 1) r = ps3ntfs_rename(Lib(path, a, sizeof(a)), Lib(to, b, sizeof(b)));
	else if (what == 3) {		// (to's replaced: unlinked and renamed onto under one hold of the lock, so a busy
								// lock can't come between them and leave neither)
		const char *t = Lib(to, b, sizeof(b));
		ps3ntfs_unlink(t);
		r = ps3ntfs_rename(Lib(path, a, sizeof(a)), t);
	}
	else r = ps3ntfs_mkdir(Lib(path, a, sizeof(a)), 0777);
	Unlock();
	return r == 0 ? 0 : r < 0 ? r : -r;
}

int ntfsx_unlink(const char *path) { return Change(path, NULL, 0); }
int ntfsx_rmdir(const char *path) { return Change(path, NULL, 0); }
int ntfsx_rename(const char *from, const char *to) { return Change(from, to, 1); }
int ntfsx_mkdir(const char *path) { return Change(path, NULL, 2); }
int ntfsx_replace(const char *from, const char *to) { return Change(from, to, 3); }
