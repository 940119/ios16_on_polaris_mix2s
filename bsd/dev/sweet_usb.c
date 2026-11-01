/*
 * SWEET: USB bring-up, step 1 - read-only state probe.
 *
 * The phone's USB is a Synopsys DWC3 (USB 2.0 only, maximum-speed
 * high-speed) at 0x0a600000 behind Qualcomm's dwc-usb3-msm wrapper, with a
 * QUSB2 PHY at 0x088e2000; its power domain is the USB30_PRIM GDSC and its
 * clocks come from GCC (offsets from Linux gcc-sdmmagpie.c). ABL uses this
 * controller for fastboot; this probe only reports what state it is left
 * in after `fastboot boot`. It never writes a register, and it touches the
 * DWC3 / PHY register blocks only when the GCC status registers say their
 * power domain and clocks are on (an access to a gated block would hang).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <net/if_media.h>
#include <net/dlil.h>
#include <sys/kern_event.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
void sweet_usb_probe(void);
static void sweet_usb_gadget_start(vm_offset_t dwc);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

#define GCC_PA                  0x00100000
#define GCC_SIZE                0x00090000
#define GCC_USB30_PRIM_GDSCR    0x0f004         /* bit 31: power on */
#define CBCR_CLK_OFF            (1u << 31)

/*
 * The names carry the offset because three of these were read for five builds
 * under a name that did not match what the register is. 0x0f010/0x0f014/0x0f018
 * are the USB30_PRIM CBCR block in order (master, mock-UTMI, sleep) and
 * 0x0f01c is the mock-UTMI CMD_RCGR, which legitimately reads 0x00100105 - an
 * RCG is not a CBCR and its bit 0 is CLK_ENABLE, not CLK_OFF. Reading 0x0f018
 * as "the UTMI clock" produced the "clk utmi ... STILL OFF" line that sent two
 * builds after a clock fault that did not exist. The last four offsets came
 * from another SoC and are printed only because they have always been printed.
 */
static const struct {
	const char *name;
	uint32_t    off;
} usb_cbcr[] = {
	{ "master@f010", 0x0f010 },
	{ "mockutmi@f014", 0x0f014 },
	{ "sleep@f018", 0x0f018 },
	{ "cfgnocaxi@502c", 0x0502c },
	{ "aggreaxi@8201c", 0x8201c },
	{ "ahb2phy@6a004", 0x6a004 },
	{ "clkref@8c010", 0x8c010 },
};

#define DWC3_PA                 0x0a600000
#define DWC3_SIZE               0x0000d000
#define QUSB2_PA                0x088e2000
#define QUSB2_SIZE              0x00000400

static uint32_t
usb_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
usb_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Put the console on the disk before the next thing that might stop the machine.
 *
 * 0149 was built with every correction 0148 had - CMDACT back on bit 10, the
 * QUSB2 table from the qusb node the dwc3 actually points at, DSTS.DevCtrlHlt
 * back on bit 22 - and the ring still came back holding 0147's text: the
 * 0147-only line "clk utmi ... STILL OFF", the 0147 strategy order
 * (resume+utmi first, noreset third), and launchd's own boot-args line saying
 * sweetfb_noconsole=1 while both 0148c and 0149 set it to 0. So the machine
 * that wrote that ring was not running this code.
 *
 * The ring is flushed by a thread once a second. The USB probe runs inside that
 * first second, so "the ring has no 0149 lines" cannot be told apart from "0149
 * never ran", and four consecutive builds have now been read that way. A flush
 * is one polled UFS write, which the probe can afford before every step that
 * touches the core; the GPU ladder has done exactly this since 0134 and its
 * checkpoints survive steps that hang the SoC.
 */
extern void sweet_log_flush_now(void);

#define US_CHECK(what)	do { \
	printf("SWEET USB: -- at %s\n", (what)); \
	sweet_log_flush_now(); \
} while (0)

/*
 * Switch on the USB3 controller's clock branches.
 *
 * The comment here used to say that 0146 had confirmed CMDACT on bit 16. It had
 * not: bit 16 is CommandParam[0], so 0146 and 0147 wrote every endpoint command
 * as "parameter 1, CMDACT clear", the core executed nothing, and the 500 ms poll
 * reported a timeout for all four strategies. 0148 moved CMDACT to bit 10, which
 * is what Linux's core.h says, and that is what this build keeps.
 *
 * The mock-UTMI branch is GCC_USB30_PRIM_MOCK_UTMI_CLK_CBCR at 0x0f014. An
 * earlier build polled 0x0f018 and called it "STILL OFF"; 0x0f018 is the RCG's
 * CMD_RCGR, which is legitimately zero under clk_rcg2_shared_ops. The real CBCR
 * reads back with bit 0 set, so the UTMI clock was never the fault.
 *
 * The window is dumped first because 0x0f000 is the block's BCR: if its bit 0
 * is set the block reset is still asserted and no clock will help.
 */
static void
ud_usb_clocks_on(vm_offset_t gcc)
{
	uint32_t off;

	printf("SWEET USB: gcc usb30_prim before:");
	for (off = 0; off < 0x40; off += 4) {
		printf(" %02x=%08x", off, usb_rd(gcc, 0x0000f000 + off));
	}
	printf("\n");

	/* BCR bit 0 is BLK_ARES: 1 means the block is held in reset. */
	if (usb_rd(gcc, 0x0000f000) & 1u) {
		printf("SWEET USB: gcc usb30_prim BCR has the block in reset; releasing it\n");
		usb_wr(gcc, 0x0000f000, usb_rd(gcc, 0x0000f000) & ~1u);
		delay(500);
		printf("SWEET USB: gcc usb30_prim BCR now 0x%08x\n", usb_rd(gcc, 0x0000f000));
	}

	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		uint32_t before = usb_rd(gcc, usb_cbcr[i].off);
		if (before & 1u) {
			continue;
		}
		usb_wr(gcc, usb_cbcr[i].off, (before & ~CBCR_CLK_OFF) | 1u);
		delay(200);
		printf("SWEET USB: clk %-12s 0x%08x -> 0x%08x (%s)\n", usb_cbcr[i].name,
		    before, usb_rd(gcc, usb_cbcr[i].off),
		    (usb_rd(gcc, usb_cbcr[i].off) & 1u) ? "enable bit took" : "STILL OFF");
	}

	printf("SWEET USB: gcc usb30_prim after:");
	for (off = 0; off < 0x40; off += 4) {
		printf(" %02x=%08x", off, usb_rd(gcc, 0x0000f000 + off));
	}
	printf("\n");
}

void
sweet_usb_probe(void)
{
	int enabled = 0;
	uint32_t gdscr, cbcr[sizeof(usb_cbcr) / sizeof(usb_cbcr[0])];
	bool clocks_on = true;

	/*
	 * sweetusb=0 (default) does nothing at all, 1 reports the controller's
	 * state from registers that are always powered, 2 also touches the PHY
	 * and starts the device.
	 *
	 * Off by default because this is not a working driver yet, and because
	 * build 0112 showed what a single untoward access costs: its first PHY
	 * access was a read, and the core stopped there instead of faulting.
	 * A phone that boots is worth more than a diagnostic line, so the
	 * diagnostics have to be asked for.
	 */
	PE_parse_boot_argn("sweetusb", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	/*
	 * Say which build this is before anything that could stop the machine,
	 * and put it on the disk.
	 *
	 * Every ring read since 0148 has come back holding the previous build's
	 * text, and there was no single line anywhere that could answer "did
	 * the new kernel run at all". This one can: it is printed before the
	 * first register access and flushed immediately, so either it is in the
	 * ring or the kernel that wrote the ring was not this one.
	 */
	printf("SWEET USB: ======== build 0156 alive: PORT_POWERDOWN sweep, every monitor "
	    "sample printed ========\n");
	sweet_log_flush_now();
	vm_offset_t gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("SWEET USB: GCC map failed\n");
		return;
	}
	gdscr = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
	printf("SWEET USB: GDSC usb30_prim 0x%08x (%s)\n", gdscr, (gdscr & (1u << 31)) ? "on" : "off");
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		cbcr[i] = usb_rd(gcc, usb_cbcr[i].off);
		printf("SWEET USB: clk %-12s CBCR 0x%08x (%s)\n", usb_cbcr[i].name, cbcr[i],
		    (cbcr[i] & CBCR_CLK_OFF) ? "off" : "on");
	}
	/* DWC3 registers need the GDSC, the master clock and the config-NoC path. */
	clocks_on = (gdscr & (1u << 31)) && !(cbcr[0] & CBCR_CLK_OFF) && !(cbcr[3] & CBCR_CLK_OFF);
	if (!clocks_on) {
		printf("SWEET USB: controller unpowered or unclocked; DWC3 not read\n");
		return;
	}
	/*
	 * 0147: switch on whatever branch the boot loader left off, then re-read
	 * so everything below runs against the real clock state.
	 */
	ud_usb_clocks_on(gcc);
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		cbcr[i] = usb_rd(gcc, usb_cbcr[i].off);
	}
	vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
	if (dwc == 0) {
		printf("SWEET USB: DWC3 map failed\n");
		return;
	}
	printf("SWEET USB: DWC3 GSNPSID 0x%08x GCTL 0x%08x GSTS 0x%08x GUSB2PHYCFG 0x%08x\n",
	    usb_rd(dwc, 0xc120), usb_rd(dwc, 0xc110), usb_rd(dwc, 0xc118), usb_rd(dwc, 0xc200));
	printf("SWEET USB: DWC3 DCFG 0x%08x DCTL 0x%08x DEVTEN 0x%08x DSTS 0x%08x DALEPENA 0x%08x\n",
	    usb_rd(dwc, 0xc700), usb_rd(dwc, 0xc704), usb_rd(dwc, 0xc708), usb_rd(dwc, 0xc70c),
	    usb_rd(dwc, 0xc720));
	printf("SWEET USB: DWC3 GEVNTADR 0x%08x%08x GEVNTSIZ 0x%08x GEVNTCOUNT 0x%08x GHWPARAMS0 0x%08x\n",
	    usb_rd(dwc, 0xc404), usb_rd(dwc, 0xc400), usb_rd(dwc, 0xc408), usb_rd(dwc, 0xc40c),
	    usb_rd(dwc, 0xc140));
	if (enabled >= 2) {
		sweet_usb_gadget_start(dwc);
	}
	/*
	 * Everything below reads the PHY, which is the one block that has
	 * stopped this kernel before: it is not clocked the way this code
	 * assumed, so a read waits instead of returning. Level 1 therefore
	 * stops here, with the report above it complete, and only level 2 goes
	 * on to the PHY.
	 */
	if (enabled < 2) {
		return;
	}
	if (cbcr[5] & CBCR_CLK_OFF) {
		printf("SWEET USB: ahb2phy clock off; QUSB2 PHY not read\n");
		return;
	}
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	if (phy == 0) {
		return;
	}
	/* DT qcom,qusb-phy-reg-offset order (Linux phy-msm-qusb-v2.c): PORT_TUNE1 0x240,
	 * PLL_COMMON_STATUS_ONE 0x1a0, PWR_CTRL1 0x210, INTR_CTRL 0x230,
	 * PLL_CORE_INPUT_OVERRIDE 0x0a8, TEST1 0x254. */
	/*
	 * These are byte registers behind an AHB slave that repeats the byte on
	 * every lane, so the 32-bit read has to be masked: 0151 printed
	 * "PLL_STATUS 0x17171717" for what is really 0x17, and 0x17171717 was
	 * briefly read as four different values.
	 *
	 * 0x1a0 is PLL_COMMON_STATUS_ONE on this PHY generation, not the
	 * msm8996-era PLL_STATUS, which is why the ready bit is bit 0 here.
	 */
	printf("SWEET USB: QUSB2 PLL_COMMON_STATUS_ONE 0x%02x PWR_CTRL1 0x%02x "
	    "PORT_TUNE1 0x%02x TEST1 0x%02x\n",
	    usb_rd(phy, 0x1a0) & 0xff, usb_rd(phy, 0x210) & 0xff,
	    usb_rd(phy, 0x240) & 0xff, usb_rd(phy, 0x254) & 0xff);
}

/*
 * Step 2: USB device mode with only the default control endpoint, enough
 * for the host to enumerate the phone (boot-arg sweetusb=2). ABL leaves
 * the DWC3 powered, clocked, in device mode and halted, with the PHY set
 * up, so only the device controller is restarted: soft reset, an event
 * buffer in our memory, endpoint 0, run. Events are polled from a kernel
 * thread (no interrupt), like the UFS driver. DMA is not cache coherent
 * (DT qcom,gsi-disable-io-coherency): buffers are cleaned/invalidated.
 * Register layout and the control-transfer state machine follow the
 * Synopsys DWC3 databook as used by Linux drivers/usb/dwc3 (gadget.c,
 * ep0.c).
 */
#define GRXTHRCFG       0xc108
#define   GRXTHRCFG_PKTCNTSEL (1u << 29)
#define   DCFG_IGNSTRMPP      (1u << 23)
#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)   (0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)   (0xc808 + 0x10 * (n))
#define DEPCMD(n)       (0xc80c + 0x10 * (n))

/*
 * Qualcomm wrapper registers ("QSCRATCH", core base + 0xf8800). The
 * controller only connects (pull-up on D+) while it sees a valid VBUS
 * session; Linux dwc3-qcom forces that in software when the charger
 * reports a cable (dwc3_qcom_vbus_override_enable).
 */
