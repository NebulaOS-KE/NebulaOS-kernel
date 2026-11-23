// SPDX-License-Identifier: GPL-2.0
/*
 * NebulaOS (2026-11-22, #27 mid-print freeze, test build 3): switch the DDR
 * controller's automatic low-power modes off at boot.
 *
 * Creality's bootloader leaves DDRC_CTRL.PDT = 3 (power-down after 32 idle
 * tCK, ACTPD = 1) and DDRC_DLP = 0xFF009306 on; auto self-refresh is off
 * (read on ke-dev during a print). Every freeze recording shows both hardware
 * threads stopping at once mid-instruction with no device access pending -
 * the signature of a memory access that never completes. The vendor's own
 * suspend path (pm_sleep.c) clears PDT and DLP before it touches the DDR
 * power state. Test build 3 keeps the RAM out of automatic power-down.
 *
 * Test build 5: test3 froze with both off (ukiduki1, ke-dev twice), so the
 * default is back to the bootloader's (stock) setting. Boot parameters to
 * switch them off again:
 *   nebulaos_ddr.keep_pd=0   clear DDRC_CTRL.PDT
 *   nebulaos_ddr.keep_dlp=0  clear DDRC_DLP
 * Every boot also logs the full DDR controller, DDR PHY and clock setup
 * (read only), to compare printers and the Ingenic reference values.
 * The values before and after are logged and readable at
 * /sys/module/nebulaos_ddr/parameters/state.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>

#define NDDR_DDRC	0xb34f0000	/* DDRC, KSEG1 (soc/ddr.h DDRC_BASE) */
#define NDDR_APB	0xb3012000	/* DDRC APB block */
#define NDDR_CPM	0xb0000000

#define DDRC_STATUS	0x00
#define DDRC_CTRL	0x10
#define DDRC_DLP	0x20
#define DDRC_AUTOSR_EN	0x28
#define DDRC_DBGINFO	0xe8
#define DDRC_CTRL_PDT_MASK	(0x7 << 12)
#define APB_CGUC0	0x64
#define APB_CGUC1	0x68
#define CPM_DDRCDR	0x2c

static bool keep_pd = true;
module_param(keep_pd, bool, 0444);
MODULE_PARM_DESC(keep_pd, "keep the bootloader's DDR automatic power-down");
static bool keep_dlp = true;
module_param(keep_dlp, bool, 0444);
MODULE_PARM_DESC(keep_dlp, "keep the bootloader's DDRC_DLP low-power setting");

static char state[256];
module_param_string(state, state, sizeof(state), 0444);

static u32 nddr_rd(unsigned long base, unsigned int off)
{
	return __raw_readl((void __iomem *)(base + off));
}

#define NDDR_PHY	0xb3011000	/* DDR PHY (Innophy) */
#define NDDR_MSCR	0xb2200060	/* CCU MSCR (L2 prefetcher control) */

static const unsigned short nddr_ctl[] = {
	0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,		/* STATUS CFG CTRL LMR DLP AUTOSR_EN/CNT REFCNT */
	0x40, 0x48, 0x50, 0x58, 0x60, 0x68,			/* TIMING1-6 */
	0x78, 0x80, 0xd8, 0xe8,					/* MMAP0 MMAP1 HREGPRO DBGINFO */
};
static const unsigned short nddr_apb[] = {
	0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,		/* DWCFG DWSTATUS REMAP1-5 */
	0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,	/* CCHC0-7 (priorities, bandwidth limits) */
	0x64, 0x68, 0x6c,					/* CGUC0 CGUC1 PREGPRO */
};
static const unsigned short nddr_phy[] = {
	0x000, 0x004, 0x008, 0x014, 0x018, 0x01c,		/* RST MEM_CFG TRAINING CL AL CWL */
	0x040, 0x044, 0x048, 0x04c, 0x07c,			/* CMD/CK drive comp, DQ_WIDTH */
	0x080, 0x084, 0x088, 0x0c8, 0x0cc,			/* PLL FBDIV CTRL PDIV LOCK, CALIB_DONE */
	0x100, 0x104, 0x108, 0x10c, 0x118,			/* ODT/drive bank 0, CALIB_BYPASS_AL */
	0x140, 0x144, 0x148, 0x14c, 0x158,			/* ODT/drive bank 1, CALIB_BYPASS_AH */
	0x190, 0x194,						/* CALIB_DELAY_AL/AH (DQS training result) */
};
static const unsigned short nddr_cpm[] = {
	0x00, 0x10, 0x14, 0x18, 0x2c, 0xd4,			/* CPCCR CPAPCR CPMPCR CPEPCR DDRCDR CPCSR */
};

