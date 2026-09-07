# UI menu / touchscreen system

Started 2026-09-07, prompted by wanting to know what the different on-screen menu buttons actually
trigger. First real look at this subsystem — a genuinely new thread, not a continuation of an existing
one. Two separate, real structures found this session; neither is fully walked yet.

## Living reference: what's confirmed so far

| Finding | Address(es) | Status |
|---|---|---|
| **Factory "FRONT CHECK MODE" screen** — a numbered list of 13 real physical front-panel buttons | list at `0x20197153` ("FRONT CHECK MODE" title) through `~0x201973c0`; selector function `FUN_2003a540` (state values `0x2c`-`0x32` from `FUN_20013070()` pick between ≥7 factory/service screens, of which this is one) | ✅ button names confirmed, screen-select mechanism traced; the actual GPIO/key-matrix scan code itself not yet chased |
| **General menu-item definition table** — real touchscreen menu-item records, 0x48 (72) bytes each, confirmed spanning at least the `QUICK MENU` and `MEMORY MENU` screens | table starts ~`0x2018f118`, confirmed items through at least `0x2018f4c4`+ (not yet bounded on either end) | 🟡 record layout mostly decoded, one real per-item handler traced to a checkbox/enabled-state query, not yet to "what pressing it actually does" |

## The "FRONT CHECK MODE" factory button list

A raw string at `0x20197153` reads `"FRONT CHECK MODE"`, immediately preceded by a clean numbered list
of real physical front-panel button names (read directly from memory, `0x20196f00`+):

```
1. TRANSMIT      2. TUNER         3. VOX/BK-IN     4. PBT-CLR
5. P.AMP/ATT     6. NOTCH         7. NB            8. NR
9. MENU          10. FUNCTION     11. M.SCOPE      12. QUICK
13. EXIT
```

