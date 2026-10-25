#!/usr/bin/env python3
"""Bake the built-in reading faces.

JIS X 0208 plus ASCII, packed as XGF2, then zlib-wrapped for flash.
The device inflates one face into PSRAM when it is selected. Regeneration
needs the source fonts; the committed .ygzf files do not.

    python3 tools/xgf2/bake_builtin.py
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import gen_xgf2 as g

ROOT = Path(__file__).resolve().parents[2]
MAGIC = 0x465A4759  # YGZF
EM = 40
RUBY = 20
MAX_PACKED = 2 * 1024 * 1024

DEFAULT_MINCHO = Path(
    "/Users/kscho/Downloads/Noto_Sans_JP,Noto_Serif_JP/Noto_Serif_JP/static/NotoSerifJP-Regular.ttf"
)
DEFAULT_GOTHIC = Path(
    "/Users/kscho/Downloads/Noto_Sans_JP,Noto_Serif_JP/Noto_Sans_JP/static/NotoSansJP-Regular.ttf"
)


def jis_and_ascii() -> list[int]:
    cps: set[int] = set(range(0x20, 0x7F))
    cps.add(0xFFFD)
    for ku in range(1, 95):
        for ten in range(1, 95):
            try:
                ch = bytes([ku + 0xA0, ten + 0xA0]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                cps.add(ord(ch))
    # Dash, ellipsis, and the katakana prolonged mark sit outside some JIS rows.
    cps.update([0x2014, 0x2015, 0x2026, 0x3000, 0x30FC])
    return sorted(cps)


def present(font, cps: list[int]) -> list[int]:
    out: list[int] = []
    for cp in cps:
        try:
            mask = font.getmask(chr(cp))
        except Exception:
            continue
        if mask.size[0] > 0 and mask.size[1] > 0:
            out.append(cp)
    return out


def pack(raw: bytes) -> bytes:
    return struct.pack("<II", MAGIC, len(raw)) + zlib.compress(raw, 9)


def ink_of(raw: bytes, cp: int) -> int:
    if raw[:4] != b"XGF2":
        raise SystemExit("baked blob is not XGF2")
    em = raw[8]
    stride = struct.unpack_from("<H", raw, 12)[0]
    body_count, _ruby_count, interval_count = struct.unpack_from("<HHH", raw, 16)
    intervals_off, _ruby_off, body_off, _ruby_bits = struct.unpack_from("<IIII", raw, 24)
    gid = None
    for i in range(interval_count):
        first, last, start = struct.unpack_from("<HHH", raw, intervals_off + i * 6)
        if first <= cp <= last:
            gid = start + (cp - first)
            break
    if gid is None or gid >= body_count:
        return 0
    slot = raw[body_off + gid * stride : body_off + (gid + 1) * stride]
    if len(slot) != stride:
        return 0
    row = (em + 3) // 4
    ink = 0
    for y in range(em):
        for x in range(em):
            v = (slot[y * row + x // 4] >> (6 - (x % 4) * 2)) & 3
            if v:
                ink += 1
    return ink


def bake(ttf: Path, dest: Path) -> None:
    from PIL import ImageFont

    probe = ImageFont.truetype(str(ttf), 16)
    cps = present(probe, jis_and_ascii())
    if len(cps) < 1000:
        raise SystemExit(f"{ttf} only covers {len(cps)} codepoints")
    id_of = g.assign_ids(cps)
    ruby_set = set(g.ruby_candidates(list(id_of)))
    body_font = ImageFont.truetype(str(ttf), EM)
    ruby_font = ImageFont.truetype(str(ttf), RUBY)
    body: dict[int, bytes] = {}
    ruby: dict[int, bytes] = {}
    ordered = sorted(id_of, key=lambda c: id_of[c])
    total = len(ordered)
    for i, cp in enumerate(ordered):
        ch = chr(cp)
        body[cp] = g.raster_char(body_font, ch, EM)
        if cp in ruby_set:
            ruby[cp] = g.raster_char(ruby_font, ch, RUBY)
        if (i + 1) % 500 == 0:
            print(f"  {dest.name} {i + 1}/{total}", file=sys.stderr)
    raw_path = Path("/tmp") / (dest.stem + ".xgf2")
    g.write_xgf2(raw_path, id_of, body, ruby, EM, RUBY)
    raw = raw_path.read_bytes()
    raw_path.unlink(missing_ok=True)
    if ink_of(raw, 0x3042) < 8:
        raise SystemExit(f"{dest.name} hiragana あ has no ink")
    packed = pack(raw)
    if len(packed) > MAX_PACKED:
        raise SystemExit(f"{dest.name} is {len(packed)} bytes, over {MAX_PACKED}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(packed)
    print(
        f"{dest.name}: glyphs={len(id_of)} raw={len(raw)} packed={len(packed)}",
        file=sys.stderr,
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mincho", type=Path, default=DEFAULT_MINCHO)
    ap.add_argument("--gothic", type=Path, default=DEFAULT_GOTHIC)
    ap.add_argument("--out", type=Path, default=ROOT / "lib/BakedFont")
    args = ap.parse_args()
    for label, path in (("mincho", args.mincho), ("gothic", args.gothic)):
        if not path.is_file():
            raise SystemExit(f"missing {label} font: {path}")
        print(f"baking {label} from {path}", file=sys.stderr)
        bake(path, args.out / f"{label}.ygzf")


if __name__ == "__main__":
    main()
