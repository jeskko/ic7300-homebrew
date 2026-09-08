#!/usr/bin/env python3
"""
Sweep a raw firmware image for candidate Shift-JIS text, flag the ones with
no plausible adjacent English translation, and persist everything to SQLite
for offline querying.

Why this exists
----------------
notes/diode-matrix-history.md's 11th session found a real, previously-unknown
JP-only feature ("4630 kHz Emergency Communication Mode") by noticing one
Shift-JIS status string (0x2032a014, "非常通信モード") had NO
English counterpart, unlike every sibling row in the same bilingual table
(0x2032a000) -- a real signal for "JP-only hidden feature" in this codebase,
found entirely by hand. This script turns that technique into a repeatable,
whole-image sweep instead of a one-table manual check.

How pairing is judged
----------------------
Ground truth for what "paired" looks like came from hex-dumping the known
table directly (0x2032a000-0x2032a0e0 in the 142 release). It is a flat
sequence of NUL-terminated, space-padded fields, e.g.:

    0x2032a000: <empty/spaces>            (English slot -- blank)
    0x2032a014: "非常通信モード"    (Japanese slot -- the untranslated hit)
    0x2032a036: "4630kHz"                 (language-neutral, stands alone)
    0x2032a058: "TUNER"                   (English slot -- populated)
    0x2032a07a: "チューナー"            (Japanese slot -- real translation of TUNER)
    0x2032a09c: "4630kHz / TUNER"         (pre-combined, English)
    0x2032a0be: "4630kHz / チューナー"     (pre-combined, Japanese)

i.e. a Japanese field's "translation partner" is the immediately-preceding
NUL-delimited field in the same table -- when that field is present and
looks like real English, the JP string is a translation; when it's blank
(all space-padding, no NUL-terminated text at all), there is no English
counterpart. This script re-derives that same preceding-field relationship
generically for every Japanese hit found anywhere in the image, with a
secondary, weaker "any plausible English word within a byte window" check
for hits that don't sit in a clean NUL-delimited table at all (free English
prose, unrelated code bytes, etc. -- the window check exists so a genuine
translation sitting just past an unrelated NUL isn't missed, but it is
explicitly weaker evidence than the same-field check and is labelled as
such in the output).

Candidate extraction
---------------------
A single left-to-right tokenizer walks the whole image. At each position it
greedily consumes a run of:
  - printable ASCII bytes (0x20-0x7E), one character each, or
  - Shift-JIS lead/trail byte pairs (lead 0x81-0x9F or 0xE0-0xFC, trail
    0x40-0x7E or 0x80-0xFC excluding 0x7F) that actually decode under the
    `shift_jis` codec, one character each.
A run of fewer than --min-chars (default 3) real characters is discarded --
this is the project's own "adjust as needed to cut noise" threshold; 3 was
chosen because it is the shortest length that already excludes the vast
majority of single stray printable bytes in code/data while still keeping
real short labels like "RX"/"TX" (2 chars, technically below threshold and
intentionally excluded -- those are common enough as raw byte coincidences
in non-string data that they dominated an earlier --min-chars=2 trial run
with clear noise, not signal).

Every run is kept, but classified:
  - 'jp'      -- contains at least one real Shift-JIS kanji/kana codepoint
                 (Hiragana/Katakana U+3040-30FF, half-width kana U+FF61-FF9F,
                 CJK ideographs U+4E00-9FFF, fullwidth forms U+FF00-FFEF).
                 This is the plausibility filter the task calls for: plenty
                 of byte pairs in icon/glyph/lookup data decode "validly" as
                 Shift-JIS without ever landing on a real character (e.g.
                 box-drawing/private-use ranges) -- those are excluded here,
                 not counted as Japanese candidates.
  - 'ascii'   -- pure printable ASCII, no Shift-JIS pairs consumed.
  - 'sjis_other' -- consumed at least one valid-but-implausible Shift-JIS
                 pair with no real kanji/kana anywhere in the run (kept for
                 audit, not treated as a Japanese-text candidate).

Usage
-----
    python3 tools/sjis_string_scan.py scratch/unpacked/142/body.bin \
        --out scratch/sjis_scan_142.sqlite

    # sanity-check known-good hits actually got found and flagged correctly
    python3 tools/sjis_string_scan.py scratch/unpacked/142/body.bin \
        --out scratch/sjis_scan_142.sqlite --check 0x2032a014 0x2032a07a

    # query afterwards, e.g. every unpaired JP candidate sorted by address
    sqlite3 scratch/sjis_scan_142.sqlite \
        "select printf('0x%x',addr), text, pair_kind from jp_candidates
         where paired=0 order by addr"

No third-party dependencies -- pure standard library (shift_jis is a builtin
codec).
"""
import argparse
import re
import sqlite3
import sys