#define QSCRATCH_PA             0x0a6f8800
#define QSCRATCH_HS_PHY_CTRL    0x10
/*
 * SDM845's HS_PHY_CTRL has exactly two bits, both from mainline
 * drivers/usb/dwc3/dwc3-qcom.c:
 *     UTMI_OTG_VBUS_VALID = BIT(20)
 *     SW_SESSVLD_SEL      = BIT(28)
 *
 * The five names after them are NOT part of this register. They come from
 * the older phy-msm-usb layout for an earlier SoC generation, where the
 * wrapper did own the D+ pull-up and the UTMI clock. On SDM845 both live
 * inside the QUSB2 PHY, and writing them here does nothing - which is what
 * 0150 measured: after writing bits 13, 14, 20, 21, 22, 23 and 28, only 20
 * and 28 read back, and the writable-mask probe answered 0x10100000, i.e.
 * exactly the two real bits and nothing else. They are kept only so the
 * probe re-measures that every build.
 */
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define   VBUSVLDEXT0           (1u << 13)  /* not this register: see above */
#define   VBUSVLDEXTSEL0        (1u << 14)  /* not this register */
#define   USB2_UTMI_CLK_EN      (1u << 21)  /* not this register */
#define   USB2_SUSPEND_N        (1u << 22)  /* not this register */
#define   USB2_SUSPEND_N_SEL    (1u << 23)  /* not this register */
#define QSCRATCH_SS_PHY_CTRL    0x30
#define   LANE0_PWR_PRESENT     (1u << 24)
/*
 * Bits from mainline drivers/usb/dwc3/dwc3-qcom.c, which is the driver this
 * SoC's "qcom,sdm845-dwc3" node binds to. The msm8996-era encoding this file
 * used to carry (SEL bit 1, PIPE3 bit 2, DIS bit 0) put the clock select
 * somewhere the hardware does not define it.
 */
#define QSCRATCH_GENERAL_CFG    0x08
#define   PIPE_UTMI_CLK_SEL     (1u << 0)
#define   PIPE3_PHYSTATUS_SW    (1u << 3)
#define   PIPE_UTMI_CLK_DIS     (1u << 8)

/*
 * DCTL.RUN_STOP: SET means run, CLEAR means stop.
 *
 * This file had it backwards until 0153 and every attach in 0148-0152 was
 * therefore a detach. Linux settles it: dwc3_gadget_run_stop(dwc, is_on) does
 * `reg |= DWC3_DCTL_RUN_STOP` when is_on is true and `reg &= ~...` when it is
 * false, so setting the bit starts the controller. core.h calls it
 * DWC3_DCTL_RUN_STOP	BIT(31).
 *
 * The register readings agree, once DevCtrlHlt is read the way core.h defines
 * it (BIT(22), a real halt flag):
 *
 *   DCTL 0x80f00000 (bit31 = 1, RUN)   DSTS 0x00920001 (bit22 = 0, running)
 *   DCTL 0x00f00000 (bit31 = 0, STOP)  DSTS 0x00d20001 (bit22 = 1, halted)
 *
 * 0152 read those two rows the other way round and concluded that bit 22 was
 * not a halt flag. It is one; the polarity of RUN_STOP was the thing that was
 * wrong. So ud_detach() was starting the core and ud_attach_and_wait() was
 * halting it, which is exactly why no build since 0148 ever saw a bus reset:
 * during the whole four-second window the controller was halted and DevCtrlHlt
 * said so, twenty samples in a row, with GEVNTCOUNT at zero because a halted
 * controller raises no events.
 */
#define DCTL_RUN_STOP   (1u << 31)   /* 1 = run, 0 = stop */
#define DCTL_CSFTRST    (1u << 30)
/* dwc3 core.h: DWC3_GUSB2PHYCFG_ENBLSLPM BIT(8), DWC3_GUSB2PHYCFG_SUSPHY BIT(6). */
#define GUSB2PHYCFG_ENBLSLPM (1u << 8)
/*
 * DSTS, from Linux dwc3 core.h:
 *   [2:0]   ConnectSpeed (0=HS, 1=FS, 2=LS, 4=SS)
 *   [17:3]  SOFFN   [17] RxFifoEmpty   [21:18] USBLNKST
 *   [22] DevCtrlHlt   [23] CoreIdle
 *
 * 0146 "fixed" this to bit 9, which is SOFFN[6]. At handover DSTS reads
 * 0x00520004, so bit 9 is set and the detach loop believed the core was
 * permanently halted - it was reading a frame counter.
 */
#define DSTS_BIT20      (1u << 20)
#define DSTS_DEVCTRLHLT (1u << 22)
#define DSTS_BIT23      (1u << 23)
#define DSTS_SPEED(v)   ((v) & 7u)
#define DSTS_SOFFN(v)   (((v) >> 3) & 0x3fffu)
#define DSTS_LNKST(v)   (((v) >> 18) & 0xfu)

/*
 * GSTS tells whether the controller thinks a cable is on the port at all, and
 * it is the one register the four attach attempts never looked at. Bits 12, 13
 * and 14 are BSessVld, BCable and BValid in Linux's core.h; build 0154 read
 * GSTS 0x7e800000, i.e. all three clear, which is why no bus reset ever came.
 * Print them by name so the next round does not have to decode the hex.
 */
#define GSTS_BSESSVLD   (1u << 12)
#define GSTS_BCABLE     (1u << 13)
#define GSTS_BVALID     (1u << 14)

#define DEPCMD_SETEPCONFIG      1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL         4
#define DEPCMD_STARTTRANSFER    6
#define DEPCMD_DEPSTARTCFG      9
/*
 * DEPCMD, verbatim from Linux drivers/usb/dwc3/core.h:
 *
 *   #define DWC3_DEPCMD_PARAM_SHIFT   16
 *   #define DWC3_DEPCMD_CMDACT        BIT(10)
 *   #define DWC3_DEPCMD_HIPRI_FORCERM BIT(11)
 *   #define DWC3_DEPCMD_STATUS(x)     (((x) >> 12) & 0x0F)
 *
 * Builds 0146 and 0147 declared CMDACT as bit 16. Bit 16 is
 * CommandParam[0], so every endpoint command was written as
 * "type | (1 << 16)" - a command with parameter 1 and CMDACT clear. The
 * core never executed a single one, and the poll then reported a 500 ms
 * timeout on all four strategies, which looked like a hardware failure.
 * That is also why 0146/0147 made every strategy fail in exactly the same
 * way, when 0145 had one that actually configured endpoint 0.
 */
#define DEPCMD_CMDACT           (1u << 10)
#define DEPCMD_HIPRI_FORCERM    (1u << 11)
#define DEPCMD_STATUS_SHIFT     12
#define DEPCMD_STATUS_MASK      0xfu

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* DMA area layout */
#define UD_EVT          0x0000          /* event buffer, 4 KiB */
#define UD_EVT_SIZE     0x1000
#define UD_TRB_OUT      0x1000          /* ep0 OUT TRB */
#define UD_TRB_IN       0x1010          /* ep0 IN TRB */
#define UD_TRB_RX       0x1020          /* ep1 OUT (bulk, host -> phone) */
#define UD_TRB_TX       0x1030          /* ep1 IN (bulk, phone -> host) */
#define UD_TRB_INT      0x1050          /* ep2 IN (interrupt, notifications) */
#define UD_SETUP        0x1040          /* 8-byte setup packet */
#define UD_DATA         0x1100          /* ep0 data stage, 512 bytes */
#define UD_NOTIFY       0x1300          /* 16-byte notification */
#define UD_RX           0x2000          /* one received frame */
#define UD_TX           0x2800          /* one frame to send */
#define UD_FRAME_MAX    0x800
#define UD_SIZE         0x4000

/* Physical endpoints: 2n = OUT, 2n+1 = IN */
#define EP_RX           2               /* 0x01 bulk OUT */
#define EP_TX           3               /* 0x81 bulk IN */
#define EP_INT          5               /* 0x82 interrupt IN */
#define TRBCTL_NORMAL   (1u << 4)

#define UD_TXQ          32

static struct {
	vm_offset_t dwc;
	vm_offset_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	int         sleep_chan;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool        data_in;        /* data stage direction */
	uint32_t    events;
	bool        configured;
	uint8_t     data_alt;       /* data interface alternate setting */
	/* network */
	ifnet_t     ifp;
	lck_mtx_t  *lock;           /* TX ring and TX endpoint */
	mbuf_t      txq[UD_TXQ];
	unsigned    txq_head, txq_count;
	bool        tx_busy;
	uint64_t    rx_frames, tx_frames;
} ud;

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
dwc_rd(uint32_t off)
{
	return usb_rd(ud.dwc, off);
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ud.dma_pa + off), len, TRUE);
}

static void
ud_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ud.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	printf("SWEET USB: epcmd ep%u cmd 0x%x PAR 0x%08x 0x%08x 0x%08x writing\n",
	    ep, cmd & 0xf, p0, p1, p2);
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	printf("SWEET USB: epcmd ep%u PAR written, firing DEPCMD\n", ep);
	sweet_log_flush_now();
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	printf("SWEET USB: epcmd ep%u DEPCMD 0x%08x, polling\n", ep, dwc_rd(DEPCMD(ep)));
	/*
	 * Two phases: CMDACT latches almost immediately after the write and
	 * drops when the core has executed the command.
	 *
	 * "active" is reported on every path, not only the failure one. A
	 * single command coming back with active 0 can just mean the core
	 * was faster than this read; every command coming back that way
	 * means CMDACT is being written to the wrong bit - which is exactly
	 * what 0146/0147 did (bit 16 is CommandParam[0]) and why all four
	 * strategies then failed identically.
	 *
	 * CmdStatus is printed even when CMDACT cleared, because a command
	 * the core rejects outright clears CMDACT at once and leaves a
	 * non-zero status. The old code called that "ok polls 0".
	 *
	 * delay() is microseconds here (the soft-reset loop uses delay(1000)
	 * per millisecond), so 5000 iterations is the 500 ms Linux allows.
	 */
	int i;
	int active = 0;
	uint32_t v = dwc_rd(DEPCMD(ep));
	for (i = 0; i < 5000; i++) {
		v = dwc_rd(DEPCMD(ep));
		if (v & DEPCMD_CMDACT) {
			active = 1;
		} else if (active) {
			break;
		} else if (i > 100) {
			/* 10 ms and CMDACT was never seen: do not burn 500. */
			break;
		}
		delay(100);
	}
	{
		uint32_t status = (v >> DEPCMD_STATUS_SHIFT) & DEPCMD_STATUS_MASK;
		int stuck = (v & DEPCMD_CMDACT) != 0;

		printf("SWEET USB: ep%u cmd 0x%x -> DEPCMD 0x%08x status %u active %d "
		    "polls %d (%d ms) PAR0 0x%08x PAR1 0x%08x PAR2 0x%08x%s\n",
		    ep, cmd & 0xf, v, status, active, i, i / 10,
		    dwc_rd(DEPCMDPAR0(ep)), dwc_rd(DEPCMDPAR1(ep)), dwc_rd(DEPCMDPAR2(ep)),
		    stuck ? " STUCK" : (status ? " REJECTED" : ""));
		/*
		 * The whole point of the build: get this line on the disk even
		 * if the next command is the one that stops the machine.
		 */
		sweet_log_flush_now();
		if (!stuck && status == 0) {
			return 0;
		}
		if (!stuck) {
			return -1;
		}
	}
	flush_dcache64((addr64_t)(ud.dma_pa + UD_EVT), 64, TRUE);
	{
		uint32_t *ev = (uint32_t *)(ud.dma + UD_EVT);
		printf("SWEET USB: evtbuf %08x %08x %08x %08x | %08x %08x %08x %08x\n",
		    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
	}
	printf("SWEET USB: GEVNTADR 0x%08x%08x GEVNTSIZ 0x%08x GEVNTCOUNT 0x%08x DEVTEN 0x%08x\n",
	    dwc_rd(GEVNTADRHI), dwc_rd(GEVNTADRLO), dwc_rd(GEVNTSIZ), dwc_rd(GEVNTCOUNT), dwc_rd(DEVTEN));
	printf("SWEET USB: DCTL 0x%08x DSTS 0x%08x DCFG 0x%08x GCTL 0x%08x DALEPENA 0x%08x\n",
	    dwc_rd(DCTL), dwc_rd(DSTS), dwc_rd(DCFG), dwc_rd(GCTL), dwc_rd(DALEPENA));
	printf("SWEET USB: dma_pa 0x%llx (%s 4GiB)\n", ud.dma_pa,
	    ud.dma_pa >= 0x100000000ULL ? ">=" : "<");
	return -1;
}

/*
 * The low DMA window is mapped as device memory (ml_io_map): bulk copies must
 * not use NEON/block loads there, only naturally aligned 32-bit accesses.
 */
static void
ud_copy32(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	while (n >= 4) {
		*(uint32_t *)d = *(const uint32_t *)s;
		d += 4;
		s += 4;
		n -= 4;
	}
	while (n) {
		*d++ = *s++;
		n--;
	}
}

static uint8_t ud_txbuf[UD_FRAME_MAX];

/* One TRB (at DMA offset trb) on physical endpoint ep. */
/*
 * Statement-level trace, because this call has now killed two boots and the
 * only thing known about it is that it does not come back. Each step prints
 * before it happens, so whatever is stuck is the line after the last one
 * that appeared. That is the difference between guessing and measuring.
 */
static void
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	printf("SWEET USB: trb ep%u begin buf 0x%llx len %u ctl 0x%x\n", ep, bpa, len, ctl);
	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	printf("SWEET USB: trb written %08x %08x %08x %08x, syncing\n", t[0], t[1], t[2], t[3]);
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	uint64_t tpa = ud.dma_pa + trb;
	printf("SWEET USB: trb synced, STARTTRANSFER ep%u trb 0x%llx\n", ep, tpa);
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
	printf("SWEET USB: trb ep%u returned\n", ep);
}

