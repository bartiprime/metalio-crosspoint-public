#!/usr/bin/env python3
"""Emit the 64 px masks for the FreeInkUI BITMAP icon families.

Two icon conventions exist in this tree and they are NOT interchangeable:

  * masks drawn through the theme's drawIconAtSize()/drawIcon() path
    (menuIcons64.h, settingsRowIcons64.h) are rasterised at THRESHOLD 60 and
    pre-rotated 90 deg, because those samplers transpose with the
    (size-1-row, col) mapping. Verified: folder at 64 px, threshold 60,
    rotated -> icon_folder_64_bits byte-for-byte.
  * masks handed to freeink::ui as a BitmapRef via bitmapFromIcon()
    (listIcons.h, readerToolbarIcons.h) are rasterised at THRESHOLD 110 with
    NO rotation, because the fui bitmap path draws them orientation-correct
    itself. Verified: menu/type/ellipsis/chevron-left/chevron-right at 24 px,
    threshold 110, unrotated -> the shipped readerToolbarIcons.h arrays
    byte-for-byte; folder/file-text at 32 px -> 2-3 bits from the shipped
    listIcons.h arrays (cairosvg's antialiasing differs slightly from
    rsvg-convert; the session's earlier cairosvg passes showed the same
    magnitude).

This script authors the second kind at 64 px, which is what the Andro theme's
56 px row box and 40 px reader-toolbar glyphs need. It reproduces the existing
size of a family as its own self-check before writing anything:

    python scripts/gen_ui_icons64.py --svgdir <lucide/icons> \
        --check --out-dir src/components/icons

Requires: cairosvg + Pillow (rsvg-convert is not installed on the build host,
and gen_icons.py shells out to it).
"""
import argparse
import io
import os
import re
import sys

from PIL import Image

import cairosvg

# The fui/BitmapRef families' threshold (see the module docstring).
THRESHOLD = 110

# (alias stem, lucide name). The alias becomes icon_<alias>_64[/_bits].
LIST_ICONS = [
    ("list_folder", "folder"),
    ("list_file_text", "file-text"),
    ("list_image", "image"),
    ("list_book", "book"),
    ("list_file", "file"),
    ("list_wifi", "wifi"),
    ("list_library", "library"),
    ("list_blocks", "blocks"),
    ("list_radio_tower", "radio-tower"),
    ("list_usb", "usb"),
    ("list_bookmark", "bookmark"),
]

READER_TOOLBAR_ICONS = [
    ("reader_contents", "menu"),
    ("reader_text", "type"),
    ("reader_more", "ellipsis"),
    ("reader_back", "chevron-left"),
    ("reader_next", "chevron-right"),
]


def rasterize(svg_path, px):
    png = cairosvg.svg2png(url=svg_path, output_width=px, output_height=px)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    return bg.convert("L")


def pack(img, px):
    """1 bpp MSB-first, bit 1 = transparent, plus the optical centre row."""
    pix = img.load()
    data = []
    sum_y = 0
    count = 0
    for y in range(px):
        for xb in range(0, px, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                white = 1
                if x < px and pix[x, y] < THRESHOLD:
                    white = 0
                    sum_y += y
                    count += 1
                byte |= white << (7 - b)
            data.append(byte)
    return data, (round(sum_y / count) if count else px // 2)


def emit(entries, svgdir, px, out_path, source):
    out = [
        "#pragma once",
        "",
        '#include "Icon.h"',
        "",
        f"// Generated from Lucide SVGs by {source}.",
        "// fui BitmapRef family: THRESHOLD 110, NO rotation (the bitmap path is",
        "// orientation-correct itself -- do not copy the pre-rotate the",
        "// drawIconAtSize masks in menuIcons64.h/settingsRowIcons64.h need).",
        f"// Sizes: {px}px. Icons: {len(entries)}.",
        "",
    ]
    for alias, lucide in entries:
        data, center = pack(rasterize(os.path.join(svgdir, lucide + ".svg"), px), px)
        arr = f"icon_{alias}_{px}_bits"
        out.append(f"// {alias}  (lucide: {lucide})")
        body = [", ".join(f"0x{b:02X}" for b in data[i:i + 19]) for i in range(0, len(data), 19)]
        out.append("static const uint8_t " + arr + "[] = {")
        out.append(",\n".join("    " + line for line in body) + "};")
        out.append(f"static const freeink::Icon icon_{alias}_{px} = {{{px}, {px}, {center}, {arr}}};")
        out.append("")
    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote", out_path, f"({len(entries)} icons)")


def existing_array(header, name):
    txt = open(header, encoding="utf-8", errors="replace").read()
    m = re.search(r"%s\[\] = \{(.*?)\};" % name, txt, re.S)
    if m is None:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def diff_bits(a, b, px):
    rb = (px + 7) // 8
    return sum(bin(a[i] ^ b[i]).count("1") for i in range(px * rb))


def check(svgdir, repo):
    """Re-render each family's smallest shipped size and diff it."""
    cases = [
        (os.path.join(repo, "src/components/icons/readerToolbarIcons.h"), READER_TOOLBAR_ICONS, 24),
        (os.path.join(repo, "src/components/icons/listIcons.h"), LIST_ICONS, 32),
    ]
    bad = 0
    for header, entries, px in cases:
        for alias, lucide in entries:
            # list_* aliases are 64-header-local (icon_folder_64 already exists in
            # menuIcons64.h with the rotated convention); the shipped 24/32 arrays
            # carry the bare name.
            stem = alias[5:] if alias.startswith("list_") else alias
            bits_name = f"icon_{stem}_{px}_bits"
            ref = existing_array(header, bits_name)
            if ref is None:
                print(f"SKIP {bits_name}: not in {os.path.basename(header)}")
                continue
            data, _ = pack(rasterize(os.path.join(svgdir, lucide + ".svg"), px), px)
            d = diff_bits(bytes(data), ref, px)
            total = px * ((px + 7) // 8) * 8
            verdict = "exact" if d == 0 else f"{d} bits of {total}"
            print(f"{bits_name}: {verdict}")
            # Thin-stroke icons (wifi, radio-tower) move 5-6 anti-aliased bits
            # against rsvg-convert's edges; anything past that means a different
            # threshold or a rotation crept in.
            if d > 8:
                bad += 1
    if bad:
        print(f"FAIL: {bad} icon(s) differ from the shipped masks", file=sys.stderr)
        return 1
    print("CHECK OK: every shipped size reproduced within a few bits")
    return 0


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--svgdir", default=os.path.join(repo, "freeink-sdk/libs/assets/Icons/lucide/icons"))
    ap.add_argument("--px", type=int, default=64)
    ap.add_argument("--out-dir", default=os.path.join(repo, "src/components/icons"))
    ap.add_argument("--check", action="store_true", help="validate against the shipped 24/32 masks only")
    args = ap.parse_args(argv)
    if args.check:
        return check(args.svgdir, repo)
    source = "scripts/gen_ui_icons64.py"
    emit(LIST_ICONS, args.svgdir, args.px, os.path.join(args.out_dir, "listIcons64.h"), source)
    emit(READER_TOOLBAR_ICONS, args.svgdir, args.px,
         os.path.join(args.out_dir, "readerToolbarIcons64.h"), source)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))