/*
 * Polaris A630 bring-up gate, plus a switch-on ladder for the half of the
 * block that is not the 3D core.
 *
 * The gate came first and still decides what happens by default: nothing is
 * touched, no command buffer is ever handed to the GPU, and the CPU
 * framebuffer is never replaced. The ladder runs only under
 * `polarisgpu=1 polarisgpuup=N`, one step at a time, and it stops before the
 * 3D core: no GMU firmware, no rings; the GPU's SMMU is only read, never
 * programmed.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

extern bool polaris_touch_i2c_board(void);

enum polaris_gpu_stage {
	POLARIS_GPU_BOARD,
	POLARIS_GPU_FIRMWARE,
	POLARIS_GPU_RAILS,
	POLARIS_GPU_CLOCKS,
	POLARIS_GPU_BUS,
	POLARIS_GPU_GMU,
	POLARIS_GPU_HFI,
	POLARIS_GPU_SMMU,
	POLARIS_GPU_PAS,
	POLARIS_GPU_CP,
	POLARIS_GPU_FENCE,
	POLARIS_GPU_STAGE_COUNT
};

enum polaris_gpu_state {
	POLARIS_GPU_OFF,
	POLARIS_GPU_STARTING,
	POLARIS_GPU_IDLE,
	POLARIS_GPU_QUARANTINED
};

/* Ownership is distinct from observing another owner's PWR_ON bit. */
struct polaris_gpu_context {
	enum polaris_gpu_state state;
	unsigned int completed;
	unsigned int owned;
};

static struct polaris_gpu_context polaris_gpu = { POLARIS_GPU_OFF, 0, 0 };

static const char *const polaris_gpu_stage_name[POLARIS_GPU_STAGE_COUNT] = {
	"board", "firmware", "rails", "clocks", "bus", "GMU",
	"HFI", "SMMU", "PAS13", "CP", "fence"
};

/*
 * Only an acquired resource may be released. Resource-specific release
 * callbacks must be installed before ANY power or mapping code is enabled.
 * No stages after the board stage have a backend in this revision.
 */
static bool
polaris_gpu_enter(struct polaris_gpu_context *ctx, enum polaris_gpu_stage stage)
{
	if (ctx->state != POLARIS_GPU_STARTING ||
	    stage != (enum polaris_gpu_stage)ctx->completed ||
	    stage >= POLARIS_GPU_STAGE_COUNT) {
		return false;
	}
	if (stage != POLARIS_GPU_BOARD) {
		printf("POLARIS GPU: %s backend not implemented; no hardware access\n",
		    polaris_gpu_stage_name[stage]);
		return false;
	}
	/* Board identity is checked again here, not only at the caller. */
	if (!polaris_touch_i2c_board()) {
		return false;
	}
	ctx->completed++;
	/* The board predicate acquires nothing; owned remains zero. */
	return true;
}

static void
polaris_gpu_unwind(struct polaris_gpu_context *ctx, bool idle_confirmed)
{
	/* Never collapse rails or unmap buffers while the GMU/CP may be busy. */
	if (!idle_confirmed && ctx->completed > POLARIS_GPU_GMU) {
		ctx->state = POLARIS_GPU_QUARANTINED;
		printf("POLARIS GPU: idle unconfirmed; quarantined, CPU display unchanged\n");
		return;
	}
	/* No hardware release is permitted while no backend owns a resource. */
	if (ctx->owned != 0) {
		ctx->state = POLARIS_GPU_QUARANTINED;
		printf("POLARIS GPU: owned resource lacks release backend; quarantined\n");
		return;
	}
	ctx->completed = 0;
	ctx->state = POLARIS_GPU_OFF;
}

#include <mach/mach_types.h>
#include <vm/vm_kern.h>

#include "polaris_a630_fw.h"
#include "polaris_a630_sqe.h"
#include "polaris_a630_zap.h"

extern uint64_t kvtophys(vm_offset_t va);

/*
 * Put the console on the disk before the next thing that might stop the machine.
 *
 * The ring is flushed by a thread every sweet_log_secs seconds, which is far too
 * slow for a ladder whose thirteen steps take microseconds: a step that hangs the
 * whole SoC leaves nothing on the disk, and the boot that produced no log cannot
 * be told apart from a boot that never happened. A flush costs one polled UFS
 * write, so the ladder can afford to do it before every step and around every
 * register write in the SMMU's translation path.
 */
extern void sweet_log_flush_now(void);

#define PG_CHECK(what)	do { \
	printf("POLARIS GPU: -- at %s\n", (what)); \
	sweet_log_flush_now(); \
} while (0)


/* ---- register access ------------------------------------------------------------------------------ */

/*
 * ldr/str, not a volatile C loop: clang turns a read loop into post-indexed
 * loads, and the boot loader's trap for MMIO need not emulate those.
 */
static inline uint32_t
pg_rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static inline void
pg_wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* delay() counts microseconds. */
static void
pg_sleep_us(uint32_t us)
{
	delay(us);
}

/* ---- 1. the global clock controller ---------------------------------------------------------------- */

/* The USB driver already maps this window at this base and reads it; same numbers. */
#define PG_GCC_PHYS		0x00100000UL
#define PG_GCC_SIZE		0x00090000UL
#define PG_CBCR_OFF		(1u << 31)	/* the branch is halted */
#define PG_CBCR_ENABLE		(1u << 0)
#define PG_GCC_APPS_VOTE_0	0x52004		/* bit 15: GPLL0 to the GPU, bit 16: its /2 */
#define PG_GCC_GPU_CFG_AHB	0x71004		/* the GPU's register bus */
#define PG_GCC_GPU_MEMNOC	0x7100c
#define PG_GCC_GPU_SNOC_DVM	0x71018

static vm_offset_t pg_gcc;
static bool pg_ahb_on;

static const struct {
	const char *name;
	uint32_t reg;
} pg_gcc_branches[] = {
	{ "gcc_gpu_cfg_ahb_clk",	PG_GCC_GPU_CFG_AHB },
	{ "gcc_gpu_memnoc_gfx_clk",	PG_GCC_GPU_MEMNOC },
	{ "gcc_gpu_snoc_dvm_gfx_clk",	PG_GCC_GPU_SNOC_DVM },
	{ "gcc_gpu_iref_clk",		0x8c010 },
	{ "gcc_gpu_vs_clk",		0x7a04c },
	{ "gcc_ddrss_gpu_axi_clk",	0x44038 },
};

/*
 * GCC never loses power here, so reading it is safe whatever the GPU is
 * doing. That is why this step cannot fail: it reports, and it makes one
 * attempt at the register bus rather than giving up on it.
 */
static bool
pg_step_gcc(void)
{
	uint32_t votes;

	pg_gcc = ml_io_map(PG_GCC_PHYS, PG_GCC_SIZE);
	if (pg_gcc == 0) {
		printf("POLARIS GPU: 1 GCC map failed\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(pg_gcc_branches) / sizeof(pg_gcc_branches[0]); i++) {
		uint32_t v = pg_rd(pg_gcc + pg_gcc_branches[i].reg);

		printf("POLARIS GPU: 1 %-24s %#07x = %#010x %s\n", pg_gcc_branches[i].name,
		    pg_gcc_branches[i].reg, v, (v & PG_CBCR_OFF) ? "OFF" : "on");
		if (pg_gcc_branches[i].reg == PG_GCC_GPU_CFG_AHB) {
			pg_ahb_on = !(v & PG_CBCR_OFF);
		}
	}
	votes = pg_rd(pg_gcc + PG_GCC_APPS_VOTE_0);
	printf("POLARIS GPU: 1 apps votes %#x = %#010x: GPLL0 %s, /2 %s\n",
	    PG_GCC_APPS_VOTE_0, votes, (votes & (1u << 15)) ? "voted" : "not voted",
	    (votes & (1u << 16)) ? "voted" : "not voted");

	/*
	 * The boot loader is not obliged to leave the GPU's register bus
	 * running. Nothing is being read across it yet, so the branch is simply
	 * switched on here, the same way the clock driver would, and then
	 * asked whether it really runs.
	 */
	if (!pg_ahb_on) {
		printf("POLARIS GPU: 1 the register bus is OFF; switching it on\n");
		pg_wr(pg_gcc + PG_GCC_GPU_CFG_AHB,
		    pg_rd(pg_gcc + PG_GCC_GPU_CFG_AHB) | PG_CBCR_ENABLE);
		pg_sleep_us(10);
		pg_ahb_on = !(pg_rd(pg_gcc + PG_GCC_GPU_CFG_AHB) & PG_CBCR_OFF);
		printf("POLARIS GPU: 1 the register bus is now %#010x: %s\n",
		    pg_rd(pg_gcc + PG_GCC_GPU_CFG_AHB),
		    pg_ahb_on ? "on" : "STILL OFF");
	}
	printf("POLARIS GPU: 1 the register bus is %s\n",
	    pg_ahb_on ? "on, GPUCC may be read" : "OFF, GPUCC will not be touched");
	return true;
}

/* ---- 2. the GPU's own clock controller ------------------------------------------------------------ */

#define PG_GPUCC_PHYS		0x05090000UL
#define PG_GPUCC_SIZE		0x9000UL
#define PG_GPUCC_GX_GDSCR	0x100c
#define PG_GPUCC_CX_GDSCR	0x106c
#define PG_GPUCC_CX_HWCTRL	0x1540	/* gpu_cx_gdsc's status lives here, bit 31 */
#define PG_GPUCC_CXO_CBCR	0x109c
#define PG_GPUCC_CX_GMU_CBCR	0x1098
#define PG_GPUCC_CX_APB_CBCR	0x1088	/* gpu_cc_cx_apb_clk: the CX half's APB port */
#define PG_GPUCC_CX_SNOC_DVM_CBCR 0x108c
#define PG_GPUCC_GMU_RCG_CMD	0x1120
#define PG_GPUCC_GMU_RCG_CFG	0x1124
#define PG_GDSC_PWR_ON		(1u << 31)

static vm_offset_t pg_gpucc;

static bool
pg_step_gpucc(void)
{
	static const struct {
		const char *name;
		uint32_t reg;
	} regs[] = {
		{ "GX GDSCR (gpu_gx_gdsc)",	PG_GPUCC_GX_GDSCR },
		{ "CX GDSCR (gpu_cx_gdsc)",	PG_GPUCC_CX_GDSCR },
		{ "gpu_cc_cx_gmu_clk",		PG_GPUCC_CX_GMU_CBCR },
		{ "gpu_cc_cxo_clk",		PG_GPUCC_CXO_CBCR },
		{ "GMU clock src CMD",		PG_GPUCC_GMU_RCG_CMD },
		{ "GMU clock src CFG",		PG_GPUCC_GMU_RCG_CFG },
		{ "CX hardware control",	PG_GPUCC_CX_HWCTRL },
	};

	if (!pg_ahb_on) {
		printf("POLARIS GPU: 2 gcc_gpu_cfg_ahb_clk is off; GPUCC not read\n");
		return false;
	}
	pg_gpucc = ml_io_map(PG_GPUCC_PHYS, PG_GPUCC_SIZE);
	if (pg_gpucc == 0) {
		printf("POLARIS GPU: 2 GPUCC map failed\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint32_t v = pg_rd(pg_gpucc + regs[i].reg);
		bool gdsc = regs[i].reg == PG_GPUCC_GX_GDSCR ||
		    regs[i].reg == PG_GPUCC_CX_GDSCR;

		printf("POLARIS GPU: 2 %-22s %#06x = %#010x%s\n", regs[i].name, regs[i].reg, v,
		    !gdsc ? "" : ((v & PG_GDSC_PWR_ON) ? " power ON" : " power off"));
	}
	return true;
}

/* ---- 3. the CX power switch ------------------------------------------------------------------------ */

#define PG_GDSCR_SW_COLLAPSE		(1u << 0)
#define PG_GDSCR_CLK_DIS_WAIT_MASK	(0xfu << 12)
#define PG_GDSCR_CLK_DIS_WAIT(x)	((uint32_t)(x) << 12)
#define PG_CX_CLK_DIS_WAIT_VAL		8	/* the device tree's qcom,clk-dis-wait-val */
#define PG_GDSC_POLL_US			2000

static bool pg_cx_on;
static bool pg_memnoc_on;
static bool pg_gmu_clk_on;

static bool
pg_cx_is_on(void)
{
	/*
	 * Two places can report the state of this switch: the hardware-control
	 * register at 0x1540, and the domain's own GDSCR. The device tree gives
	 * hw-ctrl-addr = 0x1a and the Mi 8 port reads 0x1540; rather than bet
	 * on one, accept whichever reports the switch closed.
	 */
	bool hwctrl = !!(pg_rd(pg_gpucc + PG_GPUCC_CX_HWCTRL) & PG_GDSC_PWR_ON);
	bool gdscr = !!(pg_rd(pg_gpucc + PG_GPUCC_CX_GDSCR) & PG_GDSC_PWR_ON);

	return hwctrl || gdscr;
}

static bool
pg_cx_wait(bool on)
{
	for (uint32_t waited = 0; waited <= PG_GDSC_POLL_US; waited += 10) {
		if (pg_cx_is_on() == on) {
			return true;
		}
		pg_sleep_us(10);
	}
	return false;
}

static bool
pg_step_cx_on(void)
{
	uint32_t v;

	if (pg_gpucc == 0) {
		printf("POLARIS GPU: 3 GPUCC was not read; the CX switch is not touched\n");
		return false;
	}
	v = pg_rd(pg_gpucc + PG_GPUCC_CX_GDSCR);
	printf("POLARIS GPU: 3 CX GDSCR before %#010x, control %#010x\n", v,
	    pg_rd(pg_gpucc + PG_GPUCC_CX_HWCTRL));
	if (pg_cx_is_on()) {
		printf("POLARIS GPU: 3 the CX switch is already on: left alone\n");
		pg_cx_on = true;
		return true;
	}
	/* gdsc_init() then gdsc_toggle_logic(ON): the delay field first, then the switch. */
	pg_wr(pg_gpucc + PG_GPUCC_CX_GDSCR,
	    (v & ~PG_GDSCR_CLK_DIS_WAIT_MASK) | PG_GDSCR_CLK_DIS_WAIT(PG_CX_CLK_DIS_WAIT_VAL));
	v = pg_rd(pg_gpucc + PG_GPUCC_CX_GDSCR);
	pg_wr(pg_gpucc + PG_GPUCC_CX_GDSCR, v & ~PG_GDSCR_SW_COLLAPSE);
	pg_sleep_us(1);		/* about 8 XO cycles before the status bit updates */
	pg_cx_on = pg_cx_wait(true);
	printf("POLARIS GPU: 3 CX GDSCR after %#010x, control %#010x: %s\n",
	    pg_rd(pg_gpucc + PG_GPUCC_CX_GDSCR), pg_rd(pg_gpucc + PG_GPUCC_CX_HWCTRL),
	    pg_cx_on ? "ON" : "STILL OFF after 2 ms");
	return pg_cx_on;
}

/* ---- 4. the clocks of the CX half ------------------------------------------------------------------ */

#define PG_RCG_CMD_UPDATE	(1u << 0)
#define PG_RCG_CMD_ROOT_EN	(1u << 1)
#define PG_RCG_CMD_ROOT_OFF	(1u << 31)
#define PG_CX_GMU_CBCR_WS	((0xfu << 8) | (0xfu << 4))

static bool
pg_branch_enable(vm_offset_t base, uint32_t reg, const char *name)
{
	pg_wr(base + reg, pg_rd(base + reg) | PG_CBCR_ENABLE);
	for (uint32_t waited = 0; waited <= 1000; waited += 5) {
		if (!(pg_rd(base + reg) & PG_CBCR_OFF)) {
			printf("POLARIS GPU: 4 %s on: %#010x\n", name, pg_rd(base + reg));
			return true;
		}
		pg_sleep_us(5);
	}
	printf("POLARIS GPU: 4 %s does not come on: %#010x\n", name, pg_rd(base + reg));
	return false;
}

static bool
pg_step_clocks(void)
{
	uint32_t v;
	bool done = false;

	if (!pg_cx_on) {
		printf("POLARIS GPU: 4 the CX switch is off; clocks not touched\n");
		return false;
	}
	pg_memnoc_on = pg_branch_enable(pg_gcc, PG_GCC_GPU_MEMNOC, "gcc_gpu_memnoc_gfx_clk");
	if (!pg_memnoc_on ||
	    !pg_branch_enable(pg_gcc, PG_GCC_GPU_SNOC_DVM, "gcc_gpu_snoc_dvm_gfx_clk")) {
		return false;
	}
	/* The GMU's clock source: crystal, undivided (parent 0, divider 0), then the update bit. */
	v = pg_rd(pg_gpucc + PG_GPUCC_GMU_RCG_CFG);
	pg_wr(pg_gpucc + PG_GPUCC_GMU_RCG_CFG, v & ~((7u << 8) | 0x1fu));
	pg_wr(pg_gpucc + PG_GPUCC_GMU_RCG_CMD,
	    pg_rd(pg_gpucc + PG_GPUCC_GMU_RCG_CMD) | PG_RCG_CMD_ROOT_EN | PG_RCG_CMD_UPDATE);
	for (uint32_t waited = 0; waited <= 1000 && !done; waited += 5) {
		v = pg_rd(pg_gpucc + PG_GPUCC_GMU_RCG_CMD);
		done = !(v & PG_RCG_CMD_UPDATE) && !(v & PG_RCG_CMD_ROOT_OFF);
		if (!done) {
			pg_sleep_us(5);
		}
	}
	printf("POLARIS GPU: 4 GMU clock src CMD %#010x CFG %#010x: %s\n",
	    pg_rd(pg_gpucc + PG_GPUCC_GMU_RCG_CMD), pg_rd(pg_gpucc + PG_GPUCC_GMU_RCG_CFG),
	    done ? "updated, root on" : "NOT UPDATED");
	if (!done) {
		return false;
	}
	pg_wr(pg_gpucc + PG_GPUCC_CX_GMU_CBCR,
	    (pg_rd(pg_gpucc + PG_GPUCC_CX_GMU_CBCR) & ~PG_CX_GMU_CBCR_WS) | PG_CX_GMU_CBCR_WS);
	/*
	 * The two branches the CX half's register port runs on. The vendor driver
	 * enables them with the rest of the SMMU's clocks, and without them a
	 * write into the SMMU can be dropped while the read still answers from the
	 * flops - which is exactly what log 0131 showed (SMR took, S2CR kept its
	 * type, sCR0 ignored everything). Not fatal: the report is what matters.
	 */
	(void)pg_branch_enable(pg_gpucc, PG_GPUCC_CX_APB_CBCR, "gpu_cc_cx_apb_clk");
	(void)pg_branch_enable(pg_gpucc, PG_GPUCC_CX_SNOC_DVM_CBCR, "gpu_cc_cx_snoc_dvm_clk");
	return pg_branch_enable(pg_gpucc, PG_GPUCC_CXO_CBCR, "gpu_cc_cxo_clk") &&
	    (pg_gmu_clk_on = pg_branch_enable(pg_gpucc, PG_GPUCC_CX_GMU_CBCR, "gpu_cc_cx_gmu_clk"));
}

/* ---- 5. the GMU's registers ------------------------------------------------------------------------ */

#define PG_GMU_PHYS		0x0506a000UL
#define PG_GMU_SIZE		0x30000UL
/* Word numbers, not byte offsets: the offsets in Linux's a6xx_gmu.xml are 4 bytes apart. */
#define PG_GMU_WORD(w)		((uint32_t)(w) * 4u)

static vm_offset_t pg_gmu;

static bool
pg_step_gmu(void)
{
	static const struct {
		const char *name;
		uint32_t word;
	} regs[] = {
		{ "GMU_CM3_SYSRESET",			0x5000 },
		{ "GMU_CM3_BOOT_CONFIG",		0x5001 },
		{ "GMU_CM3_FW_INIT_RESULT",		0x501c },
		{ "GMU_CM3_CFG",			0x502d },
		{ "GMU_CX_GMU_POWER_COUNTER_ENABLE",	0x5040 },
		{ "GMU_GMU_PWR_COL_KEEPALIVE",		0x50c3 },
		{ "GMU_RPMH_CTRL",			0x50e8 },
		{ "GMU_HFI_CTRL_STATUS",		0x5180 },
		{ "GMU_GENERAL_0",			0x51c5 },
	};
	uint32_t first, second, high;
	uint64_t start, now, ns;

	if (!pg_gmu_clk_on) {
		printf("POLARIS GPU: 5 the GMU's clock is not running; the GMU is not read\n");
		return false;
	}
	if (pg_gmu == 0) {
		pg_gmu = ml_io_map(PG_GMU_PHYS, PG_GMU_SIZE);
	}
	if (pg_gmu == 0) {
		printf("POLARIS GPU: 5 GMU map failed\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		printf("POLARIS GPU: 5 %-34s word %#06x off %#07x = %#010x\n", regs[i].name,
		    regs[i].word, PG_GMU_WORD(regs[i].word), pg_rd(pg_gmu + PG_GMU_WORD(regs[i].word)));
	}
	/*
	 * The always-on block counts while its clock is alive, so two reads a
	 * millisecond apart are the cheapest proof that the clock above really
	 * reaches the GMU rather than merely reporting itself on.
	 */
	first = pg_rd(pg_gmu + PG_GMU_WORD(0x5088));
	start = mach_absolute_time();
	pg_sleep_us(1000);
	second = pg_rd(pg_gmu + PG_GMU_WORD(0x5088));
	now = mach_absolute_time();
	high = pg_rd(pg_gmu + PG_GMU_WORD(0x5089));
	absolutetime_to_nanoseconds(now - start, &ns);
	printf("POLARIS GPU: 5 always-on counter %u then %u (high %u) over %llu us: %llu kHz\n",
	    first, second, high, ns / 1000,
	    ns ? (uint64_t)(second - first) * 1000000 / ns : 0);
	return second != first;
}

/* ---- 6. the GPU's SMMU ----------------------------------------------------------------------------- */

/*
 * An ARM SMMU v2, the same one the Mi 8 port reads at the same address. It
 * lives in the CX half, which is why nothing here may run before step 3, and
 * it needs gcc_gpu_memnoc_gfx_clk from step 4 for its register interface.
 * Everything in this step only reads: what the stream table and the context
 * banks say now is what the boot loader (and TrustZone) left behind, and that
 * is the state an SMMU has to be taken over from, not guessed.
 */
#define PG_SMMU_PHYS		0x05040000UL
#define PG_SMMU_SIZE		0x10000UL
#define PG_SMMU_SCR0		0x000
#define PG_SMMU_SCR1		0x004
#define PG_SMMU_SCR2		0x008
#define PG_SMMU_SACR		0x010
#define PG_SMMU_ID0		0x020
#define PG_SMMU_ID1		0x024
#define PG_SMMU_ID2		0x028
#define PG_SMMU_ID7		0x03c
#define PG_SMMU_SGFSR		0x048
#define PG_SMMU_SGFSYNR0	0x050
#define PG_SMMU_SMR(n)		(0x800 + 4 * (n))
#define PG_SMMU_S2CR(n)		(0xc00 + 4 * (n))
#define PG_SMMU_GR1		0x1000
#define PG_SMMU_CBAR(n)		(PG_SMMU_GR1 + 4 * (n))
#define PG_SMMU_CBA2R(n)	(PG_SMMU_GR1 + 0x800 + 4 * (n))
#define PG_CB_SCTLR		0x00
#define PG_CB_TCR2		0x10
#define PG_CB_TTBR0_LO		0x20
#define PG_CB_TTBR0_HI		0x24
#define PG_CB_TTBR1_LO		0x28
#define PG_CB_TTBR1_HI		0x2c
#define PG_CB_TCR		0x30
#define PG_CB_CONTEXTIDR	0x34
#define PG_CB_MAIR0		0x38
#define PG_CB_MAIR1		0x3c
#define PG_CB_FSR		0x58
#define PG_CB_TLBIASID		0x610
#define PG_CB_TLBSYNC		0x7f0
#define PG_CB_TLBSTATUS		0x7f4

static vm_offset_t pg_smmu;
static uint32_t pg_smmu_cb_base;	/* page offset of the context banks */
static uint32_t pg_smmu_page_size = 0x1000;
static uint32_t pg_smmu_num_cb, pg_smmu_num_smr;

static bool
pg_step_smmu(void)
{
	uint32_t id0, id1, id7;
	unsigned used = 0;

	if (!pg_cx_on || !pg_memnoc_on) {
		printf("POLARIS GPU: 6 the CX switch or the memory clock is off; the SMMU is not read\n");
		return false;
	}
	pg_smmu = ml_io_map(PG_SMMU_PHYS, PG_SMMU_SIZE);
	if (pg_smmu == 0) {
		printf("POLARIS GPU: 6 SMMU map failed\n");
		return false;
	}
	id0 = pg_rd(pg_smmu + PG_SMMU_ID0);
	id1 = pg_rd(pg_smmu + PG_SMMU_ID1);
	id7 = pg_rd(pg_smmu + PG_SMMU_ID7);
	printf("POLARIS GPU: 6 SMMU sCR0 %#010x sCR1 %#010x sCR2 %#010x sACR %#010x\n",
	    pg_rd(pg_smmu + PG_SMMU_SCR0), pg_rd(pg_smmu + PG_SMMU_SCR1),
	    pg_rd(pg_smmu + PG_SMMU_SCR2), pg_rd(pg_smmu + PG_SMMU_SACR));
	printf("POLARIS GPU: 6 SMMU ID0 %#010x ID1 %#010x ID2 %#010x ID7 %#010x (architecture %u.%u)\n",
	    id0, id1, pg_rd(pg_smmu + PG_SMMU_ID2), id7, (id7 >> 4) & 15, id7 & 15);
	printf("POLARIS GPU: 6 SMMU fault status sGFSR %#010x sGFSYNR0 %#010x\n",
	    pg_rd(pg_smmu + PG_SMMU_SGFSR), pg_rd(pg_smmu + PG_SMMU_SGFSYNR0));
	pg_smmu_num_smr = id0 & 0xff;
	pg_smmu_num_cb = id1 & 0xff;
	pg_smmu_page_size = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	/*
	 * The context banks start after 2^(NUMPAGENDXB+1) pages. A guess of a
	 * half of that reads the implementation's pages in between, and on the
	 * Mi 8 port that read never came back; the device tree's gpu-offset
	 * 0x48000 is 0x5040000 + 0x8000, which this gives for NUMPAGENDXB = 2.
	 */
	pg_smmu_cb_base = (1u << (((id1 >> 28) & 7) + 1)) * pg_smmu_page_size;
	if (pg_smmu_cb_base + pg_smmu_page_size * pg_smmu_num_cb > PG_SMMU_SIZE) {
		printf("POLARIS GPU: 6 the context banks would lie outside the window (%#x + %u pages): stopping\n",
		    pg_smmu_cb_base, pg_smmu_num_cb);
		return false;
	}
	printf("POLARIS GPU: 6 SMMU: %u stream entries, %u context banks, page size %u, banks from %#x "
	    "(stage 1 %s, stage 2 %s, stream matching %s)\n",
	    pg_smmu_num_smr, pg_smmu_num_cb, pg_smmu_page_size, pg_smmu_cb_base,
	    (id0 & (1u << 30)) ? "yes" : "no", (id0 & (1u << 29)) ? "yes" : "no",
	    (id0 & (1u << 27)) ? "yes" : "NO");
	printf("POLARIS GPU: 6 SMMU: unmatched streams %s (sCR0.USFCFG %u)\n",
	    (pg_rd(pg_smmu + PG_SMMU_SCR0) & (1u << 10)) ? "FAULT" : "pass",
	    (pg_rd(pg_smmu + PG_SMMU_SCR0) >> 10) & 1);
	for (uint32_t n = 0; n < pg_smmu_num_smr && n < 64; n++) {
		uint32_t smr = pg_rd(pg_smmu + PG_SMMU_SMR(n));
		uint32_t s2cr = pg_rd(pg_smmu + PG_SMMU_S2CR(n));

		if ((smr & (1u << 31)) || s2cr != 0) {
			printf("POLARIS GPU: 6 SMMU stream %2u: SMR %#010x (id %#x mask %#x%s) S2CR %#010x "
			    "(type %u, context bank %u)\n", n, smr, smr & 0x7fff, (smr >> 16) & 0x7fff,
			    (smr & (1u << 31)) ? ", valid" : "", s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		}
	}
	for (uint32_t n = 0; n < pg_smmu_num_cb && n < 32; n++) {
		uint32_t cbar = pg_rd(pg_smmu + PG_SMMU_CBAR(n));
		vm_offset_t cb = pg_smmu + pg_smmu_cb_base + pg_smmu_page_size * n;

		if (cbar == 0 && pg_rd(cb + PG_CB_SCTLR) == 0) {
			continue;
		}
		used++;
		printf("POLARIS GPU: 6 SMMU bank %u: CBAR %#010x CBA2R %#010x; SCTLR %#010x TTBR0 %#010x%08x "
		    "TCR %#010x TCR2 %#010x MAIR0 %#010x FSR %#010x\n", n, cbar,
		    pg_rd(pg_smmu + PG_SMMU_CBA2R(n)), pg_rd(cb + PG_CB_SCTLR), pg_rd(cb + PG_CB_TTBR0_HI),
		    pg_rd(cb + PG_CB_TTBR0_LO), pg_rd(cb + PG_CB_TCR), pg_rd(cb + PG_CB_TCR2),
		    pg_rd(cb + PG_CB_MAIR0), pg_rd(cb + PG_CB_FSR));
	}
	printf("POLARIS GPU: 6 SMMU: %u of %u context banks are in use\n", used, pg_smmu_num_cb);
	return true;
}

/* ---- 7. the GMU's memory --------------------------------------------------------------------------- */

/*
 * The GMU's processor loads its firmware from its own tightly coupled memory,
 * so before anything can be written into it the mapping has to be proven:
 * two words written, read back, and the old words put back. The processor has
 * to stay in reset while this runs - writing over a running image would be a
 * corrupted firmware, not a test.
 *
 * Instruction memory starts at word 0xc00 of the GMU's place (a6xx_gmu.xml's
 * GMU_CM3_ITCM_START 0x1b400, less the 0x1a800 the place itself starts at);
 * data memory 16 KB later.
 */
#define PG_GMU_ITCM		PG_GMU_WORD(0xc00)
#define PG_GMU_DTCM		PG_GMU_WORD(0x1c00)

static bool
pg_step_gmu_tcm(void)
{
	static const uint32_t pattern[2] = { 0xa5a5a5a5u, 0x5a5a5a5au };
	static const uint32_t offsets[2] = { PG_GMU_ITCM, PG_GMU_DTCM };
	static const char *const names[2] = { "instruction memory", "data memory" };
	bool good = true;

	if (!pg_gmu_clk_on || pg_gmu == 0) {
		printf("POLARIS GPU: 7 the GMU was not read; its memory is not touched\n");
		return false;
	}
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 1) {
		printf("POLARIS GPU: 7 the processor is not held in reset (CM3_SYSRESET %#010x): "
		    "its memory is not touched\n", pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
		return false;
	}
	for (int m = 0; m < 2; m++) {
		uint32_t old[2], back[2];

		old[0] = pg_rd(pg_gmu + offsets[m]);
		old[1] = pg_rd(pg_gmu + offsets[m] + 4);
		pg_wr(pg_gmu + offsets[m], pattern[0]);
		pg_wr(pg_gmu + offsets[m] + 4, pattern[1]);
		back[0] = pg_rd(pg_gmu + offsets[m]);
		back[1] = pg_rd(pg_gmu + offsets[m] + 4);
		pg_wr(pg_gmu + offsets[m], old[0]);
		pg_wr(pg_gmu + offsets[m] + 4, old[1]);
		printf("POLARIS GPU: 7 GMU %s (offset %#06x): was %#010x %#010x; wrote %#010x %#010x, "
		    "read back %#010x %#010x: %s; restored %#010x %#010x\n",
		    names[m], offsets[m], old[0], old[1], pattern[0], pattern[1], back[0], back[1],
		    (back[0] == pattern[0] && back[1] == pattern[1]) ? "same" : "DIFFERENT",
		    pg_rd(pg_gmu + offsets[m]), pg_rd(pg_gmu + offsets[m] + 4));
		good = good && back[0] == pattern[0] && back[1] == pattern[1];
	}
	return good;
}

/* ---- 8. the GMU's firmware ------------------------------------------------------------------------- */

/*
 * 32 KB, one block, to the start of the instruction memory: that is the whole
 * of what a "legacy" GMU needs, and this phone's own vendor partition has the
 * image (fw/a630_gmu.bin, 0x8000 bytes, first word the image length). It is
 * compiled in rather than read from a file because the kernel has no reason
 * to mount a filesystem to start a GPU.
 *
 * Writing it says nothing about the GPU working; the processor stays in
 * reset. What this step proves is that every word of the image is in the
 * memory it will be started from, byte for byte.
 */
static bool
pg_step_gmu_firmware(void)
{
	uint32_t wrong = 0, sum = 0, first_wrong = 0;

	if (!pg_gmu_clk_on || pg_gmu == 0) {
		printf("POLARIS GPU: 8 the GMU was not read; its memory is not written\n");
		return false;
	}
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 1) {
		printf("POLARIS GPU: 8 the processor is not held in reset (CM3_SYSRESET %#010x): "
		    "its memory is not written\n", pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
		return false;
	}
	printf("POLARIS GPU: 8 %u words (%u bytes) from this phone's own a630_gmu.bin, "
	    "first words %#010x %#010x\n", POLARIS_A630_GMU_WORDS, POLARIS_A630_GMU_WORDS * 4,
	    polaris_a630_gmu_bin[0], polaris_a630_gmu_bin[1]);
	/*
	 * Both loops stay loops. The image is 8192 words and every store is an
	 * MMIO write to the GMU; left to itself clang unrolls this into eight
	 * thousand inline stores, which took the CI build from thirteen minutes
	 * to over forty and had to be cancelled.
	 */
#pragma clang loop unroll(disable)
	for (uint32_t i = 0; i < POLARIS_A630_GMU_WORDS; i++) {
		pg_wr(pg_gmu + PG_GMU_ITCM + 4 * i, polaris_a630_gmu_bin[i]);
		sum += polaris_a630_gmu_bin[i];
	}
#pragma clang loop unroll(disable)
	for (uint32_t i = 0; i < POLARIS_A630_GMU_WORDS; i++) {
		if (pg_rd(pg_gmu + PG_GMU_ITCM + 4 * i) != polaris_a630_gmu_bin[i]) {
			if (wrong++ == 0) {
				first_wrong = i;
			}
		}
	}
	printf("POLARIS GPU: 8 the image is in the GMU's memory (word sum %#010x): "
	    "%u of %u words read back wrong%s\n", sum, wrong, POLARIS_A630_GMU_WORDS,
	    wrong ? "" : " -- all of it is as written");
	if (wrong) {
		printf("POLARIS GPU: 8 the first wrong word is %u: %#010x, should be %#010x\n",
		    first_wrong, pg_rd(pg_gmu + PG_GMU_ITCM + 4 * first_wrong),
		    polaris_a630_gmu_bin[first_wrong]);
		return false;
	}
	return true;
}

/* ---- 9. the RSC and the PDC: the GPU's own RPMh sequencer ------------------------------------------ */

/*
 * The GMU does not switch the 3D core on by itself: it asks the RPMh, and on
 * this SoC (a630, not the a6xx families that use the newer path) the request
 * goes through the *resource state coordinator* that sits inside the GPU's
 * own register window, which in turn drives a *power domain controller* whose
 * little program lives in two always-on blocks outside it. All three have to
 * be programmed before the GMU is started, or its first vote for GX power is
 * never answered and it waits forever.
 *
 *   RSCC  GMU window + 0x23000   the GPU's command sequencer
 *   PDC   0x0b280000             the PDC's registers for the GPU's domain
 *   SEQ   0x0b480000             the PDC's sequencer memory
 *
 * The values are Linux's a6xx_gmu_rpmh_init() for a630, as the Mi 8 port
 * transcribes them - the same table the same silicon took there. The three
 * registers whose read-back differs from what is written are listed in
 * pg_rs_mask() so a mismatch in them is not reported as a failure.
 */
#define PG_PDC_PHYS		0x0b280000UL
#define PG_PDC_SEQ_PHYS		0x0b480000UL
#define PG_PDC_SIZE		0x10000UL
#define PG_RSCC_OFFSET		0x23000u
#define PG_PDC_ENABLE		0x1140

enum pg_rs_region { PG_RS_RSCC, PG_RS_PDC, PG_RS_SEQ };

static const struct pg_rs_write {
	uint8_t region;
	uint16_t word;
	const char *name;
	uint32_t value;
} pg_rs_writes[] = {
	{ PG_RS_RSCC, 0x0004, "RSC_STATUS0_DRV0", 1u << 24 },
	{ PG_RS_RSCC, 0x000b, "RSCC_PDC_SLAVE_ID_DRV0", 1 },
	{ PG_RS_RSCC, 0x000e, "RSCC_HIDDEN_TCS_CMD0_DATA", 0 },
	{ PG_RS_RSCC, 0x000d, "RSCC_HIDDEN_TCS_CMD0_ADDR", 0 },
	{ PG_RS_RSCC, 0x0010, "RSCC_HIDDEN_TCS_CMD0_DATA+2", 0 },
	{ PG_RS_RSCC, 0x000f, "RSCC_HIDDEN_TCS_CMD0_ADDR+2", 0 },
	{ PG_RS_RSCC, 0x0012, "RSCC_HIDDEN_TCS_CMD0_DATA+4", 0x80000000u },
	{ PG_RS_RSCC, 0x0011, "RSCC_HIDDEN_TCS_CMD0_ADDR+4", 0 },
	{ PG_RS_RSCC, 0x0100, "RSCC_OVERRIDE_START_ADDR", 0 },
	{ PG_RS_RSCC, 0x0008, "RSCC_PDC_SEQ_START_ADDR", 0x4520 },
	{ PG_RS_RSCC, 0x0009, "RSCC_PDC_MATCH_VALUE_LO", 0x4510 },
	{ PG_RS_RSCC, 0x000a, "RSCC_PDC_MATCH_VALUE_HI", 0x4514 },
	{ PG_RS_RSCC, 0x0180, "RSCC_SEQ_MEM_0_DRV0", 0xa7a506a0u },
	{ PG_RS_RSCC, 0x0181, "RSCC_SEQ_MEM_0_DRV0+1", 0xa1e6a6e7u },
	{ PG_RS_RSCC, 0x0182, "RSCC_SEQ_MEM_0_DRV0+2", 0xa2e081e1u },
	{ PG_RS_RSCC, 0x0183, "RSCC_SEQ_MEM_0_DRV0+3", 0xe9a982e2u },
	{ PG_RS_RSCC, 0x0184, "RSCC_SEQ_MEM_0_DRV0+4", 0x0020e8a8u },
	{ PG_RS_SEQ, 0x0000, "PDC_GPU_SEQ_MEM_0", 0xfebea1e1u },
	{ PG_RS_SEQ, 0x0001, "PDC_GPU_SEQ_MEM_0+1", 0xa5a4a3a2u },
	{ PG_RS_SEQ, 0x0002, "PDC_GPU_SEQ_MEM_0+2", 0x8382a6e0u },
	{ PG_RS_SEQ, 0x0003, "PDC_GPU_SEQ_MEM_0+3", 0xbce3e284u },
	{ PG_RS_SEQ, 0x0004, "PDC_GPU_SEQ_MEM_0+4", 0x002081fcu },
	{ PG_RS_PDC, 0x1573, "PDC_GPU_TCS1_CMD_ENABLE_BANK", 7 },
	{ PG_RS_PDC, 0x1574, "PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK", 0 },
	{ PG_RS_PDC, 0x1572, "PDC_GPU_TCS1_CONTROL", 0 },
	{ PG_RS_PDC, 0x1575, "PDC_GPU_TCS1_CMD0_MSGID", 0x10108 },
	{ PG_RS_PDC, 0x1576, "PDC_GPU_TCS1_CMD0_ADDR", 0x30010 },
	{ PG_RS_PDC, 0x1577, "PDC_GPU_TCS1_CMD0_DATA", 1 },
	{ PG_RS_PDC, 0x1579, "PDC_GPU_TCS1_CMD0_MSGID+4", 0x10108 },
	{ PG_RS_PDC, 0x157a, "PDC_GPU_TCS1_CMD0_ADDR+4", 0x30000 },
	{ PG_RS_PDC, 0x157b, "PDC_GPU_TCS1_CMD0_DATA+4", 0 },
	{ PG_RS_PDC, 0x157d, "PDC_GPU_TCS1_CMD0_MSGID+8", 0x10108 },
	{ PG_RS_PDC, 0x157e, "PDC_GPU_TCS1_CMD0_ADDR+8", 0x30080 },
	{ PG_RS_PDC, 0x157f, "PDC_GPU_TCS1_CMD0_DATA+8", 0 },
	{ PG_RS_PDC, 0x15d7, "PDC_GPU_TCS3_CMD_ENABLE_BANK", 7 },
	{ PG_RS_PDC, 0x15d8, "PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK", 0 },
	{ PG_RS_PDC, 0x15d6, "PDC_GPU_TCS3_CONTROL", 0 },
	{ PG_RS_PDC, 0x15d9, "PDC_GPU_TCS3_CMD0_MSGID", 0x10108 },
	{ PG_RS_PDC, 0x15da, "PDC_GPU_TCS3_CMD0_ADDR", 0x30010 },
	{ PG_RS_PDC, 0x15db, "PDC_GPU_TCS3_CMD0_DATA", 2 },
	{ PG_RS_PDC, 0x15dd, "PDC_GPU_TCS3_CMD0_MSGID+4", 0x10108 },
	{ PG_RS_PDC, 0x15de, "PDC_GPU_TCS3_CMD0_ADDR+4", 0x30000 },
	{ PG_RS_PDC, 0x15df, "PDC_GPU_TCS3_CMD0_DATA+4", 3 },
	{ PG_RS_PDC, 0x15e1, "PDC_GPU_TCS3_CMD0_MSGID+8", 0x10108 },
	{ PG_RS_PDC, 0x15e2, "PDC_GPU_TCS3_CMD0_ADDR+8", 0x30080 },
	{ PG_RS_PDC, 0x15e3, "PDC_GPU_TCS3_CMD0_DATA+8", 3 },
	{ PG_RS_PDC, 0x1148, "PDC_GPU_SEQ_START_ADDR", 0 },
	{ PG_RS_PDC, 0x1140, "PDC_GPU_ENABLE_PDC", 0x80000001u },
};

#define PG_RS_COUNT	((uint32_t)(sizeof(pg_rs_writes) / sizeof(pg_rs_writes[0])))

static vm_offset_t pg_rs_region[3];

/*
 * Which bits of a register read back as written. The three
 * RSCC_HIDDEN_TCS_CMD0_ADDR words follow the match values and keep them, and
 * the PDC's CMD0_MSGID does not keep bit 3 (0x10108 reads back 0x10100).
 * Linux writes them the same way, so a difference there is the hardware, not
 * a failed write.
 */
static uint32_t
pg_rs_mask(const struct pg_rs_write *w)
{
	if (w->region == PG_RS_RSCC && (w->word == 0x000d || w->word == 0x000f || w->word == 0x0011)) {
		return 0;
	}
	if (w->region == PG_RS_PDC && (w->word == 0x1575 || w->word == 0x1579 || w->word == 0x157d ||
	    w->word == 0x15d9 || w->word == 0x15dd || w->word == 0x15e1)) {
		return ~0x8u;
	}
	return ~0u;
}

static bool
pg_step_rsc_pdc(void)
{
	uint32_t different = 0, enable;

	if (pg_gmu == 0) {
		printf("POLARIS GPU: 9 the GMU was not mapped; the RSC is not touched\n");
		return false;
	}
	pg_rs_region[PG_RS_RSCC] = pg_gmu + PG_RSCC_OFFSET;
	printf("POLARIS GPU: 9 RSC at GMU+%#x: status %#010x, sequence busy %#010x, start %#010x\n",
	    PG_RSCC_OFFSET, pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x4),
	    pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x101), pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x8));
	pg_rs_region[PG_RS_PDC] = ml_io_map(PG_PDC_PHYS, PG_PDC_SIZE);
	pg_rs_region[PG_RS_SEQ] = ml_io_map(PG_PDC_SEQ_PHYS, PG_PDC_SIZE);
	if (pg_rs_region[PG_RS_PDC] == 0 || pg_rs_region[PG_RS_SEQ] == 0) {
		printf("POLARIS GPU: 9 the PDC's windows did not map; nothing written\n");
		return false;
	}
	enable = pg_rd(pg_rs_region[PG_RS_PDC] + 4 * PG_PDC_ENABLE);
	printf("POLARIS GPU: 9 PDC: enable %#010x, sequence start %#010x, TCS1 control %#010x, "
	    "TCS3 control %#010x\n", enable, pg_rd(pg_rs_region[PG_RS_PDC] + 4 * 0x1148),
	    pg_rd(pg_rs_region[PG_RS_PDC] + 4 * 0x1572), pg_rd(pg_rs_region[PG_RS_PDC] + 4 * 0x15d6));

	/*
	 * An earlier run of this ladder cannot have left the PDC programmed -
	 * every boot re-enters here from the boot loader's state - but if it
	 * reads back enabled or busy, its live tables are not touched: a
	 * sequencer that is mid-program would take half an image.
	 */
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 1 ||
	    (pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR) & (PG_GDSC_PWR_ON | PG_GDSCR_SW_COLLAPSE)) != PG_GDSCR_SW_COLLAPSE ||
	    pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x101) != 0 || (enable & 1u)) {
		printf("POLARIS GPU: 9 the RSC/PDC is not idle (enable %#010x, busy %#010x): "
		    "nothing written\n", enable, pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x101));
		return false;
	}
	pg_wr(pg_rs_region[PG_RS_PDC] + 4 * PG_PDC_ENABLE, enable & ~1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (pg_rd(pg_rs_region[PG_RS_PDC] + 4 * PG_PDC_ENABLE) & 1u) {
		printf("POLARIS GPU: 9 the PDC did not disable: its tables are not touched\n");
		return false;
	}
	for (uint32_t i = 0; i < PG_RS_COUNT; i++) {
		const struct pg_rs_write *w = &pg_rs_writes[i];
		vm_offset_t address = pg_rs_region[w->region] + 4u * w->word;
		uint32_t old = pg_rd(address), back;

		pg_wr(address, w->value);
		back = pg_rd(address);
		if (((back ^ w->value) & pg_rs_mask(w)) != 0) {
			different++;
			printf("POLARIS GPU: 9   %-38s was %#010x, wrote %#010x, read back %#010x DIFFERENT\n",
			    w->name, old, w->value, back);
		}
	}
	printf("POLARIS GPU: 9 %u entries written, %u different; RSC status %#010x, busy %#010x\n",
	    PG_RS_COUNT, different, pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x4),
	    pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * 0x101));
	return different == 0;
}

