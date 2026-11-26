/*
del_host.c: del.h's two calls on a PC, for the tests: they refuse anything not under
HOST_DEL_ROOT (the test's own scratch folder), whatever del.c asks.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../src/del.h"

static int Under(const char *path)
{
	const char *root = getenv("HOST_DEL_ROOT");
	size_t n = root ? strlen(root) : 0;
	if (!n || strncmp(path, root, n) || (path[n] && path[n] != '/')) {
		fprintf(stderr, "del_host: refused %s (not under HOST_DEL_ROOT)\n", path);
		return 0;
	}
	return 1;
}

int fsx_unlink(const char *path) { return Under(path) ? (unlink(path) == 0 ? 0 : -2) : -13; }
int fsx_rmdir(const char *path) { return Under(path) ? (rmdir(path) == 0 ? 0 : -2) : -13; }
int fsx_rename(const char *from, const char *to) { return Under(from) && Under(to) ? (rename(from, to) == 0 ? 0 : -2) : -13; }
int fsx_mkdir(const char *path) { return Under(path) ? (mkdir(path, 0777) == 0 ? 0 : -2) : -13; }
