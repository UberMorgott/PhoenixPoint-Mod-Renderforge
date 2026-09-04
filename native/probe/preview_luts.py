"""Render the production HLSL on one existing screenshot using lut_probe (Pillow only)."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("probe", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--work", type=Path, required=True)
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
args.output.parent.mkdir(parents=True, exist_ok=True)
source = Image.open(args.input).convert("RGB")
# Same central scene crop for every profile; removes most overlaid menus from this 1280x720 capture.
crop = (0, 100, 1280, 540)
if source.size != (1280, 720):
    raise ValueError("Expected the documented 1280x720 screenshot; choose an explicit crop for other inputs")
scene = source.crop(crop).resize((480, 165), Image.Resampling.LANCZOS)
pixels = array("f", (component / 255 for y in range(scene.height) for x in range(scene.width)
                     for component in (*scene.getpixel((x, y)), 255)))
raw_input = args.work / "source.rgba32f"
raw_input.write_bytes(pixels.tobytes())
presets = [(None, "Source / LUT Off"), (5, "B&W Cinema"), (6, "Noir"),
           (7, "Amber Film"), (8, "Arctic"), (9, "Vintage Sepia")]
sheet = Image.new("RGB", (1440, 414), (18, 20, 24))
draw = ImageDraw.Draw(sheet)
for index, (preset, label) in enumerate(presets):
    preview = scene
    if preset is not None:
        raw_output = args.work / f"preset-{preset}.rgba32f"
        subprocess.run([str(args.probe.resolve()), str(raw_input.resolve()), "480", "165", str(preset), str(raw_output.resolve())], check=True)
        floats = array("f")
        floats.frombytes(raw_output.read_bytes())
        rgb = bytes(max(0, min(255, round(value * 255))) for i, value in enumerate(floats) if i % 4 != 3)
        preview = Image.frombytes("RGB", scene.size, rgb)
    x, y = (index % 3) * 480, (index // 3) * 197
    draw.text((x + 10, y + 8), label + (" / 100%" if preset is not None else ""), fill="white")
    sheet.paste(preview, (x, y + 28))
draw.text((10, 398), "Offline production-shader preview. Same screenshot; display RGB; sharpening Off. Not a live-game validation.", fill=(180, 190, 205))
sheet.save(args.output)
manifest = {"input": str(args.input.resolve()), "input_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
            "crop": crop, "preview_size": scene.size, "presets": presets, "strength": 1, "sharpness": 0,
            "domain": "Normalized screenshot display RGB, no sRGB decode; FP32 WARP shader output clamped to 8-bit PNG.",
            "limits": "Offline display approximation; any in-scene HUD is also graded. Live pre-UI grading and FP16-linear output are not visually validated."}
args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"PASS: {args.output.resolve()} ({args.output.stat().st_size} bytes); 6 tiles from one source")