/* ---- 10. the memory the firmware will use, and its boot configuration -------------------------------- */

/*
 * The GMU is not started with a page table here. Step 6 showed what the boot
 * loader leaves: five stream entries, every one of them bypass, five context
 * banks unused. In that state the address the firmware puts on its bus *is*
 * the physical address, so the two regions it needs - a log and the HFI
 * queue table - only have to be allocated by us and handed over by their
 * physical addresses. No translation, nothing to unmap afterwards.
 *
 * The Mi 8 port hands the firmware iovars (0x60005000, 0x60009000) because it
 * does install a context. The addresses themselves are the driver's choice:
 * they sit in the boot configuration registers below.
 */
#define PG_GMU_PAGE_SIZE	0x1000u		/* one page per region, six in all */
#define PG_GMU_CHIPID		0x06030100u	/* core 6 major 3 minor 0 patch 1: what HFI_SFR_ADDR
						 * carries, spelled out by a6xx_gmu_fw_start() and
						 * the same word the Mi 8 port writes */
#define PG_GMU_HFI_QUEUES	3u
#define PG_DTCM_LAST_WORD	0xff8u		/* a6xx_gmu_start() reads the dword at this index */

/*
 * The GMU's boot configuration carries the addresses of its regions in 32-bit
 * registers, so a region above 4 GB is not addressable at all: the register
 * would keep the low half and the firmware would read somewhere else. This
 * phone has 6 GB - two DRAM windows, 0x80000000+4 GB and 0x180000000+ - and the
 * allocator hands out the second one first: the first attempt at this step
 * landed at 0x1_035a0000.
 *
 * kmem_alloc_contig() is declared in <vm/vm_kern.h> as
 *
 *     (map, addrp, size, mask, max_pnum, pnum_mask, flags, tag)
 *
 * where mask is the *address* alignment and max_pnum is the highest page
 * number the allocation may use. An earlier attempt put the 4 GB limit in the
 * fifth slot as a bit mask; ppnum_t is 32 bits, so it arrived as zero and the
 * allocation came back above the boundary exactly as before. The limit
 * belongs in max_pnum: 4 GB / 4 KB - 1.
 */
#define PG_ADDRESS_LIMIT	0x100000000ull
/*
 * The bound is in pages of this kernel's page size, and this kernel runs 16 KB
 * pages - which is how the earlier 0xfffff came to mean 16 GB, let every
 * allocation above 4 GB through, and made the boundary check dead code. The
 * 0x4000 steps in log 0130's "came back at 0x102864000, ..." lines are the
 * proof: one page per call.
 */
#define PG_MAX_PAGE_BELOW_4G	((ppnum_t)((PG_ADDRESS_LIMIT / (uint64_t)PAGE_SIZE) - 1u))
#define PG_PAGE			((vm_size_t)PAGE_SIZE)

/*
 * The first version of this step asked for four adjacent pages and got
 * KERN_RESOURCE_SHORTAGE: by the time the probe runs, the low DRAM window has
 * no run of four free pages left. It fell back to the IPS firmware region, 64 KB
 * at 0x8c400000, which is no-map in the device tree and belongs to the modem
 * and IPA subsystems - and step 13 then showed what that memory is worth to the
 * firmware: the first DRAM transaction it ever makes, the read of the queue
 * table HFI_CTRL_INIT asks for, wedged the whole machine. The addresses are in
 * a region protected from masters that are not the modem or IPA, so the read
 * neither completes nor errors.
 *
 * So: one page per region, asked for one at a time. Adjacency was never needed
 * - every queue carries its own address in the table - and a single free page
 * below 4 GB always exists. Six in all: the log, the queue table (which also
 * holds the three queue headers) and one data page per queue, plus the debug
 * buffer step 13 asks for.
 */
#define PG_RESERVED_PHYS	0x8c400000ull
#define PG_RESERVED_PAGES	8u

/*
 * ---- the address the firmware will use ---------------------------------------------------------------
 *
 * The GMU is not a free master on the bus: it sits behind the GPU's SMMU. The
 * device tree says which stream it is -
 *
 *     /soc/qcom,gmu/gmu_kernel     iommus = <&smmu-kgsl 5>
 *     /soc/qcom,gmu/gmu_user       iommus = <&smmu-kgsl 4>
 *     /soc/qcom,kgsl-iommu/gfx3d_user  iommus = <&smmu-kgsl 0>
 *
 * - and this phone's SMMU is in a state that explains step 13 exactly: sCR0
 * reads 0x00200626, whose bit 10 is USFCFG ("unmatched streams fault"), while
 * every one of the five stream entries is invalid (SMR 0, S2CR 0x000200ff).
 * Stream 5 is therefore unmatched, and the first system-memory read the
 * firmware ever makes - the queue table HFI_CTRL_INIT asks for - faults inside
 * the SMMU instead of reaching memory. Nothing before step 13 gets that far,
 * because the firmware and its TCM are entirely inside the GPU.
 *
 * So step 10 gives stream 5 a valid entry, one of two ways:
 *
 *  - polarisgpumap=0, the default: the entry says BYPASS, like the four the boot
 *    loader configured. The firmware then uses physical addresses, which have to
 *    be below 4 GB - on this phone that means the reserved IPS region, because
 *    the allocator returns nothing below the boundary in any shape it accepts.
 *  - polarisgpumap=1: entry 0 sends stream 5 to context bank 2, whose tables map
 *    0x6000_0000 upward onto ordinary kernel pages wherever they land. This is
 *    what Linux and the Mi 8 port do, and it is the only way to hand the
 *    firmware memory this kernel actually owns.
 */
#define PG_GMU_IOVA_BASE	0x60000000ull	/* a630: one 2 MB window covers every region */
#define PG_GMU_IOVA_DEBUG	0x60001000ull
#define PG_GMU_IOVA_LOG		0x60005000ull
#define PG_GMU_IOVA_HFI		0x60009000ull
#define PG_GMU_STREAM		5u		/* /soc/qcom,gmu/gmu_kernel */
#define PG_GMU_SMR_ENTRY	0u		/* entry 0's SMR is invalid, so nothing owns it */
#define PG_GMU_CTX		2u		/* bank 0 belongs to the 3D core, which never starts */
/*
 * S2CR_TYPE is bits 17:16 and the enum is TRANS = 0, BYPASS = 1, FAULT = 2.
 * 0x000200ff is the fault entry the boot loader leaves in all five slots, and
 * writing it back is what step 0129 did by mistake: it made the GMU's very
 * first read fail instead of passing through. Bypass carries no context bank.
 */
#define PG_S2CR_TYPE_SHIFT	16
#define PG_S2CR_TRANS		0u
#define PG_S2CR_BYPASS		1u
#define PG_S2CR_FAULT		2u

/*
 * sCR0, the two bits that decide what happens to a stream nobody has mapped.
 * arm-smmu.h: CLIENTPD is bit 0 (the client port off, which takes the whole
 * SMMU out of the path) and USFCFG is bit 10.
 */
#define PG_SCR0_CLIENTPD	(1u << 0)
#define PG_SCR0_GFRE		(1u << 1)	/* global fault reporting enable */
#define PG_SCR0_USFCFG		(1u << 10)
#define PG_SMR_VALID		(1u << 31)
#define PG_CBAR_STAGE1		(1u << 16 | 3u << 8 | 0xfu << 12)
					/* arm-smmu.h: TYPE 1 = stage 1 translates and stage 2
					 * bypasses; 0xf in MEMATTR is write-back for the
					 * walks, 3 in BPSHCFG is non-shareable. The Mi 8
					 * port runs the same three fields on this SoC. */
#define PG_CBA2R_64BIT		1u

#define PG_PTE_TABLE		3ull
#define PG_PTE_PAGE		3ull
/*
 * Lower attributes of a stage 1 page descriptor:
 *   bits  9:8  SH   = 2, outer shareable
 *   bit  10    AF   = 1, accessed
 *   bit  11    nG   = 1, not global
 *   bits  7:6  AP[2:1]
 *
 * AP is the one that cost the Mi 8 port a whole experiment (its EXP-0108): with
 * AP[2:1] = 00 the page is privileged-access-only, and the 3D core issues its
 * fetches from an unprivileged context, so the first SQE read stopped with a
 * fault at FAR 0x10000000 - the exact address and the exact behaviour build
 * 0141 showed here. AP[1] = 1 lets an unprivileged read and write through.
 */
#define PG_PTE_ATTR		((2ull << 8) | (1ull << 10) | (1ull << 11) | (1ull << 6))
#define PG_PTE_ADDR		0x0000fffffffff000ull
#define PG_SCTLR_ON		(1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define PG_TCR_39_BITS		(25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define PG_TCR2_36_BITS		(1u | 1u << 4 | 7u << 15)
#define PG_MAIR0_LINUX		0xf404ff44u	/* index 0 is normal memory, not cached: what the PTEs pick */

struct pg_hfi_table {
	uint32_t version, size, qhdr0_offset, qhdr_size, num_queues, active_queues;
};

