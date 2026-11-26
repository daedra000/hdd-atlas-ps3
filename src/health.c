/*
health.c: reading what the drive said (health.h). ATA's layouts (ACS-3):
IDENTIFY is 256 little-endian words, its strings two characters a word, high
byte first; SMART data is 30 attributes of 12 bytes from byte 2, the
thresholds the same shape. Through LV1 the words can come byte-swapped:
IDENTIFY's last word signs it with A5h, so whichever byte that's in tells
which, and SMART's sectors are swapped back to match.

An SSD (word 217 is 1) says how much of its flash's rated wear is left as a
normalized value out of 100, in the attribute its maker uses for it: 231 (SSD
Life Left: SandForce, Kingston, ADATA...), 233 (Media Wearout Indicator:
Intel), 202 (Percent Lifetime Remaining: Crucial, Micron), 177 (Wear
Leveling Count: Samsung), 169 (Remaining Life), 173 (Wear Leveling Count:
Crucial's older ones, SMI). The first one there is its life left.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "lang.h"
#include "health.h"

static unsigned Word(const uint8_t *b, int i, int sw)
{
	return sw ? (unsigned)(b[2 * i] << 8 | b[2 * i + 1]) : (unsigned)(b[2 * i] | b[2 * i + 1] << 8);
}

static void Ata_String(const uint8_t *b, int first, int words, int sw, char *out)
{
	char raw[64];
	int n = 0;
	for (int k = 0; k < words; k++) {
		unsigned w = Word(b, first + k, sw);
		raw[n++] = (char)(w >> 8);
		raw[n++] = (char)(w & 0xff);
	}
	int a = 0, z = n;
	while (a < z && raw[a] == ' ') a++;
	while (z > a && (raw[z - 1] == ' ' || raw[z - 1] == 0)) z--;
	int j = 0;
	for (int i = a; i < z; i++) out[j++] = raw[i] >= 0x20 && raw[i] < 0x7f ? raw[i] : '?';
	out[j] = 0;
}

static int Sum_Ok(const uint8_t *b)
{
	unsigned s = 0;
	for (int i = 0; i < 512; i++) s += b[i];
	return (s & 0xff) == 0;
}

#define VALID(w) (((w) & 0xC000) == 0x4000)		// words 82-84 / 85-87 count only when 83 / 87 say so

static int64_t Raw_Of(const health_t *h, int id, uint64_t mask)
{
	for (int i = 0; i < h->count; i++) if (h->a[i].id == id) return (int64_t)(h->a[i].raw & mask);
	return -1;
}

static const struct { int id; const char *what; } worn[] = {
	{5, N_("reallocated sectors")}, {197, N_("sectors waiting to be reallocated")}, {198, N_("uncorrectable sectors")},
	{187, N_("reported uncorrectable errors")}, {196, N_("reallocation events")},
};

const char *health_worn_what(int id)
{
	for (size_t k = 0; k < sizeof(worn) / sizeof(worn[0]); k++) if (worn[k].id == id) return worn[k].what;
	return "";
}

int health_hot(const health_t *h) { return h->temperature > (h->ssd ? 70 : 55); }		// (an SSD's controller runs warmer, and is rated for it)

static void Judge(health_t *h)
{
	h->status = HEALTH_GOOD;
	h->why_kind = WHY_GOOD;
	snprintf(h->why, sizeof(h->why), "No reallocated, pending or uncorrectable sectors, and nothing past its threshold.");
	// past a threshold: a pre-failure attribute means the drive says it's failing
	for (int i = 0; i < h->count; i++) {
		const health_attr_t *a = &h->a[i];
		if (!a->thresh || a->thresh == 0xFE || a->value > a->thresh) continue;
		int pre = a->flags & 1;
		if (pre || h->status == HEALTH_GOOD) {
			h->status = pre ? HEALTH_FAILING : HEALTH_CAUTION;
			h->why_kind = pre ? WHY_FAILING : WHY_PAST;
			h->why_attr = a->id;
			h->why_value = a->value;
			h->why_thresh = a->thresh;
			snprintf(h->why, sizeof(h->why), "%s is at %u, at or below its threshold of %u%s.", health_attr_name(a->id, h->ssd), a->value,
				a->thresh, pre ? ": the drive says it's failing" : "");
			if (pre) return;
		}
	}
	if (h->status != HEALTH_GOOD) return;
	if (h->life_left >= 0 && h->life_left <= HEALTH_LIFE_LOW) {
		h->status = HEALTH_CAUTION;
		h->why_kind = WHY_LIFE;
		h->why_value = h->life_left;
		snprintf(h->why, sizeof(h->why), "Life left is at %d%%: the flash is nearly worn out. Keep a backup of what matters.", h->life_left);
		return;
	}
	for (size_t k = 0; k < sizeof(worn) / sizeof(worn[0]); k++) {
		int64_t v = Raw_Of(h, worn[k].id, 0xffffffffULL);
		if (v > 0) {
			h->status = HEALTH_CAUTION;
			h->why_kind = WHY_WORN;
			h->why_attr = worn[k].id;
			h->why_count = v;
			snprintf(h->why, sizeof(h->why), h->ssd ? "%lld %s: the flash is wearing. Keep a backup of what matters." :
				"%lld %s: the surface is wearing. Keep a backup of what matters.", (long long)v, worn[k].what);
			return;
		}
	}
	if (health_hot(h)) {
		h->status = HEALTH_CAUTION;
		h->why_kind = WHY_HOT;
		h->why_value = h->temperature;
		snprintf(h->why, sizeof(h->why), "Running hot (%d \xc2\xb0""C). Check the console's vents.", h->temperature);
	}
}

int health_parse(const uint8_t identify[512], const uint8_t *data, const uint8_t *thresh, health_t *h)
{
	memset(h, 0, sizeof(*h));
	h->temperature = h->life_left = -1;
	h->power_on_hours = h->power_cycles = h->reallocated = h->pending = h->uncorrectable = h->crc_errors = -1;
	int nonzero = 0;
	for (int i = 0; i < 512; i++) nonzero |= identify[i];
	if (!nonzero) return -1;
	int sw = h->swapped = identify[510] == 0xA5 ? 0 : identify[511] == 0xA5 ? 1 : 0;
	Ata_String(identify, 10, 10, sw, h->serial);
	Ata_String(identify, 23, 4, sw, h->firmware);
	Ata_String(identify, 27, 20, sw, h->model);
	if (!h->model[0]) return -1;
	unsigned w83 = Word(identify, 83, sw), w87 = Word(identify, 87, sw);
	uint64_t sectors;
	if (VALID(w83) && (w83 & (1 << 10)))
		sectors = (uint64_t)Word(identify, 100, sw) | (uint64_t)Word(identify, 101, sw) << 16 |
			(uint64_t)Word(identify, 102, sw) << 32 | (uint64_t)Word(identify, 103, sw) << 48;
	else
		sectors = (uint64_t)Word(identify, 60, sw) | (uint64_t)Word(identify, 61, sw) << 16;
	unsigned w106 = Word(identify, 106, sw), lsize = 512;
	if (VALID(w106) && (w106 & (1 << 12))) lsize = (Word(identify, 117, sw) | Word(identify, 118, sw) << 16) * 2;
	if (lsize < 512 || lsize > 65536) lsize = 512;
	h->bytes = sectors * lsize;
	h->rotation = Word(identify, 217, sw);
	if (h->rotation == 0xFFFF) h->rotation = 0;
	h->ssd = h->rotation == 1;
	h->smart_supported = VALID(w83) && (Word(identify, 82, sw) & 1);
	h->smart_enabled = VALID(w87) && (Word(identify, 85, sw) & 1);
	if (!data) return 0;

	uint8_t d[512], t[512];
	memcpy(d, data, 512);
	if (thresh) memcpy(t, thresh, 512);
	if (sw)
		for (int i = 0; i < 512; i += 2) {
			uint8_t x = d[i]; d[i] = d[i + 1]; d[i + 1] = x;
			if (thresh) { x = t[i]; t[i] = t[i + 1]; t[i + 1] = x; }
		}
	h->data_checksum_ok = Sum_Ok(d);
	h->have_thresh = thresh && Sum_Ok(t);		// (a threshold sector that doesn't add up isn't used)
	for (int i = 0; i < HEALTH_ATTRS; i++) {
		const uint8_t *e = d + 2 + i * 12;
		if (!e[0]) continue;
		health_attr_t *a = &h->a[h->count++];
		a->id = e[0];
		a->flags = (uint16_t)(e[1] | e[2] << 8);
		a->value = e[3];
		a->worst = e[4];
		for (int k = 5; k >= 0; k--) a->raw = a->raw << 8 | e[5 + k];
		if (h->have_thresh)
			for (int j = 0; j < HEALTH_ATTRS; j++)
				if (t[2 + j * 12] == a->id) { a->thresh = t[2 + j * 12 + 1]; break; }
	}
	int64_t temp = Raw_Of(h, 194, 0xff);
	if (temp <= 0 || temp > 120) temp = Raw_Of(h, 190, 0xff);
	h->temperature = temp > 0 && temp <= 120 ? (int)temp : -1;
	h->power_on_hours = Raw_Of(h, 9, 0xffffffffULL);
	h->power_cycles = Raw_Of(h, 12, 0xffffffffULL);
	h->reallocated = Raw_Of(h, 5, 0xffffffffULL);
	h->pending = Raw_Of(h, 197, 0xffffffffULL);
	h->uncorrectable = Raw_Of(h, 198, 0xffffffffULL);
	h->crc_errors = Raw_Of(h, 199, 0xffffffffULL);
	// an SSD that doesn't fill in word 217 (older ones): known by the wear attributes only SSDs have
	if (!h->rotation && (Raw_Of(h, 231, 1) >= 0 || Raw_Of(h, 233, 1) >= 0 || Raw_Of(h, 202, 1) >= 0 || Raw_Of(h, 169, 1) >= 0)) h->ssd = 1;
	if (h->ssd) {
		static const int life_ids[] = {231, 233, 202, 177, 169, 173};
		for (size_t k = 0; k < sizeof(life_ids) / sizeof(life_ids[0]) && h->life_left < 0; k++)
			for (int i = 0; i < h->count; i++)
				if (h->a[i].id == life_ids[k] && h->a[i].value >= 1 && h->a[i].value <= 100) {		// (out of 100: anything else isn't a percentage)
					h->life_left = h->a[i].value;
					h->life_attr = life_ids[k];
					break;
				}
	}
	Judge(h);
	return 0;
}

const char *health_attr_name(int id, int ssd)
{
	// on an SSD (the names its makers give them, where they differ from a disk's or a disk hasn't the number)
	static const struct { int id; const char *name; } ssd_names[] = {
		{169, N_("Life left")}, {177, N_("Wear levelling count")}, {178, N_("Used reserve blocks")}, {179, N_("Used reserve blocks")},
		{180, N_("Unused reserve blocks")}, {202, N_("Life left")}, {230, N_("Media wearout")}, {232, N_("Reserved space left")},
		{246, N_("Total written")},
	};
	for (size_t i = 0; ssd && i < sizeof(ssd_names) / sizeof(ssd_names[0]); i++) if (ssd_names[i].id == id) return ssd_names[i].name;
	static const struct { int id; const char *name; } names[] = {
		{1, N_("Read error rate")}, {2, N_("Throughput performance")}, {3, N_("Spin-up time")}, {4, N_("Start/stop count")},
		{5, N_("Reallocated sectors")}, {7, N_("Seek error rate")}, {8, N_("Seek time performance")}, {9, N_("Power-on hours")},
		{10, N_("Spin retry count")}, {11, N_("Calibration retries")}, {12, N_("Power cycles")}, {170, N_("Reserved space left")},
		{171, N_("Program fails")}, {172, N_("Erase fails")}, {173, N_("Wear levelling count")}, {174, N_("Unexpected power losses")},
		{177, N_("Wear range delta")}, {181, N_("Program fails")}, {182, N_("Erase fails")}, {183, N_("Interface downshifts")},
		{184, N_("End-to-end errors")}, {187, N_("Reported uncorrectable")}, {188, N_("Command timeouts")}, {189, N_("High fly writes")},
		{190, N_("Airflow temperature")}, {191, N_("G-sense errors")}, {192, N_("Power-off retracts")}, {193, N_("Load cycles")},
		{194, N_("Temperature")}, {195, N_("ECC recovered")}, {196, N_("Reallocation events")}, {197, N_("Pending sectors")},
		{198, N_("Uncorrectable sectors")}, {199, N_("Interface CRC errors")}, {200, N_("Write error rate")}, {220, N_("Disk shift")},
		{222, N_("Loaded hours")}, {223, N_("Load retries")}, {224, N_("Load friction")}, {226, N_("Load-in time")},
		{231, N_("Life left")}, {233, N_("Media wearout")}, {240, N_("Head flying hours")}, {241, N_("Total written")},
		{242, N_("Total read")}, {254, N_("Free fall events")},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (names[i].id == id) return names[i].name;
	return N_("Vendor specific");
}

void health_mask_serial(const char *serial, char *out, int size)
{
	int len = (int)strlen(serial), j = 0;
	for (int i = 0; i < len && j < size - 1; i++) out[j++] = len > 6 && (i < 2 || i >= len - 2) ? serial[i] : '*';
	out[j] = 0;
}

volatile int health_quitting;

void health_journal_quit(const char *path) { plat_append_line(path, "quit"); }

int health_journal_froze(const char *path, char *froze, int size)
{
	size_t len;
	char *b = (char *)plat_read_file(path, &len), open_call[64] = "";
	if (!b) return 0;
	for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
		char call[64];
		if (sscanf(line, "start %63s", call) == 1) snprintf(open_call, sizeof(open_call), "%s", call);
		else if (sscanf(line, "done %63s", call) == 1 && !strcmp(call, open_call)) open_call[0] = 0;
		else if (!strcmp(line, "quit")) open_call[0] = 0;		// (the app quit while it was going: not frozen)
		else if (!strncmp(line, "froze ", 6)) { snprintf(open_call, sizeof(open_call), "%.60s", line + 6); break; }
	}
	free(b);
	if (!open_call[0]) return 0;
	snprintf(froze, size, "%s", open_call);
	return 1;
}

int health_journal_ok_before(const char *path)
{
	size_t len;
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return 0;
	int ok = strstr(b, "done smart_data 0") != NULL;
	free(b);
	return ok;
}

static void Drive_Key(const health_t *h, char *out, int size)
{
	char masked[32];
	health_mask_serial(h->serial, masked, sizeof(masked));
	snprintf(out, size, "%s|%s", h->model, masked);
	for (char *c = out; *c; c++) if (*c == '\t' || *c == '\n') *c = ' ';
}

void health_history_add(const char *path, const health_t *h, int64_t when)
{
	char key[64], line[256];
	Drive_Key(h, key, sizeof(key));
	snprintf(line, sizeof(line), "2\t%lld\t%s\t%lld %lld %lld %lld %lld %lld %d %d %d", (long long)when, key, (long long)h->power_on_hours,
		(long long)h->power_cycles, (long long)h->reallocated, (long long)h->pending, (long long)h->uncorrectable, (long long)h->crc_errors,
		h->temperature, h->status, h->life_left);		// (2: with the life left; 1.0.0 wrote 1, without, and reads only its own)
	plat_append_line(path, line);
	// past 512 readings (of every drive it's read), the last 256 again: it's read whole each time
	size_t len;
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return;
	int lines = 0;
	for (char *c = b; *c; c++) lines += *c == '\n';
	if (lines > 512) {
		char *c = b;
		for (int skip = lines - 256; skip > 0 && *c; c++) if (*c == '\n') skip--;
		plat_write_file(path, c, strlen(c));
	}
	free(b);
}

int health_history_last(const char *path, health_rec_t *out) { return health_history_load(path, NULL, out, 1); }

// (h NULL: any drive's)
int health_history_load(const char *path, const health_t *h, health_rec_t *out, int max)
{
	char key[64] = "";
	size_t len;
	if (h) Drive_Key(h, key, sizeof(key));
	char *b = (char *)plat_read_file(path, &len);
	if (!b) return 0;
	int n = 0;
	for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
		health_rec_t r;
		long long when;
		char drive[64];
		int ver;
		memset(&r, 0, sizeof(r));
		r.life = -1;
		if (sscanf(line, "%d\t", &ver) != 1 || (ver != 1 && ver != 2)) continue;
		int got = sscanf(line, "%*d\t%lld\t%63[^\t]\t%lld %lld %lld %lld %lld %lld %d %d %d", &when, drive, (long long *)&r.hours, (long long *)&r.cycles,
			(long long *)&r.reallocated, (long long *)&r.pending, (long long *)&r.uncorrectable, (long long *)&r.crc, &r.temperature, &r.status, &r.life);
		if (got < 10 || (ver == 2 && got != 11)) continue;
		if (ver == 1) r.life = -1;
		if (h && strcmp(drive, key)) continue;
		r.when = when;
		snprintf(r.drive, sizeof(r.drive), "%s", drive);
		if (n == max) { memmove(out, out + 1, (max - 1) * sizeof(*out)); n--; }		// (the last max)
		out[n++] = r;
	}
	free(b);
	return n;
}
