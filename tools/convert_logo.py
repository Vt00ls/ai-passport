#!/usr/bin/env python3
"""Convert a small logo (SVG or PNG) into an LVGL image descriptor.

Produces a C source file holding one `lv_image_dsc_t` in LV_COLOR_FORMAT_RGB565A8:
two bytes of RGB565 plus one byte of alpha per pixel. That keeps transparency over
the dashboard background, which a plain RGB565 array cannot do.

Standard library only, and PNG is decoded here rather than through an image library,
so the pipeline stays reproducible on any machine that can run the firmware gate.

    python3 tools/convert_logo.py assets/images/agent-pi-logo.svg \\
        --name dashboard_logo_pi --size 32 --output /tmp/pi_logo.c

An SVG input needs `rsvg-convert` on PATH (Homebrew: `brew install librsvg`); a PNG
input needs nothing extra. The generated file is committed, so this script only runs
when a logo changes.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


def png_to_rgba(path: Path) -> tuple[int, int, bytes]:
    """Decode an 8-bit non-interlaced PNG into straight RGBA bytes."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    pos = 8
    width = height = 0
    bit_depth = color_type = interlace = -1
    idat = bytearray()
    palette = b""
    trns = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if bit_depth != 8 or interlace != 0:
        raise ValueError(f"{path}: only 8-bit non-interlaced PNG is supported")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"{path}: unsupported colour type {color_type}")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray()
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset : offset + stride])
        offset += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            v = line[i]
            if filter_type == 1:
                v = (v + a) & 0xFF
            elif filter_type == 2:
                v = (v + b) & 0xFF
            elif filter_type == 3:
                v = (v + ((a + b) >> 1)) & 0xFF
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                v = (v + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"{path}: bad filter type {filter_type}")
            line[i] = v
        previous = line

        for x in range(width):
            pixel = line[x * channels : x * channels + channels]
            if color_type == 6:
                out += pixel
            elif color_type == 2:
                out += bytes(pixel) + b"\xff"
            elif color_type == 0:
                out += bytes((pixel[0],) * 3) + b"\xff"
            elif color_type == 4:
                out += bytes((pixel[0],) * 3) + bytes(pixel[1:2])
            elif color_type == 3:
                index = pixel[0]
                rgb = palette[index * 3 : index * 3 + 3]
                alpha = trns[index] if index < len(trns) else 0xFF
                out += bytes(rgb) + bytes((alpha,))
    return width, height, bytes(out)


def load_rgba(source: Path, size: int) -> tuple[int, int, bytes]:
    """Rasterise an SVG with rsvg-convert, or read a PNG directly."""
    if source.suffix.lower() != ".svg":
        return png_to_rgba(source)

    rsvg = shutil.which("rsvg-convert")
    if rsvg is None:
        raise SystemExit(
            "rsvg-convert is required for SVG input: brew install librsvg (macOS) "
            "or apt-get install librsvg2-bin (Debian/Ubuntu)"
        )
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / "logo.png"
        subprocess.run(
            [rsvg, "-w", str(size), "-h", str(size), "-o", str(target), str(source)],
            check=True,
        )
        return png_to_rgba(target)


def emit(name: str, width: int, height: int, rgba: bytes, output: Path) -> None:
    """Write the descriptor as LV_COLOR_FORMAT_RGB565A8."""
    colour = bytearray()
    alpha = bytearray()
    for i in range(width * height):
        r, g, b, a = rgba[i * 4 : i * 4 + 4]
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        colour += struct.pack("<H", value)
        alpha.append(a)
    payload = bytes(colour) + bytes(alpha)

    pixels = ", ".join(f"0x{byte:02x}" for byte in payload)
    text = f"""// Generated by tools/convert_logo.py from {source_name}. Do not edit by hand.
#include "app_dashboard/dashboard_logos.h"

static const uint8_t {name}_map[] = {{
    {pixels}
}};

const lv_image_dsc_t {name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565A8,
        .flags = 0,
        .w = {width},
        .h = {height},
        .stride = {width * 2},
        .reserved_2 = 0,
    }},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
"""
    output.write_text(text, encoding="utf-8")
    print(f"wrote {output} ({len(payload)} bytes of pixel data, {width}x{height})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--name", required=True, help="C identifier, e.g. dashboard_logo_pi")
    parser.add_argument("--size", type=int, default=32, help="square pixel size (default 32)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    global source_name
    source_name = args.source.name
    width, height, rgba = load_rgba(args.source, args.size)
    if (width, height) != (args.size, args.size):
        print(f"note: rasterised {width}x{height}, expected {args.size}x{args.size}", file=sys.stderr)
    emit(args.name, width, height, rgba, args.output)
    return 0


source_name = "logo"

if __name__ == "__main__":
    raise SystemExit(main())
