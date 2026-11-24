// SPDX-License-Identifier: GPL-2.0
/*
 * NebulaOS flight recorder - see include/linux/nebulaos_flightrec.h.
 *
 * Storage: the "nebulaos,flightrec" reserved-memory node (no-map), mapped
 * uncached, so every record is in DRAM the moment the store retires and a
 * watchdog reset (which does not clear DRAM) keeps it. Layout (v3):
 *   page 0      header: magic, version, ncpu, entries per CPU, kernel banner
 *               CRC (so the next boot knows whether its symbols apply), each
 *               CPU's next event index and next register-log index
 *   page 1..    register-log rings, NFR_MMIO_N entries of 16 bytes per CPU:
 *               {event index, address, code address | 1 write | 2 done, value}
 *   then        (v4) transfer-log rings, NFR_XFER_N records of 16 bytes per
 *               CPU: USB channel starts/halts/channel interrupts, SD/MMC
 *               commands/interrupts/completions and a once-a-second clock
 *               pair (boottime, wall clock) - kept apart from the event ring,
 *               which the 8 kHz USB SOF interrupts fill in about a second
 *   then        one event ring per possible CPU, NR entries of 16 bytes
 * A record is written a, b, type/arg, ts, then the index: a record cut short
 * by the freeze is the one at index (not yet counted) and is never shown.
 *
 * At early_initcall the previous boot's rings (if the magic is valid) are
 * copied to a buffer and shown at /proc/nebulaos_flightrec_prev; then the
 * area is reset and recording starts. /proc/nebulaos_flightrec shows the
 * live rings; writing a number to it logs a MARK record.
 *
 * Two levels (1.0.5). The event ring and the once-a-second clock pair always
 * record. The DETAIL level adds what costs more: the device-register log
 * (readX/writeX, with a full barrier per access), the transfer log (USB
 * channels, SD/MMC commands) and the inter-CPU interrupt/callback events.
 * Detail is off at boot unless booted with nebulaos_flightrec_detail=1;
 * NebulaOS switches it with the log level (Debug and Verbose = on): write
 * "detail=0" or "detail=1" to /proc/nebulaos_flightrec. "mmio=0|1" switches
 * the register log alone (for A/B tests). Each change is logged as a CTL
 * record; the header keeps the current state (bit 0 register log, bit 1
 * detail), so a recording from a frozen boot says what was recorded.
 *
 * Cost: one irq-save section and five uncached stores per event.
 */
#include <linux/crc32.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/nebulaos_flightrec.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>

#define NFR_MAGIC	0x4e465231	/* "NFR1" */
#define NFR_VERSION	4
#define NFR_HDR_SIZE	4096
#define NFR_MAX_CPUS	8
/* Last device-register accesses per CPU (readX/writeX, asm/io.h). 2048 is
 * about the last 15 ms of a busy USB host. Power of two. */
#define NFR_MMIO_N	2048
#define NFR_MMIO_ENT	16
/* Transfer-log records per CPU (v4). Power of two. Wi-Fi (SDIO) alone can
 * add ~10000 records/s while a browser streams the camera, so the ring is
 * backed by the per-channel/per-host state slots below. */
#define NFR_XFER_N	8192
/* v4 state slots: the newest transfer-log record per source, so the last
 * start/interrupt/completion of every USB channel and MMC host survives
 * however busy the others are. 16 USB channels x 3 + 4 MMC hosts x 3 + CLK. */
#define NFR_ST_USB_START	0
#define NFR_ST_USB_HALT		16
#define NFR_ST_USB_CHINT	32
#define NFR_ST_MMC_CMD		48
#define NFR_ST_MMC_IRQ		52
#define NFR_ST_MMC_DONE		56
#define NFR_ST_CLK		60
#define NFR_ST_N		64

struct nfr_rec {
	u32 ts;			/* sched_clock() >> 10 (units of 1.024 us) */
	u16 type;
	u16 arg;
	u32 a;
	u32 b;
};

