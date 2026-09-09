#!/usr/bin/env python3
"""Render assets/*.svg to PNG at the size each SVG declares.

Every SVG here is authored at its final pixel size -- a sprite strip is as wide
as its frames, not square -- so the renderer reads width and height from the
file rather than being told one number. Pass a scale factor to render at a
multiple of that for a HiDPI variant.

Picks whichever SVG rasteriser is installed and reports which one it used.
Errors out if none is available -- it will not emit a placeholder.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"


def declared_size(src: Path) -> tuple[int, int]:
    """The <svg> element's own width and height, in pixels.

    Refuses to guess. An SVG with no width/height (or one given in physical
    units) has no single right answer at raster time, and picking one silently
    is how a sprite strip ends up half a frame wide.
    """
    head = src.read_text(encoding="utf-8")[:2048]
    got = {}
    for attr in ("width", "height"):
        m = re.search(rf'<svg\b[^>]*?\b{attr}\s*=\s*"([^"]+)"', head, re.S)
        if not m:
            raise SystemExit(f"{src.name}: no {attr} on the <svg> element")
        value = m.group(1).strip().removesuffix("px")
        try:
            got[attr] = int(round(float(value)))
        except ValueError:
            raise SystemExit(
                f"{src.name}: {attr}='{m.group(1)}' is not a pixel count"
            ) from None
    return got["width"], got["height"]


def renderers(src: Path, dst: Path, w: int, h: int):
    """Candidate command lines, in order of output quality."""
    yield "rsvg-convert", ["rsvg-convert", "-w", str(w), "-h", str(h),
                           "-o", str(dst), str(src)]
    yield "inkscape", ["inkscape", str(src), "--export-type=png",
                       f"--export-filename={dst}",
                       f"--export-width={w}", f"--export-height={h}"]
    yield "convert", ["convert", "-background", "none", "-density", "384",
                      str(src), "-resize", f"{w}x{h}!", str(dst)]


def render(src: Path, dst: Path, w: int, h: int) -> str:
    tried = []
    for name, cmd in renderers(src, dst, w, h):
        if not shutil.which(cmd[0]):
            tried.append(f"{name} (not installed)")
            continue
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode == 0 and dst.exists() and dst.stat().st_size > 0:
            return name
        tried.append(f"{name} (exit {proc.returncode}: {proc.stderr.strip()[:120]})")
    raise SystemExit(
        f"No working SVG rasteriser for {src.name}. Tried:\n  " + "\n  ".join(tried)
        + "\nInstall one of: librsvg2-bin, inkscape, imagemagick"
    )


def main() -> int:
    scale = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    if scale <= 0:
        raise SystemExit("scale must be positive")

    svgs = sorted(ASSETS.glob("*.svg"))
    if not svgs:
        raise SystemExit(f"No .svg files in {ASSETS}")

    for src in svgs:
        w, h = declared_size(src)
        w, h = int(round(w * scale)), int(round(h * scale))
        dst = src.with_suffix(".png")
        used = render(src, dst, w, h)
        print(f"{src.name} -> {dst.name}  {w}x{h}  via {used}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
