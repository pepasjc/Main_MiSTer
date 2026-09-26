// achievements_jotego.cpp — RetroAchievements for jotego arcade cores (jtcores)
//
// Arcade (RC_CONSOLE_ARCADE, 27). RA's arcade sets were authored against
// FinalBurn Neo, which exposes one RAM buffer per driver (retro_memory.cpp,
// StateGetMainRamAcb): "CpsRamFF" for CPS1/CPS2, and for most other drivers the
// "All Ram" block, a concatenation of the driver's RAM buffers in MemIndex order.
// FBNeo stores 68000 RAM with the two bytes of every 16-bit word swapped.
//
// FPGA side (jtframe_ra_mirror.v in the jtcores fork, JTFRAME_RA_MIRROR): a
// 64 KB window of SDRAM bank 0 (JTFRAME_RA_WRAM), fed by snooping the bank-0
// write bus, is copied to DDRAM every VBlank as a Full Mirror at offset 0x100.
// Each word is stored as-is in a little-endian DDR lane, so mirror byte k is the
// 68K byte at (k ^ 1): already FBNeo's layout for 68000 RAM. The header busy
// flag is held while the copy runs.
//
// Each core (and, where one core runs several drivers, each set) has a region
// table mapping RA address ranges onto mirror offsets. RA addresses outside
// every region read as 0.
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
#include "user_io.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_hash.h"
#endif

#define JT_MIRROR_OFFSET 0x100
#define JT_MIRROR_SIZE   0x10000

typedef struct {
	uint32_t ra;      // first RA address of the region
	uint32_t len;     // bytes
	uint32_t mirror;  // offset of that RA address in the 64 KB mirror
} jt_region_t;

typedef struct {
	const char *set_prefix;     // setname prefix this layout applies to; NULL = default
	const jt_region_t *regions;
	int count;
} jt_layout_t;

typedef struct {
	const char *core;           // CORENAME, as reported without the RA_ prefix
	const jt_layout_t *layouts; // first matching set_prefix wins, then the NULL default
	int count;
} jt_core_t;

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

