/*
icons.h: ICON0.PNGs for the lists, read and decoded on PS3 World's art lane
(jobs.c), kept while they're used and let go of the longest unused first.
*/
#ifndef ICONS_H
#define ICONS_H

#include "gfx.h"

// path's picture at w x h canvas units (made at the screen's resolution); NULL until it's
// been read (the screen's redrawn when it is), or if it can't be
image_t *icon_get(const char *path, int w, int h);
// once a frame, before drawing: what's finished comes in; 1 if any did (redraw)
int icons_poll(void);
void icons_shutdown(void);

#endif