struct nfr_hdr {
	u32 magic;
	u32 version;
	u32 ncpu;
	u32 nent;		/* entries per CPU, power of two */
	u32 banner_crc;
	u32 boot_ts;		/* sched_clock() >> 10 when recording started */
	u32 mmio_on;		/* bit 0: register log on; bit 1: detail level on */
	u32 mmio_n;		/* register-log entries per CPU, power of two */
	u32 idx[NFR_MAX_CPUS];	/* next slot per CPU (monotonic, masked on use) */
	u32 mmio_idx[NFR_MAX_CPUS]; /* next register-log slot per CPU */
	u32 xfer_n;		/* v4: transfer-log records per CPU, power of two */
	u32 xfer_idx[NFR_MAX_CPUS]; /* v4: next transfer-log slot per CPU */
	u32 st_cpu[NFR_ST_N];	/* v4: CPU that wrote each state slot */
	struct nfr_rec st[NFR_ST_N]; /* v4: newest record per source (see NFR_ST_*) */
};


int nebulaos_fr_enabled __read_mostly;
EXPORT_SYMBOL_GPL(nebulaos_fr_enabled);
/* asm/io.h notes device-register accesses only while this is set. */
int nebulaos_fr_mmio_enabled __read_mostly;
EXPORT_SYMBOL(nebulaos_fr_mmio_enabled);
/* Detail level: transfer log and inter-CPU events (nebulaos_flightrec.h). */
int nebulaos_fr_detail __read_mostly;
EXPORT_SYMBOL_GPL(nebulaos_fr_detail);

static void __iomem *nfr_base;
static u32 nfr_ncpu, nfr_nent;
static DEFINE_PER_CPU(u32, nfr_idx);
static DEFINE_PER_CPU(u32, nfr_mmio_idx);
static DEFINE_PER_CPU(u32, nfr_xfer_idx);
static struct timer_list nfr_clk_timer;
static DEFINE_PER_CPU(int, nfr_busy);	/* inside __nebulaos_fr_log: its clock reads are not noted */

static void *nfr_prev;		/* copy of the previous boot's area, or NULL */
static size_t nfr_size;
static bool nfr_param_off;
static bool nfr_detail_param_on;

static int __init nfr_param(char *s)
{
	nfr_param_off = s && (s[0] == '0' || s[0] == 'n' || s[0] == 'N');
	return 1;
}
__setup("nebulaos_flightrec=", nfr_param);

static int __init nfr_detail_param(char *s)
{
	nfr_detail_param_on = s && (s[0] == '1' || s[0] == 'y' || s[0] == 'Y');
	return 1;
}
__setup("nebulaos_flightrec_detail=", nfr_detail_param);

static u32 nfr_banner_crc(void)
{
	return crc32(0, linux_banner, strlen(linux_banner));
}

static inline size_t nfr_xfer_off(u32 ncpu, u32 mmio_n)
{
	return NFR_HDR_SIZE + (size_t)ncpu * mmio_n * NFR_MMIO_ENT;
}

static inline size_t nfr_events_off(u32 ncpu, u32 mmio_n, u32 xfer_n)
{
	return nfr_xfer_off(ncpu, mmio_n) + (size_t)ncpu * xfer_n * sizeof(struct nfr_rec);
}

static inline struct nfr_rec __iomem *nfr_slot(int cpu, u32 idx)
{
	return (struct nfr_rec __iomem *)(nfr_base +
					  nfr_events_off(nfr_ncpu, NFR_MMIO_N, NFR_XFER_N)) +
		(size_t)cpu * nfr_nent + (idx & (nfr_nent - 1));
}

static inline struct nfr_rec __iomem *nfr_xfer_slot(int cpu, u32 idx)
{
	return (struct nfr_rec __iomem *)(nfr_base + nfr_xfer_off(nfr_ncpu, NFR_MMIO_N)) +
		(size_t)cpu * NFR_XFER_N + (idx & (NFR_XFER_N - 1));
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
	__this_cpu_write(nfr_busy, 1);
	__raw_writel((u32)(sched_clock() >> 10), &r->ts);
	__this_cpu_write(nfr_busy, 0);
	idx++;
	__this_cpu_write(nfr_idx, idx);
	__raw_writel(idx, &((struct nfr_hdr __iomem *)nfr_base)->idx[cpu]);
out:
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__nebulaos_fr_log);