// CPS1/1.5/2: CpsRamFF = 68K 0xFF0000-0xFFFFFF, SDRAM word 0x300000.
static const jt_region_t r_cps[]    = { { 0x0000, 0x10000, 0x0000 } };
// TMNT: All Ram starts with Drv68KRam (68K 0x060000, 16 KB) = SDRAM word 0x102000.
static const jt_region_t r_tmnt[]   = { { 0x0000, 0x4000, 0x4000 } };
// Rastan / Operation Wolf: Taito68KRam1 at 0 = SDRAM word 0x100000.
static const jt_region_t r_rastan[] = { { 0x0000, 0x8000, 0x0000 } };
// Toki: Drv68KRAM (68K 0x60000) at RA 0x1800. Cabal: DrvMainRAM (0x40000) at 0.
// Both at SDRAM word 0x80000.
static const jt_region_t r_toki[]   = { { 0x1800, 0xE000, 0x0000 } };
static const jt_region_t r_cabal[]  = { { 0x0000, 0x10000, 0x0000 } };
// Big Karnak: palette RAM first (0x800), then Drv68KRAM (0xFF0000) = SDRAM word 0x100000.
static const jt_region_t r_gae1[]   = { { 0x0800, 0x10000, 0x0000 } };
// WWF Superstars: Drv68KRAM (0x1C0000, 16 KB) at 0 = SDRAM word 0x100000.
static const jt_region_t r_wwfss[]  = { { 0x0000, 0x4000, 0x0000 } };
// Data East DEC0 / Sly Spy: Drv68KRam at 0 = SDRAM word 0.
static const jt_region_t r_dec0[]   = { { 0x0000, 0x4000, 0x0000 } };
// Sunset Riders / TMNT2: Drv68KRam (0x104000, 16 KB) at 0. It is a BRAM in the
// core, exported through the mem.yaml ra_tap (JTFRAME_RA_TAP), byte offset = RA.
static const jt_region_t r_riders[] = { { 0x0000, 0x4000, 0x0000 } };
// Konami Aliens family, 8-bit (no swap): one 8 KB work RAM BRAM through the RA
// tap. Aliens / Crime Fighters: RA 0-0x1FFF is CPU 0-0x1FFF, the BRAM itself.
// Super Contra: FBNeo puts bank RAM (CPU 0x5800, BRAM 0x1800) first, then work
// RAM (CPU 0x4000, BRAM 0) at RA 0x800.
static const jt_region_t r_aliens[] = { { 0x0000, 0x2000, 0x0000 } };
static const jt_region_t r_scontra[] = { { 0x0800, 0x1800, 0x0000 }, { 0x0000, 0x0800, 0x1800 } };
// Sega System 16A/16B/18: FBNeo's System16 All Ram starts with the 16 KB 68K work
// RAM (swapped), a BRAM tapped through its CPU port (mem.yaml dual_port ra_tap).
static const jt_region_t r_sega16[] = { { 0x0000, 0x4000, 0x0000 } };
// Namco System 1: FBNeo's All Ram from 0x8000 is object RAM, work RAM (0x9000),
// TriRAM (0x11000, not tapped), sound RAM (0x11800) and palette R (0x138A0).
// The core taps object/work/sound RAM and palette R at the same offsets, so the
// mirror is the All Ram window 0x8000-0x17FFF (8-bit CPUs, no swap).
static const jt_region_t r_shouse[] = { { 0x8000, 0x10000, 0x0000 } };
// Pang (Mitchell) / Bubble Bobble: the game module taps the RAM itself and writes
// it at its FBNeo All Ram offset (Pang: Z80 RAM 0, VideoRam 0x3800; Bubble
// Bobble: SharedRam 0x3300), so the 32 KB mirror is RA 1:1 (8-bit, no swap).
static const jt_region_t r_ident32[] = { { 0x0000, 0x8000, 0x0000 } };
// 16 KB mirror, RA 1:1
static const jt_region_t r_ident16[] = { { 0x0000, 0x4000, 0x0000 } };
// Capcom 8-bit (1942, 1943, Black Tiger, Ghosts'n Goblins): the main CPU module
// taps its work RAM at the FBNeo All Ram offsets (1943 also moves sprite RAM
// F000-FFFF to 0x2000), RA 1:1 up to 16 KB.
// Street Fighter: Drv68kRam (0xFF8000) is at RA 0x1800 and SDRAM word 0x44000,
// i.e. mirror 0x8000 of the window based at word 0x40000 (swapped 68K layout).
static const jt_region_t r_sf[]     = { { 0x1800, 0x8000, 0x8000 } };
// Double Dragon (6809 RAM, main-module tap), Ninja Gaiden (68K RAM BRAM, 16-bit
// tap), Vigilante (Z80 bus tap: work RAM 0, sprites 0x3000, palette 0x3100,
// video 0x3900) and Tehkan World Cup (work RAM 0 + shared RAM 0x5800) all land
// at their FBNeo All Ram offsets: RA 1:1.
static const jt_region_t r_ident8[]  = { { 0x0000, 0x2000, 0x0000 } };
static const jt_region_t r_vigil[]   = { { 0x0000, 0x4900, 0x0000 } };
static const jt_region_t r_wc[]      = { { 0x0000, 0x0800, 0x0000 }, { 0x5800, 0x0800, 0x5800 } };
// Konami 8-bit, tapped by CPU address at the FBNeo All Ram offsets: Contra (work
// RAM 0x1000 -> 0), Road Fighter (0x3000 -> 0), Mikie (zero page 0, sprite +
// work RAM from 0x100), Haunted Castle (palette/work RAM from 0x200, the banked
// 0x0800 window kept flat like FBNeo).
static const jt_region_t r_castle[]  = { { 0x0200, 0x1A00, 0x0200 } };
// CPS3: FBNeo's "Main RAM" (512 KB SH-2 RAM, RA N = SH-2 byte 0x02000000+(N^3),
// longwords stored little-endian). The core taps CPU writes to the 1 KB pages any
// CPS3 RA set reads (union of sfiii, sfiii2, jojo, redearth, sfiii3: 31 pages)
// into a packed 32 KB mirror, already in FBNeo byte order. A set revision that
// reads a new page needs the page added to jtcps3_ra_map.v and here.
static const jt_region_t r_cps3[] = {
	{ 0x08400, 0x400, 0x0000 }, { 0x09000, 0x400, 0x0400 }, { 0x0A000, 0x400, 0x0800 },
	{ 0x0D000, 0xC00, 0x0C00 }, { 0x0E400, 0x800, 0x1800 }, { 0x0F000, 0x400, 0x2000 },
	{ 0x10000, 0x400, 0x2400 }, { 0x10C00, 0x800, 0x2800 }, { 0x12C00, 0x400, 0x3000 },
	{ 0x14000, 0x400, 0x3400 }, { 0x15400, 0x400, 0x3800 }, { 0x16800, 0x400, 0x3C00 },
	{ 0x17800, 0x400, 0x4000 }, { 0x28000, 0x800, 0x4400 }, { 0x2D000, 0x400, 0x4C00 },
	{ 0x2E800, 0x400, 0x5000 }, { 0x30400, 0x800, 0x5400 }, { 0x5EC00, 0x400, 0x5C00 },
	{ 0x60400, 0x400, 0x6000 }, { 0x65000, 0x400, 0x6400 }, { 0x68C00, 0x800, 0x6800 },
	{ 0x6A400, 0xC00, 0x7000 },
};
// Irem M72 (Arcade-IremM72_MiSTer fork, core name M72): FBNeo All Ram puts the
// V30 work RAM at 0x26800 (16 KB) and the Z80 RAM at 0; the core mirrors V30 RAM
// to 0 and the first 4 KB of Z80 RAM to 0x4000 (little-endian CPUs, no swap).
static const jt_region_t r_m72[] = { { 0x26800, 0x4000, 0x0000 }, { 0x00000, 0x1000, 0x4000 } };
// Irem M92 (Arcade-IremM92_MiSTer fork, core name IremM92): FBNeo All Ram is
// sprites 0, video RAM 0x1000, V33 RAM 0x11000, sound RAM 0x21000, palette 0x25000.
// The sets read main RAM (all), video RAM (Hook, Ninja Baseball Bat Man), sound
// RAM and palette (Blade Master); the core packs the used parts into 64 KB
// (little-endian CPUs, no swap).
static const jt_region_t r_m92[] = {
	{ 0x11000, 0xE000, 0x0000 }, { 0x01000, 0x1800, 0xE000 },
	{ 0x25400, 0x0400, 0xF800 }, { 0x21000, 0x0400, 0xFC00 },
};

