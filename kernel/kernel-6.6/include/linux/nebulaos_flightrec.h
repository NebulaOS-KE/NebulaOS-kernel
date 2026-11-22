/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NebulaOS flight recorder (2026-11-14): a per-CPU ring of the last few
 * thousand kernel events (hard IRQ, threaded IRQ, softirq, timer callback,
 * workqueue item, context switch) written UNCACHED into reserved RAM, so it
 * survives a watchdog reset. The previous boot's ring is readable at
 * /proc/nebulaos_flightrec_prev. Purpose: when the SoC hard-freezes (no
 * interrupt is taken any more, so nothing else can log), the last entries
 * show what each CPU was doing.
 */
#ifndef _LINUX_NEBULAOS_FLIGHTREC_H
#define _LINUX_NEBULAOS_FLIGHTREC_H

#include <linux/compiler.h>
#include <linux/types.h>

enum nfr_type {
	NFR_NONE = 0,
	NFR_IRQ_IN,	/* arg = irq, a = interrupted epc, b = interrupted ra */
	NFR_IRQ_OUT,	/* arg = irq */
	NFR_THR_IN,	/* arg = irq, a = thread_fn */
	NFR_THR_OUT,	/* arg = irq */
	NFR_SIRQ_IN,	/* arg = softirq nr, a = action */
	NFR_SIRQ_OUT,	/* arg = softirq nr */
	NFR_TMR_IN,	/* a = timer fn */
	NFR_TMR_OUT,	/* a = timer fn */
	NFR_WQ_IN,	/* a = work fn, b = work item */
	NFR_WQ_OUT,	/* a = work fn */
	NFR_SWITCH,	/* arg = prev pid (low 16), a = next pid, b = next comm[0..3] */
	NFR_MARK,	/* arg = user value; written by /proc/nebulaos_flightrec */
	NFR_CTL,	/* arg = register log now on (1) / off (0) */
	NFR_DMAC_IN,	/* arg = op (enum nfr_dmac_op), a = address, b = size */
	NFR_DMAC_OUT,	/* arg = op */
	/* v4 (2026-11-22, #27): transfer log, a separate per-CPU ring so the
	 * 8 kHz USB SOF interrupts in the event ring do not push it out. */
	NFR_USB_START,	/* arg = dev | ep<<7 | in<<11 | type<<12 | split<<14 | ping<<15,
			 * a = DMA address, b = length | channel<<24 */
	NFR_USB_HALT,	/* arg = channel, a = halt status (halt requested) */
	NFR_USB_CHINT,	/* arg = channel, a = HCINT (raw), b = HCINTMSK */
	NFR_MMC_CMD,	/* arg = opcode | host<<8, a = cmd arg, b = blocks<<16 | blksz */
	NFR_MMC_IRQ,	/* arg = host, a = SDHCI interrupt status */
	NFR_MMC_DONE,	/* arg = opcode | host<<8, a = cmd error, b = data error */
	NFR_CLK,	/* once a second: a = boottime in ms, b = wall-clock seconds */
	/* test3 (2026-11-22, #27): what each inter-CPU interrupt asked for.
	 * 4 of 11 freeze recordings end right after an IPI. */
	NFR_IPI_SEND,	/* arg = target cpu | action<<8, a = caller */
	NFR_CSD_IN,	/* IPI callback starts: a = function, b = info */
	NFR_CSD_OUT,	/* IPI callback returned: a = function */
	NFR_NR_TYPES
};

/* DMA cache maintenance ops (arch/mips/mm/c-r4k.c), logged around the op. */
enum nfr_dmac_op {
	NFR_DMAC_WBACK_INV = 1,
	NFR_DMAC_INV = 2,
};

#ifdef CONFIG_NEBULAOS_FLIGHTREC
extern int nebulaos_fr_enabled;
void __nebulaos_fr_log(unsigned int type, unsigned int arg,
		       unsigned long a, unsigned long b);

static __always_inline void nebulaos_fr_log(unsigned int type, unsigned int arg,
					    unsigned long a, unsigned long b)
{
	if (READ_ONCE(nebulaos_fr_enabled))
		__nebulaos_fr_log(type, arg, a, b);
}

void __nebulaos_fr_xfer(unsigned int type, unsigned int arg,
			unsigned long a, unsigned long b);

/* Transfer log (USB channels, SD/MMC commands, clock pairs). */
static __always_inline void nebulaos_fr_xfer(unsigned int type, unsigned int arg,
					     unsigned long a, unsigned long b)
{
	if (READ_ONCE(nebulaos_fr_enabled))
		__nebulaos_fr_xfer(type, arg, a, b);
}
#else
static inline void nebulaos_fr_log(unsigned int type, unsigned int arg,
				   unsigned long a, unsigned long b) { }
static inline void nebulaos_fr_xfer(unsigned int type, unsigned int arg,
				    unsigned long a, unsigned long b) { }
#endif

#endif /* _LINUX_NEBULAOS_FLIGHTREC_H */
