/*
 * Polaris (Mi MIX 2S, SDM845), modem step 1: read the firmware out of the
 * phone's own modem partition.
 *
 * Nothing here touches the modem. The question is whether the kernel can get at
 * the firmware at all, because every later step - MBA, the modem image, the
 * services behind it - needs those bytes, and if this read does not work the
 * rest is not worth writing.
 *
 * The Mi 8 port reads its firmware the same way and says why: the phone keeps it
 * in a FAT16 filesystem in the "modem" partition, 4096-byte sectors, with an
 * image/ directory holding mba.mbn, modem.mdt and the modem.bXX segments. On
 * this phone that partition is LUN 4, starting at byte 0x10000000 and 192 MB
 * long, which is what TWRP reports for /dev/block/sde46.
 *
 * The kernel's UFS driver is bound to one LUN - LUN 0, the one userdata is on -
 * so this goes through sweet_ufs_raw_read(), which borrows the driver for a few
 * blocks and puts the LUN back afterwards. Nothing else is doing I/O this early
 * in the boot, and the alternative (a second driver for a second LUN) is a lot
 * of code for a read.
 *
 * Only the directory and the headers are read: enough to say the filesystem is
 * there, what is in it, and whether mba.mbn looks like an image. Loading is a
 * later build, and it is the build that writes.
 *
 * Boot with polarismodem=1 to run it. Default: nothing at all.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/machine_routines.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>

extern int sweet_ufs_raw_read(uint8_t lun, uint64_t lba, uint32_t count, void *to);
extern void sweet_log_flush_now(void);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

#define MD_LUN			4
#define MD_PART_LBA		65536u		/* 0x10000000 bytes / 4096 */
#define MD_BLOCK		4096u
#define MD_MAX_CLUSTER		16384u		/* 4 sectors of 4096, this filesystem's */
#define MD_ATTR_DIR		0x10
#define MD_ATTR_LFN		0x0f

/* One device block and one cluster, kept off the stack: a probe should not need
 * a kilobyte of stack to read a directory. */
static uint8_t md_block[MD_BLOCK];
static uint8_t md_cluster[MD_MAX_CLUSTER];
static int md_chan;		/* wait channel for the short sleeps below */

static struct {
	uint32_t bps, spc, reserved, fats, root_ent, fatsz;
	uint32_t root_lba, data_lba;		/* in device blocks, relative to the partition */
	uint32_t root_blocks;
	bool ok;
} md_geo;

static bool
md_read(uint32_t lba, void *to)
{
	return sweet_ufs_raw_read(MD_LUN, MD_PART_LBA + lba, 1, to) == 0;
}

/* The 16-bit FAT entry for a cluster, or 0xffff when it cannot be read. */
static uint32_t
md_fat(uint32_t cluster)
{
	uint32_t at = md_geo.reserved + (cluster * 2) / MD_BLOCK;
	uint32_t off = (cluster * 2) % MD_BLOCK;

	if (!md_read(at, md_block)) {
		return 0xffffu;
	}
	return (uint32_t)md_block[off] | (uint32_t)md_block[off + 1] << 8;
}

static const char *
md_name(const uint8_t *entry, char *out, size_t size)
{
	unsigned n = 0, i;

	for (i = 0; i < 8 && entry[i] != ' '; i++) {
		if (n + 1 < size) {
			out[n++] = (char)entry[i];
		}
	}
	if (entry[8] != ' ') {
		if (n + 1 < size) {
			out[n++] = '.';
		}
		for (i = 8; i < 11 && entry[i] != ' '; i++) {
			if (n + 1 < size) {
				out[n++] = (char)entry[i];
			}
		}
	}
	out[n] = 0;
	return out;
}

static uint32_t
md_first_cluster(const uint8_t *entry)
{
	return (uint32_t)entry[26] | (uint32_t)entry[27] << 8;
}

static uint32_t
md_file_size(const uint8_t *entry)
{
	return (uint32_t)entry[28] | (uint32_t)entry[29] << 8 |
	    (uint32_t)entry[30] << 16 | (uint32_t)entry[31] << 24;
}

/*
 * Walk a directory's entries, calling `show` for each real one. `dir_cluster` 0
 * means the root directory, which is a fixed run of blocks rather than a chain.
 */
