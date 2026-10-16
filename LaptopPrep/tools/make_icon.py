#!/usr/bin/env python3
"""
make_icon.py - draws LaptopPrep's application icon.

The mark is the Esprit ICT "e": a solid bar on the right and two folded
arms on the left.  It is drawn from geometry here, so the repository can
build from source alone (no binary art to lose), and it is written as a
real multi-resolution .ico that Windows picks from per context:

    16  title bar / small icons        32  taskbar
    20  at 125% scaling                40  at 125%, taskbar
    24  at 150% scaling                48  medium icons
    32  shell                          64  large icons
    128/256  explorer, task switcher

Run from the project root:      python3 tools/make_icon.py
It writes src/laptopprep.ico (the file prep.rc refers to).

Requires Pillow:                pip install pillow
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required:  pip install pillow")

# brand ramp
BRAND = (0xFF, 0x8A, 0x2A, 255)
BRAND_DARK = (0xD9, 0x6C, 0x12, 255)
INK = (0x12, 0x12, 0x14, 255)
INK_SOFT = (0x1E, 0x1F, 0x22, 255)

SS = 8          # supersampling factor: everything is drawn 8x and reduced


def mark_polygons(size):
    """The three polygons of the mark, in a size x size box."""
    w = h = size

    def px(fx, fy):
        return (fx * w, fy * h)

    bar = [px(0.765, 0.000), px(0.975, 0.000), px(0.975, 1.000), px(0.765, 1.000)]
    up = [px(0.100, 0.195), px(0.765, 0.355), px(0.765, 0.510), px(0.005, 0.335)]
    down = [px(0.100, 0.805), px(0.765, 0.645), px(0.765, 0.490), px(0.005, 0.665)]
    return [bar, up, down]


def render(size, rounded=True, plate=True):
    """One icon bitmap at `size` x `size` pixels."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if plate:
        # a dark rounded square so the orange mark stays visible on any
        # wallpaper, plus a brand hairline
        pad = big * 0.055
        r = big * 0.235
        d.rounded_rectangle([pad, pad, big - pad - 1, big - pad - 1],
                            radius=r, fill=INK)
        d.rounded_rectangle([pad, pad, big - pad - 1, big - pad - 1],
                            radius=r, outline=(0x2A, 0x2B, 0x2F, 255),
                            width=max(1, int(big * 0.012)))

    inner = big * (0.66 if plate else 1.0)
    off = (big - inner) / 2.0
    polys = mark_polygons(inner)

    # draw the mark twice: a soft drop shadow, then the mark itself
    for poly in polys:
        d.polygon([(x + off + big * 0.012, y + off + big * 0.020) for x, y in poly],
                  fill=(0, 0, 0, 90))
    for poly in polys:
        d.polygon([(x + off, y + off) for x, y in poly], fill=BRAND)

    # a thin darker edge under the bar gives the mark a folded-paper feel
    bar = polys[0]
    d.polygon([(bar[0][0] + off, bar[0][1] + off),
               (bar[1][0] + off, bar[1][1] + off),
               (bar[1][0] + off - inner * 0.035, bar[1][1] + off),
               (bar[0][0] + off - inner * 0.035, bar[0][1] + off)], fill=BRAND_DARK)

    img = img.resize((size, size), Image.LANCZOS)
    if not rounded:
        img = img.convert("RGBA")
    return img


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "src", "laptopprep.ico")

    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = []
    for s in sizes:
        # the tiny sizes read better without the plate, Windows draws them
        # on solid backgrounds anyway
        frames.append(render(s, plate=(s >= 32)))

    frames[0].save(out, format="ICO",
                   sizes=[(s, s) for s in sizes],
                   append_images=frames[1:])
    print("wrote %s (%d sizes: %s)"
          % (out, len(sizes), ", ".join(str(s) for s in sizes)))
    print("prep.rc refers to this file as: IDI_APPICON ICON \"laptopprep.ico\"")


if __name__ == "__main__":
    main()
