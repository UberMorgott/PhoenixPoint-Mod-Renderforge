"""Run the production style probe on an existing screenshot; no neural/image-generation dependency."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("probe", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--scratch", type=Path, required=True)
args = parser.parse_args()
assert sys.byteorder == "little"
args.scratch.mkdir(parents=True, exist_ok=True)
args.output.mkdir(parents=True, exist_ok=True)
original = Image.open(args.source).convert("RGBA")
width, height = original.size
raw = args.scratch / "input.rgba32f"
array("f", (v / 255 for v in original.tobytes())).tofile(raw.open("wb"))
images = [original]
for mode in (1, 2):
    target = args.scratch / f"style-{mode}.rgba32f"
    subprocess.run([str(args.probe), str(raw), str(width), str(height), str(mode), "6", str(target)], check=True)
    values = array("f")
    with target.open("rb") as stream:
        values.fromfile(stream, width * height * 4)
    images.append(Image.frombytes("RGBA", original.size, bytes(max(0, min(255, round(v * 255))) for v in values)))

# Keep the same scene crop for every mode; screenshot-baked markers are part of this test input.
crop = (0, 100, width, min(540, height))
preview_width = 960
preview_height = round((crop[3] - crop[1]) * preview_width / width)
sheet = Image.new("RGB", (preview_width, (preview_height + 30) * 3 + 46), "#15191d")
draw = ImageDraw.Draw(sheet)
draw.text((12, 8), "Production shader on a saved screenshot (WARP) | NOT a live-game capture | Strength 100%", fill="white")
labels = ("Original fixture", "Cartoon: colour bands + contrast ink", "Pixel art: 6 output pixels per block, 8 levels per RGB channel")
for index, (label, img) in enumerate(zip(labels, images)):
    y = 38 + index * (preview_height + 30)
    draw.text((12, y), label, fill="white")
    sheet.paste(img.crop(crop).resize((preview_width, preview_height), Image.Resampling.LANCZOS).convert("RGB"), (0, y + 22))
sheet.save(args.output / "contact-sheet.png")
manifest = {
    "source": str(args.source.resolve()), "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
    "source_size": [width, height], "probe": str(args.probe.resolve()),
    "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
    "modes": labels, "strength": 1, "pixel_size": 6, "crop": crop,
    "method": "Actual compiled production HLSL, D3D11 WARP, display RGB; image display resized after shader evaluation.",
    "limits": "Saved screenshot includes baked UI markers. No live UI exclusion, motion, FG, NR or performance proof."
}
(args.output / "contact-sheet.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("PASS: saved same-input production-shader preview and provenance manifest")