ASCII_LO, ASCII_HI = 0x20, 0x7E

KANJI_KANA_RANGES = (
    (0x3040, 0x30FF),   # Hiragana + Katakana
    (0xFF61, 0xFF9F),   # half-width katakana
    (0x4E00, 0x9FFF),   # CJK unified ideographs
    (0xFF00, 0xFFEF),   # fullwidth forms
)


def is_kanji_kana(ch):
    o = ord(ch)
    return any(lo <= o <= hi for lo, hi in KANJI_KANA_RANGES)


def build_sjis_table():
    """Precompute every (lead, trail) -> decoded 1-char string or None, so
    the main scan loop is a dict lookup instead of a try/except decode."""
    table = {}
    leads = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))
    trails = [b for b in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)) if b != 0x7F]
    for lead in leads:
        for trail in trails:
            raw = bytes([lead, trail])
            try:
                ch = raw.decode("shift_jis")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                table[(lead, trail)] = ch
    return table


def scan_runs(data, min_chars, sjis_table):
    """Single left-to-right tokenizer pass. Returns a list of dicts:
    {start, end, kind, text}."""
    n = len(data)
    runs = []
    i = 0
    while i < n:
        start = i
        chars = []
        has_kk = False
        has_sjis = False
        while i < n:
            b = data[i]
            if ASCII_LO <= b <= ASCII_HI:
                chars.append(chr(b))
                i += 1
                continue
            if i + 1 < n:
                pair = (b, data[i + 1])
                ch = sjis_table.get(pair)
                if ch is not None:
                    chars.append(ch)
                    has_sjis = True
                    if is_kanji_kana(ch):
                        has_kk = True
                    i += 2
                    continue
            break
        if len(chars) >= min_chars:
            if has_kk:
                kind = "jp"
            elif has_sjis:
                kind = "sjis_other"
            else:
                kind = "ascii"
            runs.append({"start": start, "end": i, "kind": kind, "text": "".join(chars)})
        elif i == start:
            i += 1
        # else: run was too short but already consumed >=1 byte; i is
        # already advanced past it, nothing more to do.
    return runs


_ENGLISH_RE = re.compile(r"[A-Za-z]")


def looks_like_english(s):
    """Plausibility filter for 'is this ASCII text a real translation, not
    padding/noise/a hex-looking blob'."""
    s = s.strip(" \t")
    if len(s) < 3:
        return False
    letters = _ENGLISH_RE.findall(s)
    if len(letters) < 3:
        return False
    if len(letters) / len(s) < 0.55:
        return False
    if len({c.lower() for c in letters}) < 2:
        return False
    return True


