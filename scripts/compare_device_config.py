#!/usr/bin/env python3
"""Compare native and production-config LVGL host renders without touching standard previews.

Run from any directory with the already-present SenseCAP SDK in .deps. Captures and
temporary executables use device-config-* names under build/previews.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import subprocess
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
LVGL = ROOT / ".deps" / "sensecap-sdk" / "components" / "lvgl"
NATIVE = ROOT / "scripts" / "native"
OUTPUT = ROOT / "build" / "previews"
WIDTH = HEIGHT = 480


def png_chunk(kind: bytes, data: bytes) -> bytes:
    import binascii

    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)


def ppm_to_png(source: pathlib.Path, target: pathlib.Path) -> bytes:
    data = source.read_bytes()
    match = re.match(rb"P6\s+480\s+480\s+255\s", data)
    if not match or len(data) != match.end() + WIDTH * HEIGHT * 3:
        raise ValueError(f"Unexpected PPM data: {source}")
    pixels = data[match.end():]
    rows = b"".join(b"\0" + pixels[y * WIDTH * 3:(y + 1) * WIDTH * 3] for y in range(HEIGHT))
    target.write_bytes(b"\x89PNG\r\n\x1a\n" +
                       png_chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                       png_chunk(b"IDAT", zlib.compress(rows, 9)) + png_chunk(b"IEND", b""))
    return pixels


def sources() -> list[pathlib.Path]:
    app = ROOT / "main"
    return sorted((LVGL / "src").rglob("*.c")) + [
        app / "display.c", app / "display_format.c", app / "word_clock.c", app / "box_config.c",
        app / "usage_provider.c",
        app / "app_registry.c", app / "screensaver_registry.c", app / "ui_shell.c",
        app / "usage_app_ui.c",
        app / "usage_heading_font.c",
        app / "usage_values_font.c", app / "usage_ui_font.c",
        app / "usage_status_font.c", app / "usage_meta_font.c", NATIVE / "renderer.c",
        app / "usage_hero_font.c",
    ]


def compile_renderer(exe: pathlib.Path, config_include: pathlib.Path | None) -> None:
    args = ["clang", "-std=gnu11", "-O1", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE"]
    if config_include:
        args += ["-I", str(config_include), "-I", str(ROOT / "build" / "config")]
    args += ["-I", str(NATIVE), "-I", str(ROOT / "main"), "-I", str(LVGL)]
    if config_include:
        args += ["-include", str(ROOT / "build" / "config" / "sdkconfig.h")]
    args += [*map(str, sources()), "-lm", "-o", str(exe)]
    subprocess.run(args, cwd=ROOT, check=True)


def render(exe: pathlib.Path, scene: str, stem: str) -> bytes:
    ppm = OUTPUT / f"{stem}.ppm"
    png = OUTPUT / f"{stem}.png"
    subprocess.run([str(exe), scene, str(ppm)], cwd=ROOT, check=True)
    pixels = ppm_to_png(ppm, png)
    ppm.unlink()
    print(png.relative_to(ROOT))
    return pixels


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenes", nargs="*", default=["usage"],
                        help="renderer scenes (default: usage)")
    args = parser.parse_args()
    if not (LVGL / "lvgl.h").is_file() or not (ROOT / "build/config/sdkconfig.h").is_file():
        raise SystemExit("Existing LVGL SDK/build config absent; this script does not fetch dependencies or build firmware")
    OUTPUT.mkdir(parents=True, exist_ok=True)

    # Build in temporary config storage and use distinct executable names, never .deps/native-ui.
    with tempfile.TemporaryDirectory(prefix="lvgl-device-config-") as temp:
        temp_path = pathlib.Path(temp)
        device_config = temp_path / "device-config"
        device_config.mkdir()
        (device_config / "lv_conf.h").write_text("#ifndef LV_CONF_H\n#define LV_CONF_H\n#endif\n")
        native_exe = OUTPUT / "device-config-native-renderer"
        device_exe = OUTPUT / "device-config-device-renderer"
        compile_renderer(native_exe, None)
        compile_renderer(device_exe, device_config)
        for scene in args.scenes:
            native = render(native_exe, scene, f"device-config-native-{scene}")
            device = render(device_exe, scene, f"device-config-device-{scene}")
            changed = 0
            total_delta = 0
            max_delta = 0
            for i in range(0, len(native), 3):
                deltas = [abs(native[i + channel] - device[i + channel]) for channel in range(3)]
                if any(deltas):
                    changed += 1
                total_delta += sum(deltas)
                max_delta = max(max_delta, *deltas)
            print(f"{scene}: {changed}/{WIDTH * HEIGHT} pixels differ; "
                  f"mean absolute channel delta {total_delta / (WIDTH * HEIGHT * 3):.4f}; "
                  f"maximum channel delta {max_delta}")
        native_exe.unlink(missing_ok=True)
        device_exe.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
