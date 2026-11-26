/*
ata_host.c: health_read_drive on a PC, for the tests and the screenshots: a
made-up drive (a 1 TB laptop HDD, as in a PS3 Super Slim) with its IDENTIFY and
SMART sectors built here, through the same journal as the console.

HOST_ATA: "swapped" (words byte-swapped, as LV1 can give them), "worn" (pending
and reallocated sectors), "failing" (a pre-failure attribute past its
threshold), "refuse" (IDENTIFY refused), "freeze" (the journal is left as if
the console froze inside smart_data; nothing's returned). And an SSD (a
500 GB SATA one, Samsung's attributes): "ssd", "ssd-worn" (7% life left),
"ssd-hot" (62 C: warm for a disk, not for an SSD), "ssd-old" (word 217 not
filled in, its life in 231 as SandForce's: known by that).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/plat.h"
#include "../../src/health.h"

static void Put_Word(uint8_t *b, int i, unsigned w) { b[2 * i] = w & 0xff; b[2 * i + 1] = w >> 8; }
static void Put_String(uint8_t *b, int first, int words, const char *s)
{
	char pad[64];
	memset(pad, ' ', sizeof(pad));
	memcpy(pad, s, strlen(s));
	for (int k = 0; k < words; k++) Put_Word(b, first + k, (unsigned)(uint8_t)pad[2 * k] << 8 | (uint8_t)pad[2 * k + 1]);
}
static void Sign(uint8_t *b, int last)
{
	unsigned s = 0;
	if (last) b[510] = 0xA5;
	for (int i = 0; i < 511; i++) s += b[i];
	b[511] = (uint8_t)(-s);
}
static void Attr(uint8_t *d, int slot, int id, int flags, int value, int worst, uint64_t raw)
{
	uint8_t *e = d + 2 + slot * 12;
	e[0] = id; e[1] = flags & 0xff; e[2] = flags >> 8; e[3] = value; e[4] = worst;
	for (int k = 0; k < 6; k++) e[5 + k] = (uint8_t)(raw >> (8 * k));
}

int health_read_drive(const char *journal, uint8_t *identify, uint8_t *data, uint8_t *thresh, int *have_thresh,
	char *why, int why_size)
{
	const char *mode = getenv("HOST_ATA") ? getenv("HOST_ATA") : "";
	char froze[64];
	*have_thresh = 0;
	if (health_journal_froze(journal, froze, sizeof(froze))) { snprintf(why, why_size, "froze:%s", froze); return -1; }
	if (!strcmp(mode, "freeze")) {
		plat_append_line(journal, "start open"); plat_append_line(journal, "done open 0");
		plat_append_line(journal, "start smart_data");
		snprintf(why, why_size, "froze:smart_data");
		return -1;
	}
	plat_append_line(journal, "start identify");
	if (!strcmp(mode, "refuse")) {
		plat_append_line(journal, "done identify -2147418110");
		snprintf(why, why_size, "The drive didn't answer IDENTIFY (0x80010002).");
		return -1;
	}
	int ssd = !strncmp(mode, "ssd", 3);
	memset(identify, 0, 512);
	Put_String(identify, 10, 10, ssd ? "S3Z2NB0K123456A" : "S2TXJ9AB123456");
	Put_String(identify, 23, 4, ssd ? "RVT04B6Q" : "2BA30001");
	Put_String(identify, 27, 20, ssd ? "Samsung SSD 860 EVO 500GB" : "ST1000LM024 HN-M101MBB");
	Put_Word(identify, 82, 1); Put_Word(identify, 83, 0x4000 | (1 << 10)); Put_Word(identify, 85, 1); Put_Word(identify, 87, 0x4000);
	uint64_t sectors = ssd ? 976773168ULL : 1953525168ULL;
	for (int k = 0; k < 4; k++) Put_Word(identify, 100 + k, (unsigned)(sectors >> (16 * k)) & 0xffff);
	Put_Word(identify, 217, !ssd ? 5400 : strcmp(mode, "ssd-old") ? 1 : 0);
	Sign(identify, 1);
	plat_append_line(journal, "done identify 0");

	memset(data, 0, 512);
	memset(thresh, 0, 512);
	int worn = !strcmp(mode, "worn"), failing = !strcmp(mode, "failing");
	static const struct { int id, flags, value, worst, th; uint64_t raw; } attrs[] = {
		{1, 0x2f, 100, 100, 51, 12}, {3, 0x23, 89, 87, 25, 3412}, {4, 0x32, 97, 97, 0, 3187}, {5, 0x33, 252, 252, 10, 0},
		{7, 0x2e, 252, 252, 51, 0}, {9, 0x32, 100, 100, 0, 6121}, {10, 0x33, 252, 252, 51, 0}, {12, 0x32, 97, 97, 0, 3204},
		{191, 0x22, 100, 100, 0, 18}, {192, 0x22, 252, 252, 0, 0}, {194, 0x02, 64, 46, 0, 0x002e00240024ULL},
		{195, 0x3a, 100, 100, 0, 0}, {196, 0x32, 252, 252, 0, 0}, {197, 0x32, 252, 252, 0, 0}, {198, 0x30, 252, 252, 0, 0},
		{199, 0x36, 200, 200, 0, 0}, {200, 0x2a, 100, 100, 0, 0}, {223, 0x32, 252, 252, 0, 0}, {225, 0x32, 100, 100, 0, 9812},
	};
	static const struct { int id, flags, value, worst, th; uint64_t raw; } ssd_attrs[] = {
		{5, 0x33, 100, 100, 10, 0}, {9, 0x32, 97, 97, 0, 13021}, {12, 0x32, 99, 99, 0, 912}, {177, 0x13, 93, 93, 0, 57},
		{179, 0x13, 100, 100, 10, 0}, {181, 0x32, 100, 100, 10, 0}, {182, 0x32, 100, 100, 10, 0}, {183, 0x13, 100, 100, 10, 0},
		{187, 0x32, 100, 100, 0, 0}, {190, 0x32, 62, 48, 0, 38}, {195, 0x1a, 200, 200, 0, 0}, {199, 0x3e, 100, 100, 0, 0},
		{235, 0x12, 99, 99, 0, 37}, {241, 0x32, 99, 99, 0, 21474836480ULL},
	};
	int n = ssd ? (int)(sizeof(ssd_attrs) / sizeof(ssd_attrs[0])) : (int)(sizeof(attrs) / sizeof(attrs[0]));
	data[0] = 0x10;
	for (int i = 0; i < n; i++) {
		int id = ssd ? ssd_attrs[i].id : attrs[i].id, flags = ssd ? ssd_attrs[i].flags : attrs[i].flags;
		int value = ssd ? ssd_attrs[i].value : attrs[i].value, worst = ssd ? ssd_attrs[i].worst : attrs[i].worst, th = ssd ? ssd_attrs[i].th : attrs[i].th;
		uint64_t raw = ssd ? ssd_attrs[i].raw : attrs[i].raw;
		if (worn && id == 5) raw = 24;
		if (worn && id == 197) raw = 3;
		if (failing && id == 5) { raw = 2210; value = 9; }
		if (!strcmp(mode, "ssd-worn") && id == 177) { value = 7; raw = 2790; }
		if (!strcmp(mode, "ssd-hot") && id == 190) raw = 62;
		if (!strcmp(mode, "ssd-old") && id == 177) { id = 231; value = 88; raw = 88; }
		Attr(data, i, id, flags, value, worst < value ? worst : value, raw);
		thresh[2 + i * 12] = id;
		thresh[2 + i * 12 + 1] = th;
	}
	data[1] = 0;
	Sign(data, 0);
	Sign(thresh, 0);
	plat_append_line(journal, "start smart_data"); plat_append_line(journal, "done smart_data 0");
	plat_append_line(journal, "start smart_thresh"); plat_append_line(journal, "done smart_thresh 0");
	*have_thresh = 1;
	if (!strcmp(mode, "swapped"))
		for (uint8_t *b[3] = {identify, data, thresh}, **p = b; p < b + 3; p++)
			for (int i = 0; i < 512; i += 2) { uint8_t x = (*p)[i]; (*p)[i] = (*p)[i + 1]; (*p)[i + 1] = x; }
	return 0;
}