This is a factory/service self-test screen (walks the operator through pressing each physical button in
turn, presumably checking the key matrix isn't stuck) — confirms button #9 is the physical `MENU` key
and #12 is `QUICK` (the dedicated Quick Menu key), among others. `FUN_2003a540` is the screen-selector:
it reads a state value from `FUN_20013070()` (values seen: `0x2c`-`0x32`, i.e. 7 consecutive
states/screens) and switches between several factory-mode screens, of which "FRONT CHECK MODE"
(state `0x31`) is one — the other 6 states are other factory screens, not yet identified. The actual
key-matrix/GPIO scan logic this screen must drive isn't traced yet — this session only found the
screen-selection and title-string wiring.

## The general menu-item table

Found via a raw string search for `"QUICK MENU"` / `"MEMORY MENU"` (11 and 1 references respectively,
all in the `0x2018f1xx`-`0x2018f5xx` range) — the touchscreen menu-item *definition* table, distinct
from both the 216-item CI-V/EEPROM value-format table (`notes/diode-matrix.md`'s "Found the menu-item
table" section, `DAT_2000e230`/`0x2018a698`) and the menu-name string *label pool*
(`~0x2035a000`-`0x2035f000`, English+Japanese, already found but never connected to anything — see
`notes/diode-matrix.md`'s open question 6). This is a third structure: real per-screen-button records
that a UI-rendering/dispatch layer reads to draw and handle each menu tile.

**Record layout** (72 bytes, offsets relative to record start — decoded from 4 consecutive "QUICK MENU"
item records at `0x2018f160`, `0x2018f1a8`, `0x2018f1f0`, `0x2018f238`; not yet decoded against Ghidra
structure/data-type tools, this is a hand-derived layout from raw hex):

| Offset | Content | Notes |
|---|---|---|
| `+0x00` | `00000101` | flags/type word, constant across the items checked |
| `+0x04` | ptr to `"QUICK MENU"` (`0x20359c0c`) | parent-screen name; a later cluster (~`0x2018f430`+) instead points to `"MEMORY MENU"` (`0x20359c18`), confirming the table covers multiple screens, not just Quick Menu |
| `+0x08` | ptr, constant `0x20359e5c` across the 3 items checked | resolves to `"VOICE TX RECORD"` — a real Quick Menu feature name, but identical across sibling items, so probably *not* each item's own name (more likely a shared subtitle/group label) — not fully understood yet |
| `+0x0c` | ptr, constant `0x20359c4c` across the same 3 items | **not actually text** — read back as non-ASCII binary bytes, so despite sitting in the same string-pool region this is more likely a small icon/glyph-index reference than a name string; corrected after actually reading it rather than assuming from the pattern |
| `+0x10` | same as `+0x08` | |
| `+0x18` | small pointer incrementing by ~3 bytes per item (`0x2018f054`, `057`, `05a`, `05c`, …) into a short byte region just before the table | plausibly a 1-3-char per-item tag/icon-key, not yet decoded |
| `+0x1c` | `00000200` | constant |
| `+0x28` | `0x2004f610` | shared function pointer across items (render/hit-test callback?), not yet decompiled |
| `+0x2c` | `0x2004f688` | shared function pointer, not yet decompiled |
| `+0x30` | **distinct per item**, stride exactly `0xc` (12 bytes): `0x2004f748`, `754`, `760`, `76c`, … | the interesting field — see below |
| `+0x3c` | `0x203dca41`, shared | possibly icon/style struct, not yet decompiled |

The table's first record per screen (e.g. `0x2018f118` before the "QUICK MENU" items start) has a
different shape (no name-string fields, a distinct function pointer `0x2004f5a0`) — almost certainly
the screen-*container* record (the "QUICK MENU" tile/screen itself), with the following records being
its individual items. Not confirmed against a second screen's container record yet.

**The `+0x30` per-item function pointers are 4-byte real thunks, not full handlers**: each is exactly
`adds r0,rN,#0x7` (Thumb, 2 bytes, `rN` decrementing per item: r6,r5,r4,r3) followed by `b <target>`
(Thumb, 2 bytes), where `<target>` also increments by `0xc` per item (`0x2004f28c`, `298`, `2a4`, `2b0`).
The 8 bytes after each 4-byte thunk are currently undefined in Ghidra (not yet checked whether that's
real unreached code or genuine padding). All four targets decompile to the *same* enclosing function,
`FUN_2004f250` — so the differing target offsets are multiple internal entry points into one shared
routine, not four separate functions. Decompiled, `FUN_2004f250` reads an item's table entry (indexed
via `DAT_2004f720`/`DAT_2004f728`, structures of the same `0x48`-byte stride as this table) and returns
bit 0 of a byte at entry `+0x1c` — this reads like an **enabled/checked-state query** (e.g. whether a
toggle-style Quick Menu item is currently on), not the actual "what happens when you press this"
action dispatch. The real per-item *action* handler, if distinct from this state query, hasn't been
found yet.

## Open questions / next steps
1. What are `+0x08`/`+0x0c`'s actual strings for (read `0x20359c4c`), and what does the `+0x18`
   incrementing short-label region (`0x2018f054`+) actually hold — likely the key to getting each
   item's own real display name rather than just its parent screen's name.
2. Find the real "on press, do X" action dispatcher — `FUN_2004f250` found so far looks like a
   state/checkbox query, not an action trigger; its siblings (the shared `+0x28`/`+0x2c` function
   pointers, not yet decompiled) are the next things to check.
3. Bound the whole table: how many screens does it cover in total, and does it start/end where a
   header/count record would be expected? Only confirmed spanning `QUICK MENU` and `MEMORY MENU` so
   far, in a small address window; likely extends much further (this address range sits inside the
   `0x2018e000`-`0x2035d000` span the ARM/Thumb tooling work identified as mostly icon/table *data*,
   not code — consistent with this being exactly that kind of table).
4. Whether this table is the "walker" that was missing to connect `notes/diode-matrix.md`'s 216-item
   CI-V/EEPROM value table to the separate menu-name string pool (open question 6 there) — not
   confirmed; this table's string pointers go to a *different* string pool address range
   (`~0x20359xxx`-`0x2035dxxx`) than the one previously found (`~0x2035a000`-`0x2035f000`), which may
   actually be the same pool (ranges overlap) — worth checking directly next session.
5. The "FRONT CHECK MODE" factory screen's other 6 sibling states (`0x2c`-`0x30`, `0x32`) are other
   factory/service screens, not yet identified at all.
