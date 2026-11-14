// SPDX-License-Identifier: GPL-2.0
/*
 * NebulaOS flight recorder - see include/linux/nebulaos_flightrec.h.
 *
 * Storage: the "nebulaos,flightrec" reserved-memory node (no-map), mapped
 * uncached, so every record is in DRAM the moment the store retires and a
 * watchdog reset (which does not clear DRAM) keeps it. Layout:
 *   page 0      header: magic, version, ncpu, entries per CPU, kernel banner
 *               CRC (so the next boot knows whether its symbols apply), and
 *               each CPU's next write index
 *   page 1..    one ring per possible CPU, NR entries of 16 bytes
 * A record is written a, b, type/arg, ts, then the index: a record cut short
 * by the freeze is the one at index (not yet counted) and is never shown.
 *
 * At early_initcall the previous boot's rings (if the magic is valid) are
 * copied to a buffer and shown at /proc/nebulaos_flightrec_prev; then the
 * area is reset and recording starts. /proc/nebulaos_flightrec shows the
 * live rings; writing a number to it logs a MARK record.
 *
 * Cost: one irq-save section and five uncached stores per event.
 */
#include <linux/crc32.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/nebulaos_flightrec.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>

#define NFR_MAGIC	0x4e465231	/* "NFR1" */
#define NFR_VERSION	2
#define NFR_HDR_SIZE	4096
#define NFR_MAX_CPUS	8
/* Last device-register accesses per CPU (readX/writeX, asm/io.h), in the
 * header page: at NFR_MMIO_OFF + cpu * NFR_MMIO_STRIDE a u32 count, a pad
 * word, then NFR_MMIO_N entries of {address, code address | 1 for a write}. */
#define NFR_MMIO_OFF	0x100
#define NFR_MMIO_N	16
#define NFR_MMIO_STRIDE	(8 + NFR_MMIO_N * 8)

struct nfr_hdr {
	u32 magic;
	u32 version;
	u32 ncpu;
	u32 nent;		/* entries per CPU, power of two */
	u32 banner_crc;
	u32 boot_ts;		/* sched_clock() >> 10 when recording started */
	u32 reserved[2];
	u32 idx[NFR_MAX_CPUS];	/* next slot per CPU (monotonic, masked on use) */
};

struct nfr_rec {
	u32 ts;			/* sched_clock() >> 10 (units of 1.024 us) */
	u16 type;
	u16 arg;
	u32 a;
	u32 b;
};

int nebulaos_fr_enabled __read_mostly;
EXPORT_SYMBOL_GPL(nebulaos_fr_enabled);

static void __iomem *nfr_base;
static u32 nfr_ncpu, nfr_nent;
static DEFINE_PER_CPU(u32, nfr_idx);
static DEFINE_PER_CPU(u32, nfr_mmio_idx);

static void *nfr_prev;		/* copy of the previous boot's area, or NULL */
static size_t nfr_size;
static bool nfr_param_off;

static int __init nfr_param(char *s)
{
	nfr_param_off = s && (s[0] == '0' || s[0] == 'n' || s[0] == 'N');
	return 1;
}
__setup("nebulaos_flightrec=", nfr_param);

static u32 nfr_banner_crc(void)
{
	return crc32(0, linux_banner, strlen(linux_banner));
}

static inline struct nfr_rec __iomem *nfr_slot(int cpu, u32 idx)
{
	return (struct nfr_rec __iomem *)(nfr_base + NFR_HDR_SIZE) +
		(size_t)cpu * nfr_nent + (idx & (nfr_nent - 1));
}

void __nebulaos_fr_log(unsigned int type, unsigned int arg,
		       unsigned long a, unsigned long b)
{
	struct nfr_rec __iomem *r;
	unsigned long flags;
	int cpu;
	u32 idx;

	local_irq_save(flags);
	cpu = raw_smp_processor_id();
	if (unlikely(cpu >= nfr_ncpu))
		goto out;
	idx = __this_cpu_read(nfr_idx);
	r = nfr_slot(cpu, idx);
	__raw_writel((u32)a, &r->a);
	__raw_writel((u32)b, &r->b);
	__raw_writel((type & 0xffff) | ((arg & 0xffff) << 16), &r->type);
	__raw_writel((u32)(sched_clock() >> 10), &r->ts);
	idx++;
	__this_cpu_write(nfr_idx, idx);
	__raw_writel(idx, &((struct nfr_hdr __iomem *)nfr_base)->idx[cpu]);
out:
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__nebulaos_fr_log);

/* Called from readX/writeX BEFORE the access (asm/io.h). The barrier makes
 * the note reach DRAM before the device access is issued, so a CPU stalled
 * on that access has already recorded it. */
