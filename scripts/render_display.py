#!/usr/bin/env python3
"""Compile the production LVGL 8 display and capture real 480x480 RGB565 renders.

Run `python3 scripts/render_display.py` from any directory. Requires host clang and
the SDK installed by scripts/fetch-sdk.sh. No device, SDL, or Pillow is needed.
"""

from __future__ import annotations

import argparse
import binascii
import pathlib
import re
import struct
import subprocess
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
NATIVE = ROOT / "scripts" / "native"
LVGL = ROOT / ".deps" / "sensecap-sdk" / "components" / "lvgl"
EXE = ROOT / ".deps" / "native-ui"
OUTPUT = ROOT / "build" / "previews"
SCENES = ("usage", "usage-full", "unknown", "offline", "setup", "updating", "error", "disconnected",
          "face-refresh", "boot", "boot-slow", "boot-interrupt", "mascot-motion",
          "mascot-pause-setup", "mascot-pause-error", "mascot-pause-offline",
          "mascot-pause-updating", "mascot-pause-fullface", "mascot-pause-stale",
          "mascot-pause-tap")
FACE_FRAMES = ("open", "both-closed", "wink", "done")
BOOT_FRAMES = ("boot-open", "boot-status", "boot-done")
MASCOT_FRAMES = ("rest", *(f"shuffle-{phase:02d}" for phase in range(1, 10)),
                 "bob", "return", "return-two")


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)


def ppm_to_png(source: pathlib.Path, target: pathlib.Path) -> None:
    data = source.read_bytes()
    match = re.match(rb"P6\s+480\s+480\s+255\s", data)
    if not match or len(data) != match.end() + 480 * 480 * 3:
        raise ValueError(f"Unexpected PPM data: {source}")
    pixels = data[match.end():]
    scanlines = b"".join(b"\0" + pixels[y * 1440:(y + 1) * 1440] for y in range(480))
    target.write_bytes(b"\x89PNG\r\n\x1a\n" +
                       chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0)) +
                       chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))


def build(sanitize: bool) -> None:
    if not (LVGL / "lvgl.h").exists():
        raise SystemExit("LVGL SDK absent; run scripts/fetch-sdk.sh first")
    sources = sorted((LVGL / "src").rglob("*.c"))
    sources += [ROOT / "main" / "display.c", ROOT / "main" / "display_format.c",
                ROOT / "main" / "usage_heading_font.c", ROOT / "main" / "usage_values_font.c",
                ROOT / "main" / "usage_ui_font.c", ROOT / "main" / "usage_status_font.c",
                NATIVE / "renderer.c"]
    cmd = ["clang", "-std=gnu11", "-O1", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE",
           "-I", str(NATIVE), "-I", str(ROOT / "main"), "-I", str(LVGL),
           *map(str, sources), "-lm", "-o", str(EXE)]
    if sanitize:
        cmd[1:1] = ["-g", "-fsanitize=address,undefined"]
    print(f"Compiling {len(sources)} production/LVGL C sources to {EXE}", flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenes", nargs="*", metavar="SCENE", help=f"one of: {', '.join(SCENES)}")
    parser.add_argument("--no-build", action="store_true", help="Reuse the compiled native-ui executable")
    parser.add_argument("--sanitize", action="store_true", help="Build with address and undefined behavior sanitizers")
    args = parser.parse_args()
    for scene in args.scenes:
        if scene not in SCENES:
            parser.error(f"unknown scene {scene!r}; choose from {', '.join(SCENES)}")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if not args.no_build:
        build(args.sanitize)
    for scene in args.scenes or SCENES:
        if scene == "mascot-motion":
            base = OUTPUT / "mascot-motion"
            subprocess.run([str(EXE), scene, f"{base}.ppm"], check=True, cwd=ROOT)
            for phase in MASCOT_FRAMES:
                ppm = OUTPUT / f"mascot-motion-{phase}.ppm"
                if not ppm.exists():
                    continue
                png = OUTPUT / f"mascot-motion-{phase}.png"
                ppm_to_png(ppm, png)
                ppm.unlink()
                print(png.relative_to(ROOT))
            continue
        if scene.startswith("mascot-pause-"):
            ppm = OUTPUT / f"{scene}.ppm"
            subprocess.run([str(EXE), scene, str(ppm)], check=True, cwd=ROOT)
            for phase in ("paused-start", "paused-end"):
                phase_ppm = OUTPUT / f"{scene}-{phase}.ppm"
                phase_png = OUTPUT / f"{scene}-{phase.removeprefix('paused-')}.png"
                ppm_to_png(phase_ppm, phase_png)
                phase_ppm.unlink()
                print(phase_png.relative_to(ROOT))
            ppm.unlink()
            continue
        if scene == "face-refresh":
            base = OUTPUT / "face"
            subprocess.run([str(EXE), scene, f"{base}.ppm"], check=True, cwd=ROOT)
            for phase in FACE_FRAMES:
                ppm = OUTPUT / f"face-{phase}.ppm"
                png = OUTPUT / f"face-{phase}.png"
                ppm_to_png(ppm, png)
                ppm.unlink()
                print(png.relative_to(ROOT))
            continue
        if scene == "boot":
            base = OUTPUT / "boot"
            subprocess.run([str(EXE), scene, f"{base}.ppm"], check=True, cwd=ROOT)
            for phase in BOOT_FRAMES:
                ppm = OUTPUT / f"{phase}.ppm"
                png = OUTPUT / f"{phase}.png"
                ppm_to_png(ppm, png)
                ppm.unlink()
                print(png.relative_to(ROOT))
            continue
        ppm = OUTPUT / f"{scene}.ppm"
        png = OUTPUT / f"{scene}.png"
        subprocess.run([str(EXE), scene, str(ppm)], check=True, cwd=ROOT)
        ppm_to_png(ppm, png)
        ppm.unlink()
        print(png.relative_to(ROOT))


if __name__ == "__main__":
    main()