static const jt_layout_t l_cps[]    = { { NULL, r_cps, N(r_cps) } };
static const jt_layout_t l_tmnt[]   = { { NULL, r_tmnt, N(r_tmnt) } };
static const jt_layout_t l_rastan[] = { { NULL, r_rastan, N(r_rastan) } };
static const jt_layout_t l_toki[]   = { { "cabal", r_cabal, N(r_cabal) }, { NULL, r_toki, N(r_toki) } };
static const jt_layout_t l_gae1[]   = { { NULL, r_gae1, N(r_gae1) } };
static const jt_layout_t l_wwfss[]  = { { NULL, r_wwfss, N(r_wwfss) } };
static const jt_layout_t l_dec0[]   = { { NULL, r_dec0, N(r_dec0) } };
static const jt_layout_t l_riders[] = { { NULL, r_riders, N(r_riders) } };
static const jt_layout_t l_aliens[] = { { "scontra", r_scontra, N(r_scontra) }, { NULL, r_aliens, N(r_aliens) } };
static const jt_layout_t l_sega16[] = { { NULL, r_sega16, N(r_sega16) } };
static const jt_layout_t l_shouse[] = { { NULL, r_shouse, N(r_shouse) } };
static const jt_layout_t l_ident32[] = { { NULL, r_ident32, N(r_ident32) } };
static const jt_layout_t l_ident16[] = { { NULL, r_ident16, N(r_ident16) } };
static const jt_layout_t l_sf[]      = { { NULL, r_sf, N(r_sf) } };
static const jt_layout_t l_ident8[]  = { { NULL, r_ident8, N(r_ident8) } };
static const jt_layout_t l_vigil[]   = { { NULL, r_vigil, N(r_vigil) } };
static const jt_layout_t l_wc[]      = { { NULL, r_wc, N(r_wc) } };
static const jt_layout_t l_castle[]  = { { NULL, r_castle, N(r_castle) } };
// Namco System 86 (Rolling Thunder, Hopping Mappy) and Baraduke/Alien Sector
// (d_baraduke): the core writes object/tilemap RAM, MCU RAM and MCU internal RAM
// at each driver's All Ram offsets. Pac-Land: FBNeo's buffer starts with ROMs, the
// RAM from 0x6E000 is mirrored from 0. TNZS: sprite RAM 0x404 and shared RAM 0x2404.
// All 8-bit CPUs, no swap.
static const jt_region_t r_thundr[]   = { { 0x0000, 0x8080, 0x0000 } };
static const jt_region_t r_baraduke[] = { { 0x0000, 0x5080, 0x0000 } };
static const jt_region_t r_paclan[]   = { { 0x6E000, 0x4080, 0x0000 } };
static const jt_region_t r_kiwi[]     = { { 0x0404, 0x3000, 0x0404 } };
static const jt_layout_t l_thundr[]   = { { "aliensec", r_baraduke, N(r_baraduke) },
	{ "baraduke", r_baraduke, N(r_baraduke) }, { NULL, r_thundr, N(r_thundr) } };
