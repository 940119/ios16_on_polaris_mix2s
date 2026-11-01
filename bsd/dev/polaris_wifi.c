/*
 * Polaris (Mi MIX 2S, SDM845) Wi-Fi, step 1: read the shared memory and say
 * what is in it.
 *
 * Nothing here writes. The point is to answer one question before any of the
 * WCN3990 machinery is ported: is this phone's SMEM laid out the way the Mi 8's
 * is? The Mi 8 port (bsd/dev/mi8_rproc.c) has a working WLAN stack, and it is
 * only reusable if the tables line up. If the item numbers differ, the port
 * needs a mapping table and that is cheaper to discover here than three builds
 * later.
 *
 * The layout is Linux's (drivers/soc/qcom/smem.c):
 *
 *   the region        2 MB at 0x86000000 (dt: qcom,smem@86000000)
 *   the table of      the last 4 KB of the region: magic "$TOC", version 1,
 *   contents          then `count` 48-byte entries of {offset, size, flags,
 *                     host0, host1, cacheline}
 *   a partition       at entry.offset: magic "$PRT", its two hosts, size, and
 *                     the two free-space pointers
 *   an item           a 16-byte header {canary 0xa5a5, item, size, paddings}
 *                     followed by the item's bytes
 *
 * Host ids are Linux's: 0 is the application processor, 1 the modem, 2 the
 * audio DSP, 3 the sensors DSP, 4 the NPU, 5 the compute DSP, 6 the wireless
 * subsystem (SPSS on this part), 0xfffe "global".
 *
 * Build 0163 printed the table with the names it knew and a question mark for
 * everything else, which hid the one entry that matters: `toc 5: ap <-> ?` at
 * 0x14f000, sixteen kilobytes, which is the size of a WLAN partition and the
 * only candidate for one. So this build prints the raw host ids, every entry
 * whether or not the name table knows it, and every item in every partition
 * instead of the first ten.
 *
 * An empty partition is a normal thing to find, not a failure: SMP2P and GLINK
 * items are allocated by whichever side brings the transport up, so a remote
 * processor this kernel has never started has none of them yet.
 *
 * Why a copy loop instead of a struct pointer: the region is mapped write-
 * combined and is not normal memory, so every read goes through pw_peek() one
 * byte at a time. That is also how the Mi 8 port does it, and it is the only
 * way a short read shows up as a failed check rather than as garbage.
 *
 * Boot with polariswifi=1 to run it. Default: nothing at all.
 *
 * Build 0165 finished the walk and answered the question it was built for, and
 * the answer was not the expected one: every partition the AP is a party to is
 * empty. Partition 5 (ap <-> host 8, 16 KB) exists with a valid $PRT and has
 * nothing in it; partition 10 (ap <-> modem, 496 KB) is the same; the only
 * partition with items in it is the global one. That is not a fault - SMEM items
 * are allocated by whichever side brings a transport up, and this kernel has
 * never brought one up. The Mi 8 port allocates its own (qcom_smem_alloc, the
 * descriptor 478 and the first FIFO 479) and so will this one.
 *
 * Two things follow. First, the WLAN item numbers cannot be confirmed by
 * finding them; they have to be created. Second, partition 5 is 16384 bytes in
 * total and 32 of those are the header, so the Mi 8 port's 16 KB FIFO cannot
 * fit in it - the FIFO sizes for this edge have to be chosen against that
 * budget, not copied.
 *
 * So this build also reads what the global partition does carry: the image
 * version tables (469 and 602) are a record per firmware image, in host order,
 * and their names are the cheapest way to say which subsystem host 8 is.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);

#define PW_PHYS			0x86000000ull
#define PW_SIZE			0x00200000u

#define PW_HOST_AP		0
#define PW_HOST_MODEM		1
#define PW_HOST_ADSP		2
#define PW_HOST_SDSP		3
#define PW_HOST_NPU		4
#define PW_HOST_CDSP		5
#define PW_HOST_SPSS		6
#define PW_HOST_GLOBAL		0xfffe

#define PW_CANARY		0xa5a5

/* Linux's struct smem_table_entry, 48 bytes. */
struct pw_toc_entry {
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
	uint16_t host0;
	uint16_t host1;
	uint32_t cacheline;
	uint32_t reserved[7];
};

/* Linux's struct smem_table, 32 bytes, at PW_SIZE - 0x1000. */
struct pw_toc {
	uint8_t  magic[4];
	uint32_t version;
	uint32_t count;
	uint32_t reserved[5];
};