static void
ud_start(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, buf, len, ctl);
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	ud_start(0, UD_SETUP, 8, TRBCTL_SETUP);
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/*
 * USB descriptors: a CDC-ECM (Ethernet) function, which Linux (cdc_ether)
 * and macOS drive without extra drivers. VID 0x1209 / PID 0x0001 =
 * pid.codes test ID, not a product.
 */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[80] = {
	9, 2, 80, 0, 2, 1, 0, 0x80, 250,                /* config: 2 interfaces, 500 mA */
	9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,             /* comm interface 0: CDC ECM, 1 endpoint */
	5, 0x24, 0x00, 0x10, 0x01,                      /* header, CDC 1.10 */
	5, 0x24, 0x06, 0, 1,                            /* union: control 0, data 1 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0, /* ethernet: MAC string 4, max segment 1514 */
	7, 5, 0x82, 0x03, 16, 0, 9,                     /* ep 0x82 interrupt IN, 16 bytes, 2^(9-1) uframes */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 0: no endpoints */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 1: two bulk endpoints */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* ep 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* ep 0x01 bulk OUT, 512 */
};
/* The host side's MAC (ECM iMACAddress); the phone's own is ..:02 */
static const char *const ud_strings[] = {
	NULL, "ios-sweet", "Sweet iOS (USB Ethernet)", "0001", "02005E77EE01",
};
static const uint8_t ud_phone_mac[ETHER_ADDR_LEN] = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 };

static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx >= sizeof(ud_strings) / sizeof(ud_strings[0])) {
		return 0;
	}
	const char *str = ud_strings[idx];
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void ud_rx_arm(void);
static void ud_tx_kick_locked(void);
static void ud_link_event(bool up);

/* NETWORK_CONNECTION notification: the host brings its link up on it. */
static void
ud_notify_connected(void)
{
	uint8_t *n = (uint8_t *)(ud.dma + UD_NOTIFY);
	n[0] = 0xa1; n[1] = 0x00;       /* NETWORK_CONNECTION */
	n[2] = 1; n[3] = 0;             /* connected */
	n[4] = 0; n[5] = 0;             /* interface 0 */
	n[6] = 0; n[7] = 0;
	ud_start_trb(EP_INT, UD_TRB_INT, UD_NOTIFY, 8, TRBCTL_NORMAL);
}

/* SET_CONFIGURATION(1): bulk IN/OUT and the interrupt endpoint. */
static void
ud_configure(void)
{
	if (ud.configured) {
		return;
	}
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("SWEET USB: data endpoints setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	ud.configured = true;
	ud_rx_arm();
	printf("SWEET USB: ECM endpoints configured\n");
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	printf("SWEET USB: setup %02x %02x %04x %04x len %u\n", type, req, value, index, length);
	ud.data_in = (type & 0x80) != 0;
	if ((type & 0x60) == 0x20) {    /* class request (e.g. SET_ETHERNET_PACKET_FILTER) */
		ok = (length == 0);
	} else if ((type & 0x60) != 0) {
		ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: ud_copy32(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: ud_copy32(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS: takes effect now (DWC3 handles the timing) */
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
		printf("SWEET USB: address %u\n", value & 0x7f);
	} else if (req == 9) {          /* SET_CONFIGURATION */
		if (value == 1) {
			ud_configure();
		}
		printf("SWEET USB: configured (%u)\n", value);
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11) {         /* SET_INTERFACE */
		if (index == 1) {
			ud.data_alt = (uint8_t)value;
			if (value == 1 && ud.configured) {
				ud_notify_connected();
				ud_link_event(true);
			}
		}
	} else if (req == 10) {         /* GET_INTERFACE */
		data[0] = index == 1 ? ud.data_alt : 0; n = 1;
	} else if (req == 1 || req == 3) { /* CLEAR/SET_FEATURE (halt, remote wakeup) */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;    /* status stage on XferNotReady */
		return;
	}
	ud.state = EP0_DATA;
	if (ud.data_in) {
		if (n > length) {
			n = length;
		}
		ud_start(1, UD_DATA, n, TRBCTL_DATA);
	} else {
		ud_start(0, UD_DATA, 64, TRBCTL_DATA);  /* OUT data is not used here */
	}
}

/* ---- network interface (ue0) ---- */

static void
ud_rx_arm(void)
{
	ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
}

static void
ud_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	ud_sync(UD_TRB_RX, 16);
	uint32_t len = UD_FRAME_MAX - (t[2] & 0xffffff);
	ud_sync(UD_RX, UD_FRAME_MAX);
	if (ud.ifp != NULL && len >= ETHER_HDR_LEN && len <= ETHER_MAX_LEN) {
		/*
		 * One mbuf with a 2 KiB cluster holds any frame. (mbuf_allocpacket
		 * may return a first mbuf of length 0: stripping the header from
		 * it panicked "Failed mbuf validity check: len -14".)
		 */
		mbuf_t m = NULL;
		if (mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) == 0 &&
		    mbuf_mclget(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) == 0 &&
		    (mbuf_flags(m) & MBUF_EXT) && mbuf_maxlen(m) >= len) {
			struct ifnet_stat_increment_param stats = {};
			ud_copy32(mbuf_data(m), (const void *)(ud.dma + UD_RX), len);
			mbuf_setlen(m, len);
			mbuf_pkthdr_setlen(m, len);
			void *frame_header = mbuf_data(m);
			mbuf_pkthdr_setrcvif(m, ud.ifp);
			mbuf_pkthdr_setheader(m, frame_header);
			mbuf_pkthdr_adjustlen(m, -ETHER_HDR_LEN);
			(void)mbuf_setdata(m, (char *)mbuf_data(m) + ETHER_HDR_LEN, mbuf_len(m) - ETHER_HDR_LEN);
			stats.packets_in = 1;
			stats.bytes_in = len;
			ud.rx_frames++;
			ifnet_input(ud.ifp, m, &stats);
		} else if (m != NULL) {
			mbuf_freem(m);
		}
	}
	ud_rx_arm();
}

/* Start the next queued frame if the IN endpoint is idle. ud.lock held. */
static void
ud_tx_kick_locked(void)
{
	while (!ud.tx_busy && ud.txq_count > 0 && ud.configured) {
		mbuf_t m = ud.txq[ud.txq_head];
		ud.txq_head = (ud.txq_head + 1) % UD_TXQ;
		ud.txq_count--;
		size_t len = mbuf_pkthdr_len(m);
		if (len > ETHER_MAX_LEN - ETHER_CRC_LEN) {
			mbuf_freem(m);
			continue;
		}
		mbuf_copydata(m, 0, len, ud_txbuf);
		ud_copy32((void *)(ud.dma + UD_TX), ud_txbuf, (uint32_t)len);
		mbuf_freem(m);
		/* A frame of a multiple of 512 bytes would need a zero-length
		 * packet to end it; one pad byte does the same (IP ignores it). */
		if ((len % 512) == 0) {
			((uint8_t *)(ud.dma + UD_TX))[len++] = 0;
		}
		ud.tx_busy = true;
		ud.tx_frames++;
		ud_start_trb(EP_TX, UD_TRB_TX, UD_TX, (uint32_t)len, TRBCTL_NORMAL);
	}
}

static errno_t
ud_if_output(ifnet_t ifp, mbuf_t m)
{
	lck_mtx_lock(ud.lock);
	if (ud.txq_count == UD_TXQ || !ud.configured) {
		lck_mtx_unlock(ud.lock);
		mbuf_freem(m);
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		return 0;
	}
	ifnet_stat_increment_out(ifp, 1, mbuf_pkthdr_len(m), 0);
	ud.txq[(ud.txq_head + ud.txq_count) % UD_TXQ] = m;
	ud.txq_count++;
	ud_tx_kick_locked();
	lck_mtx_unlock(ud.lock);
	return 0;
}

/*
 * Link state for configd (KernelEventMonitor reads SIOCGIFMEDIA and the
 * KEV_DL_LINK_ON/OFF events; IPMonitor does not route through a service
 * whose interface has no active link).
 */
static void
ud_link_event(bool up)
{
	if (ud.ifp != NULL) {
		dlil_post_msg(ud.ifp, KEV_DL_SUBCLASS, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF, NULL, 0, FALSE);
	}
}

static errno_t
ud_if_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
	switch (cmd) {
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64: {
		struct ifmediareq *ifmr = (struct ifmediareq *)data;
		user_addr_t list = (cmd == SIOCGIFMEDIA64) ?
		    ((struct ifmediareq64 *)ifmr)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)ifmr)->ifmu_ulist);
		int media = IFM_ETHER | IFM_AUTO;
		ifmr->ifm_current = media;
		ifmr->ifm_active = ud.configured ? (IFM_ETHER | IFM_1000_T | IFM_FDX) : media;
		ifmr->ifm_mask = 0;
		ifmr->ifm_status = IFM_AVALID | (ud.configured ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && ifmr->ifm_count > 0) {
			ifmr->ifm_count = 1;
			return copyout(&media, list, sizeof(media));
		}
		ifmr->ifm_count = 1;
		return 0;
	}
	case SIOCSIFADDR:
	case SIOCAIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		return 0;
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static void
ud_if_detach(ifnet_t ifp __unused)
{
}

/* ue0, 192.168.7.2/24 (the host side is 192.168.7.1). */
static void
ud_if_create(void)
{
	struct ifnet_init_eparams init;
	static lck_grp_t *grp;

	grp = lck_grp_alloc_init("sweet_usb", LCK_GRP_ATTR_NULL);
	ud.lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);

	bzero(&init, sizeof(init));
	init.ver = IFNET_INIT_CURRENT_VERSION;
	init.len = sizeof(init);
	init.flags = IFNET_INIT_LEGACY;
	init.uniqueid = "sweet-usb-ecm";
	init.uniqueid_len = sizeof("sweet-usb-ecm") - 1;
	init.name = "ue";
	init.unit = 0;
	init.family = IFNET_FAMILY_ETHERNET;
	init.type = IFT_ETHER;
	init.output = ud_if_output;
	init.demux = ether_demux;
	init.add_proto = ether_add_proto;
	init.del_proto = ether_del_proto;
	init.check_multi = ether_check_multi;
	init.framer_extended = ether_frameout_extended;
	init.ioctl = ud_if_ioctl;
	init.detach = ud_if_detach;
	init.broadcast_addr = etherbroadcastaddr;
	init.broadcast_len = ETHER_ADDR_LEN;
	if (ifnet_allocate_extended(&init, &ud.ifp) != 0) {
		printf("SWEET USB: ifnet_allocate failed\n");
		ud.ifp = NULL;
		return;
	}
	ifnet_set_addrlen(ud.ifp, ETHER_ADDR_LEN);
	ifnet_set_hdrlen(ud.ifp, sizeof(struct ether_header));
	ifnet_set_mtu(ud.ifp, ETHERMTU);
	ifnet_set_baudrate(ud.ifp, 480000000);
	ifnet_set_flags(ud.ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
	if (ifnet_attach(ud.ifp, NULL) != 0) {
		printf("SWEET USB: ifnet_attach failed\n");
		ud.ifp = NULL;
		return;
	}
	ifnet_set_lladdr(ud.ifp, ud_phone_mac, ETHER_ADDR_LEN);

	errno_t err = proto_plumb(PF_INET, ud.ifp);
	socket_t so = NULL;
	if (err == 0) {
		err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	}
	if (err == 0) {
		struct ifaliasreq ifra;
		bzero(&ifra, sizeof(ifra));
		snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "ue0");
		struct sockaddr_in *a = (struct sockaddr_in *)&ifra.ifra_addr;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xc0a80702);         /* 192.168.7.2 */
		a = (struct sockaddr_in *)&ifra.ifra_mask;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xffffff00);
		a = (struct sockaddr_in *)&ifra.ifra_broadaddr;
		a->sin_len = sizeof(*a);
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(0xc0a807ff);
		err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	ifnet_set_flags(ud.ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	printf("SWEET USB: ue0 192.168.7.2/24 %s (err %d)\n", err ? "NOT configured" : "up", err);
}

static void
ud_event(uint32_t ev)
{
	if (ev & 1) {                   /* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {        /* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			printf("SWEET USB: bus reset\n");
		} else if (type == 2) { /* connect done */
			uint32_t speed = dwc_rd(DSTS) & 7;
			/* ep0 max packet: 64 at high/full speed, action INIT */
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3),
			    (1u << 8) | (1u << 10) | (0u << 25), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3),
			    (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("SWEET USB: connected, speed %u\n", speed);
		} else if (type == 0) {
			printf("SWEET USB: disconnect\n");
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;

	if (ep == EP_RX && type == 1) {
		ud_rx_done();
		return;
	}
	if (ep == EP_TX && type == 1) {
		lck_mtx_lock(ud.lock);
		ud.tx_busy = false;
		ud_tx_kick_locked();
		lck_mtx_unlock(ud.lock);
		return;
	}
	if (ep > 1) {
		return;
	}
	/*
	 * Control transfer state machine (Linux dwc3 ep0.c): SETUP completes ->
	 * request handled; the data stage (if any) is started right away; the
	 * status stage is started when the controller reports XferNotReady
	 * with status "control status"; its completion re-arms SETUP.
	 */
	if (type == 1) {                /* XferComplete */
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) { /* XferNotReady, control status phase */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start(ep, UD_DATA, 0, ctl);
		}
	}
}

static void
ud_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t ticks = 0;
	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			ud_sync(UD_EVT, UD_EVT_SIZE);
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
				ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
				if (ud.events++ < 200) {
					printf("SWEET USB: event 0x%08x\n", ev);
				}
				ud_event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
		}
		if ((++ticks % 30000) == 0) {
			printf("SWEET USB: ue0 rx %llu tx %llu frames\n", ud.rx_frames, ud.tx_frames);
		}
		ud_sleep_ms(1);
	}
}

