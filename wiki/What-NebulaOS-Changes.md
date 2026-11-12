# What NebulaOS changes in this kernel

This repository is mostly a vendor SDK. This page answers the question a developer actually
arrives with: **which parts are NebulaOS, and which are Ingenic's or OpenKE's?**

Written against the `openke` branch at `95d0321e`, which is also what ships: the firmware
manifest at tag `v1.0.2` pins `KERNEL_PIN=95d0321ef4270adb3e080fe2125ab81fcb232b91` with
`KERNEL_BRANCH=openke`, and that is the branch tip. The branch layout and the name are on
[Branch model](Branch-Model).

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


The fourth layer - freeze diagnosis and the black box, eleven commits - is large enough to have
its own page: [Freeze diagnosis](Freeze-Diagnosis).

## Not covered here

- **The vendor SDK itself.** Most of this repository is Ingenic's, including `frameworks/`,
  `development/` and `external/`. Its own documentation is under `docs/doc/`, partly in Chinese.
- **Hardware behaviour.** Nothing on this page was observed on a printer by its author.
- **U-Boot**, which lives in `u-boot/` and has its own vendor history.
- **Real-time latency figures.** The [scheduling model](Scheduling-Model) is stated; no latency
  was measured here.
