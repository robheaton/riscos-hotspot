#!/usr/bin/env python3
"""
mksprites.py - draws the Hotspot application icon and writes it as a RISC OS
sprite file (!Sprites).

    mksprites.py OUTPUT [--preview PNG]

The sprite is one 34x34 pixel, 32 bits per pixel, 90 dpi sprite called
"!hotspot": the name the Filer uses for the application's directory icon and
the name the icon bar icon asks for. It has no mask (an opaque rounded
badge), which sidesteps the mask format differences between sprite types.

Format (RISC OS PRM, sprites): a saved sprite file is the sprite area
without its leading size word -

    +0   number of sprites
    +4   offset to the first sprite (counted as in memory, so 16)
    +8   offset to the first free word
    +12  the sprites

and each sprite is a 44 byte header followed by its pixels:

    +0   offset to the next sprite
    +4   name (12 bytes, NUL padded)
    +16  width in words - 1
    +20  height in scan lines - 1
    +24  first bit used in the first word of a row
    +28  last bit used in the last word of a row
    +32  offset to the image
    +36  offset to the mask (same as the image offset when there is none)
    +40  mode word: 0x301680B5 = type 6 (32bpp, 0xBBGGRR), 90 x 90 dpi

Uses Pillow for the drawing; --preview renders the sprite *back out of the
written file* so what is checked is the file, not the drawing code.
"""

import argparse
import math
import struct
import sys

from PIL import Image, ImageDraw

W = H = 34
MODE_WORD = 0x301680B5
SS = 8                      # supersampling factor for smooth edges


def draw_icon():
    s = W * SS
    img = Image.new("RGB", (s, s), (0, 0, 0))
    px = img.load()

    # Vertical gradient badge.
    top = (46, 120, 190)
    bot = (18, 52, 96)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(s):
            px[x, y] = c

    # Round the corners: paint everything outside the badge with the
    # background colour the Wimp will show behind a filer icon (light grey).
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, s - 1, s - 1), radius=7 * SS, fill=255)
    bg = Image.new("RGB", (s, s), (221, 221, 221))
    img = Image.composite(img, bg, mask)

    d = ImageDraw.Draw(img)
    white = (255, 255, 255)
    pale = (190, 225, 255)

    cx = 17 * SS
    cy = 13 * SS

    # Base station.
    d.rounded_rectangle((10 * SS, 25 * SS, 24 * SS, 30 * SS),
                        radius=2 * SS, fill=white)
    d.rectangle((16 * SS, cy, 18 * SS, 25 * SS), fill=white)
    r = int(2.6 * SS)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=white)

    # Radio waves either side of the mast.
    for i, radius in enumerate((6, 10, 14)):
        box = (cx - radius * SS, cy - radius * SS,
               cx + radius * SS, cy + radius * SS)
        colour = white if i == 0 else pale
        width = int(1.7 * SS)
        d.arc(box, -48, 48, fill=colour, width=width)
        d.arc(box, 132, 228, fill=colour, width=width)

    return img.resize((W, H), Image.LANCZOS)


def sprite_file(img):
    name = b"!hotspot".ljust(12, b"\0")
    pixels = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = img.getpixel((x, y))
            pixels += struct.pack("<I", (b << 16) | (g << 8) | r)

    size = 44 + len(pixels)
    header = struct.pack(
        "<I12sIIIIIII",
        size,               # next sprite
        name,
        W - 1,              # width in words - 1 (one word per pixel)
        H - 1,
        0,                  # first bit used
        31,                 # last bit used
        44,                 # image offset
        44,                 # mask offset (== image: no mask)
        MODE_WORD,
    )
    assert len(header) == 44

    area = struct.pack("<III", 1, 16, 16 + size)
    return area + header + bytes(pixels)


def read_back(data):
    """Parse a sprite file and return (name, width, height, RGB image)."""
    count, first, free = struct.unpack_from("<III", data, 0)
    assert count == 1, count
    assert first == 16, first
    base = first - 4
    (nxt, name, wm1, hm1, first_bit, last_bit, img_off, mask_off,
     mode) = struct.unpack_from("<I12sIIIIIII", data, base)
    assert free == 16 + nxt, (free, nxt)
    assert mode == MODE_WORD, hex(mode)
    assert img_off == mask_off == 44
    assert first_bit == 0 and last_bit == 31
    w, h = wm1 + 1, hm1 + 1
    assert base + nxt == len(data), (base, nxt, len(data))
    out = Image.new("RGB", (w, h))
    pos = base + img_off
    for y in range(h):
        for x in range(w):
            (word,) = struct.unpack_from("<I", data, pos)
            pos += 4
            out.putpixel((x, y), (word & 255, (word >> 8) & 255,
                                  (word >> 16) & 255))
    return name.rstrip(b"\0").decode(), w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--preview")
    args = ap.parse_args()

    data = sprite_file(draw_icon())
    with open(args.output, "wb") as f:
        f.write(data)

    name, w, h, img = read_back(data)
    assert (name, w, h) == ("!hotspot", W, H)

    if args.preview:
        # Show it at 8x on the grey a Filer window would put behind it.
        big = img.resize((W * 8, H * 8), Image.NEAREST)
        big.save(args.preview)

    print("wrote %s: sprite %s %dx%d, %d bytes" % (args.output, name, w, h,
                                                  len(data)))


if __name__ == "__main__":
    main()