static int nfr_state_slot(unsigned int type, unsigned int arg, unsigned long b)
{
	switch (type) {
	case NFR_USB_START:	return NFR_ST_USB_START + ((b >> 24) & 15);
	case NFR_USB_HALT:	return NFR_ST_USB_HALT + (arg & 15);
	case NFR_USB_CHINT:	return NFR_ST_USB_CHINT + (arg & 15);
	case NFR_MMC_CMD:	return NFR_ST_MMC_CMD + ((arg >> 8) & 3);
	case NFR_MMC_IRQ:	return NFR_ST_MMC_IRQ + (arg & 3);
	case NFR_MMC_DONE:	return NFR_ST_MMC_DONE + ((arg >> 8) & 3);
	case NFR_CLK:		return NFR_ST_CLK;
	default:		return -1;
	}
}

/* Same record and write order as __nebulaos_fr_log, into the transfer log,
 * then the same record into the source's state slot. */
void __nebulaos_fr_xfer(unsigned int type, unsigned int arg,
			unsigned long a, unsigned long b)
{
	struct nfr_hdr __iomem *h = nfr_base;
	struct nfr_rec __iomem *r;
	unsigned long flags;
	int cpu, st;
	u32 idx, ts;

	local_irq_save(flags);
	cpu = raw_smp_processor_id();
	if (unlikely(cpu >= nfr_ncpu))
		goto out;
	idx = __this_cpu_read(nfr_xfer_idx);
	r = nfr_xfer_slot(cpu, idx);
	__raw_writel((u32)a, &r->a);
	__raw_writel((u32)b, &r->b);
	__raw_writel((type & 0xffff) | ((arg & 0xffff) << 16), &r->type);
	__this_cpu_write(nfr_busy, 1);
	ts = (u32)(sched_clock() >> 10);
	__this_cpu_write(nfr_busy, 0);
	__raw_writel(ts, &r->ts);
	idx++;
	__this_cpu_write(nfr_xfer_idx, idx);
	__raw_writel(idx, &h->xfer_idx[cpu]);
	st = nfr_state_slot(type, arg, b);
	if (st >= 0) {
		/* ts 0 first: a slot cut short by the freeze reads as unwritten */
		__raw_writel(0, &h->st[st].ts);
		__raw_writel((u32)a, &h->st[st].a);
		__raw_writel((u32)b, &h->st[st].b);
		__raw_writel((type & 0xffff) | ((arg & 0xffff) << 16), &h->st[st].type);
		__raw_writel(cpu, &h->st_cpu[st]);
		__raw_writel(ts ? ts : 1, &h->st[st].ts);
	}
out:
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__nebulaos_fr_xfer);

/* Once a second: pair the recorder clock (sched_clock, the record's ts) with
 * boottime (what /proc/uptime and the heartbeat use) and the wall clock, so a
 * recording can be lined up with the heartbeat and with Klipper's log. */
static void nfr_clk_fn(struct timer_list *t)
{
	/* At every level, not only with detail: lines the recording up. */
	if (READ_ONCE(nebulaos_fr_enabled))
		__nebulaos_fr_xfer(NFR_CLK, 0,
				   (u32)div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC),
				   (u32)ktime_get_real_seconds());
	mod_timer(&nfr_clk_timer, jiffies + HZ);
}

/* Called from readX/writeX BEFORE the access (asm/io.h). The barrier makes
 * the note reach DRAM before the device access is issued, so a CPU stalled
 * on that access has already recorded it. Returns the entry, which asm/io.h
 * marks done (with the value read) after the access, or NULL. */