static const jt_layout_t l_paclan[]   = { { NULL, r_paclan, N(r_paclan) } };
static const jt_layout_t l_kiwi[]     = { { NULL, r_kiwi, N(r_kiwi) } };
// Gradius III: FBNeo's 1-byte soundlatch leaves the 68K buffers at odd RA offsets;
// the core stores Z80 RAM, main/sub/shared RAM and palette one byte lower.
static const jt_region_t r_grad3[] = {
	{ 0x00000, 0x0800, 0x0000 }, { 0x00801, 0x4000, 0x0800 }, { 0x04801, 0x4000, 0x4800 },
	{ 0x08801, 0x4000, 0x8800 }, { 0x2C801, 0x1000, 0xC800 },
};
// Twin16 (Vulcan Venture): sprite, shared, main work and fix RAM (68K, swapped).
static const jt_region_t r_twin16[] = {
	{ 0x00000, 0x4000, 0x0000 }, { 0x0C000, 0x4000, 0x4000 },
	{ 0x1C000, 0x4000, 0x8000 }, { 0x21000, 0x4000, 0xC000 },
};
// OutRun: sub RAM at 0, main work RAM at 0x8000 (68K, swapped): identity 64 KB.
static const jt_layout_t l_grad3[]  = { { NULL, r_grad3, N(r_grad3) } };
static const jt_layout_t l_twin16[] = { { NULL, r_twin16, N(r_twin16) } };
static const jt_layout_t l_cps3[]    = { { NULL, r_cps3, N(r_cps3) } };
static const jt_layout_t l_m72[]     = { { NULL, r_m72, N(r_m72) } };
static const jt_layout_t l_m92[]     = { { NULL, r_m92, N(r_m92) } };
// Cave (Arcade-Cave_MiSTer fork, core name CAVE): every driver's RA buffer starts
// with the 64 KB 68K work RAM, read straight from the core's mainRam (swapped).
static const jt_layout_t l_cave[]    = { { NULL, r_cps, N(r_cps) } };
// Namco Mappy hardware (Arcade-Druaga_MiSTer fork, core name Druaga): FBNeo's All
// Ram is video RAM then sprite RAM; the core writes both at those offsets for
// every board variant (8-bit 6809, no swap).
static const jt_region_t r_druaga[] = { { 0x0000, 0x2800, 0x0000 } };
static const jt_layout_t l_druaga[]  = { { NULL, r_druaga, N(r_druaga) } };
// IGS PGM (Arcade-IGSPGM fork, core name IGSPGM): FBNeo's "68K RAM" is the 128 KB
// work RAM at 0x800000 (swapped). The core copies the 1 KB pages the RA sets use
// (kov2, dmnfrnt, espgal, ket, ddpdojblk) packed into 64 KB.
static const jt_region_t r_pgm[] = {
	{ 0x00000, 0x0400, 0x0000 }, { 0x01000, 0x0400, 0x0400 }, { 0x03400, 0x0800, 0x0800 }, { 0x07C00, 0x1400, 0x1000 },
	{ 0x09800, 0x0C00, 0x2400 }, { 0x0A800, 0x0400, 0x3000 }, { 0x0C400, 0x0C00, 0x3400 }, { 0x0D400, 0x0400, 0x4000 },
	{ 0x0E000, 0x0400, 0x4400 }, { 0x0EC00, 0x0400, 0x4800 }, { 0x0FC00, 0x4C00, 0x4C00 }, { 0x15000, 0x0C00, 0x9800 },
	{ 0x17400, 0x1C00, 0xA400 }, { 0x1A000, 0x0C00, 0xC000 }, { 0x1B400, 0x3000, 0xCC00 }, { 0x1FC00, 0x0400, 0xFC00 },
};
static const jt_layout_t l_pgm[] = { { NULL, r_pgm, N(r_pgm) } };
// Pac-Man hardware (Arcade-Pacman_MiSTer fork, core name PACMAN): the core writes
// Z80 RAM, sprite xy, colour/video RAM and the flip bit at FBNeo's All Ram offsets.
// d_pacman: Z80 RAM 0, sprite xy 0x1000, colour 0x1010, video 0x1410, flip 0x1814.
// d_jrpacman: sprite xy 0, video 0x10, Z80 RAM 0x810 (8-bit, no swap).
static const jt_region_t r_pacman[]   = { { 0x0000, 0x1815, 0x0000 } };
static const jt_region_t r_jrpacman[] = { { 0x0000, 0x1010, 0x0000 } };
static const jt_layout_t l_pacman[]   = { { "jrpacman", r_jrpacman, N(r_jrpacman) }, { NULL, r_pacman, N(r_pacman) } };

