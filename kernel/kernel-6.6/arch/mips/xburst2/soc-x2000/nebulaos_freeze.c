// SPDX-License-Identifier: GPL-2.0
/*
 * NebulaOS #27 (test build 5): runtime switches for the mid-print freeze
 * candidates, so each can be tested on its own on one printer without a
 * rebuild. /sys/module/nebulaos_freeze/parameters/<name> (root only):
 *
 *   l2_pre_iob   1 (default): uncached load before the L2 invalidate ops
 *                (XBurst II L2 prefetcher erratum, Hunter Laux 2026-11-11).
 *                0: as test3.
 *   l2_prefetch  1 (default, hardware reset state): L2 prefetcher on.
 *                0: off (CCU MSCR bit 1, "DisPFB2" on the X2600; the X2000
 *                manual does not document MSCR).
 *   idle_poll    0 (default): idle threads wait with WAIT. 1: they spin.
 *   cpu_mhz      1200 (default) or 750 (MPLL / 2). The L2 stays at half the
 *                CPU clock (375 MHz at 750), so no clock goes up.
 *   blackbox     1 (default): the flight recorder records. 0: paused.
 *
 * The vendor cpufreq driver changes the CPU clock the same way at runtime;
 * it is not enabled in NebulaOS (CONFIG_X2000_CPUFREQ is off).
 */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/nebulaos_flightrec.h>
#include <linux/smp.h>

#define NFZ_MSCR		((void __iomem *)0xb2200060)	/* CCU MSCR, KSEG1 */
#define NFZ_MSCR_DISPFB2	(1u << 1)

extern bool nebulaos_l2_pre_iob;	/* arch/mips/xburst2/core/sc.c */
extern int nebulaos_fr_mmio_enabled;	/* kernel/nebulaos_flightrec.c */

module_param_named(l2_pre_iob, nebulaos_l2_pre_iob, bool, 0644);
MODULE_PARM_DESC(l2_pre_iob, "uncached load before the L2 invalidate ops (1 = on)");

/* ---- l2_prefetch -------------------------------------------------------- */

static int nfz_prefetch_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	u32 before, after;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	before = __raw_readl(NFZ_MSCR);
	after = on ? before & ~NFZ_MSCR_DISPFB2 : before | NFZ_MSCR_DISPFB2;
	if (after != before) {
		__raw_writel(after, NFZ_MSCR);
		wmb();
	}
	pr_info("nebulaos-freeze: l2_prefetch=%d MSCR 0x%08x -> 0x%08x\n",
		on, before, __raw_readl(NFZ_MSCR));
	return 0;
}

static int nfz_prefetch_get(char *buf, const struct kernel_param *kp)
{
	u32 v = __raw_readl(NFZ_MSCR);

	return sprintf(buf, "%d (MSCR=0x%08x)\n", !(v & NFZ_MSCR_DISPFB2), v);
}

static const struct kernel_param_ops nfz_prefetch_ops = {
	.set = nfz_prefetch_set,
	.get = nfz_prefetch_get,
};
module_param_cb(l2_prefetch, &nfz_prefetch_ops, NULL, 0644);
MODULE_PARM_DESC(l2_prefetch, "L2 hardware prefetcher (1 = on, hardware default)");

/* ---- idle_poll ---------------------------------------------------------- */

static bool nfz_idle_poll;

static int nfz_idle_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (on != nfz_idle_poll) {
		cpu_idle_poll_ctrl(on);	/* counted: called once per change */
		nfz_idle_poll = on;
		wake_up_all_idle_cpus();
	}
	pr_info("nebulaos-freeze: idle_poll=%d\n", nfz_idle_poll);
	return 0;
}

static int nfz_idle_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d\n", nfz_idle_poll);
}

static const struct kernel_param_ops nfz_idle_ops = {
	.set = nfz_idle_set,
	.get = nfz_idle_get,
};
module_param_cb(idle_poll, &nfz_idle_ops, NULL, 0644);
MODULE_PARM_DESC(idle_poll, "idle threads spin instead of WAIT (1 = spin)");

/* ---- cpu_mhz ------------------------------------------------------------ */

static int nfz_cpu_rate(struct clk **div)
{
	*div = clk_get(NULL, "div_cpu_l2c_x2");
	if (IS_ERR(*div))
		return PTR_ERR(*div);
	return 0;
}

