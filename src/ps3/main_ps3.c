/*
main_ps3.c: the PS3 backend's plat_* functions and main().
(From PS3 World's: no network, no filesystem self-test (it wrote test files),
the log is hddatlas_log.txt.)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>

#include <ppu-types.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sysutil/sysutil.h>
#include <sysmodule/sysmodule.h>
#include <lv2/system.h>
#include <lv2/sysfs.h>
#include <sys/file.h>
#include <ppu-lv2.h>

#include "../plat.h"
#include "../app.h"
#include "ps3.h"
#include "ntfs_ps3.h"

#ifndef APP_DIR
#define APP_DIR "/dev_hdd0/game/HDAT00001/USRDIR"
#endif

int g_screen_w = 1280;		// (plat.h's SCREEN_W: video_ps3.c makes it 960 on a 4:3 display)
static volatile int quit_requested;
static volatile int xmb_open;		// the PS button menu is over us

/*
==================
Log
==================
*/
static FILE *log_file;
static sys_mutex_t log_lock;
static int log_lock_made;

void plat_log(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	uint64_t t = sysGetSystemTime();
	int n = snprintf(line, sizeof(line), "[%6.2f] ", t / 1000000.0);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	if (log_lock_made) sysMutexLock(log_lock, 0);
	if (log_file) {
		fputs(line, log_file);
		fputc('\n', log_file);
		fflush(log_file);
	}
	if (log_lock_made) sysMutexUnlock(log_lock);
}

static void Log_Open(void)
{
	char path[256], prev[256];
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&log_lock, &attr) == 0)
		log_lock_made = 1;
	snprintf(path, sizeof(path), "%s/hddatlas_log.txt", APP_DIR);
	snprintf(prev, sizeof(prev), "%s/hddatlas_log_prev.txt", APP_DIR);
	remove(prev);
	rename(path, prev);
	log_file = fopen(path, "w");
}

/*
==================
Time, threads, locks
==================
*/
uint64_t plat_time_us(void) { return sysGetSystemTime(); }
void plat_sleep_us(unsigned us) { usleep(us); }

typedef struct {
	plat_thread_fn fn;
	void *arg;
} thread_start_t;

static void Thread_Entry(void *p)
{
	thread_start_t ts = *(thread_start_t *)p;
	free(p);
	ts.fn(ts.arg);
	sysThreadExit(0);
}

int plat_thread_start(plat_thread_fn fn, void *arg, int prio, int stack_kb, const char *name)
{
	sys_ppu_thread_t id;
	thread_start_t *ts = malloc(sizeof(*ts));
	if (!ts) return 0;
	ts->fn = fn;
	ts->arg = arg;
	s32 ret = sysThreadCreate(&id, Thread_Entry, ts, prio, (u64)stack_kb * 1024, 0, (char *)name);
	if (ret != 0) {
		plat_log("thread %s: sysThreadCreate = 0x%08x", name, (unsigned)ret);
		free(ts);
		return 0;
	}
	return 1;
}

struct plat_mutex {
	sys_mutex_t m;
};

plat_mutex_t *plat_mutex_new(void)
{
	plat_mutex_t *pm = malloc(sizeof(*pm));
	sys_mutex_attr_t attr;
	if (!pm) return NULL;
	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&pm->m, &attr) != 0) {
		plat_log("FATAL: sysMutexCreate failed");
		free(pm);
		return NULL;
	}
	return pm;
}

void plat_mutex_lock(plat_mutex_t *m) { if (m) sysMutexLock(m->m, 0); }
int plat_mutex_lock_for(plat_mutex_t *m, unsigned us) { return m && sysMutexLock(m->m, us ? us : 1) == 0; }		// (lv2's timeout: 0 would be forever)
void plat_mutex_unlock(plat_mutex_t *m) { if (m) sysMutexUnlock(m->m); }

/*
==================
Files and system
==================
*/
const char *plat_data_dir(void) { return APP_DIR "/data"; }
const char *plat_save_dir(void) { return APP_DIR; }

