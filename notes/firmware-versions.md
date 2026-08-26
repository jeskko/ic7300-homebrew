# Firmware releases — header field survey

Parsed directly from all 10 read-only originals in `/data/misc/icom/7300/`
(`7300_1XX.dat`) using the layout in [[container-format]]. This supersedes
`foo.txt`'s hand-collected hex diffs (which mixed in `base.hex` dumps) —
figures below are computed straight from the actual files.

| ver | file size | version string | size1 | size2 | size3 | size4 | size5 | size6 | size7 | `length` (main body, decompressed) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1.11 | 3,946,563 | `3wfU3.092.003.13` | 2436080 | 97307 | 163592 | 721836 | 720648 | 691230 | 847169 | 3738328 |
| 1.12 | 3,960,355 | `3wfU3.102.003.14` | 2436080 | 97670 | 163592 | 721836 | 720648 | 704659 | 853193 | 3738328 |
| 1.13 | 3,955,508 | `3wfU3.112.003.15` | 2436080 | 97862 | 163592 | 721836 | 720648 | 699620 | 852941 | 3738328 |
| 1.14 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.20 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.21 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.30 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.40 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.41 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.42 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738392 |

## Patterns

- `size1`, `size3`, `size4`, `size5` are **byte-for-byte constant across
  every release** (`2436080`, `163592`, `721836`, `720648`) — these look
  like fixed structural constants (slot capacities or similar), not
  per-build data sizes. Not yet matched to anything else in the container
  (checked against the real chunk1/chunk2/chunk3 sizes below — no match).
- `size2`, `size6`, `size7`, and the **version string itself** all changed
  release-to-release only through **v1.11 → v1.14**, then **froze solid**
  for every release from v1.14 through v1.42 (the last official release).
  `length` (main body decompressed size), by contrast, keeps changing on
  nearly every release (+12, +48, +4 bytes across 1.14/1.20/1.30/1.40/1.42).
  This is the strongest evidence so far for [[multi-cpu-images]]: it looks
  like Icom stopped updating a secondary component (whatever `size2`,
  `size6`, `size7`, and the version string track) after v1.14, while
  continuing to update the main RZ/A1H application (`length`) every
  release through v1.42.

## Real chunk sizes vs. fixed slot offsets

Actual chunk1 (font)/chunk2 (font)/chunk3 sizes, read from their
size-prefix fields (`container[0x21002c]`, `container[0x24002c]`,
`container[0x25002c]`):

| ver | chunk1 (font1) | chunk2 (font2) | chunk3 |
|---|---|---|---|
| 1.11–1.14 | 174528 | 25636 | 11244 |
| 1.20–1.30 | 175656 | 25636 | 11244 |
| 1.40–1.42 | 176700 | 25636 | 11244 |

None of these match `size1..size7` either. But the **offsets themselves are
fixed, oversized slots**, confirmed by construction:
- `0x21002c − 0x10030 = 0x1FFFFC` (≈2 MiB) — the main-body slot is a fixed
  ~2 MiB budget, regardless of the actual compressed size.
- `0x24002c − 0x21002c = 0x30000` (192 KiB) — chunk1's fixed slot, vs. its
  actual ~170 KiB content.
- `0x25002c − 0x24002c = 0x10000` (64 KiB) — chunk2's fixed slot, vs. its
  actual ~25 KiB content.
- chunk3 and chunk4 (the trailing LZSS-compressed 0x10000-byte block), by
  contrast, are **tightly packed with no gap** — chunk4 starts immediately
  after chunk3's actual data, no fixed-offset seek involved (see
  `tunk3.py`'s code — no `seek()` call between reading chunk3 and starting
  the chunk4 `unpack()`).

**Conclusion**: this is a fixed-slot container format for the first four
regions (body/chunk1/chunk2/chunk3-start), each over-provisioned to a round
power-of-two-ish size, followed by one tightly-packed compressed tail
(chunk4). The rewritten unpacker in `tools/` should assert this shape
(fixed-offset reads for slots 1–4, immediately-following read for chunk4)
rather than silently trust it — see [[container-format]].
