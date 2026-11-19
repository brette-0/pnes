#!/usr/bin/env python3
"""Pack raw RGBA pixels into a GameCube memory-card CI8 image + RGB5A3 palette.

WHAT THE SYSTEM MENU ACTUALLY READS is not a PNG and not linear pixels: a
card save's banner and icon are GX TEXTURES, stored in the file exactly as the
hardware samples them. Two things follow, and both are silent corruption rather
than an error if skipped:

  - CI8 IS TILED, in 8x4 blocks. The bytes are tile-major (every tile's 32
    indices in row order, then the next tile to the right), NOT one image row
    after another. A linear dump renders as a recognisable-but-shredded image,
    which is why this is worth saying twice.
  - THE PALETTE IS RGB5A3, BIG-ENDIAN, 256 entries ALWAYS (512 bytes). Short
    palettes are padded with zeroes; the menu indexes it by the raw byte, so a
    truncated table is a read past the data.

The blob this writes is therefore `pixels || palette`, which is the layout the
OGC_BANNER_CI / OGC_ICON_CI arrays in the application's save struct declare:
96x32 -> 3072 + 512 = 3584, and 32x32 -> 1024 + 512 = 1536.

ONE ICON FRAME ONLY. A CI icon may hold up to CARD_MAXICONS frames sharing one
TLUT, and this writes a single frame, so the icon is static: the animation
speed bits past frame 0 stay CARD_SPEED_END and the directory entry's bounce
bit is never set. Multi-frame support is deliberately absent rather than
unfinished -- see the CMakeLists.txt block that calls this.

Input is RAW RGBA8 (ImageMagick's `RGBA:` writer), not a PNG: decoding image
formats is ImageMagick's job, and it is already a build dependency for the Wii
U branch's artwork. It is also what caps the colour count -- `-colors 256`
upstream of here -- so this script never has to quantise. It refuses a source
that still needs it rather than silently dropping colours.
"""

import argparse
import sys

# Alpha this high is stored as opaque (the RGB5A3 "5-bit" half, which has no
# alpha bits at all). 224 is the midpoint of the top 3-bit alpha step: a pixel
# rounding to a=7 loses nothing by becoming fully opaque, and gains two bits of
# colour depth per channel.
OPAQUE_ALPHA_FLOOR = 224

PALETTE_ENTRIES = 256
TILE_W, TILE_H = 8, 4


def rgb5a3(r: int, g: int, b: int, a: int) -> int:
    """One RGBA8 pixel as an RGB5A3 half-word."""
    if a >= OPAQUE_ALPHA_FLOOR:
        # Bit 15 set: no alpha, 5 bits per channel.
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    # Bit 15 clear: 3 bits of alpha, 4 bits per channel.
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rgba", required=True, help="raw RGBA8 input (ImageMagick RGBA: output)")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--out", required=True, help="CI8 pixels + 512-byte RGB5A3 palette")
    ap.add_argument("--label", default="image", help="name used in messages")
    args = ap.parse_args()

    w, h = args.width, args.height
    if w % TILE_W or h % TILE_H:
        sys.stderr.write(f"{args.label}: {w}x{h} is not a whole number of "
                         f"{TILE_W}x{TILE_H} CI8 tiles\n")
        return 1

    with open(args.rgba, "rb") as f:
        raw = f.read()
    want = w * h * 4
    if len(raw) != want:
        sys.stderr.write(f"{args.label}: expected {want} bytes of RGBA8 for {w}x{h}, "
                         f"got {len(raw)}\n")
        return 1

    # Encode first, index second: two RGBA8 colours that collapse to the same
    # RGB5A3 half-word must share one palette entry, or a 200-colour source can
    # overflow 256 entries that it does not actually need.
    pixels = [rgb5a3(raw[i], raw[i + 1], raw[i + 2], raw[i + 3])
              for i in range(0, len(raw), 4)]

    # First-appearance order, scanning in tile order below would work equally
    # well -- what matters is that it is deterministic, so an unchanged source
    # produces an unchanged blob and does not retrigger a compile.
    palette: dict[int, int] = {}
    for px in pixels:
        if px not in palette:
            palette[px] = len(palette)
    if len(palette) > PALETTE_ENTRIES:
        sys.stderr.write(f"{args.label}: {len(palette)} distinct RGB5A3 colours, "
                         f"CI8 holds {PALETTE_ENTRIES} -- quantise the source "
                         f"further (the build passes -colors {PALETTE_ENTRIES})\n")
        return 1

    indices = bytearray(w * h)
    at = 0
    for ty in range(0, h, TILE_H):
        for tx in range(0, w, TILE_W):
            for y in range(ty, ty + TILE_H):
                row = y * w
                for x in range(tx, tx + TILE_W):
                    indices[at] = palette[pixels[row + x]]
                    at += 1

    table = bytearray(PALETTE_ENTRIES * 2)
    for colour, index in palette.items():
        table[index * 2] = colour >> 8        # big-endian: the console's order
        table[index * 2 + 1] = colour & 0xFF

    blob = bytes(indices) + bytes(table)
    with open(args.out, "wb") as f:
        f.write(blob)

    print(f"{args.label}: {w}x{h} CI8, {len(palette)} colours, {len(blob)} bytes "
          f"({len(indices)} pixels + {len(table)} palette)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
