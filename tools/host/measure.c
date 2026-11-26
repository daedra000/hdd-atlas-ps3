// "font px text" a line on stdin -> the text's width in pixels, as font.c draws it
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
const char *plat_data_dir(void) { const char *d = getenv("HOST_DATA"); return d ? d : "data"; }
int main(void)
{
	if (!font_init()) { fprintf(stderr, "no fonts\n"); return 1; }
	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\n")] = 0;
		int f, px, n = 0;
		if (sscanf(line, "%d %d %n", &f, &px, &n) < 2) continue;
		printf("%d\n", font_width(f, px, line + n));
		fflush(stdout);
	}
	return 0;
}
#include "gpu.h"
int plat_system_lang(void) { return 1; }
int gpu_tex_alloc(gtex_t *t, int w, int h) { (void)t; (void)w; (void)h; return 0; }
void gpu_tex_free(gtex_t *t) { (void)t; }
void gpu_frame_begin(void) {}
void gpu_frame_submit(const gquad_t *q, int nq, const gbatch_t *b, int nb) { (void)q; (void)nq; (void)b; (void)nb; }
