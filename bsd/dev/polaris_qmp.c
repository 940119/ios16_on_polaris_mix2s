/*
 * Polaris (Mi MIX 2S, SDM845), Wi-Fi step 2: look at the QMP mailbox and say
 * what state it is in.
 *
 * Nothing here writes. The Mi 8 port reaches the wireless subsystem through
 * QMP: a small mailbox in the always-on block that the AP and the AOP share,
 * four link/channel registers each way plus a 64-byte message box. wlfw uses
 * it to ask the AOP to load firmware into the WCN3990, so the WLAN port cannot
 * get past its first step without it, and the first thing worth knowing is
 * whether this phone's mailbox is where the Mi 8's is and whether somebody has
 * already opened it.
 *
 * The device tree agrees on the address:
 *
 *   /soc/qcom,qmp-aop@c300000
 *     compatible    qcom,qmp-mbox
 *     reg           0xc300000 0x100000   (msgram)
 *                   0x1799000c 0x4      (irq-reg-base: APCS IPC)
 *
 * which is exactly the Mi 8's pair, 0x0c300000 for the mailbox and 0x1799000c
 * for the interrupt register. That is the reason this build reads rather than
 * guesses: agreement in the device tree is a good sign, not proof that the
 * registers hold what the driver expects.
 *
 * Register offsets are Linux's (drivers/mailbox/qcom-qmp.c, sdm845 data), and
 * the Mi 8 port uses the same ones. Two parties write the same words from
 * opposite ends: what the AP calls "our" is "their" from the AOP's side, so the
 * pair of registers at 0x24 and 0x0c are the same handshake seen twice.
 *
 * A mailbox is open when all six of our/their link and channel bits read 0xffff
 * - the driver writes 0x0000ffff to mean up and 0xffff0000 to mean down, and
 * the boot loader leaves an opened mailbox open for whoever comes next. If it
 * is open already, wlfw can use it as it stands; if it is closed, opening it is
 * a write and belongs in a later build with its own failure mode.
 *
 * Boot with polarisqmp=1 to run it. Default: nothing at all.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define PQ_PHYS			0x0c300000ull	/* dt: qcom,qmp-aop@c300000 */
#define PQ_SIZE			0x1000u

#define PQ_MAGIC		0x00
#define PQ_VERSION		0x04
#define PQ_THEIR_LINK		0x0c
#define PQ_THEIR_LINK_ACK	0x10
#define PQ_THEIR_CHANNEL	0x14
#define PQ_THEIR_CHANNEL_ACK	0x18
#define PQ_OUR_LINK		0x24
#define PQ_OUR_LINK_ACK		0x28
#define PQ_OUR_CHANNEL		0x2c
#define PQ_OUR_CHANNEL_ACK	0x30
#define PQ_OUR_BOX_SIZE		0x34
#define PQ_OUR_BOX		0x38
#define PQ_MESSAGE		64u

#define PQ_MAGIC_VALUE		0x4d41494c	/* "MAIL" */
#define PQ_UP			0x0000ffffu
#define PQ_DOWN			0xffff0000u

#define PQ_APCS_PHYS		0x17990000ull	/* dt: irq-reg-base */
#define PQ_APCS_IPC		0x0c
#define PQ_APCS_AOP		(1u << 0)	/* "the AOP has something for us" */

static vm_offset_t pq_base;

static uint32_t
pq_rd(uint32_t offset)
{
	return *(const volatile uint32_t *)(pq_base + offset);
}

/* A link or channel register reads as one of the two words the driver writes,
 * or as something else, which is worth printing rather than rounding off. */
static const char *
pq_state(uint32_t value)
{
	if (value == PQ_UP) {
		return "up";
	}
	if (value == PQ_DOWN) {
		return "down";
	}
	return "neither up nor down";
}

static bool
polaris_qmp_requested(void)
{
	int optin = 0;

	if (!PE_parse_boot_argn("polarisqmp", &optin, sizeof(optin))) {
		return false;
	}
	return optin == 1;
}

/*
 * The mailbox, as a block of words, when the magic is not where it belongs.
 *
 * The Mi 8's layout is the only one this port knows, and a wrong offset would
 * otherwise show up as a column of "neither up nor down" with nothing to say
 * about what the real layout is. Sixteen words is enough to see a magic, a
 * version and a handshake hiding somewhere else in the block.
 */
static void
pq_dump(void)
{
	for (uint32_t at = 0; at < 0x40; at += 0x10) {
		printf("POLARIS QMP:  %#04x: %08x %08x %08x %08x\n", at, pq_rd(at),
		    pq_rd(at + 4), pq_rd(at + 8), pq_rd(at + 0xc));
	}
}