static const jt_core_t g_jt_cores[] = {
	{ "JTCPS1",   l_cps,    N(l_cps) },
	{ "JTCPS15",  l_cps,    N(l_cps) },
	{ "JTCPS2",   l_cps,    N(l_cps) },
	{ "JTTMNT",   l_tmnt,   N(l_tmnt) },
	{ "JTRASTAN", l_rastan, N(l_rastan) },
	{ "JTTOKI",   l_toki,   N(l_toki) },
	{ "JTGAE1",   l_gae1,   N(l_gae1) },
	{ "JTWWFSS",  l_wwfss,  N(l_wwfss) },
	{ "JTCOP",    l_dec0,   N(l_dec0) },
	{ "JTNINJA",  l_dec0,   N(l_dec0) },
	{ "JTSLYSPY", l_dec0,   N(l_dec0) },
	{ "JTMIDRES", l_dec0,   N(l_dec0) },
	{ "JTRIDERS", l_riders, N(l_riders) },
	{ "JTALIENS", l_aliens, N(l_aliens) },
	{ "JTS16",    l_sega16, N(l_sega16) },
	{ "JTS16B",   l_sega16, N(l_sega16) },
	{ "JTS18",    l_sega16, N(l_sega16) },
	{ "JTSHOUSE", l_shouse, N(l_shouse) },
	{ "JTPANG",   l_ident32, N(l_ident32) },
	{ "JTBUBL",   l_ident32, N(l_ident32) },
	{ "JT1942",   l_ident16, N(l_ident16) },
	{ "JT1943",   l_ident16, N(l_ident16) },
	{ "JTBTIGER", l_ident16, N(l_ident16) },
	{ "JTGNG",    l_ident16, N(l_ident16) },
	{ "JTSF",     l_sf,      N(l_sf) },
	{ "JTDD",     l_ident8,  N(l_ident8) },
	{ "JTGAIDEN", l_ident16, N(l_ident16) },
	{ "JTVIGIL",  l_vigil,   N(l_vigil) },
	{ "JTWC",     l_wc,      N(l_wc) },
	{ "JTCONTRA", l_ident8,  N(l_ident8) },
	{ "JTROADF",  l_ident8,  N(l_ident8) },
	{ "JTMIKIE",  l_ident8,  N(l_ident8) },
	{ "JTCASTLE", l_castle,  N(l_castle) },
	{ "JTTHUNDR", l_thundr,  N(l_thundr) },
	{ "JTPACLAN", l_paclan,  N(l_paclan) },
	{ "JTKIWI",   l_kiwi,    N(l_kiwi) },
	{ "JTGRAD3",  l_grad3,   N(l_grad3) },
	{ "JTTWIN16", l_twin16,  N(l_twin16) },
	{ "JTOUTRUN", l_cps,     N(l_cps) },
	{ "JTCPS3",   l_cps3,    N(l_cps3) },
	{ "M72",      l_m72,     N(l_m72) },
	{ "IremM92",  l_m92,     N(l_m92) },
	{ "CAVE",     l_cave,    N(l_cave) },
	{ "Druaga",   l_druaga,  N(l_druaga) },
	{ "Psikyo",   l_cave,    N(l_cave) },
	{ "IGSPGM",   l_pgm,     N(l_pgm) },
	{ "PACMAN",   l_pacman,  N(l_pacman) },
};