static void
md_walk(uint32_t dir_cluster, void (*show)(const uint8_t *entry, void *ctx), void *ctx)
{
	uint32_t cluster = dir_cluster;
	uint32_t guard = 0;

	for (;;) {
		uint32_t blocks, b;

		if (cluster == 0) {
			blocks = md_geo.root_blocks;
		} else {
			blocks = md_geo.spc;
		}
		for (b = 0; b < blocks; b++) {
			uint32_t lba = cluster == 0 ? md_geo.root_lba + b :
			    md_geo.data_lba + (cluster - 2) * md_geo.spc + b;

			if (!md_read(lba, md_block)) {
				printf("POLARIS MODEM: the directory block at +%u did not read\n", lba);
				return;
			}
			for (uint32_t off = 0; off + 32 <= MD_BLOCK; off += 32) {
				const uint8_t *e = md_block + off;

				if (e[0] == 0) {
					return;		/* the end of the directory */
				}
				if (e[0] == 0xe5 || (e[11] & MD_ATTR_LFN) == MD_ATTR_LFN) {
					continue;	/* deleted, or a long-name fragment */
				}
				show(e, ctx);
			}
		}
		if (cluster == 0) {
			return;
		}
		cluster = md_fat(cluster);
		if (cluster < 2 || cluster >= 0xfff8u || ++guard > 512) {
			return;
		}
	}
}

/* Where the image/ directory is, once the root has been walked. */
static uint32_t md_image_cluster;
static bool md_image_found;

static void
md_root_entry(const uint8_t *entry, void *ctx)
{
	char name[16];

	(void)ctx;
	md_name(entry, name, sizeof(name));
	if ((entry[11] & MD_ATTR_DIR) != 0 && strcmp(name, "IMAGE") == 0) {
		md_image_cluster = md_first_cluster(entry);
		md_image_found = true;
		printf("POLARIS MODEM: the root has \"%s\", a directory at cluster %u\n",
		    name, md_image_cluster);
	}
}

/* The files in image/, and the first four bytes of each so an ELF or an MBN
 * shows up as itself. */
static void
md_image_entry(const uint8_t *entry, void *ctx)
{
	char name[16];
	uint32_t cluster, size;

	(void)ctx;
	if ((entry[11] & MD_ATTR_DIR) != 0) {
		return;
	}
	md_name(entry, name, sizeof(name));
	cluster = md_first_cluster(entry);
	size = md_file_size(entry);
	printf("POLARIS MODEM:   %-12s %9u bytes, first cluster %u", name, size, cluster);
	if (size >= 4 && cluster >= 2 && md_geo.spc > 0) {
		if (md_read(md_geo.data_lba + (cluster - 2) * md_geo.spc, md_cluster)) {
			printf(", starts %02x %02x %02x %02x", md_cluster[0], md_cluster[1],
			    md_cluster[2], md_cluster[3]);
			if (md_cluster[0] == 0x7f && md_cluster[1] == 'E' &&
			    md_cluster[2] == 'L' && md_cluster[3] == 'F') {
				printf(" (ELF)");
			}
		}
	}
	printf("\n");
}

/*
 * 0: nothing at all, 1: read the firmware (step 1), 2: also read the modem's
 * own state (step 2). The level is the boot-arg's value, so one argument covers
 * both steps and a boot can ask for either.
 */
static int
polaris_modem_level(void)
{
	int optin = 0;

	if (!PE_parse_boot_argn("polarismodem", &optin, sizeof(optin))) {
		return 0;
	}
	return optin;
}