struct pg_hfi_queue {
	uint32_t status, iova, type, size, msg_size, dropped;
	uint32_t rx_watermark, tx_watermark, rx_request, tx_request, read_index, write_index;
};

static vm_offset_t pg_log_va, pg_hfi_va, pg_dbg_va;
static uint64_t pg_log_pa, pg_hfi_pa, pg_dbg_pa;
static vm_offset_t pg_queue_va[PG_GMU_HFI_QUEUES];	/* one data page per queue */
static uint64_t pg_queue_pa[PG_GMU_HFI_QUEUES];
static uint64_t pg_log_iova, pg_hfi_iova, pg_dbg_iova;	/* what the firmware is told */
static uint64_t pg_queue_iova[PG_GMU_HFI_QUEUES];
#define PG_CP_SQE_IOVA		0x10000000ull	/* the CP's microcode and ring live here */
#define PG_CP_RING_IOVA		0x10008000ull
#define PG_CP_WINDOW_L2	0x00000080u	/* (0x10000000 >> 21) & 0x1ff: the CP's window */
/*
 * How many 2 MB level 2 entries the CP's window spans.
 *
 * Build 0154 could only fill a 64-row band of the panel, because one level 3
 * page is 2 MB and step 17's scratch already owned most of it. Five entries
 * make the window 0x10000000-0x109fffff (10 MB), which is enough for the whole
 * 1080x2160 panel (9331200 bytes) with room to spare - and the panel starts at
 * 0x100a0000, above step 17's scratch, so nothing already mapped moves.
 *
 * The level 1 entry is unchanged: 0x10000000 >> 30 is 0 for every one of these
 * five entries, and build 0140's fault was an empty level 1 entry, not an
 * empty level 2 one.
 */
#define PG_CP_WINDOW_L2S	5u

static vm_offset_t pg_l1_va, pg_l2_va, pg_l3_va;	/* one page each; adjacency is not needed */
static vm_offset_t pg_l3b_va;			/* the CP's window: PG_CP_WINDOW_L2S pages */
static bool pg_keep_running;		/* the ladder continues past step 13 */
static bool pg_smmu_mode;			/* polarisgpumap=1: translate instead of bypass */
static int pg_mmu_way;				/* polarisgpummu: 0 auto, 1 entry, 2 unmatched, 3 off */
static int pg_mem_way;				/* polarisgpumem: 0 ips, 1 ramdump, 2 splash */
static unsigned pg_region_count;		/* 0 while neither region exists */
static bool pg_boot_configured;
static uint32_t pg_old_ao_mask, pg_old_h2h_mask;
static bool pg_masks_saved;

/* The GMU reads from memory, not from this processor's cache. */
static void
pg_to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* A reserved region is not zeroed for us and is mapped uncached, so the stores
 * go straight to memory; the allocator's pages arrive zeroed already. */
static void
pg_zero(vm_offset_t start, vm_size_t size)
{
	volatile uint32_t *p = (volatile uint32_t *)start;

	for (vm_size_t i = 0; i < size / 4; i++) {
		p[i] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Where the firmware's regions come from when the allocator cannot help. All
 * three are plain DRAM below 4 GB that no live driver owns, and all three are
 * listed in the device tree's reserved-memory node:
 *
 *   ips_fw_region@0x8c400000      64 KB   no-map, removed-dma-pool
 *   cont_splash_region@0x9d400000 36 MB   the boot logo buffer, dead after sweetfb
 *   ramdump_fb_region@0xaf000000  16 MB   written only by the crash dump
 *
 * The first is the one the ladder has been using. It is also the one whose read
 * the firmware never survived - but that read was made through a stream entry
 * the hardware had rewritten into a fault, so the verdict is not in yet. The
 * other two are selected with polarisgpumem=1 and =2.
 */
static const struct {
	uint64_t base;
	const char *what;
} pg_mem_regions[] = {
	{ 0x8c400000ull, "the IPS firmware region" },
	{ 0xaf000000ull, "the ramdump framebuffer" },
	{ 0x9d400000ull, "the continuous splash region" },
};

static uint64_t
pg_reserved_base(void)
{
	unsigned i = (pg_mem_way >= 0 &&
	    pg_mem_way < (int)(sizeof(pg_mem_regions) / sizeof(pg_mem_regions[0]))) ?
	    (unsigned)pg_mem_way : 0u;

	return pg_mem_regions[i].base;
}

static const char *
pg_reserved_name(void)
{
	unsigned i = (pg_mem_way >= 0 &&
	    pg_mem_way < (int)(sizeof(pg_mem_regions) / sizeof(pg_mem_regions[0]))) ?
	    (unsigned)pg_mem_way : 0u;

	return pg_mem_regions[i].what;
}

/*
 * The firmware reads its HFI table, its three queues, its log and its debug
 * buffer with its own bus master. Two things it needs from us: a page each, and
 * an address it can put on that bus. Which address depends on the entry step 10
 * writes into the SMMU's stream table - physical when the entry bypasses, an
 * iovar when it translates - so the allocation is the same and only the fallback
 * rules differ.
 *
 * In bypass mode the address is 32 bits wide, so a page above 4 GB is useless.
 * The allocator gives nothing else (see above), so the reserved IPS firmware
 * region is what is left; in translate mode the page may be anywhere and comes
 * from the kernel's own memory.
 */
static bool
pg_gmu_region(vm_offset_t *va, uint64_t *pa, const char *name)
{
	unsigned index = pg_region_count;

	if (*va != 0) {
		return true;
	}
	if (pg_smmu_mode) {
		if (kmem_alloc(kernel_map, va, PG_GMU_PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("POLARIS GPU: 10 the allocator has no page at all for %s\n", name);
			return false;
		}
		*pa = kvtophys(*va);
		printf("POLARIS GPU: 10 %s: a kernel page at %#llx\n", name, *pa);
		return true;
	}
	/*
	 * KMA_LOMEM is what makes this work at all. kmem_alloc_contig walks the
	 * page array from a remembered index and stops at the first page above
	 * max_pnum ("no more low pages"), so after the machine has handed out
	 * memory once the scan starts high and gives up immediately - that is why
	 * every page used to come back at 0x1_035a8000 and be given back. The
	 * LOMEM flag selects a second cursor (vm_page_lomem_find_contiguous_last_idx)
	 * that starts at page 0, so the scan really does begin in low DRAM.
	 */
	for (int attempt = 0; attempt < 4; attempt++) {
		if (kmem_alloc_contig(kernel_map, va, PG_GMU_PAGE_SIZE, 0, PG_MAX_PAGE_BELOW_4G, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_LOMEM,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			*va = 0;
			printf("POLARIS GPU: 10 the allocator has no page below 4 GB for %s "
			    "(attempt %d)\n", name, attempt + 1);
			break;		/* the reserved region may still do */
		}
		*pa = kvtophys(*va);
		if (*pa != 0 && *pa + PG_GMU_PAGE_SIZE <= PG_ADDRESS_LIMIT) {
			printf("POLARIS GPU: 10 %s: ordinary DRAM from the allocator at %#llx\n",
			    name, *pa);
			pg_region_count = index + 1;
			return true;
		}
		printf("POLARIS GPU: 10 %s came back at %#llx, above the 32-bit boundary the "
		    "firmware can address: given back\n", name, *pa);
		kmem_free(kernel_map, *va, PG_GMU_PAGE_SIZE);
		*va = 0;
	}
	if (index < PG_RESERVED_PAGES) {
		uint64_t base = pg_reserved_base();
		uint64_t phys = base + (uint64_t)index * PG_GMU_PAGE_SIZE;
		vm_offset_t mapped = ml_io_map((vm_offset_t)phys, PG_PAGE);

		if (mapped != 0) {
			*va = mapped;
			*pa = phys;
			pg_zero(mapped, PG_PAGE);
			printf("POLARIS GPU: 10 %s at %#llx, out of %s, mapped uncached\n",
			    name, phys, pg_reserved_name());
			pg_region_count = index + 1;
			return true;
		}
		printf("POLARIS GPU: 10 the reserved page at %#llx could not be mapped\n", phys);
	}
	printf("POLARIS GPU: 10 no memory at all for %s\n", name);
	return false;
}

/* ---- the SMMU's context bank 2 and the tables behind it --------------------------------------------- */

static vm_offset_t
pg_smmu_cb(uint32_t n)
{
	return pg_smmu + pg_smmu_cb_base + pg_smmu_page_size * n;
}

/* One table of 4 KB pages, three levels. Every region lies inside the single
 * 2 MB window 0x6000_0000 opens, so one page per level is all it takes. */
static bool
pg_smmu_tables(void)
{
	/*
	 * One 4 KB page is exactly what 512 descriptors need, and 4 KB is the size
	 * the five region pages were just allocated at. Build 0135 asked for
	 * PAGE_SIZE (16 KB) and the machine stopped dead inside this function,
	 * so the size is the only thing that changed; the checkpoint says which
	 * of the three allocations it is if it happens again.
	 */
	if (pg_l3_va != 0) {
		return true;
	}
	PG_CHECK("tables: the level 1 page");
	if (kmem_alloc(kernel_map, &pg_l1_va, PG_GMU_PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("POLARIS GPU: 10 no memory for the SMMU's level 1 table\n");
		return false;
	}
	PG_CHECK("tables: the level 2 page");
	if (kmem_alloc(kernel_map, &pg_l2_va, PG_GMU_PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("POLARIS GPU: 10 no memory for the SMMU's level 2 table\n");
		return false;
	}
	PG_CHECK("tables: the level 3 page");
	if (kmem_alloc(kernel_map, &pg_l3_va, PG_GMU_PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("POLARIS GPU: 10 no memory for the SMMU's level 3 table\n");
		return false;
	}
	PG_CHECK("tables: the second level 3 page, for the CP's window");
	if (kmem_alloc(kernel_map, &pg_l3b_va, PG_CP_WINDOW_L2S * PG_GMU_PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("POLARIS GPU: 10 no memory for the SMMU's second level 3 table\n");
		return false;
	}
	PG_CHECK("tables: all four pages are in place");
	/*
	 * Both windows hang off the SAME level 2 page, but they hang off different
	 * LEVEL 1 entries: 0x60000000 >> 30 is 1 and 0x10000000 >> 30 is 0. Build
	 * 0140 filled only entry 1, so the CP's window walked into a zero L1 entry
	 * and the 3D core's first instruction fetch was a translation fault
	 * (CB FSR 0x402) - the ring was never read. Both entries point at the one
	 * level 2 page; that page's index 0x300 is the GMU's window and 0x80 is
	 * the CP's.
	 */
	((uint64_t *)pg_l1_va)[(PG_GMU_IOVA_BASE >> 30) & 0x1ff] =
	    kvtophys(pg_l2_va) | PG_PTE_TABLE;
	((uint64_t *)pg_l1_va)[(PG_CP_SQE_IOVA >> 30) & 0x1ff] =
	    kvtophys(pg_l2_va) | PG_PTE_TABLE;
	((uint64_t *)pg_l2_va)[(PG_GMU_IOVA_BASE >> 21) & 0x1ff] =
	    kvtophys(pg_l3_va) | PG_PTE_TABLE;
	/*
	 * The CP's window is PG_CP_WINDOW_L2S level 3 pages back to back, so the
	 * 3D core sees 10 MB of contiguous IOVA instead of 2 MB. Only the first
	 * page is used before step 18; the rest exist so the panel fits.
	 */
	for (uint32_t i = 0; i < PG_CP_WINDOW_L2S; i++) {
		((uint64_t *)pg_l2_va)[PG_CP_WINDOW_L2 + i] =
		    (kvtophys(pg_l3b_va) + (uint64_t)i * PG_GMU_PAGE_SIZE) | PG_PTE_TABLE;
	}
	PG_CHECK("tables: the descriptors are written, flushing");
	pg_to_memory(pg_l1_va, PG_GMU_PAGE_SIZE);
	pg_to_memory(pg_l2_va, PG_GMU_PAGE_SIZE);
	printf("POLARIS GPU: 10 SMMU tables: level 1 %#llx, level 2 %#llx, level 3 %#llx and "
	    "%#llx (%u pages, the CP's window: %#llx-%#llx)\n", kvtophys(pg_l1_va), kvtophys(pg_l2_va),
	    kvtophys(pg_l3_va), kvtophys(pg_l3b_va), PG_CP_WINDOW_L2S, PG_CP_SQE_IOVA,
	    PG_CP_SQE_IOVA + (uint64_t)PG_CP_WINDOW_L2S * 0x200000ull - 1ull);
	return true;
}

/*
 * Which level 3 table owns an address. The GMU's queues live in one 2 MB window
 * at 0x60000000; the CP's microcode, ring and everything it is asked to write
 * live in PG_CP_WINDOW_L2S consecutive 2 MB windows starting at 0x10000000.
 * Anything else has no table and must not be mapped - a wrong slot here would
 * silently rewrite another window's descriptors.
 */
static vm_offset_t
pg_smmu_l3(uint64_t iova)
{
	uint32_t l2 = (uint32_t)((iova >> 21) & 0x1ff);

	if (l2 == (uint32_t)((PG_GMU_IOVA_BASE >> 21) & 0x1ff)) {
		return pg_l3_va;
	}
	if (l2 >= PG_CP_WINDOW_L2 && l2 < PG_CP_WINDOW_L2 + PG_CP_WINDOW_L2S) {
		return pg_l3b_va + (vm_offset_t)(l2 - PG_CP_WINDOW_L2) * PG_GMU_PAGE_SIZE;
	}
	return 0;
}

static bool
pg_smmu_map(uint64_t iova, vm_offset_t va, uint32_t pages, const char *name)
{
	uint64_t *l3;

	/*
	 * The pointer has to be taken AFTER the tables exist. Builds 0136 and 0137
	 * read it first, so on the very first mapping pg_l3_va was still 0 and the
	 * descriptor store went to address zero plus the slot offset - the kernel
	 * data abort with far 0x20/0x28 that both builds died on, one instruction
	 * after pg_smmu_tables() returned.
	 */
	if (!pg_smmu_tables()) {
		return false;
	}
	l3 = (uint64_t *)pg_smmu_l3(iova);
	if (l3 == NULL) {
		printf("POLARIS GPU: 10 %s at %#llx is outside the windows the tables cover\n",
		    name, iova);
		return false;
	}
	for (uint32_t i = 0; i < pages; i++) {
		l3[((iova >> 12) + i) & 0x1ff] =
		    (kvtophys(va + (vm_offset_t)PG_GMU_PAGE_SIZE * i) & PG_PTE_ADDR) |
		    PG_PTE_ATTR | PG_PTE_PAGE;
	}
	/*
	 * Flush exactly what was allocated: every buffer in this file is
	 * PG_GMU_PAGE_SIZE, so the cache maintenance length has to be too.
	 * Flushing PAGE_SIZE from a 4 KB allocation reaches twelve kilobytes of
	 * unmapped kernel_map, and dc civac on that is a data abort.
	 */
	PG_CHECK("map: the descriptor is written, flushing the level 3 page");
	pg_to_memory((vm_offset_t)l3, PG_GMU_PAGE_SIZE);
	PG_CHECK("map: the level 3 page is flushed");
	printf("POLARIS GPU: 10 %s: %#llx -> %#llx, descriptor %#llx in slot %u\n", name, iova,
	    kvtophys(va), l3[((iova >> 12) & 0x1ff)], (uint32_t)((iova >> 12) & 0x1ff));
	return true;
}

/*
 * IMPL_DEF1 is at base + 6 pages, and its first word is the micro MMU's
 * control word. The vendor driver halts the SMMU around the
 * implementation-defined writes and resumes it afterwards:
 *
 *     qsmmuv2_halt(smmu);
 *     for (i = 0; i < smmu->num_impl_def_attach_registers; ++i)
 *             writel_relaxed(regs[i].value, ARM_SMMU_GR0(smmu) + regs[i].offset);
 *     qsmmuv2_resume(smmu);
 *
 * with LOCAL_HALT_REQ = bit 2 and IDLE = bit 3. Log 0131 wrote the twelve pairs
 * without halting first, and one of them was refused.
 */
#define PG_SMMU_IMPL_DEF1	0x6000u
#define PG_SMMU_MICRO_CTRL	0x0000u
#define PG_MICRO_HALT_REQ	(1u << 2)
#define PG_MICRO_IDLE		(1u << 3)

static bool
pg_smmu_halt(void)
{
	uint32_t ctl = pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL);

	pg_wr(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL, ctl | PG_MICRO_HALT_REQ);
	for (int i = 0; i < 300; i++) {
		if (pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL) & PG_MICRO_IDLE) {
			printf("POLARIS GPU: 10 the SMMU halted: micro control %#010x -> %#010x "
			    "after %d us\n", ctl,
			    pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL), i * 100);
			return true;
		}
		pg_sleep_us(100);
	}
	printf("POLARIS GPU: 10 the SMMU would not halt: micro control %#010x -> %#010x\n", ctl,
	    pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL));
	return false;
}

static void
pg_smmu_resume(void)
{
	uint32_t ctl = pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL);

	pg_wr(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL, ctl & ~PG_MICRO_HALT_REQ);
	printf("POLARIS GPU: 10 the SMMU resumed: micro control %#010x -> %#010x\n", ctl,
	    pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1 + PG_SMMU_MICRO_CTRL));
}

/*
 * What the stream table will and will not take. Entry 1 is the one to ask with:
 * nothing uses it (the GPU's streams are 0, 1, 2, 4 and 5), and the value is
 * put back afterwards. If only bits 7:0 come back, the type field is locked and
 * no amount of retrying will change it; if the whole word comes back, the write
 * path is fine and log 0131's failure was somewhere else.
 */
static void
pg_smmu_probe(void)
{
	uint32_t scr0 = pg_rd(pg_smmu + PG_SMMU_SCR0);
	uint32_t old_s2cr = pg_rd(pg_smmu + PG_SMMU_S2CR(1));
	uint32_t old_smr = pg_rd(pg_smmu + PG_SMMU_SMR(1));
	uint32_t back;

	printf("POLARIS GPU: 10 before anything: micro %#010x impl_def0 %#010x %#010x sACR %#010x "
	    "ID7 %#010x\n", pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1),
	    pg_rd(pg_smmu + 0x2000), pg_rd(pg_smmu + 0x2004), pg_rd(pg_smmu + PG_SMMU_SACR),
	    pg_rd(pg_smmu + PG_SMMU_ID7));

	pg_wr(pg_smmu + PG_SMMU_S2CR(1), 0xffffffffu);
	back = pg_rd(pg_smmu + PG_SMMU_S2CR(1));
	printf("POLARIS GPU: 10 S2CR[1] was %#010x, wrote ffffffff, reads %#010x: writable mask "
	    "%#010x\n", old_s2cr, back, back);
	pg_wr(pg_smmu + PG_SMMU_S2CR(1), old_s2cr);

	pg_wr(pg_smmu + PG_SMMU_SMR(1), 0xffffffffu);
	back = pg_rd(pg_smmu + PG_SMMU_SMR(1));
	printf("POLARIS GPU: 10 SMR[1] was %#010x, wrote ffffffff, reads %#010x\n", old_smr, back);
	pg_wr(pg_smmu + PG_SMMU_SMR(1), old_smr);

	pg_wr(pg_smmu + PG_SMMU_SCR0, scr0 ^ PG_SCR0_GFRE);
	back = pg_rd(pg_smmu + PG_SMMU_SCR0);
	printf("POLARIS GPU: 10 sCR0 %#010x, flipped bit 1, reads %#010x: %s\n", scr0, back,
	    (back & PG_SCR0_GFRE) != (scr0 & PG_SCR0_GFRE) ? "writable" : "IGNORED, read-only");
	pg_wr(pg_smmu + PG_SMMU_SCR0, scr0);
	printf("POLARIS GPU: 10 sCR0 restored to %#010x\n", pg_rd(pg_smmu + PG_SMMU_SCR0));
}

/*
 * Qualcomm's implementation-defined block, straight out of the device tree:
 *
 *     /soc/arm,smmu-kgsl@5040000  ...  attach-impl-defs = <0x6000 0x2378
 *         0x6060 0x1055 0x678c 0x8 0x6794 0x28 0x6800 0x6 0x6900 0x3ff
 *         0x6924 0x204 0x6928 0x11000 0x6930 0x800 0x6960 0xffffffff
 *         0x6b64 0x1a5551 0x6b68 0x9a82a382>;
 *
 * The vendor driver writes every pair once when it attaches to the SMMU, and
 * the offsets are far outside the architectural map (above 0x6000), so this is
 * the translation buffer unit and its timing. Log 0130 is why it matters: a
 * write of BYPASS into S2CR[0] came back as FAULT with only the low byte taken,
 * which is what a configuration register that has not had this step done looks
 * like.
 */
static const struct {
	uint32_t off;
	uint32_t value;
} pg_smmu_impl[] = {
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x8 }, { 0x6794, 0x28 },
	{ 0x6800, 0x6 }, { 0x6900, 0x3ff }, { 0x6924, 0x204 }, { 0x6928, 0x11000 },
	{ 0x6930, 0x800 }, { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 },
	{ 0x6b68, 0x9a82a382 },
};

static bool
pg_smmu_impl_defs(void)
{
	unsigned wrong = 0;

	for (unsigned i = 0; i < sizeof(pg_smmu_impl) / sizeof(pg_smmu_impl[0]); i++) {
		uint32_t off = pg_smmu_impl[i].off;
		uint32_t want = pg_smmu_impl[i].value;
		uint32_t got;

		pg_wr(pg_smmu + off, want);
		got = pg_rd(pg_smmu + off);
		if (got != want) {
			printf("POLARIS GPU: 10 impl %#06x took %#010x, wanted %#010x\n",
			    off, got, want);
			wrong++;
		}
	}
	printf("POLARIS GPU: 10 attach-impl-defs: %u of %u registers did not take\n", wrong,
	    (unsigned)(sizeof(pg_smmu_impl) / sizeof(pg_smmu_impl[0])));
	return wrong == 0;
}

/*
 * Entry 0 of the stream table, made valid for stream 5. Bypass writes no
 * context bank at all, which is exactly what the four entries the boot loader
 * configured look like; translate brings up bank 2 first, tag and translate the
 * tables, and only then sends the stream to it (arm-smmu.c's order).
 */
static bool
pg_smmu_arm(void)
{
	uint32_t s2cr = pg_smmu_mode ?
	    (PG_GMU_CTX | (PG_S2CR_TRANS << PG_S2CR_TYPE_SHIFT)) :
	    (PG_S2CR_BYPASS << PG_S2CR_TYPE_SHIFT);
	uint32_t smr = PG_SMR_VALID | PG_GMU_STREAM;

	if (pg_smmu == 0) {
		printf("POLARIS GPU: 10 the SMMU was never mapped: stream 5 stays unmatched\n");
		return false;
	}
	PG_CHECK("10: probing the stream table");
	pg_smmu_probe();
	PG_CHECK("10: the stream table is probed, halting the SMMU");
	if (pg_smmu_halt()) {
		(void)pg_smmu_impl_defs();
		pg_smmu_resume();
	} else {
		(void)pg_smmu_impl_defs();
	}
	printf("POLARIS GPU: 10 after the implementation registers: sCR0 %#010x S2CR[0] %#010x "
	    "micro %#010x\n", pg_rd(pg_smmu + PG_SMMU_SCR0),
	    pg_rd(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY)), pg_rd(pg_smmu + PG_SMMU_IMPL_DEF1));
	if (pg_smmu_mode) {
		vm_offset_t cb = pg_smmu_cb(PG_GMU_CTX);
		uint64_t root;

		if (!pg_smmu_tables()) {
			return false;
		}
		root = (kvtophys(pg_l1_va) & PG_PTE_ADDR) | ((uint64_t)PG_GMU_CTX << 48);
		printf("POLARIS GPU: 10 bank %u as the boot loader left it: CBAR %#010x CBA2R %#010x "
		    "SCTLR %#010x TTBR0 %#010x%08x TCR %#010x TCR2 %#010x FSR %#010x S2CR %#010x; "
		    "sCR0 %#010x sGFSR %#010x\n", PG_GMU_CTX,
		    pg_rd(pg_smmu + PG_SMMU_CBAR(PG_GMU_CTX)),
		    pg_rd(pg_smmu + PG_SMMU_CBA2R(PG_GMU_CTX)),
		    pg_rd(cb + PG_CB_SCTLR), pg_rd(cb + PG_CB_TTBR0_HI), pg_rd(cb + PG_CB_TTBR0_LO),
		    pg_rd(cb + PG_CB_TCR), pg_rd(cb + PG_CB_TCR2), pg_rd(cb + PG_CB_FSR),
		    pg_rd(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY)), pg_rd(pg_smmu + PG_SMMU_SCR0),
		    pg_rd(pg_smmu + PG_SMMU_SGFSR));
		PG_CHECK("bank 2: before the context bank is touched");
		pg_wr(cb + PG_CB_SCTLR, 0);
		pg_wr(pg_smmu + PG_SMMU_CBA2R(PG_GMU_CTX), PG_CBA2R_64BIT);
		pg_wr(pg_smmu + PG_SMMU_CBAR(PG_GMU_CTX), PG_CBAR_STAGE1);
		printf("POLARIS GPU: 10 bank %u: CBAR %#010x CBA2R %#010x (stage 1 translate, "
		    "64-bit TTBR)\n", PG_GMU_CTX, pg_rd(pg_smmu + PG_SMMU_CBAR(PG_GMU_CTX)),
		    pg_rd(pg_smmu + PG_SMMU_CBA2R(PG_GMU_CTX)));
		PG_CHECK("bank 2: CBAR and CBA2R written");
		pg_wr(cb + PG_CB_TCR2, PG_TCR2_36_BITS);
		pg_wr(cb + PG_CB_TCR, PG_TCR_39_BITS);
		pg_wr(cb + PG_CB_TTBR0_LO, (uint32_t)root);
		pg_wr(cb + PG_CB_TTBR0_HI, (uint32_t)(root >> 32));
		pg_wr(cb + PG_CB_TTBR1_LO, 0);
		pg_wr(cb + PG_CB_TTBR1_HI, 0);
		PG_CHECK("bank 2: TTBR0 and TCR written");
		pg_wr(cb + PG_CB_MAIR0, PG_MAIR0_LINUX);
		pg_wr(cb + PG_CB_MAIR1, 0);
		pg_wr(cb + PG_CB_FSR, 0xffffffffu);	/* one to clear, every reason */
		printf("POLARIS GPU: 10 bank %u: the descriptors are written; invalidating the TLB\n",
		    PG_GMU_CTX);
		PG_CHECK("bank 2: MAIR and FSR written, invalidating the TLB");
		pg_wr(cb + PG_CB_TLBIASID, PG_GMU_CTX);
		pg_wr(cb + PG_CB_TLBSYNC, 1);
		for (int i = 0; i < 100 && (pg_rd(cb + PG_CB_TLBSTATUS) & 1); i++) {
			pg_sleep_us(10);
		}
		printf("POLARIS GPU: 10 context bank %u: CBAR %#010x CBA2R %#x TCR %#010x TCR2 %#010x "
		    "TTBR0 %#010x%08x MAIR0 %#010x TLB status %#x\n", PG_GMU_CTX,
		    pg_rd(pg_smmu + PG_SMMU_CBAR(PG_GMU_CTX)), pg_rd(pg_smmu + PG_SMMU_CBA2R(PG_GMU_CTX)),
		    pg_rd(cb + PG_CB_TCR), pg_rd(cb + PG_CB_TCR2), pg_rd(cb + PG_CB_TTBR0_HI),
		    pg_rd(cb + PG_CB_TTBR0_LO), pg_rd(cb + PG_CB_MAIR0), pg_rd(cb + PG_CB_TLBSTATUS));
		PG_CHECK("bank 2: TLB invalidated, switching the bank on");
		pg_wr(cb + PG_CB_SCTLR, PG_SCTLR_ON);
		if (pg_rd(cb + PG_CB_SCTLR) != PG_SCTLR_ON) {
			printf("POLARIS GPU: 10 the context bank would not switch on\n");
			return false;
		}
	}
	/*
	 * How hard to push, from the boot arg, because one of these may work
	 * where another does not and each costs a reboot to try:
	 *
	 *   polarisgpummu=0  auto: an entry of its own, then let unmatched
	 *                    streams through, then take the SMMU out of the path
	 *   polarisgpummu=1  only the entry
	 *   polarisgpummu=2  only "unmatched streams do not fault"
	 *   polarisgpummu=3  only sCR0.CLIENTPD
	 *
	 * The entry is the right answer and the one Linux uses; the other two are
	 * only here because log 0130 showed the entry write being edited by the
	 * hardware into a fault.
	 */
	{
		bool want_entry = pg_mmu_way == 0 || pg_mmu_way == 1;
		bool want_unmatched = !pg_smmu_mode && (pg_mmu_way == 0 || pg_mmu_way == 2);
		bool want_off = !pg_smmu_mode && (pg_mmu_way == 0 || pg_mmu_way == 3);
		uint32_t smr_back, s2cr_back, scr0, scr0_after;

		if (want_entry) {
			PG_CHECK("the stream table: writing the entry");
			pg_wr(pg_smmu + PG_SMMU_SMR(PG_GMU_SMR_ENTRY), smr);
			pg_wr(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY), s2cr);
			smr_back = pg_rd(pg_smmu + PG_SMMU_SMR(PG_GMU_SMR_ENTRY));
			s2cr_back = pg_rd(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY));
			printf("POLARIS GPU: 10 entry %u: wrote SMR %#010x S2CR %#010x, read back "
			    "SMR %#010x S2CR %#010x\n", PG_GMU_SMR_ENTRY, smr, s2cr, smr_back,
			    s2cr_back);
			if (smr_back == smr && s2cr_back == s2cr) {
				printf("POLARIS GPU: 10 stream %u has %s\n", PG_GMU_STREAM,
				    pg_smmu_mode ? "context bank 2 behind it" : "a bypass entry");
				return true;
			}
			if (pg_smmu_mode || pg_mmu_way == 1) {
				printf("POLARIS GPU: 10 the entry will not take %#010x\n", s2cr);
				return false;
			}
			pg_wr(pg_smmu + PG_SMMU_SMR(PG_GMU_SMR_ENTRY), 0);
		}
		if (want_unmatched) {
			scr0 = pg_rd(pg_smmu + PG_SMMU_SCR0);
			pg_wr(pg_smmu + PG_SMMU_SCR0, scr0 & ~PG_SCR0_USFCFG);
			scr0_after = pg_rd(pg_smmu + PG_SMMU_SCR0);
			printf("POLARIS GPU: 10 stream %u left unmatched, sCR0 %#010x -> %#010x "
			    "(USFCFG cleared: unmatched streams no longer fault)\n", PG_GMU_STREAM,
			    scr0, scr0_after);
			if (!(scr0_after & PG_SCR0_USFCFG)) {
				return true;
			}
		}
		if (want_off) {
			scr0 = pg_rd(pg_smmu + PG_SMMU_SCR0);
			pg_wr(pg_smmu + PG_SMMU_SCR0, scr0 | PG_SCR0_CLIENTPD);
			scr0_after = pg_rd(pg_smmu + PG_SMMU_SCR0);
			printf("POLARIS GPU: 10 sCR0 %#010x -> %#010x (CLIENTPD: the SMMU off the "
			    "path, every stream bypassed)\n", scr0, scr0_after);
			if (scr0_after & PG_SCR0_CLIENTPD) {
				return true;
			}
		}
		printf("POLARIS GPU: 10 nothing would take: sCR0 %#010x entry SMR %#010x S2CR %#010x\n",
		    pg_rd(pg_smmu + PG_SMMU_SCR0), pg_rd(pg_smmu + PG_SMMU_SMR(PG_GMU_SMR_ENTRY)),
		    pg_rd(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY)));
		return false;
	}
}

