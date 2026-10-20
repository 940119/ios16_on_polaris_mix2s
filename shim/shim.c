/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "font.h"
#include "fdt.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"

#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile(".inst 0xd4000003"	/* smc #0; clang rejects it outside EL3 */
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

#ifdef TOUCHPROBE
/* SDM845 pinctrl PINGROUP base offsets (relative to TLMM 0x03400000).
 * Do not use the pin number alone: these five pins span three tiles. */
static volatile uint32_t *treg(uint32_t pin)
{
	uintptr_t tile;

	switch (pin) {
	case 23:             tile = 0x900000u; break; /* SOUTH */
	case 33: case 34:
	case 99:             tile = 0x500000u; break; /* NORTH */
	case 125:            tile = 0x100000u; break; /* EAST */
	default:             return (volatile uint32_t *)0;
	}
	return (volatile uint32_t *)(0x03400000UL + tile + pin * 0x1000u);
}

static uint32_t tsda = 33, tscl = 34;

static void tcfg(uint32_t pin, uint32_t mux, uint32_t pull, uint32_t drive, int out)
{
	volatile uint32_t *p = treg(pin);
	uint32_t cfg = p[0];

	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	p[0] = cfg;
}

static void tset(uint32_t pin, int v)
{
	volatile uint32_t *p = treg(pin);
	uint32_t x = p[1];

	if (v)
		x |= 2u;
	else
		x &= ~2u;
	p[1] = x;
}

static int tget(uint32_t pin)
{
	return (treg(pin)[1] & 1u) != 0;
}

static void tudelay(uint32_t us)
{
	uint64_t per = rsr("cntfrq_el0") / 1000000u;
	uint64_t s = now();

	while (now() - s < per * us)
		;
}

static void sda_rel(void) { tcfg(tsda, 0, 3, 2, 0); }
static void sda_low(void) { tset(tsda, 0); tcfg(tsda, 0, 0, 2, 1); }
static void scl_rel(void) { tcfg(tscl, 0, 3, 2, 0); }
static void scl_low(void) { tset(tscl, 0); tcfg(tscl, 0, 0, 2, 1); }

/* Open-drain self-test: drive low, then release; never drive I2C high. */
static uint32_t tp_selftest(void)
{
	uint32_t r = 0;

	sda_low(); tudelay(10);
	if (!tget(tsda)) r |= 1u;
	sda_rel(); tudelay(20);
	if (tget(tsda)) r |= 4u;
	scl_low(); tudelay(10);
	if (!tget(tscl)) r |= 8u;
	scl_rel(); tudelay(20);
	if (tget(tscl)) r |= 32u;
	return r; /* healthy, both lines released: 0x2d */
}

static void i2c_stop(void);

/* 0x3 means both lines released; never infer an ACK on a stuck bus. */
static uint32_t bus_state(void)
{
	sda_rel(); scl_rel(); tudelay(20);
	return (uint32_t)tget(tsda) | ((uint32_t)tget(tscl) << 1);
}

static void bus_recover(void)
{
	/* Only clock a bus whose SCL has actually risen. */
	sda_rel(); scl_rel(); tudelay(20);
	if (!tget(tscl))
		return;
	for (int i = 0; i < 9 && !tget(tsda); i++) {
		scl_low(); tudelay(10);
		scl_rel(); tudelay(10);
		if (!tget(tscl))
			return;
	}
	if (tget(tsda))
		i2c_stop();
}

static void i2c_start(void)
{
	sda_rel(); scl_rel(); tudelay(5);
	sda_low(); tudelay(5);
	scl_low(); tudelay(5);
}

static void i2c_repeat(void)
{
	sda_rel(); tudelay(5);
	scl_rel(); tudelay(5);
	sda_low(); tudelay(5);
	scl_low(); tudelay(5);
}

static void i2c_stop(void)
{
	sda_low(); tudelay(5);
	scl_rel(); tudelay(5);
	sda_rel(); tudelay(5);
}

static int i2c_write(uint8_t b)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		if (b & (1u << i))
			sda_rel();
		else
			sda_low();
		tudelay(3);
		scl_rel(); tudelay(3);
		scl_low(); tudelay(3);
	}
	sda_rel(); tudelay(5);
	scl_rel(); tudelay(5);
	if (!tget(tscl)) {
		scl_low();
		return 0;
	}
	ack = !tget(tsda);
	scl_low(); tudelay(5);
	return ack;
}