void
polaris_modem_probe(void)
{
	uint32_t total;

	if (polaris_modem_level() < 1) {
		return;
	}
	printf("POLARIS MODEM: ======== build 0169 alive: reading the modem partition on "
	    "LUN %u at +%#x, nothing else ========\n", MD_LUN, MD_PART_LBA);

	if (!md_read(0, md_block)) {
		printf("POLARIS MODEM: the boot sector did not read: LUN %u is not reachable "
		    "from here\n", MD_LUN);
		return;
	}
	if (!(md_block[510] == 0x55 && md_block[511] == 0xaa)) {
		printf("POLARIS MODEM: no 0xaa55 at the end of the boot sector (it reads "
		    "%02x %02x): this is not a filesystem\n", md_block[510], md_block[511]);
		return;
	}
	md_geo.bps = (uint32_t)md_block[11] | (uint32_t)md_block[12] << 8;
	md_geo.spc = md_block[13];
	md_geo.reserved = (uint32_t)md_block[14] | (uint32_t)md_block[15] << 8;
	md_geo.fats = md_block[16];
	md_geo.root_ent = (uint32_t)md_block[17] | (uint32_t)md_block[18] << 8;
	md_geo.fatsz = (uint32_t)md_block[22] | (uint32_t)md_block[23] << 8;
	total = (uint32_t)md_block[19] | (uint32_t)md_block[20] << 8;

	printf("POLARIS MODEM: FAT%u, %u-byte sectors, %u per cluster, %u FATs of %u, "
	    "%u root entries, %u sectors total\n", md_block[54 + 4] == '6' ? 16 : 12,
	    md_geo.bps, md_geo.spc, md_geo.fats, md_geo.fatsz, md_geo.root_ent, total);

	if (md_geo.bps != MD_BLOCK || md_geo.spc == 0 || md_geo.spc > MD_MAX_CLUSTER / MD_BLOCK ||
	    md_geo.root_ent == 0 || md_geo.fatsz == 0) {
		printf("POLARIS MODEM: this geometry is not one this reader handles "
		    "(it assumes 4096-byte sectors)\n");
		return;
	}
	md_geo.root_lba = md_geo.reserved + md_geo.fats * md_geo.fatsz;
	md_geo.root_blocks = (md_geo.root_ent * 32 + MD_BLOCK - 1) / MD_BLOCK;
	md_geo.data_lba = md_geo.root_lba + md_geo.root_blocks;
	md_geo.ok = true;
	printf("POLARIS MODEM: root at +%u, data at +%u (%#x bytes into the partition)\n",
	    md_geo.root_lba, md_geo.data_lba, md_geo.data_lba * MD_BLOCK);

	md_walk(0, md_root_entry, NULL);
	if (!md_image_found) {
		printf("POLARIS MODEM: there is no image/ directory in the root\n");
		return;
	}
	printf("POLARIS MODEM: image/ holds:\n");
	md_walk(md_image_cluster, md_image_entry, NULL);
	printf("POLARIS MODEM: read only; nothing was written\n");
}

/*
 * Step 2: is the modem already running?
 *
 * Step 1 proved the firmware can be read. That only matters if it has to be
 * loaded, and the boot loader may well have loaded it already - the ABL is the
 * thing that normally brings the modem up, and on this phone it is untouched.
 * The answer decides what the rest of this line costs:
 *
 *   the boot FSM finished (MSS_STATUS bit 0)  the modem is up and the work is
 *                                             talking to it: QMI over SMEM,
 *                                             which is where the Wi-Fi line's
 *                                             GLINK already points
 *   PBL never ran                             nothing has loaded it, and the
 *                                             AP would have to do the whole
 *                                             PIL job: authenticate the MBA,
 *                                             hand it the metadata, and wait
 *                                             on a boot FSM. That is a large
 *                                             piece of work, and it needs the
 *                                             TrustZone side too.
 *
 * Everything here is a register read, in the order Linux reads them, with one
 * exception: the MSS AHB clock has to be running before its registers can be
 * read at all, so the three clock branches the Q6 needs are switched on first.
 * That is the same kind of write the USB and GPU probes already do, on a block
 * that is always powered.
 *
 * The addresses and the offsets are not guesses. The device tree for sdm845
 * puts the Q6 control block at 0x04080000 and the RMB - the register block the
 * boot ROM and the MBA report through - at 0x04180000, and the offsets and the
 * status values below come from drivers/remoteproc/qcom_q6v5_mss.c:
 *
 *	RMB_MBA_IMAGE_REG	0x00	RMB_PBL_STATUS_REG	0x04
 *	RMB_MBA_COMMAND_REG	0x08	RMB_MBA_STATUS_REG	0x0c
 *	RMB_PMI_META_DATA_REG	0x10	RMB_PMI_CODE_START_REG	0x14
 *	RMB_PMI_CODE_LENGTH_REG	0x18	RMB_MBA_MSS_STATUS	0x40
 *	RMB_MBA_ALT_RESET	0x44
 *
 * Boot with polarismodem=2 to run it, and with =1 to run only step 1.
 */
#define MD_GCC_PHYS		0x00100000ull
#define MD_GCC_SIZE		0x00090000ull
#define MD_Q6_PHYS		0x04080000ull
#define MD_RMB_PHYS		0x04180000ull
#define MD_MAP_SIZE		0x00001000ull

#define MD_CBCR_OFF		(1u << 31)	/* branch halted, i.e. the clock is off */

struct md_reg {
	uint32_t off;
	const char *name;
};

static const struct md_reg md_clocks[] = {
	{ 0x8a000, "mss_cfg_ahb" },
	{ 0x38004, "boot_rom_ahb" },
	{ 0x8a004, "mss_mfab_axis" },
	{ 0x8a150, "mss_snoc_axi" },
	{ 0x8a154, "mss_q6_memnoc_axi" },
};

#define MD_GCC_APPS_VOTE	0x52004		/* bit 17: GPLL0 to the modem */
#define MD_GCC_MSS_CFG_AHB	0x8a000
#define MD_GCC_BOOT_ROM_AHB	0x38004

