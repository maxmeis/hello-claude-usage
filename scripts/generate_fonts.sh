#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .deps/fonts licenses
# Use reviewed local sources when present; a fresh checkout can still fetch them.
test -f .deps/fonts/usage-heading.ttf || curl -fLsS https://raw.githubusercontent.com/google/fonts/main/ofl/breeserif/BreeSerif-Regular.ttf -o .deps/fonts/usage-heading.ttf
test -f .deps/fonts/usage-values.ttf || curl -fLsS https://raw.githubusercontent.com/google/fonts/main/ofl/lato/Lato-Bold.ttf -o .deps/fonts/usage-values.ttf
test -f licenses/usage-heading-OFL.txt || curl -fLsS https://raw.githubusercontent.com/google/fonts/main/ofl/breeserif/OFL.txt -o licenses/usage-heading-OFL.txt
test -f licenses/usage-values-OFL.txt || curl -fLsS https://raw.githubusercontent.com/google/fonts/main/ofl/lato/OFL.txt -o licenses/usage-values-OFL.txt
python3 - <<'PY'
import hashlib
from pathlib import Path
for name, expected in {
    'usage-heading': 'fd080582d70112ef479ba4ddd72407f9a4fcbcec4caa7d29de259b13eeb5f431',
    'usage-values': '8a0aace75d33794eece4b28187bfc1df0bbd2888b5d8a56e01788c8d65d16be1',
}.items():
    if hashlib.sha256(Path(f'.deps/fonts/{name}.ttf').read_bytes()).hexdigest() != expected:
        raise SystemExit(f'{name} source changed; review the new font before conversion')
PY
pnpm --store-dir .deps/pnpm-store dlx lv_font_conv@1.5.3 --font .deps/fonts/usage-heading.ttf --size 40 --bpp 4 --format lvgl --symbols Usage --no-compress --lv-font-name usage_heading_font -o main/usage_heading_font.c
pnpm --store-dir .deps/pnpm-store dlx lv_font_conv@1.5.3 --font .deps/fonts/usage-values.ttf --size 48 --bpp 4 --format lvgl --symbols '0123456789%-' --no-compress --lv-font-name usage_values_font -o main/usage_values_font.c
pnpm --store-dir .deps/pnpm-store dlx lv_font_conv@1.5.3 --font .deps/fonts/usage-values.ttf --size 23 --bpp 4 --format lvgl --range 0x20-0x7e --symbols 'äöüÄÖÜß' --no-compress --lv-font-name usage_ui_font -o main/usage_ui_font.c
pnpm --store-dir .deps/pnpm-store dlx lv_font_conv@1.5.3 --font .deps/fonts/usage-values.ttf --size 16 --bpp 4 --format lvgl --range 0x20-0x7e --symbols 'äöüÄÖÜß' --no-compress --lv-font-name usage_status_font -o main/usage_status_font.c
pnpm --store-dir .deps/pnpm-store dlx lv_font_conv@1.5.3 --font .deps/fonts/usage-values.ttf --size 12 --bpp 4 --format lvgl --range 0x20-0x7e --no-compress --lv-font-name usage_meta_font -o main/usage_meta_font.c
python3 - <<'PY'
from pathlib import Path
for name, copyright in [
    ('heading', 'Copyright (c) 2011 TypeTogether; derived from Bree Serif'),
    ('values', 'Copyright (c) 2010-2014 tyPoland Lukasz Dziedzic; derived from Lato Bold'),
]:
    path = Path(f'main/usage_{name}_font.c')
    notice = f'/* {copyright}.\n * SIL Open Font License 1.1; see licenses/usage-{name}-OFL.txt.\n * Converted subset named usage_{name}_font.\n */\n'
    path.write_text(notice + path.read_text())
path = Path('main/usage_ui_font.c')
path.write_text('/* Lato Bold; SIL Open Font License 1.1.\n * Copyright (c) 2010-2014 tyPoland Lukasz Dziedzic.\n * See licenses/usage-values-OFL.txt.\n */\n' + path.read_text())
path = Path('main/usage_status_font.c')
path.write_text('/* Lato Bold; SIL Open Font License 1.1.\n * Copyright (c) 2010-2014 tyPoland Lukasz Dziedzic.\n * See licenses/usage-values-OFL.txt.\n */\n' + path.read_text())
path = Path('main/usage_meta_font.c')
path.write_text('/* Lato Bold; SIL Open Font License 1.1.\n * Copyright (c) 2010-2014 tyPoland Lukasz Dziedzic.\n * See licenses/usage-values-OFL.txt.\n */\n' + path.read_text())
PY
