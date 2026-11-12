# Build and integration

How this repository is consumed, what the firmware build does with it, and what it no longer
does. Written against `95d0321e` (the 1.0.2 pin) and firmware `3bdf3ff`.

**Do not build this repository standalone expecting a printer image.** It does not produce one.
The firmware repository pins an exact commit here, checks it out, and builds the kernel as one
stage of its own pipeline:
[Building from source](https://github.com/NebulaOS-KE/NebulaOS-firmware/wiki/Building-From-Source).

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

`.github/workflows/build.yml` compile-checks the kernel on every push and pull request -
`xImage` for `nebulaos_halley5_v30`, with Ubuntu's MIPS cross compiler (GCC 13, as in the
image's Buildroot toolchain) and without the built-in Wi-Fi firmware blobs, which come from the
firmware build. It is a compile check, not a qualification: it produces no printer image and
nothing in it runs on hardware.

## What stage 01 actually checks

Worth knowing, because it is the gate a kernel change meets first. At 1.0.2
(`scripts/build/01-apply-kernel-patches.sh`) it refuses to continue unless:

- the checkout's `HEAD` is exactly `KERNEL_PIN`, read from the manifest rather than from a
  second hardcoded copy - and it checks this independently of
  `00-fetch-vendor-sources.sh`, so a hand-run `git pull` between the two scripts is still caught;
- the checkout has **no working-tree changes at all** - the build must never modify the kernel
  tree, because every NebulaOS kernel change is a commit here;
- five specific pieces of the fork's content are present: `ns2009.c`, the
  `panel-openke-general-480x272.c` display driver, `openke,bcm4343x-bt` in `hci_h5.c`,
  `ns2009@48` in `halley5_v30.dts`, and the literal line `gzip -nv9f $(obj)/vmlinux.bin` in
  `arch/mips/boot/zcompressed/Makefile`.

That last one is the build-reproducibility fix, and it shows how this repository and the
firmware divide the work: the `-n` flag that stops gzip storing a name and an mtime in the
compressed kernel is **a commit here** - `b31be02d9`, one line, `gzip -v9f` to `gzip -nv9f` in
`arch/mips/boot/zcompressed/Makefile`, an ancestor of the 1.0.2 pin - and the firmware's job is
only to verify it did not get lost. Without it the compressed kernel carried a name and an
mtime, which is what kept `xImage` from reproducing; with it, firmware `3bdf3ff` built twice
gave the same `xImage` byte for byte (the firmware's `docs/REPRODUCIBILITY.md`, corrected
2026-11-12 in `d464b46` - it previously credited this fix to a build-time variant script that
does not exist).

It does **not** check which branch the checkout is on. That check existed and was removed: a
checkout at an exact commit is a detached HEAD, so `git rev-parse --abbrev-ref HEAD` reports the
literal string `HEAD` and the check failed a checkout that was exactly correct. The pin check is
the real invariant.
