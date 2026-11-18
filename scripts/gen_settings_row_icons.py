#!/usr/bin/env python3
"""Minimal Lucide stroke-SVG rasterizer + mask generator (no rsvg needed).

Reproduces the packing conventions of freeink-sdk/libs/assets/Icons/tools/gen_icons.py:
  * pack MSB-first, bit = 1 for transparent/white, 0 for ink (Mask1: bit 0 = ink)
  * ink = luminance below THRESHOLD (110) after compositing on white
  * struct field 3 = round(mean ink row) = optical centre

Rendering is done by flattening every stroked element to polylines in the 24x24
viewBox, drawing them with Pillow's wide-line renderer at 4x supersampling, then
box-filtering down to the target size. Lucide is stroke-only (fill none,
stroke-width 2, round caps/joins), which is the subset handled here.
"""
import math
import re
import sys
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

THRESHOLD = 110
SS = 4  # supersample factor


# ---------------------------------------------------------------- path parsing
NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def tokenize(d):
    out = []
    for tok in re.findall(r"[MmLlHhVvCcSsQqTtAaZz]|" + NUM.pattern, d):
        out.append(tok)
    return out


def arc_points(x0, y0, rx, ry, phi_deg, large, sweep, x1, y1, steps=48):
    """Endpoint-parameterised elliptical arc (SVG spec F.6.5)."""
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    phi = math.radians(phi_deg)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx2, dy2 = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    x1p = cosp * dx2 + sinp * dy2
    y1p = -sinp * dx2 + cosp * dy2
    rx, ry = abs(rx), abs(ry)
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        coef = -coef
    cxp = coef * rx * y1p / ry
    cyp = -coef * ry * x1p / rx
    cx = cosp * cxp - sinp * cyp + (x0 + x1) / 2.0
    cy = sinp * cxp + cosp * cyp + (y0 + y1) / 2.0

    def ang(ux, uy, vx, vy):
        d = math.hypot(ux, uy) * math.hypot(vx, vy)
        if d == 0:
            return 0.0
        c = max(-1.0, min(1.0, (ux * vx + uy * vy) / d))
        a = math.acos(c)
        return -a if ux * vy - uy * vx < 0 else a

    theta1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dtheta > 0:
        dtheta -= 2 * math.pi
    elif sweep and dtheta < 0:
        dtheta += 2 * math.pi
    pts = []
    for i in range(1, steps + 1):
        t = theta1 + dtheta * i / steps
        x = cx + rx * math.cos(t) * cosp - ry * math.sin(t) * sinp
        y = cy + rx * math.cos(t) * sinp + ry * math.sin(t) * cosp
        pts.append((x, y))
    return pts


def path_polylines(d):
    """Returns a list of polylines (lists of points) for one SVG path."""
    toks = tokenize(d)
    i = 0
    x = y = 0.0
    start = (0.0, 0.0)
    lines, cur = [], []
    cmd = None

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    cur.append(start)
                    lines.append(cur)
                    cur = []
                x, y = start
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            if cur:
                lines.append(cur)
            x, y = nx, ny
            start = (x, y)
            cur = [(x, y)]
            cmd = "l" if rel else "L"  # subsequent pairs are lineto
        elif c == "L":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            x, y = nx, ny
            cur.append((x, y))
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif c in ("C", "S"):
            if c == "C":
                x1, y1, x2, y2, nx, ny = (num() for _ in range(6))
                if rel:
                    x1, y1, x2, y2, nx, ny = x + x1, y + y1, x + x2, y + y2, x + nx, y + ny
            else:
                x2, y2, nx, ny = (num() for _ in range(4))
                if rel:
                    x2, y2, nx, ny = x + x2, y + y2, x + nx, y + ny
                px, py = cur[-1] if cur else (x, y)
                x1, y1 = 2 * x - px, 2 * y - py
            steps = 16
            for s in range(1, steps + 1):
                t = s / steps
                mt = 1 - t
                bx = mt**3 * x + 3 * mt**2 * t * x1 + 3 * mt * t**2 * x2 + t**3 * nx
                by = mt**3 * y + 3 * mt**2 * t * y1 + 3 * mt * t**2 * y2 + t**3 * ny
                cur.append((bx, by))
            x, y = nx, ny
        elif c in ("Q", "T"):
            if c == "Q":
                x1, y1, nx, ny = (num() for _ in range(4))
                if rel:
                    x1, y1, nx, ny = x + x1, y + y1, x + nx, y + ny
            else:
                nx, ny = num(), num()
                if rel:
                    nx, ny = x + nx, y + ny
                px, py = cur[-1] if cur else (x, y)
                x1, y1 = 2 * x - px, 2 * y - py
            steps = 16
            for s in range(1, steps + 1):
                t = s / steps
                mt = 1 - t
                bx = mt**2 * x + 2 * mt * t * x1 + t**2 * nx
                by = mt**2 * y + 2 * mt * t * y1 + t**2 * ny
                cur.append((bx, by))
            x, y = nx, ny
        elif c == "A":
            rx, ry, phi = num(), num(), num()
            large = int(num())
            sweep = int(num())
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            cur.extend(arc_points(x, y, rx, ry, phi, large, sweep, nx, ny))
            x, y = nx, ny
        else:
            raise ValueError("unsupported command " + str(cmd))
    if cur:
        lines.append(cur)
    return lines