void
polaris_qmp_probe(void)
{
	uint32_t magic, version, box, size, ipc;
	vm_offset_t apcs;
	bool open;

	if (!polaris_qmp_requested()) {
		return;
	}
	pq_base = ml_io_map((vm_offset_t)PQ_PHYS, PQ_SIZE);
	if (pq_base == 0) {
		printf("POLARIS QMP: the mailbox at %#llx did not map\n", PQ_PHYS);
		return;
	}
	printf("POLARIS QMP: ======== build 0166 alive: reading the QMP mailbox at %#llx, "
	    "nothing else ========\n", PQ_PHYS);

	magic = pq_rd(PQ_MAGIC);
	version = pq_rd(PQ_VERSION);
	printf("POLARIS QMP: magic %#x (%s), version %u\n", magic,
	    magic == PQ_MAGIC_VALUE ? "\"MAIL\"" : "not \"MAIL\"", version);
	if (magic != PQ_MAGIC_VALUE) {
		printf("POLARIS QMP: this is not the mailbox the Mi 8 port knows; the block "
		    "as it reads:\n");
		pq_dump();
		return;
	}

	printf("POLARIS QMP: their link %#x (%s), its ack %#x (%s)\n",
	    pq_rd(PQ_THEIR_LINK), pq_state(pq_rd(PQ_THEIR_LINK)),
	    pq_rd(PQ_THEIR_LINK_ACK), pq_state(pq_rd(PQ_THEIR_LINK_ACK)));
	printf("POLARIS QMP: our link %#x (%s), its ack %#x (%s)\n",
	    pq_rd(PQ_OUR_LINK), pq_state(pq_rd(PQ_OUR_LINK)),
	    pq_rd(PQ_OUR_LINK_ACK), pq_state(pq_rd(PQ_OUR_LINK_ACK)));
	printf("POLARIS QMP: their channel %#x (%s), its ack %#x (%s)\n",
	    pq_rd(PQ_THEIR_CHANNEL), pq_state(pq_rd(PQ_THEIR_CHANNEL)),
	    pq_rd(PQ_THEIR_CHANNEL_ACK), pq_state(pq_rd(PQ_THEIR_CHANNEL_ACK)));
	printf("POLARIS QMP: our channel %#x (%s), its ack %#x (%s)\n",
	    pq_rd(PQ_OUR_CHANNEL), pq_state(pq_rd(PQ_OUR_CHANNEL)),
	    pq_rd(PQ_OUR_CHANNEL_ACK), pq_state(pq_rd(PQ_OUR_CHANNEL_ACK)));

	box = pq_rd(PQ_OUR_BOX);
	size = pq_rd(PQ_OUR_BOX_SIZE);
	printf("POLARIS QMP: our message box at +%#x, %u bytes", box, size);
	if (size < PQ_MESSAGE || box < 0x3c || box + size > PQ_SIZE || (box & 3)) {
		printf(": not a box this can use\n");
	} else {
		printf(": %u words of room, the first four read %08x %08x %08x %08x\n",
		    size / 4, pq_rd(box), pq_rd(box + 4), pq_rd(box + 8), pq_rd(box + 0xc));
	}

	open = pq_rd(PQ_OUR_LINK) == PQ_UP && pq_rd(PQ_OUR_LINK_ACK) == PQ_UP &&
	    pq_rd(PQ_OUR_CHANNEL) == PQ_UP && pq_rd(PQ_THEIR_CHANNEL) == PQ_UP &&
	    pq_rd(PQ_OUR_CHANNEL_ACK) == PQ_UP;
	printf("POLARIS QMP: %s\n", open ?
	    "the mailbox is already open: wlfw can use it as it stands" :
	    "the mailbox is not open: opening it is a write, and that is a later build");

	/*
	 * The interrupt register, one word in the APCS block. Bit 0 is the AOP's
	 * line; the Mi 8 port rings it after every handshake step, so its
	 * current value says whether anything is pending right now.
	 */
	apcs = ml_io_map((vm_offset_t)PQ_APCS_PHYS, 0x1000);
	if (apcs == 0) {
		printf("POLARIS QMP: the APCS block at %#llx did not map\n", PQ_APCS_PHYS);
		return;
	}
	ipc = *(const volatile uint32_t *)(apcs + PQ_APCS_IPC);
	printf("POLARIS QMP: APCS IPC at %#llx reads %#x: the AOP's line is %s\n",
	    PQ_APCS_PHYS + PQ_APCS_IPC, ipc, (ipc & PQ_APCS_AOP) ? "raised" : "quiet");
	printf("POLARIS QMP: read only; nothing was written\n");
}
#endif
