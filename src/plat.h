/*
plat.h: what the app needs from the machine it runs on (src/ps3, PSL1GHT).
(From PS3 World's plat.h: the canvas width is chosen at start-up, 1280 for a
16:9 display or 960 for a 4:3 one, and the HTTP client is gone.)
*/
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>

// the canvas: SCREEN_W x 720. 1280 on a 16:9 display, 960 on a 4:3 one (video_ps3.c sets it
// before anything is drawn); the screens lay themselves out from it
extern int g_screen_w;
#define SCREEN_W g_screen_w
#define SCREEN_H 720
static inline int screen_narrow(void) { return SCREEN_W < 1280; }

// ---- log (USRDIR/hddatlas_log.txt on the PS3, flushed every line: the crash log) ----
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- time ----
uint64_t plat_time_us(void);
void plat_sleep_us(unsigned us);

// ---- threads ----
typedef void (*plat_thread_fn)(void *arg);
// prio: PS3 style, 0 highest; the main thread is ~1000
int plat_thread_start(plat_thread_fn fn, void *arg, int prio, int stack_kb, const char *name);

// a blocking lock for worker threads (never taken on the audio thread)
typedef struct plat_mutex plat_mutex_t;
plat_mutex_t *plat_mutex_new(void);
void plat_mutex_lock(plat_mutex_t *m);
int plat_mutex_lock_for(plat_mutex_t *m, unsigned us);	// 1 locked; 0 still held by another after us
void plat_mutex_unlock(plat_mutex_t *m);

// a spin flag the audio thread can try without blocking
static inline int spin_trylock(volatile int *f)
{
	if (!__sync_bool_compare_and_swap(f, 0, 1))
		return 0;
	__sync_synchronize();
	return 1;
}
static inline void spin_lock(volatile int *f)
{
	while (!spin_trylock(f))
		plat_sleep_us(50);
}
static inline void spin_unlock(volatile int *f)
{
	__sync_synchronize();
	*f = 0;
}

// ---- files ----
const char *plat_data_dir(void);		// bundled fonts etc. (read-only)
const char *plat_save_dir(void);		// settings, log (writable)
int plat_system_lang(void);				// the console's language, as sysutil numbers it (1: English)
int plat_current_user(char *out, int size);	// the signed-in user's name (as /dev_hdd0/home/<n>/localusername); 0 if unknown

// ---- sound effects ----
enum { SND_MOVE, SND_OK, SND_BACK, SND_COUNT };
int plat_sound_init(void);				// 0: no sound (the UI goes on without)
void plat_sound(int id);				// starts one (mixed with any playing); any thread
void plat_sound_enable(int on);
void plat_sound_quit(void);
// reads a whole file; NULL if it isn't there. *len gets the size. Caller frees.
unsigned char *plat_read_file(const char *path, size_t *len);
int plat_write_file(const char *path, const void *data, size_t len);
// line (and a newline) added to the end of path, and on the disk before this returns (fsync):
// the drive-health journal, which must survive the console freezing on the next call
int plat_append_line(const char *path, const char *line);
// downloads and install targets: big files are written with stdio (fopen/fwrite), these do the rest
long long plat_file_size(const char *path);		// -1 if it isn't there (or is a folder)
long long plat_file_mtime(const char *path);		// when it was last written (seconds since 1970); 0 if it isn't there
int plat_dir_exists(const char *path);
int plat_mkdirs(const char *path);				// every missing folder on the way; 1 if it's there after
long long plat_free_bytes(const char *path);	// on the drive path is on; -1 if unknown
int plat_rename(const char *from, const char *to);	// 0 when it's done (one drive)
int plat_remove(const char *path);				// a file; 0 when it's gone
int plat_read_only(const char *root);			// the drive can't be written (an NTFS drive mounted read-only)
// up to max names of the folders in path (each < 64 chars); returns how many
int plat_list_dirs(const char *path, char (*names)[64], int max);

// ---- screen ----
// the canvas to draw the next frame into (SCREEN_W x SCREEN_H, 0xAARRGGBB)
uint32_t *plat_canvas_next(void);

// ---- on-screen keyboard (Search) ----
int plat_osk_open(const char *title, const char *initial);	// 1 if it's up
int plat_osk_active(void);
int plat_osk_poll(char *out, int size);	// 1 text entered, -1 cancelled, 0 still open

// ---- system ----
int plat_circle_confirms(void);		// the console uses O to confirm and X to go back (Japan, Asia)
void plat_quit(void);					// back to the XMB, as the PS button's Quit does (after this frame)
void plat_random(void *buf, int n);		// unpredictable bytes
// the picture: drawn by the CPU (720p) at the next start, rather than the graphics chip
int plat_renderer_cpu(void);			// chosen for the next start
void plat_renderer_set_cpu(int cpu);
int plat_renderer_gpu_now(void);		// the graphics chip draws this time

#endif
