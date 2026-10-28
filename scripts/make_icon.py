#!/usr/bin/env python3
"""Write the EditForthPWA icons. Stdlib only."""
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1] / "web"

# 5x7 glyphs. '#' is ink.
GLYPHS = {
    "E": [
        "#####",
        "#....",
        "#....",
        "####.",
        "#....",
        "#....",
        "#####",
    ],
    "F": [
        "#####",
        "#....",
        "#....",
        "####.",
        "#....",
        "#....",
        "#....",
    ],
}

BG = (18, 22, 28)
INK = (224, 177, 90)
FRAME = (42, 51, 64)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, size):
    raw = bytearray()
    margin = size // 16
    for y in range(size):
        raw.append(0)
        for x in range(size):
            edge = (
                margin <= x < size - margin
                and margin <= y < size - margin
                and (
                    x < margin + size // 48
                    or y < margin + size // 48
                    or x >= size - margin - size // 48
                    or y >= size - margin - size // 48
                )
            )
            color = FRAME if edge else BG
            raw.extend(color)
    # Paint EF centered. Each glyph pixel is scale x scale.
    scale = max(1, size // 28)
    gap = scale
    gw, gh = 5 * scale, 7 * scale
    block_w = gw * 2 + gap
    x0 = (size - block_w) // 2
    y0 = (size - gh) // 2
    for gi, ch in enumerate("EF"):
        ox = x0 + gi * (gw + gap)
        for row, bits in enumerate(GLYPHS[ch]):
            for col, bit in enumerate(bits):
                if bit != "#":
                    continue
                px = ox + col * scale
                py = y0 + row * scale
                for dy in range(scale):
                    for dx in range(scale):
                        i = (py + dy) * (1 + size * 3) + 1 + (px + dx) * 3
                        raw[i : i + 3] = bytes(INK)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    write_png(ROOT / "icon-192.png", 192)
    write_png(ROOT / "icon-512.png", 512)


if __name__ == "__main__":
    main()
