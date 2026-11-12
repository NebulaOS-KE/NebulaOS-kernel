# The scheduling model

Written against `95d0321e`, the 1.0.2 pin. This is the change most likely to surprise someone
who read an older document: NebulaOS is **not** a PREEMPT_RT system any more.

The kernel is built with **`CONFIG_PREEMPT`** (`nebulaos_halley5_v30_defconfig` line 7), not
`CONFIG_PREEMPT_RT`. The tree is still based on `linux-stable-rt`, so the RT patch set is
present in the source and the model is one defconfig line to switch; `localversion-rt` was
removed in `11f0f0c87` so a kernel that is not RT does not report `-rt79` in `uname`.

Two consequences a developer should know:

- `NEBULAOS_KTIMERS_PRIO` is gone with it - `ktimers/N` threads only exist on RT.
- The IRQ thread priority table stays and still applies: MIPS selects `IRQ_FORCED_THREADING`,
  and the explicitly threaded handlers (`uart1`, DWC2, mmc) are threaded on `PREEMPT` too.

