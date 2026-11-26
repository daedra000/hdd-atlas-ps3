/*
health_test.c: health.c's decoders and journal on a PC, against ata_host.c's
made-up drive in each of its modes (HOST_ATA), and damaged sectors.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/plat.h"
#include "../../src/health.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fflush(stdout); exit(1); } } while (0)

static uint8_t id[512], data[512], th[512];
static char journal[512];

static int Read(const char *mode, health_t *h, char *why)
{
	int have = 0;
	setenv("HOST_ATA", mode, 1);
	int r = health_read_drive(journal, id, data, th, &have, why, 200);
	if (r == 0) CHECK(health_parse(id, data, have ? th : NULL, h) == 0);
	return r;
}

int main(void)
{
	health_t h;
	char why[200], froze[64];
	snprintf(journal, sizeof(journal), "%s/smart_journal_test.txt", plat_save_dir());
	remove(journal);
	CHECK(!health_journal_ok_before(journal) && !health_journal_froze(journal, froze, sizeof(froze)));

	// a healthy drive, its words as they are and byte-swapped: the same reading
	CHECK(Read("", &h, why) == 0);
	CHECK(!strcmp(h.model, "ST1000LM024 HN-M101MBB") && !strcmp(h.firmware, "2BA30001") && !strcmp(h.serial, "S2TXJ9AB123456"));
	CHECK(h.bytes == 1953525168ULL * 512 && h.rotation == 5400 && h.smart_supported && h.smart_enabled && !h.swapped);
	CHECK(h.count == 19 && h.have_thresh && h.data_checksum_ok);
	CHECK(h.temperature == 36 && h.power_on_hours == 6121 && h.power_cycles == 3204 && h.reallocated == 0 && h.pending == 0);
	CHECK(h.status == HEALTH_GOOD);
	health_t plain = h;
	CHECK(Read("swapped", &h, why) == 0);
	CHECK(h.swapped && !strcmp(h.model, plain.model) && h.count == plain.count && h.temperature == 36 && h.status == HEALTH_GOOD);
	CHECK(!memcmp(h.a, plain.a, sizeof(h.a)));
	CHECK(health_journal_ok_before(journal) && !health_journal_froze(journal, froze, sizeof(froze)));

	// wear: caution; past a pre-failure threshold: failing
	CHECK(Read("worn", &h, why) == 0 && h.status == HEALTH_CAUTION && h.reallocated == 24 && h.pending == 3);
	CHECK(Read("failing", &h, why) == 0 && h.status == HEALTH_FAILING && strstr(h.why, "Reallocated sectors"));
	printf("failing: %s\n", h.why);

	// a hard disk: no life left, power cycles as ever
	CHECK(Read("", &h, why) == 0 && !h.ssd && h.life_left == -1 && !health_hot(&h));
	CHECK(!strcmp(health_attr_name(177, 0), "Wear range delta"));

	// an SSD: its life left from 177 (Samsung), its own names, a warmer limit
	CHECK(Read("ssd", &h, why) == 0 && h.ssd && h.rotation == 1 && !strcmp(h.model, "Samsung SSD 860 EVO 500GB"));
	CHECK(h.life_left == 93 && h.life_attr == 177 && h.temperature == 38 && h.power_on_hours == 13021 && h.status == HEALTH_GOOD);
	CHECK(!strcmp(health_attr_name(177, 1), "Wear levelling count") && !strcmp(health_attr_name(179, 1), "Used reserve blocks"));
	CHECK(Read("ssd-hot", &h, why) == 0 && h.temperature == 62 && !health_hot(&h) && h.status == HEALTH_GOOD);
	h.temperature = 71;
	CHECK(health_hot(&h));
	CHECK(Read("ssd-worn", &h, why) == 0 && h.life_left == 7 && h.status == HEALTH_CAUTION && h.why_kind == WHY_LIFE);
	printf("ssd-worn: %s\n", h.why);
	// word 217 not filled in: an SSD by its 231 (SandForce's life left)
	CHECK(Read("ssd-old", &h, why) == 0 && h.rotation == 0 && h.ssd && h.life_left == 88 && h.life_attr == 231);
	// the readings over time keep its life left; a 1.0.0 line (without) still reads
	{
		char hp[512];
		health_rec_t r[8];
		snprintf(hp, sizeof(hp), "%s/smart_history_test.txt", plat_save_dir());
		remove(hp);
		CHECK(Read("ssd", &h, why) == 0);
		plat_append_line(hp, "1\t1700000000\tSamsung SSD 860 EVO 500GB|S3***********6A\t12000 900 0 0 0 0 37 0");
		health_history_add(hp, &h, 1800000000);
		int n = health_history_load(hp, &h, r, 8);
		CHECK(n == 2 && r[0].life == -1 && r[0].hours == 12000 && r[1].life == 93 && r[1].hours == 13021);
		remove(hp);
	}

	// a damaged threshold sector isn't used; zeros aren't a drive
	CHECK(Read("", &h, why) == 0);
	th[100] ^= 0x55;
	CHECK(health_parse(id, data, th, &h) == 0 && !h.have_thresh && h.a[0].thresh == 0);
	uint8_t zero[512] = {0};
	CHECK(health_parse(zero, data, th, &h) != 0);

	// refused: an error, and the journal's still fine
	CHECK(Read("refuse", &h, why) != 0 && strstr(why, "IDENTIFY"));
	CHECK(!health_journal_froze(journal, froze, sizeof(froze)));

	// a freeze inside smart_data: found, and never asked again
	CHECK(Read("freeze", &h, why) != 0);
	CHECK(health_journal_froze(journal, froze, sizeof(froze)) && !strcmp(froze, "smart_data"));
	CHECK(Read("", &h, why) != 0 && !strcmp(why, "froze:smart_data"));
	remove(journal);
	// the app quit while a call was going: not a freeze
	plat_append_line(journal, "start open"); plat_append_line(journal, "done open 0"); plat_append_line(journal, "start smart_data");
	health_journal_quit(journal);
	CHECK(!health_journal_froze(journal, froze, sizeof(froze)));
	remove(journal);
	printf("health_test: all PASS\n");
	return 0;
}
