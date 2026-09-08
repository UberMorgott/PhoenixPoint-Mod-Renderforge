"""Measure magenta bleed on the outer rows/cols of each logged icon rect.
usage: python measure.py <png> "<rects string from IconBleed.Rects()>"
metric per pixel = max(0, min(R,B) - G) (0 = no magenta). Per edge: max over the 3 pixel rows nearest the edge
(one outside, two inside), reported as max line intensity 0..255 and mean along the edge.
For the vanilla control the metric is edge luminance instead (no magenta available).
"""
import sys, json
from PIL import Image

png, rects = sys.argv[1], sys.argv[2]
im = Image.open(png).convert("RGB")
W, H = im.size
px = im.load()

def mag(p):
    r, g, b = p
    return max(0, min(r, b) - g)

def lum(p):
    r, g, b = p
    return (r + g + b) // 3

out = {}
for ent in rects.strip(";").split(";"):
    name, coords = ent.split(":")
    name = name.replace(",", ".")
    toks = coords.split(",")
    if len(toks) == 8:  # ru culture: decimal comma -> "260,25,1136,00,..."
        toks = [toks[i] + "." + toks[i + 1] for i in range(0, 8, 2)]
    l, t, r, b = [float(v) for v in toks]
    f = lum if name.startswith("vanilla") else mag
    li, ti, ri, bi = int(round(l)), int(round(t)), int(round(r)), int(round(b))
    edges = {}
    def band(rows):  # rows: iterable of (x,y)
        vals = [f(px[x, y]) for x, y in rows if 0 <= x < W and 0 <= y < H]
        return max(vals) if vals else -1
    res = {}
    # For each edge the band = 3 lines: one outside, two inside; exclude 2 px at corners.
    res["L"] = max(band([(x, y) for y in range(ti + 2, bi - 2)]) for x in (li - 1, li, li + 1))
    res["R"] = max(band([(x, y) for y in range(ti + 2, bi - 2)]) for x in (ri - 2, ri - 1, ri))
    res["T"] = max(band([(x, y) for x in range(li + 2, ri - 2)]) for y in (ti - 1, ti, ti + 1))
    res["B"] = max(band([(x, y) for x in range(li + 2, ri - 2)]) for y in (bi - 2, bi - 1, bi))
    # interior sanity: centre pixel luminance (glyph) and interior magenta
    cx, cy = (li + ri) // 2, (ti + bi) // 2
    res["centre"] = lum(px[cx, cy])
    res["inner"] = max(f(px[x, y]) for x in range(li + 4, ri - 4) for y in range(ti + 4, bi - 4))
    out[name] = res
print(json.dumps(out))
