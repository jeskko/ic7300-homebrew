# Display: VDC5 graphics planes (2026-09-25)

What the firmware does with the RZ/A1H VDC5 (channel 0, `0xFCFF7400`). Read live from the
registers in `qemu-machine` after boot; register meanings are from the Renesas VDC5 driver in
`scratch/r01an5093ej0170-rza1-swpkg/.../drivers/r_vdc_vdec/vdc_h/src/r_vdc_register.c` and the
RZ/A1H hardware manual ch. 35 ("Image Synthesizer",
`/data/misc/icom/7300/doc/REN_r01uh0403ej0700_rz_a1h_MAH_20240930.pdf`).

## Planes

The four graphics planes stack in a fixed hardware order, GR0 (bottom) < GR1 < GR2 < GR3 (top).
Each plane's `AB1[1:0]` `DISP_SEL` picks what it shows: 0 = background colour, 1 = the layers
below (transparent), 2 = this plane only, 3 = this plane alpha-blended over the layers below.

| Plane | Registers | State after boot |
|---|---|---|
| GR0 | `+0x200` | off (`FLM_RD` = 0) |
| GR1 | `+0x900` | off |
| **GR2** | `+0x300` | **the UI**: read on, `DISP_SEL` = 3 (blend), RGB565 (`FLM6` = `0x01df1800`: format 0, width 480, read-swap 6), stride 960 (`FLM3`), 272 lines (`FLM5` = `0x010f07ff`), area VS=12/VW=272, HS=35/HW=480 (`AB2` `0x000c0110`, `AB3` `0x002301e0`), framebuffer **`0x20974fe0`**, never seen to change (sampled 40× over 2 s: no plane-level double buffering) |
| GR3 | `+0x380` | configured but transparent: read off, `DISP_SEL` = 1 (lower) |

The UI is drawn into the GR2 framebuffer by the OpenVG GPU (`notes/kernel-rtos.md`,
`ui_graphics_lifecycle_task`), continuously, with a new frame every 40–100 ms.

Per-plane register offsets: `UPDATE` +0x00 (b0 `IBUS_VEN`, b4 `P_VEN`, b8 `UPDATE`: new values
take effect at the next vsync; the bits read 1 until then), `FLM_RD` +0x04 (b0 = read enable),
`FLM1`..`FLM6` +0x08..+0x1c (`FLM2` = base), `AB1`..`AB11` +0x20..+0x48.

## Use

`sdk/runtime/gfx.c` gives apps GR3: it copies GR2's geometry, sets its own framebuffer and
`DISP_SEL` = 2, enables the read, and flips by rewriting `FLM2` with `IBUS_VEN`. On close it puts
GR3 back to exactly the state above. Live-tested in the emulator, whose `vdc5.c` now composites
planes in this order.

## RAM around the framebuffer

A live zero-map of `0x20600000`–`0x209fffff` (64 KB granularity, after boot, before any app ran)
found data in use from `0x20800000` up, including this framebuffer. `0x20610000`–`0x207fffff`
was entirely zero; the SDK (ABI v3) uses `0x20710000`–`0x2078fbff` for app framebuffers (v2: `0x20640000`–`0x206bffff`). This is the
same "zero after boot" level of evidence as `sdk/app-loader-design.md`'s RAM-placement section,
not a marker-then-reboot test.