u32 *__nebulaos_fr_mmio(unsigned long addr, unsigned int write,
			unsigned long val, unsigned long ip)
{
	u32 __iomem *e = NULL;
	unsigned long flags;
	int cpu;
	u32 i;

	/* Reads of the free-running OST counter (sched_clock, clocksource:
	 * 0x120000xx, tens of thousands per second) would flush every device
	 * access out of the ring; they are not noted. */
	if (!write && (addr & 0xffffff00UL) == 0xb2000000UL)
		return NULL;
	local_irq_save(flags);
	cpu = raw_smp_processor_id();
	if (likely(cpu < nfr_ncpu) && !__this_cpu_read(nfr_busy)) {
		i = __this_cpu_read(nfr_mmio_idx);
		e = nfr_base + NFR_HDR_SIZE +
			((size_t)cpu * NFR_MMIO_N + (i & (NFR_MMIO_N - 1))) * NFR_MMIO_ENT;
		/* Not done yet: clear the done mark first. */
		__raw_writel((u32)ip | (write ? 1 : 0), e + 2);
		__raw_writel(__this_cpu_read(nfr_idx), e);
		__raw_writel((u32)addr, e + 1);
		__raw_writel(write ? (u32)val : 0, e + 3);
		__this_cpu_write(nfr_mmio_idx, i + 1);
		__raw_writel(i + 1, &((struct nfr_hdr __iomem *)nfr_base)->mmio_idx[cpu]);
		mb();
	}
	local_irq_restore(flags);
	return (u32 *)e;
}
EXPORT_SYMBOL(__nebulaos_fr_mmio);

/* ---- decoding ----------------------------------------------------------- */

static const char *const nfr_names[NFR_NR_TYPES] = {
	[NFR_NONE] = "?", [NFR_IRQ_IN] = "IRQ>", [NFR_IRQ_OUT] = "IRQ<",
	[NFR_THR_IN] = "THR>", [NFR_THR_OUT] = "THR<",
	[NFR_SIRQ_IN] = "SIRQ>", [NFR_SIRQ_OUT] = "SIRQ<",
	[NFR_TMR_IN] = "TMR>", [NFR_TMR_OUT] = "TMR<",
	[NFR_WQ_IN] = "WQ>", [NFR_WQ_OUT] = "WQ<",
	[NFR_SWITCH] = "SW", [NFR_MARK] = "MARK", [NFR_CTL] = "CTL",
	[NFR_DMAC_IN] = "DMAC>", [NFR_DMAC_OUT] = "DMAC<",
	[NFR_USB_START] = "USB>", [NFR_USB_HALT] = "USBH", [NFR_USB_CHINT] = "USBI",
	[NFR_MMC_CMD] = "MMC>", [NFR_MMC_IRQ] = "MMCI", [NFR_MMC_DONE] = "MMC<",
	[NFR_CLK] = "CLK",
	[NFR_IPI_SEND] = "IPI>", [NFR_CSD_IN] = "CSD>", [NFR_CSD_OUT] = "CSD<",
};

static const char *const nfr_usb_types[4] = { "ctrl", "isoc", "bulk", "intr" };

static const char *nfr_dmac_name(unsigned int op)
{
	return op == NFR_DMAC_WBACK_INV ? "wback_inv" : op == NFR_DMAC_INV ? "inv" : "?";
}

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