int plat_system_lang(void)
{
	s32 v = SYSUTIL_LANG_ENGLISH_US;
	if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_LANG, &v) != 0) v = SYSUTIL_LANG_ENGLISH_US;
	return v;
}

int plat_current_user(char *out, int size)
{
	char name[SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE + 1] = {0};
	if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME, name, SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE) != 0 || !name[0]) {
		if (size > 0) out[0] = 0;
		return 0;
	}
	snprintf(out, size, "%s", name);
	return 1;
}

// an NTFS drive's file, whole (the recycle bin's list there)
static unsigned char *Ntfs_Read_File(const char *path, size_t *len)
{
	int type;
	uint64_t size;
	if (ntfsx_stat(path, &type, &size, NULL) != 0 || type != FSX_FILE || size > 64 * 1024 * 1024) return NULL;
	unsigned char *buf = malloc(size + 1);
	int h = buf ? ntfsx_open(path, 0, 0) : -1;
	size_t got = 0;
	while (h >= 0 && got < size) {
		int r = ntfsx_read(h, buf + got, (int)(size - got), got);
		if (r <= 0) break;
		got += r;
	}
	if (h >= 0) ntfsx_close(h);
	if (got != size) { free(buf); return NULL; }
	buf[size] = 0;
	if (len) *len = size;
	return buf;
}

static int Ntfs_Write(const char *path, const void *data, size_t len, int append)
{
	int h = ntfsx_open(path, 1, append);
	if (h < 0) return 0;
	size_t done = 0;
	while (done < len) {
		int r = ntfsx_write(h, (const char *)data + done, (int)(len - done));
		if (r <= 0) break;
		done += r;
	}
	ntfsx_close(h);		// (on the drive when this returns: the library syncs a file it closes)
	return done == len;
}

unsigned char *plat_read_file(const char *path, size_t *len)
{
	if (ntfsx_path(path)) return Ntfs_Read_File(path, len);
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || !(buf = malloc(n + 1))) { fclose(f); return NULL; }
	if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	buf[n] = 0;
	if (len) *len = n;
	return buf;
}

int plat_write_file(const char *path, const void *data, size_t len)
{
	// a name of its own: two threads can write the same file (an icon and its preview)
	static volatile int seq;
	char tmp[1024];
	int tn = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, __sync_add_and_fetch(&seq, 1));
	if (tn < 0 || tn >= (int)sizeof(tmp)) return 0;
	if (ntfsx_path(path)) {
		if (!Ntfs_Write(tmp, data, len, 0)) { ntfsx_unlink(tmp); return 0; }
		if (ntfsx_replace(tmp, path) == 0) return 1;
		ntfsx_unlink(tmp);
		return 0;
	}
	FILE *f = fopen(tmp, "wb");
	if (!f) return 0;
	int ok = fwrite(data, 1, len, f) == len;
	ok = (fclose(f) == 0) && ok;
	if (!ok) { remove(tmp); return 0; }
	remove(path);
	return rename(tmp, path) == 0;
}

int plat_append_line(const char *path, const char *line)
{
	s32 fd;
	u64 done = 0;
	if (ntfsx_path(path)) {
		size_t n = strlen(line);
		char *b = malloc(n + 2);
		if (!b) return 0;
		memcpy(b, line, n);
		b[n] = '\n';
		int ok = Ntfs_Write(path, b, n + 1, 1);
		free(b);
		return ok;
	}
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0644, NULL, 0) != 0) return 0;
	int ok = sysLv2FsWrite(fd, line, strlen(line), &done) == 0 && done == strlen(line);
	ok = ok && sysLv2FsWrite(fd, "\n", 1, &done) == 0 && done == 1;
	ok = sysLv2FsFsync(fd) == 0 && ok;		// (on the disk now: the next call may freeze the console)
	sysLv2FsClose(fd);
	return ok;
}

#define FS_S_IFMT	0170000		// lv2's st_mode bits, as CELL_FS_S_IFMT / CELL_FS_S_IFDIR
#define FS_S_IFDIR	0040000
#define FS_TYPE_DIR	1			// sysFSDirent.d_type for a folder