static uint8_t  g_jt_snap[JT_MIRROR_SIZE];
static int      g_jt_snap_valid = 0;
static uint32_t g_jt_last_frame = 0;
static uint32_t g_jt_frames = 0;
static uint32_t g_jt_torn = 0;
static const jt_layout_t *g_jt_layout = NULL;

static const jt_core_t *jt_core_by_name(const char *name)
{
	if (!name) return NULL;
	if (!strncasecmp(name, "RA_", 3)) name += 3;
	for (int i = 0; i < N(g_jt_cores); i++)
		if (!strcasecmp(name, g_jt_cores[i].core)) return &g_jt_cores[i];
	return NULL;
}

// Pick the layout for a set: first matching prefix, else the default entry.
static const jt_layout_t *jt_pick_layout(const jt_core_t *core, const char *set)
{
	const jt_layout_t *fallback = NULL;
	for (int i = 0; i < core->count; i++) {
		const jt_layout_t *l = &core->layouts[i];
		if (!l->set_prefix) { if (!fallback) fallback = l; continue; }
		if (set && !strncasecmp(set, l->set_prefix, strlen(l->set_prefix))) return l;
	}
	return fallback;
}

static void jt_init(void)
{
	memset(g_jt_snap, 0, sizeof(g_jt_snap));
	g_jt_snap_valid = 0;
	g_jt_last_frame = 0;
	g_jt_frames = 0;
	g_jt_torn = 0;
}

static uint32_t jt_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
	(void)map;
	for (uint32_t i = 0; i < num_bytes; i++) {
		uint32_t a = address + i;
		uint8_t v = 0;
		if (g_jt_snap_valid && g_jt_layout) {
			for (int r = 0; r < g_jt_layout->count; r++) {
				const jt_region_t *reg = &g_jt_layout->regions[r];
				if (a >= reg->ra && a - reg->ra < reg->len) {
					uint32_t m = reg->mirror + (a - reg->ra);
					if (m < JT_MIRROR_SIZE) v = g_jt_snap[m];
					break;
				}
			}
		}
		buffer[i] = v;
	}
	return num_bytes;
}

// Copy the mirror into g_jt_snap. Returns the frame it belongs to, or 0 when
// the FPGA was writing during the copy (caller retries on the next poll).
static uint32_t jt_take_snapshot(void *map)
{
	uint32_t f0 = ra_ramread_frame(map);
	if (ra_ramread_busy(map)) return 0;
	__sync_synchronize();
	memcpy(g_jt_snap, (const uint8_t *)map + JT_MIRROR_OFFSET, JT_MIRROR_SIZE);
	__sync_synchronize();
	if (ra_ramread_busy(map) || ra_ramread_frame(map) != f0) {
		g_jt_torn++;
		return 0;
	}
	return f0;
}

static int jt_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
	if (!client || !game_loaded || !map) return 1;

	uint32_t frame = ra_ramread_frame(map);
	if (frame == g_jt_last_frame) return 1;

	uint32_t snap_frame = jt_take_snapshot(map);
	if (!snap_frame) return 1;

	g_jt_snap_valid = 1;
	g_jt_last_frame = snap_frame;
	g_jt_frames++;
	ra_frame_processed(snap_frame);
	rc_client_do_frame((rc_client_t *)client);

	if ((g_jt_frames % 3600) == 1) {
		uint8_t first[16];
		jt_read_memory(map, g_jt_layout ? g_jt_layout->regions[0].ra : 0, first, sizeof(first));
		char hex[16 * 3 + 1] = {};
		for (int i = 0; i < 16; i++) sprintf(hex + i * 3, "%02X ", first[i]);
		ra_log_write("jotego: frame=%u evaluated=%u torn=%u first region: %s\n",
			snap_frame, g_jt_frames, g_jt_torn, hex);
	}
	return 1;