static void nfr_show_xfer(struct seq_file *m, int cpu, const struct nfr_rec *r, u32 newest)
{
	unsigned int t = r->type < NFR_NR_TYPES ? r->type : 0;
	s32 dt = (s32)(newest - r->ts);	/* 1.024 us units */
	s64 us = div_s64((s64)dt * 1024, 1000);
	u32 us_abs = (u32)(us < 0 ? -us : us);

	seq_printf(m, "cpu%d X %10u dt=%c%6u.%03u %-5s", cpu, r->ts,
		   us < 0 ? '-' : ' ', us_abs / 1000, us_abs % 1000, nfr_names[t]);
	switch (t) {
	case NFR_USB_START:
		seq_printf(m, " ch=%u dev=%u ep=%u%s %s%s%s len=%u dma=0x%08x",
			   r->b >> 24, r->arg & 0x7f, (r->arg >> 7) & 0xf,
			   (r->arg >> 11) & 1 ? "in" : "out",
			   nfr_usb_types[(r->arg >> 12) & 3],
			   (r->arg >> 14) & 1 ? " split" : "",
			   (r->arg >> 15) & 1 ? " ping" : "",
			   r->b & 0xffffff, r->a);
		break;
	case NFR_USB_HALT:
		seq_printf(m, " ch=%u status=%u", r->arg, r->a);
		break;
	case NFR_USB_CHINT:
		seq_printf(m, " ch=%u hcint=0x%08x mask=0x%08x", r->arg, r->a, r->b);
		break;
	case NFR_MMC_CMD:
		seq_printf(m, " mmc%u CMD%u arg=0x%08x", r->arg >> 8, r->arg & 0xff, r->a);
		if (r->b)
			seq_printf(m, " blocks=%u blksz=%u", r->b >> 16, r->b & 0xffff);
		break;
	case NFR_MMC_IRQ:
		seq_printf(m, " mmc%u status=0x%08x", r->arg, r->a);
		break;
	case NFR_MMC_DONE:
		seq_printf(m, " mmc%u CMD%u err=%d data_err=%d", r->arg >> 8,
			   r->arg & 0xff, (s32)r->a, (s32)r->b);
		break;
	case NFR_CLK:
		seq_printf(m, " boottime=%u.%03u s wall=%u", r->a / 1000, r->a % 1000, r->b);
		break;
	default:
		seq_printf(m, " %u 0x%08x 0x%08x", r->arg, r->a, r->b);
		break;
	}
	seq_putc(m, '\n');
}

