#!/usr/bin/env python3
"""Draws an A4 test page for checking the MF3010 output.

  testpage.py page.pbm          1-bit page bitmap for pbm2ufr (4720x4516, 600x400 dpi)
  testpage.py page.pgm 300      gray A4 page at 300 dpi, feed it to rasgen

Circles should come out round and the ruler should measure right, that's how
you know the 600x400 dpi scaling and the margins are correct.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

DPI = 600
W, H = 4960, 7016          # A4 at 600 dpi
M = 120                    # printer margin, 5.08 mm


def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    for base in ("/usr/share/fonts/truetype/dejavu/", ""):
        try:
            return ImageFont.truetype(base + name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def mm(x):
    return round(x * DPI / 25.4)


def draw_page():
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)
    x0, y = M + mm(10), M + mm(10)

    d.text((x0, y), "MF3010 Wi-Fi bridge - test page", font=font(150, True), fill=0)
    y += 260
    lines = [
        "Printed through the ESP32-S3, no Canon driver involved.",
        "Page bitmap 4720 x 4516 at 600 x 400 dpi, JBIG compressed in 256 line bands.",
    ]
    for t in lines:
        d.text((x0, y), t, font=font(80), fill=0)
        y += 120

    y += 80
    d.text((x0, y), "Gray ramp:", font=font(70), fill=0)
    y += 110
    step = (W - 2 * x0) // 11
    for i in range(11):
        g = 255 - i * 255 // 10
        d.rectangle([x0 + i * step, y, x0 + (i + 1) * step - 20, y + 220], fill=g, outline=0, width=4)
    y += 340

    d.text((x0, y), "Circles (should be round):", font=font(70), fill=0)
    y += 120
    cx = x0
    for r in (mm(25), mm(18), mm(12), mm(6)):
        d.ellipse([cx, y, cx + 2 * r, y + 2 * r], outline=0, width=12)
        cx += 2 * r + mm(12)
    y += 2 * mm(25) + 140

    d.text((x0, y), "Fine lines, 1 px at 600 dpi:", font=font(70), fill=0)
    y += 120
    for i in range(60):
        d.line([x0 + i * 12, y, x0 + i * 12, y + 300], fill=0, width=1)
    for i in range(30):
        d.line([x0 + 1000, y + i * 10, x0 + 2400, y + i * 10], fill=0, width=1)
    y += 420

    d.text((x0, y), "Ruler (mm):", font=font(70), fill=0)
    y += 120
    d.line([x0, y, x0 + mm(180), y], fill=0, width=6)
    for k in range(181):
        h = 120 if k % 10 == 0 else (80 if k % 5 == 0 else 45)
        d.line([x0 + mm(k), y, x0 + mm(k), y + h], fill=0, width=5)
        if k % 10 == 0:
            d.text((x0 + mm(k) - 30, y + 140), str(k), font=font(50), fill=0)
    y += 320

    d.text((x0, y), "Gradient:", font=font(70), fill=0)
    y += 120
    gw = W - 2 * x0
    for i in range(gw):
        d.line([x0 + i, y, x0 + i, y + 200], fill=255 - i * 255 // gw)
    y += 320

    # shaded ball, mostly to see how the halftone handles smooth tones
    r = mm(28)
    cxy = (x0 + r, y + r)
    for k in range(r, 0, -4):
        g = int(40 + 200 * (1 - k / r) ** 0.6)
        off = (r - k) * 0.4
        d.ellipse([cxy[0] - k - off, cxy[1] - k - off, cxy[0] + k - off, cxy[1] + k - off], fill=g)
    d.text((x0, H - M - mm(12)), "github.com/ArnavCodez", font=font(50), fill=0)
    return im


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    page = draw_page()
    if out.endswith(".pbm"):
        body = page.crop((M, M, W - M, H - M))
        body = body.resize((4720, 4516), Image.LANCZOS)
        body.convert("1").save(out)
    else:
        dpi = int(sys.argv[2]) if len(sys.argv) > 2 else 300
        page.resize((W * dpi // 600, H * dpi // 600), Image.LANCZOS).save(out)


if __name__ == "__main__":
    main()
