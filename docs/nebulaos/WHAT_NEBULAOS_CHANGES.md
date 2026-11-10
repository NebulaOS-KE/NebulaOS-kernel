# What NebulaOS changes in this kernel

This repository is mostly a vendor SDK. This page answers the question a developer actually
arrives with: **which parts are NebulaOS, and which are Ingenic's or OpenKE's?**

Written against the `openke` branch at `95d0321e`, which is also what ships: the firmware
manifest at tag `v1.0.2` pins `KERNEL_PIN=95d0321ef4270adb3e080fe2125ab81fcb232b91` with
`KERNEL_BRANCH=openke`, and that is the branch tip.

## Branch layout

| Branch | What it is |
|---|---|
| `main` | `a98c2e1f2`, "initial release" - the **pristine vendor baseline**, forked from `Llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221` |
| `openke` | The branch NebulaOS builds from: 133 commits on top of that baseline |

`openke` is the default branch, and the one to read. `main` exists so the vendor baseline stays
available for comparison - `git diff origin/main origin/openke` is the complete set of changes.

**A caution about that diff:** it reports around 13,000 files changed, which is not a measure of
NebulaOS's footprint. Most of it is the vendor tree reorganisation that came with the OpenKE
import. The NebulaOS-specific surface is small and listed below.

## The four layers on `openke`

Reading the history from the baseline forward, the commits fall into four groups.

### 1. The OpenKE layer

`8e97319a1` - *"openke: NS2009 touch, OpenKE display panel, BT H5 vendor ext, watchdog fix, DTS
wiring"*. The work that made this SoC usable on this printer at all, inherited rather than
written here. Attribute it to OpenKE.

### 2. Real hardware bugs, found and fixed

Five commits, each fixing something genuinely wrong in the vendor board description rather than
adapting it to taste:

| Commit | What was wrong |
|---|---|
| `e67eefc40` | `wlan_pwrseq` `reset-gpios` missing its third cell - a real Wi-Fi SDIO init bug |
| `5dca5971f` | `wlan_pwrseq` missing `post-power-on-delay-ms` - a real 0-delay SDIO probe race |
| `c902097d1` | `drvvbus-gpio` wired to the wrong pin, confirmed against stock's own DTB |
| `4905cb23e` | `uart1` - the real mainboard MCU link - never wired up in the board file |
| `c60cf4d67` | `as_dmic` left enabled, a real pin conflict blocking that same `uart1` |

Worth reading as a group. The last two are the same story: the link to the mainboard MCU did not
exist in the vendor board file, and a second device was holding the pins it needed. A vendor SDK
being wrong about its own hardware is normal; this is the list of places it was.

### 3. NebulaOS's own changes

Eight commits, each with a `G`-number tying it to a NebulaOS workstream:

| Commit | Change | Subsystem |
|---|---|---|
| `862cc4c24` | NebulaOS boot card as the kernel's display-init logo | `video/logo-ingenic` |
| `27fe61c54` | Keep the power-on picture through the display takeover | `video/fbdev/ingenic` |
| `62d724789` | Boot picture diagnostics always in the boot log | `video/fbdev/ingenic/fb_stage` |
| `65f93367c` | NS2009 touch mode and `touch_down_count` exposed in sysfs | `drivers/misc` |
| `5ba3d3afc` | KE buzzer (GPC3) as a standard kernel PWM channel | `drivers/pwm` |
| `f28a670d5` | `ingenic_wdt` arms with a programmed timeout | `drivers/watchdog` |
| `38c467bf5` | `ingenic_wdt` read-only marker `nebulaos_arm_fix` | `drivers/watchdog` |
| `cf4dd8126` | Vendor boot messages at info level rather than warning/error | various |

Plus the configs - `nebulaos_halley5_v30_defconfig` and
`nebulaos_halley5_v30_diagnostic.config` - which are the most-touched files in the repository
(25 changes across the history), since every capability added has to be enabled somewhere.

### How those changes connect outward

Three of them exist because something above the kernel needs them, which is easy to miss when
reading the kernel alone:

- **The buzzer as a PWM channel** (`5ba3d3afc`) is what lets Klipper's `nebulaos_beeper` extra
  drive it from userspace through sysfs. The kernel does not play sounds; it exposes a channel.
