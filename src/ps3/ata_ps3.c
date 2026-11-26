/*
ata_ps3.c: the internal HDD's IDENTIFY and SMART sectors, through lv2's
storage syscalls (health.h): 600 open, 616 execute device command with LV1's
SEND_ATA_COMMAND (2), 601 close. Device 0x0101000000000007 is the internal
drive. The 32-byte command block is the start of Linux's ps3disk.c
lv1_ata_cmnd_block; lv2 fills in the buffer part with its own.

Only reads: IDENTIFY DEVICE (ECh), SMART READ DATA (B0h/D0h) and SMART READ
THRESHOLDS (B0h/D1h), PIO in, one sector each. Every call is journaled first
(health.h): a call the console once froze inside is never made again.
*/
#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <ppu-lv2.h>
#include "../plat.h"
#include "../health.h"

#define ATA_HDD		0x0101000000000007ULL
#define LV1_ATA		2			// LV1_STORAGE_SEND_ATA_COMMAND
#define PROTO_PIO_IN	1
#define DIR_READ	1

typedef struct {
	u16 features, sector_count, lba_low, lba_mid, lba_high;
	u8 device, command;
	u32 is_ext, proto, in_out, size;
	u32 pad;
	u64 buffer;
	u32 arglen, pad2;
} __attribute__((packed)) ata_block_t;

static u8 sector[512] __attribute__((aligned(128)));

static s32 Storage_Open(u64 dev, u32 *handle) { lv2syscall4(600, dev, 0, (u64)(uintptr_t)handle, 0); return_to_user_prog(s32); }
static s32 Storage_Close(u32 handle) { lv2syscall1(601, handle); return_to_user_prog(s32); }
static s32 Storage_Command(u32 h, u64 cmd, void *in, u64 inlen, void *out, u64 outlen, u64 *status)
{
	lv2syscall7(616, h, cmd, (u64)(uintptr_t)in, inlen, (u64)(uintptr_t)out, outlen, (u64)(uintptr_t)status);
	return_to_user_prog(s32);
}

static const char *journal;

static int Start(const char *call)
{
	char line[80];
	if (health_quitting) return 0;		// (quitting: no new call to the drive)
	snprintf(line, sizeof(line), "start %s", call);
	return plat_append_line(journal, line);		// (not on the disk: the call isn't made)
}

static void Done(const char *call, s32 rc)
{
	char line[80];
	snprintf(line, sizeof(line), "done %s %d", call, (int)rc);
	plat_append_line(journal, line);
	plat_log("health: %s = 0x%08x", call, (unsigned)rc);
}

// one sector in: command (and SMART's feature), journaled; into out
static s32 Read_Sector(u32 h, const char *call, u8 command, u16 features, u8 *out)
{
	ata_block_t blk;
	memset(&blk, 0, sizeof(blk));
	blk.features = features;
	blk.sector_count = 1;
	if (command == 0xB0) { blk.lba_mid = 0x4F; blk.lba_high = 0xC2; }		// (SMART's key)
	blk.command = command;
	blk.proto = PROTO_PIO_IN;
	blk.in_out = DIR_READ;
	blk.size = 512;
	if (!Start(call)) return 0x7fffffff;
	u64 status = 0;
	memset(sector, 0, sizeof(sector));
	s32 r = Storage_Command(h, LV1_ATA, &blk, 32, sector, 512, &status);
	Done(call, r);
	if (r == 0) memcpy(out, sector, 512);
	return r;
}

int health_read_drive(const char *journal_path, uint8_t *identify, uint8_t *data, uint8_t *thresh, int *have_thresh,
	char *why, int why_size)
{
	char froze[64];
	journal = journal_path;
	*have_thresh = 0;
	if (health_journal_froze(journal, froze, sizeof(froze))) {
		snprintf(why, why_size, "froze:%s", froze);
		return -1;
	}
	u32 h = 0;
	if (!Start("open")) { snprintf(why, why_size, "The journal couldn't be written in USRDIR, so the drive wasn't asked."); return -1; }
	s32 r = Storage_Open(ATA_HDD, &h);
	Done("open", r);
	if (r != 0) { snprintf(why, why_size, "lv2 wouldn't open the drive (0x%08x).", (unsigned)r); return -1; }
	int ok = 0;
	r = Read_Sector(h, "identify", 0xEC, 0, identify);
	if (r != 0) snprintf(why, why_size, "The drive didn't answer IDENTIFY (0x%08x).", (unsigned)r);
	else {
		r = Read_Sector(h, "smart_data", 0xB0, 0xD0, data);
		if (r != 0) snprintf(why, why_size, "The drive didn't give its SMART data (0x%08x).", (unsigned)r);
		else {
			ok = 1;
			*have_thresh = Read_Sector(h, "smart_thresh", 0xB0, 0xD1, thresh) == 0;
		}
	}
	int journaled = Start("close");		// (closed whatever the journal says: the handle mustn't stay open)
	r = Storage_Close(h);
	if (journaled) Done("close", r);
	return ok ? 0 : -1;
}
