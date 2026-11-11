#!/usr/bin/env python3
"""reColossus: draws the app's icon (package/sce_sys/icon0.png, 512x512): an original picture, a dusk sky over a lone
plain with a beam of light, and the app's name. tools/mkicon.py <out.png> [font.ttf]"""
import math, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W = H = 512
out = sys.argv[1] if len(sys.argv) > 1 else "package/sce_sys/icon0.png"
font_path = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"
img = Image.new("RGB", (W, H))
px = img.load()
top, low = (24, 30, 44), (196, 160, 112)
horizon = 330
for y in range(H):
    t = min(1.0, y / horizon) ** 1.6
    c = tuple(int(top[i] + (low[i] - top[i]) * t) for i in range(3))
    for x in range(W):
        px[x, y] = c
d = ImageDraw.Draw(img)
# the plain and a low ridge
ridge = [(0, horizon + 6)]
for x in range(0, W + 1, 8):
    ridge.append((x, horizon - 10 - 18 * math.sin(x / 70.0) - 10 * math.sin(x / 23.0 + 1.0)))
ridge += [(W, H), (0, H)]
d.polygon(ridge, fill=(46, 44, 46))
d.rectangle([0, horizon + 30, W, H], fill=(30, 28, 30))
# the beam of light
beam = Image.new("L", (W, H), 0)
bd = ImageDraw.Draw(beam)
bd.polygon([(250, 0), (262, 0), (259, horizon - 8), (253, horizon - 8)], fill=230)
beam = beam.filter(ImageFilter.GaussianBlur(5))
img = Image.composite(Image.new("RGB", (W, H), (255, 246, 222)), img, beam)
d = ImageDraw.Draw(img)
font = ImageFont.truetype(font_path, 58)
text = "reColossus"
w = d.textlength(text, font=font)
d.text(((W - w) / 2 + 2, 412), text, font=font, fill=(0, 0, 0))
d.text(((W - w) / 2, 410), text, font=font, fill=(236, 226, 204))
img.save(out)
print(f"mkicon: {out}")
