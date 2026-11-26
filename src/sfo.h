/*
sfo.h: reading a PARAM.SFO (a game's or a save's title, title ID and kind),
and a trophy set's name from its TROPCONF.SFM. Read-only, from a buffer.
*/
#ifndef SFO_H
#define SFO_H

#include <stddef.h>

// key's value (a string, or a number written as one) into out; 1 if it's there
int sfo_get(const unsigned char *buf, size_t len, const char *key, char *out, int size);
// TROPCONF.SFM's <title-name>; 1 if it's there
int sfo_trophy_title(const unsigned char *buf, size_t len, char *out, int size);
// s made fit for one line: control characters (a title's line breaks) become spaces, the
// ends trimmed, cut short of size at a whole UTF-8 character
void sfo_clean(char *s, int size);

#endif