static bool
pg_step_gmu_boot_config(void)
{
	static const uint32_t ids[PG_GMU_HFI_QUEUES] = { 0, 4, 5 };	/* legacy response/debug ids */
	struct pg_hfi_table *table;
	struct pg_hfi_queue *queue;
	static const struct {
		const char *name;
		uint32_t word;
		uint32_t value;
	} boot[] = {
		{ "GMU_GENERAL_7",			0x51cc, 0x1 },
		{ "GMU_CM3_FW_INIT_RESULT",		0x501c, 0x0 },
		{ "GMU_CM3_BOOT_CONFIG",		0x5001, 0x2 },
		{ "GMU_HFI_QTBL_INFO",			0x5184, 0x1 },
		{ "GMU_AHB_FENCE_RANGE_0",		0x9311, 0x802800a0 },
		{ "GMU_CM3_CFG",			0x502d, 0x4052 },
		{ "GMU_SYS_BUS_CONFIG",			0x4c0f, 0x1 },
		{ "GMU_ICACHE_CONFIG",			0x4c00, 0x1 },
		{ "GMU_DCACHE_CONFIG",			0x4c01, 0x1 },
		{ "GMU_PWR_COL_INTER_FRAME_CTRL",	0x50c0, 0x9c40400 },
		{ "GPU_GMU_CX_GMU_PWR_COL_CP_RESP",	0x5101, 0x0 },
	};
	bool good = true;
	int map = 0;

	if (pg_gmu == 0 || pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 1) {
		printf("POLARIS GPU: 10 the GMU is not mapped or its processor is not held in reset: "
		    "no boot configuration\n");
		return false;
	}
	(void)PE_parse_boot_argn("polarisgpumap", &map, sizeof(map));
	pg_smmu_mode = (map == 1);
	pg_mmu_way = 0;
	pg_mem_way = 0;
	(void)PE_parse_boot_argn("polarisgpummu", &pg_mmu_way, sizeof(pg_mmu_way));
	(void)PE_parse_boot_argn("polarisgpumem", &pg_mem_way, sizeof(pg_mem_way));
	printf("POLARIS GPU: 10 the firmware's regions are %s\n", pg_smmu_mode ?
	    "behind the SMMU's translation (polarisgpumap=1)" :
	    "addressed physically with stream 5 bypassing the SMMU (polarisgpumap=0)");
	PG_CHECK("10: the mode is parsed, nothing has been allocated yet");
	printf("POLARIS GPU: 10 polarisgpummu=%d, polarisgpumem=%d (%s)\n",
	    pg_mmu_way, pg_mem_way, pg_reserved_name());
	PG_CHECK("10: allocating the firmware's log");
	if (!pg_gmu_region(&pg_log_va, &pg_log_pa, "the firmware's log")) {
		return false;
	}
	PG_CHECK("10: the firmware's log is in place");
	PG_CHECK("10: allocating the HFI queue table");
	if (!pg_gmu_region(&pg_hfi_va, &pg_hfi_pa, "the HFI queue table")) {
		return false;
	}
	PG_CHECK("10: the HFI queue table is in place");
	for (uint32_t i = 0; i < PG_GMU_HFI_QUEUES; i++) {
		if (!pg_gmu_region(&pg_queue_va[i], &pg_queue_pa[i], "an HFI queue")) {
			return false;
		}
	}
	PG_CHECK("10: allocating the HFI queues");
	if (pg_smmu_mode) {
		pg_log_iova = PG_GMU_IOVA_LOG;
		pg_hfi_iova = PG_GMU_IOVA_HFI;
		pg_dbg_iova = PG_GMU_IOVA_DEBUG;
		PG_CHECK("10: building the SMMU's tables");
		if (!pg_smmu_map(pg_log_iova, pg_log_va, 1, "the firmware's log") ||
		    !pg_smmu_map(pg_hfi_iova, pg_hfi_va, 1, "the HFI queue table")) {
			return false;
		}
		PG_CHECK("10: the log and the table are mapped");
		for (uint32_t i = 0; i < PG_GMU_HFI_QUEUES; i++) {
			pg_queue_iova[i] = PG_GMU_IOVA_HFI + PG_GMU_PAGE_SIZE * (i + 1ull);
			if (!pg_smmu_map(pg_queue_iova[i], pg_queue_va[i], 1, "an HFI queue")) {
				return false;
			}
		}
	} else {
		pg_log_iova = pg_log_pa;
		pg_hfi_iova = pg_hfi_pa;
		pg_dbg_iova = 0;
		for (uint32_t i = 0; i < PG_GMU_HFI_QUEUES; i++) {
			pg_queue_iova[i] = pg_queue_pa[i];
		}
	}
	PG_CHECK("10: arming the SMMU");
	if (!pg_smmu_arm()) {
		printf("POLARIS GPU: 10 without a stream entry the firmware's next memory read faults: "
		    "stopping here\n");
		return false;
	}
	printf("POLARIS GPU: 10 log %#llx (physical %#llx), HFI table %#llx (%#llx), queues %#llx %#llx %#llx\n",
	    pg_log_iova, pg_log_pa, pg_hfi_iova, pg_hfi_pa,
	    pg_queue_iova[0], pg_queue_iova[1], pg_queue_iova[2]);

	table = (struct pg_hfi_table *)pg_hfi_va;
	queue = (struct pg_hfi_queue *)(pg_hfi_va + sizeof(*table));
	table->version = 0;
	table->size = sizeof(*table) + PG_GMU_HFI_QUEUES * sizeof(*queue);
	table->qhdr0_offset = sizeof(*table) / 4;
	table->qhdr_size = sizeof(*queue) / 4;
	table->num_queues = table->active_queues = PG_GMU_HFI_QUEUES;
	for (uint32_t i = 0; i < PG_GMU_HFI_QUEUES; i++) {
		queue[i].status = 1;
		queue[i].iova = (uint32_t)pg_queue_iova[i];
		queue[i].type = (10u << 8) | ids[i];
		queue[i].size = PG_GMU_PAGE_SIZE / 4;
		queue[i].rx_watermark = queue[i].tx_watermark = queue[i].rx_request = 1;
		printf("POLARIS GPU: 10 HFI queue %u id %u at %#x, capacity %u words\n",
		    i, ids[i], queue[i].iova, queue[i].size);
	}
	pg_to_memory(pg_hfi_va, PG_GMU_PAGE_SIZE);
	pg_to_memory(pg_log_va, PG_GMU_PAGE_SIZE);
	for (uint32_t i = 0; i < PG_GMU_HFI_QUEUES; i++) {
		pg_to_memory(pg_queue_va[i], PG_GMU_PAGE_SIZE);
	}

	/* the three that carry an address the firmware will actually use */
	pg_wr(pg_gmu + PG_GMU_WORD(0x5185), (uint32_t)pg_hfi_iova);		/* HFI_QTBL_ADDR */
	pg_wr(pg_gmu + PG_GMU_WORD(0x5182), PG_GMU_CHIPID);			/* HFI_SFR_ADDR: the chip id */
	pg_wr(pg_gmu + PG_GMU_WORD(0x5100),					/* PWR_COL_CP_MSG */
	    (uint32_t)pg_log_iova | (PG_GMU_PAGE_SIZE / 0x1000u - 1u));		/* the log and its size in 4 KB */
	printf("POLARIS GPU: 10 HFI_QTBL_ADDR %#010x HFI_SFR_ADDR %#010x CP_MSG %#010x (were %#010x %#010x %#010x)\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5185)), pg_rd(pg_gmu + PG_GMU_WORD(0x5182)),
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5100)), (uint32_t)pg_hfi_iova, PG_GMU_CHIPID,
	    (uint32_t)pg_log_iova | (PG_GMU_PAGE_SIZE / 0x1000u - 1u));

	for (unsigned i = 0; i < sizeof(boot) / sizeof(boot[0]); i++) {
		uint32_t old = pg_rd(pg_gmu + PG_GMU_WORD(boot[i].word));
		uint32_t value = boot[i].value, back;

		pg_wr(pg_gmu + PG_GMU_WORD(boot[i].word), value);
		back = pg_rd(pg_gmu + PG_GMU_WORD(boot[i].word));
		printf("POLARIS GPU: 10 %-32s was %#010x wrote %#010x read %#010x%s\n",
		    boot[i].name, old, value, back, back == value ? "" : "  DIFFERENT");
		if (back != value) {
			good = false;
		}
	}
	/* RPMH_CTRL keeps what ABL left and gains the bits Linux sets. */
	{
		uint32_t old = pg_rd(pg_gmu + PG_GMU_WORD(0x50e8));

		pg_wr(pg_gmu + PG_GMU_WORD(0x50e8), old | 0xf11u);
		printf("POLARIS GPU: 10 %-32s was %#010x wrote %#010x read %#010x\n", "GMU_RPMH_CTRL",
		    old, old | 0xf11u, pg_rd(pg_gmu + PG_GMU_WORD(0x50e8)));
	}
	pg_boot_configured = good;
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5185)) != (uint32_t)pg_hfi_iova ||
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5182)) != PG_GMU_CHIPID ||
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5100)) !=
	    ((uint32_t)pg_log_iova | (PG_GMU_PAGE_SIZE / 0x1000u - 1u))) {
		printf("POLARIS GPU: 10 the address registers do not hold what was written: "
		    "the firmware would read elsewhere\n");
		good = pg_boot_configured = false;
	}
	printf("POLARIS GPU: 10 boot configuration %s; SYSRESET %#x INIT_RESULT %#x GX %#010x (not released)\n",
	    good ? "written" : "INCOMPLETE", pg_rd(pg_gmu + PG_GMU_WORD(0x5000)),
	    pg_rd(pg_gmu + PG_GMU_WORD(0x501c)), pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR));
	return good;
}

/* ---- 11. releasing the GMU's processor ------------------------------------------------------------------ */

/*
 * a6xx_gmu_start(): mask both interrupt paths, clear the two result registers,
 * drop the reset, and wait for the firmware to announce itself. The word it
 * announces with depends on its own build, which it leaves in the last dword
 * of its data memory - so that word is read first and decides what to wait
 * for, rather than guessing one and timing out on the other.
 *
 * The processor is put back into reset whatever the outcome: this is a
 * measured release, not a boot. What it answers is whether the 32 KB in step
 * 8 runs at all.
 */
/* Back into reset: nothing stays running unowned. */
static void
pg_gmu_hold(void)
{
	pg_wr(pg_gmu + PG_GMU_WORD(0x5000), 1);
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
pg_gmu_unmask(void)
{
	if (pg_masks_saved) {
		pg_wr(pg_gmu + PG_GMU_WORD(0x9306), pg_old_ao_mask);
		pg_wr(pg_gmu + PG_GMU_WORD(0x5193), pg_old_h2h_mask);
	}
}

/*
 * a6xx_gmu_start(): mask both interrupt paths, clear what an earlier run left
 * in FW_INIT_RESULT and CP_RESP, drop SYSRESET and wait for the firmware to
 * announce itself. The word at DTCM+0xff8 selects which signature to expect;
 * the image this phone carries is a legacy one, so 0xbabeface exactly.
 */
static bool
pg_gmu_release(const char *tag)
{
	uint32_t tcm, mask, want, result;

	tcm = pg_rd(pg_gmu + PG_GMU_DTCM + 4 * PG_DTCM_LAST_WORD);
	if (tcm <= 0x20010004u) {
		mask = 0xffffffffu;
		want = 0xbabefaceu;		/* a legacy firmware */
	} else {
		mask = 0x1ffu;
		want = 0x100u;
	}
	printf("POLARIS GPU: %s last DTCM word %#010x: FW_INIT_RESULT & %#x must become %#x\n", tag, tcm, mask, want);

	pg_old_ao_mask = pg_rd(pg_gmu + PG_GMU_WORD(0x9306));
	pg_old_h2h_mask = pg_rd(pg_gmu + PG_GMU_WORD(0x5193));
	pg_wr(pg_gmu + PG_GMU_WORD(0x9306), ~0u);
	pg_wr(pg_gmu + PG_GMU_WORD(0x5193), ~0u);
	pg_masks_saved = true;
	printf("POLARIS GPU: %s interrupts masked (AO was %#010x, GMU2HOST was %#010x)\n", tag, pg_old_ao_mask,
	    pg_old_h2h_mask);

	pg_wr(pg_gmu + PG_GMU_WORD(0x5101), 0);
	pg_wr(pg_gmu + PG_GMU_WORD(0x501c), 0);
	__asm__ volatile ("dsb sy" ::: "memory");

	printf("POLARIS GPU: %s releasing the reset (SYSRESET 1 -> 0)\n", tag);
	pg_wr(pg_gmu + PG_GMU_WORD(0x5000), 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 200; i++) {
		result = pg_rd(pg_gmu + PG_GMU_WORD(0x501c));
		if ((result & mask) == want) {
			printf("POLARIS GPU: %s firmware init DONE: FW_INIT_RESULT %#010x CP_RESP %#010x "
			    "GENERAL_7 %#x SYSRESET %#x\n", tag, result, pg_rd(pg_gmu + PG_GMU_WORD(0x5101)),
			    pg_rd(pg_gmu + PG_GMU_WORD(0x51cc)), pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
			return true;
		}
		pg_sleep_us(100);
	}
	printf("POLARIS GPU: %s firmware init TIMED OUT after 20 ms: FW_INIT_RESULT %#010x CP_RESP %#010x "
	    "GENERAL_7 %#x SYSRESET %#x\n", tag, result, pg_rd(pg_gmu + PG_GMU_WORD(0x5101)),
	    pg_rd(pg_gmu + PG_GMU_WORD(0x51cc)), pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	return false;
}

static bool
pg_step_gmu_start(bool hold)
{
	bool started;

	if (!pg_boot_configured || pg_gmu == 0) {
		printf("POLARIS GPU: 11 the boot configuration was not written; the processor is not released\n");
		return false;
	}
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 1) {
		printf("POLARIS GPU: 11 the processor is not held in reset; nothing released\n");
		return false;
	}
	started = pg_gmu_release("11");
	if (pg_log_va != 0) {
		const volatile uint32_t *log = (const volatile uint32_t *)pg_log_va;

		printf("POLARIS GPU: 11 log[0..7] = %#010x %#010x %#010x %#010x %#010x %#010x %#010x %#010x\n",
		    log[0], log[1], log[2], log[3], log[4], log[5], log[6], log[7]);
	}
	/*
	 * Step 12 needs the processor still running. Resetting it here and
	 * releasing it again a moment later worked, but there is no reason to
	 * put it through that; when the ladder goes on, it stays up.
	 */
	if (hold) {
		pg_gmu_hold();
		printf("POLARIS GPU: 11 processor %s (SYSRESET %#x)\n",
		    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) == 1 ? "HELD in reset again" : "NOT HALTED",
		    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	} else {
		printf("POLARIS GPU: 11 the processor is left running for the next step (SYSRESET %#x)\n",
		    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	}
	pg_gmu_unmask();
	return started;
}

#define PG_HFI_CTRL_STATUS	0x5180u		/* the firmware's HFI is up when bit 0 reads 1 */
#define PG_HFI_CTRL_INIT	0x5186u
#define PG_HFI_QTBL_INFO	0x5184u
#define PG_H2G_INTR_SET		0x5194u		/* the host kicks the GMU here */
#define PG_G2H_INTR_INFO	0x5192u
#define PG_G2H_INTR_CLR		0x5191u
#define PG_HFI_MSG_INIT		0u
#define PG_HFI_MSG_BW_TABLE	3u
#define PG_HFI_MSG_PERF_TABLE	4u
#define PG_HFI_MSG_TEST		5u
#define PG_HFI_MSG_FW_VERSION	1u
#define PG_HFI_MSG_ERROR	100u
#define PG_HFI_QUEUE_WORDS	1024u

static bool
pg_poll(vm_offset_t reg, uint32_t mask, uint32_t want, uint32_t tries, uint32_t step_us)
{
	for (uint32_t i = 0; i < tries; i++) {
		if ((pg_rd(reg) & mask) == want) {
			return true;
		}
		pg_sleep_us(step_us);
	}
	return (pg_rd(reg) & mask) == want;
}

/* ---- the GX domain: the firmware itself raises the rail, the host only asks --------------------------- */

#define PG_BOOT_SLUMBER_OPTION	0x23f8u		/* 0 = ask for the rail, 1 = back to slumber */
#define PG_GX_VOTE_IDX		0x23f9u
#define PG_MX_VOTE_IDX		0x23fau
#define PG_RPMH_POWER_STATE	0x50ecu
#define PG_SPTPRAC_CTRL		0x81u		/* GMU_GX_SPTPRAC_POWER_CONTROL */
#define PG_SPTPRAC_STATUS	0x50d0u
#define PG_OOB_BOOT_SLUMBER_SET	22u		/* legacy out-of-band bits */
#define PG_OOB_BOOT_SLUMBER_ACK	30u

#define PG_RSCC_OFFSET		0x23000u	/* the RSC block inside the GMU's window */
#define PG_RSCC_REQ		0x9307u		/* GMU_RSCC_CONTROL_REQ */
#define PG_RSCC_ACK		0x9308u
#define PG_RSCC_STATUS0		0x4u		/* GPU_RSCC_RSC_STATUS0_DRV0, word in the RSC block */
#define PG_RSCC_SEQ_BUSY	0x101u

/*
 * a6xx_rpmh_start(): the RSC is the block that turns the firmware's votes into
 * RPMh traffic. Until it is awake the firmware can ring for the GX rail as
 * often as it likes and nothing on the other side acts on it - which is what
 * the previous run showed: the doorbell was rung and GX_GDSCR never moved.
 */
static bool
pg_rsc_wake(void)
{
	vm_offset_t rscc = pg_gmu + PG_RSCC_OFFSET;
	bool ack, idle;

	pg_wr(pg_gmu + PG_GMU_WORD(PG_RSCC_REQ), 1u << 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	ack = pg_poll(pg_gmu + PG_GMU_WORD(PG_RSCC_ACK), 1u << 1, 1u << 1, 100, 100);
	printf("POLARIS GPU: 12 RSC wake: CONTROL_ACK %#010x (bit 1 %s)\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_RSCC_ACK)), ack ? "set" : "NOT set within 10 ms");
	idle = ack && pg_poll(rscc + 4 * PG_RSCC_SEQ_BUSY, ~0u, 0, 100, 100);
	printf("POLARIS GPU: 12 RSC wake: SEQ_BUSY_DRV0 %#010x (%s), RSC_STATUS0_DRV0 %#010x\n",
	    pg_rd(rscc + 4 * PG_RSCC_SEQ_BUSY), idle ? "idle" : "busy or not reached",
	    pg_rd(rscc + 4 * PG_RSCC_STATUS0));
	pg_wr(pg_gmu + PG_GMU_WORD(PG_RSCC_REQ), 0);
	return ack && idle;
}

/*
 * The registers this step needs to explain a failure, all from a6xx_gmu.xml.h
 * so the bit numbers are the firmware's, not a guess:
 *
 *   GMU2HOST_INTR_INFO   0x5192  bit 0 = MSGQ, bit 23 = CM3_FAULT
 *   AO_HOST_INTERRUPT_*  0x9304/0x9305  watchdog, RSCC completion, droop,
 *                                       fence error, DBD wake, AHB bus error
 *   HFI_VERSION_INFO     0x5181  what firmware image is actually running
 */
#define PG_DCVS_PERF_SETTING	0x23fdu
#define PG_RPMH_CTRL		0x50e8u
#define PG_HFI_VERSION_INFO	0x5181u
#define PG_CM3_FW_BUSY		0x501au
#define PG_GENERAL_1		0x51c6u
#define PG_GENERAL_7		0x51ccu
#define PG_PWR_COL_CP_RESP	0x5101u
#define PG_AO_INTR_CLR		0x9304u
#define PG_AO_INTR_STATUS	0x9305u
#define PG_AO_INTR_MASK		0x9306u
#define PG_G2H_INTR_MASK	0x5193u
#define PG_CM3_FAULT		(1u << 23)

/* ---- the RPMh command database: what a vote index means ---------------------------------------------- */

/*
 * The two vote words are indexes into the level lists the RPMh command
 * database carries in SMEM, and the previous run sent 0 to both because
 * nothing on this phone said otherwise - the boot loader leaves them at zero,
 * so there was no value to copy either. The Mi 8 port on the same SoC reads
 * the database instead: its "gfx.lvl" list is the primary level table and
 * "mx.lvl" the secondary one, and a vote is the pair of indexes whose level is
 * at least the frequency the host wants. 64 is the Mi 8 port's default level
 * and is what this step asks for unless `polarisgpulevel` says otherwise; a
 * level of 0 means "the highest entry", which is what a6xx_gmu_gfx_rail_on()
 * asks for.
 *
 * The database sits just below SMEM in a reserved region the device tree calls
 * removed_region@85fc0000 (256 KB, no-map): the same place the Mi 8 found it,
 * which is why the scan starts there rather than trusting one address.
 */
#define PG_CMD_DB_PHYS		0x85fc0000ull
#define PG_CMD_DB_SIZE		0x40000u
#define PG_CMD_DB_MAGIC0	0xdbu		/* db 30 03 0c, the version the port checks */
#define PG_CMD_DB_MAGIC1	0x30u
#define PG_CMD_DB_MAGIC2	0x03u
#define PG_CMD_DB_MAGIC3	0x0cu

struct pg_cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};

struct pg_cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct pg_cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};

struct pg_cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static vm_offset_t pg_cmd_db;

/* One mapping for the whole window: every ml_io_map() keeps its address space
 * for good and this kernel's supply of it is small. */
static bool
pg_cmd_db_map(void)
{
	uint8_t *window;

	if (pg_cmd_db != 0) {
		return true;
	}
	window = (uint8_t *)ml_io_map((vm_offset_t)PG_CMD_DB_PHYS, PG_CMD_DB_SIZE);
	if (window == NULL) {
		printf("POLARIS GPU: 12 the command database window at %#llx did not map\n",
		    PG_CMD_DB_PHYS);
		return false;
	}
	for (uint32_t off = 0; off < PG_CMD_DB_SIZE; off += 0x1000) {
		if (window[off + 4] == PG_CMD_DB_MAGIC0 && window[off + 5] == PG_CMD_DB_MAGIC1 &&
		    window[off + 6] == PG_CMD_DB_MAGIC2 && window[off + 7] == PG_CMD_DB_MAGIC3) {
			pg_cmd_db = (vm_offset_t)window + off;
			printf("POLARIS GPU: 12 the command database is at %#llx (found at +%#x in the "
			    "window), version %#x\n", PG_CMD_DB_PHYS + off, off,
			    *(const volatile uint32_t *)pg_cmd_db);
			return true;
		}
	}
	printf("POLARIS GPU: 12 no command database in %#llx..%#llx\n", PG_CMD_DB_PHYS,
	    PG_CMD_DB_PHYS + PG_CMD_DB_SIZE - 1);
	return false;
}

/* cmd_db_read_aux_data(): the u16 level list of one resource, or NULL. */
static const uint16_t *
pg_cmd_db_levels(const char *name, uint32_t *count)
{
	const struct pg_cmd_db_header *db = (const struct pg_cmd_db_header *)pg_cmd_db;
	char id[8] = { 0 };

	for (unsigned i = 0; i < 7 && name[i] != 0; i++) {
		id[i] = name[i];
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct pg_cmd_db_slave *slave = &db->slave[s];
		const struct pg_cmd_db_entry *entry;

		if (slave->id == 0 || slave->header_offset >= PG_CMD_DB_SIZE - 0x1000 ||
		    slave->count > 512) {
			continue;
		}
		entry = (const struct pg_cmd_db_entry *)(db->data + slave->header_offset);
		for (unsigned i = 0; i < slave->count; i++) {
			bool same = true;

			for (unsigned c = 0; c < 8; c++) {
				if (entry[i].id[c] != id[c]) {
					same = false;
					break;
				}
			}
			if (!same) {
				continue;
			}
			if (entry[i].length < 2 || entry[i].length > 64 ||
			    (uint32_t)slave->data_offset + entry[i].offset + entry[i].length >=
			    PG_CMD_DB_SIZE - 0x1000) {
				printf("POLARIS GPU: 12 \"%s\" has an out of range level list "
				    "(length %u, offset %#x)\n", name, entry[i].length, entry[i].offset);
				return NULL;
			}
			*count = entry[i].length / 2;
			return (const uint16_t *)(db->data + slave->data_offset + entry[i].offset);
		}
	}
	printf("POLARIS GPU: 12 the command database knows no \"%s\"\n", name);
	/* what it does carry, so a wrong stride or a wrong window shows up as itself */
	for (unsigned s = 0; s < 8; s++) {
		const struct pg_cmd_db_slave *slave = &db->slave[s];
		const struct pg_cmd_db_entry *entry;
		char ids[4][9];

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		entry = (const struct pg_cmd_db_entry *)(db->data + slave->header_offset);
		for (unsigned i = 0; i < 4; i++) {
			for (unsigned c = 0; c < 8; c++) {
				uint8_t ch = i < slave->count ? (uint8_t)entry[i].id[c] : 0;

				ids[i][c] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : 0;
			}
			ids[i][8] = 0;
		}
		printf("POLARIS GPU: 12   slave %u id %u count %u: \"%s\" \"%s\" \"%s\" \"%s\"\n",
		    s, slave->id, slave->count, ids[0], ids[1], ids[2], ids[3]);
	}
	return NULL;
}

/*
 * a6xx_gmu_rpmh_arc_votes_init for one level: the primary index is the first
 * entry of gfx.lvl that is at least this level, the secondary index the first
 * entry of mx.lvl that is (or the largest non zero one below it). A level of 0
 * asks for the highest entry instead, which is what the boot path in Linux
 * does.
 */
static bool
pg_gx_votes(uint32_t level, uint32_t *gx_index, uint32_t *mx_index)
{
	const uint16_t *pri, *sec;
	uint32_t pc = 0, sc = 0, pindex = 0, sindex = 0, j;

	if (!pg_cmd_db_map()) {
		return false;
	}
	pri = pg_cmd_db_levels("gfx.lvl", &pc);
	sec = pg_cmd_db_levels("mx.lvl", &sc);
	if (pri == NULL || sec == NULL || pc == 0 || sc == 0) {
		return false;
	}
	printf("POLARIS GPU: 12 gfx.lvl has %u levels and mx.lvl %u; gfx.lvl[0..%u] = %u %u %u %u %u %u\n",
	    pc, sc, pc - 1, pri[0], pc > 1 ? pri[1] : 0, pc > 2 ? pri[2] : 0,
	    pc > 3 ? pri[3] : 0, pc > 4 ? pri[4] : 0, pc > 5 ? pri[5] : 0);
	if (level == 0) {
		pindex = pc - 1;
	} else {
		for (j = 0; j < pc; j++) {
			if (pri[j] >= level) {
				pindex = j;
				break;
			}
		}
		if (j == pc) {
			printf("POLARIS GPU: 12 level %u is above every entry of gfx.lvl (top %u)\n",
			    level, pri[pc - 1]);
			return false;
		}
	}
	for (j = 0; j < sc; j++) {
		if (sec[j] >= pri[pindex]) {
			sindex = j;
			break;
		} else if (sec[j] != 0) {
			sindex = j;
		}
	}
	*gx_index = pindex;
	*mx_index = sindex;
	printf("POLARIS GPU: 12 level %u votes gfx.lvl index %u (%u) and mx.lvl index %u (%u)\n",
	    level == 0 ? pri[pindex] : level, pindex, pri[pindex], sindex, sec[sindex]);
	return true;
}

/* The firmware writes its log as text into the region PWR_COL_CP_MSG names. */
static void
pg_log_text(const char *when, unsigned bytes)
{
	const volatile uint8_t *p = (const volatile uint8_t *)pg_log_va;
	char line[41];
	unsigned col = 0;

	if (pg_log_va == 0) {
		return;
	}
	printf("POLARIS GPU: 12 %s: the firmware's log region, as text\n", when);
	for (unsigned i = 0; i < bytes; i++) {
		uint8_t c = p[i];

		line[col++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		if (col == 40) {
			line[col] = 0;
			printf("POLARIS GPU: 12   |%s|\n", line);
			col = 0;
		}
	}
	if (col != 0) {
		line[col] = 0;
		printf("POLARIS GPU: 12   |%s|\n", line);
	}
}

/*
 * The same two writes the CX half needed in step 3: the delay field, then
 * SW_COLLAPSE cleared, then PWR_ON read back. Linux gives the GX switch a
 * power_on() that does nothing at all (gpucc-sdm845.c: gdsc_gx_do_nothing_enable)
 * because the firmware owns this rail - so this is a probe, not the way in.
 */
static bool
pg_gx_switch_probe(void)
{
	uint32_t v;
	bool on;

	v = pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR);
	pg_wr(pg_gpucc + PG_GPUCC_GX_GDSCR,
	    (v & ~PG_GDSCR_CLK_DIS_WAIT_MASK) | PG_GDSCR_CLK_DIS_WAIT(PG_CX_CLK_DIS_WAIT_VAL));
	v = pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR);
	pg_wr(pg_gpucc + PG_GPUCC_GX_GDSCR, v & ~PG_GDSCR_SW_COLLAPSE);
	pg_sleep_us(1);
	on = pg_poll(pg_gpucc + PG_GPUCC_GX_GDSCR, PG_GDSC_PWR_ON, PG_GDSC_PWR_ON, 100, 50);
	printf("POLARIS GPU: 12 software switch: GX GDSCR %#010x -> %s\n",
	    pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR),
	    on ? "POWER ON, the domain comes up without RPMh" : "still off (the hardware re-collapses it)");
	return on;
}

static void
pg_gx_after(const char *when)
{
	uint32_t ao = pg_rd(pg_gmu + PG_GMU_WORD(PG_AO_INTR_STATUS));
	uint32_t gx = pg_rd(pg_gpucc + PG_GPUCC_GX_GDSCR);

	printf("POLARIS GPU: 12 %s: GX GDSCR %#010x (%s), RPMH_POWER_STATE %#x, SPTPRAC %#x, CP_RESP %#x\n",
	    when, gx, (gx & PG_GDSC_PWR_ON) ? "on" : "off",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_RPMH_POWER_STATE)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_SPTPRAC_STATUS)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_PWR_COL_CP_RESP)));
	printf("POLARIS GPU: 12 %s: AO status %#010x (watchdog %u, RSCC completion %u, droop %u, "
	    "fence error %u, DBD wake %u, host bus error %u)\n", when, ao,
	    (ao >> 0) & 1u, (ao >> 1) & 1u, (ao >> 2) & 1u, (ao >> 3) & 1u, (ao >> 4) & 1u,
	    (ao >> 5) & 1u);
	printf("POLARIS GPU: 12 %s: RSC status0 %#010x seq busy %#010x, PDC enable %#010x\n", when,
	    pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * PG_RSCC_STATUS0),
	    pg_rd(pg_rs_region[PG_RS_RSCC] + 4 * PG_RSCC_SEQ_BUSY),
	    pg_rs_region[PG_RS_PDC] != 0 ?
	    pg_rd(pg_rs_region[PG_RS_PDC] + 4 * PG_PDC_ENABLE) : 0u);
}

