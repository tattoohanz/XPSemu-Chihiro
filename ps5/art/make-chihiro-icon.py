#!/usr/bin/env python3
# XPSemu Chihiro: the app icon, sce_sys/icon0.png (512x512). XPSemu's green
# "X" emblem (drawn by make-art.py, here a little smaller and higher) over a
# dark band with "Chihiro" set in Inter Display Black (SIL Open Font
# License), not Sega's Chihiro logo.
#
# Usage: make-chihiro-icon.py [OUTPUT.png]   (default: ps5/sce_sys/icon0.png)
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
icon_out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    here, '..', 'sce_sys', 'icon0.png')
FONT = '/usr/share/fonts/opentype/inter/InterDisplay-Black.otf'

# make-art.py's drawing helpers (radial, ring, emblem), without its own
# output at the end (it sets `out` for itself).
art = open(os.path.join(here, 'make-art.py')).read().split('\nmake_icon()\n')[0]
sys.argv = [sys.argv[0], '/tmp']
exec(compile(art, 'make-art.py', 'exec'))

S = 1024  # drawn at 1024, saved at 512


def text_mask(text, size, tracking):
    f = ImageFont.truetype(FONT, size)
    adv = [f.getlength(c) for c in text]
    asc, desc = f.getmetrics()
    m = Image.new('L', (int(sum(adv) + tracking * len(text)) + 40,
                        asc + desc + 40), 0)
    d = ImageDraw.Draw(m)
    x = 20
    for c, a in zip(text, adv):
        d.text((x, 20), c, font=f, fill=255)
        x += a + tracking
    return m.crop(m.getbbox())


def fill(mask, top, bottom):
    """The mask filled with a top-to-bottom gradient (RGBA)."""
    w, h = mask.size
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    rgba = np.zeros((h, w, 4), np.float32)
    rgba[..., :3] = np.array(top, np.float32) * (1 - t) + \
        np.array(bottom, np.float32) * t
    rgba[..., 3] = np.asarray(mask, np.float32)
    return Image.fromarray(rgba.astype(np.uint8), 'RGBA')


def glow(mask, color, radius, pad=60):
    w, h = mask.size
    m = Image.new('L', (w + 2 * pad, h + 2 * pad), 0)
    m.paste(mask, (pad, pad))
    g = np.asarray(m.filter(ImageFilter.GaussianBlur(radius)), np.float32) / 255
    rgba = np.zeros((h + 2 * pad, w + 2 * pad, 4), np.float32)
    rgba[..., :3] = color
    rgba[..., 3] = np.clip(g * 1.3, 0, 1) * 255
    return Image.fromarray(rgba.astype(np.uint8), 'RGBA'), pad


def make():
    scale, cy = 0.72, 0.43  # the emblem: size and centre, of the icon
    bg = radial(S, S, S / 2, S * cy, S * 0.72, (14, 48, 12), (0, 0, 0))
    icon = Image.fromarray(bg.astype(np.uint8), 'RGB').convert('RGBA')
    icon.alpha_composite(ring(S, S * 0.43 * scale / 0.8, 10, (80, 200, 40, 90), 14),
                         (0, int(S * (cy - 0.5))))
    e = emblem(int(S * scale))
    icon.alpha_composite(e, ((S - e.width) // 2, int(S * cy - e.height / 2)))

    band = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(band)
    d.rectangle((0, int(S * 0.79), S, int(S * 0.97)), fill=(0, 0, 0, 170))
    d.line((0, int(S * 0.79), S, int(S * 0.79)), fill=(120, 230, 60, 150), width=4)
    icon.alpha_composite(band)

    m = text_mask('Chihiro', 160, 2)
    x0, y0 = int(S / 2 - m.width / 2), int(S * 0.88 - m.height / 2)
    g, pad = glow(m, (90, 220, 40), 14)
    icon.alpha_composite(g, (x0 - pad, y0 - pad))
    icon.alpha_composite(fill(m, (245, 255, 225), (150, 235, 70)), (x0, y0))

    icon.resize((512, 512), Image.LANCZOS).save(icon_out)
    print('icon written to', icon_out)


make()
