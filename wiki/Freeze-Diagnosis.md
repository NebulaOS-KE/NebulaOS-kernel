# Freeze diagnosis and the black box

Eleven commits on `openke`, written after two real failures on the development printer. They do
not change how the printer prints; they change what is left behind when it stops. Written
against `95d0321e`, the 1.0.2 pin.

The userspace half - `nebulaos-blackbox`, the reset verdict and the two overlay files that go
with it - is in the firmware wiki:
[The black box](https://github.com/NebulaOS-KE/NebulaOS-firmware/wiki/Black-Box).

The two failures: a partial hang on 2026-11-06, where SSH and shell commands blocked for
minutes while the screen and Klipper kept running, and a mid-print freeze on 2026-11-08, where
both CPUs stopped and the hardware watchdog reset the board 60 s later **with nothing in the
kernel log**.

| Commit | Change | Where |
|---|---|---|
| `ef2a2c264` | pstore/ramoops in 1 MB of RAM, reset-status readback, soft-lockup panic; quiet EEPROM-busy I2C aborts | `dts/x2000/halley5_v30.dts`, defconfig, `watchdog/ingenic_wdt.c`, `i2c/busses/i2c-ingenic.c` |
| `842860ab2` | Clear the CPM reboot signature at probe; unbuffered ramoops (`mem-type = <1>`) | `ingenic_wdt.c`, `halley5_v30.dts` |
| `785b600f7` | Keep the unhandled WDT interrupt masked - it was holding the shared TCU line asserted | `ingenic_wdt.c` |
| `b9245f14e` | Hung-task detector, report only (120 s, never panics) | defconfig |
| `e5f4d8fe7` | RSR bit 0 named power-on; ramoops' empty-zone header message to debug | `ingenic_wdt.c`, `fs/pstore/ram_core.c` |
| `11f0f0c87` | `CONFIG_PREEMPT` instead of `CONFIG_PREEMPT_RT`; `localversion-rt` removed; "restart" vs "watchdog" wording; the wireless-extensions warning to `pr_debug_once` | defconfig, `localversion-rt`, `ingenic_wdt.c`, `net/wireless/wext-core.c` |
| `ece4732b0` | **Watchdog pre-timeout dump**: an interrupt 40 s without a feed prints the clockevent state, blocked tasks and every CPU backtrace, then panics; `nebulaos_core_ost_dump()`; hard-lockup (buddy) detector with panic | `ingenic_wdt.c`, `clocksource/ingenic_core_ost.c`, defconfig |
| `3b6086868` | `sched_clock` takes `clocksource_lock` with trylock, so a freeze dump cannot hang on its own first timestamp | `ingenic_core_ost.c` |
| `e8596387b` | `nebulaos_sdio_idle` module parameter for the Wi-Fi SDIO bus; a boot line saying whether KSO save/restore is in use | `brcm80211/brcmfmac/sdio.c` |
| `95d0321ef` | I2C: `INTM = 0` before a controller reset and before the clock is gated | `i2c-ingenic.c` |

Three of these are worth understanding in detail, because a developer will meet them in a log.

**The black box is pstore/ramoops.** `halley5_v30.dts` reserves 1 MB at `0x0e000000`:
512 KB console (the kernel log), 64 KB per oops/panic record, 256 KB `pmsg` for the userspace
heartbeat, with ECC and `mem-type = <1>` so the last writes before a hard reset are not sitting
in a write buffer. The address lies inside the vendor's own 256M ISP pool, which is `disabled`
in the DTS and which the bootloader leaves alone - re-enabling that pool on a 256 MB board would
overlap ramoops, and the DTS comment says so. The area survives a watchdog or panic reset and is
read back from `/sys/fs/pstore` on the next boot; a real power cut loses it, as RAM does.
The defconfig side is `CONFIG_PSTORE`, `PSTORE_RAM`, `PSTORE_CONSOLE`, `PSTORE_PMSG`, with
`PSTORE_COMPRESS` off so the result is readable without tooling.

**Three detectors, deliberately different.** `CONFIG_SOFTLOCKUP_DETECTOR` with
`BOOTPARAM_SOFTLOCKUP_PANIC` panics a CPU stuck in the kernel, so the dump is kept and
`PANIC_TIMEOUT=10` restarts the board instead of waiting out the 60 s hardware watchdog with
nothing recorded. `HARDLOCKUP_DETECTOR` (the buddy detector, since this SoC has no NMI
perf counter) with `BOOTPARAM_HARDLOCKUP_PANIC` covers a CPU that has stopped taking
interrupts. `DETECT_HUNG_TASK` with `DEFAULT_HUNG_TASK_TIMEOUT=120` only *reports* -
`BOOTPARAM_HUNG_TASK_PANIC` is deliberately off, because a slow eMMC must not restart a printer
that is printing. That asymmetry is a decision, not an oversight.

**The watchdog pre-timeout dump is the last resort.** `nebulaos_wdt_pretimeout()` in
`ingenic_wdt.c` is hooked to the watchdog's `H` compare, which the hardware reaches at two
thirds of the armed timeout - 40 s of the 60 s the userspace feeder arms. It is a device
interrupt on the RTC-clocked TCU line, so it still arrives when every kernel timer is dead, as
long as the CPU it is routed to still takes interrupts. It masks itself (one shot, no storm),
restarts the hardware count so the dump has a full timeout to finish, prints
`nebulaos_core_ost_dump()`, `show_state_filter(TASK_UNINTERRUPTIBLE)` and
`trigger_all_cpu_backtrace()`, and then panics on purpose, because the panic path is what writes
the whole kernel log into pstore past a console lock a frozen CPU may be holding. A software
restart is not a freeze and is skipped (`system_state != SYSTEM_RUNNING`). **If this never fires
and the board resets anyway, that is itself a result:** the CPU owning the line had interrupts
disabled for the whole 40 s.

**The Wi-Fi idle parameter is an experiment, not a fix.** `brcmfmac`'s SDIO bus sleeps after
`idletime` watchdog ticks (10 ms each) without traffic; on the CYW43430 with save/restore that
is a KSO sleep/wake of the chip thousands of times an hour on a quiet link.
`/sys/module/brcmfmac/parameters/nebulaos_sdio_idle` is writable at runtime: `-1` (the default)
keeps the driver's own `idletime`, `0` never sleeps the bus, `N` waits N ticks. **The default
changes nothing**, which is what makes it an A/B test rather than a behaviour change.


## Not a fix

Nothing on this page claims the freeze is fixed. These commits are instrumentation and two
mitigations, written from the evidence of two failures; none of them was verified on a printer
by this page's author. If a board resets and the pre-timeout dump never fired, that is itself a
result - see the paragraph on it above.