long long plat_file_size(const char *path)
{
	if (ntfsx_path(path)) {
		int type;
		uint64_t size;
		return ntfsx_stat(path, &type, &size, NULL) == 0 && type == FSX_FILE ? (long long)size : -1;
	}
	sysFSStat st;
	if (sysLv2FsStat(path, &st) != 0 || (st.st_mode & FS_S_IFMT) == FS_S_IFDIR)
		return -1;
	return (long long)st.st_size;
}

long long plat_file_mtime(const char *path)
{
	if (ntfsx_path(path)) {
		int64_t mt = 0;
		return ntfsx_stat(path, NULL, NULL, &mt) == 0 ? (long long)mt : 0;
	}
	sysFSStat st;
	if (sysLv2FsStat(path, &st) != 0)
		return 0;
	return (long long)st.st_mtime;
}

int plat_dir_exists(const char *path)
{
	if (ntfsx_path(path)) {
		int type;
		return ntfsx_stat(path, &type, NULL, NULL) == 0 && type == FSX_DIR;
	}
	s32 fd;
	if (sysLv2FsOpenDir(path, &fd) != 0)
		return 0;
	sysLv2FsCloseDir(fd);
	return 1;
}

static void Mkdir(const char *path) { if (ntfsx_path(path)) ntfsx_mkdir(path); else sysLv2FsMkdir(path, 0777); }

int plat_mkdirs(const char *path)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", path);
	// from the folder under the device ("/dev_hdd0/packages": "/dev_hdd0" is there already)
	char *p = strchr(buf + 1, '/');
	while (p) {
		*p = 0;
		if (!plat_dir_exists(buf)) Mkdir(buf);
		*p = '/';
		p = strchr(p + 1, '/');
	}
	if (!plat_dir_exists(buf)) Mkdir(buf);
	return plat_dir_exists(buf);
}

long long plat_free_bytes(const char *path)
{
	if (ntfsx_path(path)) {
		uint64_t t, f;
		return ntfsx_space(path, &t, &f) == 0 ? (long long)f : -1;
	}
	char dev[64];
	u32 block = 0;
	u64 blocks = 0;
	// the device's root: "/dev_usb000/PS3/THEME" -> "/dev_usb000"
	const char *e = strchr(path + 1, '/');
	int n = e ? (int)(e - path) : (int)strlen(path);
	snprintf(dev, sizeof(dev), "%.*s", n, path);
	s32 r = sysFsGetFreeSize(dev, &block, &blocks);
	if (r != 0) {
		plat_log("plat: sysFsGetFreeSize(%s) = 0x%08x", dev, (unsigned)r);
		return -1;
	}
	return (long long)block * (long long)blocks;
}

int plat_list_dirs(const char *path, char (*names)[64], int max)
{
	s32 fd;
	sysFSDirent ent;
	u64 nread;
	int n = 0;
	if (sysLv2FsOpenDir(path, &fd) != 0)
		return 0;
	while (n < max && sysLv2FsReadDir(fd, &ent, &nread) == 0 && nread > 0) {
		if (ent.d_type != FS_TYPE_DIR || ent.d_name[0] == '.')
			continue;
		snprintf(names[n++], 64, "%s", ent.d_name);
	}
	sysLv2FsCloseDir(fd);
	return n;
}

void plat_random(void *buf, int n)
{
	if (sysGetRandomNumber(buf, (u64)n) == 0) return;
	// no random numbers from the system: the clock will have to do (only a nonce)
	u64 t = sysGetSystemTime();
	for (int i = 0; i < n; i++) { t = t * 6364136223846793005ull + 1442695040888963407ull; ((u8 *)buf)[i] = (u8)(t >> 56); }
}

// the app's own Quit: the main loop ends, as on the XMB's (the download pauses, the log closes)
void plat_quit(void)
{
	plat_log("ui: quit to the XMB");
	quit_requested = 1;
}

