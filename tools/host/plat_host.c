/*
plat_host.c: the plat.h calls the scanner uses, on a PC (pthreads, stdio).
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "../../src/plat.h"

int g_screen_w = 1280;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void plat_log(const char *fmt, ...)
{
	va_list ap;
	pthread_mutex_lock(&log_lock);
	printf("[%8.2f] ", plat_time_us() / 1e6);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	pthread_mutex_unlock(&log_lock);
}

uint64_t plat_time_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void plat_sleep_us(unsigned us) { usleep(us); }

typedef struct { plat_thread_fn fn; void *arg; } ts_t;
static void *Entry(void *p) { ts_t t = *(ts_t *)p; free(p); t.fn(t.arg); return NULL; }

int plat_thread_start(plat_thread_fn fn, void *arg, int prio, int stack_kb, const char *name)
{
	(void)prio; (void)name;
	pthread_t th;
	pthread_attr_t a;
	ts_t *t = malloc(sizeof(*t));
	t->fn = fn; t->arg = arg;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, (size_t)stack_kb * 1024);		// the PS3's stack: a test that it's enough
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int ok = pthread_create(&th, &a, Entry, t) == 0;
	pthread_attr_destroy(&a);
	return ok;
}

const char *plat_save_dir(void) { const char *d = getenv("HOST_SAVE"); return d ? d : "."; }

unsigned char *plat_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(n + 1);
	if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	b[n] = 0;
	if (len) *len = n;
	return b;
}

int plat_write_file(const char *path, const void *data, size_t len)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return 0;
	int ok = fwrite(data, 1, len, f) == len;
	ok = (fclose(f) == 0) && ok;
	return ok && rename(tmp, path) == 0;
}

// (the screens' workers: jobs.c's lanes, and the icons' reads)
struct plat_mutex { pthread_mutex_t m; };
plat_mutex_t *plat_mutex_new(void)
{
	plat_mutex_t *m = calloc(1, sizeof(*m));
	if (m) pthread_mutex_init(&m->m, NULL);
	return m;
}
void plat_mutex_lock(plat_mutex_t *m) { pthread_mutex_lock(&m->m); }
int plat_mutex_lock_for(plat_mutex_t *m, unsigned us)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += us / 1000000;
	t.tv_nsec += (long)(us % 1000000) * 1000;
	if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
	return pthread_mutex_timedlock(&m->m, &t) == 0;
}
void plat_mutex_unlock(plat_mutex_t *m) { pthread_mutex_unlock(&m->m); }

#include <sys/stat.h>
long long plat_file_size(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
	return st.st_size;
}

int plat_append_line(const char *path, const char *line)
{
	FILE *f = fopen(path, "a");
	if (!f) return 0;
	int ok = fprintf(f, "%s\n", line) > 0;
	ok = fflush(f) == 0 && ok;
	fsync(fileno(f));
	return fclose(f) == 0 && ok;
}

int plat_mkdirs(const char *path)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s", path);
	for (char *c = p + 1; *c; c++)
		if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
	mkdir(p, 0755);
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_rename(const char *from, const char *to) { return rename(from, to); }
int plat_remove(const char *path) { return remove(path); }
// (HOST_READ_ONLY: a drive root that stands for an NTFS drive mounted read-only)
int plat_read_only(const char *root) { const char *r = getenv("HOST_READ_ONLY"); return r && !strcmp(r, root); }