void __nebulaos_fr_mmio(unsigned long addr, unsigned int write, unsigned long ip)
{
	u32 __iomem *e;
	unsigned long flags;
	int cpu;
	u32 i;

	local_irq_save(flags);
	cpu = raw_smp_processor_id();
	if (likely(cpu < nfr_ncpu)) {
		i = __this_cpu_read(nfr_mmio_idx);
		e = nfr_base + NFR_MMIO_OFF + cpu * NFR_MMIO_STRIDE;
		__raw_writel((u32)addr, e + 2 + (i % NFR_MMIO_N) * 2);
		__raw_writel((u32)ip | (write ? 1 : 0), e + 3 + (i % NFR_MMIO_N) * 2);
		__this_cpu_write(nfr_mmio_idx, i + 1);
		__raw_writel(i + 1, e);
		mb();
	}
	local_irq_restore(flags);
}
EXPORT_SYMBOL(__nebulaos_fr_mmio);

/* ---- decoding ----------------------------------------------------------- */

static const char *const nfr_names[NFR_NR_TYPES] = {
	[NFR_NONE] = "?", [NFR_IRQ_IN] = "IRQ>", [NFR_IRQ_OUT] = "IRQ<",
	[NFR_THR_IN] = "THR>", [NFR_THR_OUT] = "THR<",
	[NFR_SIRQ_IN] = "SIRQ>", [NFR_SIRQ_OUT] = "SIRQ<",
	[NFR_TMR_IN] = "TMR>", [NFR_TMR_OUT] = "TMR<",
	[NFR_WQ_IN] = "WQ>", [NFR_WQ_OUT] = "WQ<",
	[NFR_SWITCH] = "SW", [NFR_MARK] = "MARK",
};

/* A text address is printed as a symbol only when the image that wrote it is
 * the running one (same banner CRC); otherwise as hex, to be resolved with
 * that build's System.map. */
static void nfr_addr(struct seq_file *m, u32 v, bool sym)
{
	if (sym && v >= 0x80000000u)
		seq_printf(m, " %pS", (void *)(unsigned long)v);
	else
		seq_printf(m, " 0x%08x", v);
}

static void nfr_show_area(struct seq_file *m, const void *area, const char *what)
{
	const struct nfr_hdr *h = area;
	bool sym;
	u32 newest = 0;
	int cpu;

	if (h->magic != NFR_MAGIC || h->version != NFR_VERSION ||
	    !h->ncpu || h->ncpu > NFR_MAX_CPUS || !h->nent ||
	    (h->nent & (h->nent - 1)) ||
	    NFR_HDR_SIZE + (size_t)h->ncpu * h->nent * sizeof(struct nfr_rec) > nfr_size) {
		seq_printf(m, "%s: no valid recording\n", what);
		return;
	}
	sym = h->banner_crc == nfr_banner_crc();
	seq_printf(m, "%s: ncpu=%u entries/cpu=%u symbols=%s\n", what, h->ncpu,
		   h->nent, sym ? "this kernel" : "OTHER kernel (raw addresses)");
	seq_puts(m, "ts in units of 1.024 us; dt = time before the newest record (ms)\n");

	for (cpu = 0; cpu < h->ncpu; cpu++) {
		u32 idx = h->idx[cpu];
		if (idx) {
			const struct nfr_rec *r = (const struct nfr_rec *)(area + NFR_HDR_SIZE) +
				(size_t)cpu * h->nent + ((idx - 1) & (h->nent - 1));
			if ((s32)(r->ts - newest) > 0 || !newest)
				newest = r->ts;
		}
	}

	for (cpu = 0; cpu < h->ncpu; cpu++) {
		u32 end = h->idx[cpu];
		u32 n = min(end, h->nent);
		u32 i;

		seq_printf(m, "== cpu%d: %u events recorded, last %u shown (oldest first)\n",
			   cpu, end, n);
		for (i = end - n; i != end; i++) {
			const struct nfr_rec *r = (const struct nfr_rec *)(area + NFR_HDR_SIZE) +
				(size_t)cpu * h->nent + (i & (h->nent - 1));
			unsigned int t = r->type < NFR_NR_TYPES ? r->type : 0;
			u32 dt = newest - r->ts;	/* 1.024 us units */
			u64 us = div_u64((u64)dt * 1024, 1000);
			u32 us_rem;
			u64 ms = div_u64_rem(us, 1000, &us_rem);

			seq_printf(m, "cpu%d %10u dt=%7llu.%03u %-5s %5u", cpu, r->ts,
				   ms, us_rem, nfr_names[t], r->arg);
			switch (t) {
			case NFR_IRQ_IN:
				nfr_addr(m, r->a, sym);
				nfr_addr(m, r->b, sym);
				break;
			case NFR_THR_IN: case NFR_SIRQ_IN: case NFR_TMR_IN:
			case NFR_TMR_OUT: case NFR_WQ_OUT:
				nfr_addr(m, r->a, sym);
				break;
			case NFR_WQ_IN:
				nfr_addr(m, r->a, sym);
				seq_printf(m, " work=0x%08x", r->b);
				break;
			case NFR_SWITCH: {
				char c[5];
				memcpy(c, &r->b, 4);
				c[4] = 0;
				seq_printf(m, " -> pid %u (%.4s)", r->a, c);
				break;
			}
			default:
				break;
			}
			seq_putc(m, '\n');
		}
	}

	for (cpu = 0; cpu < h->ncpu; cpu++) {
		const u32 *e = area + NFR_MMIO_OFF + cpu * NFR_MMIO_STRIDE;
		u32 end = e[0], n = min_t(u32, end, NFR_MMIO_N), i;

		seq_printf(m, "== cpu%d: last %u of %u device register accesses (oldest first; the last one may never have completed)\n",
			   cpu, n, end);
		for (i = end - n; i != end; i++) {
			u32 a = e[2 + (i % NFR_MMIO_N) * 2], ip = e[3 + (i % NFR_MMIO_N) * 2];

			seq_printf(m, "cpu%d MMIO %c 0x%08x", cpu, (ip & 1) ? 'W' : 'R', a);
			if (a >= 0xa0000000u && a < 0xc0000000u)
				seq_printf(m, " phys=0x%08x", a & 0x1fffffffu);
			nfr_addr(m, ip & ~1u, sym);
			seq_putc(m, '\n');
		}
	}
}