static void nfr_show_area(struct seq_file *m, const void *area, const char *what)
{
	const struct nfr_hdr *h = area;
	bool sym;
	u32 newest = 0;
	int cpu, st;
	const void *ev;

	if (h->magic != NFR_MAGIC || h->version != NFR_VERSION ||
	    !h->ncpu || h->ncpu > NFR_MAX_CPUS || !h->nent ||
	    (h->nent & (h->nent - 1)) || !h->mmio_n || (h->mmio_n & (h->mmio_n - 1)) ||
	    !h->xfer_n || (h->xfer_n & (h->xfer_n - 1)) ||
	    nfr_events_off(h->ncpu, h->mmio_n, h->xfer_n) +
	    (size_t)h->ncpu * h->nent * sizeof(struct nfr_rec) > nfr_size) {
		seq_printf(m, "%s: no valid recording%s\n", what,
			   h->magic == NFR_MAGIC && h->version != NFR_VERSION ?
			   " (written by another recorder version)" : "");
		return;
	}
	ev = area + nfr_events_off(h->ncpu, h->mmio_n, h->xfer_n);
	sym = h->banner_crc == nfr_banner_crc();
	seq_printf(m, "%s: ncpu=%u entries/cpu=%u symbols=%s register-log=%s detail=%s\n", what,
		   h->ncpu, h->nent, sym ? "this kernel" : "OTHER kernel (raw addresses)",
		   (h->mmio_on & 1) ? "on" : "off", (h->mmio_on & 2) ? "on" : "off");
	seq_puts(m, "ts in units of 1.024 us; dt = time before the newest record (ms)\n");
	seq_puts(m, "not recorded: ordinary memory loads/stores, cache operations other than DMA\n"
		    "  maintenance, __raw_ register accesses, DMA done by devices themselves;\n"
		    "  the last few stores before a memory-bus hang may never reach RAM\n");

	for (cpu = 0; cpu < h->ncpu; cpu++) {
		u32 idx = h->idx[cpu];
		if (idx) {
			const struct nfr_rec *r = (const struct nfr_rec *)ev +
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
			const struct nfr_rec *r = (const struct nfr_rec *)ev +
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
			case NFR_TMR_OUT: case NFR_WQ_OUT: case NFR_CSD_OUT:
				nfr_addr(m, r->a, sym);
				break;
			case NFR_CSD_IN:
				nfr_addr(m, r->a, sym);
				seq_printf(m, " info=0x%08x", r->b);
				break;
			case NFR_IPI_SEND:
				seq_printf(m, " to cpu%u %s%s from", r->arg & 0xff,
					   (r->arg >> 8) & 1 ? "resched " : "",
					   (r->arg >> 8) & 2 ? "call-func" : "");
				nfr_addr(m, r->a, sym);
				break;
			case NFR_WQ_IN:
				nfr_addr(m, r->a, sym);
				seq_printf(m, " work=0x%08x", r->b);
				break;
			case NFR_CTL:
				seq_printf(m, " register-log=%s detail=%s",
					   (r->arg & 1) ? "on" : "off", (r->arg & 2) ? "on" : "off");
				break;
			case NFR_DMAC_IN:
				seq_printf(m, " %s addr=0x%08x size=%u", nfr_dmac_name(r->arg),
					   r->a, r->b);
				break;
			case NFR_DMAC_OUT:
				seq_printf(m, " %s", nfr_dmac_name(r->arg));
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
		u32 end = h->mmio_idx[cpu], n = min(end, h->mmio_n), i;
		u32 ev_end = h->idx[cpu];

		seq_printf(m, "== cpu%d: last %u of %u device register accesses (oldest first)\n"
			   "   ev = this CPU's event count at the access (ev N comes after event N-1 above);\n"
			   "   NOT DONE = the access never completed\n", cpu, n, end);
		for (i = end - n; i != end; i++) {
			const u32 *e = area + NFR_HDR_SIZE +
				((size_t)cpu * h->mmio_n + (i & (h->mmio_n - 1))) * NFR_MMIO_ENT;
			u32 evn = e[0], a = e[1], ipw = e[2], val = e[3];
			bool wr = ipw & 1, done = ipw & 2;

			seq_printf(m, "cpu%d MMIO ev=%u %c 0x%08x", cpu, evn, wr ? 'W' : 'R', a);
			if (a >= 0xa0000000u && a < 0xc0000000u)
				seq_printf(m, " phys=0x%08x", a & 0x1fffffffu);
			if (wr || done)
				seq_printf(m, " val=0x%08x", val);
			if (!done)
				seq_puts(m, " NOT DONE");
			if (evn && evn <= ev_end && ev_end - evn < h->nent) {
				const struct nfr_rec *r = (const struct nfr_rec *)ev +
					(size_t)cpu * h->nent + ((evn - 1) & (h->nent - 1));
				seq_printf(m, " after-ts=%u", r->ts);
			}
			nfr_addr(m, ipw & ~3u, sym);
			seq_putc(m, '\n');
		}
	}

	for (cpu = 0; cpu < h->ncpu; cpu++) {
		const struct nfr_rec *xr = area + nfr_xfer_off(h->ncpu, h->mmio_n);
		u32 end = h->xfer_idx[cpu], n = min(end, h->xfer_n), i;

		seq_printf(m, "== cpu%d: last %u of %u transfer-log records (oldest first)\n"
			   "   dt as above (negative = after the newest event)\n", cpu, n, end);
		for (i = end - n; i != end; i++)
			nfr_show_xfer(m, cpu, xr + (size_t)cpu * h->xfer_n + (i & (h->xfer_n - 1)),
				      newest);
	}

	seq_puts(m, "== transfer state: the newest record per USB channel / MMC host / clock\n"
		    "   (kept however busy the other sources were; cpu = the CPU that wrote it)\n");
	for (st = 0; st < NFR_ST_N; st++)
		if (h->st[st].ts && h->st_cpu[st] < h->ncpu)
			nfr_show_xfer(m, h->st_cpu[st], &h->st[st], newest);
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
	return single_open_size(file, nfr_live_show, NULL, 6 << 20);
}

static DEFINE_MUTEX(nfr_ctl_lock);

/* Current state word: bit 0 register log, bit 1 detail (header, CTL). */
static unsigned int nfr_state_bits(void)
{
	return (READ_ONCE(nebulaos_fr_mmio_enabled) ? 1 : 0) |
	       (READ_ONCE(nebulaos_fr_detail) ? 2 : 0);
}

static void nfr_publish_state(void)
{
	unsigned int st = nfr_state_bits();

	__raw_writel(st, &((struct nfr_hdr __iomem *)nfr_base)->mmio_on);
	nebulaos_fr_log(NFR_CTL, st, 0, 0);
	pr_info("nebulaos-flightrec: detail %s, register log %s\n",
		(st & 2) ? "on" : "off", (st & 1) ? "on" : "off");
}

static void nfr_set_mmio(unsigned int on)
{
	mutex_lock(&nfr_ctl_lock);
	WRITE_ONCE(nebulaos_fr_mmio_enabled, on);
	nfr_publish_state();
	mutex_unlock(&nfr_ctl_lock);
}

/* Detail on/off: the register log follows it. */
static void nfr_set_detail(unsigned int on)
{
	mutex_lock(&nfr_ctl_lock);
	WRITE_ONCE(nebulaos_fr_detail, on);
	WRITE_ONCE(nebulaos_fr_mmio_enabled, on);
	nfr_publish_state();
	mutex_unlock(&nfr_ctl_lock);
}

static ssize_t nfr_live_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	char k[16];
	size_t n = min(count, sizeof(k) - 1);
	unsigned int v;
	int ret;

	if (copy_from_user(k, buf, n))
		return -EFAULT;
	k[n] = 0;
	if (!strncmp(k, "mmio=", 5)) {
		ret = kstrtouint(strim(k + 5), 0, &v);
		if (ret || v > 1)
			return -EINVAL;
		nfr_set_mmio(v);
		return count;
	}
	if (!strncmp(k, "detail=", 7)) {
		ret = kstrtouint(strim(k + 7), 0, &v);
		if (ret || v > 1)
			return -EINVAL;
		nfr_set_detail(v);
		return count;
	}
	ret = kstrtouint_from_user(buf, count, 0, &v);
	if (ret)
		return ret;
	nebulaos_fr_log(NFR_MARK, v, 0, 0);
	return count;
}