#else
	(void)map; (void)client; (void)game_loaded;
	return 0;
#endif
}

// Clone set names the MiSTer MRAs use but RetroAchievements never registered,
// mapped to the registered set that runs the same program with the same RAM
// layout (CPS3 no-CD sets, System 16A/16B conversions, Japanese clones).
static const struct { const char *clone, *parent; } jt_set_aliases[] = {
	{ "sfiiin",      "sfiii"    },
	{ "sfiiina",     "sfiii"    },
	{ "sfiii2n",     "sfiii2"   },
	{ "jojon",       "jojo"     },
	{ "jojonr1",     "jojo"     },
	{ "jojonr2",     "jojo"     },
	{ "jojoban",     "jojoba"   },
	{ "jojobanr1",   "jojoba"   },
	{ "redearthn",   "redearth" },
	{ "redearthnr1", "redearth" },
	{ "shinobi3",    "shinobi"  },
	{ "aliensynjo",  "aliensyn" },
	{ "dsoccr94j",   "dsoccr94" },
	{ "dbreedjm72",  "dbreed"   },
	{ "todruagao",   "todruaga" },
	{ "todruagas",   "todruaga" },
	{ "mappyj",      "mappy"    },
	{ "digdug2o",    "digdug2"  },
	{ "mspacmat",    "mspacman" },
	{ "mspacmancr",  "mspacman" },
	{ "candory",     "ponpoko"  },
	{ "ponpokov",    "ponpoko"  },
	{ "jumpshotp",   "jumpshot" },
	{ "eyeszac",     "eyes"     },
	{ "puckmod",     "pacman"   },
	{ "hangly",      "pacman"   },
	{ "newpuckx",    "pacman"   },
	{ "gradius3",    "gradius3j" },
	{ "vulcan",      "gradius2" },
};

static const char *jt_alias_set(const char *set)
{
	for (size_t i = 0; i < sizeof(jt_set_aliases) / sizeof(jt_set_aliases[0]); i++)
		if (!strcasecmp(set, jt_set_aliases[i].clone)) return jt_set_aliases[i].parent;
	return NULL;
}

static int jt_calculate_hash(const char *rom_path, char *md5_hex_out)
{
	// Select the region layout from the set name ("<setname>.zip").
	char set[64] = {};
	const char *base = strrchr(rom_path, '/');
	snprintf(set, sizeof(set), "%s", base ? base + 1 : rom_path);
	char *dot = strrchr(set, '.');
	if (dot) *dot = 0;

	const jt_core_t *core = jt_core_by_name(user_io_get_core_name(1));
	g_jt_layout = core ? jt_pick_layout(core, set) : NULL;
	if (g_jt_layout) {
		ra_log_write("jotego: core %s set '%s' -> layout %s, %d region(s), first RA %04X+%X at mirror %04X\n",
			core->core, set, g_jt_layout->set_prefix ? g_jt_layout->set_prefix : "default",
			g_jt_layout->count, g_jt_layout->regions[0].ra, g_jt_layout->regions[0].len,
			g_jt_layout->regions[0].mirror);
	} else {
		ra_log_write("jotego: no region layout for core '%s'\n", user_io_get_core_name(1));
	}

#ifdef HAS_RCHEEVOS
	// Arcade hashing never opens the file: it is the md5 of the file name
	// without extension, so "<setname>.zip" is all it needs.
	const char *parent = jt_alias_set(set);
	char alias_path[80];
	if (parent) {
		snprintf(alias_path, sizeof(alias_path), "%s.zip", parent);
		ra_log_write("jotego: set '%s' hashed as its RA-registered parent '%s'\n", set, parent);
	}
	const char *hash_path = parent ? alias_path : rom_path;
	if (rc_hash_generate_from_file(md5_hex_out, 27, hash_path)) {
		ra_log_write("jotego: set '%s' hash %s\n", parent ? parent : set, md5_hex_out);
		return 1;
	}
	ra_log_write("jotego: rc_hash_generate_from_file failed for '%s'\n", hash_path);
#endif
	return 0;
}

