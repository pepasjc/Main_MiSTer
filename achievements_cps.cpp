// achievements_cps.cpp — RetroAchievements for jotego's Capcom CPS1/1.5/2 cores
//
// Arcade (RC_CONSOLE_ARCADE, 27). RA's arcade sets were authored against
// FinalBurn Neo, which exposes exactly one buffer for CPS hardware: CpsRamFF,
// the 64 KB of 68000 work RAM at 0xFF0000-0xFFFFFF (retro_memory.cpp,
// StateGetMainRamAcb). FBNeo stores 68K RAM with the two bytes of every 16-bit
// word swapped, so RA address N is the 68K byte at 0xFF0000 + (N ^ 1).
//
// FPGA side (ra_ram_mirror_cps.sv in the jtcores fork): a shadow of the work
// RAM, fed by snooping the SDRAM bank-0 write bus, is copied to DDRAM every
// VBlank as a Full Mirror — 64 KB at offset 0x100. Each 68K word is stored
// as-is in a little-endian DDR lane, so DDR byte k is the 68K byte at
// 0xFF0000 + (k ^ 1): already FBNeo's layout, no swap on this side. The header
// busy flag is held while the copy runs.
//
// The ARM takes a seqlock-style snapshot (frame counter equal and busy clear
// before and after the copy) so rcheevos never evaluates a torn frame.
//
// Game identity: RA hashes arcade games by set name (md5 of "sfa3" for
// sfa3.zip). The core is launched from an .mra, so achievements_init() feeds
// "<setname>.zip" from the MRA through achievements_load_game().

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include <string.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_hash.h"
#endif

#define CPS_RAM_OFFSET 0x100
#define CPS_RAM_SIZE   0x10000

static uint8_t  g_cps_snap[CPS_RAM_SIZE];
static int      g_cps_snap_valid = 0;
static uint32_t g_cps_last_frame = 0;
static uint32_t g_cps_frames = 0;
static uint32_t g_cps_torn = 0;

static void cps_init(void)
{
	memset(g_cps_snap, 0, sizeof(g_cps_snap));
	g_cps_snap_valid = 0;
	g_cps_last_frame = 0;
	g_cps_frames = 0;
	g_cps_torn = 0;
}

static uint32_t cps_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	(void)map;
	for (uint32_t i = 0; i < num_bytes; i++) {
		uint32_t a = address + i;
		buffer[i] = (a < CPS_RAM_SIZE && g_cps_snap_valid) ? g_cps_snap[a] : 0;
	}
	return num_bytes;
}

// Copy the mirror into g_cps_snap. Returns the frame it belongs to, or 0 when
// the FPGA was writing during the copy (caller retries on the next poll).
static uint32_t cps_take_snapshot(void *map)
{
	uint32_t f0 = ra_ramread_frame(map);
	if (ra_ramread_busy(map)) return 0;
	__sync_synchronize();
	memcpy(g_cps_snap, (const uint8_t *)map + CPS_RAM_OFFSET, CPS_RAM_SIZE);
	__sync_synchronize();
	if (ra_ramread_busy(map) || ra_ramread_frame(map) != f0) {
		g_cps_torn++;
		return 0;
	}
	return f0;
}

static int cps_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map) return 1;

	uint32_t frame = ra_ramread_frame(map);
	if (frame == g_cps_last_frame) return 1;

	uint32_t snap_frame = cps_take_snapshot(map);
	if (!snap_frame) return 1;

	g_cps_snap_valid = 1;
	g_cps_last_frame = snap_frame;
	g_cps_frames++;
	ra_frame_processed(snap_frame);
	rc_client_do_frame((rc_client_t *)client);

	if ((g_cps_frames % 3600) == 1) {
		char hex[16 * 3 + 1] = {};
		for (int i = 0; i < 16; i++) sprintf(hex + i * 3, "%02X ", g_cps_snap[i]);
		ra_log_write("CPS: frame=%u evaluated=%u torn=%u RA[0000..000F]: %s\n",
			snap_frame, g_cps_frames, g_cps_torn, hex);
	}
	return 1;
#else
	(void)map; (void)client; (void)game_loaded;
	return 0;
#endif
}

static int cps_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
	// Arcade hashing never opens the file: it is the md5 of the file name
	// without extension, so "<setname>.zip" is all it needs.
	if (rc_hash_generate_from_file(md5_hex_out, 27, rom_path)) {
		ra_log_write("CPS: set '%s' hash %s\n", rom_path, md5_hex_out);
		return 1;
	}
	ra_log_write("CPS: rc_hash_generate_from_file failed for '%s'\n", rom_path);
#endif
	return 0;
}

static int cps_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("CPS: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	uint8_t vmaj = 0, vmin = 0;
	ra_ramread_get_core_version(map, &vmaj, &vmin);
	ra_log_write("CPS: Full Mirror of 68K work RAM (64 KB), FPGA v%u.%u\n", vmaj, vmin);
	return 1;
}

#define CPS_HANDLER(sym, core_name)                 \
	const console_handler_t sym = {                 \
		.init = cps_init,                           \
		.reset = cps_init,                          \
		.read_memory = cps_read_memory,             \
		.poll = cps_poll,                           \
		.calculate_hash = cps_calculate_hash,       \
		.set_hardcore = NULL,                       \
		.detect_protocol = cps_detect_protocol,     \
		.console_id = 27,                           \
		.name = core_name,                          \
		.hardcore_protected = 0                     \
	};

CPS_HANDLER(g_console_jtcps1,  "JTCPS1")
CPS_HANDLER(g_console_jtcps15, "JTCPS15")
CPS_HANDLER(g_console_jtcps2,  "JTCPS2")
