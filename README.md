# NebulaOS Kernel

The Linux 6.6 kernel SDK [NebulaOS](https://github.com/NebulaOS-KE/NebulaOS) runs on the Creality
Ender-3 V3 KE (Ingenic X2000).

Yes, the main branch here is called `openke`. That's an old branch name we kept for history — this
repo is a NebulaOS component and isn't part of the separate
[OpenKE](https://github.com/coreflake1/guppyscreen) project. It just predates this repo's rename
and never got renamed for branding reasons.

| Branch | What's there |
|---|---|
| `openke` | The real NebulaOS lineage. Every kernel change NebulaOS made — NS2009 touch, the display panel driver, the BT H5 vendor extension, a watchdog fix, DTS wiring, some Kconfig compression selects — lives here as an actual commit, not a patch file applied at build time. |
| `main` | The unmodified upstream SDK ([`Llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221`](https://github.com/Llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221)) — this fork's starting point, untouched by any NebulaOS work. |

## You probably don't want to build this repo directly

If you want the complete NebulaOS image, start at
[`NebulaOS-firmware`](https://github.com/NebulaOS-KE/NebulaOS-firmware) instead. It pins an exact
commit of this repo's `openke` branch, fetches just the kernel source it needs (a sparse checkout —
the full SDK here is ~684MB), and applies 8 build-time kernel variants on top before compiling.

## The 8 build-time variants

These live in `NebulaOS-firmware`, not here — this repo only holds the base kernel source they
patch at build time. Applied in order by `scripts/build/apply-qualified-baseline.sh` (though the
order doesn't actually matter — each one touches its own files):

1. PREEMPT_RT
2. WiFi SDIO IRQ priority
3. VSYNC-gated display panning
4. A pinctrl ownership fix
5. The final backlight controller
6. PWM state readback
7. The final touch driver
8. Disabling WiFi roaming

## If you're setting up a device

The install/update/recovery docs all live in `NebulaOS-firmware`, not here — this repo is just
kernel source:

- [`NebulaOS-firmware` wiki](https://github.com/NebulaOS-KE/NebulaOS-firmware/wiki)
- [Build From Source](https://github.com/NebulaOS-KE/NebulaOS-firmware/blob/main/docs/BUILD_FROM_SOURCE.md) — how this repo's `openke` branch actually gets pulled in and patched
- [A/B Slot Model](https://github.com/NebulaOS-KE/NebulaOS-firmware/blob/main/docs/A_B_SLOT_MODEL.md)

## License

This is a whole SDK — kernel, Buildroot, u-boot, and a handful of vendored tools — each under its
own upstream license, so there's no single root `LICENSE` file that covers everything. The kernel
itself is at `kernel/kernel-6.6/COPYING` (GPL-2.0) and `kernel/kernel-6.6/LICENSES/`; other
top-level directories have their own license files. GitHub's license detector only looks at the
repo root, so it'll correctly show "no license detected" here — check the specific subdirectory
you're actually using.