def find_preceding_field(data, start, cap=256):
    """Return the *previous table slot's* content, i.e. the text bounded by
    the two nearest NUL bytes before `start` -- NOT the (trivially empty)
    gap between the single nearest NUL and `start` itself, since in these
    back-to-back NUL-delimited tables a run's own text begins immediately
    after its own field's opening NUL, one byte after the *previous*
    field's terminator. Concretely, for the known-good calibration table:
    0x2032a013 is the NUL terminating the English field "EMERGENCY MODE"
    (0x20329ff2-0x2032a012); the Japanese text "非常通信モード" begins
    immediately at 0x2032a014, right after that NUL. A naive single-NUL
    lookback finds an empty gap between 0x2032a013 and 0x2032a014 and
    concludes (wrongly) there's no preceding field. This function instead
    finds *both* nearby NULs and returns the field between them -- which
    correctly recovers "EMERGENCY MODE" as the preceding slot's content.
    Returns (field_bytes, field_start_addr_offset) or (None, None) if fewer
    than two NULs exist within `cap` bytes."""
    lo = max(0, start - cap)
    nul1 = data.rfind(b"\x00", lo, start)
    if nul1 == -1:
        return None, None
    nul0 = data.rfind(b"\x00", lo, nul1)
    if nul0 == -1:
        return None, None
    return data[nul0 + 1:nul1], nul0 + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binfile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20005000)
    ap.add_argument("--min-chars", type=int, default=3)
    ap.add_argument("--window", type=int, default=128,
                     help="byte radius for the weaker 'any English run nearby' fallback check")
    ap.add_argument("--out", default=None, help="write results to this SQLite file")
    ap.add_argument("--check", nargs="*", type=lambda s: int(s, 0), default=[],
                     help="addresses expected to show up as 'jp' candidates -- sanity check, exits nonzero if missing")
    args = ap.parse_args()

    with open(args.binfile, "rb") as f:
        data = f.read()

    sjis_table = build_sjis_table()
    runs = scan_runs(data, args.min_chars, sjis_table)
    for r in runs:
        r["addr"] = r["start"] + args.base

    by_kind = {}
    for r in runs:
        by_kind.setdefault(r["kind"], 0)
        by_kind[r["kind"]] += 1
    print(f"total runs: {len(runs)}  " + "  ".join(f"{k}={v}" for k, v in sorted(by_kind.items())))

    # index of ascii runs by address for the window fallback check
    ascii_runs = sorted((r for r in runs if r["kind"] == "ascii"), key=lambda r: r["addr"])
    ascii_addrs = [r["addr"] for r in ascii_runs]

    import bisect

    jp_results = []
    for r in runs:
        if r["kind"] != "jp":
            continue
        addr = r["addr"]
        preceding, field_start_off = find_preceding_field(data, r["start"])
        preceding_text = None
        paired = False
        pair_kind = None
        pair_addr = None
        pair_text = None
        if preceding is not None:
            stripped = preceding.rstrip(b" ").decode("latin1")
            preceding_text = stripped if stripped else None
            if stripped and looks_like_english(stripped):
                paired = True
                pair_kind = "same_field"
                pair_addr = field_start_off + args.base
                pair_text = stripped
        if not paired:
            # weaker fallback: any plausible-English ascii run within +-window bytes
            lo_i = bisect.bisect_left(ascii_addrs, addr - args.window)
            hi_i = bisect.bisect_right(ascii_addrs, addr + args.window)
            best = None
            for idx in range(lo_i, hi_i):
                cand = ascii_runs[idx]
                if looks_like_english(cand["text"]):
                    dist = abs(cand["addr"] - addr)
                    if best is None or dist < best[0]:
                        best = (dist, cand)
            if best is not None:
                paired = True
                pair_kind = "window"
                pair_addr = best[1]["addr"]
                pair_text = best[1]["text"]

        jp_results.append({
            "addr": addr, "text": r["text"], "paired": paired,
            "pair_kind": pair_kind, "pair_addr": pair_addr, "pair_text": pair_text,
            "preceding_field_text": preceding_text,
        })

    n_jp = len(jp_results)
    n_unpaired = sum(1 for j in jp_results if not j["paired"])
    print(f"jp candidates: {n_jp}   unpaired (no adjacent English found): {n_unpaired}")

    if args.check:
        found_addrs = {j["addr"] for j in jp_results}
        missing = [hex(a) for a in args.check if a not in found_addrs]
        if missing:
            print(f"SANITY CHECK FAILED -- expected jp hits not found: {missing}", file=sys.stderr)
            sys.exit(1)
        else:
            print(f"sanity check OK -- all of {[hex(a) for a in args.check]} found as jp candidates")

    if args.out:
        con = sqlite3.connect(args.out)
        con.executescript("""
        DROP TABLE IF EXISTS runs;
        DROP TABLE IF EXISTS jp_candidates;
        CREATE TABLE runs (
            addr INTEGER PRIMARY KEY,
            end_addr INTEGER,
            kind TEXT,
            text TEXT
        );
        CREATE TABLE jp_candidates (
            addr INTEGER PRIMARY KEY,
            text TEXT,
            paired INTEGER,
            pair_kind TEXT,
            pair_addr INTEGER,
            pair_text TEXT,
            preceding_field_text TEXT
        );
        CREATE INDEX idx_runs_kind ON runs(kind);
        CREATE INDEX idx_jp_paired ON jp_candidates(paired);
        """)
        con.executemany(
            "INSERT OR REPLACE INTO runs (addr, end_addr, kind, text) VALUES (?,?,?,?)",
            [(r["addr"], r["start"] + args.base + (r["end"] - r["start"]), r["kind"], r["text"]) for r in runs],
        )
        con.executemany(
            """INSERT OR REPLACE INTO jp_candidates
               (addr, text, paired, pair_kind, pair_addr, pair_text, preceding_field_text)
               VALUES (?,?,?,?,?,?,?)""",
            [(j["addr"], j["text"], int(j["paired"]), j["pair_kind"], j["pair_addr"],
              j["pair_text"], j["preceding_field_text"]) for j in jp_results],
        )
        con.commit()
        con.close()
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
