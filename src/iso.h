/*
iso.h: a disc image's title and title ID, read from inside it (read-only, a
few small reads): PS3 and PSP images' PARAM.SFO (PS3_GAME/, PSP_GAME/), PS2
and PS1 images' SYSTEM.CNF (the ID only: they keep no title). ISO9660, in
2048-byte sectors or a raw .bin's 2352. Failing all that, an ID written in
the file's name ("Game [BLUS30443].iso").
*/
#ifndef ISO_H
#define ISO_H

#include <stdint.h>

// title (may stay ""), tid ("BLUS30443") and code (PARAM.SFO's CATEGORY, or "2P"/"1P"); 1 if any found
// icon: 1 the image has PS3_GAME/ICON0.PNG, 2 PSP_GAME/ICON0.PNG, 0 neither
int iso_info(const char *path, char *title, int tsize, char *tid, int idsize, char *code, int csize, int *icon);
// a file inside the image (dir NULL: at its top), whole, malloc'd (up to 1 MB); NULL if it isn't there
unsigned char *iso_read_file(const char *path, const char *dir, const char *name, uint32_t *len);
// a title ID in s ("BLUS30443", "SLUS_203.12", "SLUS-20312"), normalised to 9 characters; 1 if found
int iso_tid_in(const char *s, char *out, int size);

#endif
