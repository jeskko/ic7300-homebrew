# Front-panel MCU (`IC501`) firmware — session history

Full narrative and evidence trail behind [notes/front-panel-firmware.md](front-panel-firmware.md),
which carries only the current-state summary and open questions. Kept verbatim, including the RL78
tooling investigation that turned out to be pointed at the wrong file — it remains sound and reusable
groundwork for whenever real front-panel firmware is located.

## Original handoff (written before any real analysis)

Picking up a new thread after the DSP-firmware-location work in [[multi-cpu-images]]. This started as
a **handoff plan**, written before any real analysis of the front-panel firmware itself.

### What was already known going in

- **`IC501` = `R5F104LCAFB`**, a Renesas **RL78/G14** MCU (64-pin LQFP), the Display/Front Unit's MCU —
  the "SX3765" mystery from an early session, resolved via the service manual's parts list and
  independently visible on the Front Unit schematic. See [[icom-ic7300-re-project]]'s memory-file history
  and `notes/ic7300-hardware.md`.
- **Full physical pinout already supplied by the user**: 8 buttons on direct individual GPIOs `P70`-`P77`
  (including `MENUK`=`P73`=MENU, confirmed elsewhere `P74`/`SCPEK`=FUNCTION per a schematic-silkscreen
  correction), a 16-button resistor-multiplexed matrix on 4 pins, and two quadrature dial-encoder pairs. See
  `notes/kernel-rtos.md`'s "Factory/service mode" section for the exact citation.
- **The main-CPU side of the link to this MCU is fully reverse-engineered already**: `SCIF3` (base
  `0xE8008800`, pins `P6_0`/`P6_1`), a 33-byte packet-framed UART, `0xFE`/`0xFD` byte framing (same shared
  template as CI-V/`SCIF0` and the service-mode link/`SCIF1`). Key functions already named in Ghidra:
  `scif3_frame_rx_statemachine`, `scif3_frame_dispatch_by_type` (dispatches complete frames by type byte
  into a shared front-panel status buffer). `boot_check_mode1_combo`/`boot_check_mode5_combo` (in
  `cold_boot_hw_init`) read specific bits of that status buffer to detect the documented MENU+FUNCTION
  service-mode combo — buffer offset `0xd` bit 3 = MENU, bit 4 = FUNCTION, confirmed exactly. See
  `notes/kernel-rtos.md`'s "Factory/service mode" section for the full derivation.
- **The firmware image itself was extracted, decompressed, and file-offset-verified** (2026-08-29):
  `scratch/unpacked/142/front_cpu.bin`, 163,592 bytes, produced by `tools/icom_fw/dsp_chunks.py` from
  the v1.42 update container's "component0" (file offset `0x252c2c`, compressed `0x17e46`→decompressed
  `0x27f08` bytes, LZSS, MD5-trailer-verified against the real container). **"Front CPU" was a
  working-hypothesis label for this component** (component ordering matched the `FUN_200a94c8`
  version-field order, not independently confirmed) — this hedge turned out to matter, see below.

### Toolability, checked concretely — better news than the DSP case (at the time)

Unlike the DSP (TMS320C6745, confirmed **no** disassembler support anywhere — see [[multi-cpu-images]]'s
"DSP firmware precisely located" section), RL78 has real, if unofficial, Ghidra support:

- **[xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78)** — a community RL78 processor module. Per
  its own README: disassembles test binaries to a good degree of accuracy, has known bugs but "mostly
  functions"; the decompiler handles basic control flow but has limitations (notably switch-idiom
  handling), and some less-common instructions are still unimplemented (none of the most common ones,
  per the README).
- **[hedgeberg/RL78_sleigh](https://github.com/hedgeberg/RL78_sleigh)** — a second, independent RL78
  SLEIGH implementation, kept as a fallback, not evaluated.
- Checked and confirmed **absent** on this machine: no `rl78-elf-objdump`/binutils target (at the time —
  see below, this changed), no RL78 support in Capstone.

### Tooling stood up, both paths work; file has a header, not raw code at offset 0 (2026-08-29)

Both toolability questions from the handoff resolved, better than expected:

- **`xyzz/ghidra-rl78` installs and loads cleanly in this Ghidra (12.1.2).** No root needed — it's a
  processor-module directory (`Module.manifest` + `data/languages/{rl78.ldefs,pspec,cspec,slaspec}`, no
  compiled Java at all), so it drops straight into the per-user extensions dir
  (`~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/rl78/`), the same place `GhidraMCP` already lives. On
  headless import (`analyzeHeadless ... -processor RL78:LE:16:default`), Ghidra auto-compiled the 224 KB
  `.slaspec` with only a benign warning ("7 NOP constructors found") and import/analysis succeeded. Real
  disassembly/decompilation testing not done yet — this only confirms the module *loads*, not that its
  instruction semantics are trustworthy.
- **Better find: RL78 needs no community tool at all for disassembly — mainline GNU binutils has had an
  official `rl78-dis.c` in `opcodes/` for years.** No Renesas-specific toolchain, no packaging on this
  machine either, but building it from plain upstream source was trivial: fetched `binutils-2.44` from
  `ftp.gnu.org`, `configure --target=rl78-elf --disable-nls --disable-werror --disable-gdb --disable-sim
  --disable-gprof`, then `make all-libiberty all-bfd all-opcodes all-binutils` — clean build, ~2 minutes,
  no patches needed. Installed at `~/.local/rl78-binutils/bin/{rl78-objdump,rl78-readelf}` (outside the
  repo — reproducible from the recipe above, not committed as a binary blob). Usage:
  `rl78-objdump -m rl78 -b binary -D front_cpu.bin`. This was the primary disassembly tool for this
  thread while it was believed active — official upstream code, not a reverse-engineered guess.
- **Important correction to the handoff's assumption: `front_cpu.bin` is *not* a raw code-at-offset-0
  image.** Linear disassembly from byte 0 with `rl78-objdump` decodes the `"TIPAcYSX"` region as nonsense
  instructions (e.g. `mov e, #73` / `mov x, #65` off literal ASCII bytes) — confirming these are header
  bytes, not code. The first ~0x22 (34) bytes look like a small structured header: magic `"TIPAcYSX"` (8
  bytes) followed by a run of small integers and **three repeats of the 3-byte tag `"YSX"`** at offsets
  0x05, 0x09, and 0x1d — shape never understood (record/section markers? length-prefixed fields? too
  little data to tell, and moot once the file was reclassified — see below).
- **Working hypothesis at the time, never confirmed: real code starts around offset 0x22.** Byte 0x23
  decoded as `subw sp, #123` (reserving stack space) right after a single odd byte at 0x22 — a plausible
  function-prologue shape — but the following stream wasn't unambiguously clean (e.g. `or a, [hl+108]`
  recurring identically at 0x2c and 0x42, which could be a real repeated field access or could mean the
  alignment was still off by one or more bytes). This was explicitly flagged as unconfirmed pending the
  real vector-table location and RL78/G14 datasheet — that check is what overturned the whole premise.