static int nfr_prev_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, nfr_prev_show, NULL, 6 << 20);
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
	if (nfr_size < nfr_events_off(ncpu, NFR_MMIO_N, NFR_XFER_N) + 4096) {
		pr_err("nebulaos-flightrec: area too small, off\n");
		return 0;
	}
	nent = rounddown_pow_of_two((nfr_size - nfr_events_off(ncpu, NFR_MMIO_N, NFR_XFER_N)) /
				    ncpu / sizeof(struct nfr_rec));

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
	__raw_writel(NFR_MMIO_N, &h->mmio_n);
	__raw_writel(NFR_XFER_N, &h->xfer_n);
	__raw_writel(nfr_banner_crc(), &h->banner_crc);
	__raw_writel((u32)(sched_clock() >> 10), &h->boot_ts);
	for_each_possible_cpu(cpu) {
		per_cpu(nfr_idx, cpu) = 0;
		per_cpu(nfr_xfer_idx, cpu) = 0;
	}
	nfr_ncpu = ncpu;
	nfr_nent = nent;
	wmb();
	__raw_writel(NFR_MAGIC, &h->magic);
	proc_create("nebulaos_flightrec", 0600, NULL, &nfr_live_ops);
	smp_wmb();
	WRITE_ONCE(nebulaos_fr_enabled, 1);
	__raw_writel(nfr_detail_param_on ? 3 : 0, &h->mmio_on);
	WRITE_ONCE(nebulaos_fr_detail, nfr_detail_param_on);
	WRITE_ONCE(nebulaos_fr_mmio_enabled, nfr_detail_param_on);
	BUILD_BUG_ON(sizeof(struct nfr_hdr) > NFR_HDR_SIZE);
	timer_setup(&nfr_clk_timer, nfr_clk_fn, 0);
	mod_timer(&nfr_clk_timer, jiffies + HZ);
	pr_info("nebulaos-flightrec: v%u recording at %pa, %u cpus x %u events + %u transfer records, detail %s; previous boot %s\n",
		NFR_VERSION, &res.start, ncpu, nent, NFR_XFER_N, nfr_detail_param_on ? "on" : "off",
		nfr_prev ? "saved" : "had no recording");
	return 0;
}
early_initcall(nfr_init);