/* Linux's struct smem_partition, 32 bytes, at the partition's offset. */
struct pw_partition {
	uint8_t  magic[4];
	uint16_t host0;
	uint16_t host1;
	uint32_t size;
	uint32_t free_uncached;
	uint32_t free_cached;
	uint32_t reserved[3];
};

/* Linux's struct smem_entry, 16 bytes, before each item's bytes. */
struct pw_item_header {
	uint16_t canary;
	uint16_t item;
	uint32_t size;
	uint16_t padding_data;
	uint16_t padding_header;
	uint32_t reserved;
};

/*
 * The items the WLAN path needs, named the way the Mi 8 port names them. These
 * are Linux's sdm845 numbers; the whole point of this build is to find out
 * whether this phone agrees.
 */
#define PW_ITEM_SMP2P_AP	429	/* our half of the pair, every remote */
#define PW_ITEM_SMP2P_REMOTE	443	/* the remote's half */
#define PW_ITEM_SMP2P_MSS_AP	428
#define PW_ITEM_SMP2P_MSS	435
#define PW_ITEM_GLINK_DESC	478
#define PW_ITEM_GLINK_FIFO	479
#define PW_ITEM_CRASH_REASON	421	/* 421 + remote pid */

#define PW_ITEMS_MAX		64u

static const volatile uint8_t *pw_smem;

static void pw_strings(uint32_t at, uint32_t size, unsigned want);

static bool
pw_peek(uint32_t offset, void *to, uint32_t size)
{
	if (pw_smem == NULL) {
		pw_smem = (const volatile uint8_t *)ml_io_map_unmappable(PW_PHYS, PW_SIZE,
		    VM_WIMG_WCOMB);
	}
	if (pw_smem == NULL || offset + size > PW_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = pw_smem[offset + i];
	}
	return true;
}

static uint32_t
pw_word(uint32_t offset)
{
	uint32_t v = 0;

	pw_peek(offset, &v, sizeof(v));
	return v;
}

static bool
pw_toc(struct pw_toc *toc)
{
	return pw_peek(PW_SIZE - 0x1000, toc, sizeof(*toc)) &&
	    memcmp(toc->magic, "$TOC", 4) == 0 && toc->version == 1 && toc->count <= 64;
}

static const char *
pw_host_name(uint16_t host)
{
	switch (host) {
	case PW_HOST_AP:      return "ap";
	case PW_HOST_MODEM:   return "modem";
	case PW_HOST_ADSP:    return "adsp";
	case PW_HOST_SDSP:    return "sdsp";
	case PW_HOST_NPU:     return "npu";
	case PW_HOST_CDSP:    return "cdsp";
	case PW_HOST_SPSS:    return "spss";
	case PW_HOST_GLOBAL:  return "global";
	default:              return "?";
	}
}

/*
 * Every distinct item id this build has seen, so the summary can answer "does
 * the WLAN set exist anywhere" directly instead of leaving it to be read off a
 * list of forty numbers.
 */
struct pw_census {
	uint32_t ids[PW_ITEMS_MAX];
	uint32_t count;
	uint32_t items;
};

static void
pw_census_add(struct pw_census *c, uint32_t id)
{
	for (uint32_t i = 0; i < c->count; i++) {
		if (c->ids[i] == id) {
			return;
		}
	}
	if (c->count < PW_ITEMS_MAX) {
		c->ids[c->count++] = id;
	}
}

static bool
pw_census_has(const struct pw_census *c, uint32_t id)
{
	for (uint32_t i = 0; i < c->count; i++) {
		if (c->ids[i] == id) {
			return true;
		}
	}
	return false;
}

/*
 * Walk one partition and report what is in it.
 *
 * Items are laid out from the start of the partition upwards, and (when the
 * partition has a cache line) from the end of it downwards as well; the two
 * meet at free_uncached / free_cached. Both directions are walked, because an
 * item allocated by the boot loader can sit in either half and the WLAN items
 * are usually the ones the boot loader put at the top.
 */