/* One request: option, votes, doorbell, and watch both the ack and the fault. */
static bool
pg_gx_request(const char *tag, bool have_votes, uint32_t gx_index, uint32_t mx_index,
    bool *faulted)
{
	uint32_t info;
	bool ack = false, fault = false;

	if (have_votes) {
		pg_wr(pg_gmu + PG_GMU_WORD(PG_GX_VOTE_IDX), gx_index);
		pg_wr(pg_gmu + PG_GMU_WORD(PG_MX_VOTE_IDX), mx_index);
	}
	pg_wr(pg_gmu + PG_GMU_WORD(PG_BOOT_SLUMBER_OPTION), 0);
	printf("POLARIS GPU: 12 %s: BOOT_SLUMBER_OPTION %#x, GX_VOTE_IDX %#x, MX_VOTE_IDX %#x, "
	    "ringing bit %u\n", tag, pg_rd(pg_gmu + PG_GMU_WORD(PG_BOOT_SLUMBER_OPTION)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GX_VOTE_IDX)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_MX_VOTE_IDX)), PG_OOB_BOOT_SLUMBER_SET);
	pg_wr(pg_gmu + PG_GMU_WORD(PG_H2G_INTR_SET), 1u << PG_OOB_BOOT_SLUMBER_SET);
	__asm__ volatile ("dsb sy" ::: "memory");

	/* Ten reads over 100 ms: the answer, or the fault bit, whichever comes. */
	for (unsigned round = 0; round < 10 && !ack && !fault; round++) {
		info = pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO));
		ack = (info & (1u << PG_OOB_BOOT_SLUMBER_ACK)) != 0;
		fault = (info & PG_CM3_FAULT) != 0;
		if (round == 0 || ack || fault) {
			printf("POLARIS GPU: 12 %s: %3u ms after the doorbell: GMU2HOST %#010x%s%s\n",
			    tag, (round + 1) * 10, info, ack ? "  <- ack bit 30" : "",
			    fault ? "  <- CM3_FAULT bit 23" : "");
		}
		if (!ack && !fault) {
			pg_sleep_us(10000);
		}
	}
	info = pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO));
	ack = (info & (1u << PG_OOB_BOOT_SLUMBER_ACK)) != 0;
	fault = (info & PG_CM3_FAULT) != 0;
	pg_wr(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_CLR), 1u << PG_OOB_BOOT_SLUMBER_ACK);
	printf("POLARIS GPU: 12 %s: the doorbell was answered%s\n", tag,
	    ack ? " at bit 30" : (fault ? " with a CM3 fault: the firmware is dead" :
	    " with nothing at all"));
	*faulted = fault;
	pg_gx_after(tag);
	return ack;
}

/*
 * a6xx_gmu_gfx_rail_on(): the legacy firmware raises the GX rail itself. The
 * host names the level in the two vote registers, sets BOOT_SLUMBER_OPTION to 0
 * and rings the doorbell at bit 22; the firmware answers at bit 30 once the
 * rail is up. a6xx_gmu_resume() does exactly this before the HFI is started
 * when the firmware is a legacy one - which this one is (0xbabeface).
 *
 * What the last run established: the boot loader leaves both vote registers at
 * zero, so sending zero was not a deviation from it - it was a copy of it - and
 * the firmware faults on that request (CM3_FAULT twenty milliseconds later,
 * with nothing in the always-on status). The vote indexes are supposed to come
 * from the RPMh command database, and until now this step had no way to read
 * it. It does now: the level lists are read, printed, and turned into the two
 * indexes Linux would send.
 */
static bool
pg_step_gx_rail_on(bool hold)
{
	uint32_t level = 64, gx_index = 0, mx_index = 0;
	bool ack, have_votes, probe, faulted = false;

	if (pg_gmu == 0 || pg_gpucc == 0) {
		printf("POLARIS GPU: 12 no GMU or no GPUCC: the rail is not requested\n");
		return false;
	}
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) != 0) {
		printf("POLARIS GPU: 12 the processor is not running; the rail is not requested\n");
		return false;
	}
	(void)PE_parse_boot_argn("polarisgpulevel", &level, sizeof(level));

	printf("POLARIS GPU: 12 the boot loader left GX_VOTE_IDX %#x, MX_VOTE_IDX %#x, "
	    "BOOT_SLUMBER_OPTION %#x, DCVS_PERF %#x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GX_VOTE_IDX)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_MX_VOTE_IDX)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_BOOT_SLUMBER_OPTION)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_DCVS_PERF_SETTING)));
	printf("POLARIS GPU: 12 RPMH_CTRL %#010x, RPMH_POWER_STATE %#x, HFI version %#010x, "
	    "CM3_FW_BUSY %#x, GENERAL_1 %#x, GENERAL_7 %#x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_RPMH_CTRL)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_RPMH_POWER_STATE)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_HFI_VERSION_INFO)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_CM3_FW_BUSY)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GENERAL_1)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GENERAL_7)));
	printf("POLARIS GPU: 12 before the request: GMU2HOST %#010x, HFI_CTRL_STATUS %#010x, "
	    "AO mask %#010x, GMU2HOST mask %#010x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_STATUS)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_AO_INTR_MASK)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_MASK)));

	/* A clean latch, so a fault bit read after the doorbell is this step's. */
	pg_wr(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_CLR), 0xffffffffu);
	pg_wr(pg_gmu + PG_GMU_WORD(PG_AO_INTR_CLR), 0xffffffffu);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("POLARIS GPU: 12 latches cleared: GMU2HOST %#010x, AO %#010x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_AO_INTR_STATUS)));

	probe = pg_gx_switch_probe();

	if (!pg_rsc_wake()) {
		printf("POLARIS GPU: 12 the RSC did not come awake; the rail is asked for anyway\n");
	}

	have_votes = pg_gx_votes(level, &gx_index, &mx_index);
	ack = pg_gx_request("the level request", have_votes, gx_index, mx_index, &faulted);
	pg_log_text("after the level request", 160);

	/* If the firmware is still alive and simply ignored it, the highest entry
	 * of the level list is the next thing to ask for - it is what Linux asks
	 * for on a cold boot. */
	if (!ack && !faulted) {
		uint32_t high_gx = 0, high_mx = 0;

		if (pg_gx_votes(0, &high_gx, &high_mx)) {
			ack = pg_gx_request("the highest level", true, high_gx, high_mx, &faulted);
		}
	}

	/*
	 * When a signal says nothing, the fault latch does: the AO block records
	 * whether the firmware hung (watchdog), whether the RPMh side completed
	 * (RSCC completion) and whether it walked into a bus it cannot reach.
	 */
	if (!ack) {
		uint32_t ao = pg_rd(pg_gmu + PG_GMU_WORD(PG_AO_INTR_STATUS));

		if (ao & (1u << 5)) {
			printf("POLARIS GPU: 12 the evidence says a host bus error: the firmware "
			    "reached for an address that is not there\n");
		} else if (ao & (1u << 0)) {
			printf("POLARIS GPU: 12 the evidence says a watchdog bite: the firmware is "
			    "alive but waiting for something that never came\n");
		} else if (ao & (1u << 1)) {
			printf("POLARIS GPU: 12 the evidence says the RSC completed a sequence but the "
			    "rail did not report on\n");
		} else if (probe) {
			printf("POLARIS GPU: 12 the switch closed by hand and the firmware still said "
			    "nothing: the request path itself is the problem\n");
		} else if (!have_votes) {
			printf("POLARIS GPU: 12 the level lists could not be read, so the vote indexes "
			    "are still a guess\n");
		} else {
			printf("POLARIS GPU: 12 a real vote, no ack, no AO bit: the request dies "
			    "inside the firmware\n");
		}
	}

	if (ack) {
		/* a6xx_sptprac_enable: the host owns SPTP power collapse at idle level ACTIVE */
		pg_wr(pg_gmu + PG_GMU_WORD(PG_SPTPRAC_CTRL), 0x778000u);
		printf("POLARIS GPU: 12 SPTPRAC enable: status %#x (%s)\n",
		    pg_rd(pg_gmu + PG_GMU_WORD(PG_SPTPRAC_STATUS)),
		    (pg_rd(pg_gmu + PG_GMU_WORD(PG_SPTPRAC_STATUS)) & 0x38) == 0x28 ? "on" : "NOT on");
	}
	if (hold) {
		pg_gmu_hold();
		printf("POLARIS GPU: 12 processor HELD in reset again (SYSRESET %#x)\n",
		    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	} else {
		printf("POLARIS GPU: 12 the processor is left running for the next step "
		    "(SYSRESET %#x)\n", pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	}
	return ack;
}

/* ---- HFI: the host's command queue is 0, the GMU's response queue is 1 ------------------------------ */


static uint32_t pg_hfi_seq;

static volatile struct pg_hfi_queue *
pg_hfi_header(uint32_t q)
{
	return (volatile struct pg_hfi_queue *)(pg_hfi_va + sizeof(struct pg_hfi_table)) + q;
}

static volatile uint32_t *
pg_hfi_data(uint32_t q)
{
	return (volatile uint32_t *)pg_queue_va[q];
}

/* Both the indices in the table page and the words in the queue page have to
 * travel between the two masters by way of memory, so both are cleaned on the
 * way out and invalidated on the way in. */
static void
pg_hfi_sync(uint32_t q)
{
	pg_to_memory(pg_hfi_va, PG_GMU_PAGE_SIZE);
	pg_to_memory(pg_queue_va[q], PG_GMU_PAGE_SIZE);
}

/*
 * a6xx_hfi_send_msg(): a header word - sequence in bits 31:20, length in 15:8,
 * message id in 7:0 - written into queue 0, then one interrupt, then the
 * answer read out of queue 1. The queue indices live in the table in shared
 * memory, so both sides see them; the data area is written before the index
 * that publishes it.
 */
static bool
pg_hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords, uint32_t *payload, uint32_t payload_words)
{
	volatile struct pg_hfi_queue *cq = pg_hfi_header(0), *rq = pg_hfi_header(1);
	uint32_t seq = ++pg_hfi_seq % 0xfff, index, space;

	pg_hfi_sync(0);
	index = cq->write_index;
	space = (cq->read_index - index - 1) & (PG_HFI_QUEUE_WORDS - 1);
	if (space < dwords) {
		printf("POLARIS GPU: 13 HFI command queue full (%u of %u words used)\n",
		    PG_HFI_QUEUE_WORDS - 1 - space, PG_HFI_QUEUE_WORDS - 1);
		return false;
	}
	msg[0] = (seq << 20) | (0u << 16) | (dwords << 8) | id;
	for (uint32_t i = 0; i < dwords; i++) {
		pg_hfi_data(0)[index] = msg[i];
		index = (index + 1) & (PG_HFI_QUEUE_WORDS - 1);
	}
	pg_to_memory(pg_queue_va[0], PG_GMU_PAGE_SIZE);
	cq->write_index = index;
	pg_to_memory(pg_hfi_va, PG_GMU_PAGE_SIZE);
	pg_wr(pg_gmu + PG_GMU_WORD(PG_H2G_INTR_SET), 1);
	__asm__ volatile ("dsb sy" ::: "memory");

	for (unsigned round = 0; round < 3; round++) {
		if (!pg_poll(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO), 1, 1, 10000, 100)) {
			printf("POLARIS GPU: 13 HFI message %u seq %u: no interrupt within 1 s "
			    "(INTR_INFO %#010x, the queue says write %u read %u)\n", id, seq,
			    pg_rd(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_INFO)), cq->write_index, cq->read_index);
			return false;
		}
		pg_wr(pg_gmu + PG_GMU_WORD(PG_G2H_INTR_CLR), 1);
		for (;;) {
			uint32_t resp[19], hdr, n, ri;

			pg_hfi_sync(1);
			if (rq->read_index == rq->write_index) {
				break;
			}
			ri = rq->read_index;
			hdr = pg_hfi_data(1)[ri];
			n = (hdr >> 8) & 0xff;
			if (n == 0 || n > 19) {
				printf("POLARIS GPU: 13 HFI response header %#010x claims %u words\n", hdr, n);
				return false;
			}
			for (uint32_t i = 0; i < n; i++) {
				resp[i] = pg_hfi_data(1)[ri];
				ri = (ri + 1) & (PG_HFI_QUEUE_WORDS - 1);
			}
			rq->read_index = ri;
			pg_to_memory(pg_hfi_va, PG_GMU_PAGE_SIZE);
			if ((hdr & 0xff) == PG_HFI_MSG_ERROR) {
				printf("POLARIS GPU: 13 the firmware reports an error, code %u\n", resp[1]);
				continue;
			}
			if (n < 3 || ((resp[1] >> 20) & 0xfff) != seq) {
				printf("POLARIS GPU: 13 unexpected HFI response (header %#010x, seq %#x): waiting on\n",
				    hdr, n > 1 ? (resp[1] >> 20) & 0xfff : 0);
				continue;
			}
			printf("POLARIS GPU: 13 HFI message %u seq %u answered: error %u%s\n", id, seq, resp[2],
			    n > 3 ? "" : " (no payload word)");
			if (resp[2] != 0) {
				return false;
			}
			if (payload != NULL) {
				for (uint32_t i = 0; i < payload_words && 3 + i < n; i++) {
					payload[i] = resp[3 + i];
				}
			}
			return true;
		}
	}
	printf("POLARIS GPU: 13 HFI message %u: the response queue stayed empty\n", id);
	return false;
}

/*
 * The vote word a level selects: the level itself in bits 31:16, the mx.lvl
 * index in 15:8 and the gfx.lvl (or cx.lvl) index in 7:0. This is the packing
 * the firmware's PERF_TABLE takes, and it is the Mi 8 port's arc_vote() word
 * for word - including the difference from the doorbell above: level 0 selects
 * the first entry, not the last, because level 0 is the table's off row.
 */
static bool
pg_perf_vote(const char *name, uint32_t level, uint32_t *vote)
{
	const uint16_t *pri, *sec;
	uint32_t pc = 0, sc = 0, pindex = 0, sindex = 0, j;

	if (!pg_cmd_db_map()) {
		return false;
	}
	pri = pg_cmd_db_levels(name, &pc);
	sec = pg_cmd_db_levels("mx.lvl", &sc);
	if (pri == NULL || sec == NULL || pc == 0 || sc == 0) {
		return false;
	}
	for (j = 0; j < pc; j++) {
		if (pri[j] >= level) {
			pindex = j;
			break;
		}
	}
	if (j == pc) {
		printf("POLARIS GPU: 13 %s has no entry at or above %u (top %u)\n", name, level, pri[pc - 1]);
		return false;
	}
	for (j = 0; j < sc; j++) {
		if (sec[j] >= pri[pindex]) {
			sindex = j;
			break;
		} else if (sec[j] != 0) {
			sindex = j;
		}
	}
	*vote = ((pri[pindex] & 0xffffu) << 16) | (sindex << 8) | pindex;
	return true;
}

/* a6xx_gmu_hfi_start(): open the channel, then the two messages every a6xx boot sends. */
static bool
pg_step_hfi(void)
{
	uint32_t init[5] = { 0 }, ver[2] = { 0 }, version = 0;
	bool ok = true;

	if (!pg_boot_configured || pg_gmu == 0 || pg_hfi_va == 0) {
		printf("POLARIS GPU: 13 no boot configuration or no HFI table: the channel is not opened\n");
		return false;
	}
	if (pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) == 1) {
		printf("POLARIS GPU: 13 the processor was held; releasing it for the channel\n");
		if (!pg_gmu_release("13")) {
			printf("POLARIS GPU: 13 the firmware did not come up a second time\n");
			pg_gmu_hold();
			pg_gmu_unmask();
			return false;
		}
	} else {
		printf("POLARIS GPU: 13 the processor is still running from the step before\n");
	}

	/* The debug buffer the firmware keeps its own messages in. Allocated before
	 * the channel opens, because asking the page allocator for memory while the
	 * firmware is waiting on its first answer is a good way to lose the answer. */
	if (pg_dbg_va == 0) {
		if (!pg_gmu_region(&pg_dbg_va, &pg_dbg_pa, "the debug buffer")) {
			printf("POLARIS GPU: 13 no region for the debug buffer\n");
			ok = false;
			goto out;
		}
		pg_zero(pg_dbg_va, PG_GMU_PAGE_SIZE);
		pg_to_memory(pg_dbg_va, PG_GMU_PAGE_SIZE);
		if (pg_smmu_mode) {
			if (!pg_smmu_map(pg_dbg_iova, pg_dbg_va, 1, "the debug buffer")) {
				ok = false;
				goto out;
			}
		} else {
			pg_dbg_iova = pg_dbg_pa;
		}
	}
	/* nothing left over from the last two steps may be mistaken for an answer */
	pg_wr(pg_gmu + PG_GMU_WORD(PG_AO_INTR_CLR), 0xffffffffu);
	printf("POLARIS GPU: 13 before HFI_CTRL_INIT: STATUS %#010x INIT %#010x QTBL_INFO %#010x "
	    "QTBL_ADDR %#010x SFR %#010x CP_MSG %#010x debug %#llx AO %#010x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_STATUS)), pg_rd(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_INIT)),
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5184)), pg_rd(pg_gmu + PG_GMU_WORD(0x5185)),
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5182)), pg_rd(pg_gmu + PG_GMU_WORD(0x5100)),
	    pg_dbg_iova, pg_rd(pg_gmu + PG_GMU_WORD(PG_AO_INTR_STATUS)));
	printf("POLARIS GPU: 13 the SMMU has seen: global fault %#010x, stream entry %u SMR %#010x "
	    "S2CR %#010x\n", pg_rd(pg_smmu + PG_SMMU_SGFSR), PG_GMU_SMR_ENTRY,
	    pg_rd(pg_smmu + PG_SMMU_SMR(PG_GMU_SMR_ENTRY)),
	    pg_rd(pg_smmu + PG_SMMU_S2CR(PG_GMU_SMR_ENTRY)));
	printf("POLARIS GPU: 13 writing HFI_CTRL_INIT; if that is the last line, the wedge is in the "
	    "firmware's first read of the table at %#llx\n", pg_hfi_iova);
	PG_CHECK("step 13: about to write HFI_CTRL_INIT");
	pg_wr(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_INIT), 1);
	PG_CHECK("step 13: HFI_CTRL_INIT has been written");
	printf("POLARIS GPU: 13 HFI_CTRL_INIT was written; reading the status back\n");
	ok = pg_poll(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_STATUS), 1, 1, 100, 100);
	printf("POLARIS GPU: 13 HFI_CTRL_INIT: HFI_CTRL_STATUS %#010x (%s)\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_HFI_CTRL_STATUS)), ok ? "the channel is up" : "NOT up");
	if (!ok) {
		goto out;
	}
	init[2] = (uint32_t)pg_dbg_iova;
	init[3] = PG_GMU_PAGE_SIZE;
	init[4] = 1;				/* GMU_COLD_BOOT */
	printf("POLARIS GPU: 13 GMU_INIT: segment %#010x size %#x, cold boot\n", init[2], init[3]);
	ok = pg_hfi_send(PG_HFI_MSG_INIT, init, 5, NULL, 0);
	if (!ok) {
		goto out;
	}
	ver[1] = (1u << 28) | (1u << 19) | (1u << 17);	/* the version a6xx asks for, 1.10 */
	ok = pg_hfi_send(PG_HFI_MSG_FW_VERSION, ver, 2, &version, 1);
	if (ok) {
		printf("POLARIS GPU: 13 the firmware reports HFI version %#x\n", version);
	} else {
		goto out;
	}
	/* a6xx_hfi_send_perf_table_v1(): one row per GPU level and one per GMU level,
	 * each carrying the RPMh vote word that level selects. The GPU's rows use
	 * gfx.lvl and mx.lvl, the GMU's use cx.lvl and mx.lvl. */
	{
		static const struct {
			uint32_t khz, level;
		} gpu_perf[] = {
			{ 0, 0 }, { 257000, 64 }, { 342000, 128 }, { 414000, 192 },
			{ 520000, 256 }, { 596000, 320 }, { 675000, 384 }, { 710000, 416 },
		}, gmu_perf[] = {
			{ 0, 0 }, { 200000, 48 }, { 400000, 128 },
		};
		uint32_t perf[43] = { 0 };
		bool table = true;

		perf[1] = sizeof(gpu_perf) / sizeof(gpu_perf[0]);
		perf[2] = sizeof(gmu_perf) / sizeof(gmu_perf[0]);
		for (uint32_t i = 0; i < perf[1] && table; i++) {
			table = pg_perf_vote("gfx.lvl", gpu_perf[i].level, &perf[3 + 2 * i]);
			perf[4 + 2 * i] = gpu_perf[i].khz;
			printf("POLARIS GPU: 13 perf GX[%u] %u kHz level %u vote %#010x%s\n", i,
			    gpu_perf[i].khz, gpu_perf[i].level, perf[3 + 2 * i], table ? "" : " NO VOTE");
		}
		for (uint32_t i = 0; i < perf[2] && table; i++) {
			table = pg_perf_vote("cx.lvl", gmu_perf[i].level, &perf[35 + 2 * i]);
			perf[36 + 2 * i] = gmu_perf[i].khz;
			printf("POLARIS GPU: 13 perf CX[%u] %u kHz level %u vote %#010x%s\n", i,
			    gmu_perf[i].khz, gmu_perf[i].level, perf[35 + 2 * i], table ? "" : " NO VOTE");
		}
		if (!table) {
			ok = false;
			goto out;
		}
		ok = pg_hfi_send(PG_HFI_MSG_PERF_TABLE, perf, 43, NULL, 0);
		if (!ok) {
			goto out;
		}
	}
	/* a6xx_build_bw_table(): the A630's GMU does not scale the bus, so one level
	 * with three fixed CXC commands and three DDR commands, all waiting on
	 * nothing. Every number below is the Mi 8 port's, message-identical. */
	{
		uint32_t bw[160] = { 0 };

		bw[1] = 1;					/* levels */
		bw[2] = 3;					/* CXC commands */
		bw[3] = 3;					/* DDR commands */
		bw[4] = 0x05;					/* CXC wait bits */
		bw[5] = 0x07;					/* DDR wait bits */
		bw[6] = 0x50034; bw[7] = 0x5007c; bw[8] = 0x5004c;	/* the CXC command registers */
		bw[12] = 0x40000000; bw[14] = 0x40000000;		/* level 0, commands 0..2 */
		bw[18] = 0x60000001; bw[19] = 0x20000001; bw[20] = 0x60000001;
		bw[24] = 0x50000; bw[25] = 0x5005c; bw[26] = 0x5000c;	/* the DDR command registers */
		bw[32] = 0x40000000; bw[33] = 0x40000000; bw[34] = 0x40000000;
		printf("POLARIS GPU: 13 BW_TABLE: %u level, %u CXC and %u DDR commands, no scaling\n",
		    bw[1], bw[2], bw[3]);
		ok = pg_hfi_send(PG_HFI_MSG_BW_TABLE, bw, 160, NULL, 0);
		if (!ok) {
			goto out;
		}
	}
	{
		uint32_t test[1] = { 0 };

		ok = pg_hfi_send(PG_HFI_MSG_TEST, test, 1, NULL, 0);
		if (ok) {
			printf("POLARIS GPU: 13 the firmware has taken INIT, FW_VERSION, PERF_TABLE, "
			    "BW_TABLE and TEST: the channel is a working channel\n");
		}
	}
out:
	if (pg_log_va != 0) {
		const volatile uint32_t *log = (const volatile uint32_t *)pg_log_va;

		printf("POLARIS GPU: 13 log[0..7] = %#010x %#010x %#010x %#010x %#010x %#010x %#010x %#010x\n",
		    log[0], log[1], log[2], log[3], log[4], log[5], log[6], log[7]);
	}
	if (!pg_keep_running) {
		pg_gmu_hold();
	}
	printf("POLARIS GPU: 13 processor %s (SYSRESET %#x)\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)) == 1 ? "HELD in reset again" : "NOT HALTED",
	    pg_rd(pg_gmu + PG_GMU_WORD(0x5000)));
	pg_gmu_unmask();
	return ok;
}

