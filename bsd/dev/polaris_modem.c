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
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>

extern int sweet_ufs_raw_read(uint8_t lun, uint64_t lba, uint32_t count, void *to);

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

static bool
polaris_modem_requested(void)
{
	int optin = 0;

	if (!PE_parse_boot_argn("polarismodem", &optin, sizeof(optin))) {
		return false;
	}
	return optin == 1;
}

void
polaris_modem_probe(void)
{
	uint32_t total;

	if (!polaris_modem_requested()) {
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
#endif