static void
pw_survey(uint32_t offset, uint32_t size, uint32_t cacheline, struct pw_census *census)
{
	struct pw_partition header;
	struct pw_item_header item;
	unsigned low = 0, high = 0;
	uint32_t at, step_back;

	if (offset == 0 || size < sizeof(header)) {
		return;
	}
	if (!pw_peek(offset, &header, sizeof(header))) {
		printf("POLARIS WIFI:   at %#x: unreadable\n", offset);
		return;
	}
	if (memcmp(header.magic, "$PRT", 4) != 0) {
		printf("POLARIS WIFI:   at %#x, %u bytes: no $PRT (magic %02x %02x %02x %02x)\n",
		    offset, size, header.magic[0], header.magic[1], header.magic[2],
		    header.magic[3]);
		return;
	}
	printf("POLARIS WIFI:   $PRT says hosts %u/%u, size %u, free %u/%u\n",
	    header.host0, header.host1, header.size, header.free_uncached,
	    header.free_cached);
	if (header.free_uncached > header.size || header.free_cached > header.size) {
		printf("POLARIS WIFI:   the free pointers are outside the partition: not "
		    "walking it\n");
		return;
	}
	for (at = (uint32_t)sizeof(header); at + sizeof(item) <= header.free_uncached;) {
		if (!pw_peek(offset + at, &item, sizeof(item)) || item.canary != PW_CANARY ||
		    item.size > header.size) {
			break;
		}
		low++;
		pw_census_add(census, item.item);
		census->items++;
		printf("POLARIS WIFI:     item %u at %#x, %u bytes\n", item.item,
		    offset + at + (uint32_t)sizeof(item) + item.padding_header,
		    item.size - item.padding_data);
		if (item.size >= 64) {
			pw_strings(offset + at + (uint32_t)sizeof(item) + item.padding_header,
			    item.size - item.padding_data,
			    (item.item == 469 || item.item == 602) ? 20u : 4u);
		}
		at += (uint32_t)sizeof(item) + item.padding_header + item.size;
	}
	if (cacheline != 0 && (cacheline & (cacheline - 1)) == 0) {
		step_back = ((uint32_t)sizeof(item) + cacheline - 1) & ~(cacheline - 1);
		for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
			if (!pw_peek(offset + at, &item, sizeof(item)) ||
			    item.canary != PW_CANARY || item.size > at) {
				break;
			}
			high++;
			pw_census_add(census, item.item);
			census->items++;
			printf("POLARIS WIFI:     item %u at %#x, %u bytes (from the top)\n",
			    item.item, offset + at - item.size,
			    item.size - item.padding_data);
			if (item.size >= 64) {
				pw_strings(offset + at - item.size, item.size - item.padding_data,
				    (item.item == 469 || item.item == 602) ? 20u : 4u);
			}
			at -= step_back + item.size;
		}
	}
	printf("POLARIS WIFI:   %u items from the bottom, %u from the top\n", low, high);
}

/*
 * A bounded strings extraction from one item.
 *
 * The global partition's items are the only ones with anything in them, and two
 * of them are the image version tables (469, 4096 bytes; 602, 10240 bytes): the
 * boot loader writes a record per firmware image it loaded, each with the
 * image's name and build id. Those names are the cheapest way to find out which
 * subsystems this phone actually has, and - because the records are in host
 * order - which host id each one is. That matters here because the table lists
 * hosts 7 and 8, which the Mi 8 port never names.
 *
 * The record layout is not assumed. Printable runs of three characters or more
 * are printed as they come, which reads a name table correctly whatever the
 * field sizes turn out to be, and prints nothing misleading if the item is not
 * a name table at all.
 */
static void
pw_strings(uint32_t at, uint32_t size, unsigned want)
{
	char buf[48];
	unsigned run = 0, shown = 0;
	uint32_t limit = size < 8192 ? size : 8192;

	for (uint32_t i = 0; i <= limit; i++) {
		uint8_t c = 0;
		bool printable = i < limit && pw_peek(at + i, &c, 1) && c >= 0x20 && c < 0x7f;

		if (printable) {
			if (run < sizeof(buf) - 1) {
				buf[run] = (char)c;
			}
			run++;
			continue;
		}
		if (run >= 3) {
			unsigned n = run < sizeof(buf) - 1 ? run : (unsigned)sizeof(buf) - 1;

			buf[n] = 0;
			printf("POLARIS WIFI:       \"%s\"\n", buf);
			if (++shown >= want) {
				return;
			}
		}
		run = 0;
	}
}

