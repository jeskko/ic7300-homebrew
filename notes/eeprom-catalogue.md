# EEPROM parameter catalogue (living document)

Started per user request while chasing D419 (see [[diode-matrix]]) — in
case some diode bits are read from the EEPROM-persisted value rather
than the live RAM scan result we've been tracing. Also useful as a
standalone reference: the generic EEPROM get/set API
(`FUN_2001e510`=get, `FUN_2001e484`=set, both `(param_id, dest, length)`)
has **73 total call sites** to the getter alone — this catalogue tracks
what we've identified from sampling them, not an exhaustive list.

**Methodology**: check the instructions immediately before each `bl
0x2001e510`/`0x2001e484` for the literal parameter ID (`r0`) — usually a
`mov`/`movw r0,#imm` right before the call. Add entries here as more get
identified. Numeric IDs below are hex unless stated otherwise.

## Confirmed parameter IDs

| ID | Length | Purpose | Source |
|---|---|---|---|
| `0x3e44` | 4 bytes | Diode-matrix scan result (see [[diode-matrix]]) — confirmed read/written from **at least 2 separate places**: `sync_diode_matrix_to_eeprom` (read-compare-write-if-changed) and `FUN_2006cb84` (read-only, as part of loading a combined settings struct) | Directly traced |
| `0x3e40` | 1 byte | A *separate* small selector (0-7 range, resets to 0 if ≥8) — indexes an 8-entry×8-byte table (`DAT_2006cc38`→`0x2019e7d0`) to pick yet another parameter ID+length pair to load. Purpose of the selector itself not yet traced — worth checking if this is actually the *real* "which region/variant" driver rather than (or in addition to) the diode-derived region code | `FUN_2006cb84` |
| `0x3e80` (`16000` decimal) | 16 bytes | A format/version signature — compared against up to 4 known signatures in a table (`FUN_20024738`) to select which settings-parser function to call (`FUN_2006cb84` is one of them). Read first in `FUN_2006cb84` too | `FUN_20024738`, `FUN_2006cb84` |
| `0x40` | 0x129c (4764) bytes | Large settings block, purpose not traced | `FUN_2006cb84` |
| `0x12e0` | 0x338 (824) bytes | Settings block, purpose not traced | `FUN_2006cb84` |
| `0x1620` | 0x468 (1128) bytes | Settings block, purpose not traced | `FUN_2006cb84` |
| `0x1a78`-`0x1a81` (skips `0x1a7b`) | 1 byte each | A sequential cluster of related single-byte settings, loaded together into consecutive struct offsets — purpose not traced, but the pattern (skip one ID) suggests one slot is reserved/unused | `~0x2000a8c4` area |
| `0x18d`, `0x1bb`, `0x1d6`, `0x1d9` | various | Standalone individual settings seen in one function, purpose not traced | `~0x2001e5d8` area |

## Discovered mechanism: versioned settings format with multi-parser dispatch

`FUN_20024738` reads a 16-byte format signature (ID `0x3e80`) and
compares it against up to 4 stored reference signatures in a table
(`DAT_20024a28`), each paired with a handler function pointer. On match,
calls that handler with the destination struct pointer (`DAT_20024a2c`,
resolves to `0x203b31e0`) as its argument. `FUN_2006cb84` is one such
handler — loads a combined settings struct (~0x1a80 bytes) from several
EEPROM parameters at once, landing the diode value at struct offset
`+0x1a7c` (i.e. `0x203b4c5c` for the currently-active struct instance).

This strongly suggests **the EEPROM layout has been revised across
firmware versions**, with this dispatch mechanism handling
backward-compatible loading of older formats — worth keeping in mind
for any future work touching EEPROM layout assumptions.

**Not yet resolved**: no consumer found reading the struct field at
`+0x1a7c` (or the computed absolute address `0x203b4c5c`) elsewhere —
checked, no references either way (same recurring indirect-access
pattern as most of this codebase). The 10 references to the struct base
pointer (`DAT_20024a2c`) are all clustered within the loader mechanism
itself (`0x20024528`-`0x200247b8`), not spread to external consumers.
D419 (bit 6) remains unconfirmed via this path too.

## Next steps if continued
- Sample more of the 73 `FUN_2001e510` call sites (only ~15-20 checked
  so far) — particularly the still-unchecked clusters at
  `0x2004c...`/`0x2004d...`, `0x2006d.../0x2006e...`, `0x2007...`.
- Trace `DAT_2006cc38`'s 8-entry table (the `0x3e40`-selector-indexed
  one) — could reveal a *second* region-variant mechanism distinct from
  the diode-derived region code.
- If a diode bit's consumer keeps eluding static tracing, this catalogue
  is exactly the kind of thing worth cross-checking live via JTAG
  (watch EEPROM parameter reads by ID, see what touches `0x3e44` or
  nearby IDs) once hardware access exists.