// the console's confirm button: 0 is O (Japanese and Asian consoles), 1 is X
int plat_circle_confirms(void)
{
	static int v = -1;
	if (v < 0) {
		s32 a = 1;
		v = sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &a) == 0 && a == 0;
		if (v) plat_log("plat: O confirms on this console: swapping O and X");
	}
	return v;
}

/*
==================
main
==================
*/
static void Sysutil_Callback(u64 status, u64 param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == SYSUTIL_EXIT_GAME)
		quit_requested = 1;
	else if (status == SYSUTIL_MENU_OPEN)
		xmb_open = 1;
	else if (status == SYSUTIL_MENU_CLOSE)
		xmb_open = 0;
	else if (status >= SYSUTIL_OSK_LOADED && status <= SYSUTIL_OSK_INPUT_CANCELED)
		ps3_osk_event(status);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	Log_Open();
	{
		time_t now = time(NULL);
		plat_log("PS3 clock: %s", ctime(&now));	// TLS checks certificate dates against it
	}
	if (sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, Sysutil_Callback, NULL) != 0)
		plat_log("WARNING: sysUtilRegisterCallback failed");
	ps3_input_init();		// (first: holding L1 at start-up keeps the RSX out of it)
	ntfsx_start();			// (NTFS drives mounted as they're found: the screen sees them a few seconds in)
	if (!ps3_video_init())
		return 1;
	if (!app_start())
		return 1;
	plat_sound_init();

	uint64_t last = plat_time_us(), present_us = 0, presents = 0, present_log = last, last_flip = last;
	while (!quit_requested) {
		sysUtilCheckCallback();
		if (xmb_open || plat_osk_active()) {
			// the XMB gets the machine: no drawing (downloads carry on by
			// themselves), and the last frame flipped again ~30 times a
			// second: the system draws the XMB on our flips
			static int was_open;
			if (!was_open) { plat_log("%s: drawing paused", xmb_open ? "XMB open" : "keyboard up"); was_open = 1; }
			app_background();
			ps3_video_reflip();
			usleep(33000);
			if (!xmb_open && !plat_osk_active()) { plat_log("drawing again"); was_open = 0; app_redraw(); }
			last = plat_time_us();
			continue;
		}
		int drawn;
		uint32_t *canvas = app_frame(&drawn);
		if (drawn) {
			uint64_t t0 = plat_time_us();
			ps3_video_present(canvas);
			present_us += plat_time_us() - t0;
			presents++;
			last_flip = plat_time_us();
		} else {
			// nothing new: the last frame again, every vblank. A steady 60
			// (the system's pop-ups are drawn on our flips too, and HEN's
			// frame counter reads what the TV gets)
			ps3_video_reflip();
			last_flip = plat_time_us();
		}
		if (plat_time_us() - present_log > 10000000) {
			if (presents)
				plat_log("present: %.1f ms each (the copy, and the wait for the vblank)", present_us / 1000.0 / presents);
			present_us = presents = 0;
			present_log = plat_time_us();
		}
		// no sleep here: every pass flips, and each flip waits for the one
		// before it, so the vblank sets the pace (a sleep of our own as well
		// beat against it and settled around 34 fps)
		last = plat_time_us();
	}
	plat_log("XMB: quit");
	app_shutdown();
	ntfsx_quit();			// (every NTFS drive unmounted: what the library cached, written out)
	plat_sound_quit();
	ps3_video_shutdown();
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	if (log_lock_made) sysMutexLock(log_lock, 0);
	if (log_file) { fclose(log_file); log_file = NULL; }
	if (log_lock_made) sysMutexUnlock(log_lock);
	return 0;
}

int plat_rename(const char *from, const char *to)
{
	if (ntfsx_path(from) || ntfsx_path(to)) return ntfsx_rename(from, to);
	return rename(from, to);
}

int plat_remove(const char *path)
{
	if (ntfsx_path(path)) return ntfsx_unlink(path);
	return remove(path);
}

int plat_read_only(const char *root) { return ntfsx_read_only(root); }
