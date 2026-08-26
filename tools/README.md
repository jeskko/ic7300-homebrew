# `icom_fw` — IC-7300 firmware container unpacker

A clean, tested rewrite of the existing ad-hoc `tunk.py`/`tunk3.py` scripts
(read-only originals at `/data/misc/icom/7300/`). See `/notes/` for the
reverse-engineered format this implements, and `/notes/multi-cpu-images.md`
in particular for why this rewrite exists: the original scripts silently
dropped ~1.46 MB (~37%) of every container.

No third-party dependencies — pure standard library, Python 3.9+.

## Usage

Unpack one firmware file:

```
python3 -m icom_fw.cli /data/misc/icom/7300/7300_142.dat scratch/out-142
```

Writes `body.bin` (main ARM image), `chunk1_font1.ttf`, `chunk2_font2.ttf`,
`chunk3.bin`, `chunk4.bin`, `chunk5_tail.bin`, and `manifest.json`
(offsets/sizes/warnings, plus an accounted-vs-total byte count).

Verify against all 10 known releases (diffs against the existing
`unpacked.dat` references, checks 100% of each container is accounted for):

```
python3 tools/verify_all.py
```

## Layout

```
tools/
  icom_fw/
    lzss.py        LZSS decompressor (see /notes/decompression-lzss.md)
    container.py   container parser (see /notes/container-format.md)
    cli.py          unpack one file -> component files + manifest.json
  verify_all.py     cross-check against all 10 releases + existing refs
```