/*
 * Whether the application processor may touch a partition at all.
 *
 * Build 0164 walked every entry in the table and stopped dead on partition 4,
 * the adsp <-> sdsp one: the label printed, then nothing - no "$PRT", no
 * "unreadable", and the other cores kept printing USB samples for another
 * eighty seconds. A read that never returns while the rest of the machine runs
 * is one core stalled on the bus, and the difference between partition 4 and
 * the three before it is that the AP is a party to those three and not to it.
 * Qualcomm's XPU guards SMEM partitions against hosts that do not own them, so
 * touching one from the wrong side is a bus hang, not a data abort.
 *
 * Only partitions with the AP on one side (or the global one, which is
 * everybody's) are read.
 */
static bool
pw_ours(uint16_t host0, uint16_t host1)
{
	return host0 == PW_HOST_AP || host1 == PW_HOST_AP ||
	    host0 == PW_HOST_GLOBAL || host1 == PW_HOST_GLOBAL;
}

static bool
polaris_wifi_requested(void)
{
	int optin = 0;

	if (!PE_parse_boot_argn("polariswifi", &optin, sizeof(optin))) {
		return false;
	}
	return optin == 1;
}

void
polaris_wifi_probe(void)
{
	struct pw_census census;
	struct pw_toc toc;
	uint32_t walked = 0, skipped = 0;

	if (!polaris_wifi_requested()) {
		return;
	}
	memset(&census, 0, sizeof(census));
	printf("POLARIS WIFI: ======== build 0166 alive: reading SMEM at %#llx (%u bytes) "
	    "and nothing else ========\n", PW_PHYS, PW_SIZE);

	printf("POLARIS WIFI: the boot loader's version word at %#x is %#x (12 in [31:16] means "
	    "the partition table is in use)\n", 0x40 + 4 * 7, pw_word(0x40 + 4 * 7));

	if (!pw_toc(&toc)) {
		printf("POLARIS WIFI: no $TOC at %#x: this is not SMEM v1\n", PW_SIZE - 0x1000);
		return;
	}
	printf("POLARIS WIFI: $TOC version %u, %u entries\n", toc.version, toc.count);
	for (uint32_t i = 0; i < toc.count; i++) {
		struct pw_toc_entry entry;

		if (!pw_peek(PW_SIZE - 0x1000 + (uint32_t)sizeof(toc) +
		    i * (uint32_t)sizeof(entry), &entry, sizeof(entry))) {
			break;
		}
		printf("POLARIS WIFI: toc %2u: hosts %u/%u (%s <-> %s) at %#x, %u bytes, "
		    "cache line %u, flags %#x\n", i, entry.host0, entry.host1,
		    pw_host_name(entry.host0), pw_host_name(entry.host1), entry.offset,
		    entry.size, entry.cacheline, entry.flags);
	}
	for (uint32_t i = 0; i < toc.count; i++) {
		struct pw_toc_entry entry;

		if (!pw_peek(PW_SIZE - 0x1000 + (uint32_t)sizeof(toc) +
		    i * (uint32_t)sizeof(entry), &entry, sizeof(entry))) {
			break;
		}
		if (entry.offset == 0 || entry.size == 0) {
			continue;
		}
		if (!pw_ours(entry.host0, entry.host1)) {
			printf("POLARIS WIFI: partition %u (hosts %u/%u): not the AP's, "
			    "not read\n", i, entry.host0, entry.host1);
			skipped++;
			continue;
		}
		printf("POLARIS WIFI: partition %u (%s <-> %s):\n", i,
		    pw_host_name(entry.host0), pw_host_name(entry.host1));
		pw_survey(entry.offset, entry.size, entry.cacheline, &census);
		walked++;
	}
	printf("POLARIS WIFI: %u partitions walked, %u left alone, %u items seen, %u distinct "
	    "ids:", walked, skipped, census.items, census.count);
	for (uint32_t i = 0; i < census.count; i++) {
		printf(" %u", census.ids[i]);
	}
	printf("\n");
	printf("POLARIS WIFI: the WLAN set: smp2p ap %d remote %d, mss ap %d mss %d, "
	    "glink desc %d fifo %d, crash reason %d\n",
	    pw_census_has(&census, PW_ITEM_SMP2P_AP),
	    pw_census_has(&census, PW_ITEM_SMP2P_REMOTE),
	    pw_census_has(&census, PW_ITEM_SMP2P_MSS_AP),
	    pw_census_has(&census, PW_ITEM_SMP2P_MSS),
	    pw_census_has(&census, PW_ITEM_GLINK_DESC),
	    pw_census_has(&census, PW_ITEM_GLINK_FIFO),
	    pw_census_has(&census, PW_ITEM_CRASH_REASON));
	printf("POLARIS WIFI: read only; nothing was written\n");
}
#endif