static int nfz_cpu_set(const char *val, const struct kernel_param *kp)
{
	struct clk *mux, *sclka, *mpll, *div, *parent;
	unsigned int mhz;
	unsigned long before, rate;
	int ret = kstrtouint(val, 0, &mhz);

	if (ret)
		return ret;
	if (mhz != 1200 && mhz != 750)
		return -EINVAL;

	mux = clk_get(NULL, "mux_cpu_l2c");
	sclka = clk_get(NULL, "sclka");
	mpll = clk_get(NULL, "mpll");
	ret = nfz_cpu_rate(&div);
	if (IS_ERR(mux) || IS_ERR(sclka) || IS_ERR(mpll) || ret) {
		pr_err("nebulaos-freeze: cpu clocks not found\n");
		ret = -ENODEV;
		goto out;
	}

	before = clk_get_rate(div);
	rate = mhz * 1000000UL;
	parent = mhz == 750 ? mpll : sclka;
	if (before != rate) {
		/* Order of the vendor ingenic_cpufreq_target(): divider for the
		 * current parent (never above the target), then the parent,
		 * then the final divider. */
		clk_set_rate(div, clk_round_rate(div, rate));
		clk_set_parent(mux, parent);
		clk_set_rate(div, rate);
	}
	pr_info("nebulaos-freeze: cpu_mhz=%u: %lu -> %lu Hz (L2 at half)\n",
		mhz, before, clk_get_rate(div));
	ret = clk_get_rate(div) == rate ? 0 : -EIO;
out:
	if (!IS_ERR_OR_NULL(div))
		clk_put(div);
	if (!IS_ERR(mpll))
		clk_put(mpll);
	if (!IS_ERR(sclka))
		clk_put(sclka);
	if (!IS_ERR(mux))
		clk_put(mux);
	return ret;
}

static int nfz_cpu_get(char *buf, const struct kernel_param *kp)
{
	struct clk *div;
	unsigned long rate = 0;

	if (!nfz_cpu_rate(&div)) {
		rate = clk_get_rate(div);
		clk_put(div);
	}
	return sprintf(buf, "%lu\n", rate / 1000000);
}

static const struct kernel_param_ops nfz_cpu_ops = {
	.set = nfz_cpu_set,
	.get = nfz_cpu_get,
};
module_param_cb(cpu_mhz, &nfz_cpu_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_mhz, "CPU clock: 1200 (default) or 750");

/* ---- blackbox ----------------------------------------------------------- */

static int nfz_bb_saved = -1, nfz_bb_mmio_saved;

static int nfz_bb_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (!on && nfz_bb_saved < 0) {
		nfz_bb_saved = READ_ONCE(nebulaos_fr_enabled);
		nfz_bb_mmio_saved = READ_ONCE(nebulaos_fr_mmio_enabled);
		WRITE_ONCE(nebulaos_fr_mmio_enabled, 0);
		WRITE_ONCE(nebulaos_fr_enabled, 0);
	} else if (on && nfz_bb_saved >= 0) {
		WRITE_ONCE(nebulaos_fr_enabled, nfz_bb_saved);
		WRITE_ONCE(nebulaos_fr_mmio_enabled, nfz_bb_mmio_saved);
		nfz_bb_saved = -1;
	}
	pr_info("nebulaos-freeze: blackbox=%d (recorder %d, register log %d)\n", on,
		READ_ONCE(nebulaos_fr_enabled), READ_ONCE(nebulaos_fr_mmio_enabled));
	return 0;
}

static int nfz_bb_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d\n", nfz_bb_saved < 0);
}

static const struct kernel_param_ops nfz_bb_ops = {
	.set = nfz_bb_set,
	.get = nfz_bb_get,
};
module_param_cb(blackbox, &nfz_bb_ops, NULL, 0644);
MODULE_PARM_DESC(blackbox, "flight recorder records (1 = on, default)");

static int __init nebulaos_freeze_init(void)
{
	struct clk *div;
	unsigned long rate = 0;

	if (!nfz_cpu_rate(&div)) {
		rate = clk_get_rate(div);
		clk_put(div);
	}
	pr_info("nebulaos-freeze: switches l2_pre_iob=%d MSCR=0x%08x idle_poll=%d cpu=%lu Hz blackbox=%d\n",
		nebulaos_l2_pre_iob, __raw_readl(NFZ_MSCR), nfz_idle_poll, rate,
		READ_ONCE(nebulaos_fr_enabled));
	return 0;
}
late_initcall(nebulaos_freeze_init);
