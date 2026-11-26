/*
settings.h: HDD Atlas's few choices, kept in USRDIR/settings.txt as "key=number" lines
(music: the background music on; delete: deleting allowed, Phase 4's lock).
*/
#ifndef SETTINGS_H
#define SETTINGS_H

int setting_get(const char *key, int def);
void setting_set(const char *key, int value);		// written at once

#endif