static uint8_t i2c_read(int ack)
{
	uint8_t v = 0;

	sda_rel();
	for (int i = 0; i < 8; i++) {
		v <<= 1;
		scl_rel(); tudelay(3);
		if (tget(tsda))
			v |= 1;
		scl_low(); tudelay(3);
	}
	if (ack)
		sda_low();
	else
		sda_rel();
	tudelay(3);
	scl_rel(); tudelay(3);
	scl_low(); tudelay(3);
	sda_rel();
	return v;
}

static int probe_addr(uint8_t a7)
{
	int ack;

	if (bus_state() != 3)
		return 0;
	i2c_start();
	ack = i2c_write(a7 << 1);
	i2c_stop();
	tudelay(500);
	return ack && bus_state() == 3;
}

/* RMI4 I2C: write 0xff,page to select a page, then send ONE register byte.
 * Reading the PDT at 0xe9 should return a nonzero function number. */
static int rmi_read(uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	if (bus_state() != 3)
		return 0;
	i2c_start();
	if (!i2c_write(addr << 1) || !i2c_write(0xff) ||
	    !i2c_write(reg >> 8)) {
		i2c_stop();
		return 0;
	}
	i2c_stop();
	if (bus_state() != 3)
		return 0;
	i2c_start();
	if (!i2c_write(addr << 1) || !i2c_write(reg & 0xff)) {
		i2c_stop();
		return 0;
	}
	i2c_repeat();
	if (!i2c_write((addr << 1) | 1)) {
		i2c_stop();
		return 0;
	}
	for (int i = 0; i < len; i++)
		buf[i] = i2c_read(i + 1 < len);
	i2c_stop();
	return bus_state() == 3;
}

/* Legacy single-byte-register read (FocalTech-style devices). */
static int leg_read(uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
	i2c_start();
	if (!i2c_write(addr << 1) || !i2c_write(reg)) {
		i2c_stop();
		return 0;
	}
	i2c_repeat();
	if (!i2c_write((addr << 1) | 1)) {
		i2c_stop();
		return 0;
	}
	for (int i = 0; i < len; i++)
		buf[i] = i2c_read(i + 1 < len);
	i2c_stop();
	return 1;
}
#endif

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* Diagnostics: reset without touching the framebuffer at all. */
static void __attribute__((noreturn)) reset_silent(unsigned after_s)
{
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static const char exc_kinds[4][8] = { "SYNC", "IRQ", "FIQ", "SERROR" };

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	/* A char array, not pointers: a string-pointer table would embed
	 * absolute addresses, and the image is linked at 0. */
	con_str("PANIC", exc_kinds[slot & 3]);
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

#ifdef TEXTREP
static char *app_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *app_hex8(char *p, uint32_t v)
{
	for (int i = 7; i >= 0; i--)
		*p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
	return p;
}
#endif

/* Debug: paint the top band of a candidate splash buffer. */
static void slot_band(uintptr_t base, uint32_t color, int rows)
{
	for (int y = 0; y < rows; y++) {
		volatile uint32_t *p = (volatile uint32_t *)(base + (uintptr_t)y * FB_STRIDE);
		for (int x = 0; x < FB_W; x++)
			p[x] = color;
	}
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

#ifdef TOUCHPROBE
	(void)dtb;
	{
		uint8_t b[8] = { 0 };
		uint32_t st, idle, recv, a20, a2c, a38, a00, a7e, swp;
		uint32_t r00 = 0, re9 = 0;
		uint32_t p23_before, p23_after;
		int irq2;

		con_config((void *)0x9d400000UL, 1080, 2160, 4320, 4);
		con_clear(0xff000060);
		con_str("TOUCH5", "TLMM TILES V5");

		/* GPIO23 VDDIO: record the TLMM latch and pad before and after.
		 * Do not change any PMIC/SPMI regulator here. */
		p23_before = treg(23)[0] & 0x3ffu;
		con_hex8("23BF", p23_before);
		tset(23, 1); tcfg(23, 0, 0, 2, 1); tudelay(5000);
		p23_after = (treg(23)[0] & 0x3ffu) |
			     ((treg(23)[1] & 3u) << 12);
		con_hex8("23AF", p23_after);
		tset(99, 0); tcfg(99, 0, 0, 8, 1); tudelay(10000);
		tset(99, 1); tudelay(200000);
		tcfg(125, 0, 3, 8, 0);

		/* Pass 1: SDA=33, SCL=34. */
		st = tp_selftest();
		con_hex8("SLFT", st);
		idle = bus_state() | ((uint32_t)tget(125) << 2);
		con_hex8("IDLE", idle);

		bus_recover();
		recv = bus_state();
		con_hex8("RECV", recv);

		if (recv == 3) {
			a20 = probe_addr(0x20);
			a2c = probe_addr(0x2c);
			a38 = probe_addr(0x38);
			a00 = probe_addr(0x00);
			a7e = probe_addr(0x7e);
		} else {
			a20 = a2c = a38 = a00 = a7e = 0;
		}
		con_hex8("A20", a20 | (a2c << 1) | (a38 << 2) |
			       (a00 << 3) | (a7e << 4));
		con_hex8("POST", bus_state());

		if (a20 && !a00 && !a7e) {
			r00 = rmi_read(0x20, 0x0000, b, 6);
			if (r00) {
				con_hex8("R00", ((uint32_t)b[0] << 24) |
					 ((uint32_t)b[1] << 16) |
					 ((uint32_t)b[2] << 8) | b[3]);
				con_hex8("R04", ((uint32_t)b[4] << 8) | b[5]);
			}
			for (int i = 0; i < 6; i++)
				b[i] = 0;
			re9 = rmi_read(0x20, 0x00e9, b, 6);
			if (re9) {
				con_hex8("RE9", ((uint32_t)b[0] << 24) |
					 ((uint32_t)b[1] << 16) |
					 ((uint32_t)b[2] << 8) | b[3]);
				con_hex8("RFN", b[5]);
			}
		}
		con_hex8("READ", r00 | (re9 << 1));
		if (a38) {
			uint8_t v[4] = { 0 };

			if (leg_read(0x38, 0xa8, v, 4))
				con_hex8("L38", ((uint32_t)v[0] << 24) |
					 ((uint32_t)v[1] << 16) |
					 ((uint32_t)v[2] << 8) | v[3]);
			else
				con_str("L38", "NACK");
		}

		/* Pass 2: swapped pins, in case 33/34 are labeled backwards. */
		sda_rel(); scl_rel();
		tsda = 34; tscl = 33;
		st = tp_selftest();
		a20 = probe_addr(0x20);
		a38 = probe_addr(0x38);
		swp = (st << 8) | (a20 << 1) | a38;
		con_hex8("SWP", swp);
		sda_rel(); scl_rel();
		tsda = 33; tscl = 34;

		irq2 = tget(125);
		con_hex8("IRQ2", (uint32_t)irq2);
		con_str("TOUCH2", "done, reset in 90s");
		delay_ms(90000);
		reset_silent(5);
	}
#endif
#ifdef HUGE
	(void)dtb;
	{
		/* Four giant 'A' glyphs per row, 30x scale, tiled down the region. */
		enum { GS = 30, GW = 8 * GS, GH = 16 * GS, SLOT = 270, BLOCK = GH + 80 };
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t nrows = 0x2400000UL / 4320;
		const uint32_t nwords = 0x2400000UL / 4;
		const unsigned char *g = font8x16['A' - 0x20];

		for (uint32_t i = 0; i < nwords; i++)
			fb[i] = 0xff000040U;

		for (uint32_t row = 0; row < nrows; row++) {
			uint32_t by = row % BLOCK;
			volatile uint32_t *dst = fb + (uintptr_t)row * 1080;

			for (uint32_t col = 0; col < 1080; col++) {
				uint32_t lx = col % SLOT;
				uint32_t c = 0xff000040U;

				if (by < GH && lx < GW) {
					uint32_t gx = lx / GS, gy = by / GS;
					if (g[gy] & (0x80 >> gx))
						c = 0xffffff00U;
				}
				dst[col] = c;
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(90000);
		reset_silent(5);
	}
#endif
#ifdef TEXTREP
	(void)dtb;
	/*
	 * Readable text without knowing the scanout base: the same 12-line block
	 * is tiled over the whole region in row-aligned units, so every screen
	 * window contains several complete copies. The lines carry the live SSPP
	 * registers, which a photo also reveals.
	 */
	{
		enum { NLINES = 12, BLOCK_ROWS = NLINES * 48, SLOT = 270, NCH = 11 };
		char text[NLINES][NCH + 1];
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t nwords = 0x2400000UL / 4;
		const uint32_t nrows = 0x2400000UL / 4320;

		for (uint32_t i = 0; i < nwords; i++)
			fb[i] = 0xff000040U;

		for (int l = 0; l < NLINES; l++)
			for (int c = 0; c < NCH; c++)
				text[l][c] = ' ';

		{
			static const char msg[NLINES][NCH + 1] = {
				"POLARIS OK ", "TEXT TEST  ", "SHIM RUNS  ",
				"NO MMIO    ", "LINE FOUR  ", "LINE FIVE  ",
				"LINE SIX   ", "LINE SEVEN ", "LINE EIGHT ",
				"LINE NINE  ", "LINE TEN   ", "END OF TEST",
			};
			for (int l = 0; l < NLINES; l++)
				for (int c = 0; c < NCH; c++)
					text[l][c] = msg[l][c];
		}

		/*
		 * Each line is repeated four times across the row. A base-address
		 * offset shifts and wraps the scanout, so at most one of the four
		 * copies can be cut; the others stay readable.
		 */
		for (uint32_t row = 0; row < nrows; row++) {
			uint32_t yin = row % BLOCK_ROWS;
			uint32_t li = yin / 48;
			uint32_t gy = yin % 48;
			const char *s = text[li];
			volatile uint32_t *dst = fb + (uintptr_t)row * 1080;

			for (uint32_t col = 0; col < 1080; col++) {
				uint32_t lx = col % SLOT;
				uint32_t ci = lx / 24;
				uint32_t gx = (lx % 24) / 3;
				char ch = ci < NCH ? s[ci] : ' ';
				uint32_t c = 0xff000040U;

				if (ch >= 0x20 && ch <= 0x7e) {
					const unsigned char *g =
						font8x16[(int)(ch - 0x20)];
					if (g[gy / 3] & (0x80 >> gx))
						c = 0xffffff00U;
				}
				dst[col] = c;
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(90000);
		reset_silent(5);
	}
#endif
#ifdef CAL4
	(void)dtb;
	/*
	 * Same block-index barcode as CAL3 but with a red gap, so the 256 KiB
	 * block boundary and the eight white/black bit bands are unambiguous in
	 * a phone photo.
	 */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t n = 0x2400000UL / 4;
		uint32_t x = 0;

		for (uint32_t w = 0; w < n; w++) {
			uint32_t rw = w & 0xffff;
			uint32_t k = w >> 16;
			uint32_t c;

			if (rw < 8 * 6 * 1080) {
				uint32_t band = rw / (6 * 1080);
				c = ((k >> (7 - band)) & 1) ? 0xffffffffU
							    : 0xff000000U;
			} else {
				c = 0xffff0000U;
			}
			if (x == 0)
				c = 0xff00ffffU;
			fb[w] = c;
			if (++x == 1080)
				x = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(60000);
		reset_silent(5);
	}
#endif
#ifdef UFSDUMP
	(void)dtb;
	/*
	 * No readable console yet, so persist the SDE/SSPP registers through a
	 * warm reset: one 4 KiB block at the start of cache (fallback misc),
	 * read back later from TWRP with adb.
	 */
	{
		static uint32_t dump[1024] __attribute__((aligned(4096)));
		static uint32_t back[1024] __attribute__((aligned(4096)));
		static uint32_t stat[1024] __attribute__((aligned(4096)));
		static const uint32_t sspp[8] = {
			0x5000, 0x7000, 0x9000, 0xb000,
			0x25000, 0x27000, 0x29000, 0x2b000,
		};
		/* misc starts at LBA 1024 (4 KiB units); stay clear of its BCB. */
		const uint32_t DLBA = 1030, SLBA = 1031;
		uint32_t n = 0, cap = 0;
		int rc_cap = ufs_capacity(&cap);

		dump[n++] = 0x53445250;	/* "PRDS" */
		dump[n++] = 2;
		dump[n++] = 8;
		dump[n++] = cap;
		for (int i = 0; i < 8; i++) {
			uintptr_t r = 0xae00000UL + sspp[i];
			dump[n++] = sspp[i];
			for (int j = 0; j < 15; j++)
				dump[n++] = rd32(r + j * 4);
		}
		for (int j = 0; j < 64; j++)
			dump[n++] = rd32(0xae00000UL + j * 4);
		for (int j = 0; j < 64; j++)
			dump[n++] = rd32(0xae01000UL + j * 4);

		int rcw = ufs_write_lba(DLBA, dump);
		int rcr = ufs_read_lba(DLBA, back);
		int same = 1;
		for (int i = 0; i < 64; i++)
			if (back[i] != dump[i])
				same = 0;

		stat[0] = 0x54415453;	/* "STAT" */
		stat[1] = (uint32_t)rc_cap;
		stat[2] = cap;
		stat[3] = (uint32_t)rcw;
		stat[4] = (uint32_t)rcr;
		stat[5] = (uint32_t)same;
		for (int i = 0; i < 16; i++)
			stat[6 + i] = back[i];
		(void)ufs_write_lba(SLBA, stat);

		delay_ms(5000);
		reset_silent(5);
	}
#endif
#ifdef CAL3
	(void)dtb;
	/*
	 * Coarse calibration for phone photos: each 256 KiB block starts with
	 * eight 6-row bands (white=1, black=0) spelling its block index 0..143,
	 * so one photo identifies the absolute address at that screen row.
	 * A cyan column at buffer x=0 gives base % 4320.
	 */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t n = 0x2400000UL / 4;
		uint32_t x = 0;

		for (uint32_t w = 0; w < n; w++) {
			uint32_t rw = w & 0xffff;
			uint32_t k = w >> 16;
			uint32_t c = 0xff202020U;

			if (rw < 8 * 6 * 1080) {
				uint32_t band = rw / (6 * 1080);
				c = ((k >> (7 - band)) & 1) ? 0xffe0e0e0U
							    : 0xff000000U;
			}
			if (x == 0)
				c = 0xff00ffffU;
			fb[w] = c;
			if (++x == 1080)
				x = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(60000);
		reset_silent(5);
	}
#endif
#ifdef CAL2
	(void)dtb;
	/*
	 * Absolute address calibration. A 1080x2160/4320 scanout is confirmed;
	 * only the base is unknown. Each 256 KiB block of the region starts with
	 * eight uniform rows encoding its block index (white=1, black=0), so the
	 * photo yields the absolute address shown at that screen row. Rows at
	 * every 64 KiB are white rulers, and a cyan column at buffer x=0 in every
	 * row encodes base % 4320 (it appears at a constant screen x).
	 */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t n = 0x2400000UL / 4;
		uint32_t x = 0;

		for (uint32_t w = 0; w < n; w++) {
			uint32_t rw = w & 0xffff;
			uint32_t k = w >> 16;
			uint32_t c = 0xff303030U;

			if (rw < 8640) {
				uint32_t row = rw / 1080;
				c = ((k >> (7 - row)) & 1) ? 0xffffffffU : 0xff000000U;
			} else if ((rw & 0x3fff) < 1080) {
				c = 0xffffffffU;
			}
			if (x == 0)
				c = 0xff00ffffU;
			fb[w] = c;
			if (++x == 1080)
				x = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(60000);
		reset_silent(5);
	}
#endif
#ifdef ADRMAP
	(void)dtb;
	/*
	 * Address-colour map. Each 32-bit word of the declared splash region is
	 * painted with R = word index >> 16 and G = (word index >> 8) & 0xff, so
	 * a photo of the panel decodes the physical addresses the MDP scans:
	 * red bands are 256 KiB apart (their spacing gives the stride), and the
	 * red/green level at the top-left gives the base.
	 */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t n = 0x2400000UL / 4;

		for (uint32_t w = 0; w < n; w++)
			fb[w] = 0xff000000U | (((w >> 16) & 0xff) << 16) |
				(((w >> 8) & 0xff) << 8);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ms(60000);
		reset_silent(5);
	}
#endif
#ifdef SDE_SCAN
	/*
	 * First run (fixed 1080x2160/4320 console) drew text at the first
	 * in-range pipe but with wrong geometry. Keep that same pipe choice,
	 * paint the whole splash region (proves we hit the scanout), then
	 * draw with the pipe's own SRC_SIZE/YSTRIDE/FORMAT values.
	 */
	(void)dtb;
	{
		static const uint32_t sspp[8] = {
			0x5000, 0x7000, 0x9000, 0xb000,
			0x25000, 0x27000, 0x29000, 0x2b000,
		};
		const uintptr_t lo = 0x9d400000UL, hi = 0x9f800000UL;
		volatile uint32_t *region = (volatile uint32_t *)lo;
		uint32_t addr[8], stride[8], fmt[8], ssz[8], sxy[8], osz[8], opm[8];
		int pi = -1;

		for (int i = 0; i < 8; i++) {
			uintptr_t r = 0xae00000UL + sspp[i];
			addr[i] = rd32(r + 0x14);
			stride[i] = rd32(r + 0x24) & 0xffff;
			fmt[i] = rd32(r + 0x30);
			ssz[i] = rd32(r + 0x00);
			sxy[i] = rd32(r + 0x08);
			osz[i] = rd32(r + 0x0c);
			opm[i] = rd32(r + 0x38);
			if (pi < 0 && addr[i] >= lo && addr[i] < hi &&
			    (uintptr_t)addr[i] + 0x8e6200UL <= hi)
				pi = i;
		}

		/* Paint the whole 36 MiB region: any live scanout must show it. */
		for (uint32_t i = 0; i < 0x2400000UL / 4; i++)
			region[i] = 0xff000060U;

		if (pi >= 0) {
			uint32_t w = ssz[pi] & 0xffff, h = ssz[pi] >> 16;
			int bpp = (int)((fmt[pi] >> 9) & 7) + 1;
			int st = (int)stride[pi];

			if (w < 200 || w > 2200)
				w = FB_W;
			if (h < 200 || h > 2400)
				h = FB_H;
			if (bpp < 2 || bpp > 4)
				bpp = 4;
			if (st < (int)w * bpp || st > 0xffff)
				st = (int)w * bpp;
			if ((uintptr_t)h * (uint32_t)st > hi - addr[pi])
				h = (uint32_t)((hi - addr[pi]) / (uint32_t)st);
			con_config((void *)(uintptr_t)addr[pi], (int)w, (int)h,
				   st, bpp);
			con_clear(0xff000060);
			con_str("SDE SCAN", "HW2");
			con_hex8("PIPE", (uint32_t)pi);
			con_hex8("ADDR", addr[pi]);
			con_hex8("STRD", (uint32_t)st);
			con_hex8("SRCS", ssz[pi]);
			con_hex8("SRCX", sxy[pi]);
			con_hex8("OUTS", osz[pi]);
			con_hex8("FMT", fmt[pi]);
			con_hex8("OPMD", opm[pi]);
		} else {
			con_config((void *)lo, FB_W, FB_H, FB_STRIDE, 4);
			con_str("SDE", "NO PIPE FOUND");
		}
		delay_ms(30000);
		reset_silent(5);
	}
#endif
#ifdef FULLFILL
	(void)dtb;
	/*
	 * The ABL logo survives while we run, so either MDP scans a buffer we
	 * never guessed or it is not scanning at all. Paint the whole declared
	 * 36 MiB splash region and read it back. Reset timing reports the
	 * result: ~30 s to fastboot = readback OK, ~75 s = readback failed.
	 */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		const uint32_t n = 0x2400000UL / 4;

		for (uint32_t i = 0; i < n; i++)
			fb[i] = 0xffff00ffU;
		__asm__ volatile("dsb sy" ::: "memory");
		int ok = fb[0] == 0xffff00ffU &&
			 fb[n / 2] == 0xffff00ffU &&
			 fb[n - 1] == 0xffff00ffU;
		delay_ms(25000);
		reset_silent(ok ? 5 : 45);
	}
#endif
#ifdef NOFB_RESET
	(void)dtb;
	reset_silent(5);
#endif

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", PLATFORM_NAME " milestone 4: xnu loader");
#ifdef SLOT_TEST
	/* 36 MiB splash region: probe four 9 MiB-aligned candidate frames. */
	slot_band(0x9d400000UL, RED, 240);
	slot_band(0x9dd00000UL, GREEN, 240);
	slot_band(0x9e600000UL, BLUE, 240);
	slot_band(0x9ef00000UL, WHITE, 240);
	con_str("DEBUG", "slot test 30s");
	delay_ms(30000);
	reset_silent(5);
#endif
#ifdef FAST_RESET
	con_str("DEBUG", "fast reset in 20s");
	delay_ms(20000);
	reset(5);
#endif
	char model[48];
	if (fdt_model(dtb, model, sizeof(model)))
		con_str("BOARD", model);
	uint64_t mem_base[4], mem_size[4];
	int nmem = fdt_memory_ranges(dtb, mem_base, mem_size, 4);
	for (int i = 0; i < nmem; i++)
		con_hex2("RAM", "hi=", (uint32_t)(mem_base[i] >> 32),
		    "lo=", (uint32_t)mem_base[i]);
	for (int i = 0; i < nmem; i++)
		con_hex2("RAM", "MiB=", (uint32_t)(mem_size[i] >> 20), "idx=", i);
	if (!nmem)
		con_str("RAM", "no /memory reg in DTB");
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	con_skip();
	/* Keep the probe results on screen (later tests clear it); reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