/*
 * Live console log over USB Ethernet: every 100 ms the new bytes of the
 * console ring (bsd/dev/sweet_ufs.c) go as UDP datagrams to
 * 192.168.7.1:5140 (on the PC: nc -ukl 5140). Starts with the last 64 KiB
 * so the boot is included. Nothing here may print (it would feed itself).
 * Boot-arg sweetusblog=0 disables it.
 */
extern uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);
extern uint64_t sweet_log_position(void);

static void
ud_log_thread(void *arg __unused, wait_result_t wr __unused)
{
	static char chunk[1400];
	socket_t so = NULL;
	struct sockaddr_in to;
	uint64_t pos = sweet_log_position();

	pos = pos > 65536 ? pos - 65536 : 0;
	bzero(&to, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_port = htons(5140);
	to.sin_addr.s_addr = htonl(0xc0a80701);         /* 192.168.7.1 */
	for (;;) {
		ud_sleep_ms(100);
		if (!ud.configured) {
			continue;
		}
		if (so == NULL && sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &so) != 0) {
			so = NULL;
			continue;
		}
		uint32_t n;
		while ((n = sweet_log_copy(&pos, chunk, sizeof(chunk))) > 0) {
			struct iovec iov = { .iov_base = chunk, .iov_len = n };
			struct msghdr msg = {
				.msg_name = &to, .msg_namelen = sizeof(to),
				.msg_iov = &iov, .msg_iovlen = 1,
			};
			size_t sent = 0;
			if (sock_send(so, &msg, 0, &sent) != 0) {
				break;          /* no route/ARP yet: retry later */
			}
		}
	}
}

/*
 * Step 2b: why endpoint commands hang.
 *
 * The controller is demonstrably alive - GSNPSID reads, the device soft
 * reset completes, and DEPCMD_DEPSTARTCFG finishes on the first poll - yet
 * DEPCFG (0x1), DEPXFERCFG (0x2) and DEPSTRTXFER (0x6) never clear CMDACT,
 * in every command order, with RUN_STOP set or clear, and with the event
 * buffer in low or high memory. So the shortlist is what Linux
 * dwc3_core_init() does after a core soft reset that this driver skipped:
 *
 *   UD_F_CLKGT     GCTL.DSBLCLKGTNG. GHWPARAMS1 EN_PWROPT is 1 (clock
 *                  gating present) and the polaris DT sets
 *                  snps,disable-clk-gating, which is exactly the DT knob
 *                  that makes Linux set this bit.
 *   UD_F_DALEPENA  raise DALEPENA before DEPCFG instead of after it.
 *   UD_F_FULLRESET the Linux dwc3_core_soft_reset() sequence: GCTL
 *                  CORESOFTRESET plus a USB2/USB3 PHY soft reset with
 *                  100 ms settling, not just DCTL.CSFTRST.
 *
 * And the possibility none of those experiments could see, because each of
 * them began with a reset:
 *
 *   UD_F_NORESET   leave the controller exactly as ABL handed it over.
 *   UD_F_VBUS      raise the QSCRATCH VBUS override before DEPCFG rather
 *                  than after it (where it was unreachable dead code).
 *
 * All of those were wrong, and the log finally said why. A QUSB2 register is
 * one byte wide but the AHB slave repeats it on every lane, so the four
 * "garbage" reads were really PLL_STATUS 0x64 (PLL_LOCKED set), TUNE1 0x72
 * and PORT_POWERDOWN 0x23 - and 0x23 is exactly Linux's qusb2_v2
 * disable_ctrl mask, POWER_DOWN included. The PHY was configured and then
 * left switched off, so the controller had no UTMI clock, so the endpoint
 * command engine - the part that runs off that clock - accepted DEPCFG,
 * raised CMDACT and never finished it, while DEPSTARTCFG (bus clock only)
 * completed instantly. Endpoint 0 could not have worked under any of the
 * earlier hypotheses, because the fault was upstream of the controller.
 *
 * So now every strategy runs the DT qcom,qusb-phy-init-seq first, which
 * ends by clearing POWER_DOWN, and the log prints the PLL lock bit.
 */
#define GSTS                0xc118
#define GUSB2PHYCFG         0xc200
#define GUSB3PIPECTL        0xc2c0
#define GHWPARAMS7          0xc15c
#define GRXFIFOSIZ          0xc300
#define GTXFIFOSIZ(n)       (0xc320 + 0x4 * (n))

#define GCTL_CORESOFTRESET  (1u << 11)
#define GCTL_DSBLCLKGTNG    (1u << 0)
#define PHYSOFTRST          (1u << 31)

#define AHB2PHY_PA          0x088ee000
#define AHB2PHY_SIZE        0x00000400

#define UD_F_CLKGT          0x1
#define UD_F_DALEPENA       0x2
#define UD_F_FULLRESET      0x4
#define UD_F_NORESET        0x8     /* do not touch the controller at all */
#define UD_F_VBUS           0x10    /* force the VBUS override before DEPCFG */
#define UD_F_PHYINIT        0x20    /* run the DT qusb-phy-init-seq first */
#define UD_F_PIPE           0x40    /* point the wrapper at the UTMI clock */
#define UD_F_RUNSTOP        0x80    /* set DCTL.RUN_STOP before endpoint 0 */
#define UD_F_RESUME         0x100   /* only clear POWER_DOWN, keep ABL config */
#define UD_F_UTMI           0x200   /* HS_PHY_CTRL: D+ pull-up, UTMI clock on */

static void
ud_fifo_dump(void)
{
	printf("SWEET USB: GHWPARAMS7 0x%08x GRXFIFOSIZ 0x%08x GTXFIFOSIZ 0x%08x 0x%08x 0x%08x\n",
	    dwc_rd(GHWPARAMS7), dwc_rd(GRXFIFOSIZ), dwc_rd(GTXFIFOSIZ(0)),
	    dwc_rd(GTXFIFOSIZ(1)), dwc_rd(GTXFIFOSIZ(2)));
}

/*
 * A QUSB2 register is one byte wide. The AHB slave drives that byte onto
 * every data lane, so a 32-bit read of 0x210 returning 0x23232323 means the
 * register holds 0x23 - which is what made the first four reads look like
 * noise. Writes stay 32-bit (the vendor init table is applied with writel())
 * and only the low lane is decoded.
 */
static uint32_t
qusb2_rd(vm_offset_t phy, uint32_t off)
{
	return usb_rd(phy, off) & 0xff;
}

static void
qusb2_wr(vm_offset_t phy, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(phy + off) = val & 0xff;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * /soc/qusb@88e2000 in the polaris DT, qcom,qusb-phy-init-seq, as
 * (offset, value) pairs. The list starts by forcing PORT_POWERDOWN to 0x23
 * and ends by writing 0x22 to it, i.e. it clears POWER_DOWN and turns the
 * PHY on. The hardware was still sitting at 0x23, so this table had never
 * been run: the PHY was configured but held powered down, which is why it
 * never produced a UTMI clock and why the controller's endpoint command
 * engine - the part that runs off that clock - never cleared CMDACT.
 *
 * 0148: the table used to say 0x220=0x00 and 0x240=0x20. Those are the
 * values from /soc/qusb@88e3000, the *second* QUSB2 PHY, whose controller
 * ssusb@a800000 is status="disabled". Our controller is ssusb@a600000 and
 * its usb-phy phandles are <181 844>, i.e. qusb@88e2000 + ssphy@88e8000.
 *
 * 0152: "every other entry is identical" was wrong, and 0148 stopped after
 * two registers instead of diffing the two tables properly. Both
 * qcom,qusb-phy-init-seq properties were read back from the live
 * /proc/device-tree in TWRP and compared entry by entry. They differ in
 * FOUR places, not two:
 *
 *     offset             88e2000   88e3000   0151 was running
 *     0x220  IMP_CTRL1     0x08      0x00          0x08      fixed in 0148
 *     0x240  TUNE1         0x07      0x20          0x07      fixed in 0148
 *     0x198  BIAS_CTRL2    0x17      0x20          0x20      fixed here
 *     0x244  TUNE2         0x2b      0x29          0x29      fixed here
 *
 * So every build up to 0151 ran the disabled sibling PHY's PLL bias and HS
 * transceiver trim. Those are the two registers that decide how the PHY
 * biases its PLL and how hard it drives the HS transceiver, i.e. D+ - the
 * one thing 0151 never explained, a PHY that reported itself ready while
 * the host saw no device at all.
 *
 * The vendor driver also folds a 3-bit efuse field (qcom,efuse-bit-pos = 25,
 * qcom,efuse-num-bits = 3) into TUNE2. That is deliberately NOT done here:
 * this build tests the DT value alone, so a change in behaviour has one
 * cause and not two.
 */
static const struct {
	uint16_t off;
	uint8_t  val;
} qusb2_init_seq[] = {
	{ 0x210, 0x23 }, { 0x004, 0x03 }, { 0x18c, 0x7c }, { 0x02c, 0x80 },
	{ 0x184, 0x0a }, { 0x0b4, 0x19 }, { 0x194, 0x40 }, { 0x198, 0x17 },
	{ 0x214, 0x21 }, { 0x220, 0x08 }, { 0x224, 0x58 }, { 0x240, 0x07 },
	{ 0x244, 0x2b }, { 0x248, 0xca }, { 0x24c, 0x04 }, { 0x250, 0x03 },
	{ 0x23c, 0x00 }, { 0x210, 0x22 },
};

#define QUSB2_PLL_STATUS    0x1a0
#define QUSB2_PORT_POWERDOWN 0x210
#define QUSB2_PORT_TUNE1    0x240
#define QUSB2_PORT_TUNE2    0x244
#define QUSB2_POWER_DOWN    (1u << 0)
/*
 * Two different ready bits, and every build up to 0151 read the wrong one.
 *
 * PLL_LOCKED = BIT(5) is the msm8996-era mask, i.e. mask_core_ready for the
 * older entries in phy-qcom-qusb2.c. This PHY declares
 * compatible = "qcom,qusb2phy-v2", and that generation uses
 * CORE_READY_STATUS = BIT(0) instead. So 0151's "PLL_STATUS 0x17 (NOT
 * locked)" said the opposite of what the v2 mask says: 0x17 has bit 0 set.
 *
 * The two readings also disagree in the one place physics can referee them.
 * POWERDOWN 0x23 (bit 0 set, PHY held down) reads 0x64, bit 0 clear.
 * Clearing POWER_DOWN to 0x22 reads 0x17, bit 0 set. A PHY that reports
 * "ready" only after it is switched on is the sane direction, so bit 0 is
 * the one that behaves. Both are printed from now on and neither is
 * asserted, because this file has been wrong about a bit before.
 */
#define QUSB2_PLL_LOCKED    (1u << 5)   /* msm8996-era mask, not this PHY */
#define QUSB2_CORE_READY    (1u << 0)   /* qusb2_v2 mask, this PHY */

static void
qusb2_pll_line(const char *tag, uint32_t pll)
{
	printf("SWEET USB: %sPLL_STATUS 0x%02x (bit0 core-ready %d, bit5 pll-locked %d)\n",
	    tag, pll, !!(pll & QUSB2_CORE_READY), !!(pll & QUSB2_PLL_LOCKED));
}


static int
ud_qusb2_init(void)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);

	if (phy == 0) {
		printf("SWEET USB: QUSB2 map failed\n");
		return -1;
	}
	printf("SWEET USB: QUSB2 in: PLL_STATUS 0x%02x POWERDOWN 0x%02x TUNE1 0x%02x REG04 0x%02x\n",
	    qusb2_rd(phy, QUSB2_PLL_STATUS), qusb2_rd(phy, QUSB2_PORT_POWERDOWN),
	    qusb2_rd(phy, QUSB2_PORT_TUNE1), qusb2_rd(phy, 0x004));

	for (unsigned i = 0; i < sizeof(qusb2_init_seq) / sizeof(qusb2_init_seq[0]); i++) {
		qusb2_wr(phy, qusb2_init_seq[i].off, qusb2_init_seq[i].val);
	}
	/* The last entry clears POWER_DOWN; the PLL needs ~150 us after that
	 * before PLL_STATUS means anything. */
	delay(200);

	uint32_t pd  = qusb2_rd(phy, QUSB2_PORT_POWERDOWN);
	uint32_t pll = qusb2_rd(phy, QUSB2_PLL_STATUS);
	printf("SWEET USB: QUSB2 out: POWERDOWN 0x%02x (%s) TUNE1 0x%02x TUNE2 0x%02x\n",
	    pd, (pd & QUSB2_POWER_DOWN) ? "still powered down" : "enabled",
	    qusb2_rd(phy, QUSB2_PORT_TUNE1), qusb2_rd(phy, QUSB2_PORT_TUNE2));
	qusb2_pll_line("QUSB2 out: ", pll);
	return (pd & QUSB2_POWER_DOWN) ? -1 : 0;
}

/*
 * ABL leaves this PHY fully configured - PLL_STATUS reads 0x64 and the
 * efuse-trimmed TUNE1 reads 0x72 - and only gates it with POWER_DOWN. The
 * vendor init table would overwrite that with its own defaults (TUNE1 0x07),
 * so try the opposite of a full init: keep ABL's configuration untouched and
 * clear nothing but the power-down bit.
 */