## This file's entire premise overturned, same day (2026-08-29)

Two independent lines of evidence both pointed away from `front_cpu.bin` actually being `IC501`'s
firmware:

1. **The datasheet number checks out, and it's a real contradiction, not a rounding error.** Renesas's own
   part page for `R5F104LCAFB` confirms 32 KB code flash / 4 KB data flash / 4 KB RAM. `front_cpu.bin`'s
   actual content (past the ~34-byte header, before the trailing `0xFF` pad) runs to `0x20c80` —
   **134,272 bytes, over 4x the entire chip's flash capacity.** Byte-entropy profiling and a zoomed 1bpp
   bitmap render (prompted by the user spotting visible periodicity in GIMP) ruled out "it's actually
   graphics/font assets" as the explanation — no recognizable local image content anywhere, just a genuine
   ~16-32 byte statistical periodicity sustained across the whole content region, more consistent with
   dense code/data than a picture. The entropy profile itself sits closer to `dsp_program.bin`'s dense,
   uniform VLIW-code signature than to confirmed-real ARM code in `body.bin`.
2. **The main-CPU update mechanism itself doesn't support the label.** Traced `chunk_transport_send_data`
   (the function that sends this exact chunk, index 0, during a firmware update) all the way down through
   `dsp_page_transfer_verify` and the ring-buffer push helpers — every one of them is hard-wired to the
   `SCIF5`/DSP transport (`scif5_send_and_wait_reply`, `scif5_ring_push_word`), with **no branch anywhere
   that picks the front-panel `SCIF3` link for chunk 0**. See [[multi-cpu-images]]'s "The 'chunk 0 = Front
   CPU' label is now actively doubtful" section for the full trace — this is the same finding from the
   main-firmware side that independently corroborates the forensic finding above.

**Working conclusion, upgraded from "probably not" to "positively looks like" (same day, continued)**:
pointed the DSP thread's newly-built disassemblers (`tic6x-objdump -EL`, Capstone `CS_ARCH_TMS320C64X` —
see [[multi-cpu-images]]'s DSP disassembly section) at `front_cpu.bin` and got a strong, cross-validated
positive result: real C67x+/C674x floating-point instruction sequences
(`spdp`/`dpsp`/`absdp`/`cmpltdp`/`mpydp`/`adddp`/`intspu`, used in coherent order), repeated correct
`mvk`/`mvkh` 32-bit-constant-building pairs, a genuine local backward branch to an in-range address, and an
`addkpc` whose encoded target is exactly `self+4` — the textbook call/return-address idiom. Two independent
disassemblers (binutils and Capstone) agree byte-for-byte on decoded immediates at the same addresses.
**`front_cpu.bin` now looks like genuine TMS320C674x object code**, not RL78 code, not graphics, and not
just a vague "some other DSP blob" — a specific, positively-supported identity. Full detail in
[[multi-cpu-images]]'s "Pointed the new DSP disassemblers at `front_cpu.bin`" section.

The RL78 tooling stood up in the first half of this investigation (Ghidra `xyzz/ghidra-rl78` module,
`rl78-objdump` from stock binutils) is still sound and reusable for whenever real front-panel firmware is
located — but **this specific file is not that**, and shouldn't be pointed at with that expectation
anymore. The plausible-looking RL78 disassembly patterns found earlier (call-target convergence, sane
branch offsets) were real but weak evidence next to this — a sufficiently dense binary blob run through a
permissive variable-length CISC decoder can produce locally-plausible-looking sequences by chance over a
small sample; they don't outweigh the specific, cross-validated C674x floating-point evidence found since.

**Genuinely open, not resolved**: whether real `IC501` firmware exists anywhere in the update container
under a different mechanism, or whether it's provisioned some other way entirely (factory-programmed once,
never updated over this path?). Worth checking whether the update file's "Front CPU" version field
(`FUN_200a94c8`'s `+0xa4`) is ever compared against anything read from `IC501` itself (e.g. over `SCIF3`)
rather than assumed — that would be the natural next thread, not a continuation of the RL78-disassembly
work done above.

## Priority-1 handout question resolved: yes, physical keys reach the main CPU over `SCIF3` (2026-09-07)

Picked the thread back up via `notes/front-panel-protocol-handout.md`'s top-priority open question: does
`key_event_resolve_and_route`'s raw key-code struct (the handout's own `0x200301f2`) have any real
connection to the confirmed `SCIF3` status buffer (`0x203dcab6`), given a same-day `references_to` search
on both raw addresses had found zero static links?

