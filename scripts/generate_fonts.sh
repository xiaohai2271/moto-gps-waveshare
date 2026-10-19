#!/usr/bin/env bash
# Generates the firmware's Chinese-capable fonts.
#
# nav_16 / nav_20 cover the UI strings plus GB2312 level-1 (3755 common hanzi),
# so music titles and road names coming from the phone render offline.
# nav_28 only needs the UI strings (headline text); keeping it small avoids
# spending flash on glyphs that never appear at that size.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_dir}"

font="third_party/lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
symbols_cn="$(cat scripts/font_symbols_cn.txt)"
symbols_ui="$(cat scripts/font_symbols_ui.txt)"

gen() {
  local size="$1"
  local symbols="$2"
  local output="$3"
  local bpp="$4"
  npx --yes lv_font_conv \
    --font "${font}" \
    --size "${size}" \
    --bpp "${bpp}" \
    --format lvgl \
    --no-compress \
    --range 0x20-0x7E \
    --symbols "${symbols}" \
    -o "${output}"
}

# bpp1 keeps the ESP32-S3 render under budget: bpp2 measured 64 ms/frame on
# the 1.75" round panel (12 fps), bpp1 stays near the 25 fps cadence.
gen 16 "${symbols_cn}" shared/nav_ui/assets/moto_font_nav_16.c 1
gen 20 "${symbols_cn}" shared/nav_ui/assets/moto_font_nav_20.c 1
gen 28 "${symbols_ui}" shared/nav_ui/assets/moto_font_nav_28.c 4
