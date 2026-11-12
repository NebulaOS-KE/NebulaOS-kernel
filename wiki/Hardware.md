# The hardware

What this kernel runs on. Written against `95d0321e`, the 1.0.2 pin.

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

