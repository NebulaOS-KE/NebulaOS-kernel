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
 * Boot parameters to keep the bootloader's setting (for comparison):
 *   nebulaos_ddr.keep_pd=1   leave DDRC_CTRL.PDT as the bootloader set it
 *   nebulaos_ddr.keep_dlp=1  leave DDRC_DLP as the bootloader set it
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

static bool keep_pd;
module_param(keep_pd, bool, 0444);
MODULE_PARM_DESC(keep_pd, "keep the bootloader's DDR automatic power-down");
static bool keep_dlp;
module_param(keep_dlp, bool, 0444);
MODULE_PARM_DESC(keep_dlp, "keep the bootloader's DDRC_DLP low-power setting");

static char state[256];
module_param_string(state, state, sizeof(state), 0444);

static u32 nddr_rd(unsigned long base, unsigned int off)
{
	return __raw_readl((void __iomem *)(base + off));
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
	return 0;
}
arch_initcall(nebulaos_ddr_init);
