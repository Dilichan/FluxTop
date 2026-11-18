"""Export FluxTop SVG masters to PNG and the multi-resolution Windows ICO.

Development-only dependencies: Inkscape CLI and Pillow. Neither is needed to
build/run FluxTop: the checked-in ICO is embedded by the resource compiler.
"""
from __future__ import annotations

import argparse
from io import BytesIO
from pathlib import Path
import os
import shutil
import struct
import subprocess

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "assets" / "branding"
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def render(inkscape: str, source: Path, destination: Path, size: int) -> None:
    result = subprocess.run(
        [inkscape, str(source), "--export-area-page", f"--export-width={size}",
         f"--export-height={size}", f"--export-filename={destination}"],
        capture_output=True, text=True, timeout=60,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if result.returncode or not destination.is_file():
        raise RuntimeError(f"SVG export failed: {source.name}: {result.stderr}")


def write_ico(frames: dict[int, Image.Image], destination: Path) -> None:
    # ICO allows a different drawing at each size. Small frames deliberately
    # use the simplified mark; 64..256 use the detailed vector reconstruction.
    payloads = []
    for size in SIZES:
        buffer = BytesIO()
        frames[size].save(buffer, format="PNG", optimize=True)
        payloads.append(buffer.getvalue())
    offset = 6 + 16 * len(SIZES)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    for size, payload in zip(SIZES, payloads):
        encoded_size = 0 if size == 256 else size
        directory += struct.pack("<BBBBHHII", encoded_size, encoded_size, 0, 0,
                                 1, 32, len(payload), offset)
        offset += len(payload)
    destination.write_bytes(bytes(directory) + b"".join(payloads))


def preview(output: Path, frames: dict[int, Image.Image]) -> None:
    canvas = Image.new("RGB", (1120, 850), "#edf2f6")
    draw = ImageDraw.Draw(canvas)
    font_file = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / "segoeui.ttf"
    font = ImageFont.truetype(str(font_file), 19) if font_file.is_file() else ImageFont.load_default()
    draw.rounded_rectangle((20, 20, 640, 830), 18, fill="white")
    draw.rounded_rectangle((660, 20, 1100, 450), 18, fill="#101824")
    draw.text((44, 36), "FluxTop / vector logo", fill="#1a344c", font=font)
    logo = Image.open(output / "fluxtop-logo-1024.png").convert("RGBA")
    logo.thumbnail((600, 650), Image.Resampling.LANCZOS)
    canvas.paste(logo, (30, 120), logo)
    mark = Image.open(output / "fluxtop-mark-512.png").convert("RGBA")
    mark.thumbnail((400, 400), Image.Resampling.LANCZOS)
    canvas.paste(mark, (680, 35), mark)
    draw.text((680, 470), "Windows icon sizes (actual pixels)", fill="#1a344c", font=font)
    x = 680
    for size in (16, 20, 24, 32, 40, 48):
        draw.rounded_rectangle((x - 6, 520, x + size + 6, 580), 4, fill="#101824")
        canvas.paste(frames[size], (x, 526), frames[size])
        draw.text((x, 596), str(size), fill="#1a344c", font=font)
        x += size + 24
    x = 680
    for size in (64, 96, 128):
        draw.rounded_rectangle((x - 6, 660, x + size + 6, 800), 6, fill="white")
        canvas.paste(frames[size], (x, 666), frames[size])
        draw.text((x, 812), str(size), fill="#1a344c", font=font)
        x += size + 28
    canvas.save(output / "branding-preview.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inkscape", help="Path to inkscape.com / inkscape")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "out" / "branding")
    args = parser.parse_args()
    inkscape = args.inkscape or shutil.which("inkscape.com") or shutil.which("inkscape")
    if not inkscape:
        bundled = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Inkscape" / "bin" / "inkscape.com"
        if bundled.is_file():
            inkscape = str(bundled)
    if not inkscape:
        parser.error("Inkscape CLI not found; specify --inkscape")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    frames = {}
    for size in SIZES:
        source = BRAND / ("fluxtop-mark-small.svg" if size <= 48 else "fluxtop-mark.svg")
        target = output / f"fluxtop-{size}.png"
        render(inkscape, source, target, size)
        frames[size] = Image.open(target).convert("RGBA")
        if frames[size].size != (size, size):
            raise RuntimeError(f"Incorrect icon dimensions: {target.name}")
    render(inkscape, BRAND / "fluxtop-logo.svg", output / "fluxtop-logo-1024.png", 1024)
    render(inkscape, BRAND / "fluxtop-mark.svg", output / "fluxtop-mark-512.png", 512)
    ico = BRAND / "fluxtop.ico"
    write_ico(frames, ico)
    with Image.open(ico) as check:
        if check.ico.sizes() != {(size, size) for size in SIZES}:
            raise RuntimeError("ICO directory validation failed")
    preview(output, frames)
    print(f"ICO: {ico.name}, {len(SIZES)} RGBA sizes, {ico.stat().st_size:,} bytes")
    print(f"PNG exports and contact sheet: {output}")


if __name__ == "__main__":
    main()
