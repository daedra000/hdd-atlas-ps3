/*
ui_int.h: what HDD Atlas's screens share. ui.c has the drives, the scans and the
overview; ui_list.c the lists (a category's items, the largest items and
files, a folder's contents).
*/
#ifndef UI_INT_H
#define UI_INT_H

#include <stdint.h>
#include "scan.h"

#define NUM_DRIVES	17		// the HDD, USB 000-007 (FAT32, the system's), and NTFS 0-7 (src/ps3/ntfs_ps3.c)
#define FIRST_NTFS	9

typedef struct {
	char root[64];			// "/dev_hdd0"
	char name[24];			// "Internal HDD", "USB 000", in English (ui_drive_name: in the language on screen)
	int present;
	scan_t *shown;			// its last scan (from the cache, or just finished); NULL: none yet
	uint64_t live_total, live_free, live_disk;		// read every few seconds
	scan_changes_t *changes;	// what changed between its last two scans (NULL: none yet)
} drive_t;

extern drive_t drives[NUM_DRIVES];
extern int cur_drive;
#define DRV	(&drives[cur_drive])

extern const uint32_t cat_colors[NUM_CATS];
#define C_TRACK		0x26ffffff

const char *ui_drive_name(int i);
void ui_dirty(void);
void ui_note(const char *msg);
void ui_sound(int id);
void ui_fmt_pct(uint64_t v, uint64_t of, char *out, int size);
int ui_scanning(void);			// a scan's running (any drive)
void ui_draw_strip(void);		// how it's going, bottom right
int ui_hints_max(void);			// where the hints along the bottom must end (before the strip, while there is one)

// the lists (ui_list.c)
void list_open_items(int cat);		// a category's items
void list_open_largest(void);
int list_update(uint32_t pressed);	// 0: Circle went back to the overview
void list_render(void);
void list_scan_changed(void);		// the drive's scan was replaced: the lists follow it
void list_shutdown(void);

void list_after_delete(void);
void list_bin_changed(void);		// a recycle bin's been tidied or expired: its list follows
int ui_bin_busy(void);
void ui_bin_counted(int drive, int64_t bytes, int32_t files);	// the drive's scan follows its recycle bin (moved in > 0, gone < 0)				// the drive on screen's recycle bin is being tidied and expired (nothing put back meanwhile)		// a row was deleted: the list follows (a folder's read again)

void list_open_changes(void);		// what changed since the scan before
void list_open_games(int dupes);		// every game added up, or only those held more than once
void list_open_free(void);			// what's likely safe to clear
void list_open_types(void);			// the drive's files by type
enum { LIST_FOLDERS, LIST_OLD, LIST_MARKS, LIST_BIN, LIST_FILTER, LIST_SAME, LIST_RECENT };
void list_open_mode(int which);		// the largest folders, games not played lately, bookmarks
uint64_t list_spare_bytes(uint64_t *deletable);
uint64_t list_recent_bytes(int days);		// what was added or changed in the last days (the scan on screen)
void list_figures(uint64_t *spare, int *games, int *dupes, int *bookmarks);	// the START menu's
void ui_rescan_category(int cat);		// one category again, the rest from the last scan

// the START menu, Settings and About (ui_menu.c)
enum { MENU_STAY, MENU_GO_CHANGES, MENU_GO_HEALTH, MENU_GO_GAMES, MENU_GO_DUPES, MENU_GO_FREE, MENU_GO_TYPES,
	MENU_GO_OLD, MENU_GO_FOLDERS, MENU_GO_MARKS, MENU_GO_MAP, MENU_GO_SAME, MENU_GO_FILTER, MENU_GO_BIN, MENU_GO_RECENT };
void menu_open_settings(void);		// Settings on its own (from the start screen: Circle goes back there)

// ui_home.c: the intro, then the start screen
enum { HOME_NONE, HOME_SCAN, HOME_LAST, HOME_TOOLS, HOME_SETTINGS, HOME_QUIT };
void home_init(void);
void home_shutdown(void);
int home_intro_active(void);
void home_intro_hurry(void);
void home_intro_draw(void);
void home_intro_at(int ms);			// (screenshots) -1 none of it, else a frame this far in
int home_update(uint32_t pressed);	// HOME_*
void home_render(void);
void home_drive_changed(void);
int ui_drives_present(void);
int ui_scan_drive(void);			// the drive being scanned (-1 none)
int ui_drive_hung(int i);			// its scan was given up on, the call still out: nothing else is done on it
void ui_hung_note(void);			// (and the note that says so)
int ui_scan_pct(void);				// how far it is, 0-99 (-1: can't be told; show counts)

// the drive as a map of boxes (ui_map.c)
void map_open(void);
int map_update(uint32_t pressed);	// 0: back to the overview
void map_render(void);
void menu_open(void);
int menu_active(void);
int menu_fullscreen(void);			// Settings or About (else a panel over the overview)
int menu_update(uint32_t pressed);	// MENU_*
void menu_render(void);
void menu_poll(void);				// each frame: a report being written
void menu_apply_settings(void);
void menu_music_toggle(void);
void menu_shutdown(void);
int menu_exporting(void);			// a report's being written (from a scan that mustn't change till it's done)

// deleting (ui_delete.c): the lock, and the panel over the screen
int delete_enabled(void);
void delete_lock_toggle(void);		// SELECT + START on the overview
int delete_open(const char *rel, int is_dir, const char *name, uint64_t bytes, uint32_t files);	// 0: not allowed (said why)
int delete_open_empty_bin(void);
void delete_same_as(const char *other_rel);	// the next delete_open's row must be the same as this copy, every byte, first	// the drive on screen's recycle bin, all of it, for good
int delete_active(void);			// a panel's up: it gets the buttons
void delete_update(uint32_t pressed);
void delete_render(void);
void delete_shutdown(void);

// the Drive health screen (ui_health.c)
void health_open(void);
int health_update(uint32_t pressed);	// 0: Circle went back to the overview
void health_render(void);
void health_shutdown(void);
int health_known_worn(void);		// the internal drive's last reading (this session's, or the last kept) found it wearing out

#endif
