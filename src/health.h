/*
health.h: the internal drive's own record of its health (SMART), read-only:
a hard disk or an SSD (IDENTIFY says which).

The drive is asked three things (src/ps3/ata_ps3.c): IDENTIFY DEVICE (model,
serial, firmware, size), SMART READ DATA (its attributes: wear and error
counts) and SMART READ THRESHOLDS (where each attribute counts as failing).
That's all: no self-test, no log, nothing the drive keeps changes.

The way of asking is lv2's storage syscalls 600, 616 and 601, with LV1's
SEND_ATA_COMMAND. It hasn't been tried on every console, so every call is
journaled first: a line saying it's starting reaches the disk before the
call, and one saying it's done after. A start with no done means the console
froze inside that call; then it's never made again.
*/
#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

#define HEALTH_ATTRS	30

enum { HEALTH_GOOD, HEALTH_CAUTION, HEALTH_FAILING };
enum { WHY_GOOD, WHY_FAILING, WHY_PAST, WHY_WORN, WHY_HOT, WHY_LIFE };
#define HEALTH_LIFE_LOW	10		// an SSD's life left at or under this (%): caution

typedef struct {
	uint8_t id, value, worst, thresh;
	uint16_t flags;				// bit 0: pre-failure (its threshold means the drive's about to fail)
	uint64_t raw;
} health_attr_t;

typedef struct {
	char model[41], serial[21], firmware[9];
	uint64_t bytes;				// the drive's size
	int rotation;				// IDENTIFY word 217: 1 an SSD, else its rpm (0 not given)
	int ssd;					// an SSD: word 217 says so, or (not given) it has an SSD's own wear attributes
	int life_left;				// an SSD's own estimate of its flash's rated wear left, % (-1 not given)
	int life_attr;				// the attribute it's from (231, 233, 202, 177, 169 or 173)
	int swapped;				// the words came byte-swapped (and SMART's data with them)
	int smart_supported, smart_enabled;
	int count;
	health_attr_t a[HEALTH_ATTRS];
	int have_thresh;
	int data_checksum_ok;		// SMART data's checksum (0: bad; the values are shown anyway)
	int temperature;			// degrees C (-1 not given)
	int64_t power_on_hours, power_cycles, reallocated, pending, uncorrectable, crc_errors;	// -1 not given
	int status;					// HEALTH_*
	char why[200];				// what decided it, in English (the log's)
	// the same as data, for the screen to say in its language (health_why)
	int why_kind;				// WHY_*
	int why_attr, why_value, why_thresh;
	int64_t why_count;
} health_t;

// what was read (raw sectors, as the drive sent them), into h; 0 if IDENTIFY makes sense
int health_parse(const uint8_t identify[512], const uint8_t *data, const uint8_t *thresh, health_t *h);
const char *health_attr_name(int id, int ssd);		// (some numbers mean another thing on an SSD)
int health_hot(const health_t *h);					// its temperature counts as hot: 55 C a disk, 70 an SSD
// what a wear count is (WHY_WORN's why_attr: 5, 197, 198, 187 or 196), in English (tr() it)
const char *health_worn_what(int id);
void health_mask_serial(const char *serial, char *out, int size);	// the first two and last two characters

/*
The journal (USRDIR/smart_journal.txt): "start <call>" before each call to the
drive, "done <call> <rc>" after.
*/
// a call the console froze inside last time (start, no done) into froze; 1 if there's one. A
// "quit" after the start isn't a freeze: the app quit cleanly while the call was still going
int health_journal_froze(const char *path, char *froze, int size);
// the app's quitting: no call to the drive starts from now on (the platform's checks it), and if
// one's still going, the journal says the app quit during it (so it isn't taken for a freeze)
extern volatile int health_quitting;
void health_journal_quit(const char *path);
// the drive's been read here before without trouble (no need to ask first)
int health_journal_ok_before(const char *path);

// readings over time (USRDIR/smart_history.txt, a line each): when, the drive (model and masked
// serial), and the figures that matter
typedef struct {
	int64_t when;
	char drive[64];
	int64_t hours, cycles, reallocated, pending, uncorrectable, crc;
	int temperature, status;
	int life;					// an SSD's life left, % (-1 not given; a line from before 1.0.1 hasn't it)
} health_rec_t;
void health_history_add(const char *path, const health_t *h, int64_t when);
// this drive's readings (the same model and serial), oldest first; how many
int health_history_load(const char *path, const health_t *h, health_rec_t *out, int max);
// the last reading kept, of whichever drive (the internal one: there's only ever been one, as USRDIR's on it); 1 if there's one
int health_history_last(const char *path, health_rec_t *out);

// platform: the three reads, journaled (512 bytes each; thresh can fail on its own, *have_thresh
// says). 0 when IDENTIFY and the data came back; else why says what went wrong
int health_read_drive(const char *journal, uint8_t *identify, uint8_t *data, uint8_t *thresh, int *have_thresh,
	char *why, int why_size);

#endif