/* ---- step 14: the 3D core's static initialisation ------------------------------------------------ */

/*
 * A630 hw_init, replayed from the Mi 8 port's mi8_a630_hw_init.inc, which is
 * Linux's a6xx_gpu.c hw_init() projected onto SDM845's UBWC profile. The table
 * is those 180 words in the same order and split into the same ranges, because
 * here the order is the part that matters: the address-mode words land before
 * HWCG, HWCG before its commit, and the core defaults after both.
 *
 * Two pieces are the caller's rather than the table's:
 *
 *  - the SECVID registers, which TrustZone lets be written once. The device
 *    tree says qcom,gpu-quirk-secvid-set-once and the port's notes record that
 *    a second write resets the phone, so this reads RBBM_SECVID_TRUST_CNTL
 *    first: 0 means they are set already and they are not touched. This is the
 *    only write in the ladder that can reboot the phone by itself.
 *  - the GMU clock-gating words and GX_SPTPRAC_CLOCK_CONTROL, which are
 *    read-modify-writes wrapped around the HWCG block. HWCG is skipped
 *    entirely when RBBM_CLOCK_CNTL already reads 0x8aa8aa02, as Linux does.
 *
 * PROTECT and the interrupt mask are deliberately not written here: both
 * belong with an interrupt path this kernel does not have yet.
 */
#define PG_GPU3D_PHYS		0x05000000ull	/* dt: reg 0x5000000 0x40000 */
#define PG_GPU3D_SIZE		0x00040000u

#define PG_INIT_ADDRESS_BEGIN		0
#define PG_INIT_ADDRESS_END		11
#define PG_INIT_HWCG_BEGIN		11
#define PG_INIT_HWCG_END		116
#define PG_INIT_HWCG_COMMIT_BEGIN	116
#define PG_INIT_HWCG_COMMIT_END		117
#define PG_INIT_CORE_BEGIN		117
#define PG_INIT_CORE_END		146

#define PG_RBBM_CLOCK_CNTL	0x00aeu
#define PG_SECVID_TRUST_CNTL	0xf400u
#define PG_GMU_AO_CGC_MODE	0x9309u
#define PG_GMU_AO_CGC_DELAY	0x930au
#define PG_GMU_AO_CGC_HYST	0x930bu
#define PG_GMU_GX_SPTPRAC_CLK	0x0080u

static vm_offset_t pg_gpu3d;

