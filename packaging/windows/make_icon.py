r"""Regenerate packaging/windows/zuuned.ico from the hicolor PNGs this repo already owns.

    python packaging\windows\make_icon.py

The sizes an .ico declares are the sizes the shell is allowed to pick, so one
512px source folded into a single entry is why a taskbar icon looks soft and a
16px file-list icon looks like noise. Every entry below is a PNG rendered from
packaging/icons/zuuned.svg at exactly that size - nothing is resampled, and
nothing is upscaled. 512 is absent because the directory field is one byte and
256, written as 0, is its ceiling.

Entries under 256 are 32-bit BGRA DIBs with a zeroed AND mask, the one shape
every shell component on every supported Windows reads; the 256 entry is the
source PNG itself, which Vista and later accept and which keeps the file at one
hundred kilobytes instead of five megabytes.

The generated file is committed so a Windows build needs no Pillow; run this
script again whenever the artwork changes, and read the result back with
the RT_GROUP_ICON check in packaging\windows\deploy.ps1.
"""
import io
import os
import struct

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]
HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(os.path.dirname(HERE), "icons", "hicolor",
                      "%dx%d", "apps", "zuuned.png")
TARGET = os.path.join(HERE, "zuuned.ico")


def dib(png_bytes):
    """Return a BITMAPINFOHEADER, BGRA rows bottom-up, and a zeroed AND mask."""
    image = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    width, height = image.size
    pixels = image.load()
    rows = bytearray()
    for y in range(height - 1, -1, -1):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            rows += bytes((b, g, r, a))
    body = bytes(rows) + bytes(((width + 31) // 32) * 4 * height)
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0,
                         len(body), 0, 0, 0, 0)
    return header + body


def main():
    images = []
    for size in SIZES:
        path = SOURCE % (size, size)
        raw = open(path, "rb").read()
        found = Image.open(io.BytesIO(raw)).size
        if found != (size, size):
            raise SystemExit("%s is %dx%d, not %dx%d"
                             % (path, found[0], found[1], size, size))
        images.append((size, raw if size == 256 else dib(raw)))

    directory, payloads, offset = bytearray(), bytearray(), 6 + 16 * len(images)
    for size, payload in images:
        field = 0 if size == 256 else size
        directory += struct.pack("<BBBBHHII", field, field, 0, 0, 1, 32,
                                 len(payload), offset)
        payloads += payload
        offset += len(payload)
    out = (struct.pack("<HHH", 0, 1, len(images)) + bytes(directory)
           + bytes(payloads))
    open(TARGET, "wb").write(out)
    print("wrote %s: %d bytes, %d entries %s"
          % (TARGET, len(out), len(images), SIZES))


main()
