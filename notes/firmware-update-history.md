# Firmware update mechanism — session history

Full session-by-session narrative and evidence trail behind [notes/firmware-update.md](firmware-update.md),
which carries only the current-state summary (confirmed facts, the update-flow mechanics, and the
active repack-and-boot procedure). Sections below are archived verbatim, in their original order.

## Archived from firmware-update.md on 2026-09-24

## Checked a real lead for the update-progress dialog renderer — not it, but real infrastructure found (2026-09-08) — SUPERSEDED, see section above

The section below was this session's *first* pass at `FUN_200aa750`, which reached the wrong conclusion.
Kept for the record per this project's correction convention — read the section above for the real answer.

## Security implication (superseded by the confirmed section above — kept for history)

**No code-execution-from-RAM-without-flashing path exists in what's been
traced.** `FUN_20024db8` always stages each chunk through a RAM buffer
only as a step before `erase`+`program`ing it into flash — nothing in the
path executes that buffer directly. Every successful update is a
permanent flash write; there's no "run once from SD card, no trace left"
shortcut via this mechanism.

**What *is* notable: no signature verification was found anywhere in
`FUN_20025ae4`'s orchestration.** The only validation against the update
file's content is structural/size sanity checks plus a self-contained
checksum (`FUN_2003c860`/`FUN_2003d38c`/`FUN_2003d458`, compared via
`memcmp`-equivalent `FUN_2017c81e` against a 16-byte expected value read
from the file itself) — no RSA/ECDSA/HMAC-shaped call sequence turned up.
**Not yet confirmed**: haven't decompiled the checksum algorithm itself,
so whether it's a simple unkeyed hash (forgeable by anyone, since the
attacker controls both the payload and can compute the matching checksum
for it) or something stronger is still open. If it turns out to be
unkeyed, the practical implication is that custom firmware satisfying the
container structure and its own checksum would plausibly flash and boot
as legitimate — a real, persistent capability, just not the RAM-only one
originally guessed.

**Sharper version of the implication: `base.dat` itself is not
protected.** Step 3 of `FUN_20025ae4` writes flash offset `0`–`0x10000`
(`base.dat`, the boot loader/unpacker) through the *exact same*
unauthenticated path as everything else — read, checksum, `memcmp`
against current flash, erase+program if different. `base.dat` is the
first code the CPU executes on reset (boot-mode-3 XIP straight into its
vector table, see [[base-loader]]); nothing in Icom's own firmware sits
above it to validate it, since its own marker check only runs *after*
it's already executing. So if the checksum turns out to be unkeyed: a
malicious update replaces the boot loader itself, unconditionally, and
that replacement has first-instruction control on every subsequent boot —
including the ability to fake or remove all update-time checks going
forward.

**Not yet checked, and the one thing that could still stop this:**
whether the RZ/A1H SoC's own on-chip, immutable boot ROM (a *silicon*
feature, not anything in Icom's firmware or in any of our `.dat`
containers) performs a signature check before jumping into SPI flash for
boot-mode-3. That would be documented in the Renesas hardware manual
(`docs/REN_r01uh0403ej0600...`), not discoverable from
anything reverse-engineered so far. **Next steps if this is worth
pursuing, in order: (1) check the Renesas manual for boot-mode-3 secure
boot / signature behavior — settles whether this matters at all; (2) if
no hardware-level check exists, decompile `FUN_2003d38c`/`FUN_2003d458` to
confirm the checksum algorithm is actually unkeyed.**

## Full dissection, start to restart (27th session)

Picked the whole thing back up end-to-end: who calls `firmware_update_main`, what happens after it returns,
and — the one piece never traced — how the radio actually restarts afterward.