**First step: found and fixed a real address typo in the handout itself.** `0x200301f2` has no Ghidra
symbol at all; the actual address `key_event_resolve_and_route` reads (`DAT_2002f74c[4]`, confirmed by
reading the pointer variable's stored bytes directly — `f2 01 39 20` little-endian) is **`0x203901f2`**,
which does have a live symbol (`DAT_203901f2`). A single mistyped digit (`2003` vs `2039`) meant every
literal-address search run against the handout's number — both the live `references_to` call and a fresh
whole-image raw-byte scan of `body.bin` for the literal word `0x200301f2` — was silently searching for an
address nothing in the firmware ever references. Re-running the exact same searches against the corrected
`0x203901f2` immediately surfaced real hits, including a `WRITE` reference Ghidra's own analysis had
already resolved through the pointer-variable indirection (something a plain literal-word grep of the raw
file can't see at all, since the address only ever exists as the *stored contents* of pointer variables
like `DAT_2002f74c`, never as a literal instruction operand).

**The writer, found and confirmed**: `scif3_key_bitfield_scan_and_resolve` (renamed from `FUN_2002fbc8`,
`0x2002fbc8`) — called every tick from `ui_input_poll_tick` (`0x2002fca8`), right before it conditionally
calls `key_event_resolve_and_route`. It:

1. Reads a 0-4 rotating index (`DAT_2002f74c+2`, i.e. the *same* struct, a different field) and increments/
   wraps it mod 5.
2. Compares one live byte at that index — via `scif3_status_live_byte_ptr` (renamed from `FUN_2002fbb8`),
   which resolves to `DAT_200306b4 + 0xd + index` — against a cached "previous scan" byte at the same
   index via `scif3_key_shadow_byte_ptr` (renamed from `FUN_2002fba8`), `DAT_2002f750 + 0xd + index`.
3. **`DAT_200306b4`'s stored value is `0x203dcab6`** — read directly, confirmed byte-for-byte — the exact,
   already-known `SCIF3` front-panel status buffer address (`scif3_frame_dispatch_by_type`'s own
   destination, reached there via its own separate pointer copy `DAT_20037590`). So this function's "live"
   byte really is a live `SCIF3`-populated byte, at buffer offset `0xd`-`0x11` (5 bytes, indices 0-4) — the
   *exact same offset* `boot_check_mode1_combo`/`_mode5_combo` already read bits 3/4 of for the MENU+
   FUNCTION service-mode combo. `DAT_2002f750` (`0x203dca96`) sits exactly `0x20` bytes before it — most
   likely a sibling field inside one larger front-panel-status struct, the shadow/previous-scan copy this
   function itself maintains for edge-detection, not another live hardware buffer.
4. On any bit difference (XORed against a per-index mask table at `DAT_2002f754`/`0x2018d904`), finds the
   changed bit's index (0-39 across the 5 bytes) and looks it up in `g_scif3_bit_to_keycode_table` (renamed
   from `DAT_2018d980`, `0x2018d980`) to get a resolved key code, then writes that code into
   `*(DAT_2002f74c+4)` — `0x203901f6`, the exact struct field `key_event_resolve_and_route` reads to decide
   what command to dispatch.

**Verified against already-known key codes, not just plausible-looking**: `g_scif3_bit_to_keycode_table`'s
bytes at index 16 and 23 are `0x09` and `0x0c` — exactly the confirmed `MENU`=9 and `QUICK`=12 key codes
from `notes/ui-menu.md`'s physical-button-chain trace. Both indices land inside the same `0xd`-`0x11`
byte range read from the live `SCIF3` buffer, closing the loop cleanly.

**Answer to the handout's open question**: possibility 2 was right — there *is* a copy from the `SCIF3`
buffer into the key-code struct, through a genuinely computed/indirect path (a rotating index into a small
window of the buffer, diffed bit-by-bit against a shadow copy, translated through a lookup table) that a
plain address `references_to`/literal-word search on the final destination address alone could never catch
— only tracing forward from the confirmed-real `SCIF3` buffer address found it. Physical button presses
**do** reach the main CPU over `SCIF3`; `key_event_resolve_and_route`'s struct is downstream of it, not an
independent, unrelated input path as the two struct addresses looking "disconnected" had suggested.

**Left open, not chased further this pass**: `FUN_2002fa9c` (called by `ui_input_poll_tick` as a fallback
when `scif3_key_bitfield_scan_and_resolve` returns nothing pending) reads two *different*, fixed bit tests
(`DAT_2002f750+0xf`, `+0x11`) against threshold counters in a separate small struct (`DAT_2002f764`) rather
than the same bit-diff+table pattern — plausibly the encoder/dial or a couple of discrete non-matrix
buttons, not yet confirmed. Also unconfirmed: whether `DAT_2002f750`/`DAT_200306b4` are truly two fields of
one larger struct (the round `0x20`-byte offset between them strongly suggests it) or coincidentally
adjacent separate allocations — would need the struct's full field layout mapped to settle for certain.

## How the main CPU gets the front-panel version, and a check for a firmware-write mechanism (2026-09-07)