static int jt_detect_protocol(void *map)
{
	if (!ra_ramread_active(map)) {
		ra_log_write("jotego: FPGA mirror not detected -- RA support unavailable\n");
		return 0;
	}
	uint8_t vmaj = 0, vmin = 0;
	ra_ramread_get_core_version(map, &vmaj, &vmin);
	ra_log_write("jotego: Full Mirror of a 64 KB work RAM window, FPGA v%u.%u\n", vmaj, vmin);
	return 1;
}

#define JT_HANDLER(sym, core_name)                  \
	const console_handler_t sym = {                 \
		.init = jt_init,                            \
		.reset = jt_init,                           \
		.read_memory = jt_read_memory,              \
		.poll = jt_poll,                            \
		.calculate_hash = jt_calculate_hash,        \
		.set_hardcore = NULL,                       \
		.detect_protocol = jt_detect_protocol,      \
		.console_id = 27,                           \
		.name = core_name,                          \
		.hardcore_protected = 0                     \
	};

JT_HANDLER(g_console_jtcps1,   "JTCPS1")
JT_HANDLER(g_console_jtcps15,  "JTCPS15")
JT_HANDLER(g_console_jtcps2,   "JTCPS2")
JT_HANDLER(g_console_jttmnt,   "JTTMNT")
JT_HANDLER(g_console_jtrastan, "JTRASTAN")
JT_HANDLER(g_console_jttoki,   "JTTOKI")
JT_HANDLER(g_console_jtgae1,   "JTGAE1")
JT_HANDLER(g_console_jtwwfss,  "JTWWFSS")
JT_HANDLER(g_console_jtcop,    "JTCOP")
JT_HANDLER(g_console_jtninja,  "JTNINJA")
JT_HANDLER(g_console_jtslyspy, "JTSLYSPY")
JT_HANDLER(g_console_jtmidres, "JTMIDRES")
JT_HANDLER(g_console_jtriders, "JTRIDERS")
JT_HANDLER(g_console_jtaliens, "JTALIENS")
JT_HANDLER(g_console_jts16,    "JTS16")
JT_HANDLER(g_console_jts16b,   "JTS16B")
JT_HANDLER(g_console_jts18,    "JTS18")
JT_HANDLER(g_console_jtshouse, "JTSHOUSE")
JT_HANDLER(g_console_jtpang,   "JTPANG")
JT_HANDLER(g_console_jtbubl,   "JTBUBL")
JT_HANDLER(g_console_jt1942,   "JT1942")
JT_HANDLER(g_console_jt1943,   "JT1943")
JT_HANDLER(g_console_jtbtiger, "JTBTIGER")
JT_HANDLER(g_console_jtgng,    "JTGNG")
JT_HANDLER(g_console_jtsf,     "JTSF")
JT_HANDLER(g_console_jtdd,     "JTDD")
JT_HANDLER(g_console_jtgaiden, "JTGAIDEN")
JT_HANDLER(g_console_jtvigil,  "JTVIGIL")
JT_HANDLER(g_console_jtwc,     "JTWC")
JT_HANDLER(g_console_jtcontra, "JTCONTRA")
JT_HANDLER(g_console_jtroadf,  "JTROADF")
JT_HANDLER(g_console_jtmikie,  "JTMIKIE")
JT_HANDLER(g_console_jtcastle, "JTCASTLE")
JT_HANDLER(g_console_jtthundr, "JTTHUNDR")
JT_HANDLER(g_console_jtpaclan, "JTPACLAN")
JT_HANDLER(g_console_jtkiwi,   "JTKIWI")
JT_HANDLER(g_console_jtgrad3,  "JTGRAD3")
JT_HANDLER(g_console_jttwin16, "JTTWIN16")
JT_HANDLER(g_console_jtoutrun, "JTOUTRUN")
JT_HANDLER(g_console_psikyo,   "Psikyo")
JT_HANDLER(g_console_pgm,      "IGSPGM")
JT_HANDLER(g_console_jtcps3,   "JTCPS3")
JT_HANDLER(g_console_m72,      "M72")
JT_HANDLER(g_console_m92,      "IremM92")
JT_HANDLER(g_console_cave,     "CAVE")
JT_HANDLER(g_console_druaga,   "Druaga")
JT_HANDLER(g_console_pacman,   "PACMAN")

