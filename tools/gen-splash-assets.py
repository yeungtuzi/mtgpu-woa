#!/usr/bin/env python3
"""Generate the aibook splash images from the 512px rabbit icon.

The animation only lights up the coloured frame; the black centre and the white
rabbit (including the ear tip that reaches into the top-right corner) stay as
they are.  A saturation mask extracts the coloured frame, which is then split
into four quadrant sprites laid over a grey copy of the icon.

Usage: python3 tools/gen-splash-assets.py [size]
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "share" / "icons" / "mtgpu-woa-512.png"
OUT = ROOT / "share" / "plasma" / "look-and-feel" / "org.aibook.splash" / "contents" / "splash" / "images"
SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 720
QUADS = (("tl", "+0+0", "northwest"), ("tr", "+{h}+0", "northeast"),
         ("bl", "+0+{h}", "southwest"), ("br", "+{h}+{h}", "southeast"))


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    icon = OUT / "icon.png"
    run("convert", str(SRC), "-resize", f"{SIZE}x{SIZE}", str(icon))
    run("convert", str(icon), "-colorspace", "Gray", "-colorspace", "sRGB",
        str(OUT / "icon-gray.png"))

    sat = OUT / ".sat.png"
    run("convert", str(icon), "-colorspace", "HSL", "-channel", "G",
        "-separate", "+channel", "-blur", "0x1", str(sat))
    frame = OUT / ".frame.png"
    run("convert", str(icon), "-colorspace", "sRGB", str(sat), "-alpha", "off",
        "-compose", "CopyOpacity", "-composite", str(frame))

    # Every sprite keeps the full icon size so the QML can simply stack them
    # (anchors.fill) without any offset/scaling maths.
    h = SIZE // 2
    for name, geom, gravity in QUADS:
        run("convert", str(frame), "-crop", f"{h}x{h}{geom.format(h=h)}",
            "+repage", "-background", "none", "-gravity", gravity,
            "-extent", f"{SIZE}x{SIZE}", str(OUT / f"q-{name}.png"))

    sat.unlink()
    frame.unlink()
    icon.unlink()
    print(f"wrote {OUT} ({SIZE}px)")


if __name__ == "__main__":
    main()