Same session as the button-press trace above. User asked two concrete questions: (1) the version-info
menu screen shows a front-panel firmware version — how does the main CPU actually get that number, and
(2) search for any code that could write a firmware image to the front panel (the handout's goal 2).

### Finding the version-info screen's own code

Found the real UI strings first: `"Main CPU:"`, `"Front CPU:"`, `"DSP Data:"`, `"DSP Program:"`, `"FPGA:"`
all live together at `~0x2035e3f4`-`0x2035e420`, inside the same menu-string-table region
(`~0x2035a000`-`0x2035f000`) `notes/diode-matrix.md`/this thread's own earlier sessions had already
located. Unlike the still-unwalked menu *name* table, these five labels had real, direct code references —
all landing within a ~1.7 KB function at `0x200a94c8`, renamed `ui_version_screen_draw_and_compare`.

That function does two things: draws the 5 labeled version fields, and (in an earlier branch) compares two
4-byte version structs — `DAT_200a9ba4` (holds `0x203ff76c`, the "current/installed" struct, renamed
`g_screen_display_scratch_buf` since it turned out to be a generic per-screen scratch buffer reused by
unrelated screens too — see below) and `DAT_200a9ba8` (holds `0x20404654`, the "update-candidate" struct,
renamed `g_update_candidate_version_struct`) — via a 4-byte memcmp-style helper (`FUN_2017c81e`), to decide
whether to draw an "update available" banner. The 5 displayed fields read from the *current* struct at
fixed offsets `+0xa0`/`+0xa4`/`+0xa8`/`+0xac`/`+0xb0` = Main CPU / **Front CPU** / DSP Program / DSP Data /
FPGA (order confirmed by matching each label string's exact byte length — 9/10/12/9/5 characters — against
the draw loop's own length arguments, since the loop order in code doesn't match the labels' storage order
in the string table).

**A real methodology near-miss**: `references_to` on the "current" struct's Front-CPU field
(`0x203ff76c+0xa4` = `0x203ff810`) turned up 3 writers, but 2 of them (inside `FUN_20042f3c` and
`FUN_20043f48`) turned out to be completely unrelated screens (a band-scope/memory-channel scanner and an
SWR/power-meter-style ADC reader respectively) that happen to reuse the exact same scratch-buffer address
for their own, differently-typed data at the same byte offset — confirmed by checking each writer's own
containing function via `references_to` on the *function* address itself (both resolve to entries in a
screen-dispatch function-pointer table, the same pattern as the already-known 279-entry system command
table), not by assuming the struct's purpose from its address alone. Renamed the struct
`g_screen_display_scratch_buf` precisely to flag this reuse for future sessions — don't assume writes to
its `+0xa0`-`+0xb4` range are version-related without checking which screen's populate-callback is doing
the writing.

### The real writer: `ui_version_screen_populate_fields` (`0x2004366c`, renamed from `FUN_2004366c`)

The third writer was the real one. Confirmed via `references_to` on its own address (a `DATA` reference
from `0x20190214`, a screen-dispatch table slot — i.e. this function is a per-screen "populate my fields"
callback, invoked when the version-info screen is opened, not a generic tick). It writes all 5 fields:

- **Main CPU** (`+0xa0`, 4 bytes): a straight 4-byte memcpy from `DAT_2004372c` — which, read directly,
  turned out to hold the *literal ASCII bytes* `"1.42"`, not a pointer at all. This is the main firmware's
  own compiled-in version string (matches this exact v1.42 image) — cosmetic/build-time, no live query
  needed, as expected.
- **Front CPU** (`+0xa4`..`+0xa7`, formatted specially): `*(iVar1+0xa4) = *(DAT_20043734+1) + '0'`, then a
  literal `'.'`, then two more digit-plus-`'0'` conversions from `DAT_20043734+2`/`+3`. **`DAT_20043734`,
  read directly, holds `0x203dca96`** — instantly recognizable as the exact same address already found
  and named `g_frontpanel_latched_status` earlier this session (the struct sitting `0x20` bytes before the
  confirmed `SCIF3` rx buffer, previously known only as the key-bit-scan's shadow-cache base). So the
  Front-CPU version's 3 source bytes live at offsets `+1`/`+2`/`+3` of that *same* struct — a genuinely
  different field than the `+0xd`-`+0x11` shadow-cache bytes the key scanner uses, but the same base
  address, confirming this struct really is a shared, multi-field "latched front-panel status" record.
- **DSP Program/Data/FPGA** (`+0xa8`/`+0xac`/`+0xb0`): straight 4-byte copies from `DAT_20043738`
  (`+6`/`+0x13`/`+0x20`) — not traced further this session, presumably the already-identified DSP/FPGA
  component version fields from `notes/multi-cpu-images.md`.

### Tracing how `g_frontpanel_latched_status+1..+3` actually gets its bytes — a real boot-time SCIF3 handshake

`references_to` on `g_frontpanel_latched_status+1` (`0x203dca97`) found a real `WRITE` at `0x2002afa0`,
inside a function immediately renamed `scif3_frontpanel_init_and_latch_version` (from `FUN_2002af80`).
That function:

1. Calls `scif3_rx_buffer_reset_defaults` (from `FUN_2002aeec`) — fills `g_scif3_rx_status_buffer`
   (`0x203dcab6`, the confirmed live `SCIF3` rx buffer `scif3_frame_dispatch_by_type` writes into) with a
   default pattern: byte 0 = 0, bytes 1-12 = `0x20` (ASCII space), bytes `0xd`-`0x1d` = 0, bytes
   `0x1e`-`0x1f` = 1 — i.e. a "blank/unpopulated" placeholder before any real reply arrives.
2. Calls `scif3_driver_init()` (already named from an earlier session).
3. Calls `scif3_frontpanel_identify_handshake` (from `FUN_20037424`) — **the real find**. This function
   sets a status-flag bit (`0x80` in `DAT_20037588`) and a byte (`5`) in a separate control field, then
   loops calling `scif3_driver_pump_tick` (renamed from `FUN_200373ac`, with a ~0x4b-tick timeout) until
   that flag clears. Decompiling the pump function's own bit-`0x80` branch showed exactly what triggers:
   once its internal counter reaches `5` (matching the value just written), it constructs and sends — via
   `scif3_send_frame`, renamed from `FUN_20037214` — a genuine **outbound**, `0xFE`-framed packet with type
   byte **`0xF0`**. `0xF0` is the exact type `scif3_frame_dispatch_by_type` already special-cases on
   *receive* (clears the same `0x80` bit, sets others) — the two sides of one real handshake. So this is a
   confirmed **main-CPU-initiated "are you there / identify yourself" request sent to the front panel**,
   with the call blocking (bounded by a real timeout) for its reply.
4. After the handshake call returns, copies `g_scif3_rx_status_buffer+1..+12` into
   `g_frontpanel_latched_status+1..+12` at matching offsets — i.e. whatever the front panel's reply put
   into the rx buffer (via frame types landing at those same offsets — type *is* the destination offset in
   `scif3_frame_dispatch_by_type`, so types `0x01`/`0x05`/`0x07`/`0x0b` specifically) gets latched into the
   struct `ui_version_screen_populate_fields` later reads bytes `+1`-`+3` of.

Confirmed this whole handshake is **boot-once, not a live per-visit query**:
`scif3_frontpanel_identify_handshake` has exactly one caller (`scif3_frontpanel_init_and_latch_version`),
which itself has exactly one caller (`cold_boot_hw_init` — the same function that already runs
`boot_check_mode1_combo`/`boot_check_mode5_combo` for the service-mode combo). Visiting the version-info
screen later just displays whatever got latched at that one boot-time exchange, not a fresh query.

**Answer to the user's question**: the main CPU learns `IC501`'s firmware version through a genuine,
one-shot `SCIF3` handshake at cold boot — it sends an outbound `0xF0` "identify" frame, blocks for the
front panel's reply (with a timeout), and latches specific reply bytes into a status struct that the
version-info screen later formats and displays as `"Front CPU: X.YZ"`. It is a real live query of the
actual attached hardware, not a stored/cosmetic value — but it only happens once per power-on, not each
time the menu is opened. **Not yet decoded**: exactly which frame type(s) the front panel's reply uses, and
what the still-uncopied byte 0 of the rx buffer (type `0x00`) might carry.

### Checking for a firmware-write mechanism (handout goal 2) — real negative result, not exhaustive

With the outbound-handshake mechanism now confirmed, the natural next check was whether the *same* outbound
path (or any other) could carry a chunked firmware image rather than just a tiny status handshake.
`scif3_send_frame` (the low-level frame-construction primitive `scif3_frontpanel_identify_handshake` and
this thread's other outbound sends. all funnel through) has exactly 3 call sites — and all 3 are inside
`scif3_driver_pump_tick`, which is in turn `scif3_send_frame`'s *only* caller. So the entire outbound
`SCIF3` driver is fully enclosed in these two functions. Reading through `scif3_driver_pump_tick`'s other
branches (triggered by status bits `0x08`/`0x10`/`0x20`/`0x40`, separate from the `0xF0`/`0xF1` handshake
path): every one sends a single frame of at most ~33 bytes (matching the known packet size), built from a
small fixed or short computed buffer — nothing resembling a chunked-write loop, no size/offset header
fields being walked, no erase/program-shaped sequence anywhere in this call graph.

**This is a real, checked negative result for goal 2** — no firmware-image-write mechanism to the front
panel exists in the confirmed outbound-`SCIF3` code path. It is **not** a whole-image sweep, though: this
only rules out the one outbound driver found via `scif3_send_frame`'s call graph. If `IC501` can be
field-updated at all, either the mechanism lives somewhere this session didn't reach, or (increasingly
plausible given how narrow and simple this driver turned out to be) it genuinely isn't field-updated over
`SCIF3` at all — factory-programmed once, matching one of the handout's original open questions.

### Correction, same day: the version IS compared, not just stored/displayed

Asked directly whether the front-panel version is ever compared to anything, or just stored in memory —
re-checked `ui_version_screen_draw_and_compare`'s own decompile (already pulled earlier this session, just
not highlighted). It's an active 4-byte compare, not a passive display:

```c
puVar3 = DAT_200a9ba8;   // g_update_candidate_version_struct (0x20404654)
iVar2  = DAT_200a9ba4;   // g_screen_display_scratch_buf (0x203ff76c)
...
if ( ... || (iVar5 = FUN_2017c81e(iVar2 + 0xa4, puVar3 + 4, 4), iVar5 != 0) || ... ) {
    uVar9 = uVar9 | 0x8000;   // "versions differ" flag
}
```

`iVar2+0xa4` is exactly the field this session traced back to the boot-time `SCIF3` handshake
(`scif3_frontpanel_identify_handshake`). `puVar3+4` (ushort-indexed, i.e. `+8` bytes) is the matching field
of the "candidate" struct. The same 4-byte-compare-then-OR-into-one-flag pattern runs for all 5 components
(Main CPU/Front CPU/DSP Program/DSP Data/FPGA) in this one `if`, and the flag drives the "new firmware
available" banner/detail block right after it (the same `FUN_200ace08`/`FUN_200ac6e0` UI-drawing calls
already noted). Also found a second, structurally near-identical function, `FUN_2009e8c0`, doing the exact
same current-vs-candidate diff over a sibling pair of `DAT_2009f1f4`/`DAT_2009f1f8` structs, driving a
different set of screen IDs (`0x261`-`0x269` vs `0x26a`-`0x272`) — very likely the SD-card-insert "update
available" notification rather than the manually-opened menu screen. Two independent call sites doing the
same comparison is good corroboration this is real, load-bearing update-detection logic, not a display-only
coincidence.