- **The watchdog arming with a programmed timeout** (`f28a670d5`) is what
  `S02nebulaos-watchdog` feeds, so a frozen printer resets itself instead of hanging until
  someone cuts the power.
- **Keeping the power-on picture through the display takeover** (`27fe61c54`) is why the boot
  logo does not flash to black before NebulaScreen appears.

A fourth connection came with the diagnosis layer below: the reserved ramoops area and the
reset-status registers are what the firmware's `nebulaos-blackbox` reads after a reboot, so
"why did the printer restart?" has an answer on the printer itself.

### 4. Freeze diagnosis and the black box

Eleven commits, written after two real failures on the development printer: a partial hang on
2026-11-06 (SSH and shell commands blocked for minutes while the screen and Klipper kept
running) and a mid-print freeze on 2026-11-08 where both CPUs stopped and the hardware watchdog
reset the board 60 s later **with nothing in the kernel log**. They are a diagnosis layer: they
do not change how the printer prints, they change what is left behind when it stops.

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

## What the firmware build does *not* do any more

This matters for anyone comparing documents, because it changed.

The kernel changes used to be applied at build time from
`patches/x2000_kernel_6.6-openke.patch`. They are now **real commits on `openke`**, and
`00-fetch-vendor-sources.sh` checks that branch out directly.

So the firmware's `scripts/build/01-apply-kernel-patches.sh` **applies nothing**. It is a
verification step: it checks that the expected changes are present in the checked-out tree. The
script keeps its `01-` number on purpose so existing documentation and habits still work.

The answer to *which changes are maintained here versus applied by the build* is therefore:
**all of them are maintained here, and none are applied by the build.**

## Building

The firmware build owns this. `03-build-kernel-and-rootfs.sh` builds the kernel inside the
pinned build container with `nebulaos_halley5_v30_defconfig`, producing the `xImage` that goes
into a system image.

The pinned commit is in `NebulaOS-firmware/manifests/dependencies.conf` as `KERNEL_PIN`, with
`KERNEL_BRANCH=openke` and `KERNEL_CONFIG_PROFILE=production`. **That pin is what ships** - this
branch's tip is not, when the two differ.

`.github/workflows/build.yml` compile-checks the kernel (`xImage`,
`nebulaos_halley5_v30`) on push.

## The scheduling model

The kernel is built with **`CONFIG_PREEMPT`** (`nebulaos_halley5_v30_defconfig` line 7), not
`CONFIG_PREEMPT_RT`. The tree is still based on `linux-stable-rt`, so the RT patch set is
present in the source and the model is one defconfig line to switch; `localversion-rt` was
removed in `11f0f0c87` so a kernel that is not RT does not report `-rt79` in `uname`.

Two consequences a developer should know:

- `NEBULAOS_KTIMERS_PRIO` is gone with it - `ktimers/N` threads only exist on RT.
- The IRQ thread priority table stays and still applies: MIPS selects `IRQ_FORCED_THREADING`,
  and the explicitly threaded handlers (`uart1`, DWC2, mmc) are threaded on `PREEMPT` too.

## The hardware

| | |
|---|---|
| SoC | Ingenic X2000 (`xburst2`, `soc-x2000`) |
| Architecture | MIPS (`mipsel`) |
| Kernel | 6.6 (`kernel/kernel-6.6/`) |
| Display | 480x272 panel, framebuffer - no X11 or Wayland |
| Touch | NS2009 resistive controller |

The 6.6 kernel version is also how NebulaOS tells its own slot apart from Creality's at boot:
the firmware's `ota_slot_holds_nebulaos()` reads the slot's kernel uImage name, and a 6.x kernel
means NebulaOS while the other system's is 4.4.

## Not covered here

- **The vendor SDK itself.** Most of this repository is Ingenic's, including
  `frameworks/`, `development/` and `external/`. Its own documentation is under `docs/doc/`,
  partly in Chinese.
- **Real-time latency figures.** The scheduling model is stated below; no latency was measured
  here.
- **Hardware behaviour.** Nothing on this page was observed on a printer by its author.
- **U-Boot**, which lives in `u-boot/` and has its own vendor history.