static int nfr_prev_show(struct seq_file *m, void *v)
{
	if (!nfr_prev)
		seq_puts(m, "previous boot: no valid recording\n");
	else
		nfr_show_area(m, nfr_prev, "previous boot");
	return 0;
}

static int nfr_live_show(struct seq_file *m, void *v)
{
	void *snap;

	if (!nfr_base) {
		seq_puts(m, "flight recorder off\n");
		return 0;
	}
	snap = vmalloc(nfr_size);
	if (!snap)
		return -ENOMEM;
	memcpy_fromio(snap, nfr_base, nfr_size);
	nfr_show_area(m, snap, "this boot");
	vfree(snap);
	return 0;
}

static int nfr_live_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, nfr_live_show, NULL, 4 << 20);
}

static ssize_t nfr_live_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	unsigned int v;
	int ret = kstrtouint_from_user(buf, count, 0, &v);

	if (ret)
		return ret;
	nebulaos_fr_log(NFR_MARK, v, 0, 0);
	return count;
}

static int nfr_prev_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, nfr_prev_show, NULL, 4 << 20);
}

static const struct proc_ops nfr_live_ops = {
	.proc_open = nfr_live_open,
	.proc_read = seq_read,
	.proc_write = nfr_live_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops nfr_prev_ops = {
	.proc_open = nfr_prev_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ---- setup -------------------------------------------------------------- */

static int __init nfr_init(void)
{
	struct device_node *np;
	struct resource res;
	struct nfr_hdr __iomem *h;
	u32 ncpu = min_t(u32, num_possible_cpus(), NFR_MAX_CPUS);
	u32 nent;
	int cpu;

	np = of_find_compatible_node(NULL, NULL, "nebulaos,flightrec");
	if (!np) {
		pr_info("nebulaos-flightrec: no reserved-memory node, off\n");
		return 0;
	}
	if (!of_property_read_bool(np, "no-map") || of_address_to_resource(np, 0, &res)) {
		pr_err("nebulaos-flightrec: node must be no-map reserved memory with reg, off\n");
		of_node_put(np);
		return 0;
	}
	of_node_put(np);
	nfr_size = resource_size(&res);
	if (nfr_size < NFR_HDR_SIZE + 4096) {
		pr_err("nebulaos-flightrec: area too small, off\n");
		return 0;
	}
	nent = rounddown_pow_of_two((nfr_size - NFR_HDR_SIZE) / ncpu / sizeof(struct nfr_rec));

	nfr_base = ioremap(res.start, nfr_size);	/* MIPS: uncached */
	if (!nfr_base) {
		pr_err("nebulaos-flightrec: ioremap failed, off\n");
		return 0;
	}
	h = nfr_base;

	if (__raw_readl(&h->magic) == NFR_MAGIC) {
		nfr_prev = vmalloc(nfr_size);
		if (nfr_prev)
			memcpy_fromio(nfr_prev, nfr_base, nfr_size);
	}

	proc_create("nebulaos_flightrec_prev", 0400, NULL, &nfr_prev_ops);

	if (nfr_param_off) {
		pr_info("nebulaos-flightrec: disabled by nebulaos_flightrec=0 (previous boot kept)\n");
		return 0;
	}

	memset_io(nfr_base, 0, NFR_HDR_SIZE);
	__raw_writel(NFR_VERSION, &h->version);
	__raw_writel(ncpu, &h->ncpu);
	__raw_writel(nent, &h->nent);
	__raw_writel(nfr_banner_crc(), &h->banner_crc);
	__raw_writel((u32)(sched_clock() >> 10), &h->boot_ts);
	for_each_possible_cpu(cpu)
		per_cpu(nfr_idx, cpu) = 0;
	nfr_ncpu = ncpu;
	nfr_nent = nent;
	wmb();
	__raw_writel(NFR_MAGIC, &h->magic);
	proc_create("nebulaos_flightrec", 0600, NULL, &nfr_live_ops);
	smp_wmb();
	WRITE_ONCE(nebulaos_fr_enabled, 1);
	pr_info("nebulaos-flightrec: recording at %pa, %u cpus x %u events; previous boot %s\n",
		&res.start, ncpu, nent, nfr_prev ? "saved" : "had no recording");
	return 0;
}
early_initcall(nfr_init);