static int
ud_qusb2_resume(void)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);

	if (phy == 0) {
		printf("SWEET USB: QUSB2 map failed\n");
		return -1;
	}
	/*
	 * Write before reading. 0112 proved this the hard way: the first PHY
	 * access of that build was a read and the core stopped there, because
	 * a read to a block that is not answering waits instead of failing.
	 *
	 * 0x22, not 0x23. Bit 0 of this register is POWER_DOWN: writing 0x23
	 * asserted it and left the PHY powered down, which is the exact
	 * opposite of a resume and of what the comment above asks for. Every
	 * build from 0106 to 0115 did that, and it is why the endpoint command
	 * engine stalled in three of the four strategies while the one that
	 * runs the vendor init table (which ends on 0x22) configured ep0
	 * straight away. 0x22 is also the last entry of the DT init sequence
	 * and the value the working stock kernel leaves behind.
	 */
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN, 0x22);

	uint32_t pll0 = qusb2_rd(phy, QUSB2_PLL_STATUS);
	uint32_t pd0  = qusb2_rd(phy, QUSB2_PORT_POWERDOWN);
	printf("SWEET USB: QUSB2 resume in: PLL_STATUS 0x%02x POWERDOWN 0x%02x TUNE1 0x%02x\n",
	    pll0, pd0, qusb2_rd(phy, QUSB2_PORT_TUNE1));

	delay(200);

	uint32_t pd  = qusb2_rd(phy, QUSB2_PORT_POWERDOWN);
	uint32_t pll = qusb2_rd(phy, QUSB2_PLL_STATUS);
	printf("SWEET USB: QUSB2 resume out: POWERDOWN 0x%02x TUNE1 0x%02x\n",
	    pd, qusb2_rd(phy, QUSB2_PORT_TUNE1));
	qusb2_pll_line("QUSB2 resume out: ", pll);
	return 0;
}

/*
 * Hand the wrapper the high-speed PHY's UTMI clock.
 *
 * Mainline's dwc3_qcom_select_utmi_clk() is three steps, not one:
 *
 *   1 set PIPE_UTMI_CLK_DIS (BIT 8)          - stop the PIPE clock first
 *   2 sleep
 *   3 set PIPE_UTMI_CLK_SEL (BIT 0) | PIPE3_PHYSTATUS_SW (BIT 3)
 *   4 sleep
 *   5 clear PIPE_UTMI_CLK_DIS (BIT 8)
 *
 * Until 0154 this did only step 3, so PIPE3_PHYSTATUS_SW was never set and the
 * clock was never stopped and restarted. That matters here because the device
 * tree for ssusb@a600000 has no qcom,select-utmi-as-pipe-clk at all, while
 * ssphy@88e8000 exists - mainline would not take this path at all, so this is
 * the one place where the port deliberately overrides the hardware's own
 * description and has to do it exactly the way the vendor path does.
 */
static void
ud_pipe_select(void)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed (pipe)\n");
		return;
	}
	uint32_t g = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	uint32_t v;

	/* 1: stop the PIPE clock. */
	usb_wr(qs, QSCRATCH_GENERAL_CFG, g | PIPE_UTMI_CLK_DIS);
	delay(1000);
	/* 2: select UTMI as the pipe clock and take the PHY status from software. */
	v = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	usb_wr(qs, QSCRATCH_GENERAL_CFG, v | PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
	delay(1000);
	/* 3: let the PIPE clock run again. */
	v = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	usb_wr(qs, QSCRATCH_GENERAL_CFG, v & ~PIPE_UTMI_CLK_DIS);
	delay(1000);
	printf("SWEET USB: GENERAL_CFG 0x%08x -> 0x%08x (sel %d phystatus_sw %d clk_dis %d)\n",
	    g, usb_rd(qs, QSCRATCH_GENERAL_CFG),
	    !!(usb_rd(qs, QSCRATCH_GENERAL_CFG) & PIPE_UTMI_CLK_SEL),
	    !!(usb_rd(qs, QSCRATCH_GENERAL_CFG) & PIPE3_PHYSTATUS_SW),
	    !!(usb_rd(qs, QSCRATCH_GENERAL_CFG) & PIPE_UTMI_CLK_DIS));
}

/*
 * Force a real detach.
 *
 * Clearing DCTL.RUN_STOP stops the core, and a stopped core drops the D+ pull-up.
 * That matters because a host only
 * issues a bus reset when it observes a *new* connect: ABL left this port
 * enumerated for fastboot and never dropped the pull-up on hand-off, so by
 * the time this driver runs the host has a device it already gave up on and
 * has no reason to reset it. Build 0105 waited three seconds in exactly that
 * state and saw zero events.
 *
 * power_cycle is the harder version: it takes the QUSB2 PHY itself down and
 * back up, which changes the D+ line even if the core's halt does not, and
 * also clears any half-finished HS handshake left over from ABL.
 */
static void
ud_detach(const char *tag, unsigned hold_ms, int power_cycle)
{
	vm_offset_t phy;
	uint32_t d;
	int i;

	/*
	 * Stop the core: clearing RUN_STOP drops the D+ pull-up.
	 *
	 * 0148 through 0152 set the bit here instead, on the reading that
	 * "RUN_STOP = 1 means stopped". It does not: Linux sets the bit to
	 * run. So every detach in those five builds started the controller and
	 * every attach stopped it, and the four-second listening window in the
	 * bisect was spent with the wire disconnected and DevCtrlHlt reading 1
	 * in all twenty monitor samples that followed.
	 */
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	for (i = 0; i < 200; i++) {
		d = dwc_rd(DSTS);
		if (d & DSTS_DEVCTRLHLT) {
			break;
		}
		delay(1000);
	}
	d = dwc_rd(DSTS);
	printf("SWEET USB: detach %s: DCTL 0x%08x DSTS 0x%08x (b22 %d cs %d after %d ms)\n",
	    tag, dwc_rd(DCTL), d, !!(d & DSTS_DEVCTRLHLT), !!(d & 1), i);
	ud_sleep_ms(hold_ms);
	if (!power_cycle) {
		return;
	}
	phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	if (phy == 0) {
		printf("SWEET USB: detach %s: PHY map failed, halt only\n", tag);
		return;
	}
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN,
	    qusb2_rd(phy, QUSB2_PORT_POWERDOWN) | QUSB2_POWER_DOWN);
	ud_sleep_ms(300);
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN,
	    qusb2_rd(phy, QUSB2_PORT_POWERDOWN) & ~QUSB2_POWER_DOWN);
	ud_sleep_ms(200);
	printf("SWEET USB: detach %s: PHY power cycled, POWERDOWN 0x%02x PLL 0x%02x\n",
	    tag, qusb2_rd(phy, QUSB2_PORT_POWERDOWN),
	    qusb2_rd(phy, QUSB2_PLL_STATUS));
}

/*
 * Raise the pull-up and drain the event ring for up to ms milliseconds.
 * Returns 1 for DWC3_DEVICE_EVENT_RESET, 0 for other traffic, -1 for silence.
 * Every event is printed as it is read, so the log shows what the host does
 * even when it is not the reset we are waiting for.
 */
/*
 * The line-state witness.
 *
 * Runs for 90 s and prints the core's own view of the wire once a second, so
 * one unplug during boot is enough to tell the two remaining cases apart: if
 * CS follows the cable the PHY's receiver and VBUS comparator are alive and
 * only the pull-up is missing; if nothing ever moves, the PHY front end is not
 * connected to the port and no register in this driver can fix it.
 */
static void
ud_mon_thread(void *arg, wait_result_t wr)
{
	int m;

	(void)arg;
	(void)wr;
	ud_sleep_ms(3000);
	/*
	 * Ten minutes, and this time every sample is printed.
	 *
	 * Build 0155 printed only on change, so a run with no change was
	 * indistinguishable from a run where the monitor never sampled - and the
	 * one thing that run did prove (the cable can come out and go back in
	 * with nothing to show for it) needed the reader to trust that the loop
	 * really ran. Six hundred lines is 78 KB against a 4 MB ring; being able
	 * to see every sample is worth more than the space.
	 */
	printf("SWEET USB: monitor start: 600 samples, one a second, every one printed - "
	    "unplug and replug the cable at any point in that window\n");
	{
		uint32_t last_d = dwc_rd(DSTS), last_g = dwc_rd(GSTS), last_ev = 0;
		uint32_t changes = 0;

		for (m = 0; m < 600; m++) {
			uint32_t d = dwc_rd(DSTS);
			uint32_t g = dwc_rd(GSTS);
			uint32_t ev = dwc_rd(GEVNTCOUNT) & 0xffff;
			bool moved = (d != last_d || g != last_g || ev != last_ev);

			{
				/*
				 * Decoded against Linux core.h, not guessed:
				 *   DSTS[2:0]   ConnectSpd: 0 = high speed, 1 = full speed,
				 *               4 = super speed (DWC3_DSTS_HIGHSPEED is (0 << 0))
				 *   DSTS[21:18] USBLNKST
				 *   DSTS[22]    DevCtrlHlt
				 * Bit 22 reads 1 exactly when RUN_STOP has been cleared, i.e.
				 * when the core is stopped, which is what "halted" means. The
				 * two are printed side by side so the pairing is visible.
				 */
				printf("SWEET USB: mon %3d spd=%u DSTS 0x%08x (b20 %d hlt %d idle %d) "
				    "DCTL 0x%08x run %d GSTS 0x%08x (cable %d sess %d bvalid %d) "
				    "ev 0x%08x%s\n",
				    m, d & 7u, d,
				    !!(d & DSTS_BIT20), !!(d & DSTS_DEVCTRLHLT), !!(d & DSTS_BIT23),
				    dwc_rd(DCTL), !!(dwc_rd(DCTL) & DCTL_RUN_STOP),
				    g, !!(g & GSTS_BCABLE), !!(g & GSTS_BSESSVLD), !!(g & GSTS_BVALID),
				    ev, moved ? "  <- changed" : "");
				if (moved) {
					changes++;
				}
				last_d = d;
				last_g = g;
				last_ev = ev;
			}
			ud_sleep_ms(1000);
		}
		printf("SWEET USB: monitor done: %u state changes in 600 s\n", changes);
	}
	/* A kernel thread may not return; park it out of the way. */
	for (;;) {
		ud_sleep_ms(60000);
	}
}

static int
ud_attach_and_wait(const char *tag, unsigned ms)
{
	int seen = 0;
	int w;

	/*
	 * Set RUN_STOP: run the core, which is what raises the D+ pull-up.
	 *
	 * GUSB2PHYCFG.ENBLSLPM is cleared first, the way Linux's
	 * dwc3_gadget_run_stop() does it. That comment in gadget.c says the
	 * workaround was found by experiment and is not in the programming
	 * guide: with LPM enabled across a start/stop the controller can time
	 * out. This port reads 0x00002500 there, i.e. ENBLSLPM set, and this
	 * driver starts and stops the core four times in a row.
	 */
	{
		uint32_t cfg = dwc_rd(GUSB2PHYCFG);

		if (cfg & GUSB2PHYCFG_ENBLSLPM) {
			dwc_wr(GUSB2PHYCFG, cfg & ~GUSB2PHYCFG_ENBLSLPM);
			printf("SWEET USB: GUSB2PHYCFG 0x%08x -> 0x%08x (ENBLSLPM cleared)\n",
			    cfg, dwc_rd(GUSB2PHYCFG));
		}
	}
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (w = 0; w < (int)(ms / 5); w++) {
		uint32_t cnt;
		int drained = 0;

		/* The controller writes the ring behind the CPU's back. */
		ud_sync(UD_EVT, UD_EVT_SIZE);
		while ((cnt = dwc_rd(GEVNTCOUNT) & 0xffff) != 0 && drained < 16) {
			uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
			printf("SWEET USB: %s event 0x%08x at %d ms (count %u DSTS 0x%08x)\n",
			    tag, ev, w * 5, cnt, dwc_rd(DSTS));
			ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
			dwc_wr(GEVNTCOUNT, 4);
			seen++;
			drained++;
			if ((ev & 0xff) == 1) {
				printf("SWEET USB: %s BUS RESET at %d ms (DSTS 0x%08x)\n",
				    tag, w * 5, dwc_rd(DSTS));
				return 1;
			}
		}
		ud_sleep_ms(5);
	}
	printf("SWEET USB: %s nothing in %d ms (DSTS 0x%08x)\n",
	    tag, (int)ms, dwc_rd(DSTS));
	return seen ? 0 : -1;
}

/*
 * The attach bisect. Each entry changes one more thing on a halted core, then
 * ud_attach_and_wait() raises the pull-up and listens. The winner is whichever
 * one makes the host reset the bus.
 */
#define GCTL_PRTCAP_MASK   0x00003000u
#define GCTL_PRTCAP_DEVICE 0x00002000u


/*
 * Walk the GCC windows that hold the USB clock branches and switch them on.
 *
 * The offsets in usb_cbcr[] came from a different SoC and their readings are
 * not trustworthy (every one of them printed 0x00000001, which as a CBCR means
 * "halted" - and a halted master clock would make DEPCFG impossible, which it
 * is not). So stop reading individual guesses and sweep the two windows that
 * matter: 0x0f000 for the USB30 prim controller clocks and 0x8c000 for the
 * USB3 PHY reference clock generator, plus 0x6a000 for the AHB2PHY gate.
 *
 * Bit 0 is CLK_OFF in a CBCR and CLK_ENABLE in an RCG CMD register, so
 * clearing it is the enable action in both cases. Every write is printed with
 * its before and after value so a wrong offset can be spotted and undone.
 */
static void ud_live_state(const char *tag);


/*
 * The PHY, in one line: is the PLL locked, is the port powered, what is the
 * transmit tuning. Every attempt prints this before and after so the log
 * says which write cost the lock, rather than only that it went away.
 */