**Entry point, confirmed**: `firmware_update_main` is invoked as case `0xb` of `sd_menu_dispatch_task`
(`0x20027528`, see [[kernel-rtos]]'s task catalog) — an ordinary command in that task's ~42-case SD-card
menu dispatcher, alongside format/save-settings/load-settings/memory-keyer operations. Nothing special
gates entry to it; it's reached the same way any other SD-menu action is.

**The "3 extra chunks" decision step — the per-chunk gate finally identified.** `chunk_needs_update(index)`
(renamed from `FUN_20021ff0`, `index` 0-2) is a trivial 3-byte lookup into `DAT_2002217c` — this is exactly
the per-chunk "does this component actually need updating" flag `firmware_update_main` consults twice (once
to compute a progress estimate, once to decide which of the 3 chunks to actually read/write/push).

**Correction, 2026-09-07 — the `FUN_200a94c8` link is retracted, not just unconfirmed.** That function
(renamed `ui_version_screen_draw_and_compare`, see `notes/front-panel-firmware.md`) turned out to be the
version-info *menu screen*'s own display/redraw-optimization logic, not an update-decision function — its
comparison structs share zero code or data with `firmware_update_main`/`chunk_needs_update` (checked
directly, every reference to both sides lands in a disjoint address range). **`DAT_2002217c`'s real writer
is still genuinely unfound** — checked again this session via an independent whole-image raw-byte scan for
the literal address on top of the original `references_to` check, still zero hits. This is a real, deeper
open gap on its own, unconnected to the version-info screen either way — worth chasing directly (maybe a
computed/indexed write, per this project's recurring pattern) rather than assuming any particular source.

**Real hardware handshake found on the "3 extra chunks" transport — corrects an earlier negative
result.** Read the literal-pool constants `FUN_20025044` (the per-chunk ring-buffer producer) and
`FUN_20025288` (see below) actually use, rather than trusting the earlier "not a literal hardware peripheral
base address" read of the `0xe2000000`-tagged value:
- `DAT_20025610` = `0xFCFF0305` — a real hardware register, used with a bit-test/wait helper
  (`FUN_20360b24`) — the wait loop for a transfer-ready condition.
- `DAT_20025614` = a **RAM** flag byte (not a register) — `FUN_20025044` writes `'P'` (`0x50`) here per
  chunk; `FUN_20025288` (below) writes `0x87` — different command/tag values over what looks like the same
  low-level transport.
- `DAT_20025618` = **`0xFCFE3200` = `PPR0`**, the RZ/A1H's **Port pin read register** block base (§54.3 of
  the hardware manual — reads the *actual electrical level* of a pin, not the software output latch).
  Both functions test `*(ushort*)(DAT_20025618+0x20)` bit `0x200` — offset `+0x20` from `PPR0` is **`PPR8`**
  (Port 8's pin-read register), and bit `0x200` = bit 9 = **`P8_9`, i.e. `HSK1`** — one of the exact
  DSP-adjacent pins from [[ic7300-signal-chain]]'s pin-mapping table.

**This corrects [[ic7300-signal-chain]]'s "no reference found anywhere" verdict for `HSK1`/`HSK0`/`FRWT`/
`RTD`** — that search only covered the *port data register* (`Pn`, base `0xFCFE3000`) literal family; this
session's finding uses the **pin *read* register** (`PPRn`, base `0xFCFE3200`) instead, a completely
different literal the earlier search never checked. `HSK1` (`P8_9`) does have a real, live CPU-side
reference after all — read (not written) as a hardware ready/handshake signal by exactly the two functions
that move the "3 extra chunks" payload and their post-update trigger. Worth re-checking `HSK0`/`FRWT`/`RTD`
against the same `PPR8`/`PPRn` literal before re-asserting "no reference" for those too.

**`FUN_20025288` — the post-update trigger, called only when component0 or DSP Data changed.**
`firmware_update_main` calls it exactly when `local_a0[0] != 0 || local_a0[2] != 0` — i.e. when the
component0 chunk (index 0) or the **DSP Data** chunk (index 2) was actually written (not when only DSP
Program, index 1, changed — a real, specific asymmetry, not chased further). Its body is shaped exactly
like `FUN_20025044`'s own low-level transfer primitive (same `DAT_20025610`/`14`/`18` triple, same
`HSK1`-wait), but sends command byte `0x87` instead of `0x50`/`'P'` — the natural reading is "tell whatever
is on the other end of this link to reload/reset now that its new firmware has arrived," though this is
inference from shape, not a decoded protocol spec.

**Correction: index 0 is not confirmed to be "Front CPU."** This section originally labeled index-0/
component0 as the "Front CPU" chunk, following the `FUN_200a94c8` field-order hypothesis above. Later work
in [[multi-cpu-images]] and [[container-format]] disassembled the actual extracted component0
(`front_cpu.bin`) and found it to be genuine TMS320C674x (DSP) object code, not RL78 front-panel code —
"component0 = Front CPU firmware" is explicitly retracted there. Its real relationship to component1 (DSP
Program) is still unresolved. So the trigger condition above is better read as "component0 or DSP Data
changed," with component0's own identity still open — not confirmed to be the front panel at all.

**The system restart mechanism — a real, unambiguous watchdog-forced reset, but not yet tied to
firmware-update completion specifically.** Found `FUN_20052bd0`: after a graceful-shutdown sequence
(checks power/PTT/SWR-safe conditions, clears some Port 6 bits via `PSR6`), it writes the RZ/A1H's real
watchdog registers in exactly the sequence the hardware manual documents for arming it (§12.3.2/12.3.3):
```c
disableIRQinterrupts();
DAT_20053124[2] = 0x5a5f;   // WRCSR (0xFCFE0004), 0x5A unlock prefix + data 0x5F (enables reset-on-overflow)
DAT_20053124[1] = 0x5afe;   // a second WRCSR-area write, same unlock convention
*DAT_20053124   = 0xa57f;   // WTCSR (0xFCFE0000), 0xA5 unlock prefix + data 0x7F (counter near overflow)
enableIRQinterrupts();
do {} while (true);          // spin forever -- the watchdog fires and hard-resets the SoC from here
```
`DAT_20053124` reads back as `0xFCFE0000` exactly — confirmed, not inferred. This is **the** system restart
primitive in this firmware: an intentional watchdog-timeout self-reset, not a dedicated software-reset
register (the RZ/A1H, being Cortex-A9-based, has no Cortex-M-style `AIRCR.SYSRESETREQ`; this is the
standard Renesas SH/RZ idiom for the same purpose).

`FUN_20052bd0` has exactly one caller: `FUN_20052e30`, the main "normal running state" loop (reached as the
default case of the top-level power-state dispatcher `FUN_2002b1c8` — i.e. this is what runs continuously
whenever the radio isn't in some special boot/update/shutdown mode). Inside that loop, the call is gated:
```c
if ((*(ushort *)(DAT_2005314c + 4) & 0x40) == 0) {   // DAT_2005314c = 0xFCFE3200 (PPR0) again;
    FUN_20052bd0();                                    // +4 = PPR1, bit 0x40 = bit 6 = P1_6
    ...
}
```
So the trigger condition is **a live read of pin `P1_6`'s actual level** (via `PPR1`), checked every
iteration of the main loop — when that bit reads `0`, the radio watchdog-resets itself.

**Resolved (user identified the schematic net, same session): `P1_6` is `PDV`, wired to the `VOUT` pin of
`IC361`, a New Japan Radio `NJU7704F3`** — a CMOS supply-voltage detector IC (the "F3" suffix sets its
fixed threshold voltage). Its output goes low when the monitored supply rail drops below that threshold.
**This reframes the whole mechanism: `main_idle_loop`'s watchdog-reset call is a power-fail/brownout safety
trip, not a firmware-update-completion trigger at all.** The firmware is continuously watching a real
external voltage supervisor and forcing an immediate, clean watchdog reset the instant supply voltage sags
below `IC361`'s threshold — a sensible, deliberate design (better to reset cleanly on a browning-out rail
than run into undefined behavior from a marginal supply) — and has nothing to do with `firmware_update_main`
specifically. This also means the earlier "two readings" framing was answering the wrong question: `P1_6`
isn't a semi-generic "restart requested" condition serving multiple callers including the updater — it's
a dedicated hardware safety input, full stop.

**Consequently, how the radio actually restarts after a successful firmware update is still genuinely
open** — this session's find rules out `main_idle_loop`/`watchdog_force_reset`/`P1_6` as the mechanism
rather than confirming it. Worth considering, not yet checked: the update-completion trigger
(`chunk_transport_send_reload_cmd`, sent only when component0 or DSP Data changed — see the "index 0 is not
confirmed to be 'Front CPU'" correction above) reloads whatever component0 turns out to be and/or the
*DSP*, not necessarily the main CPU — the user-facing "...restart. NEVER turn OFF..." warning could
plausibly describe a **front-panel/DSP-side reboot cycle** (visible as the display going blank and the
frequency screen reappearing) rather than a full main-CPU watchdog reset at all. If a main-CPU reset does
also happen, its trigger is a genuinely different, not-yet-found call — **concrete next step: look for
where the front-panel reload actually happens (a real SCIF3 command, given the confirmed UART driver in
this file's front-panel section) and treat that, not `P1_6`, as the lead for "what actually restarts after
an update."**

## A real, strong candidate found: the `"Fup_AutoEnd_3765"` top-of-RAM marker (2026-08-29, new session)

Found by extending Ghidra's memory map to cover the RZ/A1H's full, documented 10 MB on-chip RAM range
(`0x20000000`-`0x209fffff`, see [[memory-map]]) — previously only `body.bin`'s own ~3.7 MB static image was
mapped, leaving everything above it (including this marker, which sits in the last 16 bytes of the entire
range) invisible to Ghidra. User specifically asked about xrefs to the top-of-RAM addresses after doing
this; tracing them led directly here.

**The mechanism, fully traced on both the write and read side**:
- **`fup_autoend_marker_write`** (`0x2005ddb4`, renamed from `FUN_2005ddb4`): if `g_fup_autoend_trigger_flag`
  (RAM byte `0x20390307`) `== 2`, writes the literal 16-byte string `"Fup_AutoEnd_3765"` — "Fup" matching
  this codebase's own `firmware_update_main` naming, "3765" being the already-established internal model
  codename (the "SX3765" mystery resolved in an early session) — to `g_fup_autoend_marker_ram` (`0x209ffff0`,
  literally the last 16 bytes of the whole 10 MB on-chip RAM window). Otherwise it clears those same 16
  bytes to zero.
- **This is called unconditionally from `FUN_20029ca4`** (the power-state main loop, reached via
  `sys_monitor_task_loop → FUN_2002b29c → FUN_20029ca4`, `FUN_2002b29c` already established in the 20th
  session as running on *every* `sys_monitor_task_loop` iteration) — right before that same function arms
  the exact same watchdog-forced-reset register sequence already confirmed above as this firmware's restart
  primitive: identical `0xFCFE0000`-based writes, identical values `0x5a5f`/`0x5afe`/`0xa57f`. **This is a
  second, independent path into the same watchdog-reset mechanism**, not the `P1_6`-gated one in
  `main_idle_loop` — the marker gets written right before this path resets the system.
- **`fup_autoend_marker_check_and_clear`** (`0x2005ddfc`, renamed from `FUN_2005ddfc`) is the read/consume
  side: called from `FUN_2002b29c` itself (near its start, i.e. on every boot and every normal running
  cycle) with a wake/mode-source byte. If bit `0x80` of that byte is set **and** the marker RAM currently
  matches `"Fup_AutoEnd_3765"` exactly, it sets two flags (`DAT_2005dfdc`/`DAT_2005dfe0`) to `1`. Either way,
  it **always clears the marker RAM back to zero afterward** — a textbook one-shot "detect once, then rearm"
  idiom.

**Working hypothesis, not yet fully confirmed**: this is the "how does the radio know to do something
special right after a firmware-update reboot" mechanism this thread has been looking for since the
27th session. The shape fits well: on-chip RAM plausibly survives a watchdog-triggered warm reset (unlike a
full power cycle), so a marker written just before triggering that exact reset would still be readable on
the very next boot — precisely what `fup_autoend_marker_check_and_clear` looks for.

**Two concrete gaps left, both good next steps**:
1. **Who sets `g_fup_autoend_trigger_flag` (`0x20390307`) to `2`?** ~~Not yet traced~~ **Found, 2026-09-07**
   (see below) — not `firmware_update_main` itself, a separate mode-transition state machine.
2. **Who reads `DAT_2005dfdc`/`DAT_2005dfe0` after a successful marker match?** No reader found anywhere in
   `body.bin`'s static call graph — consistent with this project's established pattern of generic/indirect
   (message- or task-based) consumption defeating direct xref tracing (see [[kernel-rtos]]'s task-activation
   dead ends). Whatever shows an "update complete" state (or similar) almost certainly reads one of these
   flags several layers removed from any single traceable literal reference. **Reconfirmed independently,
   2026-09-07**: still genuinely zero readers anywhere.

## Gap 1 resolved, and checked against a "reboot then push" hypothesis (2026-09-07)

User's hypothesis: maybe the flash write and the component-firmware push are two separate phases — write
flash, reboot, then compare versions at boot and push updates to whatever's stale (DSP/FPGA/front panel).
Checked directly, at the two places such a mechanism would have to live:

**The one confirmed component push (DSP, `chunk_transport_send_data`) already rules this out for that
component.** It runs *synchronously inside `firmware_update_main` itself* — read chunk from SD file, verify
against current flash content/MD5, write flash block **and** push to DSP over the parallel handshake bus,
all in one pass, no reboot in between. Not a deferred boot-time step.

**Traced who sets `g_fup_autoend_trigger_flag` to `2`** (the exact gap flagged above) — found a real writer:
a small 11-state boot/mode-transition sequencer, `FUN_2005b2a0` (dispatched via an 11-entry function-pointer
table, `0x2019b7d8`-`0x2019b800`, one slot per state `0x33`-`0x3d`). Its state `0x3c` sets the trigger flag
to `2` when a separate flag 2 bytes further into the same small cluster (`0x20390309`, read via
`DAT_2005b970`) is nonzero, or to `1` otherwise. That secondary flag itself gets reset to `1` unconditionally
by `FUN_2005dd2c` (called from `FUN_2002b29c`, the already-established power-state main-loop dispatcher) —
i.e. it looks like a normal per-boot reset, with `FUN_2005b2a0`'s own state machine being what actually
changes it away from that default under specific conditions not yet fully pinned down. This function's
overall shape (calls into `operating_mode_change_dispatch`, calls a `FUN_2000a17c(2)`-style cleanup on error
paths) reads as a mode-transition/power-down sequencer, not obviously "did an update just run" on its own —
the "Fup_AutoEnd" naming strongly implies *some* tie to firmware updates, but the exact condition wasn't
fully pinned down this session.

**Re-confirmed the consumer side is still a dead end** (independently, not just re-citing the old note):
`DAT_2005dfdc`/`DAT_2005dfe0` are read *only* from within `fup_autoend_marker_check_and_clear` itself —
zero readers anywhere else in the whole image.

**Answer to the "reboot then push" hypothesis**: no such mechanism was found at either place it would need
to live. The one push found happens synchronously, not after a reboot; and the post-update-reboot marker
this hypothesis would need is a real, traced mechanism up through "trigger gets set," but its *consumption*
side goes nowhere — whatever this marker was meant to drive next isn't reachable by any tracing done so far.
Regardless of trigger mechanism, the front panel specifically still has no outbound chunked-write transport
at all (`notes/front-panel-firmware.md`), so this doesn't change that conclusion either way.

**Methodological note, worth remembering for future sessions**: extending Ghidra's memory map to the chip's
real, datasheet-confirmed RAM extent (rather than just what's covered by a static image dump) is not purely
cosmetic — this specific, substantial finding was invisible until that was done, simply because the address
in question happened to sit outside the previously-mapped range. Cross-reference discovery for *code that
computes/uses* an address doesn't strictly require the destination to be mapped, but in practice this
turned out to be exactly where an embedded firmware convention (a persistent marker at a fixed edge of RAM)
would live — worth checking the memory map's edges specifically, not just extending coverage generically.

**Side finding, same session — the JTAG-disable question**: searched both `body.bin` and `base.dat` for any
reference to the RZ/A1H's CPU debug-enable control register (`ICEREGJTTRCSEL`, `0xFC00F004` — holds
`DBGEN_CPU0`/`NIDEN_CPU0`, per the hardware manual §56.4.2) — **zero references in either image**. Neither
firmware component ever programmatically touches CPU debug enable/disable. If JTAG access is restricted on
production units, it isn't done by this SoC's own debug-enable register from software — would have to be an
OTP/fuse setting, a board-level strap, or some other security mechanism not covered by this specific
register (the manual also mentions `ICDISRn` "security status bits" and other debug-security registers not
yet checked). Not chased further, but a clean, real negative result worth having on record.