# ------------------------------------------------------------------- SVG walk
def svg_polylines(svg_path):
    tree = ET.parse(svg_path)
    root = tree.getroot()
    pls = []

    def tag(e):
        return e.tag.split("}")[-1]

    def attr(e, k, d=None):
        return e.get(k, d)

    for e in root.iter():
        t = tag(e)
        if t == "rect":
            x, y = float(attr(e, "x", 0)), float(attr(e, "y", 0))
            w, h = float(attr(e, "width", 0)), float(attr(e, "height", 0))
            rx = float(attr(e, "rx", attr(e, "ry", 0)) or 0)
            ry = float(attr(e, "ry", attr(e, "rx", 0)) or 0)
            rx, ry = min(rx, w / 2), min(ry, h / 2)
            if rx > 0 or ry > 0:
                d = (
                    f"M{x + rx} {y} h{w - 2 * rx} a{rx} {ry} 0 0 1 {rx} {ry} "
                    f"v{h - 2 * ry} a{rx} {ry} 0 0 1 {-rx} {ry} h{-(w - 2 * rx)} "
                    f"a{rx} {ry} 0 0 1 {-rx} {-ry} v{-(h - 2 * ry)} a{rx} {ry} 0 0 1 {rx} {-ry} z"
                )
                pls.extend(path_polylines(d))
            else:
                pls.append([(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)])
        elif t == "circle":
            cx, cy, r = (float(attr(e, k, 0)) for k in ("cx", "cy", "r"))
            n = 96
            pls.append([(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
                        for i in range(n + 1)])
        elif t == "line":
            pls.append([(float(attr(e, "x1", 0)), float(attr(e, "y1", 0))),
                        (float(attr(e, "x2", 0)), float(attr(e, "y2", 0)))])
        elif t in ("polyline", "polygon"):
            pts = [float(v) for v in re.findall(NUM.pattern, attr(e, "points", ""))]
            pl = list(zip(pts[0::2], pts[1::2]))
            if t == "polygon" and pl:
                pl.append(pl[0])
            pls.append(pl)
        elif t == "path":
            pls.extend(path_polylines(attr(e, "d", "")))
    return pls


def rasterize(svg_path, px):
    """Render at SS x px, box-filter down, return an L-mode image at px."""
    scale = (px * SS) / 24.0
    big = Image.new("L", (px * SS, px * SS), 255)
    drw = ImageDraw.Draw(big)
    width = max(1, round(2 * scale))  # stroke-width 2 in viewBox units
    for pl in svg_polylines(svg_path):
        pts = [(x * scale, y * scale) for (x, y) in pl]
        if len(pts) == 1:
            drw.ellipse([pts[0][0] - width / 2, pts[0][1] - width / 2,
                         pts[0][0] + width / 2, pts[0][1] + width / 2], fill=0)
            continue
        drw.line(pts, fill=0, width=width, joint="curve")
        # round caps: half-disc at both ends
        for (cx, cy) in (pts[0], pts[-1]):
            drw.ellipse([cx - width / 2, cy - width / 2, cx + width / 2, cy + width / 2], fill=0)
    return big.resize((px, px), Image.BOX)


def pack(img, px):
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


def emit(entries, svgdir, px, out_path):
    out = [
        "#pragma once",
        "",
        '#include "Icon.h"',
        "",
        "// Generated from Lucide SVGs by scripts/gen_settings_row_icons.py "
        "(rsvg-convert is unavailable on the build host, so the rasteriser is local).",
        f"// Sizes: {px}px. Icons: {len(entries)}.",
        "",
    ]
    for alias, lucide in entries:
        data, center = pack(rasterize(f"{svgdir}/{lucide}.svg", px), px)
        arr = f"icon_{alias}_{px}_bits"
        out.append(f"// {alias}  (lucide: {lucide})")
        out.append(f"static const uint8_t {arr}[] = {{" + ", ".join(f"0x{b:02X}" for b in data) + "};")
        out.append(f"static const freeink::Icon icon_{alias}_{px} = {{{px}, {px}, {center}, {arr}}};")
        out.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(out))
    print("wrote", out_path)


SETTINGS_ROW_ICONS = [
    # alias (C identifier stem)  lucide file
    ("srow_settings", "settings"),
    ("srow_smartphone", "smartphone"),
    ("srow_book_open", "book-open"),
    ("srow_pointer", "pointer"),
    ("srow_library", "library"),
    ("srow_wifi", "wifi"),
    ("srow_bug", "bug"),
]


def main(argv):
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--svgdir", required=True)
    ap.add_argument("--px", type=int, default=64)
    ap.add_argument("--out")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--check", nargs="*", metavar="NAME",
                    help="re-render NAME at 24/32 and print ASCII (fidelity eyeball)")
    args = ap.parse_args(argv)

    if args.check:
        for name in args.check:
            for px in (24, 32):
                data, center = pack(rasterize(f"{args.svgdir}/{name}.svg", px), px)
                print(f"== {name} {px}px (optical centre {center})")
                for row in range(px):
                    print("".join(
                        "." if (data[row * (px // 8) + col // 8] >> (7 - col % 8)) & 1 else "#"
                        for col in range(px)))
        return 0

    entries = SETTINGS_ROW_ICONS
    if args.preview:
        for alias, lucide in entries:
            data, center = pack(rasterize(f"{args.svgdir}/{lucide}.svg", args.px), args.px)
            print(f"== {alias} (lucide: {lucide}) {args.px}px centre={center}")
            for row in range(args.px):
                print("".join(
                    "." if (data[row * (args.px // 8) + col // 8] >> (7 - col % 8)) & 1 else "#"
                    for col in range(args.px)))
    if args.out:
        emit(entries, args.svgdir, args.px, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))