static void __init nddr_dump(const char *name, unsigned long base,
			     const unsigned short *off, int n)
{
	char line[200];
	int i, len = 0;

	for (i = 0; i < n; i++) {
		len += scnprintf(line + len, sizeof(line) - len, " %03x=%08x",
				 off[i], nddr_rd(base, off[i]));
		if (len > 150 || i == n - 1) {
			pr_info("nebulaos-ddr: %s%s\n", name, line);
			len = 0;
		}
	}
}

static int __init nebulaos_ddr_init(void)
{
	u32 st = nddr_rd(NDDR_DDRC, DDRC_STATUS);
	u32 dbg = nddr_rd(NDDR_DDRC, DDRC_DBGINFO);
	u32 ctrl = nddr_rd(NDDR_DDRC, DDRC_CTRL);
	u32 dlp = nddr_rd(NDDR_DDRC, DDRC_DLP);
	u32 asr = nddr_rd(NDDR_DDRC, DDRC_AUTOSR_EN);
	u32 cg0 = nddr_rd(NDDR_APB, APB_CGUC0);
	u32 cg1 = nddr_rd(NDDR_APB, APB_CGUC1);
	u32 cdr = nddr_rd(NDDR_CPM, CPM_DDRCDR);
	u32 ctrl2 = ctrl, dlp2 = dlp;

	if (!keep_pd && (ctrl & DDRC_CTRL_PDT_MASK)) {
		ctrl2 = ctrl & ~DDRC_CTRL_PDT_MASK;
		__raw_writel(ctrl2, (void __iomem *)(NDDR_DDRC + DDRC_CTRL));
	}
	if (!keep_dlp && dlp) {
		dlp2 = 0;
		__raw_writel(dlp2, (void __iomem *)(NDDR_DDRC + DDRC_DLP));
	}
	wmb();
	ctrl2 = nddr_rd(NDDR_DDRC, DDRC_CTRL);
	dlp2 = nddr_rd(NDDR_DDRC, DDRC_DLP);

	snprintf(state, sizeof(state),
		 "boot: STATUS=0x%08x DBGINFO=0x%08x CTRL=0x%08x DLP=0x%08x AUTOSR_EN=0x%08x "
		 "CGUC0=0x%08x CGUC1=0x%08x DDRCDR=0x%08x now: CTRL=0x%08x DLP=0x%08x pd=%s dlp=%s",
		 st, dbg, ctrl, dlp, asr, cg0, cg1, cdr, ctrl2, dlp2,
		 (ctrl2 & DDRC_CTRL_PDT_MASK) ? "on" : "off", dlp2 ? "on" : "off");
	pr_info("nebulaos-ddr: %s\n", state);
	nddr_dump("DDRC", NDDR_DDRC, nddr_ctl, ARRAY_SIZE(nddr_ctl));
	nddr_dump("DDRC-APB", NDDR_APB, nddr_apb, ARRAY_SIZE(nddr_apb));
	nddr_dump("DDRP", NDDR_PHY, nddr_phy, ARRAY_SIZE(nddr_phy));
	nddr_dump("CPM", NDDR_CPM, nddr_cpm, ARRAY_SIZE(nddr_cpm));
	nddr_dump("CCU-MSCR", NDDR_MSCR, nddr_cpm, 1);
	return 0;
}
arch_initcall(nebulaos_ddr_init);