**Not yet traced**: where `g_update_candidate_version_struct` itself gets populated. No direct `WRITE`
reference was found to its base address or its `+8` (Front CPU) field specifically — consistent with it
being filled by a single bulk copy whose destination is computed rather than a fixed literal operand, the
same class of gap this project keeps hitting.

### Full scenario breakdown of `ui_version_screen_draw_and_compare` (asked for directly, 2026-09-07)

Full decompile re-read to pin down exactly what happens in each branch, not just that a comparison exists:

1. **The diff itself**: one big `||`-chained `if`, 4-byte-comparing `current`'s (`g_screen_display_scratch_buf`)
   Main CPU/Front CPU/DSP Program/DSP Data/FPGA fields (`+0xa0`/`+0xa4`/`+0xa8`/`+0xac`/`+0xb0`) against
   `candidate`'s (`g_update_candidate_version_struct`) matching fields (`+4`/`+8`/`+0xc`/`+0x10`/`+0x14` in
   `ushort` units), plus 3 header-byte checks (`+0x9c`/`+0x9e`/`+0x9f`). **Any single mismatch anywhere
   sets one shared flag** (`uVar9 |= 0x8000`) — no per-component granularity at this stage.
2. **Scenario A — everything matches, including one more independent check**: if the flag is clear *and*
   `current+0xb4` also equals `candidate`'s corresponding byte (`puVar3[0xc]`, i.e. candidate `+0x18`),
   jumps straight to the tail section, skipping the entire detail block below.