static void
ud_phy_state(const char *tag)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	uint32_t pll, pd, t1, t2;

	if (phy == 0) {
		printf("SWEET USB:   %-12s PHY map failed\n", tag);
		return;
	}
	pll = qusb2_rd(phy, QUSB2_PLL_STATUS);
	pd  = qusb2_rd(phy, QUSB2_PORT_POWERDOWN);
	t1  = qusb2_rd(phy, QUSB2_PORT_TUNE1);
	t2  = qusb2_rd(phy, QUSB2_PORT_TUNE2);
	printf("SWEET USB:   %-12s POWERDOWN 0x%02x TUNE1 0x%02x TUNE2 0x%02x\n",
	    tag, pd, t1, t2);
	printf("SWEET USB:   %-12s PLL_STATUS 0x%02x (bit0 core-ready %d, bit5 pll-locked %d)\n",
	    tag, pll, !!(pll & QUSB2_CORE_READY), !!(pll & QUSB2_PLL_LOCKED));
}

/*
 * Attempt A: one write, no read.
 *
 * 0x23 is the byte ABL was running this port with, and per the 0106 sweep it
 * is also the byte that keeps the PLL locked: clearing bit 0 dropped
 * PLL_STATUS from 0x64 to 0x17 and it never came back. So the whole premise
 * is "stop clearing it" and this write makes that explicit rather than
 * implicit. It is a write because a read here is what killed 0112.
 */
static void
ud_cfg_keep23(void)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);

	if (phy == 0) {
		printf("SWEET USB:   PHY map failed\n");
		return;
	}
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN, 0x23);
	printf("SWEET USB:   POWERDOWN <- 0x23 (write only)\n");
}

/*
 * Attempt B: the corrected table and nothing else.
 *
 * 0151's bisect had no such case. A wrote POWER_DOWN back to 0x23, i.e. held
 * the PHY down, and B and C both overwrote TUNE1 with 0x57 - so the table's
 * own TUNE1 (0x07) was never actually tested, and neither was the corrected
 * TUNE2. This re-runs the table from qusb@88e2000 and attaches without
 * touching anything afterwards, which makes it the clean read on whether the
 * two wrong bytes were the whole problem.
 */
static void
ud_cfg_tableonly(void)
{
	if (ud_qusb2_init() != 0) {
		printf("SWEET USB:   table left the PHY powered down\n");
	}
}

/*
 * Attempts C and D: the DT table, correctly, then the efuse TUNE1.
 *
 * 0x57 is not a guess. The stock kernel on this same board prints
 * "qusb_phy_init(): Programming TUNE1 parameter as:57" and
 * "Programming pll_bias parameter as:0", so those are the values a working
 * boot uses. The table's own TUNE1 is 0x20 and ABL runs with 0x72; the
 * sequence writes 0x57 and then, for D, the port power byte is 0x23 instead
 * of the table's 0x22.
 */
static void
ud_phy_table(uint32_t powerdown)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	unsigned i;

	if (phy == 0) {
		return;
	}
	for (i = 0; i < sizeof(qusb2_init_seq) / sizeof(qusb2_init_seq[0]); i++) {
		qusb2_wr(phy, qusb2_init_seq[i].off, qusb2_init_seq[i].val);
	}
	qusb2_wr(phy, QUSB2_PORT_TUNE1, 0x57);
	qusb2_wr(phy, 0x230, 0x00);             /* pll_bias, as the stock kernel */
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN, powerdown);
	printf("SWEET USB:   DT table + TUNE1 0x57, POWERDOWN <- 0x%02x (write only)\n",
	    powerdown);
}

static void
ud_cfg_table57(void)
{
	ud_phy_table(0x23);
}

static void
ud_cfg_table57off(void)
{
	ud_phy_table(0x22);
}

/* One line that says everything the core currently believes about the wire. */
static void
ud_live_state(const char *tag)
{
	uint32_t d = dwc_rd(DSTS);
	uint32_t g = dwc_rd(GSTS);

	printf("SWEET USB:   %-8s cs=%d spd=%u susp=%d b22=%d bsessvld=%d DSTS 0x%08x GSTS 0x%08x ev 0x%08x\n",
	    tag, !!(d & 1), (d >> 1) & 7u, !!(d & (1u << 8)), !!(d & DSTS_DEVCTRLHLT),
	    !!(g & (1u << 30)), d, g, dwc_rd(GEVNTCOUNT) & 0xffff);
}


static void
ud_ahb2phy_probe(void)
{
	vm_offset_t br = ml_io_map(AHB2PHY_PA, AHB2PHY_SIZE);

	if (br == 0) {
		printf("SWEET USB: ahb2phy map failed\n");
		return;
	}
	printf("SWEET USB: ahb2phy cfg 0x%08x 0x%08x\n", usb_rd(br, 0x0), usb_rd(br, 0x4));
}

static int
ud_core_reset(unsigned flags)
{
	/*
	 * Save the global setup first: GCTL.CORESOFTRESET (and even
	 * DCTL.CSFTRST) can drop PRTCAPDIR back to host mode and clear the
	 * USBTRDTIM/PHY setup, and Linux re-runs dwc3_phy_setup() and
	 * dwc3_core_setup_global_control() after every reset precisely for
	 * that reason.
	 */
	uint32_t gctl = dwc_rd(GCTL);
	uint32_t gusb2 = dwc_rd(GUSB2PHYCFG);
	uint32_t gusb3 = dwc_rd(GUSB3PIPECTL);

	if (flags & UD_F_FULLRESET) {
		/* Linux dwc3_core_soft_reset(): take the core and both PHYs
		 * through reset instead of only the device controller. */
		dwc_wr(GCTL, gctl | GCTL_CORESOFTRESET);
		dwc_wr(GUSB3PIPECTL, gusb3 | PHYSOFTRST);
		dwc_wr(GUSB2PHYCFG, gusb2 | PHYSOFTRST);
		delay(100000);
		dwc_wr(GUSB3PIPECTL, gusb3 & ~PHYSOFTRST);
		dwc_wr(GUSB2PHYCFG, gusb2 & ~PHYSOFTRST);
		delay(100000);
		dwc_wr(GCTL, gctl & ~GCTL_CORESOFTRESET);
		delay(100000);
	}
	/*
	 * 0149: the whole-core reset is now opt-in, and off by default.
	 *
	 * 0148 asserted GCTL.CORESOFTRESET (bit 11) before DCTL.CSFTRST,
	 * because that is what Linux's dwc3_core_soft_reset() does. The
	 * build ran and produced a kernel that dies inside the first second
	 * of boot - before the log ring's first one-second flush, which is
	 * why the ring came back with nothing new in it at all and why the
	 * phone fell straight back to fastboot with a black screen.
	 *
	 * A whole-core reset also resets the DWC3's AXI master while its
	 * transactions are still outstanding. With the SMMU in bypass and
	 * the wrapper already programmed, that can raise a bus error the
	 * SoC answers by resetting. It is the only change in 0148 that can
	 * do anything at all this early: every other edit is either a bit
	 * definition or a write to a register that was already being
	 * written in 0147, and 0147 boots.
	 *
	 * So it stays behind sweetusbfullreset=1 and the default path is
	 * the 0147 one: pulse DCTL.CSFTRST, which is the reset that clears
	 * CMDACT, and keep the other DCTL bits that ABL set instead of
	 * zeroing them.
	 */
	int full = 0;
	PE_parse_boot_argn("sweetusbfullreset", &full, sizeof(full));
	if (full) {
		int g;
		dwc_wr(GCTL, gctl | GCTL_CORESOFTRESET);
		for (g = 0; g < 100 && (dwc_rd(GCTL) & GCTL_CORESOFTRESET); g++) {
			delay(1000);
		}
		dwc_wr(GCTL, gctl);
		printf("  core reset: GCTL.CORESOFTRESET cleared after %d ms, "
		    "GCTL 0x%08x GSTS 0x%08x%s\n", g, dwc_rd(GCTL), dwc_rd(GSTS),
		    (dwc_rd(GSTS) & 1u) ? " CSRTIMEOUT" : "");
	}

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_CSFTRST);
	int i;
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		delay(1000);
	}
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		printf("  soft reset STUCK after %d ms\n", i);
		return -1;
	}
	dwc_wr(GCTL, gctl);
	dwc_wr(GUSB2PHYCFG, gusb2);
	dwc_wr(GUSB3PIPECTL, gusb3);
	printf("  dev reset: DCTL.CSFTRST cleared after %d ms, DCTL 0x%08x DSTS 0x%08x "
	    "(full %d)\n", i, dwc_rd(DCTL), dwc_rd(DSTS), full);
	return 0;
}

/*
 * Force the wrapper to report a valid VBUS session. Linux's dwc3-qcom does
 * this at probe time (dwc3_qcom_vbus_override_enable), long before any
 * endpoint is configured; this driver only did it after endpoint 0 came up,
 * which is to say never. A device that never sees VBUS never leaves the
 * disconnected state, and the core has no reason to run the endpoint
 * command engine.
 */
static void
ud_vbus_override(void)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed\n");
		return;
	}
	uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
	uint32_t ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
	printf("SWEET USB: QSCRATCH HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x GENERAL_CFG 0x%08x\n",
	    hs, ss, usb_rd(qs, 0x08));
	*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL;
	*(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL) = ss | LANE0_PWR_PRESENT;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(1000);
	printf("SWEET USB: VBUS override on: HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n",
	    usb_rd(qs, QSCRATCH_HS_PHY_CTRL), usb_rd(qs, QSCRATCH_SS_PHY_CTRL));
}

/*
 * The wrapper state a working high-speed gadget sits in.
 *
 * This used to claim that HS_PHY_CTRL 0x10000000 had "UTMI_CLK_EN clear" and
 * "SUSPEND_N clear", and that those were why the controller had no UTMI
 * clock. 0150 disproved it: those bits are not in this register on SDM845.
 * The only two bits that exist are VBUS_VALID and SW_SESSVLD_SEL, both of
 * which are already set, so the wrapper is configured correctly and has
 * nothing more to give. Whatever is keeping D+ from being driven is on the
 * PHY side, not here.
 *
 * The decode therefore reports only the two real bits. The extra names are
 * still written by ud_hsphy_probe() on purpose, as the measurement that
 * keeps proving they are read-only.
 */
#define HS_CONNECT_BITS  (UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL)
#define HS_CLOCK_BITS    0u     /* not this register on SDM845 */

static void
ud_hsphy_state(const char *tag, uint32_t hs)
{
	printf("SWEET USB: %s HS_PHY_CTRL 0x%08x (vbus_valid %d sessvld %d; "
	    "bits 13/14/21/22/23 are not this register on SDM845: %d%d%d%d%d)\n",
	    tag, hs, !!(hs & UTMI_OTG_VBUS_VALID), !!(hs & SW_SESSVLD_SEL),
	    !!(hs & VBUSVLDEXT0), !!(hs & VBUSVLDEXTSEL0), !!(hs & USB2_UTMI_CLK_EN),
	    !!(hs & USB2_SUSPEND_N), !!(hs & USB2_SUSPEND_N_SEL));
}

/*
 * The wrapper's state, read out instead of guessed at.
 *
 * HS_PHY_CTRL sits at QSCRATCH + 0x10 and the bits this file writes there were
 * inferred from phy-msm-usb, not taken from a header - and 0150 showed the
 * inference is wrong: after writing bits 13, 14, 20, 21, 22, 23 and 28, only 20
 * and 28 read back set. dp_pullup (13), utmi_clk_en (21) and suspend_n (22)
 * never took, and those three are exactly the ones that would explain a device
 * that runs but never drives D+.
 *
 * So stop guessing in two ways at once: dump the whole 256-byte window around
 * the register, and then ask the register itself which of its bits are
 * writable. A bit that will not hold is read-only status, and the control it
 * was supposed to be is somewhere else in the window.
 */
static void
ud_qscratch_dump(const char *tag)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);

	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed (%s)\n", tag);
		return;
	}
	for (unsigned off = 0; off < 0x100; off += 0x10) {
		printf("SWEET USB: qscratch %s %02x: %08x %08x %08x %08x\n", tag, off,
		    usb_rd(qs, off), usb_rd(qs, off + 4),
		    usb_rd(qs, off + 8), usb_rd(qs, off + 12));
	}
}

/*
 * One bit at a time: set it, read it back, put the original value back. The
 * register is a configuration word, so a single-bit excursion and an immediate
 * restore is the smallest question that can be asked of it.
 */
static void
ud_hsphy_probe(void)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	uint32_t orig, sticky = 0;

	if (qs == 0) {
		return;
	}
	orig = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
	for (unsigned b = 0; b < 32; b++) {
		uint32_t v;
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = orig | (1u << b);
		__asm__ volatile ("dsb sy" ::: "memory");
		v = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = orig;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (v & (1u << b)) {
			sticky |= 1u << b;
		}
	}
	printf("SWEET USB: hsphy probe orig 0x%08x writable mask 0x%08x, restored 0x%08x\n",
	    orig, sticky, usb_rd(qs, QSCRATCH_HS_PHY_CTRL));
}

static void
ud_hsphy_connect(void)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);

	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed (hsphy)\n");
		return;
	}
	uint32_t hs0 = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
	ud_hsphy_state("before", hs0);
	*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs0 | HS_CONNECT_BITS | HS_CLOCK_BITS;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(1000);
	ud_hsphy_state("after ", usb_rd(qs, QSCRATCH_HS_PHY_CTRL));
}

/*
 * The bit that was the bug.
 *
 * Proven by the build 0100 bisect, not by reasoning: with GENERAL_CFG at 0x04
 * (what ABL leaves) a real DEPCFG sat in CMDACT for the full 500 ms; with
 * GENERAL_CFG set to 0x01 the same command retired on the first poll, and so
 * did every endpoint command after it. PIPE_UTMI_CLK_SEL is bit 0 on this
 * SoC - mainline dwc3-qcom says so, and now the hardware says so too - while
 * this file had carried the msm8996 position (bit 1) and therefore wrote
 * 0x02, a bit neither generation defines. There is no cleaner example in this
 * port of a wrong constant outliving nine experiments.
 */