/* word = offset in 32-bit words from the 3D core window, as the port stores it */
static const struct {
	uint32_t word;
	uint32_t value;
	const char *name;
} pg_hw_init_table[] = {
	{ 0x00842u, 0x00000001u, "REG_A6XX_CP_ADDR_MODE_CNTL" },
	{ 0x00c01u, 0x00000001u, "REG_A6XX_VSC_ADDR_MODE_CNTL" },
	{ 0x08601u, 0x00000001u, "REG_A6XX_GRAS_ADDR_MODE_CNTL" },
	{ 0x08e05u, 0x00000001u, "REG_A6XX_RB_ADDR_MODE_CNTL" },
	{ 0x09e01u, 0x00000001u, "REG_A6XX_PC_ADDR_MODE_CNTL" },
	{ 0x0be05u, 0x00000001u, "REG_A6XX_HLSQ_ADDR_MODE_CNTL" },
	{ 0x0a601u, 0x00000001u, "REG_A6XX_VFD_ADDR_MODE_CNTL" },
	{ 0x09601u, 0x00000001u, "REG_A6XX_VPC_ADDR_MODE_CNTL" },
	{ 0x00e00u, 0x00000001u, "REG_A6XX_UCHE_ADDR_MODE_CNTL" },
	{ 0x0ae01u, 0x00000001u, "REG_A6XX_SP_ADDR_MODE_CNTL" },
	{ 0x0b601u, 0x00000001u, "REG_A6XX_TPL1_ADDR_MODE_CNTL" },
	{ 0x000b0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP0" },
	{ 0x000b1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP1" },
	{ 0x000b2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP2" },
	{ 0x000b3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP3" },
	{ 0x000b4u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP0" },
	{ 0x000b5u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP1" },
	{ 0x000b6u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP2" },
	{ 0x000b7u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP3" },
	{ 0x000b8u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP0" },
	{ 0x000b9u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP1" },
	{ 0x000bau, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP2" },
	{ 0x000bbu, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP3" },
	{ 0x000bcu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP0" },
	{ 0x000bdu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP1" },
	{ 0x000beu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP2" },
	{ 0x000bfu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP3" },
	{ 0x000c0u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP0" },
	{ 0x000c1u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP1" },
	{ 0x000c2u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP2" },
	{ 0x000c3u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP3" },
	{ 0x000c4u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP0" },
	{ 0x000c5u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP1" },
	{ 0x000c6u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP2" },
	{ 0x000c7u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP3" },
	{ 0x000c8u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP0" },
	{ 0x000c9u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP1" },
	{ 0x000cau, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP2" },
	{ 0x000cbu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP3" },
	{ 0x000ccu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP0" },
	{ 0x000cdu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP1" },
	{ 0x000ceu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP2" },
	{ 0x000cfu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP3" },
	{ 0x000e0u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP0" },
	{ 0x000e1u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP1" },
	{ 0x000e2u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP2" },
	{ 0x000e3u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP3" },
	{ 0x000e4u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP0" },
	{ 0x000e5u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP1" },
	{ 0x000e6u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP2" },
	{ 0x000e7u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP3" },
	{ 0x000e8u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP0" },
	{ 0x000e9u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP1" },
	{ 0x000eau, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP2" },
	{ 0x000ebu, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP3" },
	{ 0x000ecu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP0" },
	{ 0x000edu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP1" },
	{ 0x000eeu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP2" },
	{ 0x000efu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP3" },
	{ 0x000d0u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP0" },
	{ 0x000d1u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP1" },
	{ 0x000d2u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP2" },
	{ 0x000d3u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP3" },
	{ 0x000d4u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP0" },
	{ 0x000d5u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP1" },
	{ 0x000d6u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP2" },
	{ 0x000d7u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP3" },
	{ 0x000d8u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP0" },
	{ 0x000d9u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP1" },
	{ 0x000dau, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP2" },
	{ 0x000dbu, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP3" },
	{ 0x000dcu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP0" },
	{ 0x000ddu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP1" },
	{ 0x000deu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP2" },
	{ 0x000dfu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP3" },
	{ 0x0010bu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_UCHE" },
	{ 0x0010cu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_UCHE" },
	{ 0x0010du, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_UCHE" },
	{ 0x0010eu, 0x00222222u, "REG_A6XX_RBBM_CLOCK_CNTL4_UCHE" },
	{ 0x00110u, 0x00000004u, "REG_A6XX_RBBM_CLOCK_HYST_UCHE" },
	{ 0x0010fu, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_UCHE" },
	{ 0x000f0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB0" },
	{ 0x000f1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB1" },
	{ 0x000f2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB2" },
	{ 0x000f3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB3" },
	{ 0x000f4u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB0" },
	{ 0x000f5u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB1" },
	{ 0x000f6u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB2" },
	{ 0x000f7u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB3" },
	{ 0x000f8u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU0" },
	{ 0x000f9u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU1" },
	{ 0x000fau, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU2" },
	{ 0x000fbu, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU3" },
	{ 0x00100u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU0" },
	{ 0x00101u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU1" },
	{ 0x00102u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU2" },
	{ 0x00103u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU3" },
	{ 0x00104u, 0x05022022u, "REG_A6XX_RBBM_CLOCK_CNTL_RAC" },
	{ 0x00105u, 0x00005555u, "REG_A6XX_RBBM_CLOCK_CNTL2_RAC" },
	{ 0x00106u, 0x00000011u, "REG_A6XX_RBBM_CLOCK_DELAY_RAC" },
	{ 0x00107u, 0x00445044u, "REG_A6XX_RBBM_CLOCK_HYST_RAC" },
	{ 0x00108u, 0x04222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TSE_RAS_RBBM" },
	{ 0x00114u, 0x00222222u, "REG_A6XX_RBBM_CLOCK_MODE_GPC" },
	{ 0x00111u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_VFD" },
	{ 0x0010au, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_TSE_RAS_RBBM" },
	{ 0x00116u, 0x04104004u, "REG_A6XX_RBBM_CLOCK_HYST_GPC" },
	{ 0x00113u, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_VFD" },
	{ 0x0011cu, 0x00000000u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ" },
	{ 0x00109u, 0x00004000u, "REG_A6XX_RBBM_CLOCK_DELAY_TSE_RAS_RBBM" },
	{ 0x00115u, 0x00000200u, "REG_A6XX_RBBM_CLOCK_DELAY_GPC" },
	{ 0x00112u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_DELAY_VFD" },
	{ 0x00117u, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ_2" },
	{ 0x0011bu, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_HLSQ" },
	{ 0x00118u, 0x00000222u, "REG_A6XX_RBBM_CLOCK_CNTL_GMU_GX" },
	{ 0x00119u, 0x00000111u, "REG_A6XX_RBBM_CLOCK_DELAY_GMU_GX" },
	{ 0x0011au, 0x00000555u, "REG_A6XX_RBBM_CLOCK_HYST_GMU_GX" },
	{ 0x000aeu, 0x8aa8aa02u, "REG_A6XX_RBBM_CLOCK_CNTL" },
	{ 0x00010u, 0x00000003u, "REG_A6XX_RBBM_VBIF_CLIENT_QOS_CNTL" },
	{ 0x0302au, 0x00000009u, "REG_A6XX_VBIF_GATE_OFF_WRREQ_EN" },
	{ 0x0050bu, 0xffffffffu, "REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED" },
	{ 0x00e05u, 0xffffffc0u, "REG_A6XX_UCHE_WRITE_RANGE_MAX_LO" },
	{ 0x00e06u, 0x0001ffffu, "REG_A6XX_UCHE_WRITE_RANGE_MAX_HI" },
	{ 0x00e09u, 0xfffff000u, "REG_A6XX_UCHE_TRAP_BASE_LO" },
	{ 0x00e0au, 0x0001ffffu, "REG_A6XX_UCHE_TRAP_BASE_HI" },
	{ 0x00e07u, 0xfffff000u, "REG_A6XX_UCHE_WRITE_THRU_BASE_LO" },
	{ 0x00e08u, 0x0001ffffu, "REG_A6XX_UCHE_WRITE_THRU_BASE_HI" },
	{ 0x00e0bu, 0x00100000u, "REG_A6XX_UCHE_GMEM_RANGE_MIN_LO" },
	{ 0x00e0cu, 0x00000000u, "REG_A6XX_UCHE_GMEM_RANGE_MIN_HI" },
	{ 0x00e0du, 0x001fffffu, "REG_A6XX_UCHE_GMEM_RANGE_MAX_LO" },
	{ 0x00e0eu, 0x00000000u, "REG_A6XX_UCHE_GMEM_RANGE_MAX_HI" },
	{ 0x00e18u, 0x00000804u, "REG_A6XX_UCHE_FILTER_CNTL" },
	{ 0x00e17u, 0x00000004u, "REG_A6XX_UCHE_CACHE_WAYS" },
	{ 0x008c2u, 0x010000c0u, "REG_A6XX_CP_ROQ_THRESHOLDS_2" },
	{ 0x008c1u, 0x8040362cu, "REG_A6XX_CP_ROQ_THRESHOLDS_1" },
	{ 0x008c3u, 0x00000080u, "REG_A6XX_CP_MEM_POOL_SIZE" },
	{ 0x09e00u, 0x00180000u, "REG_A6XX_PC_DBG_ECO_CNTL" },
	{ 0x0098du, 0x00000001u, "REG_A6XX_CP_AHB_CNTL" },
	{ 0x00500u, 0x00000001u, "REG_A6XX_RBBM_PERFCTR_CNTL" },
	{ 0x008d0u, 0x00000000u, "REG_A6XX_CP_PERFCTR_CP_SEL(0)" },
	{ 0x08e08u, 0x00000004u, "REG_A6XX_RB_NC_MODE_CNTL" },
	{ 0x0b604u, 0x00000004u, "REG_A6XX_TPL1_NC_MODE_CNTL" },
	{ 0x0ae02u, 0x00000004u, "REG_A6XX_SP_NC_MODE_CNTL" },
	{ 0x00e01u, 0x00400000u, "REG_A6XX_UCHE_MODE_CNTL" },
	{ 0x00534u, 0x00000000u, "REG_A6XX_RBBM_NC_MODE_CNTL" },
	{ 0x0001fu, 0x401fffffu, "REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL" },
	{ 0x00e19u, 0x00000081u, "REG_A6XX_UCHE_CLIENT_PF" },
	{ 0x0084fu, 0x0000000bu, "REG_A6XX_CP_PROTECT_CNTL" },
	{ 0x00850u, 0x13fc0000u, "REG_A6XX_CP_PROTECT(0)" },
	{ 0x00851u, 0x00140501u, "REG_A6XX_CP_PROTECT(1)" },
	{ 0x00852u, 0x0bd0050bu, "REG_A6XX_CP_PROTECT(2)" },
	{ 0x00853u, 0x8000050eu, "REG_A6XX_CP_PROTECT(3)" },
	{ 0x00854u, 0x80000510u, "REG_A6XX_CP_PROTECT(4)" },
	{ 0x00855u, 0x80000534u, "REG_A6XX_CP_PROTECT(5)" },
	{ 0x00856u, 0x82080800u, "REG_A6XX_CP_PROTECT(6)" },
	{ 0x00857u, 0x802008a0u, "REG_A6XX_CP_PROTECT(7)" },
	{ 0x00858u, 0x809008abu, "REG_A6XX_CP_PROTECT(8)" },
	{ 0x00859u, 0x02b808deu, "REG_A6XX_CP_PROTECT(9)" },
	{ 0x0085au, 0x81340900u, "REG_A6XX_CP_PROTECT(10)" },
	{ 0x0085bu, 0x89c8098du, "REG_A6XX_CP_PROTECT(11)" },
	{ 0x0085cu, 0x80040e00u, "REG_A6XX_CP_PROTECT(12)" },
	{ 0x0085du, 0x80300e03u, "REG_A6XX_CP_PROTECT(13)" },
	{ 0x0085eu, 0x830c3c00u, "REG_A6XX_CP_PROTECT(14)" },
	{ 0x0085fu, 0x7ffc3cc4u, "REG_A6XX_CP_PROTECT(15)" },
	{ 0x00860u, 0x873c8630u, "REG_A6XX_CP_PROTECT(16)" },
	{ 0x00861u, 0x80008e00u, "REG_A6XX_CP_PROTECT(17)" },
	{ 0x00862u, 0x80008e08u, "REG_A6XX_CP_PROTECT(18)" },
	{ 0x00863u, 0x807c8e50u, "REG_A6XX_CP_PROTECT(19)" },
	{ 0x00864u, 0x876c9624u, "REG_A6XX_CP_PROTECT(20)" },
	{ 0x00865u, 0x80049e70u, "REG_A6XX_CP_PROTECT(21)" },
	{ 0x00866u, 0x861c9e78u, "REG_A6XX_CP_PROTECT(22)" },
	{ 0x00867u, 0x873ca630u, "REG_A6XX_CP_PROTECT(23)" },
	{ 0x00868u, 0x8000ae02u, "REG_A6XX_CP_PROTECT(24)" },
	{ 0x00869u, 0x8cbcae50u, "REG_A6XX_CP_PROTECT(25)" },
	{ 0x0086au, 0x8000b604u, "REG_A6XX_CP_PROTECT(26)" },
	{ 0x0086bu, 0x8004be02u, "REG_A6XX_CP_PROTECT(27)" },
	{ 0x0086cu, 0xdf7cbe20u, "REG_A6XX_CP_PROTECT(28)" },
	{ 0x0086du, 0xaffcf000u, "REG_A6XX_CP_PROTECT(29)" },
	{ 0x0086eu, 0x7ffcfc00u, "REG_A6XX_CP_PROTECT(30)" },
	{ 0x0086fu, 0x80011c00u, "REG_A6XX_CP_PROTECT(31)" },
	{ 0x00038u, 0x03d0e242u, "REG_A6XX_RBBM_INT_0_MASK" },
};

static const struct {
	uint32_t word;
	uint32_t value;
	const char *name;
} pg_security_table[] = {
	{ 0x0f803u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_CNTL" },
	{ 0x0f800u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE_LO" },
	{ 0x0f801u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE_HI" },
	{ 0x0f802u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE" },
	{ 0x0f810u, 0x00000001u, "REG_A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL" },
};

static uint32_t
pg_hw_write(uint32_t word, uint32_t value, const char *name)
{
	uint32_t back;

	pg_wr(pg_gpu3d + 4u * word, value);
	__asm__ volatile ("dsb sy" ::: "memory");
	back = pg_rd(pg_gpu3d + 4u * word);
	if (back != value) {
		printf("POLARIS GPU: 14 %s (word %#x): wrote %#010x read %#010x\n", name, word,
		    value, back);
		return 1;
	}
	return 0;
}

static uint32_t
pg_hw_range(const char *what, uint32_t first, uint32_t end)
{
	uint32_t differs = 0;

	for (uint32_t i = first; i < end; i++) {
		differs += pg_hw_write(pg_hw_init_table[i].word, pg_hw_init_table[i].value,
		    pg_hw_init_table[i].name);
	}
	printf("POLARIS GPU: 14 %s: %u words, %u read back different\n", what, end - first,
	    differs);
	return differs;
}

static bool
pg_step_hw_init(void)
{
	uint32_t trust, clock_cntl, differs = 0;
	bool set_once;

	if (pg_gpu3d == 0) {
		pg_gpu3d = ml_io_map(PG_GPU3D_PHYS, PG_GPU3D_SIZE);
	}
	if (pg_gpu3d == 0) {
		printf("POLARIS GPU: 14 the 3D core window did not map\n");
		return false;
	}
	PG_CHECK("14: the 3D core is mapped");
	trust = pg_rd(pg_gpu3d + 4u * PG_SECVID_TRUST_CNTL);
	set_once = (trust == 0);
	printf("POLARIS GPU: 14 RBBM_SECVID_TRUST_CNTL %#010x: %s\n", trust,
	    set_once ? "already set (set-once): not written again" :
	    "not set: writing the trusted buffer registers once");
	if (!set_once) {
		for (unsigned i = 0; i < 4; i++) {
			differs += pg_hw_write(pg_security_table[i].word, pg_security_table[i].value,
			    pg_security_table[i].name);
		}
	}
	differs += pg_hw_range("ADDRESS", PG_INIT_ADDRESS_BEGIN, PG_INIT_ADDRESS_END);
	if (!set_once) {
		differs += pg_hw_write(pg_security_table[4].word, pg_security_table[4].value,
		    pg_security_table[4].name);
	}
	PG_CHECK("14: the address mode words are in");
	/* a6xx_set_hwcg: the GMU's clock gating mode, delay and hysteresis */
	pg_wr(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_MODE), 0x20202u);
	pg_wr(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_DELAY), 0x10111u);
	pg_wr(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_HYST), 0x5555u);
	printf("POLARIS GPU: 14 GMU clock gating %#x/%#x/%#x\n",
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_MODE)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_DELAY)),
	    pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_AO_CGC_HYST)));
	clock_cntl = pg_rd(pg_gpu3d + 4u * PG_RBBM_CLOCK_CNTL);
	printf("POLARIS GPU: 14 RBBM_CLOCK_CNTL %#010x: %s\n", clock_cntl,
	    clock_cntl == 0x8aa8aa02u ? "clock gating is on already: HWCG skipped" :
	    "cold state: writing HWCG and its commit");
	if (clock_cntl != 0x8aa8aa02u) {
		uint32_t v = pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_GX_SPTPRAC_CLK));

		printf("POLARIS GPU: 14 GX_SPTPRAC_CLOCK_CONTROL %#x: clearing bit 0\n", v);
		pg_wr(pg_gmu + PG_GMU_WORD(PG_GMU_GX_SPTPRAC_CLK), v & ~1u);
		differs += pg_hw_range("HWCG", PG_INIT_HWCG_BEGIN, PG_INIT_HWCG_END);
		pg_wr(pg_gmu + PG_GMU_WORD(PG_GMU_GX_SPTPRAC_CLK),
		    pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_GX_SPTPRAC_CLK)) | 1u);
		printf("POLARIS GPU: 14 GX_SPTPRAC_CLOCK_CONTROL now %#x\n",
		    pg_rd(pg_gmu + PG_GMU_WORD(PG_GMU_GX_SPTPRAC_CLK)));
		differs += pg_hw_range("HWCG_COMMIT", PG_INIT_HWCG_COMMIT_BEGIN,
		    PG_INIT_HWCG_COMMIT_END);
	}
	PG_CHECK("14: HWCG is done, the core defaults next");
	differs += pg_hw_range("CORE", PG_INIT_CORE_BEGIN, PG_INIT_CORE_END);
	printf("POLARIS GPU: 14 the 3D core has the table: %u of 146 words read back different\n",
	    differs);
	return true;
}

/* ---- step 15: the CP's microcode, its ring, and the first packet ------------------------------- */

/*
 * a6xx's CP start, in the order the Mi 8 port proved on this SoC: halt the SQE,
 * point it at its instruction memory, point CP_RB at the ring, hand the ring its
 * first packet (CP_ME_INIT), release the SQE, and watch CP_RB_RPTR walk to the
 * write pointer. Nothing here touches zap: until the zap shader has been
 * authenticated the CP runs the 3D core in secure mode, and that is step 16.
 *
 * Both regions are read by the 3D core through the same SMMU context bank the
 * GMU's queues use, which is why the page tables grew a second 2 MB window at
 * 0x10000000. Each page is allocated and mapped on its own: the ring only has
 * to be contiguous in IOVA, never in physical memory.
 */
#define PG_CP_REGION		0x8000u		/* 32 KB each, MSM_GPU_RINGBUFFER_SZ */
#define PG_CP_PAGES		8u		/* 8 x 4 KB */
#define PG_CP_PAGE_WORDS	(PG_GMU_PAGE_SIZE / 4u)

/* 3D core words */
#define PG_CP_SQE_CNTL		0x0808u
#define PG_CP_RB_RPTR		0x0806u
#define PG_CP_RB_WPTR		0x0807u
#define PG_CP_RB_BASE_LO	0x0800u
#define PG_CP_RB_BASE_HI	0x0801u
#define PG_CP_RB_CNTL		0x0802u
#define PG_CP_SQE_INSTR_LO	0x0830u
#define PG_CP_SQE_INSTR_HI	0x0831u
#define PG_RBBM_STATUS		0x0210u
#define PG_RBBM_INT_STATUS	0x0201u
#define PG_CP_ME_INIT		0x48u

static vm_offset_t pg_sqe_va[PG_CP_PAGES], pg_ring_va[PG_CP_PAGES];

/* PM4 type-7 header: the two parity bits are computed, never guessed */
static uint32_t
pg_pm4_parity(uint32_t val)
{
	return (0x9669u >> (0xfu & (val ^ (val >> 4) ^ (val >> 8) ^ (val >> 12) ^ (val >> 16) ^
	    (val >> 20) ^ (val >> 24) ^ (val >> 28)))) & 1u;
}

static uint32_t
pg_pkt7(uint32_t opcode, uint32_t count)
{
	return 0x70000000u | count | (pg_pm4_parity(count) << 15) |
	    ((opcode & 0x7fu) << 16) | (pg_pm4_parity(opcode) << 23);
}

static bool
pg_map_phys(uint64_t iova, uint64_t pa, const char *name)
{
	uint64_t *l3 = (uint64_t *)pg_smmu_l3(iova);

	if (l3 == NULL) {
		printf("POLARIS GPU: 15 %s at %#llx has no table\n", name, iova);
		return false;
	}
	l3[(iova >> 12) & 0x1ff] = (pa & PG_PTE_ADDR) | PG_PTE_ATTR | PG_PTE_PAGE;
	return true;
}

static bool
pg_cp_pages(vm_offset_t *pages, const char *name)
{
	for (uint32_t i = 0; i < PG_CP_PAGES; i++) {
		if (pages[i] != 0) {
			continue;
		}
		if (kmem_alloc(kernel_map, &pages[i], PG_GMU_PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("POLARIS GPU: 15 no memory for %s page %u\n", name, i);
			return false;
		}
	}
	return true;
}

static bool
pg_step_cp(void)
{
	static const uint32_t me_init[8] = { 0x2fu, 0x3u, 0x20000000u, 0, 0, 0, 0, 0 };
	uint32_t *ring = NULL;
	uint32_t rptr = 0, wptr = 0;
	bool done = false;

	if (pg_gpu3d == 0 || !pg_smmu_tables()) {
		printf("POLARIS GPU: 15 the 3D core or the tables are not there\n");
		return false;
	}
	if (!pg_cp_pages(pg_sqe_va, "the SQE microcode") ||
	    !pg_cp_pages(pg_ring_va, "the ring")) {
		return false;
	}
	PG_CHECK("15: the SQE and the ring are allocated");
	for (uint32_t i = 0; i < (uint32_t)POLARIS_A630_SQE_WORDS; i++) {
		((uint32_t *)pg_sqe_va[i / PG_CP_PAGE_WORDS])[i % PG_CP_PAGE_WORDS] =
		    polaris_a630_sqe_fw[i];
	}
	for (uint32_t i = 0; i < PG_CP_PAGES; i++) {
		pg_to_memory(pg_sqe_va[i], PG_GMU_PAGE_SIZE);
	}
	printf("POLARIS GPU: 15 %u words of SQE microcode in %u pages, first words %#010x %#010x\n",
	    (uint32_t)POLARIS_A630_SQE_WORDS, PG_CP_PAGES,
	    ((uint32_t *)pg_sqe_va[0])[0], ((uint32_t *)pg_sqe_va[0])[1]);
	PG_CHECK("15: the microcode is in place");
	for (uint32_t i = 0; i < PG_CP_PAGES; i++) {
		if (!pg_map_phys(PG_CP_SQE_IOVA + (uint64_t)i * PG_GMU_PAGE_SIZE,
		    kvtophys(pg_sqe_va[i]), "the SQE microcode") ||
		    !pg_map_phys(PG_CP_RING_IOVA + (uint64_t)i * PG_GMU_PAGE_SIZE,
		    kvtophys(pg_ring_va[i]), "the ring")) {
			return false;
		}
	}
	pg_to_memory(pg_l3b_va, PG_CP_WINDOW_L2S * PG_GMU_PAGE_SIZE);
	{
		uint64_t *l3 = (uint64_t *)pg_smmu_l3(PG_CP_SQE_IOVA);

		printf("POLARIS GPU: 15 mapped %#llx-%#llx and %#llx-%#llx in the CP's window\n",
		    PG_CP_SQE_IOVA, PG_CP_SQE_IOVA + PG_CP_REGION - 1,
		    PG_CP_RING_IOVA, PG_CP_RING_IOVA + PG_CP_REGION - 1);
		printf("POLARIS GPU: 15 tables: L1[0] %#llx L1[1] %#llx L2[%#x] %#llx L2[%#x] %#llx\n",
		    ((uint64_t *)pg_l1_va)[0], ((uint64_t *)pg_l1_va)[1],
		    PG_CP_WINDOW_L2, ((uint64_t *)pg_l2_va)[PG_CP_WINDOW_L2],
		    (uint32_t)((PG_GMU_IOVA_BASE >> 21) & 0x1ff),
		    ((uint64_t *)pg_l2_va)[(PG_GMU_IOVA_BASE >> 21) & 0x1ff]);
		printf("POLARIS GPU: 15 SQE slot 0 %#llx -> page %#llx, slot 8 %#llx -> %#llx\n",
		    l3[0], kvtophys(pg_sqe_va[0]) & PG_PTE_ADDR, l3[8],
		    kvtophys(pg_ring_va[0]) & PG_PTE_ADDR);
	}
	PG_CHECK("15: both regions are mapped");
	pg_wr(pg_smmu + PG_SMMU_SMR(1), PG_SMR_VALID);
	pg_wr(pg_smmu + PG_SMMU_S2CR(1),
	    PG_GMU_CTX | (PG_S2CR_TRANS << PG_S2CR_TYPE_SHIFT));
	printf("POLARIS GPU: 15 stream 0 (the 3D core): SMR %#010x S2CR %#010x, global fault %#010x\n",
	    pg_rd(pg_smmu + PG_SMMU_SMR(1)), pg_rd(pg_smmu + PG_SMMU_S2CR(1)),
	    pg_rd(pg_smmu + PG_SMMU_SGFSR));
	PG_CHECK("15: the 3D core's stream is bound");
	/*
	 * GX registers survive a slumber, so a CP left running by an earlier boot
	 * is halted before its pointers are replaced.
	 */
	printf("POLARIS GPU: 15 CP_SQE_CNTL was %#x, RB_RPTR %u WPTR %u: halting it\n",
	    pg_rd(pg_gpu3d + 4u * PG_CP_SQE_CNTL), pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR),
	    pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR));
	pg_wr(pg_gpu3d + 4u * PG_CP_SQE_CNTL, 0);
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, 0);
	pg_wr(pg_gpu3d + 4u * PG_CP_SQE_INSTR_LO, (uint32_t)PG_CP_SQE_IOVA);
	pg_wr(pg_gpu3d + 4u * PG_CP_SQE_INSTR_HI, (uint32_t)(PG_CP_SQE_IOVA >> 32));
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_BASE_LO, (uint32_t)PG_CP_RING_IOVA);
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_BASE_HI, (uint32_t)(PG_CP_RING_IOVA >> 32));
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_CNTL, 12u | (2u << 8) | (1u << 27));
	printf("POLARIS GPU: 15 CP: SQE_INSTR_BASE %#010x%08x RB_BASE %#010x%08x RB_CNTL %#010x\n",
	    pg_rd(pg_gpu3d + 4u * PG_CP_SQE_INSTR_HI), pg_rd(pg_gpu3d + 4u * PG_CP_SQE_INSTR_LO),
	    pg_rd(pg_gpu3d + 4u * PG_CP_RB_BASE_HI), pg_rd(pg_gpu3d + 4u * PG_CP_RB_BASE_LO),
	    pg_rd(pg_gpu3d + 4u * PG_CP_RB_CNTL));
	PG_CHECK("15: the CP's pointers are set");
	ring = (uint32_t *)pg_ring_va[0];
	ring[wptr++] = pg_pkt7(PG_CP_ME_INIT, 8);
	for (uint32_t i = 0; i < 8; i++) {
		ring[wptr++] = me_init[i];
	}
	pg_to_memory(pg_ring_va[0], PG_GMU_PAGE_SIZE);
	printf("POLARIS GPU: 15 ring: %u words, %#010x %#010x %#010x; releasing the SQE\n", wptr,
	    ring[0], ring[1], ring[2]);
	pg_wr(pg_gpu3d + 4u * PG_CP_SQE_CNTL, 1);
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
	PG_CHECK("15: CP_ME_INIT is in the ring and the SQE is released");
	for (uint32_t i = 0; i < 500; i++) {
		rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
		if (rptr == wptr) {
			done = true;
			break;
		}
		pg_sleep_us(100);
	}
	printf("POLARIS GPU: 15 the CP %s the ring: RPTR %u WPTR %u, RBBM_STATUS %#010x "
	    "INT_0_STATUS %#010x\n", done ? "consumed" : "did NOT consume", rptr,
	    pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR), pg_rd(pg_gpu3d + 4u * PG_RBBM_STATUS),
	    pg_rd(pg_gpu3d + 4u * PG_RBBM_INT_STATUS));
	{
		vm_offset_t cb = pg_smmu_cb(PG_GMU_CTX);

		printf("POLARIS GPU: 15 context bank %u: FSR %#010x FSYNR0 %#010x FAR %#010x%08x, "
		    "global fault %#010x\n", PG_GMU_CTX, pg_rd(cb + PG_CB_FSR),
		    pg_rd(cb + 0x5c), pg_rd(cb + 0x64), pg_rd(cb + 0x60),
		    pg_rd(pg_smmu + PG_SMMU_SGFSR));
	}
	return done;
}

/* ---- step 16: the zap shader, the CP out of secure mode, and its first write -------------------- */

/*
 * Linux loads a "zap" shader through TrustZone before the 3D core will leave
 * secure mode: qcom_mdt_load() -> qcom_scm_pas_init_image / pas_mem_setup /
 * pas_auth_and_reset, with the .mdt metadata and the one loadable segment in
 * physically contiguous memory. Then the CP is told CP_SET_SECURE_MODE(0), which
 * is the point at which the core stops refusing to touch memory.
 *
 * The Mi 8 port proved this on the same SoC and the same files (mdt 6684 bytes,
 * b02 0x7b0 bytes, PAS id 13, region gpu_region@0x8c415000). The SCM convention
 * is standard SMC64 with the SIP owner; arginfo is QCOM_SCM_ARGS(2, VAL, RW) for
 * PAS_INIT_IMAGE and ARGS(3)/ARGS(1) for the rest.
 *
 * This is the first step that hands TrustZone memory, so it is the first step
 * that could be refused by something outside the kernel. Everything it does is
 * printed, and nothing is assumed.
 */
#define PG_SCM_64		(1ull << 30)
#define PG_SCM_SIP		(2ull << 24)
#define PG_SCM_SUPPORTED	0x207ull
#define PG_PAS_INIT_IMAGE	0x201ull
#define PG_PAS_MEM_SETUP	0x202ull
#define PG_PAS_AUTH_AND_RESET	0x205ull
#define PG_PAS_SHUTDOWN		0x206ull
#define PG_ZAP_PAS_ID		13u
#define PG_ZAP_REGION_PHYS	0x8c415000ul	/* dt: reserved-memory/gpu_region@0x8c415000 */
#define PG_ZAP_REGION_SIZE	0x2000u
#define PG_CP_SET_SECURE_MODE	0x66u
#define PG_CP_MEM_WRITE		0x3du
#define PG_MEMPTRS_IOVA		0x10010000ull

/* 8 x 4 KB of ring, which is what RB_CNTL below was set up for in step 15 */
#define PG_RING_WORDS		(PG_CP_PAGES * 1024u)
#define PG_RING_PAGE_WORDS	1024u

static vm_offset_t pg_memptrs_va;
static uint32_t pg_cp_wptr = 9u;	/* carried from step 15's CP_ME_INIT to later steps */

static int64_t
pg_scm_call(uint64_t function, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t res[3])
{
	uint64_t session = 0;

	for (unsigned i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i ? 1 : function;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = a0;
		register uint64_t x3 __asm__("x3") = a1;
		register uint64_t x4 __asm__("x4") = a2;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
		    "+r" (x5), "+r" (x6), "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14",
		    "x15", "x16", "x17", "memory", "cc");
		session = x6;
		if (x0 != 1) {
			res[0] = x1;
			res[1] = x2;
			res[2] = x3;
			return (int32_t)x0;
		}
	}
	return -1000;
}

static bool
pg_zap_load(void)
{
	const uint64_t how = PG_SCM_64 | PG_SCM_SIP;
	uint64_t res[3], pa = 0;
	vm_offset_t meta = 0, region = 0;
	vm_size_t meta_size = round_page(POLARIS_A630_ZAP_MDT_WORDS * 4u);
	int64_t rc;
	uint32_t wrong = 0;
	bool shutdown = false, done = false;

	PG_CHECK("16: asking TrustZone whether PAS 13 is supported");
	rc = pg_scm_call(how | PG_SCM_SUPPORTED, 1, PG_ZAP_PAS_ID, 0, 0, res);
	printf("POLARIS GPU: 16 PAS_SUPPORTED(13): rc %lld result %#llx\n", rc, res[0]);
	if (rc != 0 || res[0] != 1) {
		printf("POLARIS GPU: 16 TrustZone does not support PAS 13: no zap\n");
		return false;
	}
	/*
	 * Physically contiguous and 4 KB aligned, the way dma_alloc_coherent gives
	 * it to Linux. With 16 KB pages a single page is both by construction.
	 */
	if (kmem_alloc_contig(kernel_map, &meta, meta_size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("POLARIS GPU: 16 no physically contiguous buffer for the zap metadata\n");
		return false;
	}
	pa = kvtophys(meta);
	printf("POLARIS GPU: 16 the zap metadata: %u bytes at %#llx (%s)\n",
	    (uint32_t)(POLARIS_A630_ZAP_MDT_WORDS * 4u), pa,
	    (pa & 0xfff) == 0 && kvtophys(meta + meta_size - 1) == pa + meta_size - 1 ?
	    "contiguous and 4 KB aligned" : "NOT contiguous or not aligned");
	for (uint32_t i = 0; i < POLARIS_A630_ZAP_MDT_WORDS; i++) {
		((uint32_t *)meta)[i] = polaris_a630_zap_mdt[i];
	}
	pg_to_memory(meta, meta_size);
	PG_CHECK("16: handing TrustZone the metadata (PAS_INIT_IMAGE)");
	rc = pg_scm_call(how | PG_PAS_INIT_IMAGE, 0x82, PG_ZAP_PAS_ID, pa, 0, res);
	printf("POLARIS GPU: 16 PAS_INIT_IMAGE(13): rc %lld result %#llx %#llx %#llx\n", rc,
	    res[0], res[1], res[2]);
	if (rc != 0 || res[0] != 0) {
		return false;
	}
	shutdown = true;
	PG_CHECK("16: PAS_MEM_SETUP for the reserved region");
	rc = pg_scm_call(how | PG_PAS_MEM_SETUP, 3, PG_ZAP_PAS_ID, PG_ZAP_REGION_PHYS, 0x1000, res);
	printf("POLARIS GPU: 16 PAS_MEM_SETUP(13, %#lx, 0x1000): rc %lld result %#llx\n",
	    (unsigned long)PG_ZAP_REGION_PHYS, rc, res[0]);
	if (rc != 0 || res[0] != 0) {
		goto out;
	}
	region = ml_io_map(PG_ZAP_REGION_PHYS, PG_ZAP_REGION_SIZE);
	if (region == 0) {
		printf("POLARIS GPU: 16 the reserved region did not map\n");
		goto out;
	}
	for (uint32_t i = 0; i < POLARIS_A630_ZAP_B02_WORDS; i++) {
		pg_wr(region + 4u * i, polaris_a630_zap_b02[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < POLARIS_A630_ZAP_B02_WORDS; i++) {
		wrong += pg_rd(region + 4u * i) != polaris_a630_zap_b02[i];
	}
	printf("POLARIS GPU: 16 the zap segment (%u bytes) in the reserved region: "
	    "%u words read back wrong\n", POLARIS_A630_ZAP_B02_WORDS * 4u, wrong);
	if (wrong != 0) {
		goto out;
	}
	PG_CHECK("16: PAS_AUTH_AND_RESET");
	rc = pg_scm_call(how | PG_PAS_AUTH_AND_RESET, 1, PG_ZAP_PAS_ID, 0, 0, res);
	printf("POLARIS GPU: 16 PAS_AUTH_AND_RESET(13): rc %lld result %#llx %#llx %#llx -- %s\n",
	    rc, res[0], res[1], res[2], (rc == 0 && res[0] == 0) ? "ZAP AUTHENTICATED" : "refused");
	done = rc == 0 && res[0] == 0;
out:
	if (shutdown && !done) {
		rc = pg_scm_call(how | PG_PAS_SHUTDOWN, 1, PG_ZAP_PAS_ID, 0, 0, res);
		printf("POLARIS GPU: 16 PAS_SHUTDOWN(13): rc %lld result %#llx\n", rc, res[0]);
	}
	return done;
}

static bool
pg_step_zap(void)
{
	uint32_t *ring = (uint32_t *)pg_ring_va[0];
	uint32_t wptr = 9, rptr = 0;
	uint32_t trust;
	bool nonsecure = false, ok = false;

	if (pg_gpu3d == 0 || pg_ring_va[0] == 0) {
		printf("POLARIS GPU: 16 the CP is not running: no zap\n");
		return false;
	}
	if (pg_memptrs_va == 0) {
		if (kmem_alloc(kernel_map, &pg_memptrs_va, PG_GMU_PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("POLARIS GPU: 16 no memory for the memptrs page\n");
			return false;
		}
		if (!pg_map_phys(PG_MEMPTRS_IOVA, kvtophys(pg_memptrs_va), "the memptrs page")) {
			return false;
		}
		pg_to_memory(pg_l3b_va, PG_CP_WINDOW_L2S * PG_GMU_PAGE_SIZE);
	}
	trust = pg_rd(pg_gpu3d + 4u * PG_SECVID_TRUST_CNTL);
	printf("POLARIS GPU: 16 RBBM_SECVID_TRUST_CNTL %#010x: %s\n", trust,
	    trust == 0 ? "already out of secure mode" : "still secure: the zap would be the way out");
	/*
	 * 0143 stopped here: TrustZone answered PAS_SUPPORTED(13) with 0, the zap
	 * never loaded, and the ladder returned before the CP had written a single
	 * word. But the CP had already fetched and executed CP_ME_INIT through this
	 * SMMU, so "secure" is not obviously in the way of a memory write, and the
	 * Mi 8 port never got past this question either (its own log says "zap
	 * authentication remains unverified").
	 *
	 * So 0144 stops treating the zap as a gate: when it does not authenticate,
	 * RBBM_SECVID_TRUST_CNTL is left exactly as it is - this phone says
	 * qcom,gpu-quirk-secvid-set-once, one write is all it has, and the zap is
	 * what would make that write legal - and the write test below runs anyway.
	 * It answers the only question that matters: may the CP write memory.
	 */
	if (trust != 0 && pg_zap_load()) {
		ring[wptr++] = pg_pkt7(PG_CP_SET_SECURE_MODE, 1);
		ring[wptr++] = 0;
		pg_to_memory(pg_ring_va[0], PG_GMU_PAGE_SIZE);
		printf("POLARIS GPU: 16 CP_SET_SECURE_MODE(0) submitted (wptr %u)\n", wptr);
		pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
		for (uint32_t i = 0; i < 500; i++) {
			rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
			if (rptr == wptr) {
				break;
			}
			pg_sleep_us(100);
		}
		trust = pg_rd(pg_gpu3d + 4u * PG_SECVID_TRUST_CNTL);
		printf("POLARIS GPU: 16 CP_SET_SECURE_MODE: RPTR %u WPTR %u, SECVID_TRUST_CNTL %#010x "
		    "(%s)\n", rptr, pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR), trust,
		    trust == 0 ? "NON-SECURE MODE" : "still secure");
	} else if (trust != 0) {
		printf("POLARIS GPU: 16 no zap on this phone: SECVID_TRUST_CNTL is left untouched "
		    "(set-once) and the CP's write test runs anyway\n");
	}
	nonsecure = pg_rd(pg_gpu3d + 4u * PG_SECVID_TRUST_CNTL) == 0;
	printf("POLARIS GPU: 16 the core is %s; the write below is the real question\n",
	    nonsecure ? "out of secure mode" : "still secure");
	PG_CHECK("16: the secure mode question is settled");
	/* The first real command: CP_MEM_WRITE of two words into the memptrs page. */
	{
		volatile uint32_t *memptrs = (volatile uint32_t *)pg_memptrs_va;

		memptrs[2] = memptrs[3] = 0;
		pg_to_memory(pg_memptrs_va, PG_GMU_PAGE_SIZE);
		ring[wptr++] = pg_pkt7(PG_CP_MEM_WRITE, 4);
		ring[wptr++] = (uint32_t)(PG_MEMPTRS_IOVA + 8);
		ring[wptr++] = (uint32_t)((PG_MEMPTRS_IOVA + 8) >> 32);
		ring[wptr++] = 0xc0ffee21u;
		ring[wptr++] = 0x0badf00du;
		pg_to_memory(pg_ring_va[0], PG_GMU_PAGE_SIZE);
		PG_CHECK("16: CP_MEM_WRITE is in the ring");
		pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
		for (uint32_t i = 0; i < 500; i++) {
			rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
			if (rptr == wptr) {
				ok = true;
				break;
			}
			pg_sleep_us(100);
		}
		pg_to_memory(pg_memptrs_va, PG_GMU_PAGE_SIZE);
		printf("POLARIS GPU: 16 CP_MEM_WRITE: RPTR %u WPTR %u, memptrs[2..3] = %#010x "
		    "%#010x -- the CP's first write %s\n", rptr, pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR),
		    memptrs[2], memptrs[3],
		    (memptrs[2] == 0xc0ffee21u && memptrs[3] == 0x0badf00du) ?
		    "ARRIVED in memory" : "did NOT arrive");
	}
	pg_cp_wptr = wptr;
	return ok;
}

/* ---- step 17: a real fill, written by the CP through the SMMU ----------------------------------- */

/*
 * The Mi 8 port's gpu_fill_screen() never touches the 3D pipeline: it fills the
 * framebuffer with CP_MEM_WRITE packets straight out of the ring. That is the
 * cheapest decisive test of the thing that actually matters here - whether the
 * command processor, running as the 3D core's stream, may write memory while
 * the core has never left secure mode.
 *
 * The image is built so that one glance says what went wrong if it does: eight
 * colour bars, a white frame along the top and the two sides, and a grey ramp
 * along the bottom, which only reads left to right if rows and columns are the
 * right way round.
 *
 * The ring is eight separately allocated pages, so it is indexed, never walked.
 * The write pointer is masked into the ring the way Linux and the Mi 8 port keep
 * it, because the hardware's read pointer is masked too: an equality test is the
 * only drain test that still works after the ring has wrapped once.
 */
#define PG_DRAW_IOVA		0x10020000ull	/* inside the CP's 2 MB window, after the memptrs */
#define PG_DRAW_WIDTH		1024u
#define PG_DRAW_HEIGHT		128u
#define PG_DRAW_WORDS		(PG_DRAW_WIDTH * PG_DRAW_HEIGHT)
#define PG_DRAW_PAGES		(PG_DRAW_WORDS * 4u / PG_GMU_PAGE_SIZE)
#define PG_DRAW_PAGE_WORDS	(PG_GMU_PAGE_SIZE / 4u)
#define PG_DRAW_CHUNK		1000u		/* dwords per CP_MEM_WRITE packet */
#define PG_DRAW_PER_BATCH	6u		/* 6 x (3 + 1000) words: inside the ring */
#define PG_DRAW_SENTINEL	0xdeadbeefu

static vm_offset_t pg_draw_va[PG_DRAW_PAGES];

static void
pg_ring_put(uint32_t at, uint32_t value)
{
	at &= PG_RING_WORDS - 1u;
	((uint32_t *)pg_ring_va[at / PG_RING_PAGE_WORDS])[at % PG_RING_PAGE_WORDS] = value;
}

static void
pg_ring_flush(void)
{
	for (uint32_t i = 0; i < PG_CP_PAGES; i++) {
		pg_to_memory(pg_ring_va[i], PG_GMU_PAGE_SIZE);
	}
}

static uint32_t *
pg_draw_word(uint32_t index)
{
	return &((uint32_t *)pg_draw_va[index / PG_DRAW_PAGE_WORDS])[index % PG_DRAW_PAGE_WORDS];
}

static uint32_t
pg_draw_pixel(uint32_t x, uint32_t y)
{
	static const uint32_t bars[8] = {
		0xffffffffu, 0xffffff00u, 0xff00ffffu, 0xff00ff00u,
		0xffff00ffu, 0xffff0000u, 0xff0000ffu, 0xff000000u
	};

	if (y >= PG_DRAW_HEIGHT - 8u) {
		uint32_t v = x * 255u / (PG_DRAW_WIDTH - 1u);

		return 0xff000000u | (v << 16) | (v << 8) | v;
	}
	if (y < 8u || x < 8u || x >= PG_DRAW_WIDTH - 8u) {
		return 0xffffffffu;
	}
	return bars[x * 8u / PG_DRAW_WIDTH];
}

static bool
pg_step_draw(void)
{
	uint32_t wptr = pg_cp_wptr, rptr = 0, sent = 0, batches = 0;
	uint32_t wrong = 0, first_bad = 0, first_bad_want = 0, first_bad_got = 0;
	uint32_t changes = 0, last = 0;
	bool ok = true;

	if (pg_gpu3d == 0 || !pg_smmu_tables() || pg_ring_va[0] == 0) {
		printf("POLARIS GPU: 17 the CP is not there\n");
		return false;
	}
	for (uint32_t i = 0; i < PG_DRAW_PAGES; i++) {
		if (kmem_alloc(kernel_map, &pg_draw_va[i], PG_GMU_PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("POLARIS GPU: 17 no memory for framebuffer page %u\n", i);
			return false;
		}
	}
	PG_CHECK("17: the framebuffer is allocated");
	for (uint32_t i = 0; i < PG_DRAW_PAGES; i++) {
		if (!pg_map_phys(PG_DRAW_IOVA + (uint64_t)i * PG_GMU_PAGE_SIZE,
		    kvtophys(pg_draw_va[i]), "the framebuffer")) {
			return false;
		}
	}
	pg_to_memory(pg_l3b_va, PG_CP_WINDOW_L2S * PG_GMU_PAGE_SIZE);
	{
		uint64_t *l3 = (uint64_t *)pg_smmu_l3(PG_DRAW_IOVA);
		uint32_t first = (uint32_t)((PG_DRAW_IOVA >> 12) & 0x1ff);
		uint32_t lastslot = (uint32_t)(((PG_DRAW_IOVA +
		    (PG_DRAW_PAGES - 1) * PG_GMU_PAGE_SIZE) >> 12) & 0x1ff);

		printf("POLARIS GPU: 17 %ux%u BGRA at iova %#llx, %u pages; slot %u -> %#llx, "
		    "slot %u -> %#llx\n", PG_DRAW_WIDTH, PG_DRAW_HEIGHT, PG_DRAW_IOVA,
		    (uint32_t)PG_DRAW_PAGES, first, l3[first] & PG_PTE_ADDR, lastslot,
		    l3[lastslot] & PG_PTE_ADDR);
	}
	PG_CHECK("17: the framebuffer is in the 3D core's window");
	/* A sentinel first, so "the CP wrote nothing" and "the CP wrote this" cannot be confused. */
	for (uint32_t i = 0; i < PG_DRAW_WORDS; i++) {
		*pg_draw_word(i) = PG_DRAW_SENTINEL;
	}
	for (uint32_t i = 0; i < PG_DRAW_PAGES; i++) {
		pg_to_memory(pg_draw_va[i], PG_GMU_PAGE_SIZE);
	}
	PG_CHECK("17: the sentinel is in memory");
	while (sent < PG_DRAW_WORDS && ok) {
		for (uint32_t c = 0; c < PG_DRAW_PER_BATCH && sent < PG_DRAW_WORDS; c++) {
			uint32_t n = PG_DRAW_WORDS - sent < PG_DRAW_CHUNK ?
			    PG_DRAW_WORDS - sent : PG_DRAW_CHUNK;
			uint64_t addr = PG_DRAW_IOVA + 4ull * sent;

			pg_ring_put(wptr++, pg_pkt7(PG_CP_MEM_WRITE, n + 2u));
			pg_ring_put(wptr++, (uint32_t)addr);
			pg_ring_put(wptr++, (uint32_t)(addr >> 32));
			for (uint32_t i = 0; i < n; i++) {
				uint32_t pixel = sent + i;

				pg_ring_put(wptr++, pg_draw_pixel(pixel % PG_DRAW_WIDTH,
				    pixel / PG_DRAW_WIDTH));
			}
			sent += n;
		}
		/*
		 * WPTR is written masked, the way Linux and the Mi 8 port do it: the
		 * hardware's RPTR is masked too, so an equality test is the only drain
		 * test that works once the ring has wrapped once.
		 */
		wptr &= PG_RING_WORDS - 1u;
		pg_ring_flush();
		pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
		__asm__ volatile ("dsb sy" ::: "memory");
		{
			bool drained = false;

			for (uint32_t i = 0; i < 20000; i++) {
				rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
				if (rptr == wptr) {
					drained = true;
					break;
				}
				pg_sleep_us(10);
			}
			if (!drained) {
				printf("POLARIS GPU: 17 the CP stopped at %u of %u dwords: RPTR %u "
				    "WPTR %u, RBBM_STATUS %#010x INT_0_STATUS %#010x\n", sent,
				    (uint32_t)PG_DRAW_WORDS, rptr,
				    pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR),
				    pg_rd(pg_gpu3d + 4u * PG_RBBM_STATUS),
				    pg_rd(pg_gpu3d + 4u * PG_RBBM_INT_STATUS));
				ok = false;
			}
		}
		batches++;
	}
	pg_cp_wptr = wptr;
	{
		vm_offset_t cb = pg_smmu_cb(PG_GMU_CTX);

		printf("POLARIS GPU: 17 the CP %s: %u dwords in %u submissions, RBBM_STATUS %#010x "
		    "INT_0_STATUS %#010x; context bank %u FSR %#010x FSYNR0 %#010x FAR %#010x%08x\n",
		    ok ? "read the whole ring" : "did NOT read the whole ring", sent, batches,
		    pg_rd(pg_gpu3d + 4u * PG_RBBM_STATUS),
		    pg_rd(pg_gpu3d + 4u * PG_RBBM_INT_STATUS), PG_GMU_CTX,
		    pg_rd(cb + PG_CB_FSR), pg_rd(cb + 0x5c), pg_rd(cb + 0x64), pg_rd(cb + 0x60));
	}
	PG_CHECK("17: the CP has been given the fill");
	/* Read it back out of memory, not out of a cache the CP never touched. */
	for (uint32_t i = 0; i < PG_DRAW_PAGES; i++) {
		pg_to_memory(pg_draw_va[i], PG_GMU_PAGE_SIZE);
	}
	for (uint32_t i = 0; i < PG_DRAW_WORDS; i++) {
		uint32_t want = pg_draw_pixel(i % PG_DRAW_WIDTH, i / PG_DRAW_WIDTH);
		uint32_t got = *pg_draw_word(i);

		if (got != want) {
			if (wrong == 0) {
				first_bad = i;
				first_bad_want = want;
				first_bad_got = got;
			}
			wrong++;
		}
		if (i == 0 || got != last) {
			changes++;
			last = got;
		}
	}
	printf("POLARIS GPU: 17 read back %u dwords: %u wrong", (uint32_t)PG_DRAW_WORDS, wrong);
	if (wrong != 0) {
		printf(", first at %u (x %u y %u) wanted %#010x got %#010x", first_bad,
		    first_bad % PG_DRAW_WIDTH, first_bad / PG_DRAW_WIDTH, first_bad_want,
		    first_bad_got);
	}
	printf("; %u colour changes along the walk\n", changes);
	printf("POLARIS GPU: 17 corners %#010x %#010x %#010x %#010x, centre %#010x, ramp %#010x "
	    "%#010x\n", *pg_draw_word(0), *pg_draw_word(PG_DRAW_WIDTH - 1),
	    *pg_draw_word(PG_DRAW_WORDS - PG_DRAW_WIDTH), *pg_draw_word(PG_DRAW_WORDS - 1),
	    *pg_draw_word((PG_DRAW_HEIGHT / 2) * PG_DRAW_WIDTH + PG_DRAW_WIDTH / 2),
	    *pg_draw_word((PG_DRAW_HEIGHT - 4) * PG_DRAW_WIDTH),
	    *pg_draw_word((PG_DRAW_HEIGHT - 4) * PG_DRAW_WIDTH + PG_DRAW_WIDTH - 1));
	if (wrong == 0) {
		printf("POLARIS GPU: 17 THE CP WROTE THE WHOLE IMAGE: this core writes memory "
		    "without the zap\n");
	} else if (changes == 1 && last == PG_DRAW_SENTINEL) {
		printf("POLARIS GPU: 17 nothing arrived: the sentinel is untouched everywhere\n");
	}
	return ok && wrong == 0;
}

/* ---- step 18: the CP writes the memory the display is scanning out --------- */

/*
 * Steps 1..17 prove the 3D core can write memory it owns. This step asks the
 * only question that matters for a display: can it write the *panel*?
 *
 * The panel is 1080x2160 at a 4320-byte stride, 9331200 bytes. Build 0154 could
 * only reach a 64-row band of it, because the CP's window was a single 2 MB
 * level 3 page and step 17's scratch buffer already owned most of it. The window
 * is now PG_CP_WINDOW_L2S (five) level 3 pages, 10 MB, so the whole panel fits -
 * starting at 0x100a0000, above everything already mapped, so no existing
 * descriptor moves.
 *
 * Build 0154 also had no read-back: the only check was the screen, and
 * backboardd's first swap erases the evidence before anyone can look at it, so
 * that build ended with "the CP consumed the ring" and nothing more. This one
 * reads pixels back through the CPU afterwards. ml_io_map gives an uncached
 * window onto the same physical pages the CP just wrote, which is the cheapest
 * possible proof that the command processor's writes reached the memory the
 * display is scanning out.
 */
#define PG_PANEL_IOVA		0x100a0000ull	/* the first page above step 17's scratch */
#define PG_PANEL_MAX_PAGES	2400u		/* 0x100a0000..0x109fffff: the rest of the window */

/*
 * The panel's geometry comes from PE_current_console(), which is where
 * SweetFramebuffer::start() gets it too (video.v_baseAddr and friends). Reading
 * it here rather than exporting it from the framebuffer keeps this file free of
 * any dependency on the display kext, and it is the same number either way.
 */
static bool
pg_panel_geometry(uint64_t *phys, uint32_t *width, uint32_t *height, uint32_t *stride)
{
	PE_Video video;

	PE_current_console(&video);
	if (video.v_baseAddr == 0 || video.v_width == 0 || video.v_height == 0 ||
	    video.v_rowBytes == 0) {
		return false;
	}
	*phys = video.v_baseAddr;
	*width = (uint32_t)video.v_width;
	*height = (uint32_t)video.v_height;
	*stride = (uint32_t)video.v_rowBytes;
	return true;
}

/*
 * The one definition of what the CP is told to write, so the read-back below
 * compares against the same arithmetic instead of a copy of it. Red ramps left
 * to right, green top to bottom, so a swapped stride or a wrong row offset is
 * visible in the numbers and not just in the picture.
 */
static uint32_t
pg_panel_pixel(uint32_t row, uint32_t col, uint32_t width, uint32_t height)
{
	uint32_t v = col * 255u / (width - 1u);
	uint32_t g = row * 255u / (height - 1u);

	return 0xff000000u | (g << 16) | (v << 8) | (255u - v);
}

/*
 * Read pixels back through the CPU.
 *
 * Build 0155 verified nothing when the fill did not finish, so a panel that was
 * three quarters right looked exactly like a panel that had never been written.
 * Build 0156 fixed that and then panicked before it printed its summary:
 *
 *   va_map_delete(0xfffffe1086419e98, 0xfffffe460dbc000, 0xfffffe460dcbc000):
 *       Attempting to remove permanent VM map entry
 *       0xfffffe108641f970 [0xfffffe460db10000:0xfffffe460dcbc000)
 *
 * ml_io_map() hands back an entry the vm map marks permanent, so ml_io_unmap()
 * may not delete any part of it - not the whole entry and not a tail of it. A
 * boot probe that leaks ten megabytes of device address space is cheaper than
 * one that panics, so the panel is mapped once here and never released.
 *
 * The mapping is not the only thing 0156 got wrong. Every pixel it managed to
 * report read 0xff000000 - opaque black - while the values it compared against
 * (0xff0000ff, 0xff007f80, 0xff00ff00) are exactly what pg_panel_pixel() says
 * the command processor was told to write. So the question is not whether the
 * arithmetic is right but where the black came from: the surface belongs to the
 * window server by this point in the boot, and the fill takes seconds, which is
 * ample time for the rows written first to be painted over.
 *
 * Hence three readings of the same three pixels: before the fill, microseconds
 * after one row is written again, and one second after that. If the first is
 * black and the second is ours, the panel was repainted and the write landed.
 * If the second is black too, the CPU is not looking at the surface the CP
 * wrote and no amount of waiting will help.
 */
static vm_offset_t pg_panel_map;
static uint32_t pg_panel_skew;
static vm_size_t pg_panel_span;

static bool
pg_panel_attach(uint64_t phys, uint32_t stride, uint32_t height)
{
	uint64_t bytes = (uint64_t)stride * (uint64_t)height;
	uint64_t base = phys & ~(uint64_t)(PG_GMU_PAGE_SIZE - 1u);

	if (pg_panel_map != 0) {
		return true;
	}
	/*
	 * The console's base is page aligned on this panel, but carrying the
	 * skew costs nothing and a dropped skew would put every read one row
	 * out without any number looking obviously wrong.
	 */
	pg_panel_skew = (uint32_t)(phys - base);
	pg_panel_span = (vm_size_t)(((uint64_t)pg_panel_skew + bytes +
	    PG_GMU_PAGE_SIZE - 1u) & ~(uint64_t)(PG_GMU_PAGE_SIZE - 1u));
	pg_panel_map = ml_io_map((vm_offset_t)base, pg_panel_span);
	if (pg_panel_map == 0) {
		printf("POLARIS GPU: 18 the panel at %#llx will not map\n", phys);
		return false;
	}
	printf("POLARIS GPU: 18 the panel is mapped once at %#llx: %#llx + %u, %u bytes\n",
	    pg_panel_map, base, pg_panel_skew, (uint32_t)pg_panel_span);
	return true;
}

static uint32_t
pg_panel_read(uint32_t row, uint32_t col, uint32_t stride)
{
	return ((volatile uint32_t *)(pg_panel_map + pg_panel_skew +
	    (uint64_t)row * (uint64_t)stride))[col];
}

/* Three pixels of one row: what they read, and what the CP was told to write. */
static uint32_t
pg_panel_show(const char *when, uint32_t row, uint32_t width, uint32_t height,
    uint32_t stride)
{
	uint32_t cols[3], got[3], want[3], same = 0;

	cols[0] = 0;
	cols[1] = width / 2u;
	cols[2] = width - 1u;
	for (uint32_t i = 0; i < 3; i++) {
		got[i] = pg_panel_read(row, cols[i], stride);
		want[i] = pg_panel_pixel(row, cols[i], width, height);
		if (got[i] == want[i]) {
			same++;
		}
	}
	printf("POLARIS GPU: 18 %s row %u: reads %#010x %#010x %#010x, wanted "
	    "%#010x %#010x %#010x\n", when, row, got[0], got[1], got[2],
	    want[0], want[1], want[2]);
	return same;
}

/*
 * One row, one packet, one drain. That is the shortest gap this code can put
 * between "the CP was told to write this" and "the CPU read it", and the gap is
 * what decides whether the reading means anything on a surface the rest of the
 * system is drawing on.
 */
static bool
pg_panel_write_row(uint32_t row, uint32_t width, uint32_t height, uint32_t stride)
{
	uint32_t wptr = pg_cp_wptr, rptr = 0, per_row = stride / 4u;
	uint64_t addr = PG_PANEL_IOVA + (uint64_t)row * (uint64_t)stride;

	pg_ring_put(wptr++, pg_pkt7(PG_CP_MEM_WRITE, per_row + 2u));
	pg_ring_put(wptr++, (uint32_t)addr);
	pg_ring_put(wptr++, (uint32_t)(addr >> 32));
	for (uint32_t i = 0; i < per_row; i++) {
		pg_ring_put(wptr++, pg_panel_pixel(row, i, width, height));
	}
	wptr &= PG_RING_WORDS - 1u;
	pg_ring_flush();
	pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 100000; i++) {
		rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
		if (rptr == wptr) {
			pg_cp_wptr = wptr;
			return true;
		}
		pg_sleep_us(10);
	}
	printf("POLARIS GPU: 18 rewriting row %u did not drain: RPTR %u WPTR %u, "
	    "RBBM_STATUS %#010x\n", row, rptr, wptr,
	    pg_rd(pg_gpu3d + 4u * PG_RBBM_STATUS));
	return false;
}

static bool
pg_step_panel(void)
{
	uint64_t phys = 0;
	uint32_t width = 0, height = 0, stride = 0;
	uint32_t bytes, words, pages, wptr = pg_cp_wptr, rptr = 0, sent = 0, batches = 0;
	uint32_t fresh = 0;
	bool ok = true;

	if (pg_gpu3d == 0 || !pg_smmu_tables() || pg_ring_va[0] == 0) {
		printf("POLARIS GPU: 18 the CP is not there\n");
		return false;
	}
	if (!pg_panel_geometry(&phys, &width, &height, &stride)) {
		printf("POLARIS GPU: 18 the console has no framebuffer; nothing to write\n");
		return false;
	}
	if (phys == 0 || stride < width * 4 || height < 8) {
		printf("POLARIS GPU: 18 the panel geometry is unusable (%#llx, %ux%u stride %u)\n",
		    phys, width, height, stride);
		return false;
	}
	bytes = stride * height;
	pages = (bytes + PG_GMU_PAGE_SIZE - 1u) / PG_GMU_PAGE_SIZE;
	if (pages > PG_PANEL_MAX_PAGES) {
		printf("POLARIS GPU: 18 the panel is %u pages and the window holds %u: not "
		    "writing a partial image\n", pages, PG_PANEL_MAX_PAGES);
		return false;
	}
	words = bytes / 4u;
	printf("POLARIS GPU: 18 panel %#llx %ux%u stride %u: the whole thing, %u bytes, "
	    "%u pages at iova %#llx\n", phys, width, height, stride, bytes, pages, PG_PANEL_IOVA);
	/*
	 * Map it for the CPU before anything is written, and read the top row
	 * once: whatever is there now is what the rest of the system left on the
	 * surface, which is the baseline the readings below are measured against.
	 */
	if (!pg_panel_attach(phys, stride, height)) {
		return false;
	}
	pg_panel_show("before anything,", 0, width, height, stride);

	/*
	 * Map the whole panel. The panel's first page is the one the display
	 * starts scanning at, so this is the top of the visible image, and the
	 * last page is nine megabytes further into the window.
	 */
	for (uint32_t i = 0; i < pages; i++) {
		if (!pg_map_phys(PG_PANEL_IOVA + (uint64_t)i * PG_GMU_PAGE_SIZE,
		    phys + (uint64_t)i * PG_GMU_PAGE_SIZE, "the panel")) {
			return false;
		}
	}
	pg_to_memory(pg_l3b_va, PG_CP_WINDOW_L2S * PG_GMU_PAGE_SIZE);
	{
		uint64_t last = PG_PANEL_IOVA + (uint64_t)(pages - 1u) * PG_GMU_PAGE_SIZE;
		uint32_t fi = (uint32_t)((PG_PANEL_IOVA >> 12) & 0x1ff);
		uint32_t li = (uint32_t)((last >> 12) & 0x1ff);
		uint64_t *l3f = (uint64_t *)pg_smmu_l3(PG_PANEL_IOVA);
		uint64_t *l3l = (uint64_t *)pg_smmu_l3(last);

		printf("POLARIS GPU: 18 first page slot %u -> %#llx, last (%#llx) slot %u -> "
		    "%#llx, tables %s\n", fi, l3f[fi] & PG_PTE_ADDR, last, li,
		    l3l[li] & PG_PTE_ADDR, l3f == l3l ? "the same" : "different");
	}
	PG_CHECK("18: the whole panel is in the 3D core's window");

	/*
	 * The decisive reading comes first, while the command processor is known
	 * to be alive: step 17 has just drained a thousand packets through it.
	 * Write one row and read it back at once. If those three pixels are ours,
	 * the CP's writes reach the memory the display scans out and the CPU sees
	 * them, and the long fill below only adds how far the ring got.
	 *
	 * Doing this after the fill instead would have measured nothing at all in
	 * both 0155 and 0156, because the fill is exactly where the CP stops.
	 */
	{
		bool wrote = pg_panel_write_row(0, width, height, stride);

		if (!wrote) {
			printf("POLARIS GPU: 18 the CP would not take another row, so the reading "
			    "below says nothing about the surface\n");
		}
		fresh = pg_panel_show(wrote ? "a moment after rewriting" :
		    "after a rewrite the CP did not take,", 0, width, height, stride);
		for (uint32_t i = 0; i < 100u; i++) {
			pg_sleep_us(10000);
		}
		pg_panel_show("one second later,", 0, width, height, stride);
		ok = wrote && (fresh == 3);
		if (ok) {
			printf("POLARIS GPU: 18 the CP's writes are in the memory the display scans "
			    "out, and the CPU reads them back within microseconds\n");
		} else if (wrote) {
			printf("POLARIS GPU: 18 a row written microseconds ago does not read back: "
			    "the CPU's view of %#llx is not the surface the CP wrote\n", phys);
		}
	}

	{
		bool stalled = false;

		while (sent < words && !stalled) {
			for (uint32_t c = 0; c < PG_DRAW_PER_BATCH && sent < words; c++) {
				uint32_t n = words - sent < PG_DRAW_CHUNK ? words - sent : PG_DRAW_CHUNK;
				uint64_t addr = PG_PANEL_IOVA + 4ull * sent;
				uint32_t per_row = stride / 4u;

				pg_ring_put(wptr++, pg_pkt7(PG_CP_MEM_WRITE, n + 2u));
				pg_ring_put(wptr++, (uint32_t)addr);
				pg_ring_put(wptr++, (uint32_t)(addr >> 32));
				for (uint32_t i = 0; i < n; i++) {
					uint32_t at = sent + i;

					pg_ring_put(wptr++, pg_panel_pixel(at / per_row, at % per_row,
					    width, height));
				}
				sent += n;
			}
			wptr &= PG_RING_WORDS - 1u;
			pg_ring_flush();
			pg_wr(pg_gpu3d + 4u * PG_CP_RB_WPTR, wptr);
			__asm__ volatile ("dsb sy" ::: "memory");
			{
				bool drained = false;

				/*
				 * One second, not 200 ms. Build 0155 gave up after 200 ms
				 * at 83% and reported a stop that may only have been the
				 * CP falling behind - the ring had 1840 words queued and
				 * RBBM_STATUS still said the block was alive. Waiting
				 * longer costs boot time only if the CP really has
				 * stopped, and then the message says so.
				 */
				for (uint32_t i = 0; i < 100000; i++) {
					rptr = pg_rd(pg_gpu3d + 4u * PG_CP_RB_RPTR);
					if (rptr == wptr) {
						drained = true;
						break;
					}
					pg_sleep_us(10);
				}
				if (!drained) {
					vm_offset_t cb = pg_smmu_cb(PG_GMU_CTX);

					printf("POLARIS GPU: 18 the CP stalled at %u of %u dwords "
					    "(%u%%) after %u submissions: RPTR %u WPTR %u, "
					    "RBBM_STATUS %#010x; keeping what landed\n",
					    sent, words, (uint32_t)((uint64_t)sent * 100u / words),
					    batches, rptr, pg_rd(pg_gpu3d + 4u * PG_CP_RB_WPTR),
					    pg_rd(pg_gpu3d + 4u * PG_RBBM_STATUS));
					/*
					 * Build 0155 stopped at 83% and 0156 at 65%,
					 * which is what a race looks like and also
					 * what a fault looks like. The context bank
					 * tells them apart: a translation fault leaves
					 * FSR non-zero and FAR pointing at the address
					 * the CP could not reach, and a stopped stream
					 * leaves both at zero.
					 */
					printf("POLARIS GPU: 18 context bank %u FSR %#010x "
					    "FSYNR0 %#010x FAR %#010x%08x INT_0_STATUS "
					    "%#010x\n", PG_GMU_CTX, pg_rd(cb + PG_CB_FSR),
					    pg_rd(cb + 0x5c), pg_rd(cb + 0x64),
					    pg_rd(cb + 0x60),
					    pg_rd(pg_gpu3d + 4u * PG_RBBM_INT_STATUS));
					stalled = true;
				}
			}
			batches++;
			if (!stalled && (batches % 64u) == 0) {
				printf("POLARIS GPU: 18 %u of %u dwords (%u%%), %u submissions\n",
				    sent, words, (uint32_t)((uint64_t)sent * 100u / words), batches);
			}
		}
	}
	pg_cp_wptr = wptr;
	printf("POLARIS GPU: 18 the CP wrote %u of %u dwords (%u%%) in %u submissions at %#llx\n",
	    sent, words, (uint32_t)((uint64_t)sent * 100u / words), batches, PG_PANEL_IOVA);
	{
		uint32_t per_row = stride / 4u;
		uint32_t written_rows = sent / per_row;

		if (written_rows > height) {
			written_rows = height;
		}
		if (written_rows == 0) {
			printf("POLARIS GPU: 18 nothing was written, so there is nothing to read back\n");
			return false;
		}
		/*
		 * These rows have had seconds to be repainted, so they are not the
		 * proof - the rewrite above was. They say how far the fill got and
		 * what the surface looked like once it stopped: row 0 was written
		 * first and has had the longest exposure, and the last row that
		 * landed is in a different level 2 entry of the CP's window, so it
		 * is also the evidence that the window is five level 3 pages and not
		 * just the first one.
		 */
		pg_panel_show("after the fill,", 0, width, height, stride);
		if (written_rows >= 16u) {
			pg_panel_show("after the fill,", written_rows - 1u, width, height, stride);
		}
	}
	return ok;
}

/* ---- the ladder ------------------------------------------------------------------------------------ */

void
polaris_gpu_up(int upto)
{
	bool ok;
	int map = -1;

	/*
	 * The mode has to be known before step 10 allocates anything, and the
	 * banner has to say which build ran: a ring buffer read after a crash is
	 * worthless if the first line cannot be tied to an image.
	 */
	(void)PE_parse_boot_argn("polarisgpumap", &map, sizeof(map));
	pg_smmu_mode = (map == 1);
	(void)PE_parse_boot_argn("polarisgpummu", &pg_mmu_way, sizeof(pg_mmu_way));
	(void)PE_parse_boot_argn("polarisgpumem", &pg_mem_way, sizeof(pg_mem_way));
	printf("POLARIS GPU: build 0144 up=%d mmu=%d mem=%d regions=%s\n", upto, pg_mmu_way,
	    pg_mem_way, pg_smmu_mode ? "behind SMMU translation (polarisgpumap=1)" :
	    "physical, stream 5 bypassed (polarisgpumap=0)");
	printf("POLARIS GPU: ladder 1..%d, addressing from the Mi 8 port (same SDM845 nodes)\n", upto);

	PG_CHECK("step 1: the GCC block");
	ok = pg_step_gcc();
	if (!ok) {
		printf("POLARIS GPU: stop after 1: GCC could not be mapped at all\n");
		return;
	}
	if (upto < 2) {
		return;
	}
	PG_CHECK("step 2: the GPUCC block");
	ok = pg_step_gpucc();
	if (!ok) {
		printf("POLARIS GPU: stop after 2: GPUCC was not read\n");
		return;
	}
	if (upto < 3) {
		return;
	}
	PG_CHECK("step 3: the CX switch");
	ok = pg_step_cx_on();
	if (!ok) {
		printf("POLARIS GPU: stop after 3: the CX switch did not come on\n");
		return;
	}
	if (upto < 4) {
		return;
	}
	PG_CHECK("step 4: the CX clocks");
	ok = pg_step_clocks();
	if (!ok) {
		printf("POLARIS GPU: stop after 4: a clock did not come on\n");
		return;
	}
	if (upto < 5) {
		return;
	}
	PG_CHECK("step 5: the GMU registers");
	if (!pg_step_gmu()) {
		printf("POLARIS GPU: stop after 5: the GMU's counter is not advancing\n");
		return;
	}
	if (upto < 6) {
		return;
	}
	PG_CHECK("step 6: the SMMU, read only");
	if (!pg_step_smmu()) {
		printf("POLARIS GPU: stop after 6: the SMMU was not read\n");
		return;
	}
	if (upto < 7) {
		return;
	}
	PG_CHECK("step 7: the GMU memory");
	if (!pg_step_gmu_tcm()) {
		printf("POLARIS GPU: stop after 7: the GMU's memory did not take the pattern\n");
		return;
	}
	if (upto < 8) {
		return;
	}
	PG_CHECK("step 8: the firmware");
	if (!pg_step_gmu_firmware()) {
		printf("POLARIS GPU: stop after 8: the firmware did not land in the GMU's memory\n");
		return;
	}
	if (upto < 9) {
		return;
	}
	PG_CHECK("step 9: the RSC and PDC");
	if (!pg_step_rsc_pdc()) {
		printf("POLARIS GPU: stop after 9: the PDC's tables did not take\n");
		return;
	}
	if (upto < 10) {
		return;
	}
	PG_CHECK("step 10: the boot configuration");
	if (!pg_step_gmu_boot_config()) {
		printf("POLARIS GPU: stop after 10: the boot configuration did not take\n");
		return;
	}
	if (upto < 11) {
		return;
	}
	PG_CHECK("step 11: the CM3 release");
	if (!pg_step_gmu_start(upto < 12)) {
		printf("POLARIS GPU: stop after 11: the firmware did not announce itself\n");
		return;
	}
	if (upto < 12) {
		return;
	}
	PG_CHECK("step 12: the GX rail");
	if (!pg_step_gx_rail_on(upto < 13)) {
		printf("POLARIS GPU: stop after 12: the GX rail did not come up\n");
		return;
	}
	if (upto < 13) {
		return;
	}
	pg_keep_running = upto >= 14;
	PG_CHECK("step 13: the HFI channel");
	if (!pg_step_hfi()) {
		printf("POLARIS GPU: stop after 13: the firmware did not answer over HFI\n");
		return;
	}
	if (upto < 14) {
		printf("POLARIS GPU: ladder done; the CX half is left powered, nothing may be "
		    "released yet\n");
		return;
	}
	PG_CHECK("step 14: the 3D core");
	if (!pg_step_hw_init()) {
		printf("POLARIS GPU: stop after 14: the 3D core did not take the table\n");
		return;
	}
	if (upto < 15) {
		printf("POLARIS GPU: ladder done at 14; the firmware and the 3D core are left "
		    "running\n");
		return;
	}
	PG_CHECK("step 15: the CP");
	if (!pg_step_cp()) {
		printf("POLARIS GPU: stop after 15: the CP did not take the ring\n");
		return;
	}
	if (upto < 16) {
		printf("POLARIS GPU: ladder done at 15; the CP has run CP_ME_INIT\n");
		return;
	}
	PG_CHECK("step 16: the zap shader and the CP's first write");
	if (!pg_step_zap()) {
		printf("POLARIS GPU: stop after 16: the CP's first write did not land\n");
		return;
	}
	if (upto < 17) {
		printf("POLARIS GPU: ladder done at 16; the CP has written to memory on its own\n");
		return;
	}
	PG_CHECK("step 17: a real fill through the CP");
	if (!pg_step_draw()) {
		printf("POLARIS GPU: stop after 17: the fill did not land\n");
		return;
	}
	if (upto < 18) {
		printf("POLARIS GPU: ladder done at 17; the CP has filled a framebuffer through the SMMU\n");
		return;
	}
	PG_CHECK("step 18: the CP writes the panel");
	if (!pg_step_panel()) {
		printf("POLARIS GPU: stop after 18: the panel fill did not land\n");
		return;
	}
	printf("POLARIS GPU: ladder done at 18; the CP has written the whole panel and the "
	    "CPU read it back\n");
}

void
polaris_gpu_init(void)
{
	int optin = 0;
	int upto = 0;

	if (!polaris_touch_i2c_board()) {
		return;
	}
	(void)PE_parse_boot_argn("polarisgpu", &optin, sizeof(optin));
	if (optin != 1 || polaris_gpu.state != POLARIS_GPU_OFF) {
		return;
	}
	/*
	 * The ladder runs only when asked for, and stops where it is told to.
	 * It leaves the CX half powered with no release path, so the context is
	 * quarantined afterwards rather than pretending the GPU is idle.
	 */
	(void)PE_parse_boot_argn("polarisgpuup", &upto, sizeof(upto));
	if (upto > 0) {
		polaris_gpu.state = POLARIS_GPU_STARTING;
		polaris_gpu_up(upto);
		polaris_gpu.state = POLARIS_GPU_QUARANTINED;
		printf("POLARIS GPU: quarantined on purpose; CPU display unchanged\n");
		return;
	}
	polaris_gpu.state = POLARIS_GPU_STARTING;
	if (!polaris_gpu_enter(&polaris_gpu, POLARIS_GPU_BOARD)) {
		goto fail;
	}
	if (!polaris_gpu_enter(&polaris_gpu, POLARIS_GPU_FIRMWARE)) {
		goto fail;
	}
	/* Unreachable until individually owned, bounded backends are installed. */
	polaris_gpu.state = POLARIS_GPU_IDLE;
	return;
fail:
	polaris_gpu_unwind(&polaris_gpu, true);
	printf("POLARIS GPU: acceleration unavailable; CPU display unchanged\n");
}
#endif