3. **Scenario B — a mismatch was found**: draws a whole detail panel at widget `0x26a`/`0x26c`/`0x26d`/
   `0x26e` — a title string assembled from a table lookup (indexed by a 2-bit mode selector and a language/
   variant bit) plus `current+0x84`'s own string, a formatted `"current[0x9e]/current[0x9f]"` counter (a
   plain `"NN/NN"` string — meaning not yet confirmed), then **an unrolled loop over all 5 components**,
   each drawing its label string (from `DAT_200a9bb0` at `+4`/`+0x10`/`+0x1c`/`+0x28`/`+0x34`) and its raw
   *current*-side 4 bytes as the displayed value (not the candidate's — this panel shows "what's currently
   installed," not "what's on offer"). **Correction, 2026-09-07, same day, caught by the user pushing back
   on "why show a progress bar if no update is happening"**: an earlier pass of this write-up wrongly
   attributed a percentage/gauge-bar computation (`iVar9*100/local_8c`, split into two draw calls) to
   *this* function — re-read the fresh decompile line-by-line and confirmed **no such code exists anywhere
   in `ui_version_screen_draw_and_compare`**. That computation is real, but belongs to `FUN_2009e8c0` (see
   below) — a mix-up from working on both functions in the same session. There is no progress bar in the
   actual version-info mismatch screen.
4. **Tail section, always evaluated** (whether or not scenario B's block ran): three near-identical blocks
   for widget IDs `0x270`/`0x271`/`0x272`. Each is gated by the *same* condition,
   `(uVar9 != 0) || (current+0xb4 != candidate's matching byte)` — i.e. re-evaluated independently per
   block, not reusing scenario A/B's earlier branch outcome. Each draws a fixed banner/icon regardless, and
   additionally draws an overlay/checkmark icon specifically when `current+0xb4` equals `1`, `2`, or `3`
   respectively (one value per block). Reads as **three fixed status rows**, each highlighted when a
   single "which single item is out of sync" byte (`current+0xb4`) points at that row — plausibly one row
   per firmware group, not yet confirmed which.

### What the candidate struct really is — retracting the "SD-card update file" guess

Went looking for `g_update_candidate_version_struct`'s writer to settle the open question from last
session. Checked all 11 raw-literal-word references to its base address (`0x20404654`) found via a
whole-image scan, resolving each to its containing function. Most didn't resolve directly (literal-pool
words sitting in code regions Ghidra's own `references_to` had already tracked through constant
propagation instead — the real consuming instructions are a *different* address list, already used to find
`ui_version_screen_draw_and_compare` and `FUN_2009e8c0`). Two of those real references were tagged `PARAM`
(`0x2008aac4`, `0x2008abc0`) — worth checking directly since a `PARAM` tag on a struct address is exactly
the shape a bulk-copy destination argument would leave.

Reading the raw listing at that address's literal pool (`0x2008abd4`-`0x2008abf4`) instead of trusting the
decompiler's variable names turned up the real finding: `DAT_2008abd8` = `0x203ff76c`
(`g_screen_display_scratch_buf`'s own base, confirmed byte-for-byte) and `DAT_2008abe0` = `0x204045b8` =
`g_update_candidate_version_struct + 0x9c` **exactly** (`0x20404654 + 0x9c = 0x204045b8`). Both pointers
belong to `FUN_2008cff8` — a **memory-channel-editor screen**, comparing channel number/mode/split-state
bytes at that same `+0x9c` offset convention, with zero relation to firmware versions. This is the *same*
struct-reuse pattern already flagged for `g_screen_display_scratch_buf` (52 reference sites across
unrelated screens) — now confirmed for the "candidate" side too, at least one unrelated user found.

**A second unrelated screen found sharing the same buffer, same day, while chasing the progress-bar
correction above**: `FUN_2009e8c0` — previously guessed to be "likely the SD-card-insert firmware-update
notification screen" purely from its structural similarity to `ui_version_screen_draw_and_compare` and its
adjacent screen-ID range (`0x261`-`0x269` vs `0x26a`-`0x272`) — **that guess is also wrong.** Checked the
string it references directly (`0x2009f210`): decodes to `"(REC:"`. Combined with its own formatting code
(one field rendered as `"X.Y MB"`, another as `"NNh NNm"`), this is a **recording/QSO-recorder storage-
capacity display** ("REC: <space used> / <duration>"), not a firmware-related screen at all — a third
confirmed unrelated reuse of the same generic buffer pair, on top of the memory-channel editor above. Its
percentage/gauge-bar computation (`iVar9*100/local_8c`, current+0xa0/+0xa4 read as a numeric ratio, not
version bytes) is exactly this screen's own "how much recording storage is used" progress bar — real code,
just never part of the firmware-version comparison at all.

**This retracts the earlier "likely an SD-card update file's header" hypothesis** — there's no positive
evidence for it, and real evidence now points the other way (a struct genuinely shared by content that
has nothing to do with update files). **New leading hypothesis, not confirmed**: `current`/`candidate` are
a generic previous-frame-vs-current-frame snapshot pair, reused across many unrelated screens as a cheap
redraw-skip optimization — each screen's own populate function fills `current` with fresh values every
tick, and whatever fills `candidate` (still not found) holds "what was on screen last render," so a widget
only gets redrawn when its specific byte range actually changed since then. Under this reading,
`ui_version_screen_draw_and_compare`'s "diff" isn't really "is an update available" logic at all — it's
"did anything about the version-info screen's own displayed values change since I last drew this screen,"
which happens to *also* answer the update-availability question correctly if `candidate` gets refreshed
from a real update file at some point, but that refresh step is now the genuinely open, unconfirmed part.
The version-info screen's own field *interpretation* (which offset means which component) stays solid —
verified independently via the label strings drawn alongside each value, unaffected by this correction.

**Still open**: who writes `g_update_candidate_version_struct`, and whether it's ever actually sourced from
a real SD-card update file (settling the original "is update-availability really detected this way"
question) or is purely a same-buffer-type previous-frame snapshot with no tie to update files at all.

**Follow-up, same day — reconsidering the retraction above after the user pushed back.** The user's own
reading (why compare firmware versions at all if there's no push mechanism, unless the comparison's real
job is telling the user "you're out of sync, go run a manual update") is better-motivated than my "generic
redraw-skip snapshot" hypothesis. **Walking back part of the retraction**: finding the same RAM reused by
two unrelated screens (the memory-channel editor, the recording-storage display) doesn't actually disprove
"candidate holds real update-file data for the version screen specifically" — screens here are mutually
exclusive (only one shown at a time), so different screens sharing one scratch-RAM pool for their own
unrelated purposes is an ordinary memory-conservation pattern, not evidence against any *particular*
screen's own interpretation. Conflating "this RAM is reused elsewhere" with "therefore not update-file data
here" was too big a leap. The user's framing (diagnostic prompt, not auto-push trigger) is the more
sensible explanation for why this comparison exists at all, and is fully consistent with everything found
so far (the only real update path, `firmware_update_main`, requires explicit manual confirmation).

**Tried to find the actual populate-candidate function, came up empty this pass**: checked one outlier SD-
file-open caller (`FUN_2003ba80`, at `0x2003ba84` — one of the 20 callers of the SD-open helper
`FUN_200bc5f4` outside `firmware_update_main`'s own neighborhood) — dead end, just a trivial wrapper
unrelated to version data. A function-name search for update/check-version patterns turned up nothing new
beyond already-known functions (`chunk_needs_update`, `firmware_update_main`, `md5_update`). Not pursued
further this session given the effort already spent without traction — a good next-session target, or
settled quickly by watching real `SCIF3`/SD-card behavior on hardware once JTAG/live access exists.

### Pinpointing exactly what the mismatch scenario displays — icons rendered directly (2026-09-07)

Asked directly to pin down what's actually shown, not just "some widgets get drawn." The text side was
already solid (title, an unconfirmed `"NN/NN"` counter, 5 labeled live version strings). The 3 tail-row
widgets (`0x270`/`0x271`/`0x272`) use `FUN_200ac2cc`/`FUN_200addac` — traced these down to
`icon_blit_by_id_v1`/`_v2` (already-named from the 2026-08-29 icon-bitmap thread, `notes/bitmaps.md`), i.e.
these draw real **icon graphics**, not text.

Resolving the icon IDs required walking `FUN_200abc20`'s multi-region locale/theme lookup table — computed
this via script rather than by hand. Icon IDs `0xea`/`0xeb`/`0x15d`/`0x11d` all land in the same lookup
region (`DAT_200ac384`-family arrays), resolving to icon-table indices 233/234/346/284 respectively
(verified identical whether `param_1` is 0 or 1 — the theme-variant split happens to alias to the same
bitmap for these specific icons, so the uncertain runtime value of that selector doesn't matter here).

**Extracted and rendered the actual bitmaps** with the existing `tools/extract_icons.py` (all-white glyphs
on a transparent alpha channel — composited over a gray background to see them, saved to
`notes/assets/`):
- Icon `0xea` (widget `0x270`, **always drawn**): an **upward-pointing triangle** ▲, 16×15px
  ([notes/assets/version-screen-icon-up-triangle.png](assets/version-screen-icon-up-triangle.png)).
- Icon `0xeb` (widget `0x271`, conditionally drawn): a **downward-pointing triangle** ▼, 16×15px
  ([notes/assets/version-screen-icon-down-triangle.png](assets/version-screen-icon-down-triangle.png)).
- Icon `0x15d` (widget `0x272`, conditionally drawn): a **curved return/reload arrow** ↩, 30×20px —
  visually a classic "restart/reload" glyph
  ([notes/assets/version-screen-icon-restart-arrow.png](assets/version-screen-icon-restart-arrow.png)).
- Icon `0x11d` (the conditional overlay, drawn when `current+0xb4` matches that row's own number 1/2/3):
  a single-pixel icon, `(0,0,0,128)` RGBA — solid black at 50% alpha. **Not a checkmark** as first assumed
  — a semi-transparent dimming/highlight wash stretched across whatever widget it's blit onto.

**Reading**: this is a genuine three-state indicator — version-ahead (▲) / version-behind (▼) /
restart-needed (↩) — with `current+0xb4` selecting which single row gets the highlight overlay. About as
concrete an answer as static analysis can give without live hardware to see the actual on-screen color/
layout.

### Further attempts to find the candidate-struct writer, all ruled out (same day)

Pushed on the "should happen early in startup" hint with several concrete new checks, all negative:

- **`cold_boot_hw_init`'s full body, read in full**: no SD-card or update-file touch anywhere in it. Real
  side-finding though: confirmed `ui_version_screen_populate_fields`'s DSP-field source (`DAT_20043738`)
  and `dsp_identity_query_record0`'s destination (`DAT_200b1ca0`) are the **same address** (`0x203def00`,
  verified byte-for-byte) — closing a gap an earlier session (2026-08-29) explicitly left as "not fully
  proven" — DSP Program/Data/FPGA really are live-queried over `SCIF5` at cold boot, the same pattern as
  the `SCIF3` front-panel handshake.
- **`system_mode_request_dispatch`'s full body** (runs on every system-mode transition, already
  extensively documented by an earlier session) — no SD-card or candidate-struct touch.
- **`sd_menu_dispatch_task`'s neighboring cases `0xc`/`0xd`** (`FUN_20026130`/`FUN_20026324`) — looked
  promising purely from adjacency (same address range and case-number range as `firmware_update_main`'s own
  case `0xb`), but both are unrelated file-loading logic (decompression/magic-byte checks matching a
  font-or-graphics-asset load, not firmware). One of them calls a getter (`FUN_200a9bcc`) that looked like
  it might resolve to the candidate struct — checked directly, resolves to a completely different, unrelated
  address (`0x20508320`).
- The one earlier "found a write" lead (`0x2003b320`, from the previous pass) was double-checked and
  confirmed a coincidental edge-case overlap from an unrelated large buffer's flag-array write, not a real
  populate function — not worth re-investigating.

**Genuinely still open** — the candidate struct's writer remains unfound after real effort across two
sessions and many different angles (boot sequence, SD-menu task siblings, literal/computed-address
searches). Best remaining options: a targeted look at whatever code runs specifically on SD-card
insertion/mount (not yet identified as its own function), or settling it directly on live hardware.

### Does a version mismatch actually trigger a firmware push? Checked from 3 angles, answer is no (2026-09-07)

User's natural next question: if a mismatch is detected, does the radio go push new firmware to whatever's
out of sync? Checked directly rather than assumed, from three independent angles, all converging on no:

1. **The mismatch branch's own calls**: every function `ui_version_screen_draw_and_compare` calls there
   (`FUN_200ace08`/`FUN_200adba4`/`FUN_200ac6e0`/`FUN_200ac2cc`/`FUN_200addac`, plus string-formatting
   helpers) lives in the UI-widget-drawing (`0x200ac000`-`0x200ae000`) or string-formatting (`0x2017c000`,
   `0x20080000`) ranges. Nothing touches flash, SD-card I/O, or any transport code — pure display.
2. **No shared code or data with the real updater**: `firmware_update_main` (the actual flash-erase/program
   orchestrator, `notes/firmware-update.md`) is reached only via the SD-card menu's explicit case `0xb` — a
   manual user confirmation, not anything automatic. Pulled every reference to both comparison structs
   (`g_screen_display_scratch_buf`, `g_update_candidate_version_struct`): every single one sits in the
   `0x2003xxxx`-`0x2008xxxx` range (other unrelated screens, per the struct-reuse finding above). **None
   land anywhere near `firmware_update_main` (`0x20025ae4`) or its own chunk-decision function** — two
   fully disjoint regions, zero overlap.
3. **`firmware_update_main`'s own per-chunk trigger is a separate, still-unfound mystery**: it decides
   whether to write each of the 3 optional "extra chunks" via `chunk_needs_update(index)` (already named
   from an earlier session), which just reads a 3-byte flag array, `DAT_2002217c`. Went looking for its
   writer: **zero writers found anywhere** — both via Ghidra's own reference tracking and an independent
   whole-image raw-byte scan for the literal address. Whatever sets this flag isn't reachable by any
   address-literal search at all (the same "computed/indexed address" gap class this project keeps
   hitting) — a real, separate open mystery, unconnected to the version-info screen either way.
   `notes/firmware-update.md` had already flagged this exact gap as "plausible, not proven" when first
   found; this session's search makes it more precisely "not found by any means tried yet," not closer to
   confirmed.
4. **Even if triggered, the transport for these 3 chunks isn't `SCIF3` anyway** — re-decompiled
   `chunk_transport_send_data` directly (not just citing the older note) to double-check: it's built on a
   completely different low-level transport, a `PPR0`/`HSK1`-handshake-gated ring buffer using hardware
   register `0xFCFF0305`, nothing resembling the `SCIF3` UART's `0xFE`/`0xFD` frame convention at all.
   Reconfirms, independently this session, that none of these 3 chunks could reach the front panel over
   `SCIF3` regardless of what triggers them.

**Answer: no traced "mismatch → push firmware" pathway exists anywhere in this firmware**, checked at all
three places one could plausibly live (the comparison screen itself, the real updater's entry point, and
the real updater's own per-chunk trigger). The version-info screen is display-only; the real updater is a
separate, manually-triggered mechanism with its own unfound trigger condition; and the front panel
specifically has no outbound firmware-write transport at all — independently reconfirmed this session on
top of the prior session's finding.

### Follow-up, same day: checked a "flash write, reboot, then compare-and-push" two-phase hypothesis too

User's natural next hypothesis: maybe the push isn't triggered by the interactive menu at all, but by a
boot-time reconciliation step after the flash write completes and the radio reboots. Checked directly —
full derivation in `notes/firmware-update.md`'s "Gap 1 resolved" section, short version here:

- The one confirmed push (DSP, via `chunk_transport_send_data`) already contradicts a two-phase model for
  that component — it runs synchronously inside `firmware_update_main` itself, no reboot in between.
- There genuinely is a "did we just reboot right after an update" detection mechanism (the
  `"Fup_AutoEnd_3765"` top-of-RAM marker, `notes/firmware-update.md`) — exactly the infrastructure this
  hypothesis would need. Found its trigger-setter this session (`mode_transition_sequencer_fup_autoend_setter`,
  renamed from `FUN_2005b2a0`) — but its consumer side is a re-confirmed dead end: the two flags it sets on
  a successful marker match have zero readers anywhere else in the whole image.

Same conclusion as the interactive-menu check above: no boot-time compare-and-push mechanism found either,
checked at the specific place it would need to live.
