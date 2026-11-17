"""Redact the identity a text gate cannot see out of the published README screenshots.

    python tests/privacy/redact_readme_screenshots.py --apply
    python tests/privacy/redact_readme_screenshots.py --check

tests/privacy/run.ps1 reads every tracked byte, so it finds an account name, a host
name or a USB serial wherever it is written, including inside a PNG.  What it cannot
read is a pixel.  docs/readme-screenshots is published deliberately from the real
library and the real player because a screenshot of a fixture library sells nothing,
and the device panel on the right of every page therefore paints the friendly name the
owner typed into the player - a person's name, in thirty pictures, on a public
repository, and invisible to a gate that only reads bytes.  The photos page also
carried one real photograph of somebody's fish tank, and Settings -> Library printed
four watch-folder paths in plain text.

So the identity is covered rather than deleted, and covered with a block mosaic
because that is a shape a gate can test.  A mosaic region is one where every 8x8 block
is exactly one colour, which is checkable without knowing what the picture used to
say, so --check fails when somebody re-takes a screenshot and paints a name, a path or
a photograph back over the mask.  A seventeenth page is refused for the same reason: a
new screenshot has to be declared here before the gate is allowed to believe it.

--apply rewrites the PNGs in place, is idempotent, and reports how many pixels each
region actually lost, so a region declared over empty background says so out loud
instead of pretending to be a mask.  Nothing outside docs/readme-screenshots is
touched.
"""
import argparse
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHOTS = os.path.join(ROOT, "docs", "readme-screenshots")
BLOCK = 8

# The friendly-name line of the device panel.  The band is as wide as the panel is at
# that height, so a longer name has to run into the mask rather than sneak past it.
DEVICE_NAME = (1440, 152, 1680, 192)
# The one real photograph standing in for a photo folder on the photos page.
PHOTO_CARD = (328, 168, 520, 360)
# One band per watch-folder row, tall enough for the path and short enough to leave the
# kind badge and the track count readable, because those are what the page is showing.
WATCH_ROWS = [
    (384, 728, 704, 752),
    (384, 776, 704, 800),
    (384, 824, 704, 848),
    (384, 872, 704, 896),
]

# Every page the README shows, in English and in Chinese.  "welcome" is the one page
# with no device panel at all, so declaring a mask on it would be a mask over nothing.
PAGES = [
    "audiobooks",
    "device-albums",
    "device-music",
    "music-artists",
    "music-library",
    "music-songs",
    "now-playing",
    "photo-library",
    "podcasts",
    "settings-appearance",
    "settings-device",
    "settings-health",
    "settings-library",
    "settings-mixtapes",
    "video-library",
]
EXEMPT = {"welcome.png", os.path.join("zh", "welcome.png")}


def declared():
    table = {}
    for name in PAGES:
        for rel in (name + ".png", os.path.join("zh", name + ".png")):
            regions = [("device name", DEVICE_NAME)]
            if name == "photo-library":
                regions.append(("photograph on the folder card", PHOTO_CARD))
            if name == "settings-library":
                for i, box in enumerate(WATCH_ROWS, start=1):
                    regions.append(("watch folder path row %d" % i, box))
            table[rel] = regions
    return table


def on_disk():
    found = []
    for folder, _dirs, names in os.walk(SHOTS):
        for n in sorted(names):
            if n.lower().endswith(".png"):
                found.append(os.path.relpath(os.path.join(folder, n), SHOTS))
    return sorted(found)


def blocks(im, box):
    x0, y0, x1, y1 = box
    for by in range(y0, y1, BLOCK):
        for bx in range(x0, x1, BLOCK):
            yield im.crop((bx, by, bx + BLOCK, by + BLOCK))


def pixels(block):
    """The block as RGB triples, read straight out of the encoded row bytes.

    Pillow deprecates Image.getdata() on the way to a rewrite, and this file is a
    gate: a warning on every run is how a real line later gets lost in the noise.
    """
    raw = block.tobytes()
    return [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]


def flat_pixels(block):
    raw = block.tobytes()
    stride = BLOCK * 3
    return raw == raw[:stride] * (len(raw) // stride)


def check_box(box, size, rel, label, problems):
    x0, y0, x1, y1 = box
    if x1 <= x0 or y1 <= y0:
        problems.append("%s :: %s names an empty box" % (rel, label))
        return False
    if (x1 - x0) % BLOCK or (y1 - y0) % BLOCK:
        problems.append("%s :: %s is not a whole number of %d-pixel blocks" % (rel, label, BLOCK))
        return False
    if x1 > size[0] or y1 > size[1]:
        problems.append("%s :: %s falls outside a %dx%d picture" % (rel, label, size[0], size[1]))
        return False
    return True


def check():
    problems = []
    table = declared()
    masked = 0
    for rel in on_disk():
        if rel not in table:
            if rel not in EXEMPT:
                problems.append("%s is a screenshot nobody declared" % rel)
            continue
        path = os.path.join(SHOTS, rel)
        with Image.open(path) as opened:
            im = opened.convert("RGB")
            for label, box in table[rel]:
                if not check_box(box, im.size, rel, label, problems):
                    continue
                loose = [i for i, b in enumerate(blocks(im, box)) if not flat_pixels(b)]
                if loose:
                    problems.append("%s :: %s still has %d block(s) with detail in it" %
                                    (rel, label, len(loose)))
                else:
                    masked += 1
    print("masked=%d notmasked=%d undeclared=%d problems=%d" %
          (masked, len([1 for r in table for _ in table[r]]) - masked,
           len([r for r in on_disk() if r not in table and r not in EXEMPT]), len(problems)))
    for p in problems:
        print("  leak  " + p)
    return 1 if problems else 0


def apply_masks():
    problems = []
    table = declared()
    rewrote = 0
    lost = 0
    for rel in on_disk():
        if rel not in table:
            continue
        path = os.path.join(SHOTS, rel)
        with Image.open(path) as opened:
            im = opened.convert("RGB")
            bad = [b for _l, b in table[rel] if not check_box(b, im.size, rel, "mask", problems)]
            if bad:
                continue
            put = im.load()
            here = 0
            for label, box in table[rel]:
                x0, y0, x1, y1 = box
                region = 0
                for by in range(y0, y1, BLOCK):
                    for bx in range(x0, x1, BLOCK):
                        block = im.crop((bx, by, bx + BLOCK, by + BLOCK))
                        data = pixels(block)
                        if len(set(data)) == 1:
                            continue
                        # Count the detail being destroyed, then flatten the block onto
                        # its own mean so the surrounding picture keeps its lighting.
                        region += len(data)
                        mean = tuple(sum(c[i] for c in data) // len(data) for i in range(3))
                        for dy in range(BLOCK):
                            for dx in range(BLOCK):
                                put[bx + dx, by + dy] = mean
                print("  masked %-34s %-34s %6d pixel(s) of detail" % (rel, label, region))
                here += region
            if here:
                im.save(path)
                rewrote += 1
            lost += here
    for p in problems:
        print("  leak  " + p)
    print("rewrote=%d lost=%d problems=%d" % (rewrote, lost, len(problems)))
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="fail if a mask is missing or broken")
    mode.add_argument("--apply", action="store_true", help="mosaic every declared region")
    args = ap.parse_args()
    return check() if args.check else apply_masks()


if __name__ == "__main__":
    sys.exit(main())
