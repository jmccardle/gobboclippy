#!/usr/bin/env python3
"""Render assets/*.svg to PNG.

Picks whichever SVG rasteriser is installed and reports which one it used.
Errors out if none is available -- it will not emit a placeholder.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"


def renderers(src: Path, dst: Path, size: int):
    """Candidate command lines, in order of output quality."""
    yield "rsvg-convert", ["rsvg-convert", "-w", str(size), "-h", str(size),
                           "-o", str(dst), str(src)]
    yield "inkscape", ["inkscape", str(src), "--export-type=png",
                       f"--export-filename={dst}",
                       f"--export-width={size}", f"--export-height={size}"]
    yield "convert", ["convert", "-background", "none", "-density", "384",
                      str(src), "-resize", f"{size}x{size}", str(dst)]


def render(src: Path, dst: Path, size: int) -> str:
    tried = []
    for name, cmd in renderers(src, dst, size):
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
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 256
    svgs = sorted(ASSETS.glob("*.svg"))
    if not svgs:
        raise SystemExit(f"No .svg files in {ASSETS}")
    for src in svgs:
        dst = src.with_suffix(".png")
        used = render(src, dst, size)
        print(f"{src.name} -> {dst.name}  {size}x{size}  via {used}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