static const struct md_reg md_rmb[] = {
	{ 0x00, "MBA_IMAGE" },
	{ 0x04, "PBL_STATUS" },
	{ 0x08, "MBA_COMMAND" },
	{ 0x0c, "MBA_STATUS" },
	{ 0x10, "PMI_META_DATA" },
	{ 0x14, "PMI_CODE_START" },
	{ 0x18, "PMI_CODE_LENGTH" },
	{ 0x40, "MBA_MSS_STATUS" },
	{ 0x44, "MBA_ALT_RESET" },
};

static const struct md_reg md_q6[] = {
	{ 0x014, "QDSP6SS_RESET" },
	{ 0x020, "QDSP6SS_GFMUX_CTL" },
	{ 0x030, "QDSP6SS_PWR_CTL" },
	{ 0x0b0, "QDSP6SS_MEM_PWR_CTL" },
	{ 0x110, "QDSP6SS_STRAP_ACC" },
	{ 0x400, "QDSP6SS_BOOT_CORE_START" },
	{ 0x404, "QDSP6SS_BOOT_CMD" },
};

static uint32_t
md_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(void *)(base + off);
}

static void
md_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(void *)(base + off) = v;
}
/*
 * Every line goes to the flash copy of the ring the moment it is printed.
 * Build 0175 printed a dozen lines between two flushes and then stopped, which
 * left no way to tell which one of them did it.
 */
#define MD_SAY(...) do {			\
	printf(__VA_ARGS__);			\
	sweet_log_flush_now();			\
} while (0)

/*
 * A spin, not a sleep. A thread that parks in the interconnect never comes
 * back, and thread_block() would add a second way to lose one. A millisecond
 * is short enough to burn.
 */
static void
md_spin_ms(unsigned ms)
{
	volatile uint64_t n = (uint64_t)ms * 40000ull;

	while (n != 0) {
		n--;
	}
}

/* Switch a clock branch on and say whether it came up. */
static void
md_clock_on(vm_offset_t gcc, uint32_t off, const char *name)
{
	uint32_t before = md_rd(gcc, off);

	if (!(before & MD_CBCR_OFF) && (before & 1u)) {
		MD_SAY("POLARIS MODEM:   %-18s +0x%05x 0x%08x already on\n", name, off,
		    before);
		return;
	}
	md_wr(gcc, off, before | 1u);
	for (unsigned i = 0; i < 20u; i++) {
		uint32_t v = md_rd(gcc, off);

		if (!(v & MD_CBCR_OFF)) {
			MD_SAY("POLARIS MODEM:   %-18s +0x%05x 0x%08x -> 0x%08x on\n",
			    name, off, before, v);
			return;
		}
		md_spin_ms(1);
	}
	MD_SAY("POLARIS MODEM:   %-18s +0x%05x 0x%08x -> 0x%08x STILL HALTED\n",
	    name, off, before, md_rd(gcc, off));
}

/*
 * The four values the MBA reports as it goes, in the order it reports them.
 * Anything outside that list is printed as a raw number rather than dressed up.
 */
static const char *
md_mba_status_name(uint32_t v)
{
	switch (v) {
	case 0: return "nothing yet: the MBA has not run";
	case 1: return "XPU unlocked";
	case 2: return "XPU unlocked, memory scribbled";
	case 3: return "metadata authenticated";
	case 4: return "AUTH COMPLETE";
	default: return "not a value this reader knows";
	}
}

/*
 * Read a whole table, one register at a time, with a line flushed before each
 * access. If one of them parks the thread, the last line in the ring is the
 * name of the register that did it - and that name is the answer, not a loss.
 */
static void
md_read_table(vm_offset_t base, const char *what, const struct md_reg *tbl,
    unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		MD_SAY("POLARIS MODEM:   %-20s +0x%03x reading\n", tbl[i].name,
		    tbl[i].off);
		MD_SAY("POLARIS MODEM:   %-20s +0x%03x = 0x%08x\n", tbl[i].name,
		    tbl[i].off, md_rd(base, tbl[i].off));
	}
	(void)what;
}

/*
 * Level 2 stops after the GCC block: every register there belongs to a
 * controller that is never powered down, and the GPU probe already read five of
 * them in this same boot, so that phase is the one that cannot hurt.
 *
 * Level 3 adds the two clock writes, the GPLL0 vote and the modem's own blocks.
 * That is where a read can park the thread, which is exactly why it is a
 * separate level and why this runs in its own thread.
 */