static void
ud_utmi_clk_select(void)
{
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	uint32_t g0;

	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed (clksel)\n");
		return;
	}
	g0 = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	*(volatile uint32_t *)(qs + QSCRATCH_GENERAL_CFG) =
	    (g0 & ~PIPE_UTMI_CLK_DIS) | PIPE_UTMI_CLK_SEL;
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(500);
	printf("SWEET USB: GENERAL_CFG 0x%08x -> 0x%08x (UTMI clock selected)\n",
	    g0, usb_rd(qs, QSCRATCH_GENERAL_CFG));
}

/*
 * Bisect the clock mux instead of guessing it a third time.
 *
 * GENERAL_CFG came up as 0x04, a bit that means PIPE_UTMI_CLK_DIS in the old
 * encoding and nothing at all in the new one, while the bit that does mean
 * PIPE_UTMI_CLK_SEL in the new encoding (bit 0) was clear. Rather than pick
 * an encoding, write each candidate, then run a real DEPSTARTCFG + DEPCFG
 * and see whether the endpoint command engine retires it. The first value
 * that works is the answer; if none works, the mux is not what is missing
 * and the next suspects are the USB30_PRIM GDSC and the PHY reference clock.
 */
static int
ud_depcfg_ok(void)
{
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0)) {
		return -1;
	}
	return ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3),
	    (1u << 8) | (1u << 10), 0);
}

static int
ud_clock_mux_bisect(void)
{
	static const struct {
		uint32_t gen;
		uint32_t hs;
		const char *name;
	} mux[] = {
		/* The known winner goes first so a regression costs nothing. */
		{ 0x01, 0,                                 "new: UTMI_CLK_SEL" },
		{ 0x04, 0,                                 "as ABL left it" },
		{ 0x02, 0,                                 "old: UTMI sel, PIPE3 off" },
		{ 0x03, 0,                                 "both select bits" },
		{ 0x01, HS_CLOCK_BITS,                     "UTMI_CLK_SEL + clk enable" },
	};
	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	int won = -1;

	if (qs == 0) {
		printf("SWEET USB: QSCRATCH map failed (mux)\n");
		return -1;
	}
	printf("SWEET USB: clock mux bisect, %u candidates\n",
	    (unsigned)(sizeof(mux) / sizeof(mux[0])));
	for (unsigned i = 0; i < sizeof(mux) / sizeof(mux[0]); i++) {
		ud_core_reset(0);
		ud_qusb2_resume();
		if (mux[i].hs) {
			uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
			*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | mux[i].hs;
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		*(volatile uint32_t *)(qs + QSCRATCH_GENERAL_CFG) = mux[i].gen;
		__asm__ volatile ("dsb sy" ::: "memory");
		delay(1000);
		if (ud_depcfg_ok() == 0) {
			printf("SWEET USB: mux 0x%02x (%s) -> endpoint 0 OK\n",
			    mux[i].gen, mux[i].name);
			won = (int)i;
			break;
		}
		printf("SWEET USB: mux 0x%02x (%s) -> still stuck (GENERAL_CFG reads 0x%08x)\n",
		    mux[i].gen, mux[i].name, usb_rd(qs, QSCRATCH_GENERAL_CFG));
	}
	if (won < 0) {
		printf("SWEET USB: no wrapper state retires DEPCFG; mux is not the fault\n");
	}
	return won;
}

/*
 * Read the whole QUSB2 window twice - once as ABL left it, once after
 * clearing POWER_DOWN - and report only the registers that moved.
 *
 * The offsets used so far (PLL_STATUS 0x1a0, TUNE1 0x240, POWERDOWN 0x210)
 * were inferred from the DT init table, not from a working reference, and
 * PLL_STATUS has never made sense: it read 0x64 (bit 5, "locked") while the
 * block was powered down and 0x17 once it was powered up. A sweep answers
 * the question without guessing. A register that changes between the two
 * samples is a live status bit; if nothing changes, the digital block is not
 * being clocked at all and the fault is upstream of the PHY.
 *
 * The original POWER_DOWN value goes back at the end, so a no-touch strategy
 * really does see the state ABL handed over.
 */
static void
ud_qusb2_sweep(void)
{
	vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);
	uint8_t off[QUSB2_SIZE / 4];
	uint32_t pd0;
	unsigned i;

	if (phy == 0) {
		printf("SWEET USB: QUSB2 map failed (sweep)\n");
		return;
	}
	printf("SWEET USB: QUSB2 sweep, powered down\n");
	for (i = 0; i < QUSB2_SIZE / 4; i++) {
		off[i] = (uint8_t)qusb2_rd(phy, i * 4);
	}
	printf("SWEET USB: QUSB2 off:");
	for (i = 0; i < QUSB2_SIZE / 4; i++) {
		if (off[i]) {
			printf(" %x=%02x", i * 4, off[i]);
		}
	}
	printf("\n");

	pd0 = qusb2_rd(phy, QUSB2_PORT_POWERDOWN);
	qusb2_wr(phy, QUSB2_PORT_POWERDOWN, pd0 & ~QUSB2_POWER_DOWN);
	delay(2000);

	printf("SWEET USB: QUSB2 on (deltas):");
	for (i = 0; i < QUSB2_SIZE / 4; i++) {
		uint8_t v = (uint8_t)qusb2_rd(phy, i * 4);
		if (v != off[i]) {
			printf(" %x:%02x>%02x", i * 4, off[i], v);
		}
	}
	printf("\n");

	qusb2_wr(phy, QUSB2_PORT_POWERDOWN, pd0);
	delay(200);
	printf("SWEET USB: QUSB2 restored POWERDOWN 0x%02x -> 0x%02x\n",
	    pd0, qusb2_rd(phy, QUSB2_PORT_POWERDOWN));
}

static int
ud_try(const char *tag, unsigned flags)
{
	printf("SWEET USB: try %s (flags 0x%x)\n", tag, flags);
	sweet_log_flush_now();
	if (flags & UD_F_NORESET) {
		/*
		 * ABL hands over a controller that was working seconds ago,
		 * halted but with its PHY and global setup intact. Every
		 * earlier attempt started with DCTL.CSFTRST and then had to
		 * put that setup back by hand; if the reset is what breaks it,
		 * no amount of re-programming helps. So leave it untouched.
		 */
		printf("  %s: no reset, DCTL 0x%08x DSTS 0x%08x GCTL 0x%08x DALEPENA 0x%08x\n",
		    tag, dwc_rd(DCTL), dwc_rd(DSTS), dwc_rd(GCTL), dwc_rd(DALEPENA));
	} else if (ud_core_reset(flags)) {
		printf("  %s: FAILED at reset\n", tag);
		return -1;
	}
	US_CHECK("core reset returned");
	if (flags & UD_F_VBUS) {
		ud_vbus_override();
	}
	if (flags & UD_F_RESUME) {
		if (ud_qusb2_resume() != 0) {
			printf("  %s: PHY resume failed\n", tag);
		}
	} else if (flags & UD_F_PHYINIT) {
		if (ud_qusb2_init() != 0) {
			printf("  %s: PHY stayed in power-down\n", tag);
		}
	}
	US_CHECK("PHY sequence returned");
	if (flags & UD_F_UTMI) {
		ud_hsphy_connect();
	}
	US_CHECK("UTMI connect returned");
	if (flags & UD_F_PIPE) {
		ud_pipe_select();
	}
	if (flags & UD_F_CLKGT) {
		dwc_wr(GCTL, dwc_rd(GCTL) | GCTL_DSBLCLKGTNG);
		printf("  %s: GCTL 0x%08x (DSBLCLKGTNG set)\n", tag, dwc_rd(GCTL));
	}
	US_CHECK("about to arm the event ring");
	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	/*
	 * Read the event buffer pointer back before trusting it. If the high
	 * half will not hold a value the core is 32-bit-addressing only, and
	 * anything above 4 GiB silently points somewhere else.
	 */
	printf("SWEET USB: GEVNTADR written 0x%08x%08x reads 0x%08x%08x SIZ 0x%08x\n",
	    (uint32_t)((ud.dma_pa + UD_EVT) >> 32), (uint32_t)(ud.dma_pa + UD_EVT),
	    dwc_rd(GEVNTADRHI), dwc_rd(GEVNTADRLO), dwc_rd(GEVNTSIZ));
	ud.evt_pos = 0;

	/*
	 * High speed, address 0, plus the two bits Linux sets in
	 * __dwc3_gadget_start() before it touches an endpoint:
	 *
	 *   DCFG.IGNSTRMPP       (bit 23) ignore the packet-pending bit when
	 *                                 selecting a stream
	 *   GRXTHRCFG.PKTCNTSEL  (bit 29) cleared, so DCFG.NUMP is used as the
	 *                                 ACK TP NumP instead of the core
	 *                                 working it out itself
	 *
	 * Neither is a databook precondition for DEPCFG, but they are part of
	 * the state a working Linux gadget has and this driver never
	 * established, and matching them costs two register writes.
	 */
	US_CHECK("event ring armed, about to write DCFG");
	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | DCFG_IGNSTRMPP);
	dwc_wr(GRXTHRCFG, dwc_rd(GRXTHRCFG) & ~GRXTHRCFG_PKTCNTSEL);
	printf("  %s: DCFG 0x%08x (DevSpd %u, addr %u) GRXTHRCFG 0x%08x "
	    "GUSB3PIPECTL 0x%08x DALEPENA 0x%08x\n",
	    tag, dwc_rd(DCFG), dwc_rd(DCFG) & 7u, (dwc_rd(DCFG) >> 3) & 0x7fu,
	    dwc_rd(GRXTHRCFG), dwc_rd(GUSB3PIPECTL), dwc_rd(DALEPENA));
	/*
	 * Device events on, but delivered only to the event ring.
	 *
	 * This driver polls, so it needs DEVTEN set or the core queues no
	 * connect/reset/setup events at all - and a polled GEVNTCOUNT then
	 * reads 0 forever no matter what the host does. What it must not do is
	 * raise an interrupt, and that is GINTMSK's job, not DEVTEN's: GINTMSK
	 * is left at its reset value of 0, so nothing ever sets the INT bit and
	 * the core's IRQ line stays low. Build 0100 ran with DEVTEN=0x7 and
	 * build 0103 with DEVTEN=0 and both died at the same statement, so
	 * DEVTEN was never the fault.
	 */
	dwc_wr(DEVTEN, 0x7);
	printf("  %s: pre-ep0 GUSB2PHYCFG 0x%08x GSTS 0x%08x DSTS 0x%08x\n",
	    tag, dwc_rd(GUSB2PHYCFG), dwc_rd(GSTS), dwc_rd(DSTS));
	sweet_log_flush_now();

	if (flags & UD_F_RUNSTOP) {
		/*
		 * Linux sets RUN_STOP only after the endpoints are configured.
		 * Some cores will not retire an endpoint command while the
		 * device controller is halted, so try it the other way round.
		 */
		dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
		delay(1000);
		printf("  %s: RUN_STOP first, DCTL 0x%08x DSTS 0x%08x\n",
		    tag, dwc_rd(DCTL), dwc_rd(DSTS));
	}

	if (flags & UD_F_DALEPENA) {
		dwc_wr(DALEPENA, 0x3);
	}
	US_CHECK("about to DEPSTARTCFG");
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0)) {
		printf("  %s: FAILED at DEPSTARTCFG\n", tag);
		return -1;
	}
	US_CHECK("DEPSTARTCFG returned");
	/*
	 * Endpoint 0 is a control endpoint, so its max packet size may only be
	 * 8, 16, 32 or 64. This diagnostic path used 512, which is not a legal
	 * DEPCFG value for that endpoint type - and DEPCFG was exactly the
	 * command that never cleared CMDACT. Linux builds ep0 from
	 * usb_endpoint_maxp() of the ep0 descriptor, i.e. 64.
	 */
	US_CHECK("about to ep0 OUT DEPCFG");
	if (ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3),
	    (1u << 8) | (1u << 10) | (0u << 25), 0)) {
		printf("  %s: FAILED at ep0 OUT DEPCFG\n", tag);
		/* Does any other command class still complete here? */
		printf("  %s: probe SETTRANSFRESOURCE -> %d\n", tag,
		    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0));
		return -1;
	}
	US_CHECK("ep0 OUT configured, about to the rest");
	if (ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3),
	    (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("  %s: FAILED at endpoint config\n", tag);
		return -1;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_fifo_dump();
	printf("  %s: endpoint 0 configured OK\n", tag);
	sweet_log_flush_now();
	return 0;
}

