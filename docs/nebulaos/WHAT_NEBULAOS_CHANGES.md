# What NebulaOS changes in this kernel

This repository is mostly a vendor SDK. This page answers the question a developer actually
arrives with: **which parts are NebulaOS, and which are Ingenic's or OpenKE's?**

Written against the `openke` branch at `d86790d`.

## Branch layout

| Branch | What it is |
|---|---|
| `main` | `a98c2e1f2`, "initial release" - the **pristine vendor baseline**, forked from `Llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221` |
| `openke` | The branch NebulaOS builds from: 122 commits on top of that baseline |

`openke` is the default branch, and the one to read. `main` exists so the vendor baseline stays
available for comparison - `git diff origin/main origin/openke` is the complete set of changes.

**A caution about that diff:** it reports around 13,000 files changed, which is not a measure of
NebulaOS's footprint. Most of it is the vendor tree reorganisation that came with the OpenKE
import. The NebulaOS-specific surface is small and listed below.

## The three layers on `openke`

Reading the history from the baseline forward, the commits fall into three groups.

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
(17 changes across the history), since every capability added has to be enabled somewhere.

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
- **PREEMPT_RT and real-time behaviour.** Referenced in firmware analysis documents; not
  established here.
- **Hardware behaviour.** Nothing on this page was observed on a printer by its author.
- **U-Boot**, which lives in `u-boot/` and has its own vendor history.
