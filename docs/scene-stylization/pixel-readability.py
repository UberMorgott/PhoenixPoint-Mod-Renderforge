"""Matched normal-framing PixelArt comparison; 1440p input is explicitly a display-scale simulation."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument("source", type=Path)
p.add_argument("probe", type=Path)
p.add_argument("output", type=Path)
p.add_argument("--scratch", type=Path, required=True)
args = p.parse_args()
args.scratch.mkdir(parents=True, exist_ok=True)
args.output.mkdir(parents=True, exist_ok=True)
source = Image.open(args.source).convert("RGBA")
assert source.size == (1280, 720), "This comparison records a specific 720p fixture"
manifest = {"source": str(args.source.resolve()), "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
            "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(), "default_block": 2,
            "palette_levels_per_channel": 32, "automatic_resolution_scaling": False,
            "limits": "Offline production HLSL/WARP. All screenshot UI is baked in. 1440p is an enlarged input, not a native game capture.",
            "cases": []}
for width, height in ((1280, 720), (2560, 1440)):
    image = source if width == 1280 else source.resize((width, height), Image.Resampling.BILINEAR)
    raw = args.scratch / f"{width}-input.rgba32f"
    with raw.open("wb") as stream:
        array("f", (v / 255 for v in image.tobytes())).tofile(stream)
    variants = [image]
    for block in (2, 3, 4):
        target = args.scratch / f"{width}-block-{block}.rgba32f"
        subprocess.run([str(args.probe), str(raw), str(width), str(height), "2", str(block), str(target)], check=True)
        values = array("f")
        with target.open("rb") as stream:
            values.fromfile(stream, width * height * 4)
        result = Image.frombytes("RGBA", image.size, bytes(max(0, min(255, round(v * 255))) for v in values))
        result.save(args.scratch / f"{width}-block-{block}.png")
        variants.append(result)
    # Full gameplay framing in every panel; the 720p sheet is pixel-for-pixel, no crop or magnification.
    display_w = 1280
    display_h = 720
    sheet = Image.new("RGB", (display_w * 2, (display_h + 30) * 2 + 40), "#15191d")
    draw = ImageDraw.Draw(sheet)
    caption = "Actual 1280x720 saved fixture; panels shown at 1:1" if width == 1280 else "2560x1440 DISPLAY-SCALE SIMULATION (enlarged 720p input); panels fit at 50%"
    draw.text((12, 8), caption + " | Offline production shader, not live gameplay", fill="white")
    for index, (label, variant) in enumerate(zip(("Original", "DEFAULT: 2 actual output pixels", "Optional: 3 actual output pixels", "Optional: 4 actual output pixels"), variants)):
        x, y = index % 2 * display_w, 40 + index // 2 * (display_h + 30)
        draw.text((x + 12, y), label + " | no extra outlines", fill="white")
        fitted = variant if width == display_w else variant.resize((display_w, display_h), Image.Resampling.LANCZOS)
        sheet.paste(fitted.convert("RGB"), (x, y + 24))
    name = f"pixel-readability-{height}p.png"
    sheet.save(args.output / name)
    manifest["cases"].append({"size": [width, height], "kind": "original fixture" if width == 1280 else "display-scale simulation",
                              "blocks": [2, 3, 4], "sheet": name})
(args.output / "pixel-readability.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("PASS: 720p native-fixture and 1440p display-scale comparisons; Off/2/3/4 blocks, normal framing")