static void
sweet_usb_gadget_start(vm_offset_t dwc)
{
	thread_t thread;

	/*
	 * Below 4 GiB, forced.
	 *
	 * Every earlier build allocated this buffer wherever the allocator felt
	 * like and got 0x103598000, i.e. 4.35 GiB, and the only place that
	 * address is used is GEVNTADR/TRB - registers whose high halves are
	 * written on the assumption that the core has 64-bit addressing. If it
	 * does not, the event ring lives at a truncated address and the core
	 * writes events somewhere else entirely, which would explain a polled
	 * GEVNTCOUNT that only ever reads 0. Rather than trust GHWPARAMS0, make
	 * the address small and take the question away. Retry because the
	 * allocator hands back whatever it finds first.
	 */
	ud.dwc = dwc;
	ppnum_t pn = 0;
	uint64_t last_pa = 0;
	int tries = 0;
	/*
	 * No KMA_PERMANENT here. The first version of this loop kept the flag
	 * and then freed the rejected buffer, and vm_map_delete panicked on it:
	 * "Attempting to remove permanent VM map entry" (vm_map.c:7940). A
	 * permanent entry cannot be unmapped by design, so the retry needs an
	 * ordinary one. Kernel object memory is never paged out, so nothing is
	 * lost by dropping the flag.
	 */
	for (; tries < 8; tries++) {
		if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
		    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("SWEET USB: no DMA memory\n");
			return;
		}
		pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
		if (!pn) {
			return;
		}
		last_pa = ptoa_64(pn);
		printf("SWEET USB: DMA try %d -> 0x%llx (%s 4GiB)\n", tries,
		    last_pa, last_pa >= 0x100000000ULL ? ">=" : "<");
		if (last_pa < 0x100000000ULL) {
			break;
		}
		kmem_free(kernel_map, ud.dma, UD_SIZE);
		ud.dma = 0;
	}
	if (ud.dma == 0) {
		/*
		 * The last attempt was freed as well; take one more and use it
		 * wherever it lands rather than leaving USB unarmed. The event
		 * pointer is then above 4 GiB, which is exactly the case this
		 * loop was trying to avoid - but the readback of GEVNTADR now
		 * says whether the core can hold it.
		 */
		printf("SWEET USB: no DMA memory below 4GiB, falling back\n");
		if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
		    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("SWEET USB: no DMA memory at all\n");
			return;
		}
		pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
		if (!pn) {
			return;
		}
	}
	ud.dma_pa = ptoa_64(pn);
	printf("SWEET USB: DMA buffer pa 0x%llx (%s 4GiB) UD_SIZE 0x%x UD_EVT 0x%x\n",
	    ud.dma_pa, ud.dma_pa >= 0x100000000ULL ? ">=" : "<", (unsigned)UD_SIZE, (unsigned)UD_EVT);
	printf("SWEET USB: pre-config GHWPARAMS0 0x%08x GCTL 0x%08x GUSB2PHYCFG 0x%08x DCFG 0x%08x DCTL 0x%08x DSTS 0x%08x\n",
	    dwc_rd(0xc140), dwc_rd(GCTL), dwc_rd(0xc200), dwc_rd(DCFG), dwc_rd(DCTL), dwc_rd(DSTS));
	ud_if_create();

	ud_fifo_dump();
	ud_ahb2phy_probe();
	/*
	 * 0112: the PHY is left alone here on purpose.
	 *
	 * This used to call ud_qusb2_sweep() (a full power cycle) and then
	 * ud_clock_mux_bisect() (five core resets, each with another
	 * ud_qusb2_resume() in front of it). By the time the first attach was
	 * tried, the locked PLL that ABL handed over had already been dropped
	 * four times over - which is why every build since 0106 has been
	 * measuring a dead PHY and blaming the wire.
	 *
	 * Both stay in the source, behind a boot arg, because they are the
	 * only path that ever made the endpoint command engine report
	 * "ok polls 0" and they may be needed again once the PHY survives.
	 */
	int phy_touch = 0;
	PE_parse_boot_argn("sweetphytouch", &phy_touch, sizeof(phy_touch));
	if (phy_touch) {
		ud_qusb2_sweep();
		ud_utmi_clk_select();
		if (ud_clock_mux_bisect() < 0) {
			printf("SWEET USB: clock mux bisect found nothing; hoping the direct write held\n");
		}
	} else {
		/*
		 * Only the wrapper's clock mux is written: that is a QSCRATCH
		 * register, not a PHY register, and build 0100 proved the
		 * endpoint engine needs it. It cannot disturb the PLL.
		 */
		ud_utmi_clk_select();
		printf("SWEET USB: sweep and mux bisect skipped (sweetphytouch=0)\n");
	}

	/*
	 * The order is no longer a guess. Build 0100 proved that GENERAL_CFG
	 * bit 0 is what the endpoint command engine was missing, and the first
	 * entry below is the no-reset path that then configured all four
	 * endpoints on the first poll. The rest stay as regression fallbacks.
	 */
	/*
	 * 0148: noreset first, and for a different reason than before.
	 *
	 * With CMDACT finally on the right bit, this build can see for the
	 * first time what the command engine actually answers. ABL hands over
	 * a controller that was enumerating seconds ago, so the untouched
	 * state is the best starting point and, if DEPSTARTCFG comes back
	 * with a non-zero CmdStatus instead of hanging, the log will say so.
	 *
	 * table+utmi is second: it is the only strategy that ever configured
	 * endpoint 0 (build 0145), and now runs the init sequence from the
	 * PHY this controller is actually wired to. resume+utmi follows, and
	 * runstop+table+utmi last, in case the core wants to be halted while
	 * its endpoints are programmed.
	 */
	static const char *names[] = {
		"noreset", "table+utmi", "resume+utmi", "runstop+table+utmi"
	};
	static const unsigned order[] = {
		UD_F_NORESET,
		UD_F_PHYINIT | UD_F_UTMI | UD_F_CLKGT,
		UD_F_RESUME | UD_F_UTMI | UD_F_CLKGT,
		UD_F_PHYINIT | UD_F_UTMI | UD_F_CLKGT | UD_F_RUNSTOP,
	};
	int won = -1;
	for (unsigned s = 0; s < sizeof(order) / sizeof(order[0]); s++) {
		if (s > 0 && (order[s] & UD_F_NORESET)) {
			/*
			 * A failed attempt leaves DEPCMD.CMDACT set, and the core
			 * accepts no new command until that clears; only the
			 * device soft reset clears it. So unstick the command
			 * engine, then leave the rest of the controller alone.
			 */
			ud_core_reset(0);
		}
		if (ud_try(names[s], order[s]) == 0) {
			won = (int)s;
			break;
		}
	}
	if (won < 0) {
		printf("SWEET USB: endpoint 0 setup failed (all strategies)\n");
		return;
	}
	/*
	 * Six checkpoints, in Linux's order: endpoints, DALEPENA, RUN_STOP,
	 * and only then the two writes that let the host see us. The previous
	 * build printed "endpoint 0 configured OK" and then nothing, with two
	 * statements in between and no way to tell which of them it died in.
	 */
	/*
	 * DALEPENA only latches for endpoints the core actually configured, so
	 * reading it back is an independent verdict on whether the endpoint
	 * commands did anything - the one thing the old CMDACT poll could not
	 * tell us. If ep0 configured, bits 0 and 1 must be set.
	 */
	printf("SWEET USB: step 1/6 DALEPENA (was 0x%08x)\n", dwc_rd(DALEPENA));
	dwc_wr(DALEPENA, 0x3);
	delay(1000);
	printf("SWEET USB: step 1/6 DALEPENA now 0x%08x (%s)\n", dwc_rd(DALEPENA),
	    (dwc_rd(DALEPENA) & 0x3) == 0x3 ? "ep0 enabled: the commands took effect"
	    : "ep0 NOT enabled: the endpoint commands did nothing");

	/*
	 * Park the core halted, then run the attach bisect.
	 *
	 * Builds 0105 and 0106 established two things by measurement: the event
	 * ring stays empty because the host never resets us, and the host never
	 * resets us because it never sees a connect - the phone vanishes from the
	 * PC's USB tree at reboot and no device ever appears again. So the pull-up
	 * is not being driven. Build 0107 then ruled out both register suspects
	 * by writing them back: GCTL 0x00102000 already reads back as
	 * PrtCapDir = 2 (device) and DCFG as DevSpd = 0 (high speed), so ABL
	 * handed over a correct core and the fault is on the analog side.
	 *
	 * Each attempt: detach, apply one more change, attach, listen 4 s. The one
	 * that produces a bus reset is the answer, and the log names it.
	 */
	printf("SWEET USB: step 2/6 detach (drop the D+ pull-up)\n");
	ud_detach("initial", 600, 0);

	printf("SWEET USB: step 3/6 arm the event ring\n");
	ud_sync(UD_EVT, UD_EVT_SIZE);
	ud.evt_pos = 0;
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	printf("SWEET USB: step 3/6 GEVNTCOUNT 0x%08x GEVNTSIZ 0x%08x DEVTEN 0x%08x\n",
	    dwc_rd(GEVNTCOUNT) & 0xffff, dwc_rd(GEVNTSIZ), dwc_rd(DEVTEN));

	/*
	 * Measure the wrapper before anything is attached, and do the one PHY
	 * read here rather than at the very end.
	 *
	 * In 0150 the core was running (DSTS.DevCtrlHlt 0) throughout the attach
	 * window and halted (1) in every one of the nineteen monitor samples
	 * that followed. Nothing in this driver writes DCTL between the two, and
	 * the only thing that happens in between is ud_phy_state("final") - so
	 * that read is the prime suspect for parking the core, and leaving it
	 * where it was makes the monitor window useless. It still has to run
	 * once, because the PHY is the block this kernel has stalled on before;
	 * it now runs before the bisect, where its cost is bounded.
	 */
	ud_qscratch_dump("pre");
	ud_hsphy_probe();
	ud_phy_state("before bisect");

	printf("SWEET USB: step 4/6 attach bisect\n");
	static const struct {
		const char *name;
		void (*apply)(void);
	} cfg[] = {
		{ "A-keep-0x23",    ud_cfg_keep23 },
		{ "B-table-only",   ud_cfg_tableonly },
		{ "C-table-57-23",  ud_cfg_table57 },
		{ "D-table-57-22",  ud_cfg_table57off },
	};
	int saw_event = 0;
	for (unsigned k = 0; k < sizeof(cfg) / sizeof(cfg[0]) && !saw_event; k++) {
		ud_detach(cfg[k].name, 600, 0);
		printf("SWEET USB: attempt %u/%u %s\n", k + 1,
		    (unsigned)(sizeof(cfg) / sizeof(cfg[0])), cfg[k].name);
		cfg[k].apply();
		printf("SWEET USB:   attach with GCTL 0x%08x DCFG 0x%08x GUSB2PHYCFG 0x%08x DSTS 0x%08x\n",
		    dwc_rd(GCTL), dwc_rd(DCFG), dwc_rd(GUSB2PHYCFG), dwc_rd(DSTS));
		if (ud_attach_and_wait(cfg[k].name, 4000) > 0) {
			saw_event = 1;
			printf("SWEET USB: attempt %u %s WON\n", k + 1, cfg[k].name);
		}
	}

	/*
	 * Build 0155 settled one thing and it is worth writing down: unplugging
	 * the cable changed nothing - not DSTS, not GSTS, not the event count.
	 * So the port is electrically dead from the controller's side, and the
	 * controller side is not what is wrong (endpoints configured, DALEPENA
	 * set, RUN_STOP set, core not halted).
	 *
	 * What is left is the PHY's own termination. In device mode the core
	 * asks the PHY for the D+ pull-up; QUSB2 decides whether it presents it,
	 * and PORT_POWERDOWN is the register that says so. The boot loader left
	 * 0x23 and this driver has been writing 0x22; neither connects, and
	 * nobody has tried anything else. Sweep the plausible values and wait
	 * for an event after each.
	 */
	if (!saw_event) {
		static const uint32_t pd[] = { 0x23u, 0x22u, 0x20u, 0x02u, 0x00u };
		vm_offset_t phy = ml_io_map(QUSB2_PA, QUSB2_SIZE);

		printf("SWEET USB: step 4b/6 PORT_POWERDOWN sweep\n");
		if (phy == 0) {
			printf("SWEET USB:   QUSB2 map failed\n");
		}
		for (unsigned k = 0; phy != 0 && k < sizeof(pd) / sizeof(pd[0]) && !saw_event; k++) {
			ud_detach("pd-sweep", 300, 0);
			usb_wr(phy, QUSB2_PORT_POWERDOWN, pd[k]);
			ud_sleep_ms(100);
			printf("SWEET USB:   POWERDOWN <- %#04x, PLL_STATUS 0x%02x\n", pd[k],
			    usb_rd(phy, QUSB2_PLL_STATUS));
			if (ud_attach_and_wait("pd-sweep", 2000) > 0) {
				saw_event = 1;
				printf("SWEET USB:   POWERDOWN %#04x WON\n", pd[k]);
			}
		}
		if (phy != 0) {
			ml_io_unmap(phy, QUSB2_SIZE);
		}
	}

	ud_qscratch_dump("post");
	printf("SWEET USB: step 5/6 %s (DSTS 0x%08x GSTS 0x%08x)\n",
	    saw_event ? "bus reset seen, arming ep0 SETUP" : "host never reset the bus",
	    dwc_rd(DSTS), dwc_rd(GSTS));
	if (saw_event) {
		ud_start_setup();
	}

	/*
	 * Leave the core running (pull-up up) and watch the wire for 90 s.
	 *
	 * This line is what made every monitor window since 0150 useless. It
	 * cleared RUN_STOP, which stops the controller, and it runs immediately
	 * before the monitor thread starts - so all twenty samples in 0150,
	 * 0151 and 0152 were taken on a stopped core (DevCtrlHlt 1, GEVNTCOUNT
	 * 0) and read as "the PHY never drove D+". Setting the bit is what
	 * keeps the pull-up up.
	 */
	printf("SWEET USB: leaving the pull-up up, monitor thread starts below\n");
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);

	printf("SWEET USB: step 6/6 threads, DMA at 0x%llx\n", ud.dma_pa);
	if (kernel_thread_start(ud_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	int livelog = 1;
	PE_parse_boot_argn("sweetusblog", &livelog, sizeof(livelog));
	if (livelog && ud.ifp != NULL && kernel_thread_start(ud_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	printf("SWEET USB: gadget armed, DSTS 0x%08x GSTS 0x%08x\n",
	    dwc_rd(DSTS), dwc_rd(GSTS));
	if (kernel_thread_start(ud_mon_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}

	/*
	 * The PHY read that used to be here now runs before the bisect, where
	 * it cannot park the core in the middle of the observation window.
	 */
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
