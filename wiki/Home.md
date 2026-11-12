# NebulaOS-kernel

The Linux 6.6 kernel NebulaOS runs on the Creality Ender-3 V3 KE (Ingenic X2000). Development
happens on **`openke`**; complete, flashable images are built by
[NebulaOS-firmware](https://github.com/NebulaOS-KE/NebulaOS-firmware), not here. Building this
repository on its own does not produce a working printer image.

Most of this repository is a vendor SDK. The page that answers the question developers arrive
with - which parts are NebulaOS? - is
[What NebulaOS changes](What-NebulaOS-Changes).

This wiki is the developer documentation for the kernel. The end-user manual is the
[NebulaOS wiki](https://github.com/NebulaOS-KE/NebulaOS/wiki).

## Which code this describes

| | |
|---|---|
| Branch | `openke` (the default branch) |
| Commit | **`95d0321e`**, the branch tip |
| Ships as | NebulaOS **1.0.2**: `KERNEL_PIN=95d0321ef4270adb3e080fe2125ab81fcb232b91` in firmware `3bdf3ff` (tag `v1.0.2`), `KERNEL_BRANCH=openke`, `KERNEL_CONFIG_PROFILE=production` |

At 1.0.2 the pin and the branch tip are the same commit, so for once the documented code is
exactly the shipped code. That is not guaranteed to stay true: the firmware manifest is the
shipping authority, and `REPOSITORY_HEAD` is not `SHIPPING_PIN`.

## The pages

- **[What NebulaOS changes](What-NebulaOS-Changes)** - the four layers on `openke`: the
  inherited OpenKE work, five real hardware bugs in the vendor board description, NebulaOS's own
  eight changes, and how they connect to the software above
- **[Freeze diagnosis](Freeze-Diagnosis)** - the eleven-commit black box: pstore/ramoops, three
  deliberately different lockup detectors, the watchdog pre-timeout dump, and one experiment
  that changes nothing by default
- **[Scheduling model](Scheduling-Model)** - `CONFIG_PREEMPT`, not `CONFIG_PREEMPT_RT`, since
  1.0.2, and what went away with it
- **[Branch model](Branch-Model)** - `openke` against `main`, and why the branch has that name
- **[Build and integration](Build-And-Integration)** - how the firmware consumes this
  repository, what stage 01 verifies, and why nothing is patched at build time any more
- **[The hardware](Hardware)** - SoC, display, touch

## What this wiki does not cover

- **The vendor SDK itself** - most of this tree, including `frameworks/`, `development/` and
  `external/`. Its own documentation is under `docs/doc/`, partly in Chinese.
- **U-Boot**, in `u-boot/`, with its own vendor history.
- **Real-time latency figures.** No latency was measured for these pages.
- **Hardware behaviour.** Nothing in this wiki was observed on a printer by its author; every
  claim is read from the source at `95d0321e`.

## Related

- [NebulaOS-firmware wiki](https://github.com/NebulaOS-KE/NebulaOS-firmware/wiki) - where the
  image is built, and the userspace half of the black box
- [Repository map](https://github.com/NebulaOS-KE/NebulaOS-firmware/wiki/Repository-Map) - every
  repository in the project, and what is upstream rather than ours