void
polaris_modem_state_probe(void)
{
	vm_offset_t gcc, rmb, q6;
	int level = polaris_modem_level();

	if (level < 2) {
		return;
	}
	MD_SAY("POLARIS MODEM: ======== build 0176 alive: level %d, read first, "
	    "the clock writes are level 3 ========\n", level);

	gcc = ml_io_map(MD_GCC_PHYS, MD_GCC_SIZE);
	if (gcc == 0) {
		MD_SAY("POLARIS MODEM: GCC did not map\n");
		return;
	}
	MD_SAY("POLARIS MODEM: phase A: the GCC block, nothing written\n");
	{
		uint32_t v = md_rd(gcc, MD_GCC_APPS_VOTE);

		MD_SAY("POLARIS MODEM:   apps vote          +0x52004 = 0x%08x "
		    "(mss gpll0 div %s)\n", v, (v & (1u << 17)) ? "voted" : "not voted");
	}
	md_read_table(gcc, "gcc", md_clocks,
	    (unsigned)(sizeof(md_clocks) / sizeof(md_clocks[0])));
	if (level < 3) {
		MD_SAY("POLARIS MODEM: phase A done; level 3 adds the clock writes "
		    "and the modem's own blocks\n");
		return;
	}

	MD_SAY("POLARIS MODEM: phase B: switching the two AHB branches on\n");
	md_clock_on(gcc, MD_GCC_MSS_CFG_AHB, "mss_cfg_ahb");
	md_clock_on(gcc, MD_GCC_BOOT_ROM_AHB, "boot_rom_ahb");
	{
		uint32_t v = md_rd(gcc, MD_GCC_APPS_VOTE);

		md_wr(gcc, MD_GCC_APPS_VOTE, v | (1u << 17));
		MD_SAY("POLARIS MODEM:   apps vote          +0x52004 0x%08x -> 0x%08x "
		    "(mss gpll0 div voted)\n", v, md_rd(gcc, MD_GCC_APPS_VOTE));
	}

	MD_SAY("POLARIS MODEM: phase C: the RMB at %#llx\n",
	    (unsigned long long)MD_RMB_PHYS);
	rmb = ml_io_map(MD_RMB_PHYS, MD_MAP_SIZE);
	if (rmb == 0) {
		MD_SAY("POLARIS MODEM: the RMB did not map\n");
		return;
	}
	md_read_table(rmb, "rmb", md_rmb, (unsigned)(sizeof(md_rmb) / sizeof(md_rmb[0])));

	{
		uint32_t pbl = md_rd(rmb, 0x04);
		uint32_t mba = md_rd(rmb, 0x0c);
		uint32_t mss = md_rd(rmb, 0x40);

		MD_SAY("POLARIS MODEM: the boot ROM %s (PBL_STATUS 0x%x)\n",
		    pbl ? "ran and handed over" : "never ran", pbl);
		MD_SAY("POLARIS MODEM: the MBA: %s (MBA_STATUS 0x%x)\n",
		    md_mba_status_name(mba), mba);
		MD_SAY("POLARIS MODEM: boot FSM %s (MSS_STATUS 0x%x, bit 0 %d)\n",
		    (mss & 1u) ? "FINISHED - the modem is up" : "did not finish", mss,
		    mss & 1u);
	}

	MD_SAY("POLARIS MODEM: phase D: the Q6 control block at %#llx\n",
	    (unsigned long long)MD_Q6_PHYS);
	q6 = ml_io_map(MD_Q6_PHYS, MD_MAP_SIZE);
	if (q6 == 0) {
		MD_SAY("POLARIS MODEM: the Q6 block did not map\n");
		return;
	}
	md_read_table(q6, "q6", md_q6, (unsigned)(sizeof(md_q6) / sizeof(md_q6[0])));
	MD_SAY("POLARIS MODEM: read only; nothing was written to the modem\n");
}

/*
 * Build 0175 called the probe straight from the mount path, and the whole
 * machine stopped there: whatever parks a thread in this interconnect parked
 * the thread that was mounting the root filesystem. The probe gets its own
 * thread now, so a park costs the probe and nothing else - the log thread keeps
 * flushing and the ring still says where it happened.
 */
static boolean_t polaris_modem_kicked;

static void
polaris_modem_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	polaris_modem_state_probe();
	MD_SAY("POLARIS MODEM: the state thread returned\n");
}

void
polaris_modem_state_kick(void)
{
	thread_t thread;
	int level = polaris_modem_level();

	if (level < 2 || polaris_modem_kicked) {
		return;
	}
	polaris_modem_kicked = TRUE;
	if (kernel_thread_start(polaris_modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		MD_SAY("POLARIS MODEM: state thread started, level %d\n", level);
	} else {
		MD_SAY("POLARIS MODEM: could not start the state thread\n");
	}
}
#endif